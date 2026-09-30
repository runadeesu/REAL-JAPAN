#include "game/driving.hpp"

#include <algorithm>
#include <cmath>

#include "world/coords.hpp"
#include "world/world.hpp"

namespace rjc {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kG = 9.81;
double wrap(double a) {
  while (a > kPi) a -= 2 * kPi;
  while (a < -kPi) a += 2 * kPi;
  return a;
}
double sgn(double x) { return x > 0 ? 1.0 : x < 0 ? -1.0 : 0.0; }

// Generic class values (kg, m, N m, rpm). Japanese market: kei cars 660 cc turbo (64 PS limit),
// sedans / taxis / minivans ~2 L to 2.5 L, 2 t cab-over trucks and city buses diesel.
//                        mass     a     b    hcg   Iz     torque rpmpk rpmmax final  r_w   CdA  n  ratios                           F      R      vmax  lock
const CarSpec kSedan   {1520,  1.28, 1.52, 0.55, 2650,  250, 4400, 6600, 3.9, 0.315, 0.66, 6, {3.54, 2.06, 1.40, 1.00, 0.71, 0.58}, false, true,  55.0, 0.62};
const CarSpec kTaxi    {1390,  1.18, 1.57, 0.60, 2300,  142, 4000, 5600, 3.7, 0.30,  0.72, 4, {2.83, 1.49, 1.00, 0.73, 0, 0},       true,  false, 45.0, 0.64};
const CarSpec kKei     { 880,  1.02, 1.43, 0.62, 1050,  100, 3000, 6800, 4.4, 0.27,  0.62, 4, {2.60, 1.52, 1.00, 0.72, 0, 0},       true,  false, 39.0, 0.66};
const CarSpec kMinivan {1690,  1.32, 1.53, 0.68, 3100,  235, 4100, 6200, 3.6, 0.33,  0.85, 6, {3.30, 2.00, 1.35, 1.00, 0.78, 0.63}, true,  false, 50.0, 0.62};
const CarSpec kVan     {1880,  1.22, 1.35, 0.75, 3300,  300, 2400, 4200, 3.9, 0.315, 0.95, 6, {3.60, 2.10, 1.40, 1.00, 0.72, 0.61}, false, true,  44.0, 0.64};
const CarSpec kTruck   {4200,  1.05, 2.25, 0.95, 11000, 410, 1800, 3200, 4.8, 0.38,  3.6,  6, {5.98, 3.39, 2.06, 1.40, 1.00, 0.75}, false, true,  30.5, 0.62};
const CarSpec kBus     {11500, 3.30, 1.95, 1.15, 70000, 1270, 1400, 2500, 5.3, 0.48, 6.0,  6, {4.10, 2.40, 1.50, 1.00, 0.76, 0.60}, false, true,  25.0, 0.70};
}  // namespace

const CarSpec& carSpec(VehicleType t) {
  switch (t) {
    case VehicleType::Taxi: return kTaxi;
    case VehicleType::Kei: return kKei;
    case VehicleType::Minivan: return kMinivan;
    case VehicleType::Van: return kVan;
    case VehicleType::Truck: return kTruck;
    case VehicleType::Bus: return kBus;
    default: return kSedan;
  }
}

DriverSeat driverSeat(VehicleType t) {
  switch (t) {
    case VehicleType::Kei: return {0.3f, -0.25f, 1.28f};
    case VehicleType::Taxi: return {0.35f, -0.45f, 1.3f};
    case VehicleType::Minivan: return {0.38f, 0.0f, 1.4f};
    case VehicleType::Van: return {0.38f, 1.2f, 1.55f};
    case VehicleType::Truck: return {0.45f, 2.3f, 1.95f};
    case VehicleType::Bus: return {0.75f, 4.3f, 2.25f};
    default: return {0.37f, -0.3f, 1.16f};
  }
}

