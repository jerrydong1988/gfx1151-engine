// Bounded upstream adaptations: image padding, literal coercion, thinking state.
#include "../src/api/vision.cpp"
#include "../src/api/chat_template.h"
#include "../src/api/toolparse.h"
#include <iostream>
#include <stdexcept>

int main() {
    int cases = 0;
    auto check = [&](bool ok, const char* name) {
        if (!ok) throw std::runtime_error(name);
        ++cases;
        std::cout << "PASS " << name << '\n';
    };
    std::string error;
    for (const auto dimensions : {std::pair<int,int>{448,448}, {1000,5}, {1200,5},
                                  {4,900}, {2000,5}, {1,201}, {201,1}}) {
        vision::Image img;
        img.width = dimensions.first; img.height = dimensions.second;
        img.rgb.resize(size_t(img.width) * img.height * 3);
        for (size_t i=0; i<img.rgb.size(); ++i) img.rgb[i] = uint8_t(i % 251);
        const auto before = img;
        const auto* data = img.rgb.data();
        check(vision::letterbox_aspect(&img, &error), "aspect padding succeeds");
        check(double(std::max(img.width,img.height))/std::min(img.width,img.height) <= 200,
              "padded ratio fits resize limit");
        if (double(std::max(before.width,before.height))/std::min(before.width,before.height) <= 200)
            check(img.rgb == before.rgb && img.rgb.data() == data, "ordinary image unchanged without allocation");
        const int ox=(img.width-before.width)/2, oy=(img.height-before.height)/2;
        bool exact=true;
        for (int y=0;y<img.height;++y) for (int x=0;x<img.width;++x) for (int c=0;c<3;++c) {
            const bool content=x>=ox && x<ox+before.width && y>=oy && y<oy+before.height;
            const uint8_t expected=content ? before.rgb[(size_t(y-oy)*before.width+x-ox)*3+c] : 127;
            exact &= img.rgb[(size_t(y)*img.width+x)*3+c] == expected;
        }
        check(exact, "all original pixels retained and only padding is gray");
        int h=0,w=0;
        check(vision::smart_resize(img.height,img.width,&h,&w,&error) && h>0 && w>0,
              "padded dimensions accepted by smart resize");
    }
    vision::Image excessive;
    excessive.width=100000000; excessive.height=1; // no allocation before rejection
    check(!vision::letterbox_aspect(&excessive,&error) && excessive.rgb.empty(),
          "padding cannot exceed decoded pixel budget");

    using json=toolparse::json;
    json schema={{"type","object"},{"properties",{{"new_string",{{"type","string"}}},
            {"payload",{{"type","object"}}},{"items",{{"type","array"}}}}},
            {"required",json::array({"new_string"})},{"additionalProperties",false}};
    json function={{"name","edit"},{"parameters",schema}};
    json definitions=json::array({{{"type","function"},{"function",function}}});
    auto parse=[&](const std::string& field, const std::string& value, bool required=true) {
        toolparse::StreamParser parser(definitions,[]{return "call_test";});
        std::string input="<tool_call>\n<function=edit>\n";
        if(required) input+="<parameter=new_string>\nTrue False None\n</parameter>\n";
        input+="<parameter="+field+">\n"+value+"\n</parameter>\n</function>\n</tool_call>";
        for(char c:input) parser.feed(std::string(1,c));
        parser.finish();
        return parser;
    };
    auto repaired=parse("payload",R"({"a":True,"b":[False,None],"s":"True \"False\" None"})");
    check(repaired.calls().size()==1 && repaired.error().empty(), "nested Python literals accepted in streamed object");
    auto args=json::parse(repaired.calls()[0].arguments);
    check(args["payload"]["a"]==true && args["payload"]["b"]==json::array({false,nullptr}) &&
          args["payload"]["s"]=="True \"False\" None" && args["new_string"]=="True False None",
          "typed literals normalized while escaped and plain strings remain exact");
    auto array=parse("items",R"([True,False,None,{"value":"None"}])");
    check(array.calls().size()==1 && json::parse(array.calls()[0].arguments)["items"][2].is_null(),
          "nested array literals accepted");
    for(const auto bad : {"{'a': True}", "{\"a\": TrueXYZ}", "{\"a\": _True}", "{\"a\": True,}", "[]"}) {
        auto rejected=parse("payload",bad);
        check(rejected.calls().empty() && rejected.has_partial_call(), "malformed or wrong-type objects still rejected");
    }
    auto missing=parse("payload",R"({"a":True})",false);
    check(missing.calls().empty() && missing.error_details()["keyword"]=="required" &&
          missing.error_details()["missing_property"]=="new_string", "coercion never invents missing required arguments");
    auto extra=parse("unknown", "True");
    check(extra.calls().empty() && extra.error_details()["keyword"]=="additionalProperties",
          "undeclared parameters remain forbidden");

    using tj=chat_template::json;
    const tj messages=tj::array({{{"role","user"},{"content","Hello"}}}), yes=true, no=false, null=nullptr;
    chat_template::Options opts; opts.add_generation_prompt=&yes;
    for(const tj* flag : {static_cast<const tj*>(nullptr), &yes, &no, &null}) {
        opts.enable_thinking=flag;
        auto rendered=chat_template::render_chat_template(&messages,opts);
        const bool enabled=flag!=&no;
        const std::string tail=enabled ? "<|im_start|>assistant\n<think>\n" : "<|im_start|>assistant\n<think>\n\n</think>\n\n";
        check(rendered.ok && rendered.thinking_enabled==enabled &&
              rendered.text.size()>=tail.size() && rendered.text.compare(rendered.text.size()-tail.size(),tail.size(),tail)==0,
              "reported thinking state matches actual prompt tail");
    }
    std::cout << "RESULT PASS cases=" << cases << '\n';
}
