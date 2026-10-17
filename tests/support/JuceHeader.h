#pragma once
// Only the JUCE primitives used by the header-only DSP. Plugin builds use JUCE.
#include <algorithm>
#include <vector>
namespace juce
{
template <typename T> T jlimit(T low, T high, T value) { return std::clamp(value, low, high); }
template <typename T> struct MathConstants { static constexpr T pi = T(3.14159265358979323846); };
template <typename T> class AudioBuffer
{
public:
    AudioBuffer(int channels, int samples) : data(static_cast<size_t>(channels), std::vector<T>(static_cast<size_t>(samples))) {}
    int getNumChannels() const { return static_cast<int>(data.size()); }
    int getNumSamples() const { return data.empty() ? 0 : static_cast<int>(data[0].size()); }
    T* getWritePointer(int channel) { return data[static_cast<size_t>(channel)].data(); }
    const T* getReadPointer(int channel) const { return data[static_cast<size_t>(channel)].data(); }
private:
    std::vector<std::vector<T>> data;
};
}
