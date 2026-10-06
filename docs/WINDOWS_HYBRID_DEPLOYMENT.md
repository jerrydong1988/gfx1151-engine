# Windows hybrid mainline deployment

## v0.0.6 selective integration — 2026-10-06

状态：已完成选择性融合、验收和部署；应用目录原生启动器运行正常，15 项现场检查通过。

本轮以 **10 月 4 日实际部署的 `27e3d88` / 引擎 `31126a5`** 为基线。
原工作分支 `c07f603` 尚未同步这次部署；已先快进同步，保留此前整合的 11 个上游提交。
实际新增评估范围为 `aad31fc..9680de2` 的 19 个提交，没有整包合并 v0.0.6。

取舍：

- 采用 `9680de2` 的普通文本 pad 标记修复，以及 `de0d650` 的图片嵌入扩容保留修复；
  图片上限仍为 16。上游新增测试中的跨临时容器迭代器错误也已修正。
- 采用 `53c72f8` 的 ThinkSplitter 部分，及时发出不属于结束标记的首段推理文字。
  CPU 测试证明首个 `We` 可以立即发出；没有把上游的 65–100 ms 描述当成本机实测收益。
- 采用 `d8d6b21` 的 wave32 路由思路，先修正同分选择顺序。原 strict-greater 归约树
  按索引的位逆序处理同分，并非按索引升序。原上游实现在专项中有 35 个用例不一致；
  修正后的 42 个路由用例和 3 个图片缓冲区用例全部通过。
- `2f49635` 的内存布局调整做了独立实测，没有显示出足够收益，已从最终代码删除。
  `e17fc1d` 所针对的新 dense-LUT 临时分配未引入，不需要同步该修补。
- 保留既有 ordered HT、MTP KV-only、v2 multi-tile 实现，避免重复计算其收益。
  未引入新的 BF16/FP16 默认策略、QSA union 默认开启、异步检查点及 GDN 快照切分。
  前两类会改变数值路径；后两类与本地状态恢复逻辑交织，收益证据不足以抵消整合成本。
  Linux PLE、多种未过上游性能门槛的实验、w4b 专属优化不进入当前 Windows v2 部署。

### 数值与性能

同一组既有 HGN v2 / n-gram 权重，未重新量化；现用 hybrid 的八项保精度设置保持不变。
参考是此前部署的量化模型及计算路径，不是 BF16 原模型，也不是任意输入的等价证明。
短混合语料 2,040 个位置、8K 自然文档 4,095 个位置，覆盖全词表的原始 FP32 logits
指纹均一致，共 6,135 个评分位置。直接检查指纹，不依赖允许漂移的上游 KLD 门槛脚本。

Windows / gfx1151，262144 容量、双槽、524288 KV token 池、8192 Prefill 分块、gamma 2；
测速关闭 RAM/SSD 前缀缓存，全部请求缓存命中为 0。A/B/B/A，每档串行/MTP 各两次，
固定输出 128 token，共 40 个请求；全部对应输出一致，20 组串行/MTP 配对也一致。
编译与 GPU 专项测试均在正式测速前结束。下表为两次测量中位数，正值表示耗时减少。

| 输入 token | 模式 | 旧版 TTFT 秒 | 候选 TTFT 秒 | TTFT 减少 | 完整请求耗时减少 |
|---:|:---|---:|---:|---:|---:|
| 256 | 串行 | 0.451 | 0.449 | +0.42% | -0.26% |
| 256 | MTP | 0.455 | 0.459 | -0.81% | -0.96% |
| 2,052 | 串行 | 1.934 | 1.930 | +0.19% | -0.29% |
| 2,052 | MTP | 1.935 | 1.933 | +0.09% | -0.94% |
| 8,192 | 串行 | 5.934 | 5.909 | +0.42% | +1.10% |
| 8,192 | MTP | 5.931 | 5.885 | +0.78% | +0.05% |
| 32,768 | 串行 | 22.321 | 22.186 | +0.61% | -0.10% |
| 32,768 | MTP | 22.415 | 22.248 | +0.74% | +0.67% |
| 131,072 | 串行 | 91.945 | 91.247 | +0.76% | +0.65% |
| 131,072 | MTP | 93.749 | 93.051 | +0.74% | +0.77% |

