#pragma once
// Small DSP building blocks for the procedural sound engine (audio.cpp): noise, one-pole and
// biquad filters, smoothed parameters, a light reverb. Everything is float, per sample.

#include <cmath>
#include <cstdint>

namespace rjc::dsp {

constexpr float kPi = 3.14159265358979f;
constexpr float kTau = 6.28318530717959f;

struct Rng {
  uint32_t s = 0x2545f491u;
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
  float uni() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }  // 0..1
  float white() { return uni() * 2.0f - 1.0f; }                                    // -1..1
};

// Pink-ish noise (Paul Kellet's economy filter), roughly -3 dB/octave.
struct Pink {
  float b0 = 0, b1 = 0, b2 = 0;
  float run(float w) {
    b0 = 0.99765f * b0 + w * 0.0990460f;
    b1 = 0.96300f * b1 + w * 0.2965164f;
    b2 = 0.57000f * b2 + w * 1.0526913f;
    return (b0 + b1 + b2 + w * 0.1848f) * 0.18f;
  }
};

// Brown (red) noise: leaky integrator, -6 dB/octave.
struct Brown {
  float y = 0;
  float run(float w) {
    y = y * 0.996f + w * 0.06f;
    return y * 2.2f;
  }
};

struct OnePole {
  float a = 1, y = 0;
  void cutoff(float hz, float sr) { a = 1.0f - std::exp(-kTau * hz / sr); }
  float lp(float x) { return y += a * (x - y); }
  float hp(float x) { return x - lp(x); }
};

// RBJ biquad.
struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
  void set(float nb0, float nb1, float nb2, float a0, float na1, float na2) {
    b0 = nb0 / a0;
    b1 = nb1 / a0;
    b2 = nb2 / a0;
    a1 = na1 / a0;
    a2 = na2 / a0;
  }
  void bandpass(float hz, float q, float sr) {  // constant 0 dB peak gain
    const float w = kTau * hz / sr, al = std::sin(w) / (2 * q), c = std::cos(w);
    set(al, 0, -al, 1 + al, -2 * c, 1 - al);
  }
  void lowpass(float hz, float q, float sr) {
    const float w = kTau * hz / sr, al = std::sin(w) / (2 * q), c = std::cos(w);
    set((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
  }
  void highpass(float hz, float q, float sr) {
    const float w = kTau * hz / sr, al = std::sin(w) / (2 * q), c = std::cos(w);
    set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
  }
  float run(float x) {  // transposed direct form II
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

// Parameter smoothing (per sample) so that frame-rate updates do not click.
struct Smooth {
  float v = 0, a = 0.001f;
  void time(float seconds, float sr) { a = 1.0f - std::exp(-1.0f / (seconds * sr)); }
  float step(float target) { return v += a * (target - v); }
};

struct Osc {
  float ph = 0;  // 0..1
  float step(float hz, float sr) {
    ph += hz / sr;
    ph -= std::floor(ph);
    return ph;
  }
  float sine(float hz, float sr) { return std::sin(kTau * step(hz, sr)); }
};

// Equal-power stereo pan, p in -1 (left) .. 1 (right).
inline void pan(float x, float p, float& l, float& r) {
  const float a = (p + 1.0f) * 0.25f * kPi;
  l += x * std::cos(a);
  r += x * std::sin(a);
}

// A compact Schroeder/Freeverb-style reverb (4 combs + 2 all-passes per channel).
class Reverb {
 public:
  void init(float sr) {
    static const int kComb[4] = {1116, 1188, 1277, 1356}, kAp[2] = {556, 441};
    const float k = sr / 44100.0f;
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 4; ++i) comb_[c][i].len = static_cast<int>((kComb[i] + c * 23) * k);
      for (int i = 0; i < 2; ++i) ap_[c][i].len = static_cast<int>((kAp[i] + c * 23) * k);
    }
  }
  void setRoom(float feedback, float damp) {
    fb_ = feedback;
    damp_ = damp;
  }
  void run(float in, float& l, float& r) {
    float o[2] = {0, 0};
    for (int c = 0; c < 2; ++c) {
      for (auto& cb : comb_[c]) {
        float& y = cb.buf[cb.i];
        const float out = y;
        cb.store = out * (1 - damp_) + cb.store * damp_;
        y = in + cb.store * fb_;
        if (++cb.i >= cb.len) cb.i = 0;
        o[c] += out;
      }
      for (auto& a : ap_[c]) {
        float& y = a.buf[a.i];
        const float bo = y;
        y = o[c] + bo * 0.5f;
        o[c] = bo - o[c];
        if (++a.i >= a.len) a.i = 0;
      }
    }
    l += o[0] * 0.25f;
    r += o[1] * 0.25f;
  }

 private:
  struct Comb {
    float buf[1600]{};
    int len = 1116, i = 0;
    float store = 0;
  };
  struct Ap {
    float buf[700]{};
    int len = 556, i = 0;
  };
  Comb comb_[2][4];
  Ap ap_[2][2];
  float fb_ = 0.8f, damp_ = 0.3f;
};

// Short echo (a single reflection off a nearby wall / the hills), used for horns and chimes.
class Echo {
 public:
  float run(float x, float delay_s, float fb, float sr) {
    const int d = static_cast<int>(delay_s * sr);
    const int n = d < kLen ? d : kLen - 1;
    int j = i_ - n;
    if (j < 0) j += kLen;
    const float y = buf_[j];
    buf_[i_] = x + y * fb;
    if (++i_ >= kLen) i_ = 0;
    return y;
  }

 private:
  static constexpr int kLen = 48000;
  float buf_[kLen]{};
  int i_ = 0;
};

}  // namespace rjc::dsp
