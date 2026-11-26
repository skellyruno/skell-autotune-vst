#pragma once
// Standalone integration of the pinned MetaTune 2.5 Playground native engine.
// Shared causal formant colour has no extra audio transport.
#include "SkellyFusionEngine.h"
#include "shared/CausalFormantShift.hpp"
#include "ScaleQuantizer.h"
#include "KeyEstimator.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace humtune {
class MetaTune25Engine {
public:
    static constexpr int kNumRanges=6, kDefaultRange=1;
    static constexpr std::array<float,kNumRanges> kRangeMinHz{{70.f,80.f,100.f,130.f,180.f,40.f}};
    void setRootNote(int v) noexcept { root_=((v%12)+12)%12; }
    void setScaleType(int v) noexcept { scale_=ScaleQuantizer::clampType(v); }
    void setRetuneSpeed(float v) noexcept { speed_=100.f*unit(v); }
    void setHumanize(float v) noexcept { human_=unit(v); }
    void setFlexTune(float v) noexcept { flex_=unit(v); }
    void setGlideMilliseconds(float v) noexcept { glide_=std::clamp(std::isfinite(v)?v:0.f,0.f,500.f); }
    void setReferenceFreq(float v) noexcept { reference_=std::clamp(std::isfinite(v)?v:440.f,400.f,480.f); }
    void setNoteStabilizer(bool v) noexcept { stabilize_=v; }
    void setRangeProtection(bool v) noexcept { protectRange_=v; }
    void setVoiceRange(int v) noexcept { range_=std::clamp(v,0,kNumRanges-1); }
    void setLowLatencyMode(bool v) noexcept { low_=v; }
    void setEnabled(bool v) noexcept { enabled_=v; }
    void setFormantShiftSemitones(float v) noexcept { formant_=std::clamp(std::isfinite(v)?v:0.f,-4.f,4.f); }
    void setCustomScale(const std::array<bool,12>& v) noexcept { mask_=v; custom_=true; }
    void clearCustomScale() noexcept { custom_=false; }

