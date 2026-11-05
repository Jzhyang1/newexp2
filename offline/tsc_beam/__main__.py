"""Train and serve a hierarchical classifier that beam-searches a 1024 TiB disk.

The address space (2**50 bytes) is split into 1024 buckets at every level:
1 TiB, then 1 GiB, 1 MiB, 1 KiB (and 1 B with --levels 5). A history encoder
runs once per request; a GRU then walks down the levels, emitting 1024 class
probabilities per level conditioned on the buckets chosen above. Training is
teacher-forced (one pass covers every level). Prediction is a beam search
batched over samples and beams, so each level costs one GRU step and one
matmul regardless of how many requests or beams are scored.
"""

from __future__ import annotations

import argparse
import math
import sys
from collections.abc import Sequence

import torch
from torch import nn

from offline.data import load_training
from offline.serve import serve

Pair = tuple[int, int]

BUCKET_BITS = 10
BUCKETS = 1 << BUCKET_BITS
DISK_BITS = 50  # 1024 TiB
DISK_BYTES = 1 << DISK_BITS
MAX_LEVELS = DISK_BITS // BUCKET_BITS


def level_shifts(levels: int) -> torch.Tensor:
    return torch.tensor([DISK_BITS - BUCKET_BITS * (level + 1) for level in range(levels)])


def encode(offsets: torch.Tensor, levels: int) -> torch.Tensor:
    """Byte offsets (...) -> bucket indices (..., levels), coarsest first."""
    return (offsets.unsqueeze(-1) >> level_shifts(levels)) & (BUCKETS - 1)


def decode(digits: torch.Tensor) -> torch.Tensor:
    """Bucket indices (..., levels) -> byte offset of the leaf bucket (...)."""
    return (digits << level_shifts(digits.shape[-1])).sum(-1)


class BeamClassifier(nn.Module):
    def __init__(self, levels: int, embed: int, hidden: int) -> None:
        super().__init__()
        self.levels = levels
        self.embeddings = nn.ModuleList(nn.Embedding(BUCKETS, embed) for _ in range(levels))
        self.convolution = nn.Sequential(
            nn.Conv1d(levels * embed, hidden, 3, padding=1), nn.ReLU(),
            nn.Conv1d(hidden, hidden, 3, padding=1), nn.ReLU(),
        )
        self.lstm = nn.LSTM(hidden, hidden, batch_first=True)
        self.prefix = nn.ModuleList(nn.Embedding(BUCKETS, hidden) for _ in range(levels - 1))
        self.cell = nn.GRUCell(hidden, hidden)
        self.heads = nn.ModuleList(nn.Linear(hidden, BUCKETS) for _ in range(levels))

    def encode_history(self, history: torch.Tensor) -> torch.Tensor:
        """(N, T, levels) bucket indices -> (N, hidden), computed once per request."""
        embedded = torch.cat([table(history[..., level])
                              for level, table in enumerate(self.embeddings)], -1)
        _, (hidden, _) = self.lstm(self.convolution(embedded.transpose(1, 2)).transpose(1, 2))
        return hidden[-1]

    def advance(self, level: int, state: torch.Tensor, digit: torch.Tensor) -> torch.Tensor:
        """Condition the decoder state on the bucket chosen at `level - 1`."""
        return self.cell(self.prefix[level - 1](digit), state)

    def forward(self, history: torch.Tensor, target: torch.Tensor) -> list[torch.Tensor]:
        """Teacher-forced logits for every level: levels x (N, 1024)."""
        state = self.encode_history(history)
        logits = [self.heads[0](state)]
        for level in range(1, self.levels):
            state = self.advance(level, state, target[:, level - 1])
            logits.append(self.heads[level](state))
        return logits


@torch.no_grad()
def beam_search(network: BeamClassifier, history: torch.Tensor, beam: int
                ) -> tuple[torch.Tensor, torch.Tensor]:
    """Top `beam` leaf paths per request: ((N, B, levels) digits, (N, B) log-probs)."""
    state = network.encode_history(history)[:, None]  # (N, 1, H)
    count, _, width = state.shape
    scores = state.new_zeros(count, 1)
    paths = torch.empty(count, 1, 0, dtype=torch.long)
    for level in range(network.levels):
        if level:
            state = network.advance(level, state.reshape(-1, width),
                                    paths[..., -1].reshape(-1)).view(count, -1, width)
        total = (scores[..., None] + network.heads[level](state).log_softmax(-1)).flatten(1)
        scores, index = total.topk(min(beam, total.shape[1]))
        parent, digit = index // BUCKETS, index % BUCKETS
        state = state.gather(1, parent[..., None].expand(-1, -1, width))
        paths = torch.cat([paths.gather(1, parent[..., None].expand(-1, -1, level)),
                           digit[..., None]], -1)
    return paths, scores