DriveInput readDriveInput() {
  DriveInput in;
  if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) in.throttle = 1;
  if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) in.brake = 1;
  if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) in.steer -= 1;
  if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) in.steer += 1;
  in.handbrake = IsKeyDown(KEY_SPACE);
  return in;
}

void Driving::enter(const Vehicle& v) {
  car_ = v;
  car_.braking = false;
  active_ = has_car_ = true;
  vx_ = v.v;
  vy_ = r_ = 0;
  delta_ = 0;
  gear_ = 0;
  reverse_ = false;
  cam_init_ = false;
}

rj::geo::Vec3d Driving::exitPosition() const {
  // right-hand drive: the driver's door is on the right; step out 1.6 m to the right
  const double rx = std::cos(car_.yaw), ry = -std::sin(car_.yaw);
  return {car_.pos.x + rx * 1.6, car_.pos.y + ry * 1.6, car_.pos.z};
}

// One integration step (h seconds) of the two-axle model.
void Driving::step(double h, const DriveInput& in_raw, double mu) {
  const CarSpec& S = carSpec(car_.type);
  const double L = S.a + S.b;
  const double m = S.mass;
  // --- driver inputs: S held while stopped selects reverse; in reverse the pedals swap
  DriveInput in = in_raw;
  if (!reverse_ && std::fabs(vx_) < 0.4 && in.brake > 0 && in.throttle == 0) reverse_ = true;
  if (reverse_ && vx_ > -0.4 && in.throttle > 0 && in.brake == 0) reverse_ = false;
  double throttle = reverse_ ? in.brake : in.throttle;
  double brake = reverse_ ? in.throttle : in.brake;
  if (!reverse_ && vx_ < -0.5 && in.throttle > 0) brake = 1, throttle = 0;  // rolling back: brake first
  if (reverse_ && vx_ > 0.5 && in.brake > 0) brake = 1, throttle = 0;
  throttle *= engine_on_ ? power_scale_ : 0.0;  // (a damaged engine gives less; no fuel, none)
  // --- steering: rate-limited, less lock at speed (keyboard), self-centring
  const double v = std::hypot(vx_, vy_);
  const double lock = S.steer_max / (1.0 + v * v / 260.0);
  const double target = in.steer * lock;
  const double rate = (in.steer == 0 ? 3.2 : 2.2) * S.steer_max;
  delta_ += std::clamp(target - delta_, -rate * h, rate * h);
  // --- normal loads with longitudinal load transfer
  const double dFz = m * ax_ * S.h_cg / L;
  const double Fzf = std::max(0.15 * m * kG, m * kG * S.b / L - dFz);
  const double Fzr = std::max(0.15 * m * kG, m * kG * S.a / L + dFz);
  // --- engine / gearbox (torque converter at launch)
  const double wheel_rpm = std::fabs(vx_) / S.wheel_r * 60.0 / (2.0 * kPi);
  const double ratio = reverse_ ? 3.2 : S.ratio[gear_];
  const double rpm_w = wheel_rpm * ratio * S.final_drive;
  const double stall = 1600.0 + 1000.0 * throttle;
  rpm_ = std::max({750.0, rpm_w, stall * throttle});  // torque converter slips at launch
  const double x = (rpm_ - S.rpm_peak) / S.rpm_max;
  double torque = S.torque * std::clamp(1.0 - 1.6 * x * x, 0.55, 1.0);
  if (rpm_ > S.rpm_max) torque = 0;                                  // rev limiter
  const double converter = 1.0 + 0.9 * std::max(0.0, 1.0 - rpm_w / 1800.0);
  double drive = 0;
  if (shift_t_ <= 0) drive = torque * converter * ratio * S.final_drive * 0.88 / S.wheel_r * throttle;
  if (throttle < 0.05 && rpm_w > 1000) drive = -0.18 * S.torque * ratio * S.final_drive / S.wheel_r * (rpm_w / S.rpm_max);  // engine braking
  if (std::fabs(vx_) > (reverse_ ? 7.0 : S.vmax)) drive = std::min(drive, 0.0);                                        // governor
  if (reverse_) drive = -drive;
  // automatic shifting
  shift_t_ -= h;
  if (!reverse_ && shift_t_ <= 0) {
    const double up = 1700.0 + (S.rpm_max * 0.9 - 1700.0) * std::pow(throttle, 1.3);
    if (gear_ + 1 < S.gears && rpm_w > up) {
      ++gear_;
      shift_t_ = 0.28;
    } else if (gear_ > 0) {
      const double rpm_down = wheel_rpm * S.ratio[gear_ - 1] * S.final_drive;
      if (rpm_w < up * 0.42 || (throttle > 0.9 && rpm_down < S.rpm_max * 0.8 && rpm_w < S.rpm_peak * 0.8)) {
        --gear_;
        shift_t_ = 0.22;
      }
    }
  }
  if (reverse_) gear_ = 0;
  // --- longitudinal tyre forces (driven axle, brakes with ABS-like limit, handbrake on the rear)
  const double muf = mu * Fzf, mur = mu * Fzr;
  double Fxf = 0, Fxr = 0;
  const double dsh = S.front_drive && S.rear_drive ? 0.5 : S.front_drive ? 1.0 : 0.0;
  Fxf += drive * dsh;
  Fxr += drive * (1.0 - dsh);
  const double dir = std::fabs(vx_) > 0.05 ? sgn(vx_) : 0.0;
  const double bforce = brake * mu * m * kG * 0.95;
  Fxf -= dir * std::min(bforce * 0.66, muf * 0.98);
  Fxr -= dir * std::min(bforce * 0.34, mur * 0.98);
  const bool hb = in.handbrake && std::fabs(vx_) > 0.3;
  if (hb) Fxr = -dir * mur * 0.85;
  Fxf = std::clamp(Fxf, -muf, muf);
  Fxr = std::clamp(Fxr, -mur, mur);
  // stopped with the brake held: hold still
  if (std::fabs(vx_) < 0.25 && (brake > 0 || in.handbrake) && throttle == 0) {
    vx_ = vy_ = r_ = 0;
    ax_ += (0.0 - ax_) * std::min(1.0, h * 8);
    return;
  }
  // --- lateral tyre forces (slip angles, saturating curve, friction circle)
  const double B = 9.5, C = 1.35;
  auto lateral = [&](double v_lat, double v_lon, double Dmax, double Fx) {
    const double alpha = std::atan2(v_lat, std::max(std::fabs(v_lon), 0.8));
    const double cap = std::sqrt(std::max(Dmax * Dmax - Fx * Fx, 0.04 * Dmax * Dmax));
    return -cap * std::sin(C * std::atan(B * alpha));
  };
  const double cd = std::cos(delta_), sd = std::sin(delta_);
  const double vf_lat = -sd * vx_ + cd * (vy_ + S.a * r_);
  const double vf_lon = cd * vx_ + sd * (vy_ + S.a * r_);
  const double Fyf = lateral(vf_lat, vf_lon, muf, Fxf);
  const double Fyr = lateral(vy_ - S.b * r_, vx_, mur * (hb ? 0.3 : 1.0), Fxr);
  slip_ = std::max(std::fabs(Fyr) / std::max(1.0, mur) * (hb ? 1.0 : 0.0),
                   std::clamp((std::fabs(std::atan2(vy_ - S.b * r_, std::max(std::fabs(vx_), 1.0))) - 0.08) * 6.0, 0.0, 1.0));
  // --- resistances
  const double drag = 0.5 * 1.2 * S.cda * vx_ * std::fabs(vx_);
  const double roll_res = std::fabs(vx_) > 0.05 ? 0.013 * m * kG * sgn(vx_) : 0.0;
  // --- equations of motion (body frame: x forward, y right, yaw clockwise)
  const double Fx = Fxf * cd - Fyf * sd + Fxr - drag - roll_res;
  const double Fy = Fxf * sd + Fyf * cd + Fyr;
  const double Mz = S.a * (Fxf * sd + Fyf * cd) - S.b * Fyr;
  const double axb = Fx / m, ayb = Fy / m;
  double vx_n = vx_ + (axb + vy_ * r_) * h;
  double vy_n = vy_ + (ayb - vx_ * r_) * h;
  double r_n = r_ + Mz / S.iz * h;
  // brakes / resistances never reverse the direction of travel on their own
  if (throttle == 0 && vx_ != 0 && sgn(vx_n) != sgn(vx_)) vx_n = 0;
  // low speed: blend to rolling kinematics (slip angles are ill-defined when nearly stopped)
  const double k = std::clamp((std::fabs(vx_n) - 1.0) / 3.0, 0.0, 1.0);
  const double r_kin = vx_n * std::tan(delta_) / L;
  vy_n = vy_n * k;
  r_n = r_n * k + r_kin * (1.0 - k);
  ax_ += ((vx_n - vx_) / h - ax_) * std::min(1.0, h * 10.0);
  ay_ += (vx_n * r_n + (vy_n - vy_) / h - ay_) * std::min(1.0, h * 10.0);
  vx_ = vx_n;
  vy_ = vy_n;
  r_ = r_n;
  // pose
  const double fx = std::sin(car_.yaw), fy = std::cos(car_.yaw), rx = fy, ry = -fx;
  car_.pos.x += (fx * vx_ + rx * vy_) * h;
  car_.pos.y += (fy * vx_ + ry * vy_) * h;
  car_.yaw = static_cast<float>(wrap(car_.yaw + r_ * h));
  car_.braking = brake > 0.05 && std::fabs(vx_) > 0.2;
}

