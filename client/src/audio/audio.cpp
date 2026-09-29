#include "audio/audio.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>

#include "audio/synth.hpp"
#include "raylib.h"

namespace rjc {
namespace {

using namespace dsp;
constexpr float SR = static_cast<float>(Audio::kRate);

float midiHz(float m) { return 440.0f * std::pow(2.0f, (m - 69.0f) / 12.0f); }
float decay(float t, float tau) { return t < 0 ? 0 : std::exp(-t / tau); }
float attackDecay(float t, float att, float tau) { return t < 0 ? 0 : (t < att ? t / att : std::exp(-(t - att) / tau)); }

// ---------------------------------------------------------------------------------------------
// Road vehicle: firing pulses through exhaust / intake resonances + tyres + wind + squeal.
struct EngineVoice {
  int key = -2;
  Smooth rpm, load, speed, gain, pan, dop, muffle, slip;
  double fire = 0, half = 0;
  int cyl = 0;
  float exc = 0, exc_k = 0;
  float cylw[8]{};
  Biquad r1, r2, r3, tyre, clat;
  OnePole wind1, wind2, m1, m2;
  Osc squeal, sq_lfo;
  bool diesel = false;
  int ncyl = 4;

  void setup(bool dsl, int n, Rng& rng) {
    diesel = dsl;
    ncyl = std::clamp(n, 2, 8);
    for (int i = 0; i < 8; ++i) cylw[i] = 0.85f + 0.3f * rng.uni();
    if (diesel) {
      r1.bandpass(75, 2.5f, SR);
      r2.bandpass(190, 3.0f, SR);
      r3.bandpass(420, 3.5f, SR);
      clat.bandpass(2300, 1.4f, SR);
    } else {
      r1.bandpass(105, 2.2f, SR);
      r2.bandpass(260, 3.0f, SR);
      r3.bandpass(620, 4.0f, SR);
      clat.bandpass(1500, 1.0f, SR);
    }
    tyre.bandpass(750, 0.7f, SR);
    wind1.cutoff(500, SR);
    wind2.cutoff(500, SR);
    exc_k = std::exp(-1.0f / (0.0016f * SR));
    for (Smooth* s : {&rpm, &load, &speed, &pan, &dop, &muffle, &slip}) s->time(0.05f, SR);
    gain.time(0.12f, SR);
  }

  // returns the mono signal; `p` receives the stereo position
  float run(const CarSound& c, Rng& rng, float& p) {
    const float g = gain.step(c.on ? c.gain : 0.0f);
    if (g < 1e-4f && !c.on) return 0.0f;
    const float r = rpm.step(c.rpm), ld = load.step(c.load), v = speed.step(c.speed), d = dop.step(c.doppler);
    const float mf = muffle.step(c.muffle), sl = slip.step(c.slip);
    p = pan.step(c.pan);
    // firing: one pulse per cylinder event, uneven strength per cylinder (the "character")
    const double ff = r / 60.0 * ncyl * 0.5 * d;
    fire += ff / SR;
    half += ff * 0.5 / SR;
    if (half >= 1.0) half -= 1.0;
    if (fire >= 1.0) {
      fire -= 1.0;
      cyl = (cyl + 1) % ncyl;
      exc = (0.75f + 0.5f * rng.uni()) * cylw[cyl] * (0.35f + 0.65f * ld);
    }
    const float e = exc * (1.0f + 0.35f * rng.white());
    exc *= exc_k;
    float x = r1.run(e) * 5.0f + r2.run(e) * 3.2f + r3.run(e) * 1.6f * (0.4f + ld);
    if (diesel) x += clat.run(e * rng.white()) * 2.2f;  // injection / combustion knock
    else x += clat.run(rng.white()) * 0.04f * ld * (r / 6000.0f);  // intake roar
    const float fp = static_cast<float>(fire), hp = static_cast<float>(half);
    x += (std::sin(kTau * fp) * 0.10f + std::sin(kTau * 2 * fp + 0.6f) * 0.05f + std::sin(kTau * hp) * 0.035f) * (0.5f + 0.5f * ld);
    x *= 0.55f + 0.45f * std::min(1.0f, r / 3000.0f);
    // tyres on asphalt, wind, squeal
    const float vr = std::max(0.0f, v);
    x += tyre.run(rng.white()) * 0.55f * std::pow(std::min(vr / 25.0f, 1.6f), 1.4f);
    const float w = wind2.lp(wind1.lp(rng.white()));
    x += w * 1.2f * std::pow(std::min(vr / 35.0f, 1.5f), 2.0f);
    if (sl > 0.12f) {
      const float f = 820.0f + 70.0f * std::sin(kTau * sq_lfo.step(6.5f, SR)) + 40.0f * rng.white();
      x += std::sin(kTau * squeal.step(f, SR)) * 0.22f * std::min(1.0f, (sl - 0.12f) * 3.0f);
    }
    // cabin: the body and glass take the highs out
    const float fc = 7000.0f * (1 - mf) + 650.0f * mf;
    m1.cutoff(fc, SR);
    m2.cutoff(fc, SR);
    return m2.lp(m1.lp(x)) * g;
  }
};

// ---------------------------------------------------------------------------------------------
// Train: joint impacts per axle, rolling noise, traction tones, aerodynamic noise.
struct RailVoice {
  int key = -2;
  double s = 0;
  bool synced = false;
  Smooth v, traction, motor, air, pan, muffle, total;
  float car_gain[16]{};
  struct Hit {
    int delay;
    float amp, t;
  };
  std::array<Hit, 48> hits{};
  int nhits = 0;
  Brown brown;
  Pink pink;
  Biquad rumble, roar, airf, click;
  OnePole m1, m2;
  Osc carrier, sb_lo, sb_hi, harm, gear, squeal[2];
  Smooth squeal_amt;
  float sq_f[2] = {2900.0f, 4300.0f}, sq_drift = 0;
  int mode = -1;

