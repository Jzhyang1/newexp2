"""Train and evaluate a convolutional LSTM predictor with Wasserstein loss."""

from __future__ import annotations

import argparse
import math
from collections.abc import Sequence
import sys

import torch
from torch import nn

from offline.data import load_training
from offline.loss import prediction_loss
from offline.loss_curve import show_loss_curve

Pair = tuple[int, int]
FeatureSequence = list[list[float]]
SIN_PERIODS = (2.0, 4.0, 8.0, 16.0)
FEATURES_PER_ACCESS = 3 + len(SIN_PERIODS)


def wasserstein_loss(predicted: torch.Tensor, actual: torch.Tensor) -> torch.Tensor:
    """Compute Wasserstein-1 for equally weighted one-dimensional samples."""
    predicted_values = torch.sort(predicted, dim=1).values
    actual_values = torch.sort(actual, dim=1).values
    return torch.abs(predicted_values - actual_values).mean()


class ConvSequenceRegressor(nn.Module):
    """Extract local patterns before recurrently modeling the access history."""

    def __init__(
        self,
        horizon: int,
        conv_channels: int,
        hidden_size: int,
        layers: int,
    ) -> None:
        super().__init__()
        self.horizon = horizon
        self.convolution = nn.Sequential(
            nn.Conv1d(
                FEATURES_PER_ACCESS,
                conv_channels,
                kernel_size=3,
                padding=1,
            ),
            nn.ReLU(),
        )
        self.lstm = nn.LSTM(
            conv_channels,
            hidden_size,
            layers,
            batch_first=True,
        )
        self.output = nn.Linear(hidden_size, horizon * 2)

    def forward(self, features: torch.Tensor) -> torch.Tensor:
        # Conv1d expects channels before the sequence dimension.
        local_features = self.convolution(features.transpose(1, 2)).transpose(1, 2)
        _, (hidden, _) = self.lstm(local_features)
        output = self.output(hidden[-1])
        return output.view(-1, self.horizon, 2)


