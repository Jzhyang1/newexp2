"""Train and serve a LSTM time-series predictor."""

from __future__ import annotations

import argparse
import math
import json
import sys
from typing import Any, Sequence
import torch
from torch import nn
from offline.data import load_training
from offline.loss import prediction_loss

Pair = tuple[int, int]
FeatureSequence = list[list[float]]
SIN_PERIODS = (2.0, 4.0, 8.0, 16.0)
FEATURES_PER_ACCESS = 3 + len(SIN_PERIODS)


class SequenceRegressor(nn.Module):
    """Map one fixed-length access history to the next offset and length."""

    def __init__(self) -> None:
        super().__init__()
        self.lstm = nn.LSTM(FEATURES_PER_ACCESS, 32, 4, batch_first=True)
        self.output = nn.Linear(32, 2)

    def forward(self, features: torch.Tensor) -> torch.Tensor:
        _, (hidden, _) = self.lstm(features)
        return self.output(hidden[-1])


class LSTMModel:
    def __init__(self, history: int, mode: str, training: Sequence[Pair]):
        self.history = history
        self.mode = mode
        self.fallback = self._summary(training)
        examples = [
            self._features(training[index - history : index])
            for index in range(history, len(training))
        ]
        targets = [training[index] for index in range(history, len(training))]
        self.network: SequenceRegressor | None = None
        self.feature_mean: torch.Tensor | None = None
        self.feature_scale: torch.Tensor | None = None
        self.target_mean: torch.Tensor | None = None
        self.target_scale: torch.Tensor | None = None
        self._fit(examples, targets)

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

    def _features(self, history: Sequence[Pair]) -> FeatureSequence:
        """Build one feature vector per timestep, preserving sequence order."""
        features: FeatureSequence = []
        for index, pair in enumerate(history):
            age = float(len(history) - index)
            vector = [float(pair[0]), float(pair[1]), age]
            vector.extend(math.sin(math.tau * age / period) for period in SIN_PERIODS)
            features.append(vector)
        return features

    def _fit(self, x: Sequence[FeatureSequence], y: Sequence[Pair]) -> None:
        if not x or not y:
            return
        # Normalize offsets and lengths separately: offsets can be much larger
        # than request lengths, and the scales are needed to undo this later.
        features = torch.tensor(x, dtype=torch.float32)
        targets = torch.tensor(y, dtype=torch.float32)
        self.feature_mean = features.mean(dim=(0, 1), keepdim=True)
        self.feature_scale = features.std(dim=(0, 1), keepdim=True).clamp_min(1e-6)
        self.target_mean = targets.mean(dim=0, keepdim=True)
        self.target_scale = targets.std(dim=0, keepdim=True).clamp_min(1e-6)
        features = (features - self.feature_mean) / self.feature_scale
        targets = (targets - self.target_mean) / self.target_scale
        torch.manual_seed(0)
        self.network = SequenceRegressor()
        optimizer = torch.optim.Adam(self.network.parameters(), lr=0.01)
        loss_fn = nn.MSELoss()
        self.network.train()
        for _ in range(100):
            optimizer.zero_grad()
            loss = loss_fn(self.network(features), targets)
            loss.backward()
            optimizer.step()
        self.network.eval()

    def predict(self, previous: Sequence[Pair], count: int) -> list[Pair]:
        if count < 0:
            raise ValueError("next_n must be non-negative")
        history = list(previous)
        if len(history) < self.history or self.network is None:
            return [self.fallback] * count
        assert self.feature_mean is not None
        assert self.feature_scale is not None
        assert self.target_mean is not None
        assert self.target_scale is not None
        predictions: list[Pair] = []
        for _ in range(count):
            features = torch.tensor(
                [self._features(history[-self.history :])], dtype=torch.float32
            )
            features = (features - self.feature_mean) / self.feature_scale
            with torch.no_grad():
                result = self.network(features) * self.target_scale + self.target_mean
            prediction = (
                max(0, round(result[0, 0].item())),
                max(1, round(result[0, 1].item())),
            )
            predictions.append(prediction)
            history.append(prediction)
        return predictions

def serve(model: LSTMModel) -> None:
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
    parser.add_argument("--training", required=True)
    args = parser.parse_args()
    if args.min_len < 1:
        parser.error("--min-len must be at least 1")
    if args.niceness < 1:
        parser.error("--niceness must be at least 1")
    training = load_training(args.training, args.predict_deltas)
    model = LSTMModel(args.min_len, args.type, training)
    split = min(len(training), max(args.min_len + 1, round(len(training) * 0.8)))
    training_loss_offset, training_loss_length = prediction_loss(
        model.predict, training[:split], args.min_len, args.niceness
    )
    print(f"offset training loss: {training_loss_offset:.0f}", file=sys.stderr, flush=True)
    print(f"length training loss: {training_loss_length:.0f}", file=sys.stderr, flush=True)
    test_loss_offset, test_loss_length = prediction_loss(
        model.predict, training[split:], args.min_len, args.niceness
    )
    print(f"offset training loss: {test_loss_offset:.0f}", file=sys.stderr, flush=True)
    print(f"length training loss: {test_loss_length:.0f}", file=sys.stderr, flush=True)
    if not args.dry_run:
        serve(model)


if __name__ == "__main__":
    main()
