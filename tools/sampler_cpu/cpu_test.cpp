// CPU glue around verbatim source excerpts emitted by validate.py.
// The GPU's candidate-collection kernels and model states are NOT emulated.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace before_fix {
#include "baseline.inc"
}
#include "current.inc"

struct Config { int vocab = 6; } g_cfg;

template <typename S>
S sampler_for(uint64_t seed, int k = 4, float temp = 1.0f) {
  S s;
  s.temp = temp; s.top_p = 1; s.min_p = 0; s.top_k = k;
  s.rng = seed; s.presence = 0; s.frequency = 0;
  return s;
}

// Supplies all candidate logits on CPU, then executes the actual final
// ordering statement from sms_topk_rows. This does NOT test GPU candidate
// collection, correction rounding, or an overflow/fallback transition.
template <typename S>
void sparse_from_logits(S& s, std::vector<float> raw, typename S::SDist& out) {
  for (auto& b : s.bias)
    if (b.first >= 0 && b.first < (int)raw.size()) raw[b.first] += b.second;
  if (s.presence != 0 || s.frequency != 0)
    for (auto& h : s.hist)
      if (h.first >= 0 && h.first < (int)raw.size())
        raw[h.first] -= s.presence + s.frequency * (float)h.second;
  std::vector<int> all(raw.size()), ord(raw.size());
  std::iota(all.begin(), all.end(), 0);
  std::iota(ord.begin(), ord.end(), 0);
  const int* ids = all.data();
  const float* vals = raw.data();
#include "gpu-order.inc"
  int k = s.top_k > 0 ? std::min(s.top_k, (int)raw.size()) : (int)raw.size();
  std::vector<int> selected(k);
  std::vector<float> selected_logits(k);
  for (int j = 0; j < k; ++j) {
    selected[j] = ids[ord[j]];
    selected_logits[j] = vals[ord[j]];
  }
  s.prepare_sparse(selected.data(), selected_logits.data(), k, out);
}

struct Round {
  std::vector<int> tokens;
  std::vector<float> logprobs;
  int accepted;
};

template <typename Sampler>
struct Selection {
  std::vector<std::vector<float>> raw_target;

  void sms_prep(Sampler& s, int row, bool, typename Sampler::SDist& out) {
    sparse_from_logits(s, raw_target.at(row), out);
  }

  Round pure(Sampler& sampler, Sampler& accept_sampler,
             const std::vector<int>& drafts,
             std::vector<std::vector<float>> q_rows,
             std::vector<typename Sampler::SDist> q_sd,
             std::vector<char> q_sparse, bool smg_v) {
    const int g = (int)drafts.size();
    std::vector<float> verify_logits, p_row(g_cfg.vocab), draft_lps(g);
    for (auto& row : raw_target) verify_logits.insert(verify_logits.end(), row.begin(), row.end());
#include "pure.inc"
    Round result; result.accepted = a;
    result.tokens.assign(drafts.begin(), drafts.begin() + a);
    result.logprobs.assign(draft_lps.begin(), draft_lps.begin() + a);
    result.tokens.push_back(next); result.logprobs.push_back(next_lp);
    return result;
  }

  Round chain(Sampler& sampler, Sampler& accept_sampler,
              const std::vector<int>& drafts,
              std::vector<std::vector<float>> q_rows,
              std::vector<typename Sampler::SDist> q_sd,
              std::vector<char> q_sparse, bool smg_v) {
    const int g = (int)drafts.size();
    std::vector<float> verify_logits, p_row(g_cfg.vocab), draft_lps(g);
    for (auto& row : raw_target) verify_logits.insert(verify_logits.end(), row.begin(), row.end());
#include "chain.inc"
    Round result; result.accepted = a;
    result.tokens.assign(drafts.begin(), drafts.begin() + a);
    result.logprobs.assign(draft_lps.begin(), draft_lps.begin() + a);
    result.tokens.push_back(next); result.logprobs.push_back(next_lp);
    return result;
  }
};

template <typename T> void array_json(const std::vector<T>& v) {
  std::cout << '[';
  for (size_t i = 0; i < v.size(); ++i) { if (i) std::cout << ','; std::cout << v[i]; }
  std::cout << ']';
}

