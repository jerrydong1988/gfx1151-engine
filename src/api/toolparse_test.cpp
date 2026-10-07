#include "toolparse.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using toolparse::json;

namespace {

void check(bool ok, const char* name) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", name);
        std::exit(1);
    }
    std::printf("PASS %s\n", name);
}

json tools() {
    json properties = {
        {"path", {{"type", "string"}}},
        {"content", {{"type", "string"}}},
        {"mode", {{"type", "integer"}}},
        {"flag", {{"type", "boolean"}}},
        {"items", {{"type", "array"}}},
    };
    json result = json::array();
    result.push_back(
        {{"type", "function"},
         {"function",
          {{"name", "write_file"},
           {"parameters", {{"type", "object"}, {"properties", properties}}}}}});
    result.push_back(
        {{"type", "function"},
         {"function", {{"name", "read_file"}, {"parameters", {{"type", "object"}}}}}});
    return result;
}

}  // namespace

int main() {
    int ids = 0;
    auto make_id = [&]() { return "call_" + std::to_string(++ids); };
    const std::string raw =
        "answer before\n\n<tool_call>\n<function=write_file>\n"
        "<parameter=path>\n/tmp/a\n</parameter>\n"
        "<parameter=content>\nhello \\\"world\\\"\nline2\n</parameter>\n"
        "<parameter=mode>\n7\n</parameter>\n"
        "<parameter=flag>\nTRUE\n</parameter>\n"
        "<parameter=items>\n[1, 2]\n</parameter>\n"
        "</function>\n</tool_call>";

    toolparse::StreamParser whole(tools(), make_id);
    whole.feed(raw);
    whole.finish();
    check(whole.content() == "answer before", "content split");
    check(whole.calls().size() == 1, "one complete call");
    json args = json::parse(whole.calls()[0].arguments);
    check(args["path"] == "/tmp/a" && args["content"] == "hello \\\"world\\\"\nline2",
          "string arguments");
    check(args["mode"] == 7 && args["flag"] == true && args["items"].size() == 2,
          "schema coercion");

    ids = 0;
    toolparse::StreamParser bytewise(tools(), make_id);
    std::string streamed_args;
    bool opened_before_close = false;
    for (char c : raw) {
        for (const auto& event : bytewise.feed(std::string(1, c))) {
            if (event.type == toolparse::EventType::ArgumentsDelta) streamed_args += event.data;
            if (event.type == toolparse::EventType::CallStart &&
                raw.find("</function>") != std::string::npos)
                opened_before_close = true;
        }
    }
    bytewise.finish();
    check(opened_before_close, "call opens incrementally");
    check(bytewise.calls().size() == 1 && streamed_args == bytewise.calls()[0].arguments,
          "streamed arguments equal final JSON");

    const std::string code = "    if (ready) {\n        write();\n    }  \n\n";
    const std::string code_call =
        "<tool_call>\n<function=write_file>\n<parameter=content>\n" + code +
        "\n</parameter>\n</function>\n</tool_call>";
    toolparse::StreamParser code_whole(tools(), make_id);
    code_whole.feed(code_call);
    code_whole.finish();
    check(code_whole.calls().size() == 1 &&
              json::parse(code_whole.calls()[0].arguments)["content"] == code,
          "code indentation and trailing whitespace preserved");
    toolparse::StreamParser code_stream(tools(), make_id);
    std::string code_deltas;
    for (char character : code_call)
        for (const auto& event : code_stream.feed(std::string(1, character)))
            if (event.type == toolparse::EventType::ArgumentsDelta) code_deltas += event.data;
    for (const auto& event : code_stream.finish())
        if (event.type == toolparse::EventType::ArgumentsDelta) code_deltas += event.data;
    check(code_stream.calls().size() == 1 &&
              code_deltas == code_stream.calls()[0].arguments &&
              json::parse(code_deltas)["content"] == code,
          "bytewise code arguments match final content");

    json union_tools = tools();
    union_tools[0]["function"]["parameters"]["properties"]["content"]["type"] =
        json::array({"integer", "string"});
    toolparse::StreamParser union_parser(union_tools, make_id);
    union_parser.feed("<tool_call>\n<function=write_file>\n"
                      "<parameter=content>\n  text  \n</parameter>\n"
                      "</function>\n</tool_call>");
    union_parser.finish();
    check(union_parser.calls().size() == 1 &&
              json::parse(union_parser.calls()[0].arguments)["content"] == "  text  ",
          "union string argument retains whitespace");

    const std::string inline_tag =
        "<tool_call>\n<function=write_file>\n<parameter=content>\n"
        "const tag = \"</parameter>\";\n</parameter>\n</function>\n</tool_call>";
    toolparse::StreamParser inline_parser(tools(), make_id);
    inline_parser.feed(inline_tag);
    inline_parser.finish();
    check(inline_parser.calls().size() == 1 &&
              json::parse(inline_parser.calls()[0].arguments)["content"] ==
                  "const tag = \"</parameter>\";",
          "inline closing tag remains file content");

    const std::string ambiguous_tag =
        "<tool_call>\n<function=write_file>\n<parameter=content>\n"
        "before\n</parameter>\nafter\n</parameter>\n</function>\n</tool_call>";
    toolparse::StreamParser ambiguous(tools(), make_id);
    ambiguous.feed(ambiguous_tag);
    ambiguous.finish();
    check(ambiguous.has_partial_call() && ambiguous.calls().empty(),
          "ambiguous closing line does not return a truncated call");

    const std::string crlf_call =
        "<tool_call>\r\n<function=write_file>\r\n<parameter=content>\r\n"
        "  line\r\n\r\n</parameter>\r\n</function>\r\n</tool_call>";
    toolparse::StreamParser crlf(tools(), make_id);
    for (char character : crlf_call) crlf.feed(std::string(1, character));
    crlf.finish();
    check(crlf.calls().size() == 1 &&
              json::parse(crlf.calls()[0].arguments)["content"] == "  line\r\n",
          "CRLF framing preserves content");

    ids = 0;
    toolparse::StreamParser partial(tools(), make_id);
    std::string partial_args;
    const std::string unfinished =
        "<tool_call>\n<function=write_file>\n<parameter=content>\n" +
        std::string(200, 'x');
    for (const auto& event : partial.feed(unfinished))
        if (event.type == toolparse::EventType::ArgumentsDelta) partial_args += event.data;
    for (const auto& event : partial.finish())
        if (event.type == toolparse::EventType::ArgumentsDelta) partial_args += event.data;
    check(partial.has_partial_call() && partial.calls().empty(), "incomplete call state");
    check(partial_args.size() > 150, "incomplete string streams early");

    toolparse::StreamParser empty_partial(tools(), make_id);
    empty_partial.feed("<tool_call>\n<function=write_file>\n<parameter=content>\n");
    empty_partial.finish();
    check(empty_partial.has_partial_call() && empty_partial.calls().empty(),
          "empty unfinished parameter is incomplete");

    toolparse::StreamParser missing_tail(tools(), make_id);
    missing_tail.feed("<tool_call>\n<function=read_file>\n</function>\n");
    missing_tail.finish();
    check(missing_tail.has_partial_call() && missing_tail.calls().empty(),
          "missing tool close is incomplete");

    toolparse::StreamParser partial_second(tools(), make_id);
    partial_second.feed("<tool_call>\n<function=read_file>\n</function>\n</tool_call>"
                        "<tool_call>\n<function=write_file>\n<parameter=content>\npartial");
    partial_second.finish();
    check(partial_second.calls().size() == 1 && partial_second.has_partial_call(),
          "later incomplete call does not hide a partial result");

    json required_tools = tools();
    required_tools[0]["function"]["parameters"]["required"] =
        json::array({"path", "content"});
    toolparse::StreamParser missing_required(required_tools, make_id);
    missing_required.feed("<tool_call>\n<function=write_file>\n"
                          "<parameter=path>\n/tmp/a\n</parameter>\n"
                          "</function>\n</tool_call>");
    missing_required.finish();
    check(missing_required.has_partial_call() && missing_required.calls().empty(),
          "missing required argument is not returned as a complete call");

    toolparse::StreamParser unknown_tool(tools(), make_id);
    unknown_tool.feed("<tool_call>\n<function=delete_file>\n</function>\n</tool_call>");
    unknown_tool.finish();
    check(unknown_tool.has_partial_call() && unknown_tool.calls().empty(),
          "unknown function is not returned as a complete call");

    json messages = json::array(
        {{{"role", "assistant"},
          {"tool_calls",
           json::array({{{"id", "a"},
                         {"function", {{"name", "write_file"}, {"arguments", "{\"mode\":7}"}}}},
                        {{"id", "b"},
                         {"function", {{"name", "read_file"}, {"arguments", "{}"}}}}})}},
         {{"role", "tool"}, {"tool_call_id", "b"}, {"content", "second"}},
         {{"role", "tool"}, {"tool_call_id", "a"}, {"content", "first"}}});
    std::string error;
    check(toolparse::normalize_messages(&messages, &error), "normalize history");
    check(messages[0]["tool_calls"][0]["function"]["arguments"].is_object(),
          "history arguments parsed");
    check(messages[1]["tool_call_id"] == "a" && messages[2]["tool_call_id"] == "b",
          "tool results reordered");

    toolparse::ToolChoice choice;
    json required = "required";
    check(toolparse::parse_tool_choice(&required, json::array({tools()[0]}), &choice, &error) &&
              choice.name == "write_file" && !choice.parser_prefix().empty(),
          "required single choice");
    json named = {{"type", "function"}, {"function", {{"name", "read_file"}}}};
    check(toolparse::parse_tool_choice(&named, tools(), &choice, &error) &&
              choice.name == "read_file",
          "named choice");


    check(unknown_tool.error().find("delete_file") != std::string::npos,
          "unknown tool diagnostic names the rejected function");
    check(missing_required.error().find("content") != std::string::npos,
          "missing argument diagnostic identifies the schema property");
    auto validate = [&](const json& schema, const std::string& value, bool accepted,
                        const char* name) {
        json defs = json::array({{{"type", "function"}, {"function", {
            {"name", "submit"}, {"parameters", {{"type", "object"},
            {"properties", {{"result", schema}}}, {"required", json::array({"result"})},
            {"additionalProperties", false}}}}}}});
        std::string call = "<tool_call>\n<function=submit>\n<parameter=result>\n" + value +
                           "\n</parameter>\n</function>\n</tool_call>";
        for (bool bytes : {false, true}) {
            toolparse::StreamParser parser(defs, make_id);
            if (bytes) for (char c : call) parser.feed(std::string(1, c));
            else parser.feed(call);
            parser.finish();
            check((parser.calls().size() == 1) == accepted &&
                  parser.has_partial_call() != accepted, name);
            if (!accepted) check(!parser.error().empty(), "invalid argument has diagnostic");
        }
    };
    validate({{"type","integer"}}, "not-an-integer", false, "reject invalid integer");
    validate({{"type","integer"}}, "7", true, "accept coerced integer");
    validate({{"type","boolean"}}, "maybe", false, "reject invalid boolean");
    validate({{"type","number"}}, "nan", false, "reject nonfinite number");
    validate({{"type","number"}}, "9223372036854775808", true, "large number avoids signed cast overflow");
    validate({{"type","string"},{"enum",json::array({"test","replace"})}}, "delete", false, "reject bad enum");
    validate({{"type","string"},{"enum",json::array({"test","replace"})}}, "test", true, "accept enum");
    validate({{"type","object"},{"properties",{{"n",{{"type","integer"}}}}},
              {"required",json::array({"n"})},{"additionalProperties",false}},
             "{\"n\":\"secret-value\"}", false, "reject nested wrong type");
    validate({{"type","object"},{"properties",{{"n",{{"type","integer"}}}}},
              {"additionalProperties",false}}, "{\"n\":2,\"extra\":3}", false, "reject additional property");
    validate({{"type","array"},{"items",{{"type","integer"}}}}, "[1,\"bad\"]", false, "reject wrong array item");
    validate({{"type","array"},{"items",{{"type","integer"}}}}, "[1,2]", true, "accept array items");
    validate({{"type","object"},{"properties",{{"n",{{"oneOf",json::array({json{{"type","integer"}},json{{"type","boolean"}}})}}}}}},
             "{\"n\":true}", true, "accept oneOf branch");
    validate({{"type","object"},{"properties",{{"n",{{"oneOf",json::array({json{{"type","integer"}},json{{"type","boolean"}}})}}}}}},
             "{\"n\":\"bad\"}", false, "reject unmatched oneOf");
    validate({{"type","object"},{"oneOf",json::array({json{{"$ref","#/$defs/a"}},json{{"$ref","#/$defs/b"}}})}},
             "{}", true, "unresolved refs do not produce false oneOf rejection");

    // Recorded Harness failure: a valid call followed by present.files[2].status.
    json present = json::parse(R"({"type":"function","function":{"name":"present",
        "parameters":{"type":"object","properties":{"files":{"type":"array",
            "items":{"type":"object","additionalProperties":false,
                "properties":{"path":{"type":"string"},"description":{"type":"string"}},
                "required":["path"]}}},"required":["files"]}}})");
    json diagnostic_tools = tools();
    diagnostic_tools.push_back(present);
    const std::string good_call = "<tool_call>\n<function=read_file>\n</function>\n</tool_call>";
    auto present_call = [](const json& files) {
        return "<tool_call>\n<function=present>\n<parameter=files>\n" + files.dump() +
               "\n</parameter>\n</function>\n</tool_call>";
    };
    json files = json::array({json{{"path", "secret-value-a"}},
                              json{{"path", "secret-value-b"}},
                              json{{"path", "secret-value-c"}, {"status", "secret-value"}}});
    for (bool bytes : {false, true}) {
        toolparse::StreamParser parser(diagnostic_tools, make_id);
        const std::string input = good_call + present_call(files);
        if (bytes) for (char c : input) parser.feed(std::string(1, c));
        else parser.feed(input);
        parser.finish();
        const json& d = parser.error_details();
        check(parser.has_partial_call() && parser.calls().size() == 1 &&
              d["tool"] == "present" && d["call_index"] == 1 &&
              d["path"] == "$.files[2].status" && d["keyword"] == "additionalProperties" &&
              d["unexpected_property"] == "status" &&
              d["allowed_properties"] == json::array({"path", "description"}),
              "present diagnostic identifies second call and third array item");
        check(parser.error().find("present") != std::string::npos &&
              parser.error().find("undeclared property") != std::string::npos &&
              parser.error().find("description") != std::string::npos &&
              parser.error().find("secret-value") == std::string::npos &&
              d.dump().find("secret-value") == std::string::npos,
              "readable schema diagnostic omits argument values");
    }
    files[2].erase("status");
    toolparse::StreamParser corrected(diagnostic_tools, make_id);
    corrected.feed(present_call(files)); corrected.finish();
    check(!corrected.has_partial_call() && corrected.calls().size() == 1 &&
          corrected.error().empty() && corrected.error_details().is_null() &&
          json::parse(corrected.calls()[0].arguments)["files"] == files,
          "corrected present call remains valid and unmodified");

    auto diagnose = [&](const json& schema, const std::string& input) {
        json defs = tools();
        defs[0]["function"]["parameters"]["properties"]["items"] = schema;
        toolparse::StreamParser parser(defs, make_id);
        parser.feed("<tool_call>\n<function=write_file>\n<parameter=items>\n" + input +
                    "\n</parameter>\n</function>\n</tool_call>");
        parser.finish();
        check(parser.has_partial_call() && parser.calls().empty(), "diagnostic does not accept bad call");
        return parser.error_details();
    };
    json type_error = diagnose({{"type", "array"}, {"items", {{"type", "string"}}}},
                               "[\"ok\",{\"private\":\"secret-value\"}]");
    check(type_error["path"] == "$.items[1]" && type_error["keyword"] == "type" &&
          type_error["expected_types"] == json::array({"string"}) &&
          type_error["actual_type"] == "object" &&
          type_error.dump().find("secret-value") == std::string::npos,
          "array type mismatch reports expected and actual type only");
    json required_error = diagnose({{"type", "object"}, {"required", json::array({"a.b"})}}, "{}");
    check(required_error["path"] == "$.items[\"a.b\"]" &&
          required_error["keyword"] == "required" && required_error["missing_property"] == "a.b",
          "missing property path escapes punctuation without implying nesting");
    json false_error = diagnose({{"type", "object"}, {"properties", {{"blocked", false}}}},
                                "{\"blocked\":\"secret-value\"}");
    check(false_error["path"] == "$.items.blocked" && false_error["keyword"] == "falseSchema",
          "false property schema is distinct from an undeclared property");
    json allowed_error = diagnose({{"type", "object"}, {"additionalProperties", false},
        {"properties", {{"blocked", false}, {"path", {{"type", "string"}}}}}}, "{\"extra\":1}");
    check(allowed_error["allowed_properties"] == json::array({"path"}),
          "allowed property hint excludes explicitly forbidden properties");
    json many_properties = json::object();
    for (int i = 0; i < 20; ++i)
        many_properties[std::to_string(i) + std::string(200, 'x')] = {{"type", "string"}};
    json bounded = diagnose({{"type", "object"}, {"properties", many_properties},
                             {"additionalProperties", false}}, "{\"extra\":\"secret-value\"}");
    check(bounded["allowed_properties"].size() == 8 &&
          bounded["allowed_properties_truncated"] == true && bounded.dump().size() < 2000,
          "large schema diagnostics have bounded property names and count");
    toolparse::StreamParser duplicate(tools(), make_id);
    duplicate.feed("<tool_call>\n<function=write_file>\n<parameter=path>\na\n</parameter>\n"
                   "<parameter=path>\nb\n</parameter>\n</function>\n</tool_call>");
    duplicate.finish();
    check(duplicate.calls().empty() && duplicate.has_partial_call() &&
          duplicate.error().find("Duplicate") != std::string::npos &&
          duplicate.error_details()["tool"] == "write_file" &&
          duplicate.error_details()["keyword"] == "duplicateParameter" &&
          duplicate.error_details()["path"] == "$.path",
          "duplicate XML parameters are rejected instead of last-key-wins");

    toolparse::StreamParser unicode_name(tools(), make_id);
    unicode_name.feed("<tool_call>\n<function=" + std::string(127, 'x') +
                      "\xe4\xb8\xad>\n</function>\n</tool_call>");
    unicode_name.finish();
    check(unicode_name.has_partial_call() && unicode_name.error().size() < 200,
          "bounded UTF-8 diagnostic does not split or throw");
    std::puts("RESULT PASS");
    return 0;
}
