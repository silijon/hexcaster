#include "hexcaster/input_gain.h"
#include "hexcaster/midi_map.h"
#include "hexcaster/param_registry.h"
#include "NeuralAudio/NeuralModel.h"

#include <cmath>
#include <cstdio>

namespace {

bool near(float actual, float expected, float tolerance = 1.0e-5f)
{
    if (std::fabs(actual - expected) <= tolerance) return true;
    std::fprintf(stderr, "expected %.6f, got %.6f\n", expected, actual);
    return false;
}

// Exercise NeuralAudio's native recommended-adjustment implementation without
// duplicating its calibration formula in HexCaster production code.
class CalibrationProbe final : public NeuralAudio::NeuralModel {
public:
    void setModelInputLevel(float levelDBu) { modelInputLevelDBu = levelDBu; }
};

} // namespace

int main()
{
    bool ok = true;

    hexcaster::ParamRegistry params;
    hexcaster::MidiMap midi;
    midi.map(7, hexcaster::ParamId::InputGain_dB);
    midi.dispatch(7, 0, params);
    ok &= near(params.get(hexcaster::ParamId::InputGain_dB), -12.f);
    midi.dispatch(7, 64, params);
    ok &= params.get(hexcaster::ParamId::InputGain_dB) == 0.f;
    midi.dispatch(7, 127, params);
    ok &= near(params.get(hexcaster::ParamId::InputGain_dB), 12.f);

    CalibrationProbe model;
    model.SetAudioInputLevelDBu(11.63f);
    model.setModelInputLevel(8.f);
    ok &= near(model.GetRecommendedInputDBAdjustment(), 3.63f, 1.0e-4f);
    model.setModelInputLevel(14.f);
    ok &= near(model.GetRecommendedInputDBAdjustment(), -2.37f, 1.0e-4f);

    ok &= near(hexcaster::effectivePreNamGainDb(3.63f, 0.f), 3.63f);
    ok &= near(hexcaster::effectivePreNamGainDb(3.63f, -6.f), -2.37f);

    std::puts(ok ? "NAM input calibration and MIDI trim: PASS"
                 : "NAM input calibration and MIDI trim: FAIL");
    return ok ? 0 : 1;
}