template <typename S>
bool filtering_case(const std::string& name, const std::vector<float>& raw,
                    S base, bool baseline) {
  S dense = base, sparse = base;
  auto probabilities = raw;
  dense.prepare(probabilities);
  typename S::SDist sd;
  sparse_from_logits(sparse, raw, sd);
  double max_error = 0;
  std::vector<int> support;
  for (int i = 0; i < (int)raw.size(); ++i) {
    if (probabilities[i] > 0) support.push_back(i);
    max_error = std::max(max_error, std::abs((double)probabilities[i] - S::sdist_prob(sd, i)));
  }
  const bool order = dense.cand == sd.ids;
  const bool good = max_error < 1e-7 && order;
  std::cout << "{\"test\":\"filter_dense_sparse\",\"case\":\"" << name
            << "\",\"implementation\":\"" << (baseline ? "baseline" : "current")
            << "\",\"distribution_max_abs_error\":" << max_error
            << ",\"candidate_order_equal\":" << (order ? "true" : "false")
            << ",\"dense_candidate_ids\":"; array_json(dense.cand);
  std::cout << ",\"sparse_candidate_ids\":"; array_json(sd.ids);
  std::cout << ",\"pass\":" << (good ? "true" : "false") << "}\n";
  return good;
}

int filtering_tests() {
  int failures = 0;
  const std::vector<std::pair<std::string, std::vector<float>>> cases = {
    {"exact_raw_logit_ties", {1,1,1,1,1,1,1,1}},
    {"exact_raw_logit_ties_large_partition", std::vector<float>(128, 1.0f)},
    {"mixed_raw_logit_ties", [] { std::vector<float> v(128); for (int i=0; i<128; ++i) v[i] = (float)(i % 2); return v; }()},
    {"exp_rounding_creates_ties", {0,1e-9f,2e-9f,3e-9f,4e-9f,5e-9f,6e-9f,7e-9f}},
    {"distinct_logits", {-3,2,4,1,-1,3,0,-2}},
    {"large_negative_logits", {-1000,-1001,-1002,-1003,-1004,-1005,-1006,-1007}},
  };
  for (auto& test : cases) {
    float temp = 1.0f;
    auto current = sampler_for<HostSampler>(777, 3, temp);
    auto baseline = sampler_for<before_fix::HostSampler>(777, 3, temp);
    filtering_case(test.first, test.second, baseline, true);
    if (!filtering_case(test.first, test.second, current, false)) ++failures;
    if (test.first == "exp_rounding_creates_ties") {
      const float hi = test.second.back();
      bool all_tied = true;
      for (float value : test.second) all_tied &= expf((value - hi) / temp) == 1.0f;
      if (!all_tied) throw std::runtime_error("Test did not create the intended expf tie");
    }
  }
  for (int variant = 0; variant < 3; ++variant) {
    auto current = sampler_for<HostSampler>(123, 5, .8f);
    current.top_p = variant == 0 ? .7f : 1;
    current.min_p = variant == 1 ? .15f : 0;
    if (variant == 2) {
      current.presence = .3f; current.frequency = .2f;
      current.hist = {{1, 2}, {3, 1}};
      current.bias = {{0, .1f}, {5, -.2f}};
    }
    if (!filtering_case("filters_" + std::to_string(variant), {2,2,2,1,0,-1,-2,-3}, current, false)) ++failures;
  }
  auto plain = sampler_for<HostSampler>(0, 0, .7f);
  std::vector<float> logits = {-.3f, 0, .2f, .6f, -2, 1};
  auto actual = logits;
  plain.prepare(actual);
  double sum = 0, maxerr = 0;
  for (float x : logits) sum += std::exp(((double)x - 1) / .7f);
  for (size_t j = 0; j < logits.size(); ++j)
    maxerr = std::max(maxerr, std::abs(actual[j] - std::exp(((double)logits[j] - 1) / .7f) / sum));
  if (maxerr > 1e-6) ++failures;
  std::cout << "{\"test\":\"unfiltered_softmax_vs_double_oracle\",\"max_abs_error\":" << maxerr << ",\"pass\":" << (maxerr <= 1e-6 ? "true" : "false") << "}\n";
  return failures;
}

std::vector<float> logits_of(const std::vector<float>& probabilities) {
  std::vector<float> result;
  for (float p : probabilities) result.push_back(p > 0 ? logf(p) : -INFINITY);
  return result;
}

