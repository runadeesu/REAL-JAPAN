#pragma once
// Axis conventions.
//   World / cell data: ENU metres (x = east, y = north, z = up), right-handed.
//   raylib render space: X = east, Y = up, Z = south (-north), right-handed.

#include "raylib.h"
#include "rj/geo/local_frame.hpp"

namespace rjc {

inline Vector3 enuToRl(const rj::geo::Vec3d& p) {
  return {static_cast<float>(p.x), static_cast<float>(p.z), static_cast<float>(-p.y)};
}
inline rj::geo::Vec3d rlToEnu(const Vector3& v) { return {v.x, -v.z, v.y}; }

// Model matrix for geometry stored in a cell's ENU frame, given the rigid
// transform cell-ENU -> origin-ENU. Result maps cell ENU -> raylib space.
inline Matrix rigidToRaylib(const rj::geo::Rigid3d& T) {
  const auto& R = T.R;
  // A = S * R with S = [[1,0,0],[0,0,1],[0,-1,0]]
  const double A[9] = {R[0], R[1], R[2], R[6], R[7], R[8], -R[3], -R[4], -R[5]};
  const double t[3] = {T.t.x, T.t.z, -T.t.y};
  Matrix m{};
  m.m0 = static_cast<float>(A[0]);
  m.m4 = static_cast<float>(A[1]);
  m.m8 = static_cast<float>(A[2]);
  m.m12 = static_cast<float>(t[0]);
  m.m1 = static_cast<float>(A[3]);
  m.m5 = static_cast<float>(A[4]);
  m.m9 = static_cast<float>(A[5]);
  m.m13 = static_cast<float>(t[1]);
  m.m2 = static_cast<float>(A[6]);
  m.m6 = static_cast<float>(A[7]);
  m.m10 = static_cast<float>(A[8]);
  m.m14 = static_cast<float>(t[2]);
  m.m15 = 1.0f;
  return m;
}

}  // namespace rjc
