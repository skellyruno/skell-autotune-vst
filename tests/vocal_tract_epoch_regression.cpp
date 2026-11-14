#include "PitchShifter.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>
#include <cassert>
#include <atomic>
#include <cstdlib>
#include <new>
#if defined(_WIN32)
#include <malloc.h>
#endif
namespace
{
std::atomic<bool> gTrackAllocations { false };
std::atomic<unsigned long long> gAllocationCount { 0 };

void noteAllocation() noexcept
{
    if (gTrackAllocations.load(std::memory_order_relaxed))
        gAllocationCount.fetch_add(1, std::memory_order_relaxed);
}
}

void* operator new (std::size_t size)
{
    noteAllocation();
    if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
    throw std::bad_alloc();
}
void* operator new[] (std::size_t size)
{
    noteAllocation();
    if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
    throw std::bad_alloc();
}
void operator delete (void* p) noexcept { std::free(p); }
void operator delete[] (void* p) noexcept { std::free(p); }
void operator delete (void* p, std::size_t) noexcept { std::free(p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free(p); }
void* operator new (std::size_t size, std::align_val_t alignment)
{
    noteAllocation();
    const auto align = static_cast<std::size_t>(alignment);
#if defined(_WIN32)
    if (void* p = _aligned_malloc(size == 0 ? align : size, align)) return p;
#else
    void* p = nullptr;
    if (posix_memalign(&p, align, size == 0 ? align : size) == 0) return p;
#endif
    throw std::bad_alloc();
}
void* operator new[] (std::size_t size, std::align_val_t alignment)
{
    return ::operator new(size, alignment);
}
void operator delete (void* p, std::align_val_t) noexcept {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
void operator delete[] (void* p, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete (void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete[] (void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }


#ifdef NDEBUG
#error "Source-period regression requires active assertions"
#endif
int main() {
    constexpr double pi=3.14159265358979323846,frequency=211.3,target=220;
    for(double sr:{44100.,48000.,96000.})for(bool low:{false,true})for(bool preserve:{false,true})for(double bias:{0.,-20.,20.}) {
        const double hint=frequency*std::exp2(bias/1200.);
        const int frames=static_cast<int>(sr*1.5);std::vector<float> input(static_cast<size_t>(frames));
        std::array<double,3> y1{},y2{};double peak=0;
        for(int i=0;i<frames;++i) {
            const double time=i/sr,phase=2*pi*frequency*time;double value=0;
            for(int h=1;h<=60;++h)value+=std::sin(h*phase+.07*h*h)/std::pow(h,1.2);
            const double formants[]={650+180*std::sin(2*pi*4*time),1900+350*std::sin(2*pi*1.7*time+.2),3100};
            const double bandwidths[]={90,150,220};
            for(int j=0;j<3;++j) {
                const double radius=std::exp(-pi*bandwidths[j]/sr),c=2*radius*std::cos(2*pi*formants[j]/sr);
                const double out=(1-radius)*value+c*y1[static_cast<size_t>(j)]-radius*radius*y2[static_cast<size_t>(j)];
                y2[static_cast<size_t>(j)]=y1[static_cast<size_t>(j)];y1[static_cast<size_t>(j)]=out;value=out;
            }
            input[static_cast<size_t>(i)]=static_cast<float>(value);peak=std::max(peak,std::abs(value));
        }
        for(auto& x:input)x*=static_cast<float>(.4/peak);
        humtune::PitchShifter s;s.prepare(sr,1);s.setMinFrequency(80);s.setPhaseLockedSource(true);
        s.setLowLatencyExperiment(low);s.setLowLatencyBudgetMilliseconds(1.9);s.setFormantPreserve(preserve);
        std::vector<double> periodError;double inputPower=0,power=0,outPeak=0;
        humtune::EpochWhitening cache;cache.prepare(sr);int cacheChecks=0;
        for(int i=0;i<frames;++i) {
            cache.feed(input[static_cast<size_t>(i)],i);
            if(i>sr*.4 && i%static_cast<int>(sr*.01)==0) {
                const double period=sr/frequency,previous=i-2*period,expected=previous+period+.37;
                const int span=static_cast<int>(1.5*period),radius=std::max(2,static_cast<int>(.08*period)),step=std::max(1,static_cast<int>(std::lround(sr/16000.)));
                if(cache.prepareMatch(previous,expected,span,radius,step,i)) {
                    for(int shift=-radius;shift<=radius;++shift) {
                        double xx=0,yy=0,xy=0;
                        for(int j=0;j<span;j+=step) {
                            const double a=cache.value(expected+shift-j),b=cache.value(previous-j);
                            xx+=a*a;yy+=b*b;xy+=a*b;
                        }
                        const double direct=xx>1e-12 && yy>1e-12 ? xy/std::sqrt(xx*yy) : -1.;
                        assert(std::abs(cache.matchScore(shift)-direct)<2e-12);
                    }
                    ++cacheChecks;
                }
            }
            if(low && i%static_cast<int>(sr*.004)==0)s.setSourcePitchObservation(static_cast<float>(hint),.020,.99f);
            float sample=input[static_cast<size_t>(i)];
            gTrackAllocations.store(true,std::memory_order_relaxed);
            s.process(&sample,1,static_cast<float>(sr/hint),static_cast<float>(target/hint));
            gTrackAllocations.store(false,std::memory_order_relaxed);
            assert(gAllocationCount.load(std::memory_order_relaxed)==0);
            assert(std::isfinite(sample));
            if(i>sr*.4) {
                inputPower+=input[static_cast<size_t>(i)]*static_cast<double>(input[static_cast<size_t>(i)]);power+=sample*static_cast<double>(sample);
                outPeak=std::max(outPeak,std::abs(static_cast<double>(sample)));
                if(i%static_cast<int>(sr*.002)==0) {const auto d=s.getSourceCycleDiagnostics();assert(d.period>0 && std::isfinite(d.period));periodError.push_back(std::abs(1200*std::log2(sr/(frequency*d.period))));}
            }
        }
        std::sort(periodError.begin(),periodError.end());
        std::printf("rate=%.0f low=%d formants=%d bias=%.0f count=%zu source_period_p95=%.7f p99=%.7f gain=%.7f peak=%.7f\n",sr,low,preserve,bias,periodError.size(),periodError[static_cast<size_t>(.95*(periodError.size()-1))],periodError[static_cast<size_t>(.99*(periodError.size()-1))],10*std::log10(power/inputPower),outPeak);
        std::fflush(stdout);
        assert(periodError.size()>500);
        assert(cacheChecks>30);
        assert(periodError[static_cast<size_t>(.95*(periodError.size()-1))]<(bias==0 ? 12. : 35.));
        assert(periodError[static_cast<size_t>(.99*(periodError.size()-1))]<(bias==0 ? 15. : 38.));
        assert(std::abs(10*std::log10(power/inputPower))<.5);
        assert(outPeak<.405);
    }
}