void Driving::collide(const World& world, const Traffic& traffic, const rj::geo::Vec3d& before) {
  (void)before;
  const double len = Traffic::lengthOf(car_.type);
  const double half_w = car_.type == VehicleType::Bus ? 1.25 : car_.type == VehicleType::Kei ? 0.74 : 0.9;
  const double fx = std::sin(car_.yaw), fy = std::cos(car_.yaw);
  double pushx = 0, pushy = 0;
  const double offs[3] = {len * 0.5 - half_w, 0.0, -(len * 0.5 - half_w)};
  // buildings: three circles along the body
  for (double o : offs) {
    rj::geo::Vec3d c{car_.pos.x + fx * o + pushx, car_.pos.y + fy * o + pushy, car_.pos.z};
    const rj::geo::Vec3d c0 = c;
    world.collide(c, half_w + 0.05);
    world.collideWalls(c, half_w + 0.05, extra_walls_);  // guard rails, parapets, tunnel walls, lowered barriers
    pushx += c.x - c0.x;
    pushy += c.y - c0.y;
  }
  // other vehicles: capsules
  for (const auto& o : traffic.vehicles()) {
    if (std::hypot(o.pos.x - car_.pos.x, o.pos.y - car_.pos.y) > 14.0) continue;
    const double ol = Traffic::lengthOf(o.type) * 0.5 - 0.8;
    const double ofx = std::sin(o.yaw), ofy = std::cos(o.yaw);
    for (double off : offs) {
      const double px = car_.pos.x + fx * off + pushx, py = car_.pos.y + fy * off + pushy;
      const double t = std::clamp((px - o.pos.x) * ofx + (py - o.pos.y) * ofy, -ol, ol);
      const double qx = o.pos.x + ofx * t, qy = o.pos.y + ofy * t;
      const double dx = px - qx, dy = py - qy, d = std::hypot(dx, dy);
      const double rr = half_w + 0.85;
      if (d < rr && d > 1e-4) {
        pushx += dx / d * (rr - d);
        pushy += dy / d * (rr - d);
      }
    }
  }
  const double pl = std::hypot(pushx, pushy);
  if (pl < 1e-3) return;
  car_.pos.x += pushx;
  car_.pos.y += pushy;
  // bounce: remove the velocity into the obstacle (restitution 0.25), scrub some along it
  const double nx = pushx / pl, ny = pushy / pl;
  const double rx = fy, ry = -fx;
  double wx = fx * vx_ + rx * vy_, wy = fy * vx_ + ry * vy_;
  const double vn = wx * nx + wy * ny;
  if (vn < 0) {
    impact_ = std::max(impact_, static_cast<float>(-vn));
    impact_dmg_ = std::max(impact_dmg_, static_cast<float>(-vn));
    wx -= 1.25 * vn * nx;
    wy -= 1.25 * vn * ny;
    const double tx = wx - (wx * nx + wy * ny) * nx, ty = wy - (wx * nx + wy * ny) * ny;
    const double scrub = std::min(0.5, std::fabs(vn) * 0.08);
    wx -= tx * scrub;
    wy -= ty * scrub;
    r_ *= 0.5;
  }
  vx_ = wx * fx + wy * fy;
  vy_ = wx * rx + wy * ry;
}

