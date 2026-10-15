"""Shared preprocessing for offline time-series predictors."""

from __future__ import annotations

from collections.abc import Sequence

import pandas as pd

Pair = tuple[int, int]


def load_training(path: str, predict_deltas: bool = False) -> list[Pair]:
    """Read a training trace, optionally representing offsets as deltas."""
    df = pd.read_csv(path)
    pairs = [(int(row.offset), int(row.length)) for row in df.itertuples()]
    if not predict_deltas:
        return pairs
    return [
        (offset if index == 0 else offset - pairs[index - 1][0], length)
        for index, (offset, length) in enumerate(pairs)
    ]


def preprocess_history(history: Sequence[Pair], predict_deltas: bool) -> list[Pair]:
    """Represent a history as offsets or offset deltas plus lengths."""
    values = list(history)
    if not predict_deltas:
        return values
    return [
        (offset if index == 0 else offset - values[index - 1][0], length)
        for index, (offset, length) in enumerate(values)
    ]


def preprocess_target(
    previous: Sequence[Pair], target: Pair, predict_deltas: bool
) -> Pair:
    """Represent one target as an absolute offset or a delta."""
    if not predict_deltas or not previous:
        return target
    return target[0] - previous[-1][0], target[1]


def restore_prediction(
    previous: Sequence[Pair], prediction: Pair, predict_deltas: bool
) -> Pair:
    """Convert a model prediction back to the absolute-offset protocol."""
    if not predict_deltas or not previous:
        return prediction
    return previous[-1][0] + prediction[0], prediction[1]


def preprocess_targets(pairs: Sequence[Pair], predict_deltas: bool) -> list[Pair]:
    """Convert a complete training trace into target representations."""
    return [
        preprocess_target(pairs[index - 1 : index], pair, predict_deltas)
        for index, pair in enumerate(pairs)
    ]
