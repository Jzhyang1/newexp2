These are offline models that are trained once to fit example data, and 
A python server is provided. It will behave as follows:

run via `python -m <path>`

`offline.tsc_brute` is a full-disk LRCN baseline: two temporal convolutions,
the four-layer, 32-unit LSTM used by `ts_lstm`, and a dense output with
1,048,576 logits. It models a 4 GiB disk as 512 B blocks grouped into 4 KiB
pages. Sigmoid outputs give independent probabilities that each page is
accessed by the next request. Every page touched by that request is a positive
label; training minimizes mean binary cross entropy over all pages. CSV
`offset,length` values are bytes and must fit within the disk.

Example (using the repository virtual environment):

```sh
source ./bin/activate
python -m offline.tsc_brute --training logs/oracle.csv --min-len 32 \
    --epochs 70 --batch-size 8 --niceness 4 --alpha 0.5 --k 8 \
    --hide-loss-curve --dry-run
```

Training and held-out testing each report classification BCE and regression
RMSE in bytes. The regression prediction is the starting byte offset of the
most probable page. `--niceness N` lets that prediction match the closest of
the next N actual request offsets; it affects regression scoring only.
The chronological split is 80/20 and each split supplies its own history.
A split without enough history reports `nan`; insufficient training data or
out-of-disk requests are rejected. `--loss-curve PATH` saves both metric curves;
otherwise curves are displayed unless `--hide-loss-curve` is given.

The last parsed arguments, trained weights, and loss history are cached in
`offline/tsc_brute/training_cache.pt`. Runs with matching arguments reuse that
model without loading the CSV or retraining; changing any argument replaces
the cache after training. Delete the cache to retrain after changing the CSV
contents or model code while keeping the same arguments.

Omit `--dry-run` to serve the existing line-oriented JSON protocol. In this
server, `prev_accesses` and returned ranges use **512 B block units**, matching
the C++ `ModelPolicy`; the model converts them to/from byte units. For a
positive `next_n`, it returns at most one range for the next request. Around
the most probable page x, it chooses the widest inclusive interval
`[x-i, x+j]` with `0 <= i,j < k` and average page probability strictly greater
than `alpha`. Ties prefer higher average probability, then the lower start.
Intervals stay inside the disk. It returns `[]` when no interval qualifies,
history is too short, or `next_n` is zero. `put` acknowledges with `[]` and
`kill` exits; evaluation losses are reported after offline training.
`--type` is accepted for launcher compatibility and has no effect;
`--predict-deltas` is rejected because classes are absolute pages.

`offline.tsc_beam` classifies over a 1024 TiB (2**50 B) address space with 1024
buckets at every granularity: 1 TiB, 1 GiB, 1 MiB, 1 KiB (`--levels 5` adds
1 B). A history encoder (per-level bucket embeddings, two convolutions, LSTM)
runs once per request, then a GRU walks down the levels emitting 1024 class
probabilities per level, conditioned on the buckets chosen above. Training is
teacher-forced, so one pass scores every level (loss is -log P(leaf path)).
Prediction is a beam search (`--beam`, widened to `next_n` when larger) that
is batched over requests and beams: each level is one GRU step plus one matmul,
so evaluation cost is `levels` batched steps, not `beam * levels` per request.
Training and held-out testing report cross-entropy, beam recall of the true
leaf bucket, and top-1 RMSE in bytes. `get` returns up to `next_n` leaf
buckets as `(offset, leaf_size)` ranges, most probable first. Run it with
`python -m offline.tsc_beam --training logs/oracle.csv --min-len 8 --dry-run`.
No training cache is kept.

`ts_*` folders are timeseries regression models -- they take in the last n offset,span pairs and predict the next offset,span pairs. The interface for these are: 
boot parameters: 
    - `--type`: `predict_mean`, `predict_min`, or `predict_max`
    - `--min-len`: the minimum number of prev_accesses that is necessary to make a prediction
    - `--training`: a file path that contains the list of offset,span pairs to train off of
effects:
    - a training loss value is printed to the console upon build
    - a test loss value is printed to the console upon kill
runtime api:
    - accepts: `{type: "get", prev_accesses: (int,int)[], next_n: int}`, `{type: "put", predicted_accesses: (int,int)[], actual_accesses: (int,int)}` or `{type: "kill"}`
      where the accesses here are all least_offset,span pairs
    - returns: `(int,int)[]` of length `next_n`

`tsc_*` (proposed) folders are timeseries classification models -- there are several implementations for this. Below are the implementations listed.

1. Take in the last n offsets truncated to some precision (e.g. 64KiB) and predicts the next offsets to the same precision. Illustrative example: a series of 128 1-GiB precision offsets is passed into the model, which returns min-out of 128 1-GiB precision offsets as predicted future page accesses. The top values are decoded, and for each, it is passed into another model along with a sequence of 128 8-MiB precision offsets (note: consider taking subset of history and only use those that are in the same 1-GiB range; we don't need to do this if we include the GiB offset with each one-hot encoding), and return min-out 128 80MiB precision offsets. Then decode, take the top values, and repeat with 64-KiB precision values concatenated with the decoded GiB and MiB offsets. This is more optimized for prefetching tasks as our loss directly represent the difference between the entire set of future accesses and the predicted output (loss is regularized by the sum of predicted encodings and the sum of actual encodings). **What things are problematic**: if we had a single page that is highly likely in a 1-GiB range but all other pages are unlikely, then we may miss that page.

2. Following Voyager memory prefetching paper (similar architecture). We replace the page-sequence, offset-sequence inputs with 1-GiB sequence, **Why things are problematic**: 

The interface for these are: 
boot parameters: 
    - `--min-len`: the minimum number of prev_accesses that is necessary to make a prediction
    - `--min-out`: the number of outputs to generate at once
    - `--training`: a file path that contains the list of offset,span pairs to train off of
effects:
    - a training loss value is printed to the console upon build
    - a test loss value is printed to the console upon kill
runtime api:
    - accepts: `{type: "get", prev_accesses: (int,int)[], next_n: int}`, `{type: "put", predicted_accesses: (int,int)[], actual_accesses: (int,int)}` or `{type: "kill"}`
      where the accesses here are all least_offset,span pairs
    - returns: `(int,int)[]` of length `next_n`

`tsd_*` (proposed) folders are density models that are almost the same as `tsc_*`, however, we explicitly model the entire state of the disk, not just future accesses. The new concept is that we introduce a model that stores: the probability of each 1 GiB offset being needed, the probability of each 8 MiB offset being needed, and the probability of each 64 KiB offset being needed. **Why this is bad**: we cannot distinguish a page at 1 GiB + 8 MiB and 2 GiB + 8 MiB, so we will need to fetch both, but if only 1 GiB + 8 MiB appears with high probability, then we will also waste resources on 2 GiB + 8 MiB. **Why this may be acceptable**: we expect several offsets to all have high probability to be extremely low, and since we represent absolute probability, the amount that pagees in low-probability offset-groups interfere is low. This model will take the encoded current access density as input and output the encoded access density after k accesses. We decide whether a page is needed with the formula: $P(k,m,g) = P(k|m,g)P(m|g)P(g)$, and we find the $m,g$ that maximizes $P(k|m,g)$ and $P(m|g)$. Our sampling scheme is to take the top $K$ probabilities of 64 KiB offsets, the top $M$ probabilities of 8 MiB offsets, and the top $G$ probabilities of 1 GiB offsets, and take all pages formed by the combination of some k 64KiB + m 8MiB + g 1GiB.
