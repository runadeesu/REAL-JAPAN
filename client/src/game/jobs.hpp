#pragma once
// Work the player can do (fictional island; game simplifications are labelled in the UI):
//  * Taxi driver: drive a taxi, pick up a waiting passenger at the kerb, take them to their
//    destination. The meter follows the Tokyo special-ward tariff structure (500 yen for the first
//    1.096 km, 100 yen per 255 m, time charge below 10 km/h, 20 % late-night surcharge 22-5 h);
//    the driver's share of the fare is a game assumption.
//  * Delivery: collect an order at a shop and take it to a home before the time runs out;
//    pay per delivery by distance (gig-style, game values).
// (The train driver, shop-till and shift jobs live in the app / trains code.)

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "rj/geo/local_frame.hpp"

namespace rjc {

class World;
class Traffic;

enum class JobKind : int { None = 0, Taxi, Delivery };

struct JobPlace {
  std::string name;    // building / POI name (may be empty)
  int usage = 0;       // building usage code (0 = street)
  rj::geo::Vec3d pos;  // where to stop / stand (kerb or door)
};

struct JobEvent {
  std::string key;  // language key
  std::map<std::string, std::string> args;
  int64_t pay = 0;  // credited to the player
};

class Jobs {
 public:
  JobKind kind() const { return kind_; }
  bool active() const { return kind_ != JobKind::None; }
  int stage() const { return stage_; }  // 0: going to pick up, 1: carrying
  const JobPlace& target() const { return stage_ == 0 ? pickup_ : dropoff_; }
  bool startTaxi(const World& world, const Traffic& traffic, const rj::geo::Vec3d& from);
  bool startDelivery(const World& world, const Traffic& traffic, const rj::geo::Vec3d& from);
  void stop() { kind_ = JobKind::None; }
  void shiftOrigin(const rj::geo::Rigid3d& X) {
    pickup_.pos = X.apply(pickup_.pos);
    dropoff_.pos = X.apply(dropoff_.pos);
    last_ = X.apply(last_);
  }
  std::vector<JobEvent> update(double dt, const World& world, const Traffic& traffic, const rj::geo::Vec3d& player, double speed_ms,
                               bool in_taxi, bool in_vehicle, int hour);
  int64_t meterYen(int hour) const;  // taxi fare so far
  double timeLeft() const { return limit_ - elapsed_; }
  double elapsed() const { return elapsed_; }
  int64_t earned() const { return earned_; }
  int count() const { return count_; }
  double distanceTo(const rj::geo::Vec3d& p) const;

 private:
  bool pickTaxiRide(const World& world, const Traffic& traffic, const rj::geo::Vec3d& from);
  bool randomBuilding(const World& world, const rj::geo::Vec3d& from, double rmin, double rmax, bool shop, JobPlace& out);
  bool kerbNear(const Traffic& traffic, const rj::geo::Vec3d& q, rj::geo::Vec3d& out) const;
  uint32_t rnd();
  JobKind kind_ = JobKind::None;
  int stage_ = 0;
  JobPlace pickup_, dropoff_;
  double meter_m_ = 0, meter_slow_s_ = 0;
  double elapsed_ = 0, limit_ = 0, stopped_s_ = 0;
  double trip_m_ = 0;
  rj::geo::Vec3d last_{};
  bool last_ok_ = false;
  int64_t earned_ = 0;
  int count_ = 0;
  uint32_t rng_ = 0x9e3779b9u;
};

}  // namespace rjc