  void setup() {
    rumble.lowpass(170, 0.8f, SR);
    roar.bandpass(620, 0.8f, SR);
    airf.bandpass(1000, 0.5f, SR);
    click.highpass(1400, 0.7f, SR);
    for (Smooth* sm : {&v, &traction, &motor, &air, &pan, &muffle, &total}) sm->time(0.08f, SR);
    traction.time(0.25f, SR);
    squeal_amt.time(0.4f, SR);
  }

  // Joint crossings during the next `frames` samples (25 m rails; Shinkansen: long welded rail).
  void scheduleJoints(const RailSound& r, int frames, Rng& rng) {
    if (r.shinkansen || r.v < 0.3f) return;
    const double J = 25.0, L = r.car_len;
    const double ds = r.dir * static_cast<double>(r.v) * frames / SR;
    const float speed_amp = std::min(1.3f, std::pow(r.v / 20.0f, 0.8f));
    for (int k = 0; k < std::min(r.cars, 16); ++k) {
      if (car_gain[k] < 0.002f) continue;
      const double bogie = L * 0.345;  // bogie centres 13.8 m apart on a 20 m car
      const double offs[4] = {k * L + 0.5 * L - bogie - 1.05, k * L + 0.5 * L - bogie + 1.05, k * L + 0.5 * L + bogie - 1.05,
                              k * L + 0.5 * L + bogie + 1.05};
      for (double b : offs) {
        const double a0 = s - r.dir * b, a1 = a0 + ds;
        const double j0 = std::floor(a0 / J), j1 = std::floor(a1 / J);
        if (j0 == j1 || nhits >= static_cast<int>(hits.size())) continue;
        const double jx = (r.dir > 0 ? j1 : j0) * J;
        const double f = std::clamp((jx - a0) / (a1 - a0), 0.0, 1.0);
        hits[static_cast<size_t>(nhits++)] = Hit{static_cast<int>(f * frames), car_gain[k] * speed_amp * (0.8f + 0.4f * rng.uni()), 0.0f};
      }
    }
  }

