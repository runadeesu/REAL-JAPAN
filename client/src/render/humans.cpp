#include "render/humans.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "raymath.h"
#include "render/gpu_mesh.hpp"
#include "world/detail.hpp"

namespace rjc {
namespace {

constexpr int kMatTop = 31, kMatSkinId = 32, kMatBottom = 36, kMatHair = 37, kMatShoe = 29;

// Model space while building: x right, y forward, z up (feet at z = 0, 1.70 m adult).
struct Geo {
  std::vector<float> pos, nrm, uv, t2;
  std::vector<unsigned char> col;
  std::vector<unsigned short> idx;
  unsigned short v() const { return static_cast<unsigned short>(pos.size() / 3); }
  void vert(Vector3 p, Vector3 n, int mat, float shade) {
    pos.insert(pos.end(), {p.x, p.z, -p.y});
    nrm.insert(nrm.end(), {n.x, n.z, -n.y});
    uv.insert(uv.end(), {0.0f, 0.0f});
    const unsigned char c = static_cast<unsigned char>(255 * shade);
    col.insert(col.end(), {c, c, c, 255});
    t2.insert(t2.end(), {static_cast<float>(mat), 0.0f});
  }
  Mesh upload() {
    if (std::getenv("RJ_HUMAN_DUMP")) {  // debug: write the pose as OBJ
      static int n = 0;
      if (n++ == 0) {
        FILE* f = std::fopen("human.obj", "w");
        if (f) {
          for (size_t i = 0; i + 2 < pos.size(); i += 3) std::fprintf(f, "v %.4f %.4f %.4f\n", pos[i], pos[i + 1], pos[i + 2]);
          for (size_t i = 0; i + 2 < idx.size(); i += 3) std::fprintf(f, "f %d %d %d\n", idx[i] + 1, idx[i + 1] + 1, idx[i + 2] + 1);
          std::fclose(f);
        }
      }
    }
    Mesh m{};
    m.vertexCount = static_cast<int>(pos.size() / 3);
    m.triangleCount = static_cast<int>(idx.size() / 3);
    m.vertices = pos.data();
    m.normals = nrm.data();
    m.texcoords = uv.data();
    m.texcoords2 = t2.data();
    m.colors = col.data();
    m.indices = idx.data();
    UploadMesh(&m, false);
    releaseCpuArrays(m);
    return m;
  }
};

// Loft of elliptical rings along a bone from a to b. rings: (t along bone, radius x, radius y).
// The ring frame follows the bone; `side` is the bone's local x axis.
struct Ring {
  float t, rx, ry;
};
void loftBone(Geo& g, Vector3 a, Vector3 b, Vector3 side, const std::vector<Ring>& rings, int mat, int seg = 10, bool capA = true,
              bool capB = true) {
  const Vector3 ax = Vector3Normalize(Vector3Subtract(b, a));
  Vector3 sx = Vector3Normalize(Vector3Subtract(side, Vector3Scale(ax, Vector3DotProduct(side, ax))));
  const Vector3 sy = Vector3CrossProduct(ax, sx);
  const unsigned short base = g.v();
  for (const Ring& r : rings) {
    const Vector3 c = Vector3Lerp(a, b, r.t);
    for (int i = 0; i < seg; ++i) {
      const float th = static_cast<float>(i) / seg * 2 * PI;
      const Vector3 d = Vector3Add(Vector3Scale(sx, std::cos(th) * r.rx), Vector3Scale(sy, std::sin(th) * r.ry));
      const Vector3 n = Vector3Normalize(Vector3Add(Vector3Scale(sx, std::cos(th) / r.rx), Vector3Scale(sy, std::sin(th) / r.ry)));
      g.vert(Vector3Add(c, d), n, mat, 0.92f + 0.08f * std::sin(th));
    }
  }
  const int nr = static_cast<int>(rings.size());
  for (int k = 0; k + 1 < nr; ++k)
    for (int i = 0; i < seg; ++i) {
      const auto p0 = static_cast<unsigned short>(base + k * seg + i), p1 = static_cast<unsigned short>(base + k * seg + (i + 1) % seg);
      const auto q0 = static_cast<unsigned short>(p0 + seg), q1 = static_cast<unsigned short>(p1 + seg);
      g.idx.insert(g.idx.end(), {p0, p1, q1, p0, q1, q0});
    }
  auto cap = [&](int ring, bool flip) {
    const Ring& r = rings[static_cast<size_t>(ring)];
    const Vector3 c = Vector3Lerp(a, b, r.t);
    const Vector3 n = flip ? Vector3Negate(ax) : ax;
    const unsigned short ci = g.v();
    g.vert(c, n, mat, 0.9f);
    for (int i = 0; i < seg; ++i) {
      const auto p0 = static_cast<unsigned short>(base + ring * seg + i), p1 = static_cast<unsigned short>(base + ring * seg + (i + 1) % seg);
      if (flip) g.idx.insert(g.idx.end(), {ci, p1, p0});
      else g.idx.insert(g.idx.end(), {ci, p0, p1});
    }
  };
  if (capA) cap(0, true);
  if (capB) cap(nr - 1, false);
}

void ellipsoid(Geo& g, Vector3 c, Vector3 r, int mat, int seg = 12, int rings = 8, float zcut = -2.0f) {
  const unsigned short base = g.v();
  for (int j = 0; j <= rings; ++j) {
    const float ph = -PI / 2 + PI * j / rings;
    for (int i = 0; i <= seg; ++i) {
      const float th = 2 * PI * i / seg;
      Vector3 d{std::cos(ph) * std::cos(th), std::cos(ph) * std::sin(th), std::sin(ph)};
      if (d.z < zcut) d.z = zcut;
      const Vector3 p{c.x + d.x * r.x, c.y + d.y * r.y, c.z + d.z * r.z};
      g.vert(p, Vector3Normalize(Vector3{d.x / r.x, d.y / r.y, d.z / r.z}), mat, 0.95f);
    }
  }
  for (int j = 0; j < rings; ++j)
    for (int i = 0; i < seg; ++i) {
      const auto a = static_cast<unsigned short>(base + j * (seg + 1) + i), b = static_cast<unsigned short>(a + 1);
      const auto c2 = static_cast<unsigned short>(a + seg + 1), d = static_cast<unsigned short>(c2 + 1);
      g.idx.insert(g.idx.end(), {a, b, d, a, d, c2});
    }
}

Vector3 rotX(Vector3 v, float a) { return {v.x, v.y * std::cos(a) - v.z * std::sin(a), v.y * std::sin(a) + v.z * std::cos(a)}; }
Vector3 rotZ(Vector3 v, float a) { return {v.x * std::cos(a) - v.y * std::sin(a), v.x * std::sin(a) + v.y * std::cos(a), v.z}; }

// One pose. phase: walk cycle; walk=false -> idle stance.
Mesh buildPose(BodyVariant var, float phase, bool walk) {
  Geo g;
  const float s = walk ? std::sin(phase) : 0.0f;
  const float bob = walk ? 0.018f * std::cos(2 * phase) : 0.0f;
  const float pelvis_yaw = walk ? 0.07f * s : 0.0f;
  const Vector3 pelvis{0, 0, 0.97f + bob};
  const bool skirt = var == BodyVariant::Skirt;
  const bool long_hair = var != BodyVariant::Trousers;
  // ---- legs: hip -> knee -> ankle -> toe, pitch about x (forward swing = positive y) ----
  for (int side = -1; side <= 1; side += 2) {
    const float leg_ph = side < 0 ? phase : phase + PI;
    const float swing = walk ? 0.40f * std::sin(leg_ph) : 0.0f;                          // thigh angle
    const float knee = walk ? 0.12f + 0.55f * std::pow(std::max(0.0f, std::sin(leg_ph + 1.1f)), 2.0f) : 0.04f;  // knee flex
    Vector3 hip = Vector3Add(pelvis, rotZ(Vector3{0.095f * side, 0, -0.05f}, pelvis_yaw));
    const Vector3 thigh = rotX(Vector3{0, 0, -0.44f}, swing);
    const Vector3 kneeP = Vector3Add(hip, thigh);
    const Vector3 shin = rotX(Vector3{0, 0, -0.42f}, swing - knee);
    const Vector3 ankle = Vector3Add(kneeP, shin);
    const Vector3 xs{1, 0, 0};
    const int legmat = skirt ? kMatSkinId : kMatBottom;
    loftBone(g, hip, kneeP, xs, {{0.0f, 0.085f, 0.09f}, {0.55f, 0.07f, 0.075f}, {1.0f, 0.055f, 0.058f}}, legmat, 10, false, false);
    loftBone(g, kneeP, ankle, xs, {{0.0f, 0.055f, 0.058f}, {0.35f, 0.052f, 0.056f}, {1.0f, 0.036f, 0.04f}}, legmat, 10, false, true);
    // shoe
    const float foot_pitch = walk ? std::clamp(swing - knee * 0.5f, -0.35f, 0.35f) * 0.6f : 0.0f;
    const Vector3 toe = Vector3Add(ankle, rotX(Vector3{0, 0.2f, -0.04f}, foot_pitch));
    loftBone(g, Vector3Add(ankle, Vector3{0, -0.06f, -0.03f}), toe, xs,
             {{0.0f, 0.045f, 0.04f}, {0.5f, 0.05f, 0.038f}, {1.0f, 0.042f, 0.03f}}, kMatShoe, 8);
  }
  // ---- torso (lofted along z), slight counter-rotation ----
  const float chest_yaw = -pelvis_yaw * 1.2f;
  auto tor = [&](float z, float yaw) { return Vector3Add(pelvis, rotZ(Vector3{0, 0, z}, yaw)); };
  if (skirt) {
    // skirt: cone from the waist to the knee line
    loftBone(g, tor(0.08f, 0), tor(-0.42f, 0), Vector3{1, 0, 0},
             {{0.0f, 0.15f, 0.12f}, {0.4f, 0.19f, 0.16f}, {1.0f, 0.24f, 0.21f}}, kMatBottom, 14, true, true);
  } else {
    loftBone(g, tor(-0.1f, 0), tor(0.08f, 0), Vector3{1, 0, 0}, {{0.0f, 0.17f, 0.115f}, {1.0f, 0.15f, 0.105f}}, kMatBottom, 14, true, false);
  }
  loftBone(g, tor(0.06f, pelvis_yaw * 0.5f), tor(0.50f, chest_yaw), Vector3{std::cos(chest_yaw), std::sin(chest_yaw), 0},
           {{0.0f, 0.15f, 0.105f}, {0.35f, 0.155f, 0.11f}, {0.7f, 0.18f, 0.12f}, {0.92f, 0.19f, 0.105f}, {1.0f, 0.12f, 0.08f}}, kMatTop, 14,
           false, true);
  // neck + head + hair
  const Vector3 neck0 = tor(0.49f, chest_yaw), neck1 = tor(0.58f, chest_yaw);
  loftBone(g, neck0, neck1, Vector3{1, 0, 0}, {{0.0f, 0.055f, 0.055f}, {1.0f, 0.05f, 0.05f}}, kMatSkinId, 8, false, false);
  const Vector3 head{neck1.x, neck1.y + 0.01f, neck1.z + 0.11f};
  ellipsoid(g, head, {0.078f, 0.095f, 0.112f}, kMatSkinId, 12, 8);
  if (long_hair) {
    ellipsoid(g, {head.x, head.y - 0.012f, head.z + 0.012f}, {0.087f, 0.1f, 0.118f}, kMatHair, 12, 8, -0.15f);
    loftBone(g, {head.x, head.y - 0.05f, head.z}, {head.x, head.y - 0.07f, head.z - 0.2f}, Vector3{1, 0, 0},
             {{0.0f, 0.08f, 0.05f}, {1.0f, 0.075f, 0.03f}}, kMatHair, 10);
  } else {
    ellipsoid(g, {head.x, head.y - 0.008f, head.z + 0.02f}, {0.083f, 0.099f, 0.108f}, kMatHair, 12, 8, 0.05f);
  }
  // ---- arms: shoulder -> elbow -> wrist (swing opposite to the legs) ----
  for (int side = -1; side <= 1; side += 2) {
    const float arm_ph = side < 0 ? phase + PI : phase;
    const float swing = walk ? 0.32f * std::sin(arm_ph) : 0.0f;
    const float elbow = walk ? 0.25f + 0.2f * (0.5f + 0.5f * std::sin(arm_ph)) : 0.12f;
    const Vector3 sh = tor(0.45f, chest_yaw);
    const Vector3 shoulder = Vector3Add(sh, rotZ(Vector3{0.19f * side, 0, 0}, chest_yaw));
    const Vector3 upper = rotX(Vector3{0.02f * side, 0, -0.29f}, swing);
    const Vector3 elbowP = Vector3Add(shoulder, upper);
    const Vector3 fore = rotX(Vector3{0.01f * side, 0, -0.26f}, swing + elbow);
    const Vector3 wrist = Vector3Add(elbowP, fore);
    const Vector3 xs{1, 0, 0};
    loftBone(g, shoulder, elbowP, xs, {{0.0f, 0.052f, 0.05f}, {1.0f, 0.042f, 0.04f}}, kMatTop, 9, true, false);
    loftBone(g, elbowP, wrist, xs, {{0.0f, 0.04f, 0.038f}, {0.3f, 0.036f, 0.034f}, {1.0f, 0.028f, 0.026f}}, kMatTop, 9, false, true);
    ellipsoid(g, Vector3Add(wrist, rotX(Vector3{0, 0, -0.07f}, swing + elbow)), {0.03f, 0.022f, 0.06f}, kMatSkinId, 8, 5);
  }
  return g.upload();
}

Mesh buildUmbrella() {
  Geo g;
  // Canopy: 8 panels, 0.95 m across, slightly domed, centre 2.05 m above the feet, tilted forward.
  const Vector3 top{0.06f, 0.08f, 2.12f};
  const int n = 8;
  const float r = 0.48f;
  for (int i = 0; i < n; ++i) {
    const float a0 = 2 * PI * i / n, a1 = 2 * PI * (i + 1) / n;
    const Vector3 p0{top.x + r * std::cos(a0), top.y + r * std::sin(a0) + 0.03f, top.z - 0.26f};
    const Vector3 p1{top.x + r * std::cos(a1), top.y + r * std::sin(a1) + 0.03f, top.z - 0.26f};
    const Vector3 nrm = Vector3Normalize(Vector3CrossProduct(Vector3Subtract(p0, top), Vector3Subtract(p1, top)));
    const auto b = g.v();
    g.vert(top, nrm.z < 0 ? Vector3Negate(nrm) : nrm, 31, 1.0f);
    g.vert(p0, nrm.z < 0 ? Vector3Negate(nrm) : nrm, 31, 0.92f);
    g.vert(p1, nrm.z < 0 ? Vector3Negate(nrm) : nrm, 31, 0.92f);
    g.idx.insert(g.idx.end(), {b, static_cast<unsigned short>(b + 1), static_cast<unsigned short>(b + 2)});
  }
  // Shaft from the hand to the canopy.
  loftBone(g, {0.2f, 0.12f, 1.02f}, top, Vector3{1, 0, 0}, {{0.0f, 0.012f, 0.012f}, {1.0f, 0.009f, 0.009f}}, 29, 6);
  return g.upload();
}

}  // namespace

void HumanModels::build() {
  if (ready_) return;
  for (int v = 0; v < static_cast<int>(BodyVariant::Count); ++v) {
    for (int f = 0; f < kFrames; ++f) walk_[v][f] = buildPose(static_cast<BodyVariant>(v), 2 * PI * f / kFrames, true);
    idle_[v] = buildPose(static_cast<BodyVariant>(v), 0.0f, false);
  }
  umbrella_ = buildUmbrella();
  ready_ = true;
}

void HumanModels::unload() {
  if (!ready_) return;
  for (int v = 0; v < static_cast<int>(BodyVariant::Count); ++v) {
    for (auto& m : walk_[v]) UnloadMesh(m);
    UnloadMesh(idle_[v]);
  }
  UnloadMesh(umbrella_);
  ready_ = false;
}

const Mesh& HumanModels::frame(BodyVariant v, float phase, bool idle) const {
  const int vi = static_cast<int>(v);
  if (idle) return idle_[vi];
  float p = std::fmod(phase, 2 * PI);
  if (p < 0) p += 2 * PI;
  const int f = static_cast<int>(p / (2 * PI) * kFrames) % kFrames;
  return walk_[vi][f];
}

}  // namespace rjc
