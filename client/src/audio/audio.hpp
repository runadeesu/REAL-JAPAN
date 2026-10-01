#pragma once
// Procedural sound. No recorded, sampled or third-party audio is used: every sound is synthesised
// at run time from simple models of how it is produced.
//  * road vehicles: cylinder firing pulses exciting exhaust / intake resonances (engine order =
//    rpm/60 x cylinders/2), tyre-road noise, wind, tyre squeal when sliding; passing traffic with
//    distance, stereo position and Doppler shift
//  * trains: wheel impacts at the rail joints (25 m rails; each axle of each car, so the familiar
//    "ta-tan ... ta-tan" rhythm emerges from the bogie spacing), rolling rumble, the inverter /
//    traction-motor tones (asynchronous then synchronous PWM modes whose pitch steps as the speed
//    rises), gear whine; Shinkansen on long welded rail (no joint rhythm) with aerodynamic noise
//  * departure melody, door chime and on-board chimes: original tunes composed for this game,
//    played through a band-limited "PA speaker"
//  * ferry diesel and sea, ship's horn (one prolonged blast on leaving the berth)
//  * jet cabin noise (engine thrust, runway rumble), light aircraft engine / propeller, stall horn
//  * the phone's music player: tracks composed at run time from a seed (tempo, key, chord
//    progression, a pentatonic melody, bass, soft drums), and the player's guitar (plucked strings)
//  * city: traffic rumble, pedestrian-signal guide tones (bird-call style, as used at Japanese
//    crossings), crows and sparrows by day, rain, wind, footsteps; reverb underground
// All levels are game tuning values, not measured sound pressure levels.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace rjc {

struct CarSound {  // a road vehicle as heard by the listener
  bool on = false;
  int key = -1;          // vehicle id (keeps a voice on the same car)
  float rpm = 800, load = 0, speed = 0;  // engine speed, throttle 0..1, road speed (m/s)
  float slip = 0;        // tyres sliding 0..1 (squeal)
  int cylinders = 4;
  bool diesel = false;
  float gain = 1, pan = 0, doppler = 1;  // loudness at the listener, stereo position, pitch factor
  float muffle = 0;      // 0 open air .. 1 heard from inside a closed cabin
};

struct RailSound {  // a train as heard by the listener
  bool on = false;
  int key = -1;
  bool shinkansen = false;
  double s = 0;          // front of the train along its line (m)
  float v = 0;           // speed (m/s)
  int dir = 1;           // direction along the line
  int cars = 10;
  float car_len = 20;
  float traction = 0;    // -1 electric brake .. 1 full power
  float car_gain[16]{};  // loudness of each car's running gear at the listener
  float motor_gain = 0;  // loudness of the traction equipment (inverter, motors)
  float air_gain = 0;    // aerodynamic noise
  float curve = 0;       // track curvature (1/m) at the listener: flange squeal in tight curves
  float pan = 0, muffle = 0;
};

struct SoundScene {
  float master = 0.8f;
  float outdoor = 1;      // 0 inside a closed cabin .. 1 open air (city ambience, weather)
  float reverb = 0.05f;   // send to the room reverb (underground, concourse)
  float room = 0.6f;      // reverb size 0..1
  float traffic = 0;      // 0..1 nearby road traffic (distant rumble)
  float rain = 0, wind = 0;
  float daylight = 1;     // birds by day
  float step_rate = 0;    // footsteps per second
  int crossing = 0;       // pedestrian signal guide tone: 0 off, 1 "cuckoo" type, 2 "chirp" type
  float crossing_gain = 0, crossing_pan = 0;
  CarSound cars[5];       // [0] the player's car, [1..4] nearby traffic
  RailSound rail[3];      // [0] the train ridden (if any), others nearby
  float ship_engine = 0, ship_gain = 0, sea = 0;  // ferry engine load 0..1, loudness; waves
  float jet = 0, jet_gain = 0, jet_rumble = 0;     // jet thrust 0..1, loudness; runway rumble 0..1
  float prop_rpm = 0, prop_gain = 0, airflow = 0;  // light aircraft
  bool stall_horn = false;
  int music = -1;          // the phone's music player: track number (procedurally composed), -1 off
  float music_gain = 0;
  bool music_backing = false;  // (only the rhythm section: the band backing the player's guitar)
};

enum class Cue : int {
  DepartureMelody,  // platform melody before the doors close
  DoorChime,        // doors closing
  DoorAir,          // door engine (pneumatic)
  TrainChime,       // on-board chime before the next-stop announcement
  ShipHorn,
  CabinChime,       // airliner seat-belt sign
  GearThunk,        // landing gear up / down
  Shutter,          // camera
  Beep,             // barcode scanner / IC card reader
  GateBeep,         // ticket gate (IC card touch)
  Coins,
  ShrineBell,       // rope bell at the offering box
  Clap,
  Splash,
  Rattle,           // omikuji box
  Crash,
  Horn,             // car horn
  CrossingBell,     // level crossing warning bell (one stroke; rung twice a second)
  Click,            // phone tap
  Count
};

class Audio {
 public:
  static constexpr int kRate = 44100;
  Audio();
  ~Audio();
  // Opens the sound device and starts the stream; returns false (the game runs silently) when
  // there is none. `wav` (test aid): render offline, in step with the game, into this file.
  bool init(const std::filesystem::path& wav = {});
  void shutdown();
  bool active() const { return device_ || offline_; }
  void setScene(const SoundScene& s);
  void cue(Cue c, float gain = 1.0f, float pan = 0.0f);
  // a plucked string (the player's guitar: Karplus-Strong), MIDI note number
  void pluck(float midi, float gain = 1.0f);
  static int musicTracks() { return 6; }
  static float musicKey(int track) { return 52.0f + static_cast<float>((track * 5) % 9); }     // tonic (MIDI)
  static float musicBpm(int track) { return 78.0f + 8.0f * static_cast<float>((track * 3) % 6); }
  // offline rendering (test aid): produce `seconds` of sound into the WAV buffer
  void advanceOffline(double seconds);
  void render(float* stereo, int frames);  // audio thread
  // self-test: synthesise a car, a train, the departure melody for a moment without a device;
  // true when the output is audible, finite and within range
  static bool synthCheck();

 private:
  struct Impl;
  Impl* p_;
  bool device_ = false, offline_ = false;
  std::filesystem::path wav_path_;
  std::vector<int16_t> wav_;
  double offline_carry_ = 0;
};

}  // namespace rjc
