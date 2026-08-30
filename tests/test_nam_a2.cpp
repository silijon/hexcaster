#include "hexcaster/nam_stage.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: test_nam_a2 <packed-a2.nam> <a1.nam>\n");
        return 2;
    }

    hexcaster::NamStage nam;
    nam.prepare(48000.f, 4096); // Plugin-sized host block; inference is chunked to 128.
    if (!nam.loadModel(argv[1], hexcaster::NamQualityPolicy::Auto)) return 1;
    auto info = nam.modelInfo();
    if (info.variant != hexcaster::NamModelVariant::A2Lite ||
        !info.nativeStatic || info.selectedQuality != 0.f) return 1;

    std::vector<float> audio(257, 0.f);
    audio[0] = 0.1f;
    nam.process(audio.data(), static_cast<int>(audio.size()));
    if (!std::all_of(audio.begin(), audio.end(), [](float x) { return std::isfinite(x); })) return 1;

    // The old model remains active when an unsupported A1 load is rejected.
    if (nam.loadModel(argv[2], hexcaster::NamQualityPolicy::Auto)) return 1;
    if (!nam.hasModel()) return 1;

    std::puts("A2 packed-model auto selection and A1 rejection: PASS");
    return 0;
}
