#include "AutoTuneEngine.h"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <iomanip>
#include <chrono>

static void writeU16(std::ofstream& f, uint16_t v) { f.put(static_cast<char>(v & 255)); f.put(static_cast<char>((v >> 8) & 255)); }
static void writeU32(std::ofstream& f, uint32_t v) { for (int i=0;i<4;++i) f.put(static_cast<char>((v >> (8*i)) & 255)); }
static bool writeFloatWav(const std::string& path, const std::vector<float>& interleaved, int sampleRate, int channels) {
    std::ofstream f(path, std::ios::binary); if(!f) return false;
    const uint32_t dataBytes = static_cast<uint32_t>(interleaved.size() * sizeof(float));
    f.write("RIFF",4); writeU32(f, 36 + dataBytes); f.write("WAVE",4);
    f.write("fmt ",4); writeU32(f,16); writeU16(f,3); writeU16(f,static_cast<uint16_t>(channels));
    writeU32(f,static_cast<uint32_t>(sampleRate)); writeU32(f,static_cast<uint32_t>(sampleRate*channels*sizeof(float)));
    writeU16(f,static_cast<uint16_t>(channels*sizeof(float))); writeU16(f,32);
    f.write("data",4); writeU32(f,dataBytes);
    f.write(reinterpret_cast<const char*>(interleaved.data()), dataBytes);
    return static_cast<bool>(f);
}
int main(int argc,char**argv) {
    if(argc<7){std::cerr<<"usage: harness input.raw output.wav samples blockSize lowLatency range [metrics.csv] [rootMidiPitchClass] [preserveFormants]\n";return 2;}
    const std::string inputPath=argv[1], outputPath=argv[2];
    const int samples=std::stoi(argv[3]), blockSize=std::stoi(argv[4]), lowLatency=std::stoi(argv[5]), range=std::stoi(argv[6]);
    constexpr int channels=2;
    const int sr=argc>=13 ? std::stoi(argv[12]) : 44100;
    if(samples<=0 || blockSize<=0 || sr<8000 || sr>192000) return 2;
    std::vector<float> input(static_cast<size_t>(samples)*channels);
    std::ifstream in(inputPath,std::ios::binary);
    in.read(reinterpret_cast<char*>(input.data()), static_cast<std::streamsize>(input.size()*sizeof(float)));
    if(in.gcount()!=static_cast<std::streamsize>(input.size()*sizeof(float))){std::cerr<<"input read failed: "<<in.gcount()<<" expected "<<input.size()*sizeof(float)<<"\n";return 3;}
    std::vector<float> output(input.size());
    humtune::AutoTuneEngine engine;
    engine.setRangeProtection(argc >= 12 && std::stoi(argv[11]) != 0);
    engine.setVoiceRange(range); engine.setRootNote(argc >= 9 ? std::stoi(argv[8]) : 0); engine.setScaleType(0);
    engine.setAmount(1.0f); engine.setRetuneSpeed(0.0f); engine.setHumanize(0.0f);
    // Match the requested SkellyTune default: Note Stabilizer OFF. Dedicated
    // note-lock regressions explicitly enable it when testing that behavior.
    engine.setNoteStabilizer(false); engine.setFormantPreserve(argc >= 10 ? std::stoi(argv[9]) != 0 : true);
    engine.setLowLatencyMode(lowLatency != 0); engine.setEnabled(true); engine.prepare(sr,blockSize);
    // The allocation-capable vector belongs exclusively to this OFFLINE renderer.
    // Plugin builds leave AutoTuneEngine's audio-thread callback disconnected.
    std::vector<humtune::AutoTuneEngine::HopTrace> hopTraces;
    if(argc >= 11 && std::string(argv[10]) != "-")
    {
        hopTraces.reserve(static_cast<size_t>(samples / 32 + 256));   // sized for the fast (2 ms) hop as well
        engine.setOfflineTraceCallback([](void* ctx, const humtune::AutoTuneEngine::HopTrace& t) noexcept {
            auto* traces = static_cast<std::vector<humtune::AutoTuneEngine::HopTrace>*>(ctx);
            // A fixed reserve cap prevents accidental allocation during a DSP callback.
            if (traces->size() < traces->capacity()) traces->push_back(t);
        }, &hopTraces);
    }
    std::ofstream metrics;
    if(argc>=8 && std::string(argv[7]) != "-") { metrics.open(argv[7]); metrics<<"offset,detected_hz,target_hz,correction_cents,confidence,detected_midi,target_midi\n"; }
    int offset=0;
    std::vector<double> callbackTimes;
    callbackTimes.reserve(static_cast<size_t>((samples+blockSize-1)/blockSize));
    double rmsIn=0.0,rmsOut=0.0; float peak=0.0f;
    while(offset<samples){
      const int n=std::min(blockSize,samples-offset);
      juce::AudioBuffer<float> b(channels,n);
      for(int i=0;i<n;++i){b.getWritePointer(0)[i]=input[(size_t)(offset+i)*channels];b.getWritePointer(1)[i]=input[(size_t)(offset+i)*channels+1];}
      const auto callbackStart=std::chrono::steady_clock::now();
      engine.process(b);
      callbackTimes.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now()-callbackStart).count());
      if(metrics) metrics<<offset<<','<<engine.getDetectedHz()<<','<<engine.getTargetHz()<<','<<engine.getCorrectionCents()<<','<<engine.getConfidence()<<','<<engine.getDetectedMidiNote()<<','<<engine.getTargetMidiNote()<<'\n';
      for(int i=0;i<n;++i){ for(int ch=0;ch<channels;++ch){const float x=b.getReadPointer(ch)[i]; output[(size_t)(offset+i)*channels+ch]=x; peak=std::max(peak,std::abs(x)); rmsOut+=double(x)*x; rmsIn+=double(input[(size_t)(offset+i)*channels+ch])*input[(size_t)(offset+i)*channels+ch];}}
      offset+=n;
    }
    if(!writeFloatWav(outputPath,output,sr,channels)){std::cerr<<"write wav failed\n";return 4;}
    if (argc >= 11 && std::string(argv[10]) != "-")
    {
        std::ofstream trace(argv[10]);
        trace << "sample,time_s,observed_hz,predicted_hz,observed_midi,predicted_midi,measured_target_midi,selected_target_midi,confidence,onset_strength,onset,voiced,correction_cents,ratio,grain_coverage,grain_gain,grain_contributors,mark_delta_samples,max_output_step,phase_match_score,dry_mix,neutral_dry_selected,overlap_match_score,overlap_shift_samples,overlap_cancellation\n";
        trace << std::setprecision(8);
        for (const auto& t : hopTraces)
            trace << t.sample << ',' << (static_cast<double>(t.sample)/sr) << ','
                << t.observedHz << ',' << t.predictedHz << ',' << t.observedMidi << ','
                << t.predictedMidi << ',' << t.measuredTargetMidi << ',' << t.selectedTargetMidi << ','
                << t.confidence << ',' << t.onsetStrength << ',' << static_cast<int>(t.onset)
                << ',' << static_cast<int>(t.voiced) << ',' << t.correctionCents << ',' << t.ratio
                << ',' << t.grainCoverage << ',' << t.grainGain << ',' << t.grainContributors
                << ',' << t.grainPhaseMarkDelta << ',' << t.maxOutputStep << ',' << t.grainPhaseScore << ','
                << t.dryMix << ',' << static_cast<int>(t.neutralDrySelected) << ','
                << t.overlapMatchScore << ',' << t.overlapShiftSamples << ','
                << t.overlapCancellation << '\n';
    }
    const double sampleCount=static_cast<double>(samples)*channels;
    double cpuSeconds=0;for(double t:callbackTimes)cpuSeconds+=t;
    std::sort(callbackTimes.begin(),callbackTimes.end());
    std::cout<<"dspSeconds="<<cpuSeconds<<" callbackP99Seconds="<<callbackTimes[static_cast<size_t>(.99*(callbackTimes.size()-1))]
             <<" callbackMaxSeconds="<<callbackTimes.back()<<" nominalDeadlineSeconds="<<double(blockSize)/sr<<"\n";
    std::cout<<"block="<<blockSize<<" lowLatency="<<lowLatency<<" range="<<range<<" algoLatencySamples="<<engine.getLatencySamples()<<" duration="<<double(samples)/sr<<"s inputRMS="<<sqrt(rmsIn/sampleCount)<<" outputRMS="<<sqrt(rmsOut/sampleCount)<<" peak="<<peak<<" finalDetected="<<engine.getDetectedHz()<<" finalTarget="<<engine.getTargetHz()<<"\n";
}