void Driving::update(double dt, const World& world, const Traffic& traffic, const DriveInput& in, float wetness) {
  if (!has_car_) return;
  throttle_ = active_ ? in.throttle : 0.0f;
  dt = std::min(dt, 0.05);
  if (!active_) {  // parked
    car_.v = 0;
    return;
  }
  // dry asphalt ~1.0, wet ~0.7
  const double mu = 1.0 - 0.3 * std::clamp(static_cast<double>(wetness), 0.0, 1.0);
  const rj::geo::Vec3d before = car_.pos;
  const int n = std::max(1, static_cast<int>(std::ceil(dt / (1.0 / 240.0))));
  for (int i = 0; i < n; ++i) step(dt / n, in, mu);
  collide(world, traffic, before);
  // road surface: height and pitch from the front and rear axles
  const CarSpec& S = carSpec(car_.type);
  const double fx = std::sin(car_.yaw), fy = std::cos(car_.yaw);
  const auto hf = world.roadHeight(car_.pos.x + fx * S.a, car_.pos.y + fy * S.a);
  const auto hr = world.roadHeight(car_.pos.x - fx * S.b, car_.pos.y - fy * S.b);
  double road_pitch = car_.pitch - pitch_body_;
  if (hf && hr) {
    const double z = (*hf * S.b + *hr * S.a) / (S.a + S.b);
    car_.pos.z = car_.pos.z + (z - car_.pos.z) * std::min(1.0, dt * 14.0);
    road_pitch = std::atan2(*hf - *hr, S.a + S.b);
  }
  // body on springs: squat / dive and roll (about 1.4 Hz, lightly damped)
  const double w = 2.0 * kPi * 1.4, zeta = 0.45;
  const double pitch_t = std::clamp(ax_ * (ax_ > 0 ? 0.0035 : 0.0065), -0.07, 0.05);
  const double roll_t = std::clamp(-ay_ * 0.0075 * (S.h_cg / 0.55), -0.09, 0.09);
  pitch_v_ += (w * w * (pitch_t - pitch_body_) - 2 * zeta * w * pitch_v_) * dt;
  pitch_body_ += pitch_v_ * dt;
  roll_v_ += (w * w * (roll_t - roll_) - 2 * zeta * w * roll_v_) * dt;
  roll_ += roll_v_ * dt;
  car_.pitch = static_cast<float>(road_pitch + pitch_body_);
  car_.roll = static_cast<float>(roll_);
  car_.steer = static_cast<float>(delta_);
  car_.v = vx_;
  car_.reversing = reverse_;
  car_.wheel_dist = static_cast<float>(std::fmod(car_.wheel_dist + vx_ * dt, 1000.0));
  updateCamera(dt);
}

