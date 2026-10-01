// Historical HostSampler from commit 306676ec9c200ba2c82618429bf0bcba6e5d0daf.
// Regression-only fixture; never included by the inference engine.
// Original source: src/gpu/parts/51_host_cfg.inc. See baseline.json.
struct HostSampler {
  float temp, top_p, min_p;
  int top_k;
  uint64_t rng;
  float presence, frequency;
  std::vector<std::pair<int, float>> bias;
  std::unordered_map<int, int> hist;  // emitted token counts (not the prompt)
  std::vector<int> cand;              // scratch: candidate token ids
  std::vector<float> work;            // scratch: top-k preservation / residual

  static uint64_t mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  uint64_t next64() {  // splitmix64
    return mix64(rng += 0x9E3779B97F4A7C15ull);
  }

  double uniform() {
    return (double)(next64() >> 11) * (1.0 / 9007199254740992.0);
  }

  HostSampler fork_stream(uint64_t tag) const {
    HostSampler out = *this;
    out.rng = mix64(rng ^ tag);
    out.cand.clear();
    out.work.clear();
    return out;
  }

  void remember(int id) { hist[id]++; }

  // Convert raw logits in place to the normalized distribution after the
  // engine's penalty/bias -> temperature -> top-k -> top-p -> min-p pipeline.
  void prepare(std::vector<float>& logits) {
    const int n = (int)logits.size();
    for (auto& b : bias)
      if (b.first >= 0 && b.first < n) logits[b.first] += b.second;
    if (presence != 0.0f || frequency != 0.0f)
      for (auto& h : hist)
        if (h.first >= 0 && h.first < n)
          logits[h.first] -= presence + frequency * (float)h.second;
    float lmax = -INFINITY;
    for (int i = 0; i < n; i++) lmax = fmaxf(lmax, logits[i]);
    // unnormalized probs after temperature
    for (int i = 0; i < n; i++) logits[i] = expf((logits[i] - lmax) / temp);
    cand.clear();
    if (top_k > 0 && top_k < n) {
      cand.resize(n);
      for (int i = 0; i < n; i++) cand[i] = i;
      std::nth_element(cand.begin(), cand.begin() + top_k, cand.end(),
                       [&](int a, int b) { return logits[a] > logits[b]; });
      cand.resize(top_k);
      work.resize(cand.size());
      for (size_t j = 0; j < cand.size(); j++) work[j] = logits[cand[j]];
      std::fill(logits.begin(), logits.end(), 0.0f);
      for (size_t j = 0; j < cand.size(); j++) logits[cand[j]] = work[j];
    } else {
      cand.reserve(n);
      for (int i = 0; i < n; i++)
        if (logits[i] > 0.0f) cand.push_back(i);
    }
    if (top_p > 0.0f && top_p < 1.0f && !cand.empty()) {
      // largest threshold t with mass(p >= t) >= top_p * total
      double total = 0;
      for (int i : cand) total += logits[i];
      double lo = 0, hi = 1.0;  // probs are scaled so max == 1
      for (int it = 0; it < 50; it++) {
        double mid = 0.5 * (lo + hi), m = 0;
        for (int i : cand)
          if (logits[i] >= (float)mid) m += logits[i];
        if (m >= top_p * total) lo = mid; else hi = mid;
      }
      std::vector<int> keep;
      keep.reserve(cand.size());
      for (int i : cand)
        if (logits[i] >= (float)lo)
          keep.push_back(i);
        else
          logits[i] = 0.0f;
      cand.swap(keep);
    }
    if (min_p > 0.0f && !cand.empty()) {
      float pmax = 0;
      for (int i : cand) pmax = fmaxf(pmax, logits[i]);
      float thr = min_p * pmax;
      std::vector<int> keep;
      keep.reserve(cand.size());
      for (int i : cand)
        if (logits[i] >= thr) keep.push_back(i);
      if (!keep.empty()) {
        for (int i : cand)
          if (logits[i] < thr) logits[i] = 0.0f;
        cand.swap(keep);  // never empty the candidate set
      }
    }
    double z = 0;
    for (int i : cand) z += logits[i];
    for (int i : cand) logits[i] = (float)(logits[i] / z);
  }

  // Draw from a distribution produced by prepare(). Candidate order is kept
  // stable with the serial sampler so existing seeded behavior is unchanged.
  int draw_prepared(const std::vector<float>& probs, float* lp,
                    bool add_hist) {
    double z = 0;
    for (int i : cand) z += probs[i];
    double u = uniform() * z;
    int sel = cand.back();
    for (int i : cand) {
      u -= probs[i];
      if (u <= 0) { sel = i; break; }
    }
    if (lp) *lp = (float)(log((double)probs[sel]) - log(z));
    if (add_hist) remember(sel);
    return sel;
  }

  int sample(std::vector<float>& logits, float* lp) {
    prepare(logits);
    return draw_prepared(logits, lp, true);
  }

