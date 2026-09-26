#include "rj/geo/local_frame.hpp"

namespace rj::geo {

LocalFrame::LocalFrame(const Geodetic& anchor) : anchor_(anchor), origin_(geodeticToEcef(anchor)) {
  const double lat = anchor.lat_deg * kDegToRad;
  const double lon = anchor.lon_deg * kDegToRad;
  const double sl = std::sin(lat), cl = std::cos(lat);
  const double so = std::sin(lon), co = std::cos(lon);
  ecef_to_enu_ = {-so,      co,       0.0,   // east
                  -sl * co, -sl * so, cl,    // north
                  cl * co,  cl * so,  sl};   // up
}

Vec3d LocalFrame::ecefToLocal(const Ecef& p) const {
  const double dx = p.x - origin_.x, dy = p.y - origin_.y, dz = p.z - origin_.z;
  const auto& M = ecef_to_enu_;
  return {M[0] * dx + M[1] * dy + M[2] * dz, M[3] * dx + M[4] * dy + M[5] * dz,
          M[6] * dx + M[7] * dy + M[8] * dz};
}

Ecef LocalFrame::localToEcef(const Vec3d& v) const {
  const auto& M = ecef_to_enu_;  // orthonormal: inverse = transpose
  return {origin_.x + M[0] * v.x + M[3] * v.y + M[6] * v.z,
          origin_.y + M[1] * v.x + M[4] * v.y + M[7] * v.z,
          origin_.z + M[2] * v.x + M[5] * v.y + M[8] * v.z};
}

Rigid3d LocalFrame::transformFrom(const LocalFrame& from) const {
  // R = M_this * M_from^T ; t = M_this * (origin_from - origin_this)
  const auto& A = ecef_to_enu_;
  const auto& B = from.ecef_to_enu_;
  Rigid3d r;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      double s = 0.0;
      for (int k = 0; k < 3; ++k) s += A[i * 3 + k] * B[j * 3 + k];
      r.R[i * 3 + j] = s;
    }
  r.t = ecefToLocal(from.origin_);
  return r;
}

FloatingOrigin::FloatingOrigin(const Geodetic& start, double rebase_distance_m)
    : frame_(start), rebase_distance_m_(rebase_distance_m) {}

bool FloatingOrigin::update(Vec3d& local) {
  const double horiz = std::sqrt(local.x * local.x + local.y * local.y);
  if (horiz < rebase_distance_m_) return false;
  const Ecef world = frame_.localToEcef(local);
  Geodetic g = ecefToGeodetic(world);
  g.h_ellipsoidal_m = frame_.anchor().h_ellipsoidal_m;  // keep origin height stable
  frame_ = LocalFrame(g);
  local = frame_.ecefToLocal(world);
  ++rebases_;
  return true;
}

}  // namespace rj::geo
