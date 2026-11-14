#include "PitchDetector.h"
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>
#ifdef NDEBUG
#error "Regression assertions must be active"
#endif
namespace humtune {
struct PitchDetectorDifferenceOracle {
 static float actual(const float* a,const float* b,int n) {return PitchDetector::squaredDifference(a,b,n);}
};
}
static float reference(const float* a,const float* b,int n) {
 float s0=0,s1=0,s2=0,s3=0;int j=0;
 for(;j+4<=n;j+=4) {const float d0=a[j]-b[j],d1=a[j+1]-b[j+1],d2=a[j+2]-b[j+2],d3=a[j+3]-b[j+3];s0+=d0*d0;s1+=d1*d1;s2+=d2*d2;s3+=d3*d3;}
 for(;j<n;++j) {const float d=a[j]-b[j];s0+=d*d;}
 return (s0+s1)+(s2+s3);
}
int main() {
 std::mt19937 rng(0x534b454c);std::uniform_real_distribution<float> random(-1,1);unsigned cases=0;
 for(float amplitude:{0.f,1e-20f,1e-4f,.1f,1.f,1e6f}) {
  // Exact-sized allocations make a four-lane over-read visible to ASan.
  for(int n:{0,1,2,3,4,5,7,8,15,31,32,33,49,75,127,150,256,343,400,512,693}) {
   for(int shift=0;shift<4;++shift) for(int other=0;other<4;++other) {
    std::vector<float>a(std::max(1,n+shift)),b(std::max(1,n+other));
    for(float& x:a)x=random(rng)*amplitude;for(float& x:b)x=random(rng)*amplitude;
    const float expected=reference(a.data()+shift,b.data()+other,n);
    const float actual=humtune::PitchDetectorDifferenceOracle::actual(a.data()+shift,b.data()+other,n);
    assert(std::bit_cast<std::uint32_t>(actual)==std::bit_cast<std::uint32_t>(expected));++cases;
   }
  }
 }
 std::printf("Bit-exact scalar/SIMD difference parity: %u cases\n",cases);
}
