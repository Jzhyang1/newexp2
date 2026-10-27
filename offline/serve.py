"""Shared line-oriented JSON server for ModelPolicy (policies/policy_model.cpp).

The wire protocol counts 512 B blocks as unsigned 64-bit integers; training
traces (logs/oracle.csv) and every model count bytes. `serve` converts at the
boundary so models only ever see bytes.
"""

from __future__ import annotations

import json
import math
import sys
from collections.abc import Callable, Sequence
from typing import Any

Pair = tuple[int, int]
BLOCK_BYTES = 512
MAX_BLOCK = (1 << 64) - 1


def blocks_to_bytes(pairs: Sequence[Sequence[int]]) -> list[Pair]:
    return [(int(offset) * BLOCK_BYTES, int(length) * BLOCK_BYTES) for offset, length in pairs]


def bytes_to_blocks(pairs: Sequence[Sequence[float]]) -> list[Pair]:
    """Byte predictions to wire blocks: offsets floor into [0, 2**64), lengths round up to >= 1.

    Non-finite predictions are dropped; ModelPolicy rejects the whole reply
    if any pair is malformed.
    """
    result: list[Pair] = []
    for offset, length in pairs:
        if not (math.isfinite(offset) and math.isfinite(length)):
            continue
        result.append((min(max(0, int(offset)) // BLOCK_BYTES, MAX_BLOCK),
                       min(max(1, -(-int(length) // BLOCK_BYTES)), MAX_BLOCK)))
    return result


def serve(predict: Callable[[list[Pair], int], Sequence[Sequence[float]]]) -> None:
    """Answer get/put/kill requests on stdin/stdout until `kill` or EOF.

    `predict(previous, count)` takes a byte-unit history and returns up to
    `count` byte-unit (offset, length) predictions.
    """
    test_offset_errors: list[float] = []
    test_length_errors: list[float] = []
    for line in sys.stdin:
        request: dict[str, Any] = json.loads(line)
        request_type = request.get("type")
        if request_type == "get":
            previous = blocks_to_bytes(request["prev_accesses"])
            predictions = predict(previous, int(request["next_n"]))
            print(json.dumps(bytes_to_blocks(predictions)), flush=True)
        elif request_type == "put":
            predicted = request.get("predicted_accesses", [])
            actual = request["actual_accesses"]
            if predicted:
                # Errors are in blocks, as sent over the wire.
                test_offset_errors.append((int(predicted[0][0]) - int(actual[0])) ** 2)
                test_length_errors.append((int(predicted[0][1]) - int(actual[1])) ** 2)
            print("[]", flush=True)
        elif request_type == "kill":
            if test_offset_errors:
                print(f"offset test loss: {math.sqrt(sum(test_offset_errors) / len(test_offset_errors)):.2f}",
                      file=sys.stderr, flush=True)
                print(f"length test loss: {math.sqrt(sum(test_length_errors) / len(test_length_errors)):.2f}",
                      file=sys.stderr, flush=True)
            break
        else:
            raise ValueError(f"unknown request type: {request_type!r}")
