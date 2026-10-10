# Windows hybrid mainline deployment

## 上游兼容性整合 — 2026-10-10

纳入窄长图片补边、共享 KV 总容量上报、YaRN 启动前校验和有限的工具参数格式兼容。
Windows 原生启动器检查实际传给子进程的参数；84 项有效设置保持一致，仍为双路、
单路 262,144 上下文、524,288 共享 KV、32 图以及既有精度优先配置。
默认思考档位、提示文本、模型文件、量化和推理算术路径保持不变。
本轮成对替换 API/引擎并更新原生启动器；运行库与 `service.conf` 不替换。

取舍、验证边界及完整数值见 [上游精选整合记录](UPSTREAM_INTEGRATION_20261010.md)。
本节之后的版本号、二进制身份及验证描述为历史记录；安装身份、现场验证和回退路径以应用目录 `deployment.json` 为准。

## 多图容量更新 — 2026-10-07

本次 API 与引擎成对扩容，本地 `MAX_IMAGES` 从 16 提高到 32，其他有效运行参数保持一致。
增加总视觉数据预算，不改变已有图片缩放、GPU 计算和精度设置。
40 个新旧交错请求未观察到明显性能回退；17／20／32 图、32 张 1080p 图、双槽和失败恢复验证通过。
本节之后的 16 图限制及二进制哈希为历史记录；最新限制、二进制身份和验证边界见
[32 张图片扩容验收](VISION_CAPACITY_20261007.md)，现场状态与回退目录见应用目录 `deployment.json`。

## 鲜明科技视觉升级 — 2026-10-07

采用用户选定的“深色底色、更大胆的色彩与版式”方向，已部署到既有应用路径。
使用 UI/UX Pro Max 的设计检索作为参考，并按本地推理控制台的信息密度进行取舍。

- 深蓝黑底色、荧光黄绿主强调与冷紫蓝辅助色；主要按钮和选中导航使用高对比实色。
- 总览采用不等权布局：Prefill 为主指标，Decode 与首字延迟并列，请求总数收成横向摘要。
  调整数字字体、标题层级、间距和图表配色；装饰线条不表示测量数据。
- 请求、设置、对话、内存五页共享同一套颜色与视觉层级。移动端保留完整导航，
  标题说明和时间筛选分行排列，表单仍保留字段错误定位及键盘遮挡修复。
- 使用 Windows 本地字体回退；无字体下载、CDN、额外框架或持续装饰动画。
  推理参数、统计口径、刷新频率和请求协议不变。

验证：18 组浏览器回归、编译后的 API 双槽模拟回归、部署后的设置交互验证通过；
40 个页面/宽度组合无横向溢出（320–1920 px），12 组文字配色抽测最低 7.89:1。
真实部署的五页与移动端截图已检查；未修改生产参数覆盖表。
仅更新 API 程序与部署文档，引擎、启动器和 `service.conf` 的 SHA-256 保持不变。

API SHA-256：`d3f932fb1d3c6c62b51cf5178ae1474937cdf55b7da5bd3f6a04dfa8dcee3d89`。
验证、截图及上一版备份：`logs/ui-style-20261007/`。

## 设置页交互与可访问性修复 — 2026-10-07

按 UI/UX Pro Max 审查结果修复六项问题，保留现有视觉风格和服务端参数语义：

- 键盘聚焦字段时为移动导航和保存栏保留可见空间；保存栏超过可用高度一半时恢复普通布局。
- 数值错误在字段旁显示，关联 `aria-invalid` 与错误说明，保存时定位首个错误字段；
  网络、认证和持久化错误放在保存操作区，失败时定位相应输入框或错误提示。
- “跳转到内容”只移动焦点，不再将设置页切换到总览。
- 保存、重新读取完成后恢复焦点；用户已切换页面或操作其他控件时不抢回焦点。
- 浮点输入允许范围内任意小数，整数参数仍要求整数；管理密钥增加持续可见的标签。

验证：18 组浏览器回归通过，含桌面/窄屏正反向 Tab 遍历、多个错误定位与修正、
认证/服务端故障、异步保存期间切换页面，以及原有五页功能。编译后的 API 双槽模拟回归通过。
部署后的 HTML 与源码一致；真实控制台的跳转、数值校验和错误可见性通过，
没有向生产服务提交参数修改。尚未进行 NVDA 真人读屏验证。

应用路径：`C:\Users\Jerry\llama.cpp\gfx1151-engine`。仅替换 API 程序；
引擎、启动器、`service.conf` 的 SHA-256 保持不变，生产参数覆盖表保持不变。
API SHA-256：`a1e084576eaba24b830082a804fbda8a29378987615d8dae6281fa201ac00c6b`。
浏览器/API 验证、部署截图及上一版备份位于 `logs/ui-settings-fix-20261007/`。

## Impeccable 控制台优化 — 2026-10-07

状态：已完成并部署。按 Impeccable 的 Operate / Polish 路径优化既有界面，
保留已经选定的深色、薄荷绿配色及五个功能页；没有引入框架、外部字体或 CDN。

- 统一辅助文字与数据字号，区分导航选中和悬停状态，缩减并发面板的嵌套容器和无效留白。
- 手机端五个导航入口完整可见；请求记录改为纵向指标布局，十项参数改为双列控件与独立说明，
  无需横向滚动查看或编辑。触控控件至少 44 像素高，保存操作保持可达。
