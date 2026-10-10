// Include production preprocessing so its pre-allocation budget path can be
// checked with a tiny fixture, without allocating a 1.5 GiB request.
#include "../src/api/vision.cpp"
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    int cases = 0;
    auto check = [&](bool ok, const char* name) {
        if (!ok) throw std::runtime_error(name);
        ++cases;
        std::cout << "PASS " << name << '\n';
    };
    const int cap = argc == 2 ? std::atoi(argv[1]) : 32;
    check(vision::max_images() == cap, "configured image cap (including clamp)");
    const std::string url = "data:image/png;base64,"
        "iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAIAAAD8GO2jAAAANUlEQVR4nO3QsQ0AMAzDsLT//9yeoCkbeYAN6LzZdZf3x0GSKEmUJEoSJYmSREmiJFGSaMoHo8QBPwYSAhsAAAAASUVORK5CYII=";
    std::vector<uint8_t> bytes;
    std::string error;
    check(vision::decode_image_url(url, &bytes, &error), "decode fixture");
    vision::Frame reference, bounded;
    check(vision::preprocess(bytes, &reference, &error), "reference preprocess");
    const uint64_t count = reference.patches.size() / 1536;
    uint64_t used = gdec_vision_limits::kMaxPatches - count;
    check(vision::preprocess_bounded(bytes, &bounded, &error, &used) &&
          used == gdec_vision_limits::kMaxPatches && bounded.grid == reference.grid &&
          bounded.patches == reference.patches, "exact budget preserves every patch value");
    vision::Frame rejected;
    used = gdec_vision_limits::kMaxPatches - count + 1;
    const auto before = used;
    check(!vision::preprocess_bounded(bytes, &rejected, &error, &used) &&
          rejected.patches.empty() && used == before && error.find("total vision budget") != std::string::npos,
          "over budget rejects before patch allocation");
    used = 0;
    check(!gdec_vision_limits::admit_patches(UINT64_MAX, &used) && used == 0,
          "overflow input rejects without wrapping");
    used = gdec_vision_limits::kMaxPatches + 1;
    check(!gdec_vision_limits::admit_patches(1, &used), "invalid existing budget rejects");
    using vision::json;
    json messages = json::array();
    for (int i = 0; i < cap; ++i) {
        messages.push_back({{"role", "user"}, {"content", json::array({
            {{"type", "image_url"}, {"image_url", {{"url", url}}}}
        })}});
    }
    std::vector<vision::Frame> frames;
    check(vision::prepare_messages(messages, &frames, &error) && frames.size() == size_t(cap),
          "cap images accepted across message history");
    check(std::all_of(frames.begin(), frames.end(), [&](const auto& f) {
        return f.grid == reference.grid && f.patches == reference.patches;
    }), "all images preserve preprocessing precision");
    messages.push_back(messages[0]);
    check(!vision::prepare_messages(messages, &frames, &error) && frames.empty() &&
          error.find("at most " + std::to_string(cap)) != std::string::npos &&
          error.find("history") != std::string::npos, "cap plus one rejects before preprocessing");
    messages.erase(messages.end() - 1);
    check(vision::prepare_messages(messages, &frames, &error), "valid request works after rejection");
    check(vision::prepare_messages(json::array({{{"role", "user"}, {"content", "text"}}}),
                                   &frames, &error) && frames.empty(), "text path allocates no frames");
    std::cout << "RESULT PASS cases=" << cases << '\n';
}