  float run(const RailSound& r, Rng& rng, float& p) {
    const float g = total.step(r.on ? 1.0f : 0.0f);
    if (g < 1e-4f && !r.on) return 0.0f;
    const float vv = v.step(r.v), tr = traction.step(r.traction), mg = motor.step(r.motor_gain), ag = air.step(r.air_gain);
    const float mf = muffle.step(r.muffle);
    p = pan.step(r.pan);
    s += r.dir * static_cast<double>(vv) / SR;
    float sum_g = 0;
    for (int k = 0; k < std::min(r.cars, 16); ++k) sum_g += car_gain[k];
    sum_g = std::min(sum_g, 2.5f);
    // wheel impacts at the joints: a sharp click and a heavy thump
    float x = 0;
    for (int i = 0; i < nhits;) {
      Hit& h = hits[static_cast<size_t>(i)];
      if (h.delay > 0) {
        --h.delay;
        ++i;
        continue;
      }
      const float t = h.t;
      x += h.amp * (click.run(rng.white()) * 0.8f * decay(t, 0.003f) + rng.white() * 0.25f * decay(t, 0.006f) +
                    std::sin(kTau * 68.0f * t) * 1.1f * decay(t, 0.035f) + std::sin(kTau * 163.0f * t) * 0.45f * decay(t, 0.018f));
      h.t += 1.0f / SR;
      if (h.t > 0.25f) hits[static_cast<size_t>(i)] = hits[static_cast<size_t>(--nhits)];
      else ++i;
    }
    // rolling: low rumble + wheel-rail roar
    const float vn = std::max(0.0f, vv);
    const float sp = r.shinkansen ? vn / 40.0f : vn / 22.0f;
    x += rumble.run(brown.run(rng.white())) * 0.9f * sum_g * std::pow(std::min(sp, 1.8f), 1.1f);
    x += roar.run(pink.run(rng.white())) * 0.45f * sum_g * std::pow(std::min(sp, 1.8f), 1.6f);
    x += airf.run(rng.white()) * ag * 0.5f * std::pow(std::min(vn / 70.0f, 1.4f), 2.5f);
    // traction equipment: inverter carrier (asynchronous at low speed, then synchronous modes
    // with 15 / 9 / 5 / 3 pulses whose pitch rises with speed and drops at each change) and
    // the motor's own harmonics; gear-mesh whine
    float y = 0;
    const float kmh = vn * 3.6f;
    const float fo = kmh * 1.31f + (tr > 0 ? 1.5f : -1.0f);
    const float at = std::fabs(tr);
    if (mg > 1e-4f && at > 0.03f && (vn > 0.2f || tr > 0.0f)) {
      int m;
      float fc;
      // (asynchronous carrier rising 700 -> 1050 Hz up to ~38 km/h, then synchronous modes)
      const float base = r.shinkansen ? 1.4f : 1.0f;
      if (fo < 50.0f) { m = 0; fc = (700.0f + 7.0f * std::max(fo, 0.0f)) * base; }
      else if (fo < 65.0f) { m = 1; fc = 15.0f * fo; }
      else if (fo < 90.0f) { m = 2; fc = 9.0f * fo; }
      else if (fo < 120.0f) { m = 3; fc = 5.0f * fo; }
      else if (fo < 150.0f) { m = 4; fc = 3.0f * fo; }
      else { m = 5; fc = 0.0f; }
      mode = m;
      const float lvl = mg * std::pow(at, 0.7f) * (tr > 0 ? 1.0f : 0.7f) * (r.shinkansen ? 0.5f : 1.0f);
      if (fc > 0.0f) {
        const float fsb = std::max(fo, 3.0f) * 2.0f;
        y += (std::sin(kTau * carrier.step(fc, SR)) + 0.45f * std::sin(kTau * sb_lo.step(std::max(40.0f, fc - fsb), SR)) +
              0.45f * std::sin(kTau * sb_hi.step(fc + fsb, SR))) * 0.09f * lvl;
      }
      y += std::sin(kTau * harm.step(std::max(fo, 2.0f) * 6.0f, SR)) * 0.05f * lvl;
    }
    const float axle_rps = vn / (kPi * 0.86f);
    y += std::sin(kTau * gear.step(axle_rps * 87.0f, SR)) * 0.03f * mg * std::min(1.0f, vn / 15.0f) * (0.4f + 0.6f * at);
    // flange squeal: wheels grinding round a tight curve (radius under ~300 m), two unsteady tones
    const float radius = r.curve > 1e-5f ? 1.0f / r.curve : 1e5f;
    const float sq = squeal_amt.step(r.shinkansen ? 0.0f : std::clamp((300.0f - radius) / 200.0f, 0.0f, 1.0f) * std::clamp((vn - 2.0f) / 6.0f, 0.0f, 1.0f));
    if (sq > 1e-3f) {
      sq_drift += (rng.white() * 30.0f - sq_drift * 0.001f) / SR;
      float s2 = 0;
      for (int k = 0; k < 2; ++k) s2 += std::sin(kTau * squeal[k].step(sq_f[k] + sq_drift * (k ? 1.4f : 1.0f), SR)) * (k ? 0.5f : 1.0f);
      x += s2 * 0.06f * sq * sum_g * (0.6f + 0.4f * std::sin(kTau * 0.7f * static_cast<float>(s)));
    }
    const float fc = 9000.0f * (1 - mf) + 900.0f * mf;
    m1.cutoff(fc, SR);
    m2.cutoff(fc, SR);
    return (m2.lp(m1.lp(x)) + y * (1.0f - 0.5f * mf)) * g;
  }
};

// ---------------------------------------------------------------------------------------------
// One-shot sounds (chimes, melody, horn, effects).
struct Note {
  float t, midi, dur;
};

// Original platform melody (composed for this game): four bars of eighth notes at 138 bpm.
const std::vector<Note>& departureMelody() {
  static std::vector<Note> n = [] {
    const int m[] = {74, 78, 81, 86, 85, 81, 78, 81, 83, 79, 76, 79, 81, 78, 74, 76, 78, 81, 86, 88, 86, 85, 83, 81, 79, 83, 81, 78, 74};
    std::vector<Note> v;
    const float e = 60.0f / 138.0f / 2.0f;
    for (int i = 0; i < 29; ++i) v.push_back({i * e, static_cast<float>(m[i]), i == 28 ? e * 4 : e * 1.6f});
    return v;
  }();
  return n;
}

struct CueVoice {
  Cue c;
  float t = 0, gain = 1, pan = 0, dur = 1;
  uint32_t seed = 1;
  Biquad bp1, bp2;
  OnePole lp;
  std::vector<std::pair<float, float>> hits;  // time, pitch factor (bells, coins, rattle)
};

float bell(float t, float f, float tau) {  // glockenspiel-like partials
  if (t < 0) return 0;
  const float a = std::min(1.0f, t / 0.002f);
  return a * (std::sin(kTau * f * t) * decay(t, tau) + 0.35f * std::sin(kTau * f * 2.756f * t) * decay(t, tau * 0.4f) +
              0.15f * std::sin(kTau * f * 5.404f * t) * decay(t, tau * 0.2f));
}

float hornTone(float t, float f) {  // saw-like reed tone (sum of harmonics)
  float y = 0;
  for (int k = 1; k <= 12; ++k) y += std::sin(kTau * f * k * t + k * 0.3f) / static_cast<float>(k);
  return y;
}

void setupCue(CueVoice& v, Rng& rng) {
  v.seed = rng.next() | 1u;
  switch (v.c) {
    case Cue::DepartureMelody: v.dur = 7.0f; v.bp1.highpass(380, 0.7f, SR); v.bp2.lowpass(4200, 0.9f, SR); break;
    case Cue::DoorChime: v.dur = 2.6f; v.bp1.highpass(380, 0.7f, SR); v.bp2.lowpass(4200, 0.9f, SR); break;
    case Cue::TrainChime: v.dur = 1.8f; v.bp1.highpass(300, 0.7f, SR); v.bp2.lowpass(5000, 0.8f, SR); break;
    case Cue::DoorAir: v.dur = 1.0f; v.bp1.bandpass(2800, 0.8f, SR); break;
    case Cue::ShipHorn: v.dur = 6.8f; v.lp.cutoff(1400, SR); break;
    case Cue::CabinChime: v.dur = 1.6f; break;
    case Cue::GearThunk: v.dur = 1.2f; v.lp.cutoff(300, SR); break;
    case Cue::Shutter: v.dur = 0.2f; v.bp1.highpass(1800, 0.7f, SR); break;
    case Cue::Beep: v.dur = 0.09f; break;
    case Cue::GateBeep: v.dur = 0.14f; break;
    case Cue::CrossingBell: v.dur = 0.45f; break;
    case Cue::Coins:
      v.dur = 0.8f;
      for (int i = 0; i < 4; ++i) v.hits.push_back({0.07f * i + 0.04f * rng.uni(), 0.9f + 0.2f * rng.uni()});
      break;
    case Cue::ShrineBell:
      v.dur = 1.8f;
      for (float t = 0; t < 1.2f; t += 0.015f + 0.05f * rng.uni() * (0.3f + t)) v.hits.push_back({t, 0.93f + 0.14f * rng.uni()});
      break;
    case Cue::Clap: v.dur = 0.35f; v.bp1.bandpass(1300, 0.9f, SR); break;
    case Cue::Splash: v.dur = 0.9f; v.lp.cutoff(1800, SR); break;
    case Cue::Rattle:
      v.dur = 1.1f;
      v.bp1.bandpass(2400, 1.5f, SR);
      for (int i = 0; i < 7; ++i) v.hits.push_back({0.13f * i + 0.04f * rng.uni(), 0.8f + 0.4f * rng.uni()});
      break;
    case Cue::Crash: v.dur = 0.9f; v.lp.cutoff(2200, SR); break;
    case Cue::Horn: v.dur = 0.42f; v.lp.cutoff(2500, SR); break;
    case Cue::Click: v.dur = 0.03f; break;
    default: v.dur = 0.1f; break;
  }
}

float runCue(CueVoice& v, Rng& rng) {
  const float t = v.t;
  float y = 0;
  switch (v.c) {
    case Cue::DepartureMelody: {
      for (const Note& n : departureMelody())
        if (t >= n.t && t < n.t + n.dur + 1.2f) y += bell(t - n.t, midiHz(n.midi), 0.55f) * 0.32f;
      const float root = t < 1.74f ? 50 : (t < 3.48f ? 55 : (t < 5.22f ? 52 : 50));  // soft bass
      y += std::sin(kTau * midiHz(root) * t) * 0.10f * std::min(1.0f, t / 0.05f) * (t > 6.0f ? decay(t - 6.0f, 0.3f) : 1.0f);
      y = v.bp2.run(v.bp1.run(y));
      y = std::tanh(y * 1.6f) * 0.7f;  // small PA horn speaker
      break;
    }
    case Cue::DoorChime: {
      for (int i = 0; i < 3; ++i) {
        const float t0 = i * 0.8f;
        y += bell(t - t0, midiHz(88), 0.35f) * 0.4f + bell(t - t0 - 0.3f, midiHz(84), 0.45f) * 0.4f;
      }
      y = std::tanh(v.bp2.run(v.bp1.run(y)) * 1.5f) * 0.7f;
      break;
    }
    case Cue::TrainChime: {
      const float n[3] = {76, 81, 85};
      for (int i = 0; i < 3; ++i) y += bell(t - i * 0.32f, midiHz(n[i]), 0.6f) * 0.35f;
      y = v.bp2.run(v.bp1.run(y));
      break;
    }
    case Cue::DoorAir:
      y = v.bp1.run(rng.white()) * 0.8f * attackDecay(t, 0.03f, 0.25f) + std::sin(kTau * 90.0f * t) * 0.4f * decay(t - 0.75f, 0.04f) * (t > 0.75f);
      break;
    case Cue::ShipHorn: {
      const float env = std::min(1.0f, t / 0.35f) * (t > 5.5f ? decay(t - 5.5f, 0.35f) : 1.0f);
      const float f = 122.0f * (1.0f - 0.04f * decay(t, 0.25f));
      y = v.lp.lp(hornTone(t, f) + 0.5f * hornTone(t, f * 1.498f)) * 0.35f * env;
      break;
    }
    case Cue::CabinChime:
      y = (std::sin(kTau * 880.0f * t) + 0.25f * std::sin(kTau * 1760.0f * t)) * 0.3f * attackDecay(t, 0.004f, 0.5f);
      break;
    case Cue::GearThunk:
      y = std::sin(kTau * 55.0f * t) * 0.8f * decay(t, 0.09f) + v.lp.lp(rng.white()) * 1.2f * attackDecay(t, 0.02f, 0.4f);
      break;
    case Cue::Shutter:
      y = v.bp1.run(rng.white()) * (decay(t, 0.005f) + 0.7f * decay(t - 0.07f, 0.008f) * (t > 0.07f)) * 0.9f;
      break;
    case Cue::Beep: y = (std::sin(kTau * 3100.0f * t) > 0 ? 0.18f : -0.18f) * (t < 0.08f); break;
    case Cue::GateBeep: y = std::sin(kTau * 2300.0f * t) * 0.35f * (t < 0.12f) * std::min(1.0f, t / 0.003f); break;
    case Cue::CrossingBell:  // an electronic level crossing bell: a bright struck tone with inharmonic partials
      y = (std::sin(kTau * 740.0f * t) + 0.55f * std::sin(kTau * 1580.0f * t) + 0.3f * std::sin(kTau * 2410.0f * t)) * 0.22f *
          attackDecay(t, 0.002f, 0.16f);
      break;
    case Cue::Coins:
      for (auto& h : v.hits)
        if (t >= h.first) {
          const float u = t - h.first;
          y += (std::sin(kTau * 3200 * h.second * u) + 0.6f * std::sin(kTau * 5130 * h.second * u) + 0.4f * std::sin(kTau * 7700 * h.second * u)) *
               0.12f * decay(u, 0.12f);
        }
      break;
    case Cue::ShrineBell:
      for (auto& h : v.hits)
        if (t >= h.first && t < h.first + 0.6f) {
          const float u = t - h.first;
          y += (std::sin(kTau * 2100 * h.second * u) + 0.7f * std::sin(kTau * 3380 * h.second * u) + 0.4f * std::sin(kTau * 5250 * h.second * u)) *
               0.07f * decay(u, 0.11f) * decay(h.first, 0.8f);
        }
      break;
    case Cue::Clap: y = v.bp1.run(rng.white()) * 1.4f * decay(t, 0.012f); break;
    case Cue::Splash: y = v.lp.lp(rng.white()) * 0.9f * attackDecay(t, 0.01f, 0.18f); break;
    case Cue::Rattle:
      for (auto& h : v.hits)
        if (t >= h.first) {
          const float u = t - h.first;
          y += v.bp1.run(rng.white()) * 0.5f * decay(u, 0.012f) + std::sin(kTau * 900 * h.second * u) * 0.15f * decay(u, 0.02f);
        }
      break;
    case Cue::Crash:
      y = std::sin(kTau * 48.0f * t) * 1.0f * decay(t, 0.15f) + v.lp.lp(rng.white()) * 1.1f * decay(t, 0.2f) +
          (std::sin(kTau * 1310.0f * t) + std::sin(kTau * 2090.0f * t)) * 0.12f * decay(t, 0.3f);
      break;
    case Cue::Horn: {
      const float env = std::min(1.0f, t / 0.02f) * (t > 0.36f ? decay(t - 0.36f, 0.02f) : 1.0f);
      y = v.lp.lp(hornTone(t, 415.0f) + hornTone(t, 498.0f)) * 0.22f * env;
      break;
    }
    case Cue::Click: y = std::sin(kTau * 2900.0f * t) * 0.15f * decay(t, 0.004f); break;
    default: break;
  }
  v.t += 1.0f / SR;
  return y * v.gain;
}

// ---------------------------------------------------------------------------------------------
// City and nature: traffic rumble, wind, rain, pedestrian-signal tones, birds, footsteps, ship, aircraft.
struct Ambient {
  Brown brown, brown2;
  Pink pink, pink2;
  Biquad traffic_lp, traffic_hum, wind_bp, rain_hp1, rain_hp2, roof_lp, sea_lp, sea_wash, ship_r1, ship_r2, ship_r3, ship_mech,
      jet_lp, jet_mid, rumble_lp, prop_r1, prop_r2, prop_r3, air_bp, stall_bp, step_lp, step_hp, caw1, caw2;
  Smooth traffic, wind, rain, outdoor, xgain, ship_e, ship_g, sea, jet, jet_g, rumble, prop_rpm, prop_g, airflow, daylight;
  float gust = 0, gust_t = 0;
  // crossing tone
  float cross_t = 0;
  int cross_type = 0;
  // birds
  float crow_next = 8.0f, crow_t = -1, crow_pan = 0, crow_amp = 0;
  int crow_n = 0;
  float spar_next = 4.0f, spar_t = -1, spar_pan = 0;
  int spar_n = 0;
  // footsteps
  float step_ph = 0, step_t = 10, step_amp = 0;
  int step_side = 0;
  // ship / prop pulses
  double ship_fire = 0, prop_fire = 0;
  float ship_exc = 0, prop_exc = 0;
  // raindrops
  float drop_t = 10, drop_f = 3000, drop_amp = 0;
  Osc fan, stall;