  // Standard speculative correction distribution: normalize max(p-q, 0).
  // The reported logprob remains the target probability p(token), matching
  // serial LOGPROBS semantics rather than the residual draw probability.
  int sample_residual(const std::vector<float>& p,
                      const std::vector<float>& q, float* lp) {
    const int n = (int)p.size();
    work.assign(n, 0.0f);
    cand.clear();
    double z = 0.0;
    for (int i = 0; i < n; i++) {
      float d = fmaxf(0.0f, p[i] - q[i]);
      if (d > 0.0f) {
        work[i] = d;
        cand.push_back(i);
        z += d;
      }
    }
    if (cand.empty() || z <= 0.0) {
      for (int i = 0; i < n; i++)
        if (p[i] > 0.0f) {
          work[i] = p[i];
          cand.push_back(i);
          z += p[i];
        }
    }
    double u = uniform() * z;
    int sel = cand.back();
    for (int i : cand) {
      u -= work[i];
      if (u <= 0.0) {
        sel = i;
        break;
      }
    }
    if (lp) *lp = p[sel] > 0.0f ? logf(p[sel]) : -INFINITY;
    return sel;
  }

  // ---- sparse candidates (GPU top-k path) ----
  // SDist is the sparse mirror of a prepare()d row: the candidate ids in draw
  // scan order and their normalized probabilities. prepare_sparse() replays
  // the post-top_k filter chain (temperature -> top-p -> min-p -> normalize)
  // on exactly the top-k corrected logits supplied by the GPU.
  struct SDist {
    std::vector<int> ids;
    std::vector<float> pr;
  };

  void prepare_sparse(const int* ids, const float* vals, int k, SDist& out) {
    float lmax = -INFINITY;
    for (int j = 0; j < k; j++) lmax = fmaxf(lmax, vals[j]);
    cand.assign(ids, ids + k);
    work.resize(k);
    for (int j = 0; j < k; j++) work[j] = expf((vals[j] - lmax) / temp);
    if (top_p > 0.0f && top_p < 1.0f && !cand.empty()) {
      double total = 0;
      for (float w : work) total += w;
      double lo = 0, hi = 1.0;  // probs are scaled so max == 1
      for (int it = 0; it < 50; it++) {
        double mid = 0.5 * (lo + hi), m = 0;
        for (float w : work)
          if (w >= (float)mid) m += w;
        if (m >= top_p * total) lo = mid; else hi = mid;
      }
      std::vector<int> keep;
      std::vector<float> kwork;
      keep.reserve(cand.size());
      kwork.reserve(cand.size());
      for (size_t j = 0; j < cand.size(); j++)
        if (work[j] >= (float)lo) {
          keep.push_back(cand[j]);
          kwork.push_back(work[j]);
        }
      cand.swap(keep);
      work.swap(kwork);
    }
    if (min_p > 0.0f && !cand.empty()) {
      float pmax = 0;
      for (float w : work) pmax = fmaxf(pmax, w);
      float thr = min_p * pmax;
      std::vector<int> keep;
      std::vector<float> kwork;
      for (size_t j = 0; j < cand.size(); j++)
        if (work[j] >= thr) {
          keep.push_back(cand[j]);
          kwork.push_back(work[j]);
        }
      if (!keep.empty()) {
        cand.swap(keep);  // never empty the candidate set
        work.swap(kwork);
      }
    }
    double z = 0;
    for (float w : work) z += w;
    out.ids = cand;
    out.pr.resize(cand.size());
    for (size_t j = 0; j < cand.size(); j++) out.pr[j] = (float)(work[j] / z);
  }

  static float sdist_prob(const SDist& d, int id) {
    for (size_t j = 0; j < d.ids.size(); j++)
      if (d.ids[j] == id) return d.pr[j];
    return 0.0f;
  }

  int draw_sparse(const SDist& d, float* lp, bool add_hist) {
    double z = 0;
    for (float p : d.pr) z += p;
    double u = uniform() * z;
    size_t si = d.ids.size() - 1;
    for (size_t j = 0; j < d.ids.size(); j++) {
      u -= d.pr[j];
      if (u <= 0) { si = j; break; }
    }
    const int sel = d.ids[si];
    if (lp) *lp = (float)(log((double)d.pr[si]) - log(z));
    if (add_hist) remember(sel);
    return sel;
  }

  // Sparse residual: max(p-q, 0) renormalized. p's support suffices (d > 0
  // implies p > 0); ids are scanned in ascending order like the dense loop.
  int sample_residual_sparse(const SDist& p, const SDist& q, float* lp) {
    cand = p.ids;
    std::sort(cand.begin(), cand.end());
    work.clear();
    std::vector<int> sel_ids;
    std::vector<float> sel_w;
    double z = 0.0;
    for (int id : cand) {
      const float pv = sdist_prob(p, id);
      const float d = fmaxf(0.0f, pv - sdist_prob(q, id));
      if (d > 0.0f) {
        sel_ids.push_back(id);
        sel_w.push_back(d);
        z += d;
      }
    }
    if (sel_ids.empty() || z <= 0.0) {
      sel_ids.clear();
      sel_w.clear();
      z = 0.0;
      for (int id : cand) {
        const float pv = sdist_prob(p, id);
        if (pv > 0.0f) {
          sel_ids.push_back(id);
          sel_w.push_back(pv);
          z += pv;
        }
      }
    }
    double u = uniform() * z;
    int sel = sel_ids.back();
    for (size_t j = 0; j < sel_ids.size(); j++) {
      u -= sel_w[j];
      if (u <= 0.0) {
        sel = sel_ids[j];
        break;
      }
    }
    if (lp) {
      const float pv = sdist_prob(p, sel);
      *lp = pv > 0.0f ? logf(pv) : -INFINITY;
    }
    return sel;
  }
};
