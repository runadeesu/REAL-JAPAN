// Offline renders of the procedural sound engine for checking and tuning (not part of the game).
// usage: rj_audio_demo <out-dir>   -> car.wav, train.wav, shinkansen.wav, street.wav, ferry.wav,
//                                     jet.wav, plane.wav, cues.wav
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>

#include "audio/audio.hpp"

using namespace rjc;

static void render(const std::filesystem::path& out, double seconds, const std::function<void(double, SoundScene&, Audio&)>& script) {
  Audio a;
  a.init(out);
  const double dt = 1.0 / 60.0;
  for (double t = 0; t < seconds; t += dt) {
    SoundScene sc;
    sc.traffic = 0.0f;
    sc.daylight = 0.0f;
    script(t, sc, a);
    a.setScene(sc);
    a.advanceOffline(dt);
  }
  a.shutdown();
  std::printf("%s\n", out.string().c_str());
}

int main(int argc, char** argv) {
  const std::filesystem::path dir = argc > 1 ? argv[1] : ".";
  std::filesystem::create_directories(dir);
  // car: idle, full-throttle run through the gears, lift off, brake with a slide
  render(dir / "car.wav", 22.0, [](double t, SoundScene& sc, Audio&) {
    CarSound& c = sc.cars[0];
    c.on = true;
    c.key = 1;
    c.gain = 1.0f;
    c.muffle = 0.2f;
    static double v = 0, rpm = 800;
    static int gear = 0;
    const double ratio[5] = {3.6, 2.1, 1.4, 1.0, 0.8};
    const bool power = t > 3 && t < 15;
    if (power) v += (3.2 - 0.4 * gear) / 60.0;
    else if (t >= 15 && t < 17) v *= 0.999;
    else if (t >= 17) v = std::max(0.0, v - 7.0 / 60.0);
    rpm = std::max(800.0, v / 0.31 * ratio[gear] * 4.1 * 60 / (2 * M_PI));
    if (rpm > 6200 && gear < 4) ++gear;
    if (rpm < 1500 && gear > 0 && !power) --gear;
    c.rpm = static_cast<float>(rpm);
    c.load = power ? 1.0f : 0.05f;
    c.speed = static_cast<float>(v);
    c.slip = t > 18 && t < 19.5 ? 0.6f : 0.0f;
  });
  // commuter train (listener seated in car 3 of 10): start, notch up, coast, brake to a stop
  render(dir / "train.wav", 60.0, [](double t, SoundScene& sc, Audio& a) {
    RailSound& r = sc.rail[0];
    static double s = 500, v = 0;
    double acc = 0;
    float tr = 0;
    if (t > 2 && t < 30) { acc = 0.8 * std::max(0.3, 1.0 - v / 30.0); tr = 1.0f; }
    else if (t >= 30 && t < 40) { acc = -0.02; tr = 0; }
    else if (t >= 40) { acc = v > 0 ? -0.9 : 0; tr = v > 0.5 ? -0.8f : 0.0f; }
    v = std::max(0.0, v + acc / 60.0);
    s += v / 60.0;
    r.on = true;
    r.key = 7;
    r.s = s;
    r.v = static_cast<float>(v);
    r.cars = 10;
    r.car_len = 20;
    r.traction = tr;
    for (int k = 0; k < 10; ++k) r.car_gain[k] = 1.0f / (1.0f + std::pow(std::fabs(k - 3) * 20.0f / 8.0f, 2.0f));
    r.motor_gain = 0.8f;
    r.muffle = 0.45f;
    sc.outdoor = 0.1f;
    if (t > 0.9 && t < 0.92) a.cue(Cue::DoorChime, 0.6f);
    if (t > 44 && t < 44.02) a.cue(Cue::TrainChime, 0.5f);
  });
  render(dir / "shinkansen.wav", 12.0, [](double t, SoundScene& sc, Audio&) {
    RailSound& r = sc.rail[0];
    r.on = true;
    r.key = 2;
    r.shinkansen = true;
    r.s = 1000 + t * 70.0;
    r.v = 70.0f;
    r.cars = 16;
    r.car_len = 25;
    r.traction = 0.3f;
    for (int k = 0; k < 16; ++k) r.car_gain[k] = 1.0f / (1.0f + std::pow(std::fabs(k - 5) * 25.0f / 8.0f, 2.0f));
    r.motor_gain = 0.6f;
    r.air_gain = 0.6f;
    r.muffle = 0.6f;
    sc.outdoor = 0.05f;
  });
  // street corner: traffic passing both ways, pedestrian signal tones, birds, footsteps
  render(dir / "street.wav", 30.0, [](double t, SoundScene& sc, Audio&) {
    sc.traffic = 0.6f;
    sc.daylight = 1.0f;
    sc.reverb = 0.08f;
    sc.step_rate = t < 8 ? 1.8f : 0.0f;
    sc.crossing = t > 10 && t < 24 ? (t < 17 ? 1 : 2) : 0;
    sc.crossing_gain = 0.8f;
    sc.crossing_pan = 0.4f;
    for (int i = 1; i <= 3; ++i) {
      CarSound& c = sc.cars[i];
      const double t0 = i * 6.0, v = 12.0 + i * 2;
      const double x = (t - t0) * v * (i % 2 ? 1 : -1), y = 4.0 + i * 2;
      const double d = std::hypot(x, y);
      c.on = std::fabs(x) < 150;
      c.key = 10 + i;
      c.rpm = 1800;
      c.load = 0.3f;
      c.speed = static_cast<float>(v);
      c.cylinders = i == 3 ? 6 : 4;
      c.diesel = i == 3;
      c.gain = static_cast<float>(1.0 / (1.0 + d / 5.0));
      c.pan = static_cast<float>(x / (d + 1.0));
      const double vx = v * (i % 2 ? 1 : -1), dd = x * vx / (d + 1e-3);  // rate of change of the distance
      c.doppler = static_cast<float>(343.0 / (343.0 + dd));
    }
  });
  render(dir / "ferry.wav", 16.0, [](double t, SoundScene& sc, Audio& a) {
    sc.outdoor = 1.0f;
    sc.wind = 0.4f;
    sc.ship_gain = 0.6f;
    sc.ship_engine = t < 6 ? 0.2f : 0.8f;
    sc.sea = 0.5f;
    if (t > 1 && t < 1.02) a.cue(Cue::ShipHorn, 0.8f);
  });
  render(dir / "jet.wav", 40.0, [](double t, SoundScene& sc, Audio& a) {
    sc.outdoor = 0.0f;
    sc.jet_gain = 0.7f;
    sc.jet = t < 8 ? 0.2f : (t < 30 ? 1.0f : 0.8f);
    const double v = t < 8 ? 5 : std::min(80.0, (t - 8) * 2.2);
    sc.jet_rumble = t < 26 ? static_cast<float>(std::min(1.0, v / 60.0)) : 0.0f;
    if (t > 2 && t < 2.02) a.cue(Cue::CabinChime, 0.6f);
    if (t > 30 && t < 30.02) a.cue(Cue::GearThunk, 0.7f);
  });
  render(dir / "plane.wav", 16.0, [](double, SoundScene& sc, Audio&) {
    sc.outdoor = 0.2f;
    sc.prop_gain = 0.7f;
    sc.prop_rpm = 2400.0f;
    sc.airflow = 0.7f;
  });
  render(dir / "cues.wav", 26.0, [](double t, SoundScene&, Audio& a) {
    const Cue order[] = {Cue::DepartureMelody, Cue::DoorAir, Cue::Shutter, Cue::Beep, Cue::GateBeep, Cue::Coins, Cue::ShrineBell,
                         Cue::Clap, Cue::Splash, Cue::Rattle, Cue::Crash, Cue::Horn, Cue::Click};
    const double at[] = {0.1, 8, 9.5, 10.5, 11.5, 12.5, 14, 16, 17, 18.5, 20.5, 22, 23.5};
    for (int i = 0; i < 13; ++i)
      if (t >= at[i] && t < at[i] + 1.0 / 60.0) a.cue(order[i], 0.8f);
  });
  return 0;
}
