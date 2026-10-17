#include "AutoTuneEngine.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>

namespace { bool countAllocations = false; size_t allocations = 0; }
void* operator new(std::size_t n)
{
    if (countAllocations) ++allocations;
    if (auto* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace
{
constexpr double pi = 3.14159265358979323846;
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
float voice(double phase)
{
    return static_cast<float>(0.2 * std::sin(phase) + 0.1 * std::sin(2*phase + .3)
                              + .07 * std::sin(3*phase + .1) + .05 * std::sin(5*phase));
}
double energy(const std::vector<float>& data, int start, int n)
{
    double sum = 0;
    for (int i = start; i < start+n; ++i) sum += data[static_cast<size_t>(i)] * data[static_cast<size_t>(i)];
    return sum / n;
}
// Fit the expected harmonic family over 100 complete target cycles. Broadband
// discontinuities and off-pitch sidebands remain in the residual, unlike a peak-only test.
double residual(const std::vector<float>& data, double sr, double hz)
{
    const int start = static_cast<int>(sr * .5);
    const int n = std::min(static_cast<int>(std::round(100 * sr / hz)), static_cast<int>(data.size()) - start);
    std::vector<double> fit(static_cast<size_t>(n), 0.0);
    for (int h = 1; h <= 8; ++h)
    {
        double a=0, b=0;
        for (int i=0; i<n; ++i)
        {
            const double phase = 2*pi*hz*h*i/sr;
            a += data[static_cast<size_t>(start+i)] * std::sin(phase);
            b += data[static_cast<size_t>(start+i)] * std::cos(phase);
        }
        for (int i=0; i<n; ++i)
        {
            const double phase = 2*pi*hz*h*i/sr;
            fit[static_cast<size_t>(i)] += 2.0/n * (a*std::sin(phase) + b*std::cos(phase));
        }
    }
    double error=0;
    for (int i=0; i<n; ++i)
    {
        const double delta = data[static_cast<size_t>(start+i)] - fit[static_cast<size_t>(i)];
        error += delta*delta;
    }
    return error/n / std::max(1.0e-20, energy(data,start,n));
}

void transparentAndLatency()
{
    for (double sr : {44100., 48000., 96000.})
        for (bool voiced : {false, true})
        {
            humtune::PitchShifter shifter;
            shifter.prepare(sr,96);
            shifter.setLowLatencyBudgetMilliseconds(1.9);
            shifter.setLowLatencyExperiment(true);
            const int delay = shifter.getLatencySamples();
            require(delay == static_cast<int>(std::ceil(sr*.0019)), "wrong fixed delay");
            std::vector<float> input(16384), output(input.size());
            unsigned state=1;
            for (auto& x : input)
            {
                state = state*1664525u+1013904223u;
                x = static_cast<float>(state>>8)/16777216.f - .5f;
            }
            input[0]=1; // startup impulse must emerge at exactly the reported delay
            output=input;
            for (int i=0; i<static_cast<int>(output.size()); i+=96)
                shifter.process(output.data()+i, std::min(96,static_cast<int>(output.size())-i),
                                voiced ? static_cast<float>(sr/80) : 0.f, voiced ? 1.f : 1.12f);
            for (size_t i=0; i<output.size(); ++i)
                require(output[i] == (i >= static_cast<size_t>(delay) ? input[i-static_cast<size_t>(delay)] : 0.f),
                        "unchanged/unvoiced audio is not exact delayed dry");
        }
    std::cout << "PASS transparent noise/transients and exact 1.9 ms scheduling at 44.1/48/96 kHz\n";
}

void correctionQuality()
{
    double worstResidual=0, worstGain=0;
    for (double sr : {44100.,48000.,96000.})
        for (double hz : {40.,80.,220.,880.})
            for (double semitones : {-2., -1., 1., 2.})
            {
                const double ratio = std::exp2(semitones/12);
                humtune::PitchShifter shifter;
                shifter.prepare(sr,96);shifter.setMinFrequency(40);
                shifter.setLowLatencyBudgetMilliseconds(1.9);shifter.setLowLatencyExperiment(true);
                const int n=static_cast<int>(sr*3.5);
                std::vector<float> input(static_cast<size_t>(n)),output(static_cast<size_t>(n));
                for(int i=0;i<n;++i)input[static_cast<size_t>(i)]=voice(2*pi*hz*i/sr);
                output=input;
                for(int i=0;i<n;i+=96)
                    shifter.process(output.data()+i,std::min(96,n-i),static_cast<float>(sr/hz),static_cast<float>(ratio));
                const double err=residual(output,sr,hz*ratio);
                const int start=static_cast<int>(sr*.5);
                const double gain=10*std::log10(energy(output,start,n-start)/energy(input,start,n-start));
                worstResidual=std::max(worstResidual,err);worstGain=std::max(worstGain,std::abs(gain));
                require(std::all_of(output.begin(),output.end(),[](float x){return std::isfinite(x) && std::abs(x)<1.2f;}),"invalid shifted audio");
                if(err>.04 || std::abs(gain)>1.5)
                    std::cerr << "quality sr="<<sr<<" hz="<<hz<<" st="<<semitones<<" residual="<<err<<" gain="<<gain<<'\n';
                require(err<.04,"excessive nonharmonic distortion or wrong corrected pitch");
                require(std::abs(gain)<1.5,"correction changes steady voice level by >1.5 dB");
            }
    std::cout<<"PASS 48 shifted harmonic cases, worst residual "<<10*std::log10(worstResidual)<<" dB, gain error "<<worstGain<<" dB\n";
}

std::vector<float> runEngine(const std::vector<float>& input, int block, bool antiphase=false)
{
    humtune::AutoTuneEngine engine;
    engine.setLowLatencyMode(true);engine.prepare(44100,block);
    engine.setScaleType(2);engine.setRetuneSpeed(0);
    juce::AudioBuffer<float> buffer(2,block);
    std::vector<float> result(input.size());
    double worstMs=0,totalMs=0;
    const auto before=allocations;
    for(int pos=0;pos+block<=static_cast<int>(input.size());pos+=block)
    {
        for(int i=0;i<block;++i)
        {
            buffer.getWritePointer(0)[i]=input[static_cast<size_t>(pos+i)];
            buffer.getWritePointer(1)[i]=(antiphase?-1.f:1.f)*input[static_cast<size_t>(pos+i)];
        }
        countAllocations=true;
        const auto start=std::chrono::steady_clock::now();
        engine.process(buffer);
        const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        countAllocations=false;
        worstMs=std::max(worstMs,ms);totalMs+=ms;
        std::copy_n(buffer.getReadPointer(0),block,result.data()+pos);
        for(int i=0;i<block;++i)
            require(std::abs(buffer.getReadPointer(1)[i]-(antiphase?-1.f:1.f)*buffer.getReadPointer(0)[i])<1.e-6f,"stereo phase mismatch");
    }
    require(allocations==before,"audio callback allocated memory");
    require(engine.getLatencySamples()==84,"engine reported latency != 84 samples at 44.1 kHz");
    std::cout<<"engine block="<<block<<" max callback="<<worstMs<<" ms, CPU time/audio time="<<totalMs/(input.size()/44.1)<<'\n';
    return result;
}

void integratedAndBlockSizes()
{
    constexpr int n=44100*2;
    std::vector<float> steady(n),moving(n);
    double phase=0;
    for(int i=0;i<n;++i)
    {
        steady[static_cast<size_t>(i)]=voice(2*pi*223*i/44100);
        const double hz=(i<44100 ? 220. : 246.94165)*std::exp2(.25*std::sin(2*pi*5*i/44100)/12);
        phase+=2*pi*hz/44100;
        moving[static_cast<size_t>(i)]=voice(phase);
    }
    const auto corrected=runEngine(steady,96);
    const double error=residual(corrected,44100,220);
    std::cout<<"integrated 223->220 Hz residual="<<10*std::log10(error)<<" dB\n";
    require(error<.06,"integrated detector/corrector is not tuning 223 Hz to A3");
    const auto reference=runEngine(moving,96);
    for(int block:{32,64,128,256,512})
    {
        const auto output=runEngine(moving,block,block==64);
        const int usable=(n/std::max(96,block)-1)*std::max(96,block);
        for(int i=0;i<usable;++i)
            require(std::abs(output[static_cast<size_t>(i)]-reference[static_cast<size_t>(i)])<2.e-5f,"output depends on host callback size");
    }
    float maxJump=0;
    for(int i=22050;i<n-512;++i)
        maxJump=std::max(maxJump,std::abs(reference[static_cast<size_t>(i)]-reference[static_cast<size_t>(i-1)]));
    std::cout<<"changing pitch/vibrato max sample step="<<maxJump<<'\n';
    require(maxJump<.12f,"changing pitch produces a discontinuity");
    std::cout<<"PASS integrated pitch correction, stereo, block-size invariance, zero callback allocations\n";
}

void resetAndModeChanges()
{
    humtune::PitchShifter shifter;
    shifter.prepare(44100,96);shifter.setLowLatencyBudgetMilliseconds(1.9);shifter.setLowLatencyExperiment(true);
    float block[96];
    for(int repeat=0;repeat<12;++repeat)
    {
        std::fill_n(block,96,.25f);shifter.process(block,96,200,1.05f);
        shifter.setLowLatencyExperiment(false);shifter.setLowLatencyExperiment(true);
        for(int i=0;i<32;++i)
        {
            std::fill_n(block,96,0.f);shifter.process(block,96,200,1.05f);
            for(float x:block)require(x==0.f,"stale grain leaks after mode change");
        }
    }
    shifter.prepare(48000,96);
    require(shifter.getLatencySamples()==92,"re-prepare did not recalculate latency");
    std::cout<<"PASS mode changes and sample-rate re-prepare\n";
}
}
int main()
{
    try { transparentAndLatency();correctionQuality();integratedAndBlockSizes();resetAndModeChanges(); }
    catch(const std::exception& e){countAllocations=false;std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
