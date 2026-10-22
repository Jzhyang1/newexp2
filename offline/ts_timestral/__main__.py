from __future__ import annotations

import torch
from timestral import TimestralForecaster

import argparse
import numpy as np
import sys
from typing import Sequence
from offline.data import load_training
from offline.loss import prediction_loss

Pair = tuple[int, int]
Vector = list[float]
SIN_PERIODS = (2.0, 4.0, 8.0, 16.0)


class TimestralModel:
    def __init__(self, history: int, mode: str, training: Sequence[Pair]):
        self.history = history
        self.mode = mode
        self.fallback = self._summary(training)
        self.model = TimestralForecaster.from_pretrained(
            "mistralai/Timestral-209M",
            device="cuda" if torch.cuda.is_available() else "cpu"
        )

    def _summary(self, pairs: Sequence[Pair]) -> Pair:
        if not pairs:
            return 0, 1
        values = list(pairs)
        if self.mode == "predict_min":
            return min(pair[0] for pair in values), min(pair[1] for pair in values)
        if self.mode == "predict_max":
            return max(pair[0] for pair in values), max(pair[1] for pair in values)
        return (
            round(sum(pair[0] for pair in values) / len(values)),
            max(1, round(sum(pair[1] for pair in values) / len(values))),
        )

    def _features(self, history: Sequence[Pair]) -> Vector:
        features: Vector = []
        for index, pair in enumerate(history):
            features.extend((float(pair[0]), float(pair[1])))
            # age = float(len(history) - index)
            # features.append(age)
            # features.extend(math.sin(math.tau * age / period) for period in SIN_PERIODS)
        return features

    def predict(self, previous: Sequence[Pair], count: int) -> list[Pair]:
        if count < 0:
            raise ValueError("next_n must be non-negative")
        history = list(previous)
        if len(history) < self.history:
            return [self.fallback] * count
        forecast = self.model.forecast(previous, count)
        predictions = forecast.median
        return [predictions]

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--type", choices=("predict_mean", "predict_min", "predict_max"), default="predict_mean")
    parser.add_argument("--min-len", type=int, default=1)
    parser.add_argument("--niceness", type=int, default=1)
    parser.add_argument("--predict-deltas", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--training", required=True)
    args = parser.parse_args()
    if args.min_len < 1:
        parser.error("--min-len must be at least 1")
    if args.niceness < 1:
        parser.error("--niceness must be at least 1")
    data = load_training(args.training, args.predict_deltas)
    split = min(len(data), max(args.min_len + 1, round(len(data) * 0.8)))
    train, test = data[:split], data[split:]
    model = TimestralModel(args.min_len, args.type, train)
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
        pass


if __name__ == "__main__":
    main()
