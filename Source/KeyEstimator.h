#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace humtune
{

// Lightweight, allocation-free running tonal-centre estimator. It analyzes note-class
// duration from the live pitch detector, then correlates that histogram with standard
// major/minor tonal profiles. This produces a suggestion, not a guarantee of the song key.
class KeyEstimator
{
public:
    struct Estimate
    {
        int root = 0;                 // C=0 ... B=11
        bool minor = false;
        float confidence = 0.0f;      // heuristic 0..1; not a statistical probability
        float evidenceSeconds = 0.0f;
        float profileScore = 0.0f;
        float scoreMargin = 0.0f;
        int distinctPitchClasses = 0;
        bool valid = false;
    };

    static constexpr std::array<float, 12> kMajorProfile =
        {6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f};
    static constexpr std::array<float, 12> kMinorProfile =
        {6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f};

    void reset() noexcept { histogram.fill(0.0); }

    // Call once per detector hop, including unvoiced hops, so old evidence fades away.
    void advanceTime(double seconds) noexcept
    {
        if (!std::isfinite(seconds) || seconds <= 0.0)
            return;
        // Approx. 18 s exponential time constant: recent phrases dominate, but a normal
        // melodic phrase can accumulate enough evidence before the estimate expires.
        const double decay = std::exp(-seconds / 18.0);
        for (double& value : histogram)
            value *= decay;
    }

    void observePitch(float midiNote, float confidence, double seconds) noexcept
    {
        if (!std::isfinite(midiNote) || !std::isfinite(confidence)
            || !std::isfinite(seconds) || seconds <= 0.0 || confidence < 0.45f)
            return;

        const double confWeight = std::clamp((static_cast<double>(confidence) - 0.35) / 0.55, 0.0, 1.0);
        // Reduce to a pitch class before integer conversion: extreme but finite
        // inputs must not overflow lround/int in the audio callback.
        const double wrapped = std::fmod(std::round(static_cast<double>(midiNote)), 12.0);
        const int pc = static_cast<int>(wrapped < 0.0 ? wrapped + 12.0 : wrapped);
        histogram[static_cast<std::size_t>(pc)] += std::min(seconds, 0.05) * confWeight;
    }

    Estimate estimate() const noexcept
    {
        Estimate result;
        double total = 0.0;
        double maxClass = 0.0;
        for (double v : histogram)
        {
            total += v;
            maxClass = std::max(maxClass, v);
        }

        result.evidenceSeconds = static_cast<float>(total);
        if (total < 1.5 || maxClass <= 0.0)
            return result;

        for (double v : histogram)
            if (v >= std::max(0.04, maxClass * 0.025))
                ++result.distinctPitchClasses;
        if (result.distinctPitchClasses < 3)
            return result;

        float best = -2.0f;
        float second = -2.0f;
        int bestRoot = 0;
        bool bestMinor = false;
        for (int root = 0; root < 12; ++root)
        {
            const float majorScore = correlate(root, false);
            const float minorScore = correlate(root, true);
            const auto consider = [&](float score, int candidateRoot, bool candidateMinor,
                                      float& currentBest, float& currentSecond,
                                      int& currentRoot, bool& currentMinor)
            {
                if (score > currentBest)
                {
                    currentSecond = currentBest;
                    currentBest = score;
                    currentRoot = candidateRoot;
                    currentMinor = candidateMinor;
                }
                else if (score > currentSecond)
                {
                    currentSecond = score;
                }
            };
            consider(majorScore, root, false, best, second, bestRoot, bestMinor);
            consider(minorScore, root, true, best, second, bestRoot, bestMinor);
        }

        result.root = bestRoot;
        result.minor = bestMinor;
        result.profileScore = best;
        result.scoreMargin = best - second;
        // An easily understood heuristic combines fit and separation from the runner-up.
        result.confidence = std::clamp((best - 0.10f) * 0.60f + result.scoreMargin * 2.0f,
                                       0.0f, 1.0f);
        result.valid = best >= 0.52f && result.scoreMargin >= 0.018f
                    && result.distinctPitchClasses >= 3;
        return result;
    }

private:
    float correlate(int root, bool minor) const noexcept
    {
        const auto& profile = minor ? kMinorProfile : kMajorProfile;
        double meanHist = 0.0;
        double meanProfile = 0.0;
        for (int pc = 0; pc < 12; ++pc)
        {
            meanHist += histogram[static_cast<std::size_t>(pc)];
            meanProfile += profile[static_cast<std::size_t>((pc - root + 12) % 12)];
        }
        meanHist /= 12.0;
        meanProfile /= 12.0;

        double numerator = 0.0;
        double histPower = 0.0;
        double profilePower = 0.0;
        for (int pc = 0; pc < 12; ++pc)
        {
            const double h = histogram[static_cast<std::size_t>(pc)] - meanHist;
            const double p = profile[static_cast<std::size_t>((pc - root + 12) % 12)] - meanProfile;
            numerator += h * p;
            histPower += h * h;
            profilePower += p * p;
        }
        const double denominator = std::sqrt(histPower * profilePower);
        return denominator > 1.0e-12 ? static_cast<float>(numerator / denominator) : -1.0f;
    }

    std::array<double, 12> histogram{};
};

} // namespace humtune
