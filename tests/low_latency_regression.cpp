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
    for (bool live : {false, true})
    for (double sr : {44100., 48000., 96000.})
        for (bool voiced : {false, true})
        {
            humtune::PitchShifter shifter;
            shifter.prepare(sr,96);
            shifter.setLowLatencyBudgetMilliseconds(1.9);
            shifter.setLowLatencyExperiment(live);
            const int delay = shifter.getLatencySamples();
            require(delay == (live ? static_cast<int>(std::ceil(sr*.0019)) : static_cast<int>(std::ceil(1.3*std::ceil(sr/35.)))+8), "wrong reported delay");
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
    std::cout << "PASS transparent noise/transients and exact reported scheduling in both modes at 44.1/48/96 kHz\n";
}

void correctionQuality()
{
    double worstResidual=0, worstGain=0;
    for (bool live : {false, true})
    for (double sr : {44100.,48000.,96000.})
        for (double hz : {40.,80.,220.,880.})
            for (double semitones : {-2., -1., 1., 2.})
            {
                const double ratio = std::exp2(semitones/12);
                humtune::PitchShifter shifter;
                shifter.prepare(sr,96);shifter.setMinFrequency(40);
                shifter.setLowLatencyBudgetMilliseconds(1.9);shifter.setLowLatencyExperiment(live);
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
    std::cout<<"PASS 96 shifted harmonic cases across both modes, worst residual "<<10*std::log10(worstResidual)<<" dB, gain error "<<worstGain<<" dB\n";
}

std::vector<float> runEngine(const std::vector<float>& input, int block, bool antiphase=false, bool live=true)
{
    humtune::AutoTuneEngine engine;
    engine.setLowLatencyMode(live);engine.prepare(44100,block);
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
    require(engine.getLatencySamples()==(live ? 84 : 726),"engine reported incorrect latency at 44.1 kHz");
    std::cout<<"engine live="<<live<<" block="<<block<<" max callback="<<worstMs<<" ms, CPU time/audio time="<<totalMs/(input.size()/44.1)<<'\n';
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
    for (bool live : {false, true})
    {
    const auto corrected=runEngine(steady,96,false,live);
    const double error=residual(corrected,44100,220);
    std::cout<<"integrated 223->220 Hz residual="<<10*std::log10(error)<<" dB\n";
    require(error<.06,"integrated detector/corrector is not tuning 223 Hz to A3");
    const auto reference=runEngine(moving,96,false,live);
    for(int block:{32,64,128,256,512})
    {
        const auto output=runEngine(moving,block,block==64,live);
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
}

// Track phase of the corrected fundamental with a four-cycle Hann projection.
// Unlike a largest-FFT-bin check, this catches short pitch excursions and warble.
double pitchWobble(const std::vector<float>& data, double hz)
{
    const int window=static_cast<int>(std::round(4*44100/hz));
    const int step=static_cast<int>(std::round(44100/hz));
    double previous=0, sum=0;int count=0;bool first=true;
    for(int pos=26460;pos+window<static_cast<int>(data.size())-8820;pos+=step)
    {
        double re=0,im=0;
        for(int j=0;j<window;++j)
        {
            const double w=.5-.5*std::cos(2*pi*j/(window-1));
            const double phase=2*pi*hz*(pos+j)/44100;
            re+=w*data[static_cast<size_t>(pos+j)]*std::cos(phase);
            im-=w*data[static_cast<size_t>(pos+j)]*std::sin(phase);
        }
        const double phase=std::atan2(im,re);
        if(!first)
        {
            const double delta=std::remainder(phase-previous,2*pi);
            const double frequency=hz+delta*44100/(2*pi*step);
            const double cents=1200*std::log2(std::max(1.,frequency)/hz);
            sum+=cents*cents;++count;
        }
        first=false;previous=phase;
    }
    require(count>50,"pitch-motion analysis did not execute");
    return std::sqrt(sum/count);
}

void vibratoAndTargetStability()
{
    for(bool live:{false,true})for(double base:{110.,220.,223.,440.})
    {
        constexpr int n=44100*3, block=96;
        const double target=440*std::exp2(std::round(12*std::log2(base/440))/12);
        const int targetMidi=static_cast<int>(std::round(69+12*std::log2(target/440)));
        humtune::AutoTuneEngine engine;engine.setLowLatencyMode(live);engine.prepare(44100,block);
        engine.setScaleType(2);engine.setRetuneSpeed(0);
        juce::AudioBuffer<float> buffer(1,block);std::vector<float> output(n);
        double phase=0;
        for(int pos=0;pos+block<=n;pos+=block)
        {
            for(int i=0;i<block;++i)
            {
                const double hz=base*std::exp2(30*std::sin(2*pi*5*(pos+i)/44100)/1200);
                phase+=2*pi*hz/44100;buffer.getWritePointer(0)[i]=voice(phase);
            }
            engine.process(buffer);std::copy_n(buffer.getReadPointer(0),block,output.data()+pos);
            if(pos>4410)require(engine.getTargetMidiNote()==targetMidi,"target chatters during vibrato near a scale boundary");
        }
        const double rms=pitchWobble(output,target);
        std::cout<<"vibrato live="<<live<<" input="<<base<<" +/-30 cents, corrected pitch RMS="<<rms<<" cents\n";
        require(rms<10.,"hard tuning leaves excessive pitch wobble or dry/wet phase jumps");
        // Changing the permitted notes must invalidate a held target immediately.
        engine.setNoteStabilizer(true);
        std::array<bool,12> cOnly {};cOnly[0]=true;engine.setCustomScale(cOnly);
        for(int repeat=0;repeat<4;++repeat)
        {
            for(int i=0;i<block;++i)
            {
                phase+=2*pi*base/44100;
                buffer.getWritePointer(0)[i]=voice(phase);
            }
            engine.process(buffer);
        }
        require(engine.getTargetMidiNote()>=0 && engine.getTargetMidiNote()%12==0,"target hysteresis retained a disallowed note");
    }
    std::cout<<"PASS strong correction, phase-continuous vibrato and target hysteresis in both modes\n";
}

void fastNoteTransitions()
{
    constexpr int block=96, duration=15456;
    constexpr double notes[] {223.,336.,197.,446.,112.,281.,167.,559.};
    for(bool live:{false,true})
    {
        humtune::AutoTuneEngine engine;engine.setLowLatencyMode(live);engine.prepare(44100,block);
        engine.setScaleType(2);engine.setRetuneSpeed(0);
        juce::AudioBuffer<float> buffer(1,block);
        double phase=0;float previous=0,maxStep=0;
        for(int note=0;note<8;++note)
        {
            int firstCorrect=-1;
            const int target=static_cast<int>(std::round(69+12*std::log2(notes[note]/440.)));
            for(int pos=0;pos<duration;pos+=block)
            {
                for(int j=0;j<block;++j)
                {
                    // A 10 ms phase-continuous slide includes octave jumps and
                    // downward transitions, where stale periods can cause bursts.
                    const double u=std::clamp((pos+j)/441.,0.,1.);
                    const double from=notes[std::max(0,note-1)];
                    phase+=2*pi*from*std::exp2(u*std::log2(notes[note]/from))/44100;
                    buffer.getWritePointer(0)[j]=voice(phase);
                }
                engine.process(buffer);
                if(engine.getTargetMidiNote()==target && firstCorrect<0)firstCorrect=pos;
                if(pos>4410)require(engine.getTargetMidiNote()==target,"wrong-note burst after a legato transition");
                for(int j=0;j<block;++j)
                {
                    const float x=buffer.getReadPointer(0)[j];
                    require(std::isfinite(x) && std::abs(x)<.6f,"transition produces a burst or invalid sample");
                    maxStep=std::max(maxStep,std::abs(x-previous));previous=x;
                }
            }
            require(firstCorrect>=0 && firstCorrect<2646,"note acquisition exceeds 60 ms on transition test");
        }
        require(maxStep<.12f,"legato transition produces a discontinuity");
    }
    std::cout<<"PASS fast legato/octave transitions, bounded acquisition and no persistent wrong-note bursts\n";
}

void phraseAttacksAndHistory()
{
    for(bool live:{false,true})
    {
        constexpr int n=44100,block=96;
        humtune::AutoTuneEngine engine;engine.setLowLatencyMode(live);engine.prepare(44100,block);engine.setScaleType(2);
        juce::AudioBuffer<float> buffer(1,block);std::vector<float> input(n),output(n);
        unsigned state=37;double phase=0;
        for(int i=0;i<n;++i)
        {
            state=state*1664525u+1013904223u;
            if(i<5292)input[static_cast<size_t>(i)]=(static_cast<float>(state>>8)/16777216.f-.5f)*.12f;
            else
            {
                phase+=2*pi*223/44100;
                const float attack=std::min(1.f,static_cast<float>(i-5292)/176.f);
                input[static_cast<size_t>(i)]=voice(phase)*attack;
            }
        }
        for(int pos=0;pos+block<=n;pos+=block)
        {
            std::copy_n(input.data()+pos,block,buffer.getWritePointer(0));engine.process(buffer);
            std::copy_n(buffer.getReadPointer(0),block,output.data()+pos);
        }
        const int delay=engine.getLatencySamples();
        for(int i=0;i<4410;++i)
            require(output[static_cast<size_t>(i)]==(i>=delay?input[static_cast<size_t>(i-delay)]:0.f),"unpitched phrase attack is reshaped");
        float jump=0;
        for(int i=5292;i<11025;++i)jump=std::max(jump,std::abs(output[static_cast<size_t>(i)]-output[static_cast<size_t>(i-1)]));
        std::cout<<"phrase attack live="<<live<<" max step="<<jump<<'\n';
        require(jump<.12f,"voice onset produces a discontinuity");
        engine.setEnabled(false);engine.process(buffer);engine.setEnabled(true);
        for(int repeat=0;repeat<2;++repeat)
        {
            engine.process(buffer);
            require(engine.getConfidence()==0.f,"bypass re-enable reuses stale detector history");
        }
        engine.setLowLatencyMode(!live);engine.process(buffer);
        require(engine.getConfidence()==0.f,"mode change reuses stale detector history");
    }
    std::cout<<"PASS transparent phrase attacks and fresh detector history after bypass/mode changes\n";
}

void continuousBypass()
{
    for(bool live:{false,true})
    {
        humtune::AutoTuneEngine engine;engine.setLowLatencyMode(live);engine.prepare(44100,96);
        juce::AudioBuffer<float> buffer(1,96);std::vector<float> input(96*300);
        unsigned rng=73;
        for(auto& x:input){rng=rng*1664525u+1013904223u;x=(static_cast<float>(rng>>8)/16777216.f-.5f)*.1f;}
        const int delay=engine.getLatencySamples();
        for(int block=0;block<300;++block)
        {
            if(block==100)engine.setEnabled(false);
            if(block==200)engine.setEnabled(true);
            std::copy_n(input.data()+block*96,96,buffer.getWritePointer(0));engine.process(buffer);
            for(int i=0;i<96;++i)
            {
                const int t=block*96+i-delay;
                require(buffer.getReadPointer(0)[i]==(t>=0?input[static_cast<size_t>(t)]:0.f),"bypass/re-enable inserts a gap or changes unpitched audio");
            }
        }
    }
    std::cout<<"PASS exact delay continuity across tuning bypass and re-enable\n";
}

void correctedBypassTransitions()
{
    for(bool live:{false,true})
    {
        constexpr int sr=44100,block=96;
        humtune::AutoTuneEngine engine;engine.setLowLatencyMode(live);engine.prepare(sr,block);
        engine.setRetuneSpeed(0);engine.setScaleType(2);
        juce::AudioBuffer<float> buffer(1,block);
        float previous=0,maxStep=0;int disabledSamples=0;
        for(int pos=0;pos<sr*4;pos+=block)
        {
            const bool on=(pos/(block*101))%2==0 && (pos/block)%403!=50;
            engine.setEnabled(on);
            for(int j=0;j<block;++j)
                buffer.getWritePointer(0)[j]=voice(2*pi*223*(pos+j)/sr);
            engine.process(buffer);
            for(int j=0;j<block;++j)
            {
                disabledSamples=on ? 0 : disabledSamples+1;
                const float x=buffer.getReadPointer(0)[j];
                maxStep=std::max(maxStep,std::abs(x-previous));previous=x;
                if(disabledSamples>block*2)
                    require(x==voice(2*pi*223*(pos+j-engine.getLatencySamples())/sr),
                            "bypass fade does not settle to exact delayed dry");
            }
        }
        require(maxStep<.08f,"bypassing corrected audio causes a phase-switch click");
    }
    std::cout<<"PASS smooth corrected-vocal bypass, short re-enable and settled exact dry\n";
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
    try { transparentAndLatency();correctionQuality();integratedAndBlockSizes();vibratoAndTargetStability();fastNoteTransitions();phraseAttacksAndHistory();continuousBypass();correctedBypassTransitions();resetAndModeChanges(); }
    catch(const std::exception& e){countAllocations=false;std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
