"""Train and serve a convolutional LSTM with one probability per 2 MiB page."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import pickle
import sys
import tempfile
from collections.abc import Sequence

import torch
from torch import nn
from torch.utils.data import DataLoader, TensorDataset

from offline.serve import serve
from offline.data import load_training
# We ignore the span for now -- only predict offsets
EMBED_DIM = 32

Pair = tuple[int, int]
DISK_GB = 20
DISK_BYTES = DISK_GB * (1 << 30)
BLOCK_BYTES = 512
BLOCKS_PER_PAGE = 4096
PAGE_BYTES = BLOCK_BYTES * BLOCKS_PER_PAGE
NUM_PAGES = DISK_BYTES // PAGE_BYTES
CACHE_PATH = Path(__file__).resolve().parent / "training_cache_coarse_emb.pt"


def features(pairs: Sequence[Pair]) -> torch.Tensor:
    return torch.tensor([offset // PAGE_BYTES for (offset, length) in pairs], dtype=torch.long)


class SequenceClassifier(nn.Module):
    """Temporal convolutions followed by the ts_lstm recurrent stack."""

    def __init__(self) -> None:
        super().__init__()
        self.embedding = nn.Embedding(NUM_PAGES, EMBED_DIM)
        self.convolution = nn.Sequential(
            nn.Conv1d(EMBED_DIM, 64, 3, padding=1),
            nn.ReLU(),
            nn.Conv1d(64, 64, 3, padding=1),
            nn.ReLU(),
        )
        self.lstm = nn.LSTM(64, 64, 1, batch_first=True)
        self.output = nn.Linear(64, NUM_PAGES)

    def forward(self, values: torch.Tensor) -> torch.Tensor:
        encoded = self.convolution(self.embedding(values).transpose(1, 2)).transpose(1, 2)
        _, (hidden, _) = self.lstm(encoded)
        return self.output(hidden[-1])  # logits; softmax gives page probabilities


def classification_loss(
    logits: torch.Tensor, target_page: torch.Tensor, radius: int = 0,
) -> torch.Tensor:
    """Cross-entropy against a triangular soft target centered on the next page.

    radius=0 is the plain one-hot target; larger radii give partial credit to
    pages within `radius` of the target.
    """
    pages = torch.arange(logits.shape[-1], device=logits.device)
    dist = (pages - target_page[:, None]).abs().float()
    soft = (1 - dist / (radius + 1)).clamp(min=0)
    soft = soft / soft.sum(-1, keepdim=True)
    return -(soft * logits.log_softmax(-1)).sum(-1).mean()


def prefetch_range(probabilities: torch.Tensor) -> list[Pair]:
    return [(int(probabilities.argmax()) * PAGE_BYTES, PAGE_BYTES)]


class BruteModel:
    def __init__(
        self, history: int, niceness: int, train: Sequence[Pair],
        test: Sequence[Pair], epochs: int, batch_size: int = 8,
    ) -> None:
        if min(history, niceness, epochs, batch_size) < 1:
            raise ValueError("history, niceness, epochs, batch size must be positive")
        if len(train) <= history:
            raise ValueError("training split needs at least min-len + 1 requests")
        self.history, self.niceness = history, niceness
        self.batch_size = batch_size
        torch.manual_seed(1)
        self.network = SequenceClassifier()
        self.train_losses: list[float] = []
        self.test_losses: list[float] = []
        self.train_regression_losses: list[float] = []
        self.test_regression_losses: list[float] = []
        train_data, test_data = self._dataset(train), self._dataset(test)
        optimizer = torch.optim.Adam(self.network.parameters(), lr=0.001)
        num_batches = math.ceil(len(train_data) / batch_size)
        print(f"Training on {len(train_data)} examples; testing on {len(test_data)} examples; "
              f"{epochs} epochs, {num_batches} batches per epoch",
              file=sys.stderr, flush=True)
        for epoch in range(1, epochs + 1):
            self.network.train()
            for batch, (values, targets, _) in enumerate(
                DataLoader(train_data, batch_size=batch_size, shuffle=True), start=1,
            ):
                optimizer.zero_grad(set_to_none=True)
                loss = classification_loss(self.network(values), targets)
                loss.backward()
                optimizer.step()
                if batch == 1 or batch % 100 == 0 or batch == num_batches:
                    print(f"Epoch {epoch}/{epochs}, batch {batch}/{num_batches}: "
                          f"CE={loss.item():.6f}", file=sys.stderr, flush=True)
            print(f"Epoch {epoch}/{epochs}: evaluating train and test losses",
                  file=sys.stderr, flush=True)
            training = self._evaluate(train_data)
            testing = self._evaluate(test_data)
            self.train_losses.append(training[0])
            self.train_regression_losses.append(training[1])
            self.test_losses.append(testing[0])
            self.test_regression_losses.append(testing[1])
            print(f"Epoch {epoch}/{epochs}: train CE={training[0]:.6f}, "
                  f"test CE={testing[0]:.6f}, train RMSE={training[1]:.2f} bytes, "
                  f"test RMSE={testing[1]:.2f} bytes", file=sys.stderr, flush=True)

    def _dataset(self, pairs: Sequence[Pair]) -> TensorDataset:
        values, targets, future = [], [], []
        for index in range(self.history, len(pairs)):
            values.append(features(pairs[index - self.history:index]))
            targets.append(pairs[index][0] // PAGE_BYTES)
            offsets = [pair[0] for pair in pairs[index:index + self.niceness]]
            # Repeating the last offset preserves the minimum for short tails.
            future.append(offsets + [offsets[-1]] * (self.niceness - len(offsets)))
        if not values:
            return TensorDataset(
                torch.empty((0, self.history), dtype=torch.long),
                torch.empty((0,), dtype=torch.long),
                torch.empty((0, self.niceness), dtype=torch.float64),
            )
        return TensorDataset(torch.stack(values), torch.tensor(targets),
                             torch.tensor(future, dtype=torch.float64))

    @torch.no_grad()
    def _evaluate(self, dataset: TensorDataset) -> tuple[float, float]:
        self.network.eval()
        if not len(dataset):
            return float("nan"), float("nan")
        total_loss = total_error = 0.0
        for values, targets, future in DataLoader(dataset, batch_size=self.batch_size):
            logits = self.network(values)
            total_loss += classification_loss(logits, targets).item() * len(values)
            predicted = logits.argmax(-1).double() * PAGE_BYTES
            error = (predicted[:, None] - future).square().min(dim=1).values
            total_error += error.sum().item()
        return total_loss / len(dataset), math.sqrt(total_error / len(dataset))

    @torch.no_grad()
    def probabilities(self, previous: Sequence[Pair]) -> torch.Tensor | None:
        if len(previous) < self.history:
            return None
        history = previous[-self.history:]
        self.network.eval()
        return self.network(features(history).unsqueeze(0))[0].softmax(-1)

    def predict(self, previous: Sequence[Pair], count: int) -> list[Pair]:
        """At most one range for the next request, using byte units."""
        if count < 0:
            raise ValueError("next_n must be non-negative")
        if count == 0:
            return []
        probabilities = self.probabilities(previous)
        if probabilities is None:
            return []
        return prefetch_range(probabilities)


def show_losses(model: BruteModel, output_path: str | None) -> None:
    import matplotlib
    if output_path is not None:
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    figure, axes = plt.subplots(1, 2, figsize=(10, 4))
    for axis, train, test, label in (
        (axes[0], model.train_losses, model.test_losses, "Classification CE"),
        (axes[1], model.train_regression_losses, model.test_regression_losses,
         "Regression RMSE (bytes)"),
    ):
        epochs = range(1, len(train) + 1)
        axis.plot(epochs, train, label="train")
        axis.plot(epochs, test, label="test")
        axis.set(xlabel="Epoch", ylabel=label)
        axis.legend()
        axis.grid(True, alpha=0.3)
    figure.tight_layout()
    if output_path is None:
        plt.show()
    else:
        figure.savefig(output_path)
    plt.close(figure)


def load_or_train(args: argparse.Namespace) -> BruteModel:
    """Reuse the last model when the parsed command-line arguments match."""
    cache_args = vars(args).copy()
    # Relative training paths can refer to different files across working directories.
    cache_args["training"] = str(Path(args.training).resolve())
    if CACHE_PATH.exists():
        try:
            checkpoint = torch.load(CACHE_PATH, map_location="cpu", weights_only=True)
            if checkpoint["args"] == cache_args:
                model = BruteModel.__new__(BruteModel)
                model.__dict__.update(checkpoint["model"])
                model.network = SequenceClassifier()
                model.network.load_state_dict(checkpoint["state_dict"])
                model.network.eval()
                print(f"Loaded cached model from {CACHE_PATH}", file=sys.stderr, flush=True)
                return model
        except (OSError, EOFError, RuntimeError, ValueError, KeyError, pickle.UnpicklingError) as error:
            print(f"Could not load cached model; retraining: {error}", file=sys.stderr, flush=True)

    data = load_training(args.training)
    split = min(len(data), max(args.min_len + 1, round(len(data) * 0.8)))
    model = BruteModel(args.min_len, args.niceness, data[:split], data[split:],
                       args.epochs, args.batch_size)
    checkpoint = {
        "args": cache_args,
        "model": {key: value for key, value in vars(model).items() if key != "network"},
        "state_dict": model.network.state_dict(),
    }
    # Replace atomically so an interrupted save cannot damage the previous cache.
    with tempfile.NamedTemporaryFile(dir=CACHE_PATH.parent, suffix=".pt", delete=False) as temporary:
        temporary_path = Path(temporary.name)
    try:
        torch.save(checkpoint, temporary_path)
        temporary_path.replace(CACHE_PATH)
    finally:
        temporary_path.unlink(missing_ok=True)
    return model


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--training", required=True)
    parser.add_argument("--min-len", type=int, default=1)
    parser.add_argument("--niceness", type=int, default=1,
                        help="future request window for regression scoring only")
    parser.add_argument("--epochs", type=int, default=30)
    parser.add_argument("--batch-size", type=int, default=8)
    parser.add_argument("--type", choices=("predict_mean", "predict_min", "predict_max"),
                        default="predict_min", help="accepted for ModelPolicy compatibility; unused")
    parser.add_argument("--predict-deltas", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    curve = parser.add_mutually_exclusive_group()
    curve.add_argument("--hide-loss-curve", action="store_true")
    curve.add_argument("--loss-curve", metavar="PATH")
    args = parser.parse_args()
    for name in ("min_len", "niceness", "epochs", "batch_size"):
        if getattr(args, name) < 1:
            parser.error(f"--{name.replace('_', '-')} must be at least 1")
    if args.predict_deltas:
        parser.error("--predict-deltas is not supported: classes are absolute pages")
    try:
        model = load_or_train(args)
    except ValueError as error:
        parser.error(str(error))
    for label, classification, regression in (
        ("training", model.train_losses[-1], model.train_regression_losses[-1]),
        ("testing", model.test_losses[-1], model.test_regression_losses[-1]),
    ):
        print(f"classification {label} loss: {classification:.6f}", file=sys.stderr, flush=True)
        print(f"regression {label} loss (RMSE bytes): {regression:.2f}", file=sys.stderr, flush=True)
    if not args.hide_loss_curve:
        show_losses(model, args.loss_curve)
    if not args.dry_run:
        serve(model.predict)


if __name__ == "__main__":
    main()