  void setup() {
    traffic_lp.lowpass(230, 0.7f, SR);
    traffic_hum.bandpass(900, 0.6f, SR);
    wind_bp.bandpass(330, 0.6f, SR);
    rain_hp1.highpass(2200, 0.7f, SR);
    rain_hp2.lowpass(9000, 0.7f, SR);
    roof_lp.lowpass(1100, 0.7f, SR);
    sea_lp.lowpass(300, 0.7f, SR);
    sea_wash.bandpass(700, 0.5f, SR);
    ship_r1.bandpass(46, 2.5f, SR);
    ship_r2.bandpass(96, 3.0f, SR);
    ship_r3.bandpass(190, 3.0f, SR);
    ship_mech.bandpass(420, 0.9f, SR);
    jet_lp.lowpass(700, 0.7f, SR);
    jet_mid.bandpass(2600, 1.2f, SR);
    rumble_lp.lowpass(140, 0.8f, SR);
    prop_r1.bandpass(110, 2.0f, SR);
    prop_r2.bandpass(330, 3.0f, SR);
    prop_r3.bandpass(900, 3.0f, SR);
    air_bp.bandpass(600, 0.5f, SR);
    stall_bp.bandpass(1600, 4.0f, SR);
    step_lp.lowpass(480, 0.8f, SR);
    step_hp.highpass(3000, 0.7f, SR);
    caw1.bandpass(1150, 1.6f, SR);
    caw2.bandpass(1900, 2.0f, SR);
    for (Smooth* s : {&traffic, &wind, &rain, &outdoor, &xgain, &ship_e, &ship_g, &sea, &jet, &jet_g, &rumble, &prop_rpm, &prop_g, &airflow, &daylight})
      s->time(0.3f, SR);
    jet.time(1.5f, SR);  // engines spool slowly
    prop_rpm.time(0.4f, SR);
  }

