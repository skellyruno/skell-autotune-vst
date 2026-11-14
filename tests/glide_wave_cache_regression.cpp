#include <PitchShifter.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
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
#error "Gliding cache regression requires active assertions"
#endif
namespace humtune {
struct PitchShifterSpectralOracle {
    static int stream(double sr,bool low,bool preserve) {
        PitchShifter p;p.prepare(sr,1);p.setMinFrequency(35);p.setPhaseLockedSource(true);
        p.setLowLatencyExperiment(low);p.setLowLatencyBudgetMilliseconds(1.9);p.setFormantPreserve(preserve);
        double phase=0,maxError=0,peak=0;int reused=0,checked=0;
        for(int i=0;i<static_cast<int>(sr);++i) {
            const double time=i/sr,hz=45*std::exp2(.12*std::sin(6.283185307179586*3*time));
            const double target=49*std::exp2(.04*std::sin(6.283185307179586*2.3*time));
            phase+=6.283185307179586*hz/sr;
            float sample=static_cast<float>(.16*std::sin(phase)+.06*std::sin(3*phase+.2)+.03*std::sin(11*phase+.5));
            if(i%static_cast<int>(sr/40)==0)p.setFormantShiftSemitones((i/static_cast<int>(sr/40))&1 ? 4.f : -4.f);
            gTrackAllocations.store(true,std::memory_order_relaxed);
            p.process(&sample,1,static_cast<float>(sr/hz),static_cast<float>(target/hz));
            gTrackAllocations.store(false,std::memory_order_relaxed);
            assert(gAllocationCount.load(std::memory_order_relaxed)==0 && std::isfinite(sample));
            peak=std::max(peak,static_cast<double>(std::abs(sample)));
            if(i>sr*.25 && i%31==0 && p.liveSpectrumValid) {
                const double direct=p.evaluateLiveSpectrumHorner(p.livePhase,p.liveFrequency);
                const double actual=p.evaluateLiveSpectrum(p.livePhase,p.liveFrequency);
                const double error=std::abs(actual-direct);maxError=std::max(maxError,error);assert(error<3e-5);
                float cache=0;reused+=p.tryLiveWave(p.livePhase,p.liveFrequency,cache);++checked;
            }
        }
        std::printf("glide stream sr=%.0f low=%d formants=%d reused=%d checked=%d max_error=%.9g peak=%.7f allocations=%llu\n",sr,low,preserve,reused,checked,maxError,peak,gAllocationCount.load());
        std::fflush(stdout);assert(checked>1000 && peak<.5);return reused;
    }

    static long double oracle(const PitchShifter& p,double phase,double frequency) {
        const long double ceiling=std::min(24000.,.49*p.sr),roll=std::max(500.,.05*static_cast<double>(ceiling));
        const long double angle=6.283185307179586476925286766559L*phase;
        long double result=0;
        for(int bank=0;bank<2;++bank) {
            const auto* c=p.liveCoefficients(bank);const int count=bank ? p.liveCurrentCount : p.liveOldCount;
            const double weight=bank ? p.liveSpectrumBlend : 1-p.liveSpectrumBlend;
            long double value=c[0];
            for(int h=1;h<=count;++h) {
                const long double band=std::clamp((ceiling-h*frequency)/roll,0.L,1.L);
                value+=2*band*(c[h]*std::cos(h*angle)-c[PitchShifter::kLiveHarmonics+1+h]*std::sin(h*angle));
            }
            result+=weight*value;
        }
        return result;
    }
    static void check(double sr,double reference,int kind,double scale=1.) {
        PitchShifter p;p.prepare(sr,127);p.liveOldIndex=0;p.liveCurrentIndex=1;p.liveSpareIndex=2;
        p.liveOldCount=2048;p.liveCurrentCount=1536;p.liveSpectrumBlend=.37;p.liveSpectrumValid=true;
        for(int bank=0;bank<2;++bank) {
            auto* c=p.liveCoefficients(bank);const int count=bank ? p.liveCurrentCount : p.liveOldCount;
            c[0]=static_cast<float>(.002*scale);c[PitchShifter::kLiveHarmonics+1]=0;
            for(int h=1;h<=PitchShifter::kLiveHarmonics;++h) {
                const double magnitude=h<=10 ? .007/std::pow(h,1.2)
                    : kind==1 ? .007/(1+.01*h) : kind==2 ? 1e-10 : 0;
                c[h]=static_cast<float>(scale*magnitude*std::cos(h*.73+bank*.5));
                c[PitchShifter::kLiveHarmonics+1+h]=static_cast<float>(scale*magnitude*std::sin(h*1.13+bank*.3));
            }
            p.buildLiveWaveTable(bank,count,reference);
        }
        int reused=0;double maximum=0,bandDelta=0;
        for(double shift:{-.06,-.015,0.,.02,.08}) {
            const double frequency=reference*(1+shift);
            for(int j=0;j<41;++j) {
                const double phase=j==40 ? std::nextafter(1.,0.) : j*.02471685317;
                float cached=0;const bool accepted=p.tryLiveWave(phase,frequency,cached);
                if(accepted!=(kind!=1 || shift==0)) {
                    std::printf("unexpected reuse sr=%.0f f=%.3f kind=%d scale=%.1e shift=%.4f accepted=%d\n",sr,reference,kind,scale,shift,accepted);
                    std::fflush(stdout);
                }
                assert(accepted==(kind!=1 || shift==0));
                const long double target=oracle(p,phase,frequency);
                const double actual=p.evaluateLiveSpectrum(phase,frequency);
                assert(std::isfinite(actual));
                const double error=std::abs(actual-static_cast<double>(target))/scale;
                maximum=std::max(maximum,error);assert(error<(accepted ? 3e-5 : 3e-7));
                if(accepted && shift!=0) {
                    ++reused;const double delta=static_cast<double>(std::abs(oracle(p,phase,reference)-target))/scale;
                    bandDelta=std::max(bandDelta,delta);assert(delta<2e-7);
                }
            }
        }
        assert(kind==1 ? reused==0 : reused==164);
        float unused=0;
        for(double bad:{0.,-1.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
            assert(!p.tryLiveWave(.3,bad,unused));
        p.reset();assert(!p.tryLiveWave(.3,reference,unused));
        std::printf("glide cache sr=%.0f reference=%.3f kind=%d scale=%.1e reuse=%d max_error=%.9g cutoff_delta=%.9g\n",sr,reference,kind,scale,reused,maximum,bandDelta);
        std::fflush(stdout);
    }
};
}
int main() {
    int streamReused=0;
    for(double sr:{44100.,48000.,96000.})for(bool low:{false,true})for(bool preserve:{false,true})
        streamReused+=humtune::PitchShifterSpectralOracle::stream(sr,low,preserve);
    assert(streamReused>100);
    for(double sr:{44100.,48000.,96000.})for(double frequency:{35.1,82.4,211.3,713.7,1499.})
        for(int kind:{0,1,2})humtune::PitchShifterSpectralOracle::check(sr,frequency,kind);
    for(double sr:{44100.,48000.,96000.})for(double frequency:{82.4,1499.})
        for(double scale:{1e-3,1e-6,1e-9})for(int kind:{0,1,2})
            humtune::PitchShifterSpectralOracle::check(sr,frequency,kind,scale);
}
