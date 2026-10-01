  // R13 experimental branch projection. Included inside GpuModel.
  // This is NOT a 4P GEMM: all four Lt calls retain N=K=2560, columns=P.
  static bool mtp_branch_strided_enabled() {
    static const bool enabled = [] {
      const char* value = getenv("GDEC_MTP_BRANCH_STRIDED");
      return value && !strcmp(value, "1");
    }();
    return enabled;
  }
  struct MtpBranchLtEntry {
    LtEntry lt;
    hipblasLtMatmulAlgo_t original{};
    bool supported = false;
    ~MtpBranchLtEntry() {
      if (lt.opd) hipblasLtMatmulDescDestroy(lt.opd);
      if (lt.Al) hipblasLtMatrixLayoutDestroy(lt.Al);
      if (lt.Bl) hipblasLtMatrixLayoutDestroy(lt.Bl);
      if (lt.Cl) hipblasLtMatrixLayoutDestroy(lt.Cl);
      if (lt.Dl) hipblasLtMatrixLayoutDestroy(lt.Dl);
    }
    MtpBranchLtEntry() = default;
    MtpBranchLtEntry(const MtpBranchLtEntry&) = delete;
    MtpBranchLtEntry& operator=(const MtpBranchLtEntry&) = delete;
  };
  // Separate descriptor namespace: never insert strided layouts in lt_algos.
  std::unordered_map<int, std::unique_ptr<MtpBranchLtEntry>> mtp_branch_lt_;

  // No scratch writes or conversion-cache changes until all capability and
  // Lt support checks pass. R15 may also use this before producing BF16 input.
  MtpBranchLtEntry* mtp_branch_strided_prepare(const WView& w, int P,
                          bool normal_prompt, uint16_t* scratch,
                          size_t scratch_elems) {
    constexpr size_t D = 2560, ROW = 4 * D;
    // Capability is passed only by normal prefill. A repair/re-ingest caller
    // cannot enable this by leaving prog_total nonzero. No shared small ws.
    if (!mtp_branch_strided_enabled() || !normal_prompt || prog_total <= 0 ||
        mtp_hagg_mode != 1 || P <= 16 || P > maxbatch_cap ||
        nslots != 1 || bound_ != 0 || ws_lent_ ||
        g_borrow_light.load(std::memory_order_acquire) ||
        borrow_fn || borrow_query_fn || borrow_skip_fn ||
        spec_snap || spec_fp32_active || aligned_spec_active() ||
        ngram_verify_active || mrope_active_ ||
        g_cfg.d != D || g_cfg.branches != 4 ||
        w.dtype != 7 || w.rows != D || w.cols != D || !w.data ||
        getenv("GDEC_GEMM_RB") || !lth || !d_ltws || !d_wbf16 ||
        !d_mRhatb || !d_mRb || !scratch || scratch != d_xbf16 ||
        scratch_elems < (size_t)P * ROW ||
        (((uintptr_t)d_mRhatb | (uintptr_t)d_mRb | (uintptr_t)scratch) & 15))
      return nullptr;
    // For N=K=2560, normal P>16 cannot take MR, aligned or the WMMA
    // shape whitelist. The existing GEMM therefore uses this exact Lt key.
    const uint64_t key = ((uint64_t)D << 40) ^ ((uint64_t)D << 20) ^ (uint64_t)P;
    const auto baseline = lt_algos.find(key);
    if (baseline == lt_algos.end()) return nullptr; // no tuning/guessing here
    hipStreamCaptureStatus capture;
    if (hipStreamIsCapturing(g_str, &capture) != hipSuccess ||
        capture != hipStreamCaptureStatusNone) return nullptr;

    const auto& selected = baseline->second.algo;
    auto selected_copy = selected;
    // Empirically validated by R1 on the frozen TheRock gfx1151 runtime only.
    // Same mathematical shape/index is NOT itself a bitwise-equivalence proof.
    // A different selected solution keeps the original route even if supported.
    if (hipblaslt_ext::getIndexFromAlgo(selected_copy) != 1251) return nullptr;
    auto found = mtp_branch_lt_.find(P);
    if (found != mtp_branch_lt_.end() &&
        (memcmp(found->second->original.data, selected.data, sizeof selected.data) ||
         found->second->original.max_workspace_bytes != selected.max_workspace_bytes)) {
      // A future baseline cache replacement must not leave a stale candidate.
      mtp_branch_lt_.erase(found);
      found = mtp_branch_lt_.end();
    }
    if (found == mtp_branch_lt_.end()) {
      auto owned = std::make_unique<MtpBranchLtEntry>();
      auto& e = owned->lt;
      owned->original = selected;
      e.algo = selected; // support query mutates only this private copy
      hipblasOperation_t opT = HIPBLAS_OP_T, opN = HIPBLAS_OP_N;
      bool ok = hipblasLtMatmulDescCreate(&e.opd, HIPBLAS_COMPUTE_32F, HIP_R_32F) == HIPBLAS_STATUS_SUCCESS;
      ok = ok && hipblasLtMatmulDescSetAttribute(e.opd, HIPBLASLT_MATMUL_DESC_TRANSA, &opT, sizeof opT) == HIPBLAS_STATUS_SUCCESS;
      ok = ok && hipblasLtMatmulDescSetAttribute(e.opd, HIPBLASLT_MATMUL_DESC_TRANSB, &opN, sizeof opN) == HIPBLAS_STATUS_SUCCESS;
      ok = ok && hipblasLtMatrixLayoutCreate(&e.Al, HIP_R_16BF, D, D, D) == HIPBLAS_STATUS_SUCCESS;
      ok = ok && hipblasLtMatrixLayoutCreate(&e.Bl, HIP_R_16BF, D, P, ROW) == HIPBLAS_STATUS_SUCCESS;
      ok = ok && hipblasLtMatrixLayoutCreate(&e.Cl, HIP_R_32F, D, P, ROW) == HIPBLAS_STATUS_SUCCESS;
      ok = ok && hipblasLtMatrixLayoutCreate(&e.Dl, HIP_R_32F, D, P, ROW) == HIPBLAS_STATUS_SUCCESS;
      float one = 1.f, zero = 0.f;
      size_t workspace = 0;
      int original_index = hipblaslt_ext::getIndexFromAlgo(owned->original);
      if (ok) ok = hipblaslt_ext::matmulIsAlgoSupported(
          lth, e.opd, &one, e.Al, e.Bl, &zero, e.Cl, e.Dl, e.algo, workspace) == HIPBLAS_STATUS_SUCCESS;
      const int checked_index = hipblaslt_ext::getIndexFromAlgo(e.algo);
      owned->supported = ok && workspace <= lt_ws_bytes &&
          original_index >= 0 && checked_index == original_index;
      fprintf(stderr, "[mtp-branch-strided] P=%d baseline_cached_index=%d checked_index=%d "
                      "N=2560 K=2560 ldb=10240 ldo=10240 workspace=%zu supported=%d\n",
              P, original_index, checked_index, workspace, (int)owned->supported);
      found = mtp_branch_lt_.emplace(P, std::move(owned)).first;
    }
    return found->second->supported ? found->second.get() : nullptr;
  }

  static bool mtp_tap_norm_dual_enabled() {
    static const bool enabled = [] {
      const char* value = getenv("GDEC_MTP_TAP_NORM_DUAL");
      return value && !strcmp(value, "1");
    }();
    return enabled;
  }

  bool mtp_branch_strided(const WView& w, int P, bool normal_prompt,
                          uint16_t* scratch, size_t scratch_elems,
                          const uint16_t* norm_bf16_ready = nullptr) {
    constexpr size_t D = 2560, ROW = 4 * D;
    // Readiness is explicit and local to this call, never a forged xf16 hit.
    if (norm_bf16_ready && norm_bf16_ready != scratch) return false;
    auto* entry = mtp_branch_strided_prepare(w, P, normal_prompt, scratch,
                                            scratch_elems);
    if (!entry) return false; // the caller still has complete FP32 output
    auto pt0 = std::chrono::steady_clock::now();
    // No GPU writes and no conversion-cache mutation occur before eligibility
    // and support pass. ROW*P BF16 elements fit the caller's explicit scratch;
    // d_xbf16b only has 6144 elements/row and is deliberately not used.
    xf16_src = nullptr;
    const uint16_t* wp = gemm_wbf16(w); // same q8g64 conversion, once not four
    const size_t elements = (size_t)P * ROW;
    if (!norm_bf16_ready)
      k_f32_to_bf16_v4<<<(unsigned)((elements / 4 + 255) / 256), 256, 0, g_str>>>(
          d_mRhatb, scratch, D, 4 * P, (int)D);
    const auto& e = entry->lt;
    float one = 1.f, zero = 0.f;
    for (int b = 0; b < 4; ++b) {
      float* output = d_mRb + (size_t)b * D;
      hipblasStatus_t status = hipblasLtMatmul(
          lth, e.opd, &one, wp, e.Al, scratch + (size_t)b * D, e.Bl,
          &zero, output, e.Cl, output, e.Dl, &e.algo,
          d_ltws, lt_ws_bytes, g_str);
      if (status != HIPBLAS_STATUS_SUCCESS) {
        fprintf(stderr, "mtp branch strided matmul failed status=%d P=%d branch=%d\n",
                (int)status, P, b);
        exit(1); // same fatal policy as gemm, never silently keep partial rows
      }
    }
    if (prof) p_gemm += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - pt0).count();
    return true; // caller still performs the original separate broadcast add
  }