    void prepare(double rate,int) {
        rate_=std::clamp(std::isfinite(rate)?rate:48000.,8000.,192000.);
        core_.prepare(rate_,70.f,1000.f);
        colour_.prepare(rate_,true);
        for(auto& lane:dry_)lane.assign(size_t(std::ceil(.050*rate_))+2,0.f);
        pos_=0; fade_=0.f; fadeStep_=1.f/float(std::max(1,int(std::lround(.006*rate_))));
        latency_=0;
        detected_.store(0.f);target_.store(-1);confidence_.store(0.f);cents_.store(0.f);referenceReadback_.store(reference_);
        key_.reset();keySamples_=0;publishKey();
        prepared_=true;
        configure();
        core_.reset();
    }
    int getLatencySamples() const noexcept { return latency_; }
    float getDetectedHz() const noexcept { return detected_.load(std::memory_order_relaxed); }
    float getConfidence() const noexcept { return confidence_.load(std::memory_order_relaxed); }
    float getCorrectionCents() const noexcept { return cents_.load(std::memory_order_relaxed); }
    int getTargetMidiNote() const noexcept { return target_.load(std::memory_order_relaxed); }
    float getTargetMidiFloat() const noexcept { return float(getTargetMidiNote()); }
    int getDetectedMidiNote() const noexcept {
        const float hz=getDetectedHz(),ref=referenceReadback_.load(std::memory_order_relaxed);
        return hz>0.f?int(std::lround(69.+12.*std::log2(double(hz)/ref))):-1;
    }
    float getTargetHz() const noexcept {
        const int note=getTargetMidiNote();
        return note>=0?referenceReadback_.load(std::memory_order_relaxed)*std::exp2(float(note-69)/12.f):0.f;
    }
    KeyEstimator::Estimate getAutoKeyEstimate() const noexcept {
        KeyEstimator::Estimate e;
        e.root=keyRoot_.load();e.minor=keyMinor_.load();e.confidence=keyConfidence_.load();
        e.evidenceSeconds=keyEvidence_.load();e.profileScore=keyScore_.load();e.scoreMargin=keyMargin_.load();
        e.distinctPitchClasses=keyDistinct_.load();e.valid=keyValid_.load();return e;
    }
    void process(float* const* io,int channels,int frames) {
        if(!prepared_||!io||channels<1||channels>2||frames<=0||!io[0]||(channels==2&&!io[1]))return;
        configure();
        const bool wet=enabled_&&activeMask_!=0;
        for(int offset=0;offset<frames;){
            const int count=std::min(128,frames-offset);
            float* q[2]{io[0]+offset,channels==2?io[1]+offset:nullptr};
            std::array<std::array<float,128>,2> raw{};
            for(int c=0;c<channels;++c)for(int i=0;i<count;++i){
                const float x=q[c][i];q[c][i]=raw[size_t(c)][size_t(i)]=std::isfinite(x)?std::clamp(x,-32.f,32.f):0.f;
            }
            core_.processChannels(q,channels,count);
            for(int i=0;i<count;++i){
                const auto capacity=dry_[0].size();
                const auto read=(pos_+capacity-size_t(latency_))%capacity;
                for(int c=0;c<channels;++c)dry_[size_t(c)][pos_]=raw[size_t(c)][size_t(i)];
                const auto y=colour_.process(q[0][i],channels==2?q[1][i]:q[0][i]);
                q[0][i]=y[0];if(channels==2)q[1][i]=y[1];
                const float targetGain=wet?1.f:0.f;
                fade_+=std::clamp(targetGain-fade_,-fadeStep_,fadeStep_);
                for(int c=0;c<channels;++c){
                    const float d=dry_[size_t(c)][read],w=std::isfinite(q[c][i])?q[c][i]:d;
                    q[c][i]=fade_==1.f?w:fade_==0.f?d:d+(w-d)*fade_;
                }
                if(++pos_==capacity)pos_=0;
            }
            const float midi=core_.getDetectedMidi();
            const float hz=midi>0?reference_*std::exp2((midi-69.f)/12.f):0.f;
            const float conf=core_.getConfidence();
            detected_.store(wet?hz:0.f,std::memory_order_relaxed);
            confidence_.store(wet?conf:0.f,std::memory_order_relaxed);
            target_.store(wet?core_.getTargetMidi():-1,std::memory_order_relaxed);
            cents_.store(wet?100.f*core_.getCorrectionSemitones()*fade_:0.f,std::memory_order_relaxed);
            const double seconds=double(count)/rate_;
            key_.advanceTime(seconds);
            if(midi>0)key_.observePitch(midi,conf,seconds);
            keySamples_+=count;if(keySamples_>=int(rate_*.1)){keySamples_=0;publishKey();}
            offset+=count;
        }
    }
private:
    static float unit(float v) noexcept {return std::clamp(std::isfinite(v)?v:0.f,0.f,1.f);}
    void configure() {
        const auto notes=custom_?mask_:ScaleQuantizer::approximateMask(scale_,root_);
        activeMask_=0;for(int i=0;i<12;++i)if(notes[size_t(i)])activeMask_|=uint16_t(1u<<i);
        autotune::Settings p;p.key=0;p.scale=autotune::Scale::Custom;p.customScale=notes;
        p.referenceHz=reference_;p.retuneMs=speed_;p.glideMs=glide_;
        p.lowLatency=low_;p.ultraLowLatency=low_;p.strictRealtime=true;p.snapLive=false;
        p.noteConfirmMs=stabilize_?25.f:10.f;p.noteStabilizerMs=0.f;
        p.follow=.85f;p.voicingThreshold=.68f;
        const float hz=getDetectedHz(),targetHz=getTargetHz();
        const float error=hz>0.f&&targetHz>0.f?std::abs(float(1200.*std::log2(double(hz)/targetHz))):0.f;
        const float radius=6.f+42.f*(1.f-flex_),width=12.f+18.f*(1.f-flex_);
        const float x=std::clamp((error-radius)/width,0.f,1.f);
        p.strength=1.f-flex_*x*x*(3.f-2.f*x);
        // Native floor is fixed at 70Hz to match the audition engine. The old
        // high-range protection switch maps to correction eligibility only.
        if(!protectRange_&&hz>0.f&&hz<kRangeMinHz[size_t(range_)])p.strength=0.f;
        if(low_){const float weight=std::clamp(std::min((speed_-5.f)/5.f,(50.f-speed_)/25.f),0.f,1.f);p.consonantReleaseMs=8.f-4.f*weight;}
        core_.setSustain(-human_);
        core_.setSettings(p);
        referenceReadback_.store(reference_,std::memory_order_relaxed);
        colour_.setSemitones(formant_);
        const int delay=core_.latencySamples();
        if(delay!=latency_){latency_=delay;fade_=0.f;}
    }
    void publishKey() noexcept {
        const auto e=key_.estimate();keyRoot_.store(e.root);keyMinor_.store(e.minor);
        keyConfidence_.store(e.confidence);keyEvidence_.store(e.evidenceSeconds);keyScore_.store(e.profileScore);
        keyMargin_.store(e.scoreMargin);keyDistinct_.store(e.distinctPitchClasses);keyValid_.store(e.valid);
    }
    autotune::SkellyFusionEngine core_;
    skell::research::CausalFormantShift colour_;
    KeyEstimator key_;
    std::array<std::vector<float>,2> dry_;
    size_t pos_=0;
    double rate_=48000.;
    float speed_=0.f,human_=0.f,flex_=0.f,glide_=0.f,reference_=440.f,formant_=0.f,fade_=0.f,fadeStep_=1.f;
    int root_=0,scale_=0,range_=kDefaultRange,latency_=0,keySamples_=0;
    bool protectRange_=true,low_=true,enabled_=true,stabilize_=false,custom_=false,prepared_=false;
    uint16_t activeMask_=0;
    std::array<bool,12> mask_{};
    std::atomic<float> detected_{0.f},confidence_{0.f},cents_{0.f},referenceReadback_{440.f};
    std::atomic<int> target_{-1},keyRoot_{0},keyDistinct_{0};
    std::atomic<bool> keyMinor_{false},keyValid_{false};
    std::atomic<float> keyConfidence_{0.f},keyEvidence_{0.f},keyScore_{0.f},keyMargin_{0.f};
};
} // namespace humtune