- 吞吐图默认按请求顺序等距显示，避免相隔数小时的请求挤成一簇；横轴明确标注，
  仍可切回实际时间。鼠标、方向键与 Home/End 可查看真实时间和吞吐值。
  指标计算、日期 UTC 口径、最近 100 条记录范围和最近 50 次请求选取范围保持不变。
- 改善未保存、处理中、暂停/继续、无匹配记录等状态；手动切换日期时，即使自动刷新暂停，
  仍读取所选范围的数据。异步查询串行收敛到最后一次选择。
- 删除详情抽屉的重复英文装饰标题，统一 SVG 图标、键盘焦点和输入控件的说明关联。

验证：13 组浏览器回归通过，覆盖六种宽度（320–1920）、五个页面、完整手机控件、图表键盘操作、
日期筛选、鉴权/持久化故障、流式/取消/恢复及原有交互。编译后的 API 双槽模拟回归通过；
现场真实模型、参数读取、请求详情、内存采样通过，浏览器无脚本错误。
Impeccable 单次检测没有主要发现；两项提示中，toast 的边框叠加阴影已修正，
缺失数据使用的 `—` 占位符按数据语义保留。未启用项目 hooks，未自动创建产品上下文文件。

仅更新控制台所在 API 程序；部署前后引擎、启动器和 `service.conf` 的 SHA-256 一致，
生产参数覆盖表保持不变。没有重新量化、修改精度开关或重新进行模型性能基准。
API SHA-256：`4ded0b2131f607b265d096b59c9eeb53ad706576d3f8620bec6d40c9446500fd`。
验证记录、前后截图和上一版备份位于源工作区 `logs/ui-impeccable-20261007/`。

## 深色监控控制台 — 2026-10-06

状态：第一版设计已接入真实接口、编译并部署到应用目录。访问
`http://127.0.0.1:18861/` 或 `/dashboard` 即可使用。

- 左侧导航分为运行总览、请求记录、参数设置、对话测试、内存资源。
  总览以 Prefill、Decode、TTFT 和请求数为主要指标，详细数据放入请求抽屉。
- 所有指标均来自现有 API。`/health` 标为 API 自报状态，不代表引擎存活检测；
  不推断独立槽位状态，也不把静态精度标签伪装成实时配置。
- 请求日期使用 UTC。汇总覆盖所选日期的全部日志；列表在最近 100 条记录内筛选，
  曲线来自其中最近 50 次请求的有效测量。缺失耗时不填零，全缓存命中不计算 Prefill 吞吐。
  草稿接受数按 `max(commit - rounds, 0)` 计算，与 API 的接受率口径一致。
- 保留全部十项服务端参数覆盖、鉴权与持久化错误提示。切换页面或自动刷新不会丢失编辑；
  参数页只调整采样和思考参数，不切换引擎计算精度策略。
- 对话支持中文流式、思考分离、停止和多轮。提前断流显示错误并保留部分文字；
  未完成的轮次不进入后续对话上下文。管理密钥不写入浏览器存储。
- 隐藏标签页及手动暂停时停止自动轮询；内存只在该页可见且 API 空闲时采样，
  降低查询占用推理槽位的影响。查询发出后仍可能与新请求竞争，并非无干扰采样保证。
- 继续采用单文件内联 HTML/CSS/JS，无 CDN、外部字体或前端构建依赖。

验证：

- `tools/dashboard_ui_test.cjs` 的 11 组浏览器回归通过：真实形状的接口数据、UTC/结束原因筛选、
  抽屉键盘与焦点、十项参数编辑、鉴权/磁盘失败、中文跨块 SSE、断流/错误、取消/恢复、
  按需内存采样、暂停/重连，以及六种宽度和五个页面。测试写入只进入本地模拟服务。
- Edge 无页面脚本错误、无外部资源请求；320、390、768、1280、1600、1920 像素宽度检查通过，
  桌面和手机截图已实际查看。表格在窄屏内部横向滚动。
- 编译后的 `/dashboard` 返回值与源码内嵌 HTML 一致，`Cache-Control: no-store` 保留。
  双槽模拟引擎上的 `tools/api_regression_test.py --slots 2` 全部通过。
- 部署后真实模型回复“你好”，停止长输出后能再次回复“恢复正常”；实际请求日志、参数读取、
  详情抽屉和内存采样通过。生产参数覆盖表未修改。
- 只替换 `build/gdec-api-win.exe`；引擎、启动器和 `service.conf` 部署前后 SHA-256 一致，
  保留之前的八项精度优先设置及双槽/256K 配置。此轮没有重新进行模型数值或速度基准。

API SHA-256：`a84fe5ea488b5843b337d1dd7275ca3965c04e9d3a38d8388023fbea70c008a7`。
原 API 与部署元数据保存在源工作区 `logs/ui-implementation/previous-deployment/`；
浏览器、API 和现场证据在 `logs/ui-implementation/`。这些测试输出不进入版本库。

复现浏览器回归：已有 Node 和 Playwright 环境中运行 `node tools/dashboard_ui_test.cjs`。
若未使用 Playwright 自带浏览器，可用 `BROWSER_EXECUTABLE` 指向 Edge/Chrome；
`NODE_PATH` 可指定现有 Playwright 安装，`DASHBOARD_TEST_OUTPUT` 可指定截图目录。

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
