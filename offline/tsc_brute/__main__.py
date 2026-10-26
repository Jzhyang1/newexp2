"""Train and serve a convolutional LSTM with one probability per 4 KiB page."""

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

from offline.data import load_training
from offline.ts_lstm.__main__ import FEATURES_PER_ACCESS, SIN_PERIODS

Pair = tuple[int, int]
DISK_GB = 20
DISK_BYTES = DISK_GB * (1 << 30)
BLOCK_BYTES = 512
BLOCKS_PER_PAGE = 8
PAGE_BYTES = BLOCK_BYTES * BLOCKS_PER_PAGE
NUM_PAGES = DISK_BYTES // PAGE_BYTES
CACHE_PATH = Path(__file__).resolve().parent / "training_cache.pt"


def page_bounds(pair: Pair) -> tuple[int, int]:
    """Inclusive page bounds of a byte-addressed request."""
    offset, length = pair
    if offset < 0 or length < 1 or offset + length > DISK_BYTES:
        raise ValueError(f"request {pair!r} is outside the {DISK_GB} GiB disk")
    return offset // PAGE_BYTES, (offset + length - 1) // PAGE_BYTES


def features(pairs: Sequence[Pair]) -> torch.Tensor:
    rows = []
    for index, (offset, length) in enumerate(pairs):
        age = float(len(pairs) - index)
        rows.append([
            offset / DISK_BYTES, length / PAGE_BYTES, age,
            *(math.sin(math.tau * age / period) for period in SIN_PERIODS),
        ])
    return torch.tensor(rows, dtype=torch.float32)


class SequenceClassifier(nn.Module):
    """Temporal convolutions followed by the ts_lstm recurrent stack."""

    def __init__(self) -> None:
        super().__init__()
        self.convolution = nn.Sequential(
            nn.Conv1d(FEATURES_PER_ACCESS, 32, 3, padding=1),
            nn.ReLU(),
            nn.Conv1d(32, 32, 3, padding=1),
            nn.ReLU(),
        )
        self.lstm = nn.LSTM(32, 32, 4, batch_first=True)
        self.output = nn.Linear(32, NUM_PAGES)

    def forward(self, values: torch.Tensor) -> torch.Tensor:
        encoded = self.convolution(values.transpose(1, 2)).transpose(1, 2)
        _, (hidden, _) = self.lstm(encoded)
        return self.output(hidden[-1])  # logits; sigmoid gives page probabilities


def classification_loss(logits: torch.Tensor, bounds: torch.Tensor) -> torch.Tensor:
    """BCE over all pages; every page overlapping the next request is positive."""
    pages = torch.arange(logits.shape[-1], device=logits.device)
    labels = (pages >= bounds[:, :1]) & (pages <= bounds[:, 1:])
    return nn.functional.binary_cross_entropy_with_logits(logits, labels.to(logits.dtype))


def prefetch_range(probabilities: torch.Tensor, alpha: float, k: int) -> list[Pair]:
    """Widest interval containing argmax with i,j < k and mean P > alpha.

    Break width ties by mean probability, then by the lower starting page.
    Return byte offset/length pairs, including both endpoints of the interval.
    """
    if not math.isfinite(alpha) or not 0 <= alpha <= 1 or k < 1:
        raise ValueError("alpha must be in [0, 1] and k must be positive")
    # TODO
    return [int(probabilities.argmax()) * PAGE_BYTES]


