#include "ScaleQuantizer.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace
{
bool near(float a, float b, float tolerance = 0.001f)
{
    return std::abs(a - b) <= tolerance;
}
}

int main()
{
    bool pass = true;
    auto check = [&pass](const char* label, bool ok)
    {
        std::printf("scale quantizer: %-34s %s\n", label, ok ? "PASS" : "FAIL");
        pass = pass && ok;
    };

    // Existing parameter index meanings are a compatibility contract.
    check("legacy scale indices retained",
          std::string(humtune::ScaleQuantizer::name(0)) == "Major"
       && std::string(humtune::ScaleQuantizer::name(1)) == "Minor"
       && std::string(humtune::ScaleQuantizer::name(2)) == "Chromatic");

    check("major accepts E in C", near(humtune::ScaleQuantizer::quantizeMidi(64.0f, 0, 0), 64.0f));
    check("C major resolves F# upward to G", near(humtune::ScaleQuantizer::quantizeMidi(66.0f, 0, 0), 67.0f));
    check("major pentatonic rejects F", near(humtune::ScaleQuantizer::quantizeMidi(65.0f, 0, 3), 64.0f));
    check("minor pentatonic accepts G", near(humtune::ScaleQuantizer::quantizeMidi(67.0f, 0, 4), 67.0f));
    check("whole-tone preserves F#", near(humtune::ScaleQuantizer::quantizeMidi(66.0f, 0, 9), 66.0f));
    check("root transposition crosses octave", near(humtune::ScaleQuantizer::quantizeMidi(71.0f, 2, 0), 71.0f));

    // Quarter-tone test: MIDI 60.24 should snap to the half-step 60.5, not an equal-tempered note.
    check("24-TET preserves quarter-step target",
          humtune::ScaleQuantizer::isMicrotonal(11)
       && near(humtune::ScaleQuantizer::quantizeMidi(60.26f, 0, 11), 60.5f));
    check("Rast uses E half-flat", near(humtune::ScaleQuantizer::quantizeMidi(63.1f, 0, 12), 63.5f));
    check("Sikah uses fractional scale tones", near(humtune::ScaleQuantizer::quantizeMidi(63.42f, 0, 17), 63.5f));
    check("Maqam key transposition", near(humtune::ScaleQuantizer::quantizeMidi(65.0f, 2, 13), 65.0f));

    const auto cMajor = humtune::ScaleQuantizer::approximateMask(0, 0);
    check("C major keyboard projection", cMajor[0] && cMajor[2] && cMajor[4] && cMajor[5]
                                             && cMajor[7] && cMajor[9] && cMajor[11]
                                             && !cMajor[1] && !cMajor[3]);
    const auto equal24 = humtune::ScaleQuantizer::approximateMask(11, 0);
    bool allNotes = true;
    for (bool on : equal24) allNotes = allNotes && on;
    check("24-TET projection lights 12 keys", allNotes);

    // Custom keyboard edits still resolve strictly to integer semitone keys.
    std::array<bool, 12> custom{};
    custom[0] = true;
    custom[7] = true;
    check("custom key mask remains 12-TET",
          near(humtune::ScaleQuantizer::quantizeMidiToMask(63.0f, custom), 60.0f));

    return pass ? 0 : 1;
}
