// Live telemetry must not keep a stalled generation alive indefinitely.
#include "engine_client.h"
#include <chrono>
#include <cstdio>
#ifdef _WIN32
#include "../src/gpu/os_win32.h"
#endif
int main(int argc, char** argv) {
#ifdef _WIN32
    wsa_init();
#endif
    if (argc != 3) return 2;
    gdec::EngineClient engine;
    std::string error;
    if (!engine.connect(argv[1], &error, 1)) return 3;
    engine.set_timeouts(.3, .3);
    gdec::GenParams p;
    p.req = 7; p.ids = {1,2,3,4,5,6,7,8}; p.max_tokens = 8; p.progress = true;
    int progress = 0, tokens = 0;
    const auto start = std::chrono::steady_clock::now();
    const auto result = engine.generate(p, [&](int, float) { ++tokens; return true; }, {},
        [&](const gdec::GenProgress&) { ++progress; });
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    const int expected = argv[2][0]=='1' ? 1 : 0;
    if (!result.timed_out || result.transport_ok || tokens != expected || progress < 2 || elapsed > .8)
        return 4;
    printf("PASS stalled %s timed out after %.3fs despite %d progress events\n",
           expected ? "decode" : "prefill", elapsed, progress);
}
