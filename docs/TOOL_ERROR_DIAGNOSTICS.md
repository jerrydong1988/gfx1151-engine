# 工具调用错误诊断

API 继续以 `invalid_tool_call` 报告模型生成的非法工具调用。本次改进只增加诊断，
不改变工具校验、流式发送顺序、失败状态或输出预算分类，也不执行工具、自动改写参数或重试。

## 示例：文件交付时传入多余字段

若 `present.files` 的元素只允许 `path`、`description`，但第三个文件携带 `status`，
错误消息会明确说明是未声明字段，而不再只说 `value is forbidden`：

```text
Invalid tool arguments for tool "present" (call index 1) at "$.files[2].status": undeclared property "status"; allowed properties: ["path","description"] (additionalProperties=false)
```

对应的 `error.details`：

```json
{
  "path": "$.files[2].status",
  "keyword": "additionalProperties",
  "unexpected_property": "status",
  "allowed_properties": ["path", "description"],
  "tool": "present",
  "call_index": 1
}
```

`call_index: 1` 表示该响应的第二个工具调用；`files[2]` 表示数组第三项。索引均从 0 开始，
不是整个会话的步骤编号，也不表示工具已执行。

## 字段与错误类别

| 字段 | 含义 |
| --- | --- |
| `tool` | 解析到的工具名称；无法识别名称的 framing 错误可能没有此字段 |
| `call_index` | 本次响应内从 0 开始的工具调用序号 |
| `path` | 参数位置；普通名称使用点号，含点号等特殊字符的名称使用 `$.items["a.b"]` |
| `keyword` | 违反的规则，例如 `type`、`required`、`additionalProperties`、`enum`、`const`、`allOf`/`anyOf`/`oneOf` |
| `expected_types` / `actual_type` | 类型错误的预期类型与实际类型，不包含实际参数值 |
| `missing_property` | 缺失的必填字段名 |
| `unexpected_property` / `allowed_properties` | 多余字段名与该对象声明的候选字段；实际值仍须通过各自 schema 校验 |

解析器自身的规则使用 `unknownTool`、`duplicateParameter`、`argumentsObject`、`framing`、
`validationDepth`；布尔 `false` schema 使用 `falseSchema`，以区别于未声明字段。
这些是本 API 的诊断约定，并非所有 OpenAI 兼容服务通用的错误字段。

诊断只返回有长度上限的工具名、字段名、位置、规则和类型，不回显参数值、原始 XML 或完整 schema。
长名称/路径可能显示 `...`；允许字段列表最多列出 8 个名称，名称最长保留 64 字节的 UTF-8 前缀，
发生列表或名称截断时标记 `allowed_properties_truncated: true`。
预期类型列表也有限长；列表截断时标记 `expected_types_truncated: true`。

## 返回与日志

- Chat Completions：错误中的 `code`、`type`、`message`、`param` 保持原有形状，另加 `details`。
  非流式错误保持 HTTP 502；流已开始时仍发送 SSE 错误事件，不改变已发送的 HTTP 200。
- Responses：非流式 `response.error.details` 与流式 `response.failed.response.error.details` 一致。
  原来的 `failed`/`incomplete` 区分保持不变。
- 日志原来的 `terminal=invalid_tool_call` 行新增 `diagnostic={...}`，仍不记录参数值。
- 普通 HTTP/服务错误不附加工具诊断。客户端应按错误码识别错误，不依赖完整英文消息逐字匹配。

本 API 不知道客户端是否已执行先前发出的调用。诊断不承诺可以安全重试；
客户端仍需结合自己的执行记录决定如何恢复。

## 验证

`src/api/toolparse_test.cpp` 覆盖整个响应/逐字节解析、具体数组位置、多调用定位、
字段转义、类型/必填/布尔 false 规则、诊断限长和参数值隔离。

```text
python tools/api_tool_failure_test.py --api <candidate-api-executable>
```

该测试启动独立的 CPU 假引擎与 API 实例，覆盖 Chat Completions/Responses ×
流式/非流式共 68 个场景，包括 `present` 多余字段、`web_search` 数组类型错误、
修正后的合法调用、未知工具、未完成调用及输出长度截断。它不使用生产推理实例。
