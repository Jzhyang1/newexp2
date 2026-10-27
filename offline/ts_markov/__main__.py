"""Train and serve a Markov-chain time-series predictor."""

from __future__ import annotations

import argparse
import json
import math
import sys
from collections import Counter, defaultdict
from collections.abc import Sequence
from typing import Any

from offline.serve import serve
from offline.data import (
    load_training,
    preprocess_history,
    restore_prediction,
)
from offline.loss import prediction_loss

Pair = tuple[int, int]


class ConditionalModel:
    """Estimates P(next value | previous `order` values) from counts.

    When the exact context was never seen in training, the search backs off to
    progressively shorter suffixes of the context. If even the last value is
    unseen, the model returns `fallback`.
    """

    def __init__(self, order: int, series: Sequence[int], fallback: int):
        self.order = order
        self.fallback = fallback
        # counts[n][context] counts the values that followed an n-long context.
        self.counts: list[dict[tuple[int, ...], Counter[int]]] = [
            defaultdict(Counter) for _ in range(order + 1)
        ]
        for index in range(order, len(series)):
            for length in range(1, order + 1):
                context = tuple(series[index - length : index])
                self.counts[length][context][series[index]] += 1

    def probabilities(self, context: Sequence[int]) -> dict[int, float]:
        """Return P(next | context) using the longest context seen in training."""
        for length in range(self.order, 0, -1):
            followers = self.counts[length].get(tuple(context[-length:]))
            if followers:
                total = sum(followers.values())
                return {value: count / total for value, count in followers.items()}
        return {}

    def predict(self, context: Sequence[int]) -> int:
        """Return the most probable next value."""
        probabilities = self.probabilities(context)
        if not probabilities:
            return self.fallback
        return max(probabilities, key=probabilities.__getitem__)


class MarkovModel:
    """Predicts the next (offset, span) with one independent model per field.

    The offset model sees only the previous offsets and the span model sees
    only the previous spans.
    """

    def __init__(self, history: int, mode: str, training: Sequence[Pair]):
        self.history = history
        self.fallback = self._summary(mode, training)
        self.offset_model = ConditionalModel(
            history, [offset for offset, _ in training], self.fallback[0]
        )
        self.span_model = ConditionalModel(
            history, [span for _, span in training], self.fallback[1]
        )

    @staticmethod
    def _summary(mode: str, pairs: Sequence[Pair]) -> Pair:
        """Summarize the training data; used when a context was never seen."""
        if not pairs:
            return 0, 1
        offsets = [offset for offset, _ in pairs]
        spans = [span for _, span in pairs]
        if mode == "predict_min":
            return min(offsets), min(spans)
        if mode == "predict_max":
            return max(offsets), max(spans)
        return round(sum(offsets) / len(offsets)), max(1, round(sum(spans) / len(spans)))

    def predict(self, previous: Sequence[Pair], count: int) -> list[Pair]:
        if count < 0:
            raise ValueError("next_n must be non-negative")
        history = list(previous)
        if len(history) < self.history:
            return [self.fallback] * count
        predictions: list[Pair] = []
        for _ in range(count):
            recent = history[-self.history :]
            prediction = (
                self.offset_model.predict([offset for offset, _ in recent]),
                self.span_model.predict([span for _, span in recent]),
            )
            predictions.append(prediction)
            history.append(prediction)
        return predictions


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--type", choices=("predict_mean", "predict_min", "predict_max"), default="predict_min")
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
    model = MarkovModel(args.min_len, args.type, train)
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
        def predict(previous: list[Pair], count: int) -> list[Pair]:
            predictions = model.predict(preprocess_history(previous, args.predict_deltas), count)
            absolute: list[Pair] = []
            for prediction in predictions:
                absolute.append(restore_prediction(previous + absolute, prediction, args.predict_deltas))
            return absolute

        serve(predict)


if __name__ == "__main__":
    main()
