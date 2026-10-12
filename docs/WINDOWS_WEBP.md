# Windows WebP 图片支持

Windows API 支持静态 PNG、JPEG、WebP，包括 WebP 有损、无损和带透明通道的图片。
WebP 使用与 Linux 相同的 libwebp RGB 解码路径；透明通道不参与模型输入，保持原有 RGB 预处理规则。
不支持动画 WebP，返回明确的 400 错误。`/health.vision` 的 `formats`、
`animated_webp`、`webp_decoder` 和 `webp_decoder_version` 来自实际编入的解码模块。

Chat Completions 支持用户和工具消息中的 `image_url`；Responses 的
`function_call_output.output` 支持文本与 `input_image` 混合数组，保留顺序和多轮历史中的图片。
图片仍须使用内联 data URL 或 base64，不会访问远程图片 URL。
原有图片数量、像素和总视觉 token 限制继续生效。

## 构建与分发

使用原有 Windows C++ / SDK 环境，另外需要 Python 3.12+、CMake、Ninja：

```sh
bash build_win.sh api
```

构建脚本调用 `tools/build_webp_win.py`，首次从官方发行目录下载 libwebp 1.6.0
源码并验证 SHA-256：

`e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564`

来源：<https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz>

源码和编译缓存位于 `build/deps/`。仅静态链接 `webpdecoder.lib`，不引入 WebP DLL。
分发 API 时同时保留 `build/licenses/libwebp/` 中的 COPYING、PATENTS、AUTHORS。
未安装解码库或编译失败会使构建失败，不会生成宣称支持 WebP 的残缺版本。

## 验证

```sh
# Pillow 仅用于生成独立对照样本，不参与服务运行。
bash build_win.sh webp-test
```

CPU 测试检查有损/无损/透明图片的预处理结果、PNG/JPEG 回归、截断/损坏/动画/
超大图片拒绝、能力声明，以及带图工具结果和历史回放。

`tools/harness_webp_test.cjs` 使用已安装 Harness 的 read_image 工具、附件转换、
附件读取和流式 LLM 适配器，连接真实服务；仅服务注册和测试文件系统使用隔离测试接口。
需要通过 Harness 的 Electron 可执行文件，以 `ELECTRON_RUN_AS_NODE=1` 运行，参数依次为
`app.asar/dsh` 路径、`http://127.0.0.1:18861/v1` 和独立输出目录。
读取现有 `gfx1151-engine` 模型路由，配置只在测试进程内覆盖，不修改 Harness 用户配置或会话。
这项验证不操作桌面页面，也不自动续跑已有用户任务。
