#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
namespace humtune {
// Experimental causal whitening of already recorded epoch fingerprints.
// This is not a glottal-closure ground-truth detector or SEDREAMS implementation.
class EpochWhitening {
public:
    void prepare(double rate,int maxSpan=0,int maxRadius=0) {
        sr=rate;stride=std::max(1,static_cast<int>(std::lround(rate/16000.)));
        window=std::clamp(static_cast<int>(std::lround(.025*rate/stride)),128,512);
        ring.resize(4096);residual.resize(2048);win.resize(static_cast<size_t>(window));frame.resize(static_cast<size_t>(window));
        maxSpan=maxSpan>0 ? maxSpan : std::max(24,static_cast<int>(std::lround(512*rate/44100.)));
        maxRadius=maxRadius>0 ? maxRadius : std::max(2,static_cast<int>(std::lround(48*rate/44100.)));
        matchPrevious.resize(static_cast<size_t>(maxSpan));
        matchCurrent.resize(static_cast<size_t>(maxSpan+2*maxRadius));
        double sum=0;
        for(int i=0;i<31;++i) {
            const int t=i-15;const double cutoff=.4/stride;
            const double sinc=t==0 ? 2*cutoff : std::sin(6.283185307179586*cutoff*t)/(3.141592653589793*t);
            const double phase=6.283185307179586*i/30;
            fir[static_cast<size_t>(i)]=sinc*(.42-.5*std::cos(phase)+.08*std::cos(2*phase));sum+=fir[static_cast<size_t>(i)];
        }
        for(auto& x:fir)x/=sum;
        for(int i=0;i<window;++i)win[static_cast<size_t>(i)]=.5-.5*std::cos(6.283185307179586*i/(window-1));
        reset();
    }
    double delaySamples() const noexcept {return 15+3*stride;}
    void reset() noexcept {
        history.fill(0);position=phase=0;written=0;latest=-1;lastModel=-1000000;valid=false;count=0;matchSpan=0;
    }
    void feed(float sample,int64_t time) noexcept {
        if(ring.empty())return;
        history[static_cast<size_t>(position)]=sample;if(++position==31)position=0;
        if(++phase<stride)return;phase=0;double value=0;int at=position;
        for(int i=0;i<31;++i){value+=history[static_cast<size_t>(at)]*fir[static_cast<size_t>(i)];if(++at==31)at=0;}
        ring[static_cast<size_t>(written&4095)]=static_cast<float>(value);++written;latest=time;
    }
    bool prepareRegion(double first,double last,int64_t now) noexcept {
        if(now-lastModel>=std::max<int64_t>(1,static_cast<int64_t>(.004*sr)))fit(now);
        if(!valid || latest<0)return false;
        // The extra three analysis samples allow bounded cubic interpolation
        // using only available history, even on a partial decimation interval.
        const double a=grid(first),b=grid(last);
        begin=static_cast<int64_t>(std::floor(a))-1;
        const int64_t end=static_cast<int64_t>(std::floor(b))+2;
        count=static_cast<int>(end-begin+1);
        if(count<=0 || count>static_cast<int>(residual.size()) || begin-order<std::max<int64_t>(0,written-4096) || end>=written)return false;
        for(int i=0;i<count;++i) {
            const int64_t at=begin+i;double value=read(at);
            for(int k=1;k<=order;++k)value+=coeff[static_cast<size_t>(k)]*read(at-k);
            residual[static_cast<size_t>(i)]=static_cast<float>(value);
        }
        return true;
    }
    double value(double time) const noexcept {
        const double p=grid(time)-begin;const int i=static_cast<int>(std::floor(p));const double f=p-i;
        if(i<1 || i+2>=count)return 0;
        const double a=residual[static_cast<size_t>(i-1)],b=residual[static_cast<size_t>(i)],c=residual[static_cast<size_t>(i+1)],d=residual[static_cast<size_t>(i+2)];
        return b+.5*f*(c-a+f*(2*a-5*b+4*c-d+f*(3*(b-c)+d-a)));
    }
    bool prepareMatch(double previous,double expected,int span,int radius,int step,int64_t now) noexcept {
        matchSpan=0;
        if(span<=0 || radius<0 || step<=0 || static_cast<size_t>(span)>matchPrevious.size()
           || static_cast<size_t>(span+2*radius)>matchCurrent.size()
           || !prepareRegion(previous-span,expected+radius,now))return false;
        // Every integer-shift candidate reuses the same fractional grid. Cache
        // each interpolated value once, retaining doubles and the original NCC
        // summation order. Storage is allocated only by prepare().
        matchPreviousPower=0;
        for(int j=0;j<span;j+=step) {
            const double sample=value(previous-j);matchPrevious[static_cast<size_t>(j)]=sample;
            matchPreviousPower+=sample*sample;
        }
        for(int offset=-(span-1)-radius;offset<=radius;++offset)
            matchCurrent[static_cast<size_t>(offset+span-1+radius)]=value(expected+offset);
        matchSpan=span;matchRadius=radius;matchStep=step;return true;
    }
    double matchScore(int shift) const noexcept {
        if(matchSpan<=0 || std::abs(shift)>matchRadius)return -1;
        double xx=0,xy=0;
        const int base=matchSpan-1+matchRadius+shift;
        for(int j=0;j<matchSpan;j+=matchStep) {
            const double a=matchCurrent[static_cast<size_t>(base-j)],b=matchPrevious[static_cast<size_t>(j)];
            xx+=a*a;xy+=a*b;
        }
        return xx>1e-12 && matchPreviousPower>1e-12 ? xy/std::sqrt(xx*matchPreviousPower) : -1.;
    }
private:
    static constexpr int order=12;
    float read(int64_t at) const noexcept {return ring[static_cast<size_t>(at&4095)];}
    double grid(double time) const noexcept {return written-1+(time-latest)/stride-3;}
    void fit(int64_t now) noexcept {
        lastModel=now;valid=false;
        if(written<window+order+4)return;
        std::array<double,order+1> covariance{},next{};
        for(int i=0;i<window;++i)frame[static_cast<size_t>(i)]=read(written-window+i)*win[static_cast<size_t>(i)];
        for(int k=0;k<=order;++k)for(int i=k;i<window;++i)covariance[static_cast<size_t>(k)]+=frame[static_cast<size_t>(i)]*frame[static_cast<size_t>(i-k)];
        if(covariance[0]<1e-7)return;
        covariance[0]*=1.0001;coeff.fill(0);coeff[0]=1;double error=covariance[0];
        for(int k=1;k<=order;++k) {
            double sum=covariance[static_cast<size_t>(k)];
            for(int j=1;j<k;++j)sum+=coeff[static_cast<size_t>(j)]*covariance[static_cast<size_t>(k-j)];
            const double reflection=std::clamp(-sum/std::max(error,covariance[0]*1e-7),-.98,.98);
            next=coeff;next[static_cast<size_t>(k)]=reflection;
            for(int j=1;j<k;++j)next[static_cast<size_t>(j)]=coeff[static_cast<size_t>(j)]+reflection*coeff[static_cast<size_t>(k-j)];
            coeff=next;error*=1-reflection*reflection;
        }
        if(!std::isfinite(error) || error<=0)return;
        // A nearly sinusoidal residual is insufficient excitation evidence:
        // inverse filtering can fit away those few predictable partials.
        // Qualify the residual's shape before letting it refine a source mark.
        double second=0,fourth=0;
        for(int i=0;i<window;++i) {
            const int64_t at=written-window+i;double value=read(at);
            for(int k=1;k<=order;++k)value+=coeff[static_cast<size_t>(k)]*read(at-k);
            const double square=value*value;second+=square;fourth+=square*square;
        }
        valid=second>1e-20 && window*fourth>2.5*second*second;
    }
    double sr=44100;int stride=3,window=368,position=0,phase=0,count=0;
    int64_t written=0,latest=-1,lastModel=-1000000,begin=0;bool valid=false;
    std::array<double,31> fir{};std::array<float,31> history{};std::vector<double> win,frame;
    std::array<double,order+1> coeff{};std::vector<float> ring,residual;
    std::vector<double> matchPrevious,matchCurrent;
    int matchSpan=0,matchRadius=0,matchStep=1;double matchPreviousPower=0;
};
}
