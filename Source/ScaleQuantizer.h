#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>

namespace humtune
{

// Scale definitions used by the target-note selector. The first three indices
// intentionally retain SkellyTune's historical APVTS values (Major=0, Minor=1,
// Chromatic=2) so sessions and automation remain compatible.
class ScaleQuantizer
{
public:
    static constexpr int kNumScales = 20;
    static constexpr int kMaxDegrees = 24;

    struct Definition
    {
        const char* name;
        std::array<float, kMaxDegrees> intervals{}; // semitones relative to the root
        int count = 0;
        bool microtonal = false;
    };

    static const std::array<Definition, kNumScales>& definitions()
    {
        static const auto table = []
        {
            std::array<Definition, kNumScales> t{};
            auto put = [&t](int index, const char* name, std::initializer_list<float> intervals,
                            bool microtonal = false)
            {
                auto& d = t[static_cast<std::size_t>(index)];
                d.name = name;
                d.count = static_cast<int>(std::min<std::size_t>(intervals.size(), kMaxDegrees));
                d.microtonal = microtonal;
                int i = 0;
                for (float v : intervals)
                    if (i < d.count) d.intervals[static_cast<std::size_t>(i++)] = v;
            };

            put(0, "Major",             {0, 2, 4, 5, 7, 9, 11});
            put(1, "Minor",             {0, 2, 3, 5, 7, 8, 10});
            put(2, "Chromatic",         {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
            put(3, "Major Pentatonic",  {0, 2, 4, 7, 9});
            put(4, "Minor Pentatonic",  {0, 3, 5, 7, 10});
            put(5, "Blues",             {0, 3, 5, 6, 7, 10});
            put(6, "Dorian",            {0, 2, 3, 5, 7, 9, 10});
            put(7, "Phrygian",          {0, 1, 3, 5, 7, 8, 10});
            put(8, "Mixolydian",        {0, 2, 4, 5, 7, 9, 10});
            put(9, "Whole Tone",        {0, 2, 4, 6, 8, 10});
            put(10, "Diminished",       {0, 2, 3, 5, 6, 8, 9, 11});
            put(11, "24-TET",            {0, .5f, 1, 1.5f, 2, 2.5f, 3, 3.5f, 4, 4.5f, 5, 5.5f,
                                          6, 6.5f, 7, 7.5f, 8, 8.5f, 9, 9.5f, 10, 10.5f, 11, 11.5f}, true);
            put(12, "Maqam Rast",        {0, 2, 3.5f, 5, 7, 9, 10.5f}, true);
            put(13, "Maqam Bayati",      {0, 1.5f, 3, 5, 7, 8, 10}, true);
            put(14, "Maqam Hijaz",       {0, 1, 4, 5, 7, 8, 10});
            put(15, "Maqam Kurd",        {0, 1, 3, 5, 7, 8, 10});
            put(16, "Maqam Saba",        {0, 1.5f, 3, 4, 7, 8, 10}, true);
            put(17, "Maqam Sikah",       {0, 1.5f, 3.5f, 4.5f, 6.5f, 8.5f, 10.5f}, true);
            put(18, "Maqam Nahawand",    {0, 2, 3, 5, 7, 8, 11});
            put(19, "Maqam Ajam",        {0, 2, 4, 5, 7, 9, 11});
            return t;
        }();
        return table;
    }

    static int clampType(int type) { return std::clamp(type, 0, kNumScales - 1); }

    static const char* name(int type)
    {
        return definitions()[static_cast<std::size_t>(clampType(type))].name;
    }

    static bool isMicrotonal(int type)
    {
        return definitions()[static_cast<std::size_t>(clampType(type))].microtonal;
    }

    // Convert a scale to the 12 note-class lights used by SkellyTune's piano.
    // For microtonal scales, this is an explanatory nearest-semitone projection;
    // the audio target still uses the exact fractional interval.
    static std::array<bool, 12> approximateMask(int type, int root)
    {
        std::array<bool, 12> mask{};
        root = ((root % 12) + 12) % 12;
        const auto& d = definitions()[static_cast<std::size_t>(clampType(type))];
        for (int i = 0; i < d.count; ++i)
        {
            const int semitones = static_cast<int>(std::floor(d.intervals[static_cast<std::size_t>(i)] + 0.5f));
            mask[static_cast<std::size_t>((root + semitones) % 12)] = true;
        }
        return mask;
    }

    static float quantizeMidi(float midiNote, int root, int type)
    {
        if (!std::isfinite(midiNote))
            return midiNote;

        root = ((root % 12) + 12) % 12;
        const auto& d = definitions()[static_cast<std::size_t>(clampType(type))];
        if (d.count <= 0)
            return midiNote;

        // Search adjacent octaves as well, so notes near C/B boundaries choose
        // the genuinely nearest pitch rather than being trapped in one octave.
        const int baseOctave = static_cast<int>(std::floor((midiNote - static_cast<float>(root)) / 12.0f));
        float best = midiNote;
        float bestDistance = 1.0e9f;
        for (int octave = baseOctave - 1; octave <= baseOctave + 1; ++octave)
        {
            const float octaveRoot = static_cast<float>(root + octave * 12);
            for (int degree = 0; degree < d.count; ++degree)
            {
                const float candidate = octaveRoot + d.intervals[static_cast<std::size_t>(degree)];
                const float distance = std::abs(candidate - midiNote);
                // Keep SkellyTune's historical tie-break: when a pitch is exactly
                // between two allowed targets, resolve upward rather than changing
                // the established Major/Minor vocal behavior.
                if (distance < bestDistance
                    || (std::abs(distance - bestDistance) < 1.0e-6f && candidate > best))
                {
                    best = candidate;
                    bestDistance = distance;
                }
            }
        }
        return best;
    }

    // Target the nearest allowed 12-TET key when the user has edited the piano.
    static float quantizeMidiToMask(float midiNote, const std::array<bool, 12>& mask)
    {
        if (!std::isfinite(midiNote))
            return midiNote;
        bool any = false;
        for (bool allowed : mask) any = any || allowed;
        if (!any) return midiNote;

        const int nearest = static_cast<int>(std::round(midiNote));
        int bestMidi = nearest;
        int bestDistanceCents = std::numeric_limits<int>::max();
        for (int candidate = nearest - 12; candidate <= nearest + 12; ++candidate)
        {
            const int pc = ((candidate % 12) + 12) % 12;
            if (!mask[static_cast<std::size_t>(pc)]) continue;
            const int distance = std::abs(candidate * 100 - static_cast<int>(std::round(midiNote * 100.0f)));
            if (distance < bestDistanceCents
                || (distance == bestDistanceCents && candidate > bestMidi))
            {
                bestMidi = candidate;
                bestDistanceCents = distance; // deterministic; ties resolve upward as before
            }
        }
        return static_cast<float>(bestMidi);
    }
};

} // namespace humtune
