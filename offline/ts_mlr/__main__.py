"""Train and serve a multivariate linear time-series predictor."""

from __future__ import annotations

import argparse
import math
import pandas as pd
import numpy as np
import sys
from typing import Sequence
from sklearn import linear_model
from offline.loss import prediction_loss

Pair = tuple[int, int]
Vector = list[float]
SIN_PERIODS = (2.0, 4.0, 8.0, 16.0)


def load_training(path: str) -> list[Pair]:
    """Read offset,length pairs from CSV"""
    df = pd.read_csv(path)
    pairs: list[Pair] = []
    for row in df.itertuples():
        offset, length = row.offset, row.length
        pairs.append((offset, length))
    return pairs


class SVMModel:
    def __init__(self, history: int, mode: str, training: Sequence[Pair]):
        self.history = history
        self.mode = mode
        self.fallback = self._summary(training)
        x = [
            self._features(training[index - history : index])
            for index in range(history, len(training))
        ]
        y_offset = [training[index][0] for index in range(history, len(training))]
        y_length = [training[index][1] for index in range(history, len(training))]
        self._fit(x, y_offset, y_length)

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
            age = float(len(history) - index)
            features.extend((float(pair[0]), float(pair[1]), age))
            features.extend(math.sin(math.tau * age / period) for period in SIN_PERIODS)
        return features

    def _fit(self, x: Sequence[Vector], y_offset: Sequence[float], y_length: Sequence[float]) -> None:
        self.fit_offset = linear_model.LinearRegression().fit(x, y_offset)
        self.fit_length = linear_model.LinearRegression().fit(x, y_length)

    def predict(self, previous: Sequence[Pair], count: int) -> list[Pair]:
        if count < 0:
            raise ValueError("next_n must be non-negative")
        history = list(previous)
        if len(history) < self.history:
            return [self.fallback] * count
        predictions: list[Pair] = []
        for _ in range(count):
            features = [self._features(history[-self.history :])]
            offset, span = self.fit_offset.predict(features)[0], self.fit_length.predict(features)[0]
            prediction = (offset, round(span))
            predictions.append(prediction)
            history.append(prediction)
        return predictions

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--type", choices=("predict_mean", "predict_min", "predict_max"), default="predict_mean")
    parser.add_argument("--min-len", type=int, default=1)
    parser.add_argument("--niceness", type=int, default=1)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--training", required=True)
    args = parser.parse_args()
    if args.min_len < 1:
        parser.error("--min-len must be at least 1")
    if args.niceness < 1:
        parser.error("--niceness must be at least 1")
    training = load_training(args.training)
    model = SVMModel(args.min_len, args.type, training)
    split = max(args.min_len + 1, round(len(training) * 0.8))
    training_loss_offset, training_loss_length = prediction_loss(
        model.predict, training[:split], args.min_len, args.niceness
    )
    print(f"offset training loss: {training_loss_offset:.0f}", file=sys.stderr, flush=True)
    print(f"length training loss: {training_loss_length:.0f}", file=sys.stderr, flush=True)
    test_loss_offset, test_loss_length = prediction_loss(
        model.predict, training[split:], args.min_len, args.niceness
    )
    print(f"offset testing loss:  {test_loss_offset:.0f}", file=sys.stderr, flush=True)
    print(f"length testing loss:  {test_loss_length:.0f}", file=sys.stderr, flush=True)
    if not args.dry_run:
        raise NotImplementedError()


if __name__ == "__main__":
    main()
