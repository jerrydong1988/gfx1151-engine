// Clean-room Qwen XML tool-call adapter. The wire grammar and schema coercion
// semantics follow the public Apache-2.0 Transformers response parser.
#include "toolparse.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <unordered_map>

#include "json_py.h"

namespace toolparse {
namespace {

constexpr const char* kToolOpen = "<tool_call>";
constexpr const char* kFunctionOpen = "<function=";
constexpr const char* kParameterOpen = "<parameter=";
constexpr const char* kParameterClose = "\n</parameter>\n";
constexpr const char* kParameterCloseCrlf = "\r\n</parameter>\r\n";
constexpr const char* kFunctionClose = "</function>";
constexpr const char* kToolClose = "</tool_call>";

bool ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

// gufo b722a61：模型在结构化参数里有时会吐 Python 字面量（True/False/None）。
// json::parse 失败时，把双引号字符串之外的这些词改写成 JSON 字面量再试一次。
std::string python_literals_to_json(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    bool in_string = false;
    for (size_t i = 0; i < value.size();) {
        const char c = value[i];
        if (in_string) {
            const size_t len = c == '\\' && i + 1 < value.size() ? 2 : 1;
            out.append(value.substr(i, len));
            in_string = c != '"';
            i += len;
            continue;
        }
        if (!std::isalpha(static_cast<unsigned char>(c))) {
            out += c;
            in_string = c == '"';
            ++i;
            continue;
        }
        size_t end = i;
        while (end < value.size() &&
               (std::isalnum(static_cast<unsigned char>(value[end])) != 0 ||
                value[end] == '_'))
            ++end;
        const std::string_view word = value.substr(i, end - i);
        out += word == "True" ? std::string_view("true")
             : word == "False" ? std::string_view("false")
             : word == "None" ? std::string_view("null")
                              : word;
        i = end;
    }
    return out;
}

void trim_ascii(std::string* value) {
    size_t begin = 0;
    while (begin < value->size() && ascii_space((*value)[begin])) ++begin;
    size_t end = value->size();
    while (end > begin && ascii_space((*value)[end - 1])) --end;
    *value = value->substr(begin, end - begin);
}

bool is_prefix_of(std::string_view value, std::string_view token) {
    return value.size() <= token.size() && token.substr(0, value.size()) == value;
}

size_t suffix_prefix_len(std::string_view value, std::string_view token) {
    const size_t top = std::min(value.size(), token.size() - 1);
    for (size_t n = top; n > 0; --n) {
        if (value.substr(value.size() - n) == token.substr(0, n)) return n;
    }
    return 0;
}

std::string quote_json(const std::string& value) {
    return json_py::dumps(json(value), /*spaced=*/false);
}

std::string escape_json_fragment(const std::string& value) {
    std::string quoted = quote_json(value);
    if (quoted.size() < 2) return {};
    return quoted.substr(1, quoted.size() - 2);
}

void collect_schema_types(const json& schema, std::vector<std::string>* result) {
    if (!schema.is_object()) return;
    auto type = schema.find("type");
    if (type != schema.end()) {
        if (type->is_string()) result->push_back(type->get<std::string>());
        if (type->is_array()) {
            for (const auto& item : *type)
                if (item.is_string()) result->push_back(item.get<std::string>());
        }
    }
    auto any_of = schema.find("anyOf");
    if (any_of != schema.end() && any_of->is_array()) {
        for (const auto& item : *any_of) collect_schema_types(item, result);
    }
    auto nullable = schema.find("nullable");
    if (nullable != schema.end() && nullable->is_boolean() && nullable->get<bool>() &&
        std::find(result->begin(), result->end(), "null") == result->end())
        result->push_back("null");
}

json coerce_value(const std::string& raw, const std::vector<std::string>& types) {
    for (const std::string& type : types) {
        if (type == "integer") {
            errno = 0;
            char* end = nullptr;
            const long long value = std::strtoll(raw.c_str(), &end, 10);
            if (errno == 0 && end != raw.c_str() && *end == '\0') return value;
        } else if (type == "number") {
            errno = 0;
            char* end = nullptr;
            const double value = std::strtod(raw.c_str(), &end);
            if (errno == 0 && end != raw.c_str() && *end == '\0' && std::isfinite(value)) {
                if (std::floor(value) == value && raw.find('.') == std::string::npos &&
                    value >= static_cast<double>(std::numeric_limits<long long>::min()) &&
                    value < 9223372036854775808.0)
                    return static_cast<long long>(value);
                return value;
            }
        } else if (type == "boolean") {
            std::string value = raw;
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (value == "true" || value == "1") return true;
            if (value == "false" || value == "0") return false;
        } else if (type == "null") {
            if (raw == "null" || raw == "None") return nullptr;
        } else if (type == "object" || type == "array") {
            json value = json::parse(raw, nullptr, false);
            if (value.is_discarded())
                value = json::parse(python_literals_to_json(raw), nullptr, false);
            if ((type == "object" && value.is_object()) ||
                (type == "array" && value.is_array()))
                return value;
        }
    }
    return raw;
}

bool function_exists(const json& tools, const std::string& name) {
    for (const auto& tool : tools) {
        if (tool.is_object() && tool.contains("function") && tool["function"].is_object() &&
            tool["function"].contains("name") && tool["function"]["name"].is_string() &&
            tool["function"]["name"].get_ref<const std::string&>() == name)
            return true;
    }
    return false;
}

std::string diagnostic_name(const std::string& name) {
    size_t end = std::min<size_t>(name.size(), 128);
    while (end > 0 && end < name.size() &&
           (static_cast<unsigned char>(name[end]) & 0xc0) == 0x80) --end;
    return json(name.substr(0, end)).dump(-1, ' ', false, json::error_handler_t::replace) +
           (end < name.size() ? "..." : "");
}

std::string bounded_text(const std::string& text, size_t limit = 128) {
    size_t end = std::min(text.size(), limit);
    while (end > 0 && end < text.size() &&
           (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
    // Replace invalid UTF-8 as well as keeping truncation on a codepoint boundary.
    return json::parse(json(text.substr(0, end)).dump(-1, ' ', false,
                       json::error_handler_t::replace)).get<std::string>() +
           (end < text.size() ? "..." : "");
}

std::string property_path(const std::string& path, const std::string& key) {
    const auto identifier_char = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    bool identifier = !key.empty() && identifier_char(key[0]);
    for (unsigned char c : key)
        identifier = identifier && (identifier_char(c) || (c >= '0' && c <= '9'));
    const std::string name = bounded_text(key);
    return bounded_text(path + (identifier ? "." + name : "[" + quote_json(name) + "]"), 512);
}

std::string value_type(const json& value) {
    if (value.is_number_integer()) return "integer";
    return value.type_name();
}

// Output validation for common tool schemas, not constrained decoding or a full
// JSON Schema implementation. Unknown keywords remain unsupported. Bound depth
// and diagnostic size; never echo model argument values in an error response.
bool matches_type(const json& value, const std::string& type) {
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "string") return value.is_string();
    if (type == "boolean") return value.is_boolean();
    if (type == "null") return value.is_null();
    if (type == "number") return value.is_number();
    if (type == "integer") {
        if (value.is_number_integer()) return true;
        if (!value.is_number_float()) return false;
        const double d = value.get<double>();
        return std::isfinite(d) && std::floor(d) == d;
    }
    return true; // Not a supported type declaration.
}

bool supported_schema(const json& schema, int depth = 0) {
    if (depth > 64) return false;
    if (schema.is_boolean()) return true;
    if (!schema.is_object()) return false;
    for (auto it = schema.begin(); it != schema.end(); ++it) {
        const auto& k = it.key();
        if (k == "type" || k == "enum" || k == "const" || k == "required" || k == "nullable" ||
            k == "description" || k == "title" || k == "default" || k == "examples") continue;
        if (k == "properties" && it->is_object()) {
            for (const auto& child : *it) if (!supported_schema(child, depth + 1)) return false;
        } else if (k == "items" || k == "additionalProperties") {
            if (!supported_schema(*it, depth + 1)) return false;
        } else if ((k == "allOf" || k == "anyOf" || k == "oneOf") && it->is_array()) {
            for (const auto& child : *it) if (!supported_schema(child, depth + 1)) return false;
        } else return false;
    }
    return true;
}

bool check_value(const json& value, const json& schema, const std::string& path,
                 std::string* error, json* details, int depth = 0) {
    auto fail = [&](const std::string& reason, const std::string& keyword,
                    json extra = json::object(), const std::string& location = "") {
        const std::string at = bounded_text(location.empty() ? path : location, 512);
        *error = "Invalid tool arguments at " + quote_json(at) + ": " + reason;
        *details = {{"path", at}, {"keyword", keyword}};
        details->update(extra);
        return false;
    };
    if (depth > 64) return fail("validation depth limit exceeded", "validationDepth");
    if (schema.is_boolean())
        return schema.get<bool>() || fail("value is forbidden by a false schema", "falseSchema");
    if (!schema.is_object()) return true;
    if (schema.contains("type")) {
        const auto& t = schema["type"];
        bool ok = !t.is_string() && !t.is_array();
        if (t.is_string()) ok = matches_type(value, t.get<std::string>());
        if (t.is_array()) for (const auto& item : t)
            if (item.is_string() && matches_type(value, item.get<std::string>())) ok = true;
        if (value.is_null() && schema.value("nullable", json(false)) == true) ok = true;
        if (!ok) {
            json expected = json::array();
            if (t.is_string()) expected.push_back(bounded_text(t.get<std::string>(), 32));
            if (t.is_array()) for (const auto& item : t) {
                if (expected.size() == 8) break;
                if (item.is_string()) expected.push_back(bounded_text(item.get<std::string>(), 32));
            }
            if (schema.value("nullable", json(false)) == true &&
                std::find(expected.begin(), expected.end(), json("null")) == expected.end())
                expected.push_back("null");
            json extra = {{"expected_types", expected}, {"actual_type", value_type(value)}};
            if (t.is_array() && t.size() > 8) extra["expected_types_truncated"] = true;
            return fail("expected type " + expected.dump() + "; received " + value_type(value),
                        "type", std::move(extra));
        }
    }
    if (schema.contains("enum") && schema["enum"].is_array()) {
        bool found = false;
        for (const auto& item : schema["enum"]) if (value == item) found = true;
        if (!found) return fail("value is not in the declared enum", "enum");
    }
    if (schema.contains("const") && value != schema["const"])
        return fail("value does not match the declared const", "const");
    for (const char* kind : {"allOf", "anyOf", "oneOf"}) {
        if (!schema.contains(kind) || !schema[kind].is_array()) continue;
        // In particular, an unresolved $ref cannot be counted as a matching
        // branch of oneOf: doing that rejects otherwise valid requests.
        bool supported = true;
        for (const auto& branch : schema[kind])
            if (!supported_schema(branch)) supported = false;
        if (!supported) continue;
        size_t matched = 0;
        for (const auto& branch : schema[kind]) {
            std::string ignored;
            json ignored_details;
            if (check_value(value, branch, path, &ignored, &ignored_details, depth + 1)) ++matched;
        }
        if ((std::string(kind) == "allOf" && matched != schema[kind].size()) ||
            (std::string(kind) == "anyOf" && matched == 0) ||
            (std::string(kind) == "oneOf" && matched != 1))
            return fail(std::string("does not satisfy ") + kind, kind);
    }
    if (value.is_object()) {
        if (schema.contains("required") && schema["required"].is_array())
            for (const auto& key : schema["required"])
                if (key.is_string() && !value.contains(key.get_ref<const std::string&>()))
                    return fail("missing required property " + diagnostic_name(key.get<std::string>()),
                                "required", {{"missing_property", bounded_text(key.get<std::string>())}},
                                property_path(path, key.get<std::string>()));
        const auto properties = schema.find("properties");
        for (auto it = value.begin(); it != value.end(); ++it) {
            const std::string child = property_path(path, it.key());
            if (properties != schema.end() && properties->is_object() && properties->contains(it.key())) {
                if (!check_value(it.value(), (*properties)[it.key()], child, error, details, depth + 1)) return false;
            } else if (schema.contains("additionalProperties")) {
                // patternProperties requires a regex dialect not implemented here;
                // do not falsely reject a possibly pattern-matched property.
                if (!schema.contains("patternProperties")) {
                    if (schema["additionalProperties"] == false) {
                        json allowed = json::array();
                        bool truncated = false;
                        if (properties != schema.end() && properties->is_object()) {
                            for (auto p = properties->begin(); p != properties->end(); ++p) {
                                if (p.value() == false) continue;
                                if (allowed.size() == 8) { truncated = true; break; }
                                allowed.push_back(bounded_text(p.key(), 64));
                                truncated = truncated || p.key().size() > 64;
                            }
                        }
                        json extra = {{"unexpected_property", bounded_text(it.key())},
                                      {"allowed_properties", allowed}};
                        if (truncated) extra["allowed_properties_truncated"] = true;
                        return fail("undeclared property " + diagnostic_name(it.key()) +
                                    "; allowed properties: " + allowed.dump() +
                                    (truncated ? " (truncated)" : "") + " (additionalProperties=false)",
                                    "additionalProperties", std::move(extra), child);
                    }
                    if (!check_value(it.value(), schema["additionalProperties"], child,
                                     error, details, depth + 1)) return false;
                }
            }
        }
    }
    if (value.is_array() && !schema.contains("prefixItems") && schema.contains("items") &&
        (schema["items"].is_object() || schema["items"].is_boolean()))
        for (size_t i = 0; i < value.size(); ++i)
            if (!check_value(value[i], schema["items"],
                             bounded_text(path + "[" + std::to_string(i) + "]", 512),
                             error, details, depth + 1)) return false;
    return true;
}

bool append_function(const json& source, json* output, std::string* error) {
    json function;
    if (source.contains("function")) {
        if (!source["function"].is_object()) {
            *error = "function tools require a function object";
            return false;
        }
        function = source["function"];
    } else {
        for (const char* key : {"name", "description", "parameters", "strict"})
            if (source.contains(key)) function[key] = source[key];
    }
    if (!function.contains("name") || !function["name"].is_string() ||
        function["name"].get_ref<const std::string&>().empty()) {
        *error = "function tools require a non-empty string name";
        return false;
    }
    if (function.contains("description") && !function["description"].is_string()) {
        *error = "function tool description must be a string";
        return false;
    }
    if (function.contains("parameters") && !function["parameters"].is_object()) {
        *error = "function tool parameters must be an object";
        return false;
    }
    if (function.contains("strict") && !function["strict"].is_boolean()) {
        *error = "function tool strict must be a boolean";
        return false;
    }
    output->push_back(json{{"type", "function"}, {"function", std::move(function)}});
    return true;
}

bool append_tool(const json& tool, json* output, std::string* error) {
    if (!tool.is_object()) {
        *error = "tool entries must be objects";
        return false;
    }
    if (!tool.contains("type") || !tool["type"].is_string()) {
        *error = "tool entries require a string type";
        return false;
    }
    const std::string& type = tool["type"].get_ref<const std::string&>();
    if (type == "function") return append_function(tool, output, error);
    if (type != "namespace") return true;  // unsupported built-ins are ignored

    const json* nested = nullptr;
    if (tool.contains("tools")) nested = &tool["tools"];
    else if (tool.contains("functions")) nested = &tool["functions"];
    if (nested == nullptr) return true;
    if (!nested->is_array()) {
        *error = "namespace tools must be an array";
        return false;
    }
    for (const auto& child : *nested) {
        if (child.is_object() && !child.contains("type") && child.contains("name")) {
            if (!append_function(child, output, error)) return false;
        } else if (!append_tool(child, output, error)) {
            return false;
        }
    }
    return true;
}

std::string message_role(const json& message) {
    if (!message.is_object() || !message.contains("role") || !message["role"].is_string())
        return {};
    return message["role"].get<std::string>();
}

}  // namespace

std::string ToolChoice::parser_prefix() const {
    if (!forced()) return {};
    std::string result = "<tool_call>\n<function=";
    if (!name.empty()) result += name + ">\n";
    return result;
}

std::string ToolChoice::prompt_suffix(bool thinking_enabled) const {
    if (!forced()) return {};
    std::string result;
    if (thinking_enabled) result = "</think>\n\n";
    result += parser_prefix();
    return result;
}

bool normalize_tools(const json* input, json* output, std::string* error) {
    *output = json::array();
    error->clear();
    if (input == nullptr || input->is_null()) return true;
    if (!input->is_array()) {
        *error = "tools must be an array";
        return false;
    }
    for (const auto& tool : *input)
        if (!append_tool(tool, output, error)) return false;
    return true;
}

bool parse_tool_choice(const json* input, const json& tools, ToolChoice* output,
                       std::string* error) {
    *output = ToolChoice{};
    error->clear();
    if (input == nullptr || input->is_null()) return true;
    if (input->is_string()) {
        const std::string& value = input->get_ref<const std::string&>();
        if (value == "auto") return true;
        if (value == "none") {
            output->mode = ToolChoice::Mode::None;
            return true;
        }
        if (value == "required") {
            if (tools.empty()) {
                *error = "tool_choice='required' requires at least one function tool";
                return false;
            }
            output->mode = ToolChoice::Mode::Required;
            if (tools.size() == 1)
                output->name = tools[0]["function"]["name"].get<std::string>();
            return true;
        }
        *error = "tool_choice must be 'auto', 'none', 'required', or a named function";
        return false;
    }
    if (!input->is_object()) {
        *error = "tool_choice must be a string or object";
        return false;
    }
    const json* function = input;
    if (input->contains("function")) function = &(*input)["function"];
    if (!function->is_object() || !function->contains("name") ||
        !(*function)["name"].is_string() ||
        (*function)["name"].get_ref<const std::string&>().empty()) {
        *error = "named tool_choice requires a non-empty function name";
        return false;
    }
    output->mode = ToolChoice::Mode::Named;
    output->name = (*function)["name"].get<std::string>();
    if (!function_exists(tools, output->name)) {
        *error = "tool_choice names an unknown function: " + output->name;
        return false;
    }
    return true;
}

bool normalize_messages(json* messages, std::string* error) {
    error->clear();
    if (!messages->is_array()) {
        *error = "messages must be an array";
        return false;
    }

    for (auto& message : *messages) {
        if (!message.is_object()) continue;
        if (message_role(message) == "tool" && message.contains("tool_call_id") &&
            !message["tool_call_id"].is_string()) {
            *error = "tool_call_id must be a string";
            return false;
        }
        if (message_role(message) != "assistant" || !message.contains("tool_calls") ||
            message["tool_calls"].is_null())
            continue;
        if (!message["tool_calls"].is_array()) {
            *error = "assistant tool_calls must be an array";
            return false;
        }
        for (auto& call : message["tool_calls"]) {
            if (!call.is_object()) {
                *error = "assistant tool_calls entries must be objects";
                return false;
            }
            json* function = &call;
            if (call.contains("function")) {
                if (!call["function"].is_object()) {
                    *error = "assistant tool call function must be an object";
                    return false;
                }
                function = &call["function"];
            }
            if (!function->contains("name") || !(*function)["name"].is_string()) {
                *error = "assistant tool calls require a string function name";
                return false;
            }
            if (!function->contains("arguments") || (*function)["arguments"].is_null()) {
                (*function)["arguments"] = json::object();
            } else if ((*function)["arguments"].is_string()) {
                try {
                    json parsed = json::parse((*function)["arguments"].get<std::string>());
                    if (!parsed.is_object()) throw std::runtime_error("not object");
                    (*function)["arguments"] = std::move(parsed);
                } catch (...) {
                    *error = "assistant tool call arguments must be a JSON object string";
                    return false;
                }
            } else if (!(*function)["arguments"].is_object()) {
                *error = "assistant tool call arguments must be an object or JSON object string";
                return false;
            }
        }
    }

    json ordered = json::array();
    for (size_t i = 0; i < messages->size();) {
        json message = std::move((*messages)[i]);
        ordered.push_back(std::move(message));
        if (message_role(ordered.back()) != "assistant" ||
            !ordered.back().contains("tool_calls") ||
            !ordered.back()["tool_calls"].is_array()) {
            ++i;
            continue;
        }

        size_t end = i + 1;
        while (end < messages->size() && message_role((*messages)[end]) == "tool") ++end;
        std::vector<bool> used(end - (i + 1), false);
        for (const auto& call : ordered.back()["tool_calls"]) {
            if (!call.is_object() || !call.contains("id") || !call["id"].is_string()) continue;
            const std::string& id = call["id"].get_ref<const std::string&>();
            for (size_t j = i + 1; j < end; ++j) {
                const json& result = (*messages)[j];
                if (!used[j - (i + 1)] && result.contains("tool_call_id") &&
                    result["tool_call_id"].is_string() &&
                    result["tool_call_id"].get_ref<const std::string&>() == id) {
                    ordered.push_back(std::move((*messages)[j]));
                    used[j - (i + 1)] = true;
                    break;
                }
            }
        }
        for (size_t j = i + 1; j < end; ++j)
            if (!used[j - (i + 1)]) ordered.push_back(std::move((*messages)[j]));
        i = end;
    }
    *messages = std::move(ordered);
    return true;
}

StreamParser::StreamParser(json tools, IdFactory make_id, bool enabled)
    : tools_(std::move(tools)), make_id_(std::move(make_id)), enabled_(enabled) {}

std::vector<Event> StreamParser::feed(std::string_view piece) {
    buffer_.append(piece.data(), piece.size());
    return pump(false);
}

std::vector<Event> StreamParser::finish() { return pump(true); }

void StreamParser::set_error(std::string message, const std::string& keyword,
                             const std::string& path) {
    error_ = std::move(message);
    error_details_ = {{"path", bounded_text(path, 512)}, {"keyword", keyword},
                      {"call_index", current_.name.empty() ? next_index_ : current_.index}};
    if (!current_.name.empty()) error_details_["tool"] = bounded_text(current_.name);
}

void StreamParser::emit_content(std::string text, std::vector<Event>* events) {
    if (text.empty()) return;
    content_ += text;
    events->push_back(Event{EventType::Content, 0, {}, {}, std::move(text)});
}

void StreamParser::emit_arguments(std::string text, std::vector<Event>* events) {
    if (text.empty()) return;
    current_.arguments += text;
    events->push_back(
        Event{EventType::ArgumentsDelta, current_.index, current_.id, current_.name,
              std::move(text)});
}

void StreamParser::start_call(std::string name, std::vector<Event>* events) {
    current_ = ToolCall{};
    current_.index = next_index_++;
    current_.id = make_id_ ? make_id_() : std::string();
    current_.name = std::move(name);
    argument_count_ = 0;
    parameter_keys_.clear();
    events->push_back(Event{EventType::CallStart, current_.index, current_.id,
                            current_.name, {}});
}

bool StreamParser::start_parameter(std::string key, std::vector<Event>* events) {
    if (std::find(parameter_keys_.begin(), parameter_keys_.end(), key) != parameter_keys_.end()) {
        set_error("Duplicate tool parameter " + diagnostic_name(key) + " for tool " +
                  diagnostic_name(current_.name), "duplicateParameter", property_path("$", key));
        return false;
    }
    parameter_keys_.push_back(key);
    parameter_key_ = std::move(key);
    parameter_types_ = schema_types(current_.name, parameter_key_);
    stream_parameter_ = !parameter_types_.empty() &&
                        std::all_of(parameter_types_.begin(), parameter_types_.end(),
                                    [](const std::string& type) {
                                        return type == "string" || type == "null";
                                    }) &&
                        std::find(parameter_types_.begin(), parameter_types_.end(), "string") !=
                            parameter_types_.end();
    value_started_ = false;
    if (stream_parameter_) {
        std::string prefix = argument_count_++ == 0 ? "{" : ",";
        prefix += quote_json(parameter_key_) + ":\"";
        emit_arguments(std::move(prefix), events);
    }
    return true;
}

void StreamParser::finish_parameter(std::string raw, std::vector<Event>* events) {
    if (stream_parameter_) {
        emit_arguments(escape_json_fragment(raw), events);
        emit_arguments("\"", events);
    } else {
        std::string trimmed = raw;
        trim_ascii(&trimmed);
        json value = coerce_value(trimmed, parameter_types_);
        if (value.is_string()) value = raw;
        std::string encoded = argument_count_++ == 0 ? "{" : ",";
        encoded += quote_json(parameter_key_) + ":";
        encoded += json_py::dumps(value, /*spaced=*/false);
        emit_arguments(std::move(encoded), events);
    }
    parameter_key_.clear();
    parameter_types_.clear();
    stream_parameter_ = false;
    value_started_ = false;
}

bool StreamParser::valid_call() {
    json args = json::parse(current_.arguments + (argument_count_ == 0 ? "{}" : "}"),
                            nullptr, false);
    if (!args.is_object()) {
        set_error("Tool arguments for " + diagnostic_name(current_.name) +
                  " are not a valid JSON object.", "argumentsObject");
        return false;
    }
    for (const auto& tool : tools_) {
        const json& function = tool["function"];
        if (function["name"] != current_.name) continue;
        if (!function.contains("parameters") || !function["parameters"].is_object())
            return true;
        const json& schema = function["parameters"];
        if (check_value(args, schema, "$", &error_, &error_details_)) return true;
        error_details_["tool"] = bounded_text(current_.name);
        error_details_["call_index"] = current_.index;
        error_.insert(std::string("Invalid tool arguments").size(),
                      " for tool " + diagnostic_name(current_.name) +
                      " (call index " + std::to_string(current_.index) + ")");
        return false;
    }
    return false;
}

bool StreamParser::finish_call(std::vector<Event>* events) {
    if (!valid_call()) return false;
    emit_arguments(argument_count_ == 0 ? "{}" : "}", events);
    calls_.push_back(current_);
    events->push_back(Event{EventType::CallEnd, current_.index, current_.id,
                            current_.name, current_.arguments});
    current_ = ToolCall{};
    argument_count_ = 0;
    return true;
}

std::vector<std::string> StreamParser::schema_types(
    const std::string& function, const std::string& parameter) const {
    for (const auto& tool : tools_) {
        if (!tool.is_object() || !tool.contains("function") ||
            !tool["function"].is_object())
            continue;
        const json& fn = tool["function"];
        if (!fn.contains("name") || !fn["name"].is_string() ||
            fn["name"].get_ref<const std::string&>() != function ||
            !fn.contains("parameters") || !fn["parameters"].is_object() ||
            !fn["parameters"].contains("properties") ||
            !fn["parameters"]["properties"].is_object() ||
            !fn["parameters"]["properties"].contains(parameter))
            continue;
        std::vector<std::string> result;
        collect_schema_types(fn["parameters"]["properties"][parameter], &result);
        return result;
    }
    return {};
}

std::vector<Event> StreamParser::pump(bool final) {
    std::vector<Event> events;
    if (!enabled_) {
        emit_content(std::move(buffer_), &events);
        buffer_.clear();
        return events;
    }

    while (true) {
        if (state_ == State::Broken) {
            partial_ = true;
            if (final) buffer_.clear();
            break;
        }

        if (state_ == State::Text) {
            const size_t marker = buffer_.find(kToolOpen);
            if (marker == std::string::npos) {
                if (final) {
                    emit_content(std::move(buffer_), &events);
                    buffer_.clear();
                } else {
                    const size_t partial = suffix_prefix_len(buffer_, kToolOpen);
                    size_t keep_at = buffer_.size() - partial;
                    while (keep_at > 0 && ascii_space(buffer_[keep_at - 1])) --keep_at;
                    emit_content(buffer_.substr(0, keep_at), &events);
                    buffer_.erase(0, keep_at);
                }
                break;
            }

            size_t content_end = marker;
            while (content_end > 0 && ascii_space(buffer_[content_end - 1])) --content_end;
            emit_content(buffer_.substr(0, content_end), &events);
            buffer_.erase(0, content_end);
            const size_t open_at = marker - content_end;
            size_t at = open_at + std::char_traits<char>::length(kToolOpen);
            while (at < buffer_.size() && ascii_space(buffer_[at])) ++at;
            const std::string_view rest(buffer_.data() + at, buffer_.size() - at);
            if (rest.size() < std::char_traits<char>::length(kFunctionOpen)) {
                if (!final && is_prefix_of(rest, kFunctionOpen)) break;
                state_ = State::Broken;
                continue;
            }
            if (rest.substr(0, std::char_traits<char>::length(kFunctionOpen)) != kFunctionOpen) {
                state_ = State::Broken;
                continue;
            }
            at += std::char_traits<char>::length(kFunctionOpen);
            const size_t close = buffer_.find('>', at);
            const size_t newline = buffer_.find_first_of("\r\n", at);
            if (close == std::string::npos) {
                if (!final && newline == std::string::npos) break;
                state_ = State::Broken;
                continue;
            }
            if (close == at || (newline != std::string::npos && newline < close)) {
                state_ = State::Broken;
                continue;
            }
            if (!function_exists(tools_, buffer_.substr(at, close - at))) {
                const std::string name = buffer_.substr(at, close - at);
                set_error("Unknown tool function: " + diagnostic_name(name), "unknownTool");
                error_details_["tool"] = bounded_text(name);
                state_ = State::Broken;
                continue;
            }
            start_call(buffer_.substr(at, close - at), &events);
            buffer_.erase(0, close + 1);
            state_ = State::Between;
            continue;
        }

        if (state_ == State::Between) {
            size_t ws = 0;
            while (ws < buffer_.size() && ascii_space(buffer_[ws])) ++ws;
            buffer_.erase(0, ws);
            if (buffer_.empty()) {
                if (final) state_ = State::Broken;
                break;
            }
            if (buffer_.rfind(kFunctionClose, 0) == 0) {
                buffer_.erase(0, std::char_traits<char>::length(kFunctionClose));
                state_ = State::ToolTail;
                continue;
            }
            if (buffer_.rfind(kParameterOpen, 0) == 0) {
                const size_t begin = std::char_traits<char>::length(kParameterOpen);
                const size_t close = buffer_.find('>', begin);
                const size_t newline = buffer_.find_first_of("\r\n", begin);
                if (close == std::string::npos) {
                    if (!final && newline == std::string::npos) break;
                    state_ = State::Broken;
                    continue;
                }
                if (close == begin || (newline != std::string::npos && newline < close)) {
                    state_ = State::Broken;
                    continue;
                }
                std::string key = buffer_.substr(begin, close - begin);
                buffer_.erase(0, close + 1);
                state_ = start_parameter(std::move(key), &events) ? State::Parameter : State::Broken;
                continue;
            }
            if (!final && (is_prefix_of(buffer_, kFunctionClose) ||
                           is_prefix_of(buffer_, kParameterOpen)))
                break;
            state_ = State::Broken;
            continue;
        }

        if (state_ == State::Parameter) {
            if (!value_started_) {
                if (buffer_.empty() || (buffer_[0] == '\r' && buffer_.size() == 1))
                    break;
                if (buffer_.rfind("\r\n", 0) == 0) buffer_.erase(0, 2);
                else if (buffer_[0] == '\n') buffer_.erase(0, 1);
                else {
                    state_ = State::Broken;
                    continue;
                }
                value_started_ = true;
            }
            size_t close = buffer_.find(kParameterClose);
            size_t close_len = std::char_traits<char>::length(kParameterClose);
            const size_t crlf_close = buffer_.find(kParameterCloseCrlf);
            if (crlf_close != std::string::npos &&
                (close == std::string::npos || crlf_close < close)) {
                close = crlf_close;
                close_len = std::char_traits<char>::length(kParameterCloseCrlf);
            }
            if (close != std::string::npos) {
                std::string raw = buffer_.substr(0, close);
                buffer_.erase(0, close + close_len);
                finish_parameter(std::move(raw), &events);
                state_ = State::Between;
                continue;
            }
            if (!stream_parameter_) {
                if (final) state_ = State::Broken;
                break;
            }

            if (final) {
                emit_arguments(escape_json_fragment(buffer_), &events);
                buffer_.clear();
                partial_ = true;
                break;
            }
            const size_t partial = std::max(suffix_prefix_len(buffer_, kParameterClose),
                                            suffix_prefix_len(buffer_, kParameterCloseCrlf));
            const size_t safe = buffer_.size() - partial;
            if (safe == 0) break;
            emit_arguments(escape_json_fragment(buffer_.substr(0, safe)), &events);
            buffer_.erase(0, safe);
            continue;
        }

        if (state_ == State::ToolTail) {
            size_t ws = 0;
            while (ws < buffer_.size() && ascii_space(buffer_[ws])) ++ws;
            buffer_.erase(0, ws);
            if (buffer_.rfind(kToolClose, 0) == 0) {
                buffer_.erase(0, std::char_traits<char>::length(kToolClose));
                state_ = finish_call(&events) ? State::Text : State::Broken;
                continue;
            }
            if (!final && is_prefix_of(buffer_, kToolClose)) break;
            state_ = State::Broken;
            continue;
        }
    }
    if (state_ == State::Broken || (final && state_ != State::Text)) partial_ = true;
    if (partial_ && error_.empty())
        set_error("Invalid or unfinished tool-call framing" +
                  (current_.name.empty() ? std::string() : " for tool " + diagnostic_name(current_.name)) +
                  ".", "framing");
    return events;
}

}  // namespace toolparse
