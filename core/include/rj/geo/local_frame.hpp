#pragma once
// Local East-North-Up frames and the floating origin.
//
// World model: every cooked streaming cell stores geometry in its own local
// ENU frame (origin = cell anchor on the ellipsoid). At runtime the client
// renders relative to a *floating origin* near the player; a cell is placed
// by the rigid transform ENU(cell) -> ECEF -> ENU(origin). This keeps float
// precision at the camera anywhere in Japan (122°E..154°E, 20°N..46°N) and
// makes earth curvature correct at long view distances, without a single
// national projection (a single TM zone would distort badly across Japan).

#include <array>

#include "rj/geo/ellipsoid.hpp"

namespace rj::geo {

struct Vec3d {
  double x = 0.0, y = 0.0, z = 0.0;
  Vec3d operator+(const Vec3d& o) const { return {x + o.x, y + o.y, z + o.z}; }
  Vec3d operator-(const Vec3d& o) const { return {x - o.x, y - o.y, z - o.z}; }
  Vec3d operator*(double s) const { return {x * s, y * s, z * s}; }
  double dot(const Vec3d& o) const { return x * o.x + y * o.y + z * o.z; }
  double norm() const { return std::sqrt(dot(*this)); }
};

// Rigid transform: p' = R * p + t (row-major 3x3).
struct Rigid3d {
  std::array<double, 9> R{1, 0, 0, 0, 1, 0, 0, 0, 1};
  Vec3d t{};
  Vec3d apply(const Vec3d& p) const {
    return {R[0] * p.x + R[1] * p.y + R[2] * p.z + t.x, R[3] * p.x + R[4] * p.y + R[5] * p.z + t.y,
            R[6] * p.x + R[7] * p.y + R[8] * p.z + t.z};
  }
};

class LocalFrame {
 public:
  explicit LocalFrame(const Geodetic& anchor);

  const Geodetic& anchor() const { return anchor_; }
  const Ecef& anchorEcef() const { return origin_; }

  // x = east, y = north, z = up (metres).
  Vec3d ecefToLocal(const Ecef& p) const;
  Ecef localToEcef(const Vec3d& enu) const;
  Vec3d geodeticToLocal(const Geodetic& g) const { return ecefToLocal(geodeticToEcef(g)); }

  // Transform taking coordinates in `from` into this frame.
  Rigid3d transformFrom(const LocalFrame& from) const;

 private:
  Geodetic anchor_;
  Ecef origin_;
  std::array<double, 9> ecef_to_enu_;  // rows: east, north, up
};

// Floating origin policy: the render/physics origin is rebased when the
// tracked point drifts beyond `rebase_distance_m` from it.
class FloatingOrigin {
 public:
  FloatingOrigin(const Geodetic& start, double rebase_distance_m = 4096.0);

  const LocalFrame& frame() const { return frame_; }
  int rebaseCount() const { return rebases_; }

  // Update with the tracked point in current origin-local coordinates.
  // Returns true if the origin was rebased; `local` is rewritten into the new frame.
  bool update(Vec3d& local);

 private:
  LocalFrame frame_;
  double rebase_distance_m_;
  int rebases_ = 0;
};

}  // namespace rj::geo
