"""Train and serve an encoded Classification-based time-series predictor."""

from __future__ import annotations

import argparse
import math
import json
import sys
from typing import Any, Sequence
import torch
from torch import nn
from offline.serve import serve
from offline.data import load_training
from offline.loss import prediction_loss
from offline.loss_curve import show_loss_curve

type Pair = tuple[int, int]
type SzSequence = list[int]
type SzTensor = torch.Tensor
type FeatureSequence = list[list[float]]

FINE_BYTES  = 64 * (1 << 10)    # 64 KiB minimum granularity
COARSE_BYTES = 8 * (1 << 20)    # 8 MiB

MAX_COARSE_CATEGORIES = 1024    # our data spans 1024 * 8 MiB = 8 GiB
FINE_CATEGORIES = COARSE_BYTES // FINE_BYTES


class SequenceClassifier(nn.Module):
    """Voyager-style hierarchical classifier.

    Each access is split into a coarse "page" (8 MiB region) and a fine
    "offset" (64 KiB slot inside it). As in Voyager, the offset embedding is
    page-aware: the offset table holds several expert vectors per offset and
    the page embedding attends over them, so the same fine offset can mean
    different things in different regions. An LSTM consumes the sequence and
    two independent heads classify the next page and the next offset.
    """

    def __init__(self,
                 embed_size: int,
                 lstm_hidden_size: int = 32,
                 lstm_hidden_layers: int = 4,
                 experts: int = 4,
                 ):
        super().__init__()
        self.embed_size = embed_size
        self.experts = experts
        self.coarse_embedding = nn.Embedding(MAX_COARSE_CATEGORIES, embed_size)
        self.fine_embedding = nn.Embedding(FINE_CATEGORIES, embed_size * experts)
        self.lstm = nn.LSTM(2 * embed_size, lstm_hidden_size, lstm_hidden_layers, batch_first=True)
        self.coarse_decode = nn.Linear(lstm_hidden_size, MAX_COARSE_CATEGORIES)
        self.fine_decode = nn.Linear(lstm_hidden_size, FINE_CATEGORIES)

    def forward(self, coarse: SzTensor, fine: SzTensor):
        coarse_embedded = self.coarse_embedding(coarse)                  # (B, T, E)
        fine_experts = self.fine_embedding(fine).view(*fine.shape, self.experts, self.embed_size)
        # page-aware offset embedding: page embedding is the attention query
        scores = (fine_experts * coarse_embedded.unsqueeze(-2)).sum(-1) / math.sqrt(self.embed_size)
        weights = torch.softmax(scores, dim=-1)                          # (B, T, experts)
        fine_embedded = (weights.unsqueeze(-1) * fine_experts).sum(-2)   # (B, T, E)
        _, (hidden, _) = self.lstm(torch.cat([coarse_embedded, fine_embedded], -1))
        return self.coarse_decode(hidden[-1]), self.fine_decode(hidden[-1])


def _label_loss(logits: SzTensor, labels: SzTensor) -> SzTensor:
    """Multi-label cross entropy: target is uniform over the next few accesses
    (labels has shape (B, niceness)), matching the 'niceness' scoring rule."""
    return -torch.log_softmax(logits, dim=-1).gather(1, labels).mean()


