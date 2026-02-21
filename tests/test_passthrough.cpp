#include "hexcaster/pipeline.h"
#include "hexcaster/gain_stage.h"
#include "hexcaster/param_registry.h"

#include <cstdio>
#include <cmath>
#include <cstring>

// Simple assertion helper -- no external test framework.
static int gFailures = 0;

#define CHECK(expr, msg)                                                \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::fprintf(stderr, "FAIL [%s:%d]: %s\n",                 \
                         __FILE__, __LINE__, msg);                      \
            ++gFailures;                                                \
        }                                                               \
    } while (0)

// ----------------------------------------------------------------------------
// Test: Unity gain passthrough
//   Pipeline with a single GainStage at 0 dB.
//   Output must equal input within floating-point tolerance.
// ----------------------------------------------------------------------------
static void testUnityPassthrough()
{
    static constexpr int   kBlockSize  = 128;
    static constexpr float kSampleRate = 48000.f;
    static constexpr float kTolerance  = 1e-5f;

    hexcaster::GainStage gain;
    gain.setGainDb(0.f); // unity

    hexcaster::Pipeline pipeline;
    pipeline.addStage(&gain);
    pipeline.prepare(kSampleRate, kBlockSize);

    // Fill a reference buffer with a ramp [0, 1)
    float reference[kBlockSize];
    for (int i = 0; i < kBlockSize; ++i) {
        reference[i] = static_cast<float>(i) / static_cast<float>(kBlockSize);
    }

    // Copy to processing buffer
    float buffer[kBlockSize];
    std::memcpy(buffer, reference, sizeof(buffer));

    pipeline.process(buffer, kBlockSize);

    // After a full block the smoother should have converged to unity (1.0).
    // The first few samples may be slightly off due to smoother startup --
    // check the latter half of the block where convergence is guaranteed.
    for (int i = kBlockSize / 2; i < kBlockSize; ++i) {
        const float diff = std::fabs(buffer[i] - reference[i]);
        CHECK(diff < kTolerance, "Unity gain output deviates from input");
    }

    std::printf("testUnityPassthrough: %s\n", gFailures == 0 ? "PASS" : "FAIL");
}

// ----------------------------------------------------------------------------
// Test: GainStage scales by expected factor
// ----------------------------------------------------------------------------
static void testGainScaling()
{
    static constexpr int   kBlockSize  = 128;
    static constexpr float kSampleRate = 48000.f;
    static constexpr float kGainDb     = 6.f;
    static constexpr float kExpected   = 1.99526f; // 10^(6/20)
    static constexpr float kTolerance  = 1e-3f;

    hexcaster::GainStage gain;
    gain.setGainDb(kGainDb);

    hexcaster::Pipeline pipeline;
    pipeline.addStage(&gain);
    pipeline.prepare(kSampleRate, kBlockSize);

    // All-ones buffer
    float buffer[kBlockSize];
    for (int i = 0; i < kBlockSize; ++i) buffer[i] = 1.f;

    pipeline.process(buffer, kBlockSize);

    // Check last sample (smoother fully settled)
    const float diff = std::fabs(buffer[kBlockSize - 1] - kExpected);
    CHECK(diff < kTolerance, "+6 dB gain output is not ~1.995");

    std::printf("testGainScaling:       %s\n", gFailures == 0 ? "PASS" : "FAIL");
}

// ----------------------------------------------------------------------------
// Test: ParamRegistry stores and retrieves values
// ----------------------------------------------------------------------------
static void testParamRegistry()
{
    hexcaster::ParamRegistry registry;

    // Default master gain should be 0 dB
    const float def = registry.get(hexcaster::ParamId::MasterGain_dB);
    CHECK(std::fabs(def) < 1e-6f, "Default MasterGain_dB is not 0");

    // Set and retrieve
    registry.set(hexcaster::ParamId::MasterGain_dB, 12.f);
    const float val = registry.get(hexcaster::ParamId::MasterGain_dB);
    CHECK(std::fabs(val - 12.f) < 1e-6f, "MasterGain_dB not stored correctly");

    // Out-of-range clamp (max is 24 dB)
    registry.set(hexcaster::ParamId::MasterGain_dB, 999.f);
    const float clamped = registry.get(hexcaster::ParamId::MasterGain_dB);
    CHECK(clamped <= 24.f, "MasterGain_dB not clamped to max");

    std::printf("testParamRegistry:     %s\n", gFailures == 0 ? "PASS" : "FAIL");
}

// ----------------------------------------------------------------------------

int main()
{
    std::printf("--- HexCaster passthrough tests ---\n");

    testUnityPassthrough();
    testGainScaling();
    testParamRegistry();

    std::printf("---\n");
    if (gFailures == 0) {
        std::printf("All tests PASSED.\n");
        return 0;
    } else {
        std::printf("%d test(s) FAILED.\n", gFailures);
        return 1;
    }
}