void Driving::updateCamera(double dt) {
  // chase camera: trails the direction of travel (not the nose) so slides read clearly
  const double yaw_travel = std::hypot(vx_, vy_) > 2.0 ? car_.yaw + std::atan2(vy_, std::fabs(vx_)) * 0.6 : car_.yaw;
  const double want = vx_ < -1.0 ? car_.yaw : yaw_travel;
  if (!cam_init_) {
    cam_yaw_ = car_.yaw;
    cam_pos_ = car_.pos;
    cam_init_ = true;
  }
  cam_yaw_ += wrap(want - cam_yaw_) * std::min(1.0, dt * 3.0);
  cam_pos_.x += (car_.pos.x - cam_pos_.x) * std::min(1.0, dt * 9.0);
  cam_pos_.y += (car_.pos.y - cam_pos_.y) * std::min(1.0, dt * 9.0);
  cam_pos_.z += (car_.pos.z - cam_pos_.z) * std::min(1.0, dt * 5.0);
}

rj::geo::Vec3d Driving::driverEye() const {
  const DriverSeat st = driverSeat(car_.type);
  const double side = st.side, fwd = st.fwd, up = st.up;
  const double fx = std::sin(car_.yaw), fy = std::cos(car_.yaw), rx = fy, ry = -fx;
  // follow the body's pitch / roll a little
  const double z = car_.pos.z + up + std::tan(car_.pitch) * fwd - std::sin(car_.roll) * side;
  return {car_.pos.x + rx * side + fx * fwd, car_.pos.y + ry * side + fy * fwd, z};
}

