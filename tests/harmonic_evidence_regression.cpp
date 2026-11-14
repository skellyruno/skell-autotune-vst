#include "AutoTuneEngine.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>
#ifdef NDEBUG
#error "Release regression assertions must stay active"
#endif
namespace {
std::vector<float> fixture() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/harmonic_alias_g_500ms.f32";
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  assert(f.good() && f.tellg() == std::streampos(22000 * sizeof(float)));
  f.seekg(0);
  std::vector<float> result(22000);
  f.read(reinterpret_cast<char *>(result.data()),
         static_cast<std::streamsize>(result.size() * sizeof(float)));
  assert(f.good());
  return result;
}
struct Result {
  std::vector<float> audio;
  int falseHarmonics = 0;
};
Result renderFixture(const std::vector<float> &raw, double sr, int block) {
  humtune::AutoTuneEngine e;
  e.setVoiceRange(4);
  e.setRangeProtection(true);
  e.setRootNote(7);
  e.setLowLatencyMode(true);
  e.setFormantPreserve(false);
  e.prepare(sr, block);
  struct Context {
    double sr;
    int bad = 0;
  } context{sr};
  e.setOfflineTraceCallback(
      [](void *p, const humtune::AutoTuneEngine::HopTrace &t) noexcept {
        auto &c = *static_cast<Context *>(p);
        const double time = t.sample / c.sr;
        if (time > 0.20 && time < 0.26 && t.observedHz > 400)
          ++c.bad;
      },
      &context);
  Result result;
  result.audio.resize(static_cast<size_t>(raw.size() * sr / 44100));
  for (int at = 0; at < static_cast<int>(result.audio.size()); at += block) {
    const int count =
        std::min(block, static_cast<int>(result.audio.size()) - at);
    juce::AudioBuffer<float> b(1, count);
    for (int i = 0; i < count; ++i) {
      const double position = (at + i) * 44100.0 / sr;
      const int j = static_cast<int>(position);
      const double f = position - j;
      b.getWritePointer(0)[i] = static_cast<float>(
          raw[j] * (1 - f) +
          raw[std::min(static_cast<int>(raw.size()) - 1, j + 1)] * f);
    }
    e.process(b);
    std::copy_n(b.getReadPointer(0), count, result.audio.begin() + at);
  }
  result.falseHarmonics = context.bad;
  return result;
}
// A genuine supported three-to-one register jump must still be accepted
// promptly. This audits note observation time independently of the unchanged
// audio PDC.
double trueJump(double sr) {
  humtune::AutoTuneEngine e;
  e.setVoiceRange(4);
  e.setRangeProtection(true);
  e.setScaleType(2);
  e.setLowLatencyMode(true);
  e.prepare(sr, 127);
  struct Context {
    double sr, first = -1;
  } context{sr};
  e.setOfflineTraceCallback(
      [](void *p, const humtune::AutoTuneEngine::HopTrace &t) noexcept {
        auto &c = *static_cast<Context *>(p);
        const double time = t.sample / c.sr;
        if (time >= .4 && t.observedHz > 650 && t.observedHz < 800 &&
            c.first < 0)
          c.first = time - .4;
      },
      &context);
  double phase = 0;
  for (int at = 0; at < static_cast<int>(sr * .7); at += 127) {
    const int count = std::min(127, static_cast<int>(sr * .7) - at);
    juce::AudioBuffer<float> b(1, count);
    for (int i = 0; i < count; ++i) {
      const double hz = (at + i) < sr * .4 ? 240 : 720;
      phase += 6.283185307179586 * hz / sr;
      b.getWritePointer(0)[i] = static_cast<float>(
          .2 * std::sin(phase) + .05 * std::sin(2 * phase + .3));
    }
    e.process(b);
  }
  return context.first;
}
} // namespace
int main() {
  const auto raw = fixture();
  for (double sr : {44100.0, 48000.0, 96000.0}) {
    const auto reference = renderFixture(raw, sr, 127);
    std::printf("harmonic fixture sr=%.0f false commands=%d\n", sr,
                reference.falseHarmonics);
    assert(reference.falseHarmonics == 0);
    for (int block : {1, 512})
      assert(reference.audio == renderFixture(raw, sr, block).audio);
    for (float x : reference.audio)
      assert(std::isfinite(x));
    const double seconds = trueJump(sr);
    std::printf("supported register jump sr=%.0f accepted after %.6f s\n", sr,
                seconds);
    assert(seconds >= 0 && seconds < 0.030);
  }
}