def history_digits(offsets: Sequence[int], levels: int) -> torch.Tensor:
    # Offsets beyond the disk saturate rather than crash while serving.
    clipped = torch.tensor([min(max(o, 0), DISK_BYTES - 1) for o in offsets], dtype=torch.long)
    return encode(clipped, levels)


class BeamModel:
    def __init__(self, history: int, niceness: int, levels: int, beam: int,
                 train: Sequence[Pair], test: Sequence[Pair], epochs: int,
                 batch_size: int, embed: int, hidden: int) -> None:
        if len(train) <= history:
            raise ValueError("training split needs at least min-len + 1 requests")
        self.history, self.niceness, self.levels, self.beam = history, niceness, levels, beam
        self.leaf_bytes = 1 << (DISK_BITS - BUCKET_BITS * levels)
        self.train_losses: list[float] = []
        self.test_losses: list[float] = []
        torch.manual_seed(1)
        self.network = BeamClassifier(levels, embed, hidden)
        train_set, test_set = self._dataset(train), self._dataset(test)
        optimizer = torch.optim.Adam(self.network.parameters(), lr=0.001)
        count = len(train_set[0])
        print(f"Training on {count} examples; testing on {len(test_set[0])}; "
              f"{levels} levels x {BUCKETS} buckets, beam {beam}", file=sys.stderr, flush=True)
        for epoch in range(1, epochs + 1):
            self.network.train()
            order = torch.randperm(count)
            for start in range(0, count, batch_size):
                pick = order[start:start + batch_size]
                optimizer.zero_grad(set_to_none=True)
                loss = self._loss(train_set[0][pick], train_set[1][pick])
                loss.backward()
                optimizer.step()
            self.train_losses.append(self._mean_loss(train_set))
            self.test_losses.append(self._mean_loss(test_set))
            print(f"Epoch {epoch}/{epochs}: train CE={self.train_losses[-1]:.4f}, "
                  f"test CE={self.test_losses[-1]:.4f}", file=sys.stderr, flush=True)
        self.train_metrics = self.evaluate(train_set)
        self.test_metrics = self.evaluate(test_set)

    def _dataset(self, pairs: Sequence[Pair]) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
        """(history digits (N,T,L), target digits (N,L), next `niceness` offsets (N,K))."""
        offsets = torch.tensor([offset for offset, _ in pairs], dtype=torch.long)
        if len(offsets) and (offsets.min() < 0 or offsets.max() >= DISK_BYTES):
            raise ValueError("requests must lie within the 1024 TiB disk")
        starts = torch.arange(self.history, len(offsets))
        before = starts[:, None] + torch.arange(-self.history, 0)
        # Clamping repeats the last offset for short tails, keeping the minimum valid.
        after = (starts[:, None] + torch.arange(self.niceness)).clamp(max=max(len(offsets) - 1, 0))
        digits = encode(offsets, self.levels) if len(offsets) else torch.empty(0, self.levels, dtype=torch.long)
        return digits[before], digits[starts], offsets[after] if len(offsets) else offsets[:0]

    def _loss(self, history: torch.Tensor, target: torch.Tensor) -> torch.Tensor:
        """Sum over levels of the mean cross entropy (= -log P(leaf path))."""
        logits = self.network(history, target)
        return sum(nn.functional.cross_entropy(logit, target[:, level])
                   for level, logit in enumerate(logits))

    @torch.no_grad()
    def _mean_loss(self, data, chunk: int = 4096) -> float:
        self.network.eval()
        total = 0.0
        for start in range(0, len(data[0]), chunk):
            history, target = data[0][start:start + chunk], data[1][start:start + chunk]
            total += self._loss(history, target).item() * len(history)
        return total / len(data[0]) if len(data[0]) else float("nan")

    @torch.no_grad()
    def evaluate(self, data, chunk: int = 1024) -> dict[str, float]:
        """Batched beam search over a split: recall of the true leaf and top-1 RMSE."""
        self.network.eval()
        history, target, future = data
        if not len(history):
            return {"recall": float("nan"), "rmse": float("nan")}
        hits = error = 0.0
        for start in range(0, len(history), chunk):
            paths, _ = beam_search(self.network, history[start:start + chunk], self.beam)
            sl = slice(start, start + chunk)
            hits += (paths == target[sl, None]).all(-1).any(-1).sum().item()
            top = decode(paths[:, 0]).double()
            error += (top[:, None] - future[sl].double()).square().min(1).values.sum().item()
        return {"recall": hits / len(history), "rmse": math.sqrt(error / len(history))}

    def predict(self, previous: Sequence[Pair], count: int) -> list[Pair]:
        """Up to `count` most probable leaf buckets, in byte units."""
        if count < 0:
            raise ValueError("next_n must be non-negative")
        if count == 0 or len(previous) < self.history:
            return []
        recent = [offset for offset, _ in previous[-self.history:]]
        paths, _ = beam_search(self.network, history_digits(recent, self.levels)[None],
                               max(self.beam, count))
        return [(int(offset), self.leaf_bytes) for offset in decode(paths[0, :count])]