struct StreamResult { std::vector<int> tokens; bool logprobs_ok = true; long accepted = 0, proposed = 0, rounds = 0; };

std::vector<float> state_logits(const std::vector<float>& base, int previous, bool conditional) {
  if (!conditional || previous < 0) return base;
  auto result = base;
  for (int i = 0; i < 4; ++i) result[(i + previous + 1) % 4] = base[i];
  return result;
}

StreamResult sequence(const std::vector<float>& p_raw, const std::vector<float>& q_raw,
                      uint64_t seed, int gamma, int path, bool chain, bool conditional) {
  auto sampler = sampler_for<HostSampler>(seed);
  StreamResult result;
  while (result.tokens.size() < 3) {
    int g = gamma;
    // Actual engine fork tags and q+1 history position; no GPU state exists.
    uint64_t position = 101 + result.tokens.size();
    auto draft_sampler = sampler.fork_stream(0xd1b54a32d192ed03ull ^ position);
    auto accept_sampler = sampler.fork_stream(0x3c6ef372fe94f82aull ^ position);
    std::vector<int> drafts(g);
    std::vector<std::vector<float>> q_rows(g, q_raw);
    std::vector<HostSampler::SDist> q_sd(g);
    std::vector<char> q_sparse(g, (path == 1 || path == 2));
    for (int j = 0; j < g; ++j) {
      int previous = j ? drafts[j - 1] : result.tokens.empty() ? -1 : result.tokens.back();
      q_rows[j] = state_logits(q_raw, previous, conditional);
      if (q_sparse[j]) {
        sparse_from_logits(draft_sampler, q_rows[j], q_sd[j]);
        drafts[j] = draft_sampler.draw_sparse(q_sd[j], nullptr, false);
      } else {
        draft_sampler.prepare(q_rows[j]);
        drafts[j] = draft_sampler.draw_prepared(q_rows[j], nullptr, false);
      }
    }
    Selection<HostSampler> selection;
    for (int j = 0; j <= g; ++j) {
      int previous = j ? drafts[j - 1] : result.tokens.empty() ? -1 : result.tokens.back();
      selection.raw_target.push_back(state_logits(p_raw, previous, conditional));
    }
    const bool verify_sparse = path == 1 || path == 3;
    Round r = chain ? selection.chain(sampler, accept_sampler, drafts, q_rows, q_sd, q_sparse, verify_sparse)
                    : selection.pure(sampler, accept_sampler, drafts, q_rows, q_sd, q_sparse, verify_sparse);
    result.accepted += r.accepted; result.proposed += g; ++result.rounds;
    for (size_t j = 0; j < r.tokens.size() && result.tokens.size() < 3; ++j) {
      auto target = sampler_for<HostSampler>(0);
      auto p = state_logits(p_raw, result.tokens.empty() ? -1 : result.tokens.back(), conditional);
      target.prepare(p);
      int token = r.tokens[j];
      result.logprobs_ok &= token >= 0 && token < g_cfg.vocab && p[token] > 0 &&
        std::isfinite(r.logprobs[j]) && std::abs(r.logprobs[j] - logf(p[token])) < 2e-6f;
      result.tokens.push_back(token);
      sampler.remember(token);
    }
  }
  return result;
}

