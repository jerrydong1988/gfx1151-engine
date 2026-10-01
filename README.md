# gfx1151-engine：Windows HGN v2 推理优化分支

![Strix Halo — Qwen3.8-Flash-Next](media/strix_banner_21x9_v2.png)

*English: [README_EN.md](README_EN.md)*

这是 [gfx1151-engine 上游实验项目](https://github.com/IIIIIllllIIIIIlllll/gfx1151-engine)
的研究 Fork，当前分支为 `codex/halogen-v2-support`。面向 **Windows 11 /
Ryzen AI Max+ 395 / Radeon 8060S（gfx1151）/ 128 GiB 统一内存**，重点是
Halogen Flash-Next v2 HGN 格式支持、专家计算与 Prefill 优化，以及串行/MTP 的数值对齐。
目标模型仍是 Qwen3.8-Flash-Next（`qwen4_exp`）及兼容结构，不是通用模型推理框架。

**旧版 w4b / Q4CP 路径仍保留，v2 是新增支持，并非替换旧格式。**
不同格式的验证范围见下表；格式兼容不表示所有新内核都适用于旧权重，也不保证新旧版本逐位输出相同。

截至 **2026-10-01**：已提交的累计引擎实现为 R1–R13，R14 是该引擎的工具任务测试；
R15 双输出归一化仍是**未接入引擎的独立候选**。这些编号是本分支研究阶段，不是上游发布版本。
当前结果不构成生产可用、全场景无损或全面超过上游的承诺。

## 我们相对上游做了什么

| 方向 | 本分支增加或改进的内容 | 当前边界 |
|---|---|---|
| HGN v2 加载 | 支持已观察到的 HT / rotated grouped Q4 / Q6 布局，Windows 加载器识别独立 PLE sidecar，校验布局和缺失张量 | 不是所有 HGN 变体；旧 Q4CP 与 GGUF 分派保留 |
| v2 专家计算 | 保持 FP32 运算顺序的 `exact` 批量/小批次内核；可选分组缩放矩阵核、激活高低位拆分、native HT 投影 | `grouped` 等路径会改变浮点计算，不能称为逐位等价 |
| MTP 数值与状态 | 可选 `aligned` 策略，对齐残差、GDN、QSA 和短验证计算；修复已复现的多行累加差异，补充槽位/检查点保护与采样器修正 | 有限样例通过不等于随机采样、缓存、多槽位全部等价 |
| 普通 Prefill | 延后专家缩放、重排 Q4 读取、PLE 预读、gate/up 与激活融合、归约与 Hadamard 融合 | R10/R12 有同权重、同二进制的开关对照 |
| MTP 前置处理 | 可选 KV-only ingest、融合读取、跳过未使用验证头、tap/RMSNorm 融合及四路原尺寸投影 | 按形状、槽位和运行库条件启用，保留回退；并非总能缩短完整请求 |
| 研究记录 | 格式/内核检查、数值对照、长输入计时、真实工具循环与失败案例 | 未把局部微核速度或“能加载”当作完整任务质量证明 |

这项工作建立在上游已有的 Windows 移植、OpenAI 风格 API、Q4CP/GGUF、MTP、
PLE、视觉和缓存实现之上；这些不是本分支从零新增的功能。
实现与开关详情见 [累计研究记录](docs/WINDOWS_V2_RESEARCH_CHECKPOINT.md)。

## 权重兼容性：旧格式没有移除

| 权重路径 | 当前代码 | 本分支实测范围 |
|---|---|---|
| `qwen38-flash-next-w4b.hgn` + 旧版 overlay / 外部 MTP | 旧 HGN、Q4CP 解码及串行/批量专家分派保留；仓库 `service.conf` 仍以此为默认 | 早期 v2 分支同一 Windows EXE 已实际加载和生成；**最新 R13 累计版本未完整重跑旧权重回归** |
| 其他旧格式 HQ / overlay 组合 | 继承旧容器与 overlay 路径，仍须满足原张量布局与模型结构 | 未逐一验证全部 HQ 文件/组合，不能按文件后缀承诺兼容 |
| `qwen38-flash-next-v2.hgn` + `qwen38-flash-next-ngram.hgn` | 新增 storage 16 / 23 / 24 的已观察布局；主文件含内置 MTP | 当前 Windows 研究主线，有内核、整模型、长输入及工具循环记录 |
| 同结构 GGUF | 继承 GGUF 加载和专家计算分支，Linux 入口仍在 | 本轮 Windows v2 研究未做 GGUF 端到端回归，不能据此声称 Windows GGUF 全面可用 |

代码依据：[HGN 解码](src/hgn.h)、[模型分派](src/gpu/parts/40_model.inc)、
[主程序加载](src/gpu/parts/52_main.inc)。
新 v2 专家核按张量类型选择，旧 Q4CP 不会自动获得下面的 v2 专属提速。
公共采样器与 MTP 逻辑也有修改，因此“保留兼容”不是“旧路径所有行为完全不变”。

v2 的 `NGRAM_FILE` 是 **PLE 查表权重**，不是开启 ngram 推测解码的开关。
测试 v2 时应清空旧 `OVERLAY_FILE`，避免覆盖新权重；清空 `MTP_FILE` 只是不加载
外部草稿，**不会关闭主文件内置 MTP**。参见 [格式和隔离配置](docs/HGN_V2.md)。

## 实测性能与比较口径

测试设备为上述 Windows 11 / 395 / 8060S / 128 GiB 机器。以下是各阶段的历史结果，
不是对最新提交所有开关组合重新做出的统一排行。未修改或重新量化模型文件；
这并不代表所有计算路径的数值和模型质量相同。

### 早期专家优化与上游旧权重组合

三次重复的中位数；上下文容量 16384，单槽位、BF16 KV、greedy、关闭思考/视觉，
无复用 prompt token。单位为阶段 token/s。

| 测项 | 较早 v2 exact | v2 grouped + native HT | 上游实验版本：旧 w4b + quality overlay + 外部 MTP |
|---|---:|---:|---:|
| Prefill，8138 输入 token | 358.45 | 1164.40 | 1397.20 |
| 串行 Decode，128 输出 token | 15.37 | 26.57 | 32.75 |
| MTP Decode，128 输出 token | 15.60 | 27.38 | 32.69 |

`grouped` 的 Prefill 达到较早 `exact` 的 **3.25 倍**、该上游配置的 **83.3%**。
上游对照为 `78a41cc`，较早 exact 为 `1825905`；两边权重、MTP 和执行路径不同，
所以这不是“只改引擎”的对照，也不是最新 R13 对上游的结论。
额外舍入更大的 `grouped-f16` 曾达到 1351.85 token/s（该上游配置的 96.8%），
但不是等价加速或默认设置。[完整条件与数值检查](docs/HGN_V2_GROUPED_OPTIMIZATION.md)

### 后续同一 v2 配置下的受控优化

同一二进制开关对照，固定输入、串行 greedy、128 输出 token、单槽位、
BF16 KV、262144 上下文容量、8192 分块，实际 cached token 为 0。
后续实验沿用 grouped/native HT 等研究配置，并非默认 exact 配置。
每个长度三对，保留慢样本。表中是**逐对耗时降幅的中位数**，不是吞吐增幅。

| 阶段 / 输入 token | Prefill 耗时减少 | 完整请求耗时减少 |
|---|---:|---:|
| R10 / 32768 | 5.34% | 4.69% |
| R10 / 131072 | 6.65% | 6.37% |
| R10 / 260000 | 6.25% | 6.18% |
| R12c / 32768 | 2.264% | 1.861% |
| R12c / 131072 | 2.387% | 2.269% |

这些 Prefill 对照均为 3/3 更快；Decode 并非每次更快。
R10 的 260K 完整请求中位耗时为 **247.659 → 232.758 秒**。
请求时间不含进程启动、模型加载、tokenization 和清理。不同阶段收益不能相加，
也不能直接乘到前一张历史表上。[配置、方法与边界](docs/WINDOWS_V2_RESEARCH_CHECKPOINT.md)

### MTP：Decode 加速不等于任务必然加速

R13 新增两项前置处理开关，在 131072 / 260000 输入下相对关闭它们的 MTP，
完整请求逐对耗时仅减少 0.1667% / 0.1734%；抖动和自动 kernel 选择限制了归因。
**260000 输入、128 输出时，开启它们的 MTP 仍比串行慢 0.2585%，三对均更慢。**
因此尚未实现长输入短输出下稳定的整体收益。

R14 在四个隔离实例中依次运行串行/MTP/MTP/串行，每个实例执行一个办公聚合任务和
一个代码修复任务，共 64 次模型请求。工具实际执行，不是只检查生成的 JSON。

| 场景 | 串行 Decode | MTP Decode | 完成情况 |
|---|---:|---:|---|
| 代码修复 | 22.28 token/s | 40.72 token/s | 两种模式各 2/2 成功；任务平均 96.01 → 51.91 秒 |
| 办公聚合 | 23.05 token/s | 40.55 token/s | 两种模式各 0/2 成功，均在工具协议处失败 |

代码任务平均耗时减少 45.9%，但首次串行 Prefill 波动较大，不能据此声称 MTP 稳定提升
Prefill。办公任务生成了未知工具 `query_ledser`（应为 `query_ledger`）和无效 SQL；
API 又将解析失败映射为 `length`，实际并未耗尽输出预算。这仍是未解决问题。

上述是每种固定场景各两次测试，不是完整 Octop UI 测试或广泛 Agent 排名；
实际最长 prompt 为办公 45496、代码 4114 token，不能称为 256K Agent 测试。
[研究记录与计时定义](docs/WINDOWS_V2_RESEARCH_CHECKPOINT.md)

### 最新候选：R15 归一化双输出（尚未接入）

将 R13 的 norm → FP32 → BF16 转换改为同一次 norm 同时写两种输出，
保持原 FP32 运算顺序与舍入。已测 88 个有限输入用例的完整 FP32/BF16 输出逐位一致。
在固定应用目录 HIP 运行库的热微测试中，P=8192、4 组的局部耗时为
**5.470750 → 3.647000 ms（减少 33.3%）**。

这仅是归一化与转换步骤，不含后续投影、完整 Prefill、Decode 或任务时间。
**候选代码尚未合入引擎，收益不计入已发布引擎指标。**
[候选记录与原始计时](docs/R15_NORM_OUTPUT_PROBE.md)

## 精度、默认值与未完成事项

- 默认 `GDEC_V2_MOE=exact`、native HT 关闭、`GDEC_SPEC_PRECISION=legacy`。
  `exact` 指专家运算保持该 FP32 参考的顺序，不是未量化模型质量保证；
  默认密集 HT/Q6 转 BF16 还有约 7.83 GiB 常驻副本开销。
- `grouped`、`grouped-f16`、native HT、`aligned` 和后续可选融合需显式选择。
  本页高速数据并非出厂默认值。公共采样器修正并非全部由 `aligned` 门控。
- 较早 1020 个 teacher-forced 位置，exact 首选 token 与存储参考一致率 100%，
  grouped 为 87.843%。这是行为一致率，**不是答题正确率**；局部误差小、PPL 接近，
  也不能保证整模型输出或统计任务质量。
- 部分串行/MTP 差异已定位并修复，测试过的有限配置可对齐；
  任意随机采样、warm/cache、多槽位、所有长输入与多模态仍不能承诺全场景等价。
- 最新累计版本的旧权重完整回归、Linux 新路径实机验证、办公工具失败修复、
  长输入短输出 MTP 稳定收益和 R15 端到端验证仍待完成。

[精度策略](docs/SPEC_NUMERIC_ALIGNMENT.md) /
[专家数值实验](docs/HGN_V2_GROUPED_OPTIMIZATION.md) /
[完整边界说明](docs/WINDOWS_V2_RESEARCH_CHECKPOINT.md)

## Windows 构建与开始使用

使用单独目录检出本分支，准备 Git Bash、TheRock ROCm 工具链及匹配的驱动。
在 Git Bash 中设置 `THEROCK` 为本机工具链目录，再构建：

```bash
git clone --branch codex/halogen-v2-support https://github.com/jerrydong1988/gfx1151-engine.git
cd gfx1151-engine
bash build_win.sh
bash build_win.sh api
bash build_win.sh launcher
bash build_win.sh v2-test
bash build_win.sh test
```

1. 按 [v2 隔离配置](docs/HGN_V2.md) 修改 `service.conf`：主权重、匹配的 PLE sidecar、
   tokenizer，先用短上下文和单槽位。仓库默认配置仍指向旧 w4b，克隆不会自动切换 v2。
2. 在 PowerShell 运行 `.\start_win.exe --check` 检查配置，再运行 `.\start_win.exe`。
   首次完整加载和请求成功是运行验证，单元测试通过不能替代它们。
3. 要复现后续研究配置，按 [R13 参数与限制](docs/WINDOWS_V2_RESEARCH_CHECKPOINT.md)
   在启动前设置进程环境变量；它们不是新增 `service.conf` 键。
   空外部 MTP 文件或 gamma=0 都不表示串行模式。

Windows 当前启动器已设置 `GDEC_GEMM_WMMA` 和 `GDEC_GDN_FUSED`；
不要沿用旧 README 中“Windows 未启用”的描述。实际运行库身份、内存/提交额度、
上下文长度和并发数仍会影响加载与运行，不能由 128 GiB 容量单独推定 256K 多并发可用。

详细构建步骤见 [BUILD.md](BUILD.md)。部分上游文档保留了历史参数、性能和平台结论；
本分支的新路径与验证范围以这里链接的研究记录为准。本仓库未发布全部本地测试清单、
输入捕获和任务驱动，现有构建命令不能一键复现本页全部表格。

## 优化记录与文档导航

| 记录 | 内容 |
|---|---|
| [HGN v2](docs/HGN_V2.md) / [首次验证](docs/HGN_V2_VALIDATION.md) | 格式、加载、配置、早期 Windows 实测和旧权重检查 |
| [首轮内核优化](docs/HGN_V2_OPTIMIZATION.md) | exact / WMMA 探索及被拒绝的精度折中 |
| [顺序保持优化](docs/HGN_V2_EXACT_OPTIMIZATION.md) | 同一 v2 下的 exact 提速与 token 对照 |
| [分组矩阵核](docs/HGN_V2_GROUPED_OPTIMIZATION.md) | grouped/native HT、历史上游对照、数值及任务失败 |
| [MTP 数值对齐](docs/SPEC_NUMERIC_ALIGNMENT.md) | 对齐策略、使用方法与未覆盖范围 |
| [R1–R14 累计记录](docs/WINDOWS_V2_RESEARCH_CHECKPOINT.md) | 已提交实现、长输入性能、工具循环及复现条件 |
| [R15 候选](docs/R15_NORM_OUTPUT_PROBE.md) | 尚未接入引擎的双输出归一化微测试 |

继承的格式与使用文档：[快速开始](QUICKSTART.md)、[GGUF](GGUF.md)、
[旧 HGN HQ](HGN-HQ.md)、[HGN 容器](HGN-FORMAT.md)、[MTP](MTP.md)、
[ngram](NGRAM.md)、[并发](CONCURRENCY.md)、[转换](CONVERT.md)、[KLD](KLD.md)。

## 致谢

本项目的实现方式借鉴了 peonist-ai 的 [halogen-flash-server](https://github.com/peonist-ai/halogen-flash-server);`.hgn` 权重容器格式即 halogen 的 checkpoint 容器格式(见 [HGN-FORMAT.md](HGN-FORMAT.md))。感谢 halogen 作者的工作。

GGUF 支持大量参考了 [gufo](https://github.com/gufo-org/gufo)(MIT 许可证):路由专家 F16 WMMA GEMM kernel 移植自其 RoutedF16GEMMKernel(`src/gpu/parts/26_kernels_moe_gguf.inc`),hgn q4cp / GGUF IQ4 的 LUT 解码版沿用同一条流水线(`27_kernels_moe_lut.inc`),GGUF 与引擎张量间的变换语义参考其 reference.cpp(`src/gguf_map.h`);prefill 的 HC 门控融合、生产者 epilogue 直写下一 GEMM 输入等优化也借鉴了 gufo 的做法(对照分析见 [GUFO-GAP.md](GUFO-GAP.md))。

ngram 投机解码的起草思路与两级 prompt 缓存借鉴了开源项目 [llama.cpp](https://github.com/ggml-org/llama.cpp)(MIT 许可证),GGUF 权重与视觉塔(mmproj)直接复用 llama.cpp 的同一份文件;ngram 声明详见 [NGRAM.md](NGRAM.md) 的「来源与声明」一节。

## 许可证

[AGPL-3.0](LICENSE)