  void run(const SoundScene& sc, Rng& rng, float& l, float& r, float& send) {
    const float od = outdoor.step(sc.outdoor), tf = traffic.step(sc.traffic), wd = wind.step(sc.wind), rn = rain.step(sc.rain);
    const float dl = daylight.step(sc.daylight);
    // distant city traffic
    float m = traffic_lp.run(brown.run(rng.white())) * (0.05f + 0.28f * tf) * (0.25f + 0.75f * od);
    m += traffic_hum.run(pink.run(rng.white())) * 0.06f * tf * od;
    // wind in gusts
    gust_t -= 1.0f / SR;
    if (gust_t <= 0) {
      gust_t = 0.5f + 2.0f * rng.uni();
      gust = 0.4f + 0.6f * rng.uni();
    }
    gs += (gust - gs) * 0.00005f;
    m += wind_bp.run(rng.white()) * 0.35f * wd * gs * (0.2f + 0.8f * od);
    // rain: hiss and drops outdoors, drumming on the roof inside a vehicle
    float rl = 0, rr = 0;
    if (rn > 0.01f) {
      const float h = rain_hp2.run(rain_hp1.run(rng.white())) * 0.25f * std::pow(rn, 0.8f) * od;
      rl += h * (0.8f + 0.2f * rng.uni());
      rr += h * (0.8f + 0.2f * rng.uni());
      if (rng.uni() < rn * 40.0f / SR) {
        drop_t = 0;
        drop_f = 1800.0f + 3000.0f * rng.uni();
        drop_amp = 0.04f * (0.3f + rng.uni());
      }
      if (drop_t < 0.05f) {
        const float d = std::sin(kTau * drop_f * (1.0f + 3.0f * drop_t) * drop_t) * drop_amp * decay(drop_t, 0.012f) * od;
        rl += d * 0.7f;
        rr += d * 0.3f;
      }
      drop_t += 1.0f / SR;
      if (rng.uni() < rn * 500.0f / SR * (1.0f - od)) roof_imp = (0.5f + rng.uni()) * rn;
      m += roof_lp.run(roof_imp) * 1.6f * (1.0f - od);
      roof_imp *= 0.6f;
    }
    // pedestrian signal guide tones: bird-call style, two speakers answering across the road
    const float xg = xgain.step(sc.crossing ? sc.crossing_gain : 0.0f);
    if (sc.crossing) cross_type = sc.crossing;
    if (xg > 1e-4f) {
      cross_t += 1.0f / SR;
      float y = 0, p = sc.crossing_pan;
      if (cross_type == 1) {  // "ka-kkoo" then the far side answers "ka-ka-kkoo"
        const float period = 3.0f, u = std::fmod(cross_t, period);
        const bool far = u >= 1.5f;
        const float w = far ? u - 1.5f : u;
        auto note = [&](float t0, float d, float f) {
          const float x = w - t0;
          if (x < 0 || x > d) return 0.0f;
          const float e = std::min(1.0f, x / 0.01f) * std::min(1.0f, (d - x) / 0.03f);
          const float ff = f * (1.0f + 0.01f * std::sin(kTau * 6.0f * x));
          return (std::sin(kTau * ff * x) + 0.25f * std::sin(kTau * 2 * ff * x)) * e;
        };
        if (far) y = note(0.0f, 0.09f, 1040) + note(0.16f, 0.09f, 1040) + note(0.32f, 0.38f, 830);
        else y = note(0.0f, 0.11f, 1040) + note(0.18f, 0.38f, 830);
        if (far) p = -p;
        y *= 0.16f;
      } else {  // "piyo" / "piyo-piyo"
        const float period = 2.0f, u = std::fmod(cross_t, period);
        const bool far = u >= 1.0f;
        const float w = far ? u - 1.0f : u;
        auto piyo = [&](float t0) {
          const float x = w - t0;
          if (x < 0 || x > 0.2f) return 0.0f;
          const float f = x < 0.1f ? 2300.0f + 10000.0f * x : 3300.0f - 8000.0f * (x - 0.1f);
          const float e = std::min(1.0f, x / 0.008f) * std::min(1.0f, (0.2f - x) / 0.02f);
          return std::sin(kTau * f * x) * e;
        };
        y = piyo(0.0f) + (far ? piyo(0.28f) : 0.0f);
        if (far) p = -p;
        y *= 0.12f;
      }
      pan(y * xg, p, l, r);
      send += y * xg * 0.3f;
    } else {
      cross_t = 0;
    }
    // crows and sparrows by day (outdoors)
    if (od > 0.3f && dl > 0.3f) {
      crow_next -= 1.0f / SR;
      if (crow_next <= 0 && crow_t < 0) {
        crow_t = 0;
        crow_n = 2 + static_cast<int>(rng.uni() * 2.0f);
        crow_pan = rng.white() * 0.8f;
        crow_amp = 0.03f + 0.07f * rng.uni();
        crow_next = 25.0f + 50.0f * rng.uni();
      }
      spar_next -= 1.0f / SR;
      if (spar_next <= 0 && spar_t < 0) {
        spar_t = 0;
        spar_n = 2 + static_cast<int>(rng.uni() * 4.0f);
        spar_pan = rng.white() * 0.9f;
        spar_next = 5.0f + 12.0f * rng.uni();
      }
    }
    if (crow_t >= 0) {
      const float per = 0.45f;
      const int i = static_cast<int>(crow_t / per);
      const float u = crow_t - i * per;
      if (i < crow_n && u < 0.3f) {
        const float f0 = 540.0f - 90.0f * u;
        float saw = 0;
        for (int k = 1; k <= 10; ++k) saw += std::sin(kTau * f0 * k * crow_t) / static_cast<float>(k);
        const float e = std::min(1.0f, u / 0.02f) * std::min(1.0f, (0.3f - u) / 0.08f);
        const float y = (caw1.run(saw + 0.4f * rng.white()) + 0.6f * caw2.run(saw)) * crow_amp * e * od;
        pan(y, crow_pan, l, r);
        send += y * 0.5f;
      }
      crow_t += 1.0f / SR;
      if (crow_t > crow_n * per) crow_t = -1;
    }
    if (spar_t >= 0) {
      const float per = 0.16f;
      const int i = static_cast<int>(spar_t / per);
      const float u = spar_t - i * per;
      if (i < spar_n && u < 0.06f) {
        const float f = 4200.0f + 1400.0f * std::sin(kPi * u / 0.06f);
        const float y = std::sin(kTau * f * u) * 0.025f * std::sin(kPi * u / 0.06f) * od;
        pan(y, spar_pan, l, r);
      }
      spar_t += 1.0f / SR;
      if (spar_t > spar_n * per) spar_t = -1;
    }
    // footsteps on paving
    if (sc.step_rate > 0.1f) {
      step_ph += sc.step_rate / SR;
      if (step_ph >= 1.0f) {
        step_ph -= 1.0f;
        step_t = 0;
        step_amp = 0.8f + 0.4f * rng.uni();
        step_side ^= 1;
      }
    }
    if (step_t < 0.12f) {
      const float n = rng.white();
      const float y = (step_lp.run(n) * 0.9f * decay(step_t, 0.03f) + step_hp.run(n) * 0.25f * decay(step_t, 0.012f)) * step_amp * 0.35f;
      pan(y, step_side ? 0.12f : -0.12f, l, r);
      send += y;
      step_t += 1.0f / SR;
    }
    // ferry: medium-speed diesel (firing pulses into low resonances), machinery, sea
    const float sg = ship_g.step(sc.ship_gain), se = ship_e.step(sc.ship_engine), sv = sea.step(sc.sea);
    if (sg > 1e-4f) {
      const double ff = (380.0 + 360.0 * se) / 60.0 * 4.0;  // 8 cylinders, four-stroke
      ship_fire += ff / SR;
      if (ship_fire >= 1.0) {
        ship_fire -= 1.0;
        ship_exc = 0.8f + 0.4f * rng.uni();
      }
      const float e = ship_exc * (1.0f + 0.3f * rng.white());
      ship_exc *= 0.9985f;
      const float y = (ship_r1.run(e) * 6.0f + ship_r2.run(e) * 3.0f + ship_r3.run(e) * 1.2f) * (0.45f + 0.55f * se) +
                      ship_mech.run(pink2.run(rng.white())) * 0.25f * (0.5f + se);
      m += y * sg;
    }
    if (sv > 1e-4f) {
      swell_ph += 1.0f / (7.0f * SR);
      if (swell_ph > 1) swell_ph -= 1;
      const float sw = 0.6f + 0.4f * std::sin(kTau * swell_ph);
      const float y = sea_lp.run(brown2.run(rng.white())) * 0.8f * sw + sea_wash.run(rng.white()) * 0.12f * sw * sw;
      m += y * sv;
    }
    // jet cabin: broadband engine / airflow noise, fan tone, runway rumble
    const float jg = jet_g.step(sc.jet_gain), jt = jet.step(sc.jet), rb = rumble.step(sc.jet_rumble);
    if (jg > 1e-4f) {
      const float n1 = 0.22f + 0.78f * jt;
      float y = jet_lp.run(pink2.run(rng.white())) * 1.3f * (0.35f + 0.65f * jt) + jet_mid.run(rng.white()) * 0.05f * jt;
      y += std::sin(kTau * fan.step(2350.0f * n1, SR)) * 0.012f * n1;
      y += rumble_lp.run(brown2.run(rng.white())) * 1.5f * rb * (0.7f + 0.6f * rng.uni());
      m += y * jg;
    }
    // light aircraft: engine / propeller (blade passing and firing at rpm/60 x 2), airflow, stall horn
    const float pg = prop_g.step(sc.prop_gain), pr = prop_rpm.step(sc.prop_rpm), af = airflow.step(sc.airflow);
    if (pg > 1e-4f) {
      const double ff = pr / 60.0 * 2.0;
      prop_fire += ff / SR;
      if (prop_fire >= 1.0) {
        prop_fire -= 1.0;
        prop_exc = 0.8f + 0.4f * rng.uni();
      }
      const float e = prop_exc * (1.0f + 0.3f * rng.white());
      prop_exc *= 0.996f;
      float y = prop_r1.run(e) * 5.0f + prop_r2.run(e) * 3.0f + prop_r3.run(e) * 1.2f;
      y += air_bp.run(rng.white()) * 0.3f * af * af;
      if (sc.stall_horn) y += stall_bp.run(std::sin(kTau * stall.step(1600.0f, SR)) > 0 ? 1.0f : -1.0f) * 0.25f;
      m += y * pg;
    }
    l += m;
    r += m;
    l += rl;
    r += rr;
    send += m * 0.2f;
  }
  float roof_imp = 0, gs = 0.5f, swell_ph = 0;
};

}  // namespace

// ---------------------------------------------------------------------------------------------
struct Audio::Impl {
  std::mutex mu;
  SoundScene pending;
  bool has_pending = false;
  struct QCue {
    Cue c;
    float gain, pan;
  };
  std::vector<QCue> queue;
  SoundScene sc;
  Rng rng;
  EngineVoice cars[5];
  RailVoice rail[3];
  Ambient amb;
  std::vector<CueVoice> cues;
  Reverb rev;
  Echo echo;
  Smooth master, send_amt, room;
  AudioStream stream{};

