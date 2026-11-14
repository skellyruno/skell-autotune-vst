#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
namespace juce {
template<class T> T jlimit(T lo,T hi,T v){ return std::max(lo,std::min(hi,v)); }
template<class T> struct MathConstants { static constexpr T pi = static_cast<T>(3.14159265358979323846); };
template<class T> class AudioBuffer {
public:
 AudioBuffer(int channels,int samples): data(static_cast<size_t>(channels),std::vector<T>(static_cast<size_t>(samples))) {}
 int getNumSamples() const { return data.empty()?0:static_cast<int>(data[0].size()); }
 int getNumChannels() const { return static_cast<int>(data.size()); }
 T* getWritePointer(int c){return data[static_cast<size_t>(c)].data();}
 const T* getReadPointer(int c) const{return data[static_cast<size_t>(c)].data();}
private: std::vector<std::vector<T>> data;
};
}
