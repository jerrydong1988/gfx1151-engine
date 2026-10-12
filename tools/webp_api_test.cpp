// Link with the API translation units except main.cpp (included for normalization).
#define main gdec_api_real_main
#include "../src/api/main.cpp"
#undef main
#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    if (argc != 2) return 2;
    const std::string root = argv[1];
    int cases = 0;
    auto check = [&](bool ok, const std::string& name) {
        if (!ok) { std::cerr << "FAIL " << name << '\n'; std::exit(1); }
        std::cout << "PASS " << name << '\n'; ++cases;
    };
    auto bytes = [&](const std::string& name) {
        std::ifstream f(root + "/" + name, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
    };
    std::string error;
    for (const auto& name : {"lossless", "lossy", "alpha", "jpeg", "png"}) {
        vision::Frame actual, reference;
        check(vision::preprocess(bytes(std::string(name) + ".img"), &actual, &error),
              std::string(name) + " decode: " + error);
        check(vision::preprocess(bytes(std::string(name) + ".png"), &reference, &error),
              std::string(name) + " reference decode");
        float max_diff = 0;
        if (actual.patches.size() == reference.patches.size())
            for (size_t i = 0; i < actual.patches.size(); ++i)
                max_diff = std::max(max_diff, std::abs(actual.patches[i] - reference.patches[i]));
        check(actual.grid == reference.grid && actual.patches.size() == reference.patches.size() &&
              max_diff <= (std::string(name) == "jpeg" ? 0.04f : 1e-6f),
              std::string(name) + " patches match independent RGB decode");
    }
    for (const auto& name : {"truncated", "corrupt", "animated", "oversized"}) {
        vision::Frame frame;
        check(!vision::preprocess(bytes(std::string(name) + ".webp"), &frame, &error) &&
              frame.patches.empty(), std::string(name) + " rejected without patches");
        if (std::string(name) == "animated")
            check(error.find("animated WebP") != std::string::npos, "explicit animation error");
        if (std::string(name) == "oversized")
            check(error.find("too large") != std::string::npos, "dimensions checked before allocation");
    }
    const json cap = vision::capabilities();
    check(cap["formats"] == json::array({"png", "jpeg", "webp"}) &&
          cap["animated_webp"] == false && cap["webp_decoder"] == "libwebp" &&
          cap["webp_decoder_version"].is_string(), "compiled decoder capabilities");
    std::ifstream fixture(root + "/messages.json");
    json input; fixture >> input;
    const json normalized = normalize_responses_input(input);
    check(normalized.size() == 3 && normalized[2]["role"] == "tool" &&
          normalized[2]["content"].size() == 3, "Responses preserves mixed tool output");
    const auto prompt = chat_template::render_chat_template(&normalized, {});
    check(prompt.ok && prompt.text.find("before<|vision_start|><|image_pad|><|vision_end|>after") !=
          std::string::npos, "tool image and adjacent text retain order in template");
    std::vector<vision::Frame> frames;
    check(vision::prepare_messages(normalized, &frames, &error) && frames.size() == 1,
          "Responses tool image reaches vision preprocessing");
    auto history = normalized;
    history.push_back({{"role", "assistant"}, {"content", "Blue"}});
    history.push_back({{"role", "user"}, {"content", "Remember the image?"}});
    check(vision::prepare_messages(history, &frames, &error) && frames.size() == 1,
          "multi-turn tool image retained");
    const auto replay = chat_template::render_chat_template(&history, {});
    check(replay.ok && replay.text.find("<|image_pad|>") != std::string::npos,
          "multi-turn template retains image placeholder");
    for (const auto& output : {json("text result"), json::array({{{"type", "input_text"}, {"text", "text result"}}})}) {
        auto text = input; text["input"][2]["output"] = output;
        auto result = normalize_responses_input(text);
        check(chat_template::render_chat_template(&result, {}).ok, "text tool result preserved");
    }
    auto invalid = input; invalid["input"][2]["output"] = json::array({42});
    bool rejected = false;
    try { normalize_responses_input(invalid); } catch (const http::Error&) { rejected = true; }
    check(rejected, "invalid tool output content rejected");
    std::cout << "RESULT PASS cases=" << cases << '\n';
}