int distribution_tests(int trials, bool conditional_only) {
  struct Case { const char* name; std::vector<float> p, q; bool conditional = false; };
  const std::vector<Case> cases = {
    {"equal", {.15f,.25f,.2f,.4f,0,0}, {.15f,.25f,.2f,.4f,0,0}},
    {"reversed", {.05f,.15f,.3f,.5f,0,0}, {.5f,.3f,.15f,.05f,0,0}},
    {"disjoint", {.4f,.6f,0,0,0,0}, {0,0,0,0,.6f,.4f}},
    {"history_dependent", {.05f,.15f,.3f,.5f,0,0}, {.5f,.3f,.15f,.05f,0,0}, true},
  };
  int failures = 0;
  for (const auto& c : cases) for (int gamma : {1,4,8}) for (int path = 0; path < 4; ++path) for (bool chain : {false, true}) {
    if (conditional_only && !c.conditional) continue;
    auto p_raw = logits_of(c.p), q_raw = logits_of(c.q);
    auto target = sampler_for<HostSampler>(0);
    auto pf = p_raw; target.prepare(pf);
    double norm = std::accumulate(pf.begin(), pf.end(), 0.0);
    std::vector<double> p; for (float x : pf) p.push_back(x / norm);
    std::vector<std::vector<double>> transitions;
    for (int previous = 0; previous < 6; ++previous) {
      auto row = state_logits(p_raw, previous, c.conditional);
      auto view = sampler_for<HostSampler>(0); view.prepare(row);
      double z = std::accumulate(row.begin(), row.end(), 0.0);
      transitions.emplace_back();
      for (float x : row) transitions.back().push_back(x / z);
    }
    std::vector<long> counts(216, 0);
    long accepted = 0, proposed = 0, rounds = 0;
    bool logprobs = true, reproducible = true;
    for (int trial = 0; trial < trials; ++trial) {
      uint64_t seed = 0x5a170000ull + trial;
      auto r = sequence(p_raw, q_raw, seed, gamma, path, chain, c.conditional);
      if (trial < 3) reproducible &= r.tokens == sequence(p_raw, q_raw, seed, gamma, path, chain, c.conditional).tokens;
      ++counts[(r.tokens[0] * 6 + r.tokens[1]) * 6 + r.tokens[2]];
      accepted += r.accepted; proposed += r.proposed; rounds += r.rounds;
      logprobs &= r.logprobs_ok;
    }
    double tv = 0, maxz = 0;
    bool good = logprobs && reproducible;
    int occupied = 0;
    for (int a = 0; a < 6; ++a) for (int b = 0; b < 6; ++b) for (int d = 0; d < 6; ++d) {
      int index = (a * 6 + b) * 6 + d;
      const double expected = p[a] * transitions[a][b] * transitions[b][d];
      const double observed = (double)counts[index] / trials;
      const double err = std::abs(observed - expected);
      tv += err * .5;
      const double sd = std::sqrt(expected * (1 - expected) / trials);
      if (expected > 0) {
        ++occupied;
        maxz = std::max(maxz, sd > 0 ? err / sd : 0.0);
        // Six sigma plus six-count allowance: a conservative regression
        // threshold, not a proof of distribution equality or a p-value.
        if (err > 6 * sd + 6.0 / trials) good = false;
      } else if (counts[index] != 0) good = false;
    }
    if (!good) ++failures;
    std::cout << "{\"test\":\"three_token_joint_target_distribution\",\"case\":\"" << c.name
              << "\",\"selection_excerpt\":\"" << (chain ? "chain" : "pure")
              << "\",\"gamma\":" << gamma << ",\"path\":" << path
              << ",\"history_dependent\":" << (c.conditional ? "true" : "false")
              << ",\"trials\":" << trials << ",\"joint_support_bins\":" << occupied
              << ",\"total_variation_empirical\":" << tv << ",\"max_bin_z\":" << maxz
              << ",\"logprobs_target_semantics\":" << (logprobs ? "true" : "false")
              << ",\"repeat_seed_deterministic\":" << (reproducible ? "true" : "false")
              << ",\"accepted\":" << accepted << ",\"proposed\":" << proposed
              << ",\"rounds\":" << rounds << ",\"joint_counts\":";
    array_json(counts);
    std::cout << ",\"target_probabilities\":"; array_json(p);
    std::cout << ",\"pass\":" << (good ? "true" : "false") << "}\n";
  }
  return failures;
}

int main(int argc, char** argv) {
  try {
    std::cout << std::setprecision(10);
    int trials = argc > 1 ? std::atoi(argv[1]) : 30000;
    bool filters_only = argc > 2 && std::string(argv[2]) == "filters";
    bool conditional_only = argc > 2 && std::string(argv[2]) == "conditional";
    int failures = filtering_tests();
    if (!filters_only) failures += distribution_tests(trials, conditional_only);
    std::cout << "{\"summary\":true,\"failures\":" << failures
              << ",\"gpu_executed\":false,\"source_selection_blocks\":2,\"distribution_cases\":" << (filters_only ? 0 : conditional_only ? 24 : 96) << "}\n";
    return failures ? 1 : 0;
  } catch (const std::exception& e) {
    std::cerr << "CPU_TEST_EXCEPTION " << e.what() << '\n';
    return 2;
  }
}
