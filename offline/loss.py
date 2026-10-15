"""Shared scoring for offline time-series predictors."""

from __future__ import annotations

import math
from collections.abc import Callable, Sequence

Pair = tuple[int, int]


def prediction_loss(
    predict: Callable[[Sequence[Pair], int], list[Pair]],
    pairs: Sequence[Pair],
    history: int,
    niceness: int,
) -> tuple[float, float]:
    """Return RMSE, allowing each prediction to match the next few accesses."""
    if len(pairs) <= history:
        return 0.0, 0.0

    offset_errors: list[float] = []
    length_errors: list[float] = []
    for index in range(history, len(pairs)):
        predicted = predict(pairs[index - history : index], 1)[0]
        window = pairs[index : index + niceness]
        offset_errors.append(
            min((predicted[0] - actual[0]) ** 2 for actual in window)
        )
        length_errors.append(
            min((predicted[1] - actual[1]) ** 2 for actual in window)
        )
    return (
        math.sqrt(sum(offset_errors) / len(offset_errors)),
        math.sqrt(sum(length_errors) / len(length_errors)),
    )