class VoyagerModel:
    def __init__(
        self,
        history: int,
        horizon: int,
        mode: str,
        train: Sequence[Pair],
        test: Sequence[Pair],
        epochs: int,
        embed_size: int = 16,
        batch_size: int = 256,
        topk: bool = False,
    ) -> None:
        self.history = history
        self.horizon = horizon
        self.mode = mode
        self.train_offsets = [offset for offset, _ in train]
        self.test_offsets = [offset for offset, _ in test]
        self.fallback_length = self._summary([length for _, length in train])
        self.fallback_offset = self._summary(self.train_offsets)
        self.batch_size = batch_size
        self.topk = topk
        self.train_losses: list[float] = []
        self.test_losses: list[float] = []
        self.network: SequenceClassifier | None = None
        torch.manual_seed(0)
        train_data = self._dataset(self.train_offsets)
        test_data = self._dataset(self.test_offsets)
        if train_data is not None:
            self.network = SequenceClassifier(embed_size)
            self._fit(train_data, test_data, epochs=epochs)

    def _summary(self, values: Sequence[int]) -> int:
        if not values:
            return 1
        if self.mode == "predict_min":
            return min(values)
        if self.mode == "predict_max":
            return max(values)
        return max(1, round(sum(values) / len(values)))

    def _windows(self, history: SzSequence) -> tuple[list[SzSequence], list[int]]:
        """Bundles slices bunches of sequences and their next item"""
        hist = [history[i-self.history:i] for i in range(self.history, len(history))]
        pred = [history[i] for i in range(self.history, len(history))]
        return hist, pred

    def _coarse_fine_split(self, history: SzSequence) -> tuple[SzSequence, SzSequence]:
        """Builds a list of coarse and a list of fine offsets;
        assumes that the history is given in bytes;
        the coarse offsets will be divided by the coarse bytes,
        and fine offsets will be divided by the fine bytes
        """
        coarse, fine = [], []
        for sz in history:
            # offsets beyond the modeled range saturate rather than crash
            coarse.append(min(max(sz, 0) // COARSE_BYTES, MAX_COARSE_CATEGORIES - 1))
            fine.append((max(sz, 0) % COARSE_BYTES) // FINE_BYTES)
        return coarse, fine

    def _coarse_fine_join(self, coarse: SzSequence, fine: SzSequence) -> SzSequence:
        return [(c * COARSE_BYTES + f * FINE_BYTES) for c, f in zip(coarse, fine)]

    def _dataset(self, offsets: SzSequence):
        """Returns (coarse_in, fine_in, coarse_labels, fine_labels) or None.
        Inputs: (N, history); labels: (N, horizon) -- the next `horizon` accesses."""
        n = len(offsets) - self.horizon + 1
        if n <= self.history:
            return None
        coarse, fine = self._coarse_fine_split(offsets)
        c, f = torch.tensor(coarse), torch.tensor(fine)
        starts = torch.arange(self.history, n)
        hist_idx = starts.unsqueeze(1) + torch.arange(-self.history, 0)
        next_idx = starts.unsqueeze(1) + torch.arange(self.horizon)
        return c[hist_idx], f[hist_idx], c[next_idx], f[next_idx]

    def _loss(self, data) -> SzTensor:
        assert self.network is not None
        ci, fi, cl, fl = data
        coarse_out, fine_out = self.network(ci, fi)
        return _label_loss(coarse_out, cl) + _label_loss(fine_out, fl)

    def _fit(self, train_data, test_data, *, epochs: int) -> None:
        assert self.network is not None
        optimizer = torch.optim.Adam(self.network.parameters(), lr=0.01)
        count = len(train_data[0])
        for _ in range(epochs):
            self.network.train()
            perm = torch.randperm(count)
            total = 0.0
            for start in range(0, count, self.batch_size):
                idx = perm[start:start + self.batch_size]
                optimizer.zero_grad()
                loss = self._loss(tuple(t[idx] for t in train_data))
                loss.backward()
                optimizer.step()
                total += loss.item() * len(idx)
            self.train_losses.append(total / count)
            self.network.eval()
            if test_data is not None:
                with torch.no_grad():
                    self.test_losses.append(self._loss(test_data).item())
            else:
                self.test_losses.append(float("nan"))
        self.network.eval()

    def _logits(self, offsets: SzSequence) -> tuple[SzTensor, SzTensor]:
        assert self.network is not None
        coarse, fine = self._coarse_fine_split(offsets[-self.history:])
        with torch.no_grad():
            coarse_out, fine_out = self.network(torch.tensor([coarse]), torch.tensor([fine]))
        return torch.log_softmax(coarse_out[0], -1), torch.log_softmax(fine_out[0], -1)

    def _predict_next(self, offsets: SzSequence) -> int:
        """Greedy top-1 page and top-1 offset for the next access."""
        coarse_lp, fine_lp = self._logits(offsets)
        return self._coarse_fine_join([int(coarse_lp.argmax())], [int(fine_lp.argmax())])[0]

    def _predict_topk(self, offsets: SzSequence, count: int) -> SzSequence:
        """The `count` most likely accesses from a single forward pass, ranked
        by joint log-probability log P(page) + log P(offset). The best `count`
        pairs are always among the top-`count` pages x top-`count` offsets."""
        coarse_lp, fine_lp = self._logits(offsets)
        top_c = coarse_lp.topk(min(count, len(coarse_lp)))
        top_f = fine_lp.topk(min(count, len(fine_lp)))
        joint = (top_c.values.unsqueeze(1) + top_f.values.unsqueeze(0)).flatten()
        best = joint.topk(min(count, len(joint))).indices
        coarse = [int(top_c.indices[i // len(top_f.indices)]) for i in best]
        fine = [int(top_f.indices[i % len(top_f.indices)]) for i in best]
        return self._coarse_fine_join(coarse, fine)

    def predict(self, previous: Sequence[Pair], count: int) -> list[Pair]:
        if count < 0:
            raise ValueError("next_n must be non-negative")
        offsets = [offset for offset, _ in previous]
        if len(offsets) < self.history or self.network is None:
            return [(self.fallback_offset, self.fallback_length)] * count
        if self.topk:
            return [(o, self.fallback_length) for o in self._predict_topk(offsets, count)]
        predictions: list[Pair] = []
        for _ in range(count):
            nxt = self._predict_next(offsets)
            predictions.append((nxt, self.fallback_length))
            offsets.append(nxt)
        return predictions


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--type", choices=("predict_mean", "predict_min", "predict_max"), default="predict_min")
    parser.add_argument("--min-len", type=int, default=1)
    parser.add_argument("--niceness", type=int, default=1)
    parser.add_argument("--predict-deltas", action="store_true")
    parser.add_argument("--predict-topk", action="store_true",
                        help="rank next accesses by joint page/offset probability in one pass instead of greedy autoregression")
    parser.add_argument("--epochs", type=int, default=800)
    parser.add_argument("--dry-run", action="store_true")
    loss_curve = parser.add_mutually_exclusive_group()
    loss_curve.add_argument("--hide-loss-curve", action="store_true")
    loss_curve.add_argument("--loss-curve", metavar="PATH")
    parser.add_argument("--training", required=True)
    args = parser.parse_args()
    if args.min_len < 1:
        parser.error("--min-len must be at least 1")
    if args.niceness < 1:
        parser.error("--niceness must be at least 1")
    if args.epochs < 1:
        parser.error("--epochs must be at least 1")
    if args.predict_deltas:
        parser.error("--predict-deltas is not supported: classes are absolute offsets")
    data = load_training(args.training, False)
    split = min(len(data), max(args.min_len + 1, round(len(data) * 0.8)))
    train, test = data[:split], data[split:]

    model = VoyagerModel(
        args.min_len,
        args.niceness,
        args.type,
        train,
        test,
        args.epochs,
        topk=True,
    )
    if not args.hide_loss_curve:
        show_loss_curve(model.train_losses, model.test_losses, args.loss_curve)

    training_loss_offset, training_loss_length = prediction_loss(
        model.predict, train, args.min_len, args.niceness
    )
    print(f"offset training loss: {training_loss_offset:.0f}", file=sys.stderr, flush=True)
    print(f"length training loss: {training_loss_length:.0f}", file=sys.stderr, flush=True)
    test_loss_offset, test_loss_length = prediction_loss(
        model.predict, test, args.min_len, args.niceness
    )
    print(f"offset testing loss:  {test_loss_offset:.0f}", file=sys.stderr, flush=True)
    print(f"length testing loss:  {test_loss_length:.0f}", file=sys.stderr, flush=True)
    if not args.dry_run:
        serve(model.predict)

def test1() -> None:
    """Checksums for encode and decode"""
    from random import Random
    model = VoyagerModel(1, 1, "predict_min", [], [], 1)
    for i in range(10000):
        rand = Random(i)
        seq = [FINE_BYTES * rand.randint(0, 1000) for _ in range(100)]
        assert model._coarse_fine_join(*model._coarse_fine_split(seq)) == seq

def test2() -> None:
    """Checksum for windows"""
    from random import Random
    for hist_len in range(1, 32):
        rand = Random(hist_len)
        seq = [(FINE_BYTES * rand.randint(0, 1000), 16) for _ in range(100)]
        model = VoyagerModel(hist_len, 1, "predict_min", seq, [], 1)
        coarse, fine = model._coarse_fine_split(model.train_offsets)
        x, y = model._windows(coarse)
        assert all(len(x_item) == hist_len for x_item in x)

def test3() -> None:
    """Checksum for SequencClassifier"""
    input_coarse, input_fine = torch.randint(0, 1023, (8, 16)), torch.randint(0, 127, (8, 16))
    model = SequenceClassifier(2)
    output_coarse, output_fine = model(input_coarse, input_fine)
    assert output_coarse.shape == (8, 1024)
    assert output_fine.shape == (8, 128)
    

if __name__ == "__main__":
    main()