收益很小：8K–128K 的 MTP 首字中位数减少约 0.74%–0.78%，完整请求并非全部更快。
不能包装成明显或普遍的任务加速。候选有一次模型重载耗时 165.5 秒，其余正式轮次约
21.6–22.6 秒；启动耗时另行记录，不混入请求计时。加载波动根因未在本轮确定。

421 项 API/tokenizer/流式分片边界检查、6 项原生配置检查，以及工具参数错误、
双槽断连、取消超时恢复测试通过。运行库 262 个文件与既有部署逐一校验一致。

R23-d 台账与代码任务的旧版、候选均首次通过。台账各完成 17 轮模型请求，
耗时 341.25 / 343.81 秒；两边都出现两次 SQL 工具错误，均恢复后交付正确台账。
独立核验覆盖 1,002 条输入记录。代码任务各完成 7 轮请求、5,507 个输出 token，
修复后 545/545 个隐藏用例通过；首次公开测试失败是任务要求的缺陷复现。
代码执行范围为固定函数的有界 AST 解释器，不是完整仓库或 Shell 开发任务。
移除随机生成的工具调用 ID 后，两类任务的完整消息记录均一致。

代码任务初次候选 148.74 秒，旧版 129.06 秒，候选慢 15.25%，因此追加了预先固定顺序的
三组复测：候选 130.49 秒、旧版 186.76 秒、候选关闭新路由 130.38 秒。
追加的三组也全部通过 545/545 隐藏用例，调用内容和生成内容一致。
原减速结果保留，不能通过挑选轮次声称任务提速；复测没有支持新路由导致持续回归的结论。
采样见到其他后台 CPU 活动，但没有确认波动根因，也没有控制温度、功耗或整个系统的空闲状态。
完整工具任务用于检验正确性，耗时仅作观察；不用于宣称稳定端到端加速。

最终保留修正后的 wave32 路由：长输入交错测量有小幅收益，数值路径保持一致，
新路由开/关的代码任务复测近乎相同；同时保留一个开关即可恢复旧路由。
主要升级价值仍是文本标记、多图扩容和流式边界修复，不以牺牲精度换取性能。
本轮 19 个上游提交逐项取舍、二进制哈希、完整测量与测试结果见
[机器可读验收记录](benchmarks/v006-integration-20261006.json)。

### 当前部署与回退

应用目录为 `C:\Users\Jerry\llama.cpp\gfx1151-engine`，源码已同步回
`C:\Users\Jerry\Projects\gfx1151-engine` 的 `codex/hybrid-mainline`。
本轮仅在本地提交，未推送或创建远端 PR。引擎从 `dd6cb8f` 构建，API 从 `4e00e91` 构建；
之后的提交仅整理配置测试名称、验收记录与部署文档，不改变对应生产代码。

| 已部署文件 | SHA-256 |
|---|---|
| `build/gdec-win.exe` | `b64f434f569be3a9ac5a44b270616b8f44ef815880984d3ae070da0d9a0beb08` |
| `build/gdec-api-win.exe` | `e6266c88fc829c9ae0c1bc712ed260de64693f4ef04523418de14e08b806971f` |

原生配置检查证明既有全部有效参数保持一致，仅增加显式 `GDEC_ROUTER_W32=1`。
运行库 262 个文件哈希再次核验；加载的 6 个 ROCm 核心模块均来自应用目录。
API 继续使用 `http://127.0.0.1:18861/v1`，模型 ID 为 `qwen3.8-flash-next`。

现场 15 项检查覆盖：双槽/256K 健康状态、模型列表、普通对话、Responses SSE、
流式工具调用与结果回传、单图识别、真实双槽并发、三个 API 路径的文本 pad 字面量、
多图扩容、17 图超过原上限的拒绝、真实请求取消后同连接复用，以及结束后空闲健康状态。
扩容用三张 1120×1120 的红/绿/蓝图片，合计 4800 行嵌入超过初始 4096 行缓冲；
同时混入普通 pad 文本，返回三种颜色顺序正确。双槽并发实际观察到 2 个在途请求，
输出与顺序执行一致。本轮未重复进行两路同时接近 256K 的满容量压力测试。
完整现场结果保存在应用目录 `docs/DEPLOYMENT_VALIDATION_20261006.json`，
进程身份、有效参数、文件哈希及验证时间见应用目录 `deployment.json`。

