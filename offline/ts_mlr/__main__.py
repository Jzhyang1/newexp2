"""Train and serve a multivariate linear time-series predictor."""

from __future__ import annotations

import argparse
import json
import math
import numpy as np
import sys
from typing import Any, Sequence
from sklearn import linear_model
from offline.data import load_training
from offline.loss import prediction_loss

Pair = tuple[int, int]
Vector = list[float]
SIN_PERIODS = (2.0, 4.0, 8.0, 16.0)


class MLRModel:
    def __init__(self, history: int, mode: str, training: Sequence[Pair], offset_length_covar: bool):
        self.history = history
        self.mode = mode
        self.fallback = self._summary(training)
        self.offset_length_covar = offset_length_covar
        x = [
            training[index - history : index]
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
        for pair in history:
            # empirically, extending the features with sine/age has not been helpful
            features.extend((float(pair[0]), float(pair[1])))
        return features
    
    def _features_offset(self, history: Sequence[Pair]) -> Vector:
        if self.offset_length_covar: return self._features(history)
        return [float(pair[0]) for pair in history]
    

    def _features_length(self, history: Sequence[Pair]) -> Vector:
        if self.offset_length_covar: return self._features(history)
        return [float(pair[1]) for pair in history]

    def _fit(self, x: Sequence[Vector[Pair]], y_offset: Sequence[float], y_length: Sequence[float]) -> None:
        self.fit_offset = linear_model.LinearRegression().fit([self._features_offset(x_item) for x_item in x], y_offset)
        self.fit_length = linear_model.LinearRegression().fit([self._features_length(x_item) for x_item in x], y_length)

    def predict(self, previous: Sequence[Pair], count: int) -> list[Pair]:
        if count < 0:
            raise ValueError("next_n must be non-negative")
        history = list(previous)
        if len(history) < self.history:
            return [self.fallback] * count
        predictions: list[Pair] = []
        for _ in range(count):
            h = history[-self.history:]
            offset_feat, length_feat = [self._features_offset(h)], [self._features_length(h)]
            offset, span = self.fit_offset.predict(offset_feat)[0], self.fit_length.predict(length_feat)[0]
            prediction = (offset, round(span))
            predictions.append(prediction)
            history.append(prediction)
        return predictions

def serve(model: MLRModel) -> None:
    """Serve the line-oriented JSON protocol used by ModelPolicy."""
    test_offset_errors: list[float] = []
    test_length_errors: list[float] = []
    for line in sys.stdin:
        request: dict[str, Any] = json.loads(line)
        request_type = request.get("type")
        if request_type == "get":
            previous = [tuple(pair) for pair in request["prev_accesses"]]
            print(json.dumps(model.predict(previous, int(request["next_n"]))), flush=True)
        elif request_type == "put":
            predicted = request.get("predicted_accesses", [])
            actual = tuple(request["actual_accesses"])
            if predicted:
                test_offset_errors.append((int(predicted[0][0]) - actual[0]) ** 2)
                test_length_errors.append((int(predicted[0][1]) - actual[1]) ** 2)
            print("[]", flush=True)
        elif request_type == "kill":
            if test_offset_errors:
                print(
                    f"offset test loss: {math.sqrt(sum(test_offset_errors) / len(test_offset_errors)):.2f}",
                    file=sys.stderr,
                    flush=True,
                )
                print(
                    f"length test loss: {math.sqrt(sum(test_length_errors) / len(test_length_errors)):.2f}",
                    file=sys.stderr,
                    flush=True,
                )
            break
        else:
            raise ValueError(f"unknown request type: {request_type!r}")

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--type", choices=("predict_mean", "predict_min", "predict_max"), default="predict_mean")
    parser.add_argument("--min-len", type=int, default=1)
    parser.add_argument("--niceness", type=int, default=1)
    parser.add_argument("--predict-deltas", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--covar-o-l", action="store_true", help="Whether offsets should see previous sequences of lengths and vice versa")
    parser.add_argument("--training", required=True)
    args = parser.parse_args()
    if args.min_len < 1:
        parser.error("--min-len must be at least 1")
    if args.niceness < 1:
        parser.error("--niceness must be at least 1")
    data = load_training(args.training, args.predict_deltas)
    split = min(len(data), max(args.min_len + 1, round(len(data) * 0.8)))
    train, test = data[:split], data[split:]
    model = MLRModel(args.min_len, args.type, train, args.covar_o_l)
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