Camera3D Driving::rearCamera() const {
  const DriverSeat st = driverSeat(car_.type);
  const double back = Traffic::lengthOf(car_.type) * 0.5 + 0.05;
  const double fx = std::sin(car_.yaw), fy = std::cos(car_.yaw);
  const rj::geo::Vec3d eye{car_.pos.x - fx * back, car_.pos.y - fy * back, car_.pos.z + st.up + 0.02};
  const rj::geo::Vec3d tgt{eye.x - fx, eye.y - fy, eye.z - 0.035};
  Camera3D c{};
  c.position = enuToRl(eye);
  c.target = enuToRl(tgt);
  c.up = {0, 1, 0};
  c.fovy = 17.0f;
  c.projection = CAMERA_PERSPECTIVE;
  return c;
}

Camera3D Driving::camera(float fov, bool first_person, float look_yaw, float look_pitch) const {
  Camera3D c{};
  rj::geo::Vec3d eye, target;
  float fovy = fov;
  if (first_person) {
    eye = driverEye();
    const double y = car_.yaw + look_yaw, p = car_.pitch * 0.8 + look_pitch;
    target = {eye.x + std::sin(y) * std::cos(p), eye.y + std::cos(y) * std::cos(p), eye.z + std::sin(p)};
  } else {
    const double y = cam_yaw_ + look_yaw;
    const double fx = std::sin(y), fy = std::cos(y);
    const double dist = car_.type == VehicleType::Bus ? 15.0 : car_.type == VehicleType::Truck ? 11.0 : 6.8;
    const double hgt = car_.type == VehicleType::Bus ? 4.2 : car_.type == VehicleType::Truck ? 3.4 : 2.1;
    const double spd = std::fabs(vx_);
    const double d = dist + std::min(2.0, spd * 0.05);
    eye = {cam_pos_.x - fx * d, cam_pos_.y - fy * d, cam_pos_.z + hgt + std::sin(look_pitch) * 3.0};
    target = {cam_pos_.x + fx * 2.5, cam_pos_.y + fy * 2.5, cam_pos_.z + hgt * 0.55};
    fovy = fov + static_cast<float>(std::min(10.0, spd * 0.22));  // sense of speed
  }
  c.position = enuToRl(eye);
  c.target = enuToRl(target);
  c.up = {0, 1, 0};
  c.fovy = fovy;
  c.projection = CAMERA_PERSPECTIVE;
  return c;
}

}  // namespace rjc
