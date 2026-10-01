# CPU sampler regression tests

This tool compiles the engine's actual `HostSampler` and verbatim pure-MTP / chain-MTP accept-and-correct blocks against small synthetic distributions. It uses no HIP runtime, GPU, model weights, network, or third-party Python package. Requirements: Python 3.9+ and a C++17 compiler.

Default execution prints a plan. The source root defaults to the repository containing this tool. An explicit run writes all generated files into a new or empty `--outdir`; existing evidence is never overwritten.

```sh
python tools/sampler_cpu/validate.py
python tools/sampler_cpu/validate.py --run --source . --outdir ../sampler-cpu-results --compiler clang++ --trials 10000
python tools/sampler_cpu/validate.py --run --source . --outdir ../sampler-cpu-filters --compiler clang++ --suite filters
python tools/sampler_cpu/validate.py --run --source . --outdir ../sampler-cpu-conditional --compiler clang++ --suite conditional
```

`--compiler` accepts a name on PATH or a path to a compiler. Without it, the tool tries `clang++`, `g++`, `clang-cl`, then `cl`. MSVC-family compilers require their normal developer environment. C++ code uses only the standard library. Compiler options, source excerpt hashes, test-template hash and executable hash are recorded in `metadata.json`; diagnostics go to `compile.log`; full counts go to `results.jsonl`.

## Coverage

- Exact top-k logit ties, ties introduced by `expf` rounding, top-p, min-p, static history penalties and bias.
- Direct dense/sparse `HostSampler` probabilities and candidate order.
- Actual pure-MTP and chain-MTP accept/residual source blocks, gamma 1/4/8.
- Dense/dense, sparse/sparse, sparse proposal/dense verification and dense proposal/sparse verification.
- Equal target/proposal distributions, reversed probabilities, disjoint supports, and distributions changing with the previously emitted token.
- Three-token joint output distributions, deterministic repeated seeds and target-distribution logprob semantics.

The full suite has 96 configurations, each with 10,000 seeds by default. The conditional-only suite has 24. Seeds are reused across configurations; results are not independent aggregate evidence. The frequency threshold is a conservative regression threshold (`6 * binomial standard error + 6 / N` per joint bin), not a proof of equality or a formal p-value. Outcomes with zero target probability are forbidden. Finite-sample empirical total variation includes sampling noise and is not a model-quality-loss metric.

`path` in a distribution result means: 0 = dense/dense; 1 = sparse/sparse; 2 = sparse proposal/dense verification; 3 = dense proposal/sparse verification. For history-dependent rows, the first four raw logits are rotated by `(previous_token + 1) mod 4`; the first row uses the original order. Dynamic history penalties are not enabled in those statistical cases.

## Historical fixture

`fixtures/host_sampler_before_alignment.inc` is the unchanged `HostSampler` from commit `306676ec9c200ba2c82618429bf0bcba6e5d0daf`, with a regression-only header comment. Its normalized source hash is checked against `fixtures/baseline.json`. It is compiled into a separate namespace and is never used by the engine. Baseline failures are recorded as observations and do not fail the current implementation's test suite.

This baseline shows why selecting top-k after exponentiation is unsafe: distinct logits may round to equal probabilities, and an unordered tie can select a different support from the sparse path's `(raw corrected logit descending, token ID ascending)` ordering. Some tie examples happen to match on a given standard library; the suite preserves those results too.

## Evidence boundary

The sampler and acceptance/correction blocks are verbatim source excerpts, not an independently rewritten replacement algorithm. Extraction fails if the expected source anchors disappear. Synthetic CPU glue supplies logits, proposals and histories, and uses the real final GPU-path ordering statement on a complete CPU candidate set. It does **not** execute or validate GPU radix/candidate-collection kernels, GPU correction rounding, real model logits, KV/GDN/conv state, rollback, cancellation or multi-slot serving.

In particular, rejection sampling preserves the distribution supplied to the verifier. If batched model logits differ from serial logits, these tests do not make the two target distributions equal. Pure ngram's sampled token-match branch is separate from the MTP `p/q` branch tested here.

A small future refactor could move `HostSampler` into a standard-library-only header and consolidate the duplicate accept/correct blocks into one directly callable helper shared by both serving paths and these tests. The current source-excerpt approach avoids changing GPU engine control flow for testing.