def show_losses(model: BeamModel, output_path: str | None) -> None:
    import matplotlib
    if output_path is not None:
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    figure, axis = plt.subplots()
    epochs = range(1, len(model.train_losses) + 1)
    axis.plot(epochs, model.train_losses, label="train")
    axis.plot(epochs, model.test_losses, label="test")
    axis.set(xlabel="Epoch", ylabel="-log P(leaf path)")
    axis.legend()
    axis.grid(True, alpha=0.3)
    figure.tight_layout()
    if output_path is None:
        plt.show()
    else:
        figure.savefig(output_path)
    plt.close(figure)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--training", required=True)
    parser.add_argument("--min-len", type=int, default=1)
    parser.add_argument("--niceness", type=int, default=1,
                        help="future request window for regression scoring only")
    parser.add_argument("--levels", type=int, default=4,
                        help="1024-way levels: 4 ends at 1 KiB buckets, 5 at bytes")
    parser.add_argument("--beam", type=int, default=8)
    parser.add_argument("--epochs", type=int, default=30)
    parser.add_argument("--batch-size", type=int, default=64)
    parser.add_argument("--embed", type=int, default=16)
    parser.add_argument("--hidden", type=int, default=64)
    parser.add_argument("--type", choices=("predict_mean", "predict_min", "predict_max"),
                        default="predict_min", help="accepted for ModelPolicy compatibility; unused")
    parser.add_argument("--predict-deltas", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    curve = parser.add_mutually_exclusive_group()
    curve.add_argument("--hide-loss-curve", action="store_true")
    curve.add_argument("--loss-curve", metavar="PATH")
    args = parser.parse_args()
    for name in ("min_len", "niceness", "beam", "epochs", "batch_size", "embed", "hidden"):
        if getattr(args, name) < 1:
            parser.error(f"--{name.replace('_', '-')} must be at least 1")
    if not 1 <= args.levels <= MAX_LEVELS:
        parser.error(f"--levels must be between 1 and {MAX_LEVELS}")
    if args.predict_deltas:
        parser.error("--predict-deltas is not supported: classes are absolute buckets")
    data = load_training(args.training)
    split = min(len(data), max(args.min_len + 1, round(len(data) * 0.8)))
    try:
        model = BeamModel(args.min_len, args.niceness, args.levels, args.beam,
                          data[:split], data[split:], args.epochs, args.batch_size,
                          args.embed, args.hidden)
    except ValueError as error:
        parser.error(str(error))
    for label, loss, metrics in (("training", model.train_losses[-1], model.train_metrics),
                                 ("testing", model.test_losses[-1], model.test_metrics)):
        print(f"{label}: CE={loss:.4f}, beam-{args.beam} recall={metrics['recall']:.4f}, "
              f"top-1 RMSE={metrics['rmse']:.0f} bytes", file=sys.stderr, flush=True)
    if not args.hide_loss_curve:
        show_losses(model, args.loss_curve)
    if not args.dry_run:
        serve(model.predict)


if __name__ == "__main__":
    main()