class CWLSTMModel:
    def __init__(
        self,
        history: int,
        horizon: int,
        mode: str,
        train: Sequence[Pair],
        test: Sequence[Pair],
        epochs: int,
        conv_channels: int,
        hidden_size: int,
        layers: int,
    ) -> None:
        self.history = history
        self.horizon = horizon
        self.mode = mode
        self.fallback = self._summary(train)
        examples = [
            self._features(train[index - history : index])
            for index in range(history, len(train) - horizon + 1)
        ]
        targets = [
            train[index : index + horizon]
            for index in range(history, len(train) - horizon + 1)
        ]
        test_examples = [
            self._features(test[index - history : index])
            for index in range(history, len(test) - horizon + 1)
        ]
        test_targets = [
            test[index : index + horizon]
            for index in range(history, len(test) - horizon + 1)
        ]
        self.network: ConvSequenceRegressor | None = None
        self.feature_mean: torch.Tensor | None = None
        self.feature_scale: torch.Tensor | None = None
        self.target_mean: torch.Tensor | None = None
        self.target_scale: torch.Tensor | None = None
        self.train_losses: list[float] = []
        self.test_losses: list[float] = []
        self._fit(
            examples,
            targets,
            test_examples,
            test_targets,
            epochs,
            conv_channels,
            hidden_size,
            layers,
        )

    def _summary(self, pairs: Sequence[Pair]) -> Pair:
        if not pairs:
            return 0, 1
        if self.mode == "predict_min":
            return min(pair[0] for pair in pairs), min(pair[1] for pair in pairs)
        if self.mode == "predict_max":
            return max(pair[0] for pair in pairs), max(pair[1] for pair in pairs)
        return (
            round(sum(pair[0] for pair in pairs) / len(pairs)),
            max(1, round(sum(pair[1] for pair in pairs) / len(pairs))),
        )

    def _features(self, history: Sequence[Pair]) -> FeatureSequence:
        features: FeatureSequence = []
        for index, pair in enumerate(history):
            age = float(len(history) - index)
            vector = [float(pair[0]), float(pair[1]), age]
            vector.extend(math.sin(math.tau * age / period) for period in SIN_PERIODS)
            features.append(vector)
        return features

    def _fit(
        self,
        examples: Sequence[FeatureSequence],
        targets: Sequence[Sequence[Pair]],
        test_examples: Sequence[FeatureSequence],
        test_targets: Sequence[Sequence[Pair]],
        epochs: int,
        conv_channels: int,
        hidden_size: int,
        layers: int,
    ) -> None:
        features = torch.tensor(examples, dtype=torch.float32)
        target_values = torch.tensor(targets, dtype=torch.float32)
        test_features = torch.tensor(test_examples, dtype=torch.float32)
        test_target_values = torch.tensor(test_targets, dtype=torch.float32)
        self.feature_mean = features.mean(dim=(0, 1), keepdim=True)
        self.feature_scale = features.std(
            dim=(0, 1), keepdim=True, unbiased=False
        ).clamp_min(1e-6)
        self.target_mean = target_values.mean(dim=(0, 1), keepdim=True)
        self.target_scale = target_values.std(
            dim=(0, 1), keepdim=True, unbiased=False
        ).clamp_min(1e-6)
        features = (features - self.feature_mean) / self.feature_scale
        target_values = (target_values - self.target_mean) / self.target_scale
        test_features = (test_features - self.feature_mean) / self.feature_scale
        test_target_values = (test_target_values - self.target_mean) / self.target_scale
        torch.manual_seed(0)
        self.network = ConvSequenceRegressor(
            self.horizon,
            conv_channels,
            hidden_size,
            layers,
        )
        optimizer = torch.optim.Adam(self.network.parameters(), lr=0.01)
        self.network.train()
        for _ in range(epochs):
            optimizer.zero_grad()
            loss = wasserstein_loss(self.network(features), target_values)
            loss.backward()
            optimizer.step()
            self.train_losses.append(loss.item())

            with torch.no_grad():
                test_loss = wasserstein_loss(
                    self.network(test_features), test_target_values
                )
            self.test_losses.append(test_loss.item())

        self.network.eval()

    def _predict_horizon(self, history: Sequence[Pair]) -> list[Pair]:
        assert self.network is not None
        assert self.feature_mean is not None
        assert self.feature_scale is not None
        assert self.target_mean is not None
        assert self.target_scale is not None
        features = torch.tensor(
            [self._features(history[-self.history :])], dtype=torch.float32
        )
        features = (features - self.feature_mean) / self.feature_scale
        with torch.no_grad():
            result = self.network(features) * self.target_scale + self.target_mean
        return [
            (max(0, round(pair[0].item())), max(1, round(pair[1].item())))
            for pair in result[0]
        ]

    def predict(self, previous: Sequence[Pair], count: int) -> list[Pair]:
        if count < 0:
            raise ValueError("next_n must be non-negative")
        history = list(previous)
        if len(history) < self.history or self.network is None:
            return [self.fallback] * count
        predictions: list[Pair] = []
        while len(predictions) < count:
            forecast = self._predict_horizon(history)
            remaining = count - len(predictions)
            predictions.extend(forecast[:remaining])
            history.extend(forecast)
        return predictions


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--type",
        choices=("predict_mean", "predict_min", "predict_max"),
        default="predict_mean",
    )
    parser.add_argument("--min-len", type=int, default=1)
    parser.add_argument("--niceness", type=int, default=1)
    parser.add_argument("--predict-deltas", action="store_true")
    parser.add_argument("--epochs", type=int, default=800)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--conv-channels", type=int, default=16)
    parser.add_argument("--hidden-size", type=int, default=32)
    parser.add_argument("--layers", type=int, default=4)
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
    if args.conv_channels < 1 or args.hidden_size < 1 or args.layers < 1:
        parser.error("model dimensions must be at least 1")
    data = load_training(args.training, args.predict_deltas)
    split = min(len(data), max(args.min_len + 1, round(len(data) * 0.8)))
    train, test = data[:split], data[split:]

    model = CWLSTMModel(
        args.min_len,
        args.niceness,
        args.type,
        train,
        test,
        args.epochs,
        args.conv_channels,
        args.hidden_size,
        args.layers,
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
        pass


if __name__ == "__main__":
    main()
