#pragma once
// The player's car: take the wheel of a stopped car (E), drive it, leave it parked (E).
//
// Vehicle dynamics (per type: mass, axle positions, centre-of-gravity height, yaw inertia, driven
// axle, engine torque, gear ratios, drag area):
//  * two-axle ("bicycle") model in the body frame: slip angles -> lateral tyre forces with a
//    saturating (Pacejka-shaped) curve and a friction circle shared with drive / brake forces
//  * longitudinal load transfer under acceleration and braking changes each axle's grip
//  * automatic gearbox (shift points follow the throttle), engine torque curve, engine braking,
//    aerodynamic drag and rolling resistance; brakes limited at the grip (ABS-like), reverse gear
//  * handbrake locks the rear wheels (the rear steps out); wet roads lower the friction
//  * body pitch / roll on springs from the accelerations (visual)
//  * collisions with buildings and other vehicles bounce the car off (restitution, friction)
// The numbers are generic values for each class of car, not any real model's specification.

#include "game/traffic.hpp"
#include "raylib.h"

namespace rjc {

class World;

struct DriveInput {
  float throttle = 0, brake = 0, steer = 0;  // 0..1, 0..1, -1 (left)..1 (right)
  bool handbrake = false;
};

struct CarSpec {
  double mass, a, b, h_cg, iz;   // kg, CoG to front / rear axle (m), CoG height (m), yaw inertia (kg m^2)
  double torque, rpm_peak, rpm_max, final_drive, wheel_r, cda;
  int gears;
  double ratio[6];
  bool front_drive, rear_drive;
  double vmax;                   // governor (m/s)
  double steer_max;              // road-wheel lock (rad)
};

const CarSpec& carSpec(VehicleType t);

// Driver's eye (right-hand drive) in the car's model frame: metres right of the centre line,
// forward of the car's origin, above the ground. Shared by the camera and the cockpit model.
struct DriverSeat {
  float side, fwd, up;
};
DriverSeat driverSeat(VehicleType t);

class Driving {
 public:
  bool active() const { return active_; }
  bool hasCar() const { return has_car_; }
  const Vehicle& car() const { return car_; }
  float speedKmh() const { return static_cast<float>(car_.v * 3.6); }
  float rpm() const { return static_cast<float>(rpm_); }
  int gear() const { return reverse_ ? -1 : gear_ + 1; }
  float steerAngle() const { return static_cast<float>(delta_); }  // road-wheel angle (rad)
  float roll() const { return static_cast<float>(roll_); }
  float bodyPitch() const { return static_cast<float>(pitch_body_); }
  float slip() const { return static_cast<float>(slip_); }          // 0..1 how hard the tyres are sliding
  void enter(const Vehicle& v);           // take the wheel
  void enterParked() { active_ = has_car_; }
  rj::geo::Vec3d exitPosition() const;   // driver's door (right-hand drive)
  void leave() {
    active_ = false;
    car_.v = 0;
    vx_ = vy_ = r_ = 0;
  }
  void drop() { active_ = has_car_ = false; }
  void update(double dt, const World& world, const Traffic& traffic, const DriveInput& in, float wetness = 0.0f);
  // chase camera (smoothed) or the driver's eye; look offsets from the mouse
  Camera3D camera(float fov, bool first_person, float look_yaw, float look_pitch) const;
  rj::geo::Vec3d driverEye() const;
  Camera3D rearCamera() const;  // looking back from the rear of the roof (mirror view)

 private:
  void step(double h, const DriveInput& in, double mu);
  void collide(const World& world, const Traffic& traffic, const rj::geo::Vec3d& before);
  void updateCamera(double dt);

  Vehicle car_{};
  bool active_ = false, has_car_ = false;
  double vx_ = 0, vy_ = 0, r_ = 0;  // body-frame velocity (forward, left) and yaw rate (compass sense)
  double delta_ = 0;                // road-wheel steering angle (positive = right)
  double ax_ = 0, ay_ = 0;          // body accelerations (filtered)
  int gear_ = 0;
  bool reverse_ = false;
  double rpm_ = 800;
  double shift_t_ = 0;
  double roll_ = 0, roll_v_ = 0, pitch_body_ = 0, pitch_v_ = 0;
  double slip_ = 0;
  rj::geo::Vec3d cam_pos_{};
  double cam_yaw_ = 0;
  bool cam_init_ = false;
};

DriveInput readDriveInput();

}  // namespace rjc