  Impl() {
    for (auto& c : cars) c.setup(false, 4, rng);
    for (auto& r : rail) r.setup();
    amb.setup();
    rev.init(SR);
    master.time(0.2f, SR);
    send_amt.time(0.5f, SR);
    room.time(0.5f, SR);
    cues.reserve(32);
  }

  void render(float* out, int frames) {
    {
      std::lock_guard<std::mutex> lk(mu);
      if (has_pending) {
        sc = pending;
        has_pending = false;
      }
      for (const QCue& q : queue) {
        if (cues.size() >= 24) break;
        CueVoice v;
        v.c = q.c;
        v.gain = q.gain;
        v.pan = q.pan;
        setupCue(v, rng);
        cues.push_back(std::move(v));
      }
      queue.clear();
    }
    // (re)assign voices to the vehicles / trains in the scene
    for (int i = 0; i < 5; ++i) {
      const CarSound& c = sc.cars[i];
      if (c.on && c.key != cars[i].key) {
        cars[i].setup(c.diesel, c.cylinders, rng);
        cars[i].key = c.key;
        cars[i].rpm.v = c.rpm;
        cars[i].speed.v = c.speed;
        cars[i].dop.v = c.doppler;
        cars[i].pan.v = c.pan;
        cars[i].muffle.v = c.muffle;
        cars[i].gain.v = i == 0 ? c.gain : 0.0f;
      }
    }
    for (int i = 0; i < 3; ++i) {
      const RailSound& t = sc.rail[i];
      RailVoice& rv = rail[i];
      if (!t.on) continue;
      if (t.key != rv.key || std::fabs(t.s - rv.s) > 4.0) {
        if (t.key != rv.key) {
          rv.v.v = t.v;
          rv.traction.v = t.traction;
          rv.muffle.v = t.muffle;
          rv.motor.v = t.motor_gain;
          rv.nhits = 0;
        }
        rv.key = t.key;
        rv.s = t.s;
      } else {
        rv.s += (t.s - rv.s) * 0.02;
      }
      for (int k = 0; k < 16; ++k) rv.car_gain[k] = t.car_gain[k];
      rv.scheduleJoints(t, frames, rng);
    }
    const float rm = std::clamp(sc.room, 0.0f, 1.0f);
    rev.setRoom(0.70f + 0.18f * rm, 0.35f - 0.15f * rm);
    for (int n = 0; n < frames; ++n) {
      float l = 0, r = 0, send = 0;
      for (int i = 0; i < 5; ++i) {
        float p = 0;
        const float x = cars[i].run(sc.cars[i], rng, p);
        if (x != 0.0f) {
          pan(x, p, l, r);
          send += x * 0.3f;
        }
      }
      for (int i = 0; i < 3; ++i) {
        float p = 0;
        const float x = rail[i].run(sc.rail[i], rng, p);
        if (x != 0.0f) {
          pan(x, p, l, r);
          send += x * 0.3f;
        }
      }
      amb.run(sc, rng, l, r, send);
      float echo_in = 0;
      for (size_t i = 0; i < cues.size();) {
        CueVoice& v = cues[i];
        const float x = runCue(v, rng);
        pan(x, v.pan, l, r);
        send += x * 0.6f;
        if (v.c == Cue::ShipHorn || v.c == Cue::DepartureMelody || v.c == Cue::DoorChime) echo_in += x;
        if (v.t > v.dur) {
          cues[i] = std::move(cues.back());
          cues.pop_back();
        } else {
          ++i;
        }
      }
      const float e = echo.run(echo_in, 0.42f, 0.28f, SR) * 0.35f;
      l += e;
      r += e;
      rev.run(send * send_amt.step(sc.reverb), l, r);
      const float mv = master.step(std::clamp(sc.master, 0.0f, 1.0f));
      out[2 * n] = std::tanh(l * mv * 0.9f);
      out[2 * n + 1] = std::tanh(r * mv * 0.9f);
    }
  }
};

namespace {
Audio* g_audio = nullptr;
void streamCallback(void* buffer, unsigned int frames) {
  if (g_audio) g_audio->render(static_cast<float*>(buffer), static_cast<int>(frames));
  else std::memset(buffer, 0, frames * 2 * sizeof(float));
}
}  // namespace

Audio::Audio() : p_(new Impl()) {}
Audio::~Audio() {
  shutdown();
  delete p_;
}

bool Audio::init(const std::filesystem::path& wav) {
  if (!wav.empty()) {
    offline_ = true;
    wav_path_ = wav;
    TraceLog(LOG_INFO, "RJ: audio rendered offline to %s", wav.string().c_str());
    return true;
  }
  InitAudioDevice();
  if (!IsAudioDeviceReady()) {
    TraceLog(LOG_WARNING, "RJ: no audio device, running silently");
    return false;
  }
  SetAudioStreamBufferSizeDefault(1024);
  p_->stream = LoadAudioStream(kRate, 32, 2);
  g_audio = this;
  SetAudioStreamCallback(p_->stream, streamCallback);
  PlayAudioStream(p_->stream);
  device_ = true;
  return true;
}

void Audio::shutdown() {
  if (device_) {
    StopAudioStream(p_->stream);
    UnloadAudioStream(p_->stream);
    g_audio = nullptr;
    CloseAudioDevice();
    device_ = false;
  }
  if (offline_ && !wav_.empty()) {
    std::ofstream f(wav_path_, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = static_cast<uint32_t>(wav_.size() * 2);
    f.write("RIFF", 4);
    u32(36 + bytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);
    u32(kRate);
    u32(kRate * 4);
    u16(4);
    u16(16);
    f.write("data", 4);
    u32(bytes);
    f.write(reinterpret_cast<const char*>(wav_.data()), static_cast<std::streamsize>(bytes));
    TraceLog(LOG_INFO, "RJ: audio written (%.1f s)", wav_.size() / 2.0 / kRate);
    wav_.clear();
  }
  offline_ = false;
}

void Audio::setScene(const SoundScene& s) {
  if (!active()) return;
  std::lock_guard<std::mutex> lk(p_->mu);
  p_->pending = s;
  p_->has_pending = true;
}

void Audio::cue(Cue c, float gain, float pan) {
  if (!active()) return;
  std::lock_guard<std::mutex> lk(p_->mu);
  if (p_->queue.size() < 16) p_->queue.push_back({c, gain, std::clamp(pan, -1.0f, 1.0f)});
}

void Audio::advanceOffline(double seconds) {
  if (!offline_) return;
  offline_carry_ += seconds * kRate;
  int frames = static_cast<int>(offline_carry_);
  offline_carry_ -= frames;
  std::vector<float> buf;
  while (frames > 0) {
    const int n = std::min(frames, 1024);
    buf.assign(static_cast<size_t>(n) * 2, 0.0f);
    p_->render(buf.data(), n);
    for (float x : buf) wav_.push_back(static_cast<int16_t>(std::clamp(x, -1.0f, 1.0f) * 32767.0f));
    frames -= n;
  }
}

void Audio::render(float* stereo, int frames) { p_->render(stereo, frames); }

bool Audio::synthCheck() {
  Impl im;
  SoundScene sc;
  sc.cars[0].on = true;
  sc.cars[0].key = 1;
  sc.cars[0].rpm = 3000;
  sc.cars[0].load = 0.8f;
  sc.cars[0].speed = 15;
  sc.rail[1].on = true;
  sc.rail[1].key = 2;
  sc.rail[1].v = 15;
  sc.rail[1].traction = 1;
  sc.rail[1].motor_gain = 0.5f;
  for (float& g : sc.rail[1].car_gain) g = 0.3f;
  im.pending = sc;
  im.has_pending = true;
  im.queue.push_back({Cue::DepartureMelody, 0.5f, 0.0f});
  std::vector<float> buf(2048 * 2);
  double sum = 0;
  float peak = 0;
  for (int b = 0; b < 22; ++b) {
    im.render(buf.data(), 2048);
    for (float x : buf) {
      if (!std::isfinite(x)) return false;
      sum += static_cast<double>(x) * x;
      peak = std::max(peak, std::fabs(x));
    }
  }
  const double rms = std::sqrt(sum / (22.0 * 4096.0));
  return rms > 0.01 && peak <= 1.0f;
}

}  // namespace rjc
