#include "PitchShifter.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace humtune;
static const double kPi = 3.14159265358979323846;

// Voiced-like signal: 12 harmonics with a formant-ish rolloff, optional vibrato (5.5 Hz, +/-vib cents).
static double phaseAt(double f0, double vibCents, double t, double& instHz)
{
    const double dev = vibCents / 1200.0 * std::log(2.0);
    instHz = f0 * std::exp(dev * std::sin(2 * kPi * 5.5 * t));
    // phase integral of f0*exp(dev*sin(wt)) ~ f0*(t + dev*(1-cos(wt))/w) (first order)
    const double w = 2 * kPi * 5.5;
    return 2 * kPi * f0 * (t + dev * (1 - std::cos(w * t)) / w);
}

struct M { double pitchErrCents, rmsRatio, click; };

M run(double floorHz, double f0, double ratio, double vib, double sr, int block)
{
    PitchShifter s; s.prepare(sr, block); s.setMinFrequency(floorHz); s.setFormantPreserve(true);
    const int total = static_cast<int>(sr * 1.2);
    std::vector<float> in(total), out(total);
    std::vector<double> inst(total);
    for (int i = 0; i < total; ++i) {
        const double t = i / sr; double hz; const double ph = phaseAt(f0, vib, t, hz); inst[i] = hz;
        double x = 0; for (int h = 1; h <= 12; ++h) x += std::sin(h * ph) / (h * (1.0 + 0.15 * h));
        in[i] = static_cast<float>(0.3 * x);
    }
    out = in;
    for (int o = 0; o < total; o += block) {
        const int n = std::min(block, total - o);
        s.process(out.data() + o, n, static_cast<float>(sr / inst[o + n / 2]), static_cast<float>(ratio));
    }
    const int b = static_cast<int>(sr * 0.4), e = total - static_cast<int>(sr * 0.1);
    double so = 0, si = 0, dOutSq = 0, dOutMax = 0;
    for (int i = b; i < e; ++i) { so += out[i] * out[i]; si += in[i] * in[i];
        const double d = out[i] - out[i - 1]; dOutSq += d * d; dOutMax = std::max(dOutMax, std::fabs(d)); }
    const int N = e - b;
    // pitch: sinusoidal projection sweep, fundamental only (vibrato-free runs use tight tolerance)
    const double exp = f0 * ratio; double best = -1, bestHz = exp;
    for (double hz = exp * 0.93; hz <= exp * 1.07; hz += 0.4) {
        double re = 0, im = 0; const double w = 2 * kPi * hz / sr;
        const double cs = std::cos(w), sn = std::sin(w); double pc = std::cos(w * b), ps = std::sin(w * b);
        for (int i = b; i < e; ++i) { re += out[i] * pc; im += out[i] * ps; const double nc = pc * cs - ps * sn; ps = ps * cs + pc * sn; pc = nc; }
        const double m = std::hypot(re, im); if (m > best) { best = m; bestHz = hz; }
    }
    M m; m.pitchErrCents = 1200.0 * std::log2(bestHz / exp);
    m.rmsRatio = std::sqrt(so / N) / std::sqrt(si / N);
    m.click = dOutMax / std::sqrt(dOutSq / N);
    return m;
}

int main()
{
    const double sr = 48000.0;
    const double floors[] = {80, 130, 180};
    const double mult[] = {1.0, 1.6};
    const double ratios[] = {0.89, 1.0, 1.06};
    double worstErr = 0, worstRms = 0, worstClick = 0, sumRms = 0; int cnt = 0;
    for (double F : floors) for (double mu : mult) for (double r : ratios) for (double vib : {0.0, 35.0}) {
        const double f0 = F * mu; if (f0 * r > 1000) continue;
        const M m = run(F, f0, r, vib, sr, 127);
        worstErr = std::max(worstErr, std::fabs(m.pitchErrCents));
        worstRms = std::max(worstRms, std::fabs(std::log(m.rmsRatio)));
        worstClick = std::max(worstClick, m.click); sumRms += m.rmsRatio; ++cnt;
    }
    const double levelDb = 8.686 * worstRms;
    const bool pass = worstErr < 10.0 && levelDb < 1.5 && worstClick < 20.0
                   && std::fabs(sumRms / cnt - 1.0) < 0.08;
    std::printf("standard-latency floor-edge sweep: %d cases, worst pitch err %.1f cents, worst level dev %.2f dB, "
                "worst click %.1f, mean level %.3f -> %s\n",
                cnt, worstErr, levelDb, worstClick, sumRms / cnt, pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