class BruteModel:
    def __init__(
        self, history: int, niceness: int, train: Sequence[Pair],
        test: Sequence[Pair], epochs: int, batch_size: int = 8,
        alpha: float = 0.5, k: int = 8,
    ) -> None:
        if min(history, niceness, epochs, batch_size, k) < 1:
            raise ValueError("history, niceness, epochs, batch size and k must be positive")
        if not math.isfinite(alpha) or not 0 <= alpha <= 1:
            raise ValueError("alpha must be in [0, 1]")
        for pair in (*train, *test):
            page_bounds(pair)
        if len(train) <= history:
            raise ValueError("training split needs at least min-len + 1 requests")
        self.history, self.niceness = history, niceness
        self.batch_size, self.alpha, self.k = batch_size, alpha, k
        torch.manual_seed(0)
        self.network = SequenceClassifier()
        self.train_losses: list[float] = []
        self.test_losses: list[float] = []
        self.train_regression_losses: list[float] = []
        self.test_regression_losses: list[float] = []
        train_data, test_data = self._dataset(train), self._dataset(test)
        optimizer = torch.optim.Adam(self.network.parameters(), lr=0.01)
        for _ in range(epochs):
            self.network.train()
            for values, bounds, _ in DataLoader(train_data, batch_size=batch_size, shuffle=True):
                optimizer.zero_grad(set_to_none=True)
                loss = classification_loss(self.network(values), bounds)
                loss.backward()
                optimizer.step()
            training = self._evaluate(train_data)
            testing = self._evaluate(test_data)
            self.train_losses.append(training[0])
            self.train_regression_losses.append(training[1])
            self.test_losses.append(testing[0])
            self.test_regression_losses.append(testing[1])

    def _dataset(self, pairs: Sequence[Pair]) -> TensorDataset:
        values, bounds, future = [], [], []
        for index in range(self.history, len(pairs)):
            values.append(features(pairs[index - self.history:index]))
            bounds.append(page_bounds(pairs[index]))
            offsets = [pair[0] for pair in pairs[index:index + self.niceness]]
            # Repeating the last offset preserves the minimum for short tails.
            future.append(offsets + [offsets[-1]] * (self.niceness - len(offsets)))
        if not values:
            return TensorDataset(
                torch.empty((0, self.history, FEATURES_PER_ACCESS)),
                torch.empty((0, 2), dtype=torch.long),
                torch.empty((0, self.niceness), dtype=torch.float64),
            )
        return TensorDataset(torch.stack(values), torch.tensor(bounds),
                             torch.tensor(future, dtype=torch.float64))

    @torch.no_grad()
    def _evaluate(self, dataset: TensorDataset) -> tuple[float, float]:
        self.network.eval()
        if not len(dataset):
            return float("nan"), float("nan")
        total_loss = total_error = 0.0
        for values, bounds, future in DataLoader(dataset, batch_size=self.batch_size):
            logits = self.network(values)
            total_loss += classification_loss(logits, bounds).item() * len(values)
            predicted = logits.argmax(-1).double() * PAGE_BYTES
            error = (predicted[:, None] - future).square().min(dim=1).values
            total_error += error.sum().item()
        return total_loss / len(dataset), math.sqrt(total_error / len(dataset))

    @torch.no_grad()
    def probabilities(self, previous: Sequence[Pair]) -> torch.Tensor | None:
        if len(previous) < self.history:
            return None
        history = previous[-self.history:]
        for pair in history:
            page_bounds(pair)
        self.network.eval()
        return self.network(features(history).unsqueeze(0))[0].sigmoid()

    def predict(self, previous: Sequence[Pair], count: int) -> list[Pair]:
        """At most one range for the next request, using byte units."""
        if count < 0:
            raise ValueError("next_n must be non-negative")
        if count == 0:
            return []
        probabilities = self.probabilities(previous)
        if probabilities is None:
            return []
        return prefetch_range(probabilities, self.alpha, self.k)


def serve(model: BruteModel) -> None:
    """ModelPolicy's JSON protocol uses 512 B blocks; CSV and model use bytes."""
    for line in sys.stdin:
        request = json.loads(line)
        kind = request.get("type")
        if kind == "get":
            previous = [(int(o) * BLOCK_BYTES, int(n) * BLOCK_BYTES)
                        for o, n in request["prev_accesses"]]
            predictions = model.predict(previous, int(request["next_n"]))
            result = [(o // BLOCK_BYTES, n // BLOCK_BYTES) for o, n in predictions]
            print(json.dumps(result), flush=True)
        elif kind == "put":
            # Offline baseline: held-out losses are reported after training.
            print("[]", flush=True)
        elif kind == "kill":
            break
        else:
            raise ValueError(f"unknown request type: {kind!r}")


def show_losses(model: BruteModel, output_path: str | None) -> None:
    import matplotlib
    if output_path is not None:
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    figure, axes = plt.subplots(1, 2, figsize=(10, 4))
    for axis, train, test, label in (
        (axes[0], model.train_losses, model.test_losses, "Classification BCE"),
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
                       args.epochs, args.batch_size, args.alpha, args.k)
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
    parser.add_argument("--epochs", type=int, default=40)
    parser.add_argument("--batch-size", type=int, default=8)
    parser.add_argument("--alpha", type=float, default=0.3)
    parser.add_argument("--k", type=int, default=8, help="exclusive limit on pages on each side")
    parser.add_argument("--type", choices=("predict_mean", "predict_min", "predict_max"),
                        default="predict_min", help="accepted for ModelPolicy compatibility; unused")
    parser.add_argument("--predict-deltas", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    curve = parser.add_mutually_exclusive_group()
    curve.add_argument("--hide-loss-curve", action="store_true")
    curve.add_argument("--loss-curve", metavar="PATH")
    args = parser.parse_args()
    for name in ("min_len", "niceness", "epochs", "batch_size", "k"):
        if getattr(args, name) < 1:
            parser.error(f"--{name.replace('_', '-')} must be at least 1")
    if not math.isfinite(args.alpha) or not 0 <= args.alpha <= 1:
        parser.error("--alpha must be in [0, 1]")
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
        print(f"classification {label} loss: {classification:.0f}", file=sys.stderr, flush=True)
        print(f"regression {label} loss (RMSE bytes): {regression:.0f}", file=sys.stderr, flush=True)
    if not args.hide_loss_curve:
        show_losses(model, args.loss_curve)
    if not args.dry_run:
        serve(model)


if __name__ == "__main__":
    main()