旧版二进制、配置、部署元数据、使用说明和被替换的部署文档保存在
`C:\Users\Jerry\Projects\gfx1151-engine\logs\v006\previous-installation`，附 `SHA256.json`。
回退时先从托盘退出服务，再按相对路径恢复这些文件并重新启动。
只需对照新旧路由时，可在 `service.conf` 将 `GDEC_ROUTER_W32` 设为 `0` 后重启。

原始证据与受控驱动保留在本机 `logs/v006/`，其中包含被拒绝的布局实验及上游路由失败记录。
`GDEC_ROUTER_W32=0` 可恢复旧路由；其余数值开关沿用
[既有精度配置](UPSTREAM_PRECISION_PRESERVE_20261004.env.json)。
Windows 专项复现：在正常 TheRock/Visual Studio 构建环境设置 `TOK_DIR` 后运行
`bash build_win.sh v006-test`，不下载权重。

## Previous deployment notes — R24/R25 history

以下是历史记录，旧哈希、单槽配置与验证范围不代表 2026-10-06 部署状态。

Selected on 2026-10-02, diagnostics updated on 2026-10-03: HGN v2 hybrid arithmetic,
aligned MTP, gamma 2.
The native launcher reads explicit `GDEC_*` configuration entries from
`service.conf`, in addition to the existing high-level options. It applies them
after built-in flags; the existing context, KV, gamma and arithmetic configuration
keys retain their final authority. Existing parent-environment precedence is
preserved. `--check` prints the effective child `GDEC_*` environment without
starting children. Normal startup records the same settings in its launcher log.

For a Windows installation, copy `tools/windows_hybrid.conf.example` to the
installation's `service.conf` and set `MODEL_DIR` to the existing weight directory.
Use the matching `start_win.exe`, `build/gdec-win.exe`, `build/gdec-api-win.exe`
and validated ROCm runtime files. Start only `start_win.exe`; stop the service
with its tray menu. The source repository retains alternative computation paths
for research and numerical regression, but they need no separately installed
services or shortcuts.

The selected installation uses:

- `qwen38-flash-next-v2.hgn` plus `qwen38-flash-next-ngram.hgn`.
- No legacy w4b overlay or external MTP file; v2 provides its own draft head.
- Existing `qwen38-flash-next-vision.hgn` and tokenizer, unchanged.
- Context capacity 262144, one slot, BF16 paged KV, Prefill chunk 8192.
- MTP gamma 2, aligned verification and serial accumulator order.
- Upstream dense backend with ordered HT reduction; upstream expert Prefill
  for large batches, retaining the fork's short verification path.
- RAM/SSD checkpoints disabled; one-request image limit 16.
- Loopback engine/API ports 18860/18861; model ID `qwen3.8-flash-next`.
- Request/chunk host timing enabled; experimental PLE page I/O and double-read
  verification disabled. Numerical settings and weights are unchanged by R24.

The R24 inference engine SHA-256 is
`fe91c4f38050e4b274c20350c2eeb0ff80049ab877796f3d4b25e2f107d9b577`;
the API remains
`a4b38906fe189454670929271a37af5f5d0dfb884148af89b77433e5e60f7da0`.
The API source remains `2e68d6845d38bc5e22d8dbdc1877509c310ee427`.
The engine includes the R24 diagnostics and default-off experimental reader,
built on the `93b801a` source baseline. The installed manifest records the exact
local source revision. The native launcher is unchanged from the prior deployment.

Prior deployment validation: five native runtime-configuration checks, 32 launcher/host
checks, real API smoke (Chat, Completions, Responses, streaming, stop and sampled
logprobs), a streamed tool/result round trip, red-image recognition and rejection
of 17 images. No 256K performance or all-input numerical-equivalence claim is
made by these deployment checks. R24 separately checks the effective configuration,
engine identity, an actual tool/result/final-answer exchange, and red-image
recognition; it does not claim the entire earlier deployment suite was repeated.

See [R24 conditions and limits](RESEARCH_WORKLOAD_R24.md). The optional source-ID
report helper is client-side and is not automatically integrated into Octop.

Output budgets remain client-controlled. A fixed 8192-token output allowance was
preferable in the bounded R23 tasks; the server does not silently cap or override
Octop requests. R23's schema-specific independent delivery checker is **not**
installed into Octop. Hybrid remains numerically different from the exact
reference, and a successful API/tool smoke does not prove business-data accuracy.
