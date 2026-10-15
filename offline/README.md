These are offline models that are trained once to fit example data, and 
A python server is provided. It will behave as follows:

run via `python -m <path>`

`ts_*` folders are timeseries predictors (i.e. they take in the last n offset,span pairs and predict the next offset,span pairs). The interface for these are: 
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