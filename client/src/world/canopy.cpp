#include "world/canopy.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "world/detail.hpp"

namespace rjc {
namespace {

float hash2(int x, int y) {
  uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return static_cast<float>((h ^ (h >> 16)) & 0xffffff) / 16777215.0f;
}

float valueNoise(float x, float y) {
  const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
  const float fx = x - x0, fy = y - y0;
  const float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
  const float a = hash2(x0, y0), b = hash2(x0 + 1, y0), c = hash2(x0, y0 + 1), d = hash2(x0 + 1, y0 + 1);
  return (a + (b - a) * sx) * (1 - sy) + (c + (d - c) * sx) * sy;
}

// bilinear sample of one channel (0..1) of an RGBA8 image at uv (0..1, row 0 = north)
float sampleChannel(const Image& img, float u, float v, int ch) {
  const auto* px = static_cast<const unsigned char*>(img.data);
  const float fx = std::clamp(u * img.width - 0.5f, 0.0f, img.width - 1.001f);
  const float fy = std::clamp(v * img.height - 0.5f, 0.0f, img.height - 1.001f);
  const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
  const float tx = fx - x0, ty = fy - y0;
  auto at = [&](int x, int y) { return px[(static_cast<size_t>(y) * img.width + x) * 4 + ch] / 255.0f; };
  return (at(x0, y0) * (1 - tx) + at(x0 + 1, y0) * tx) * (1 - ty) + (at(x0, y0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1) * tx) * ty;
}

// ground texture shows a surface that must stay open (carriageway, paving, track bed, river bed, runway)
bool openGround(const Image& g, float u, float v) {
  if (!g.data) return false;
  const int x = std::clamp(static_cast<int>(u * g.width), 0, g.width - 1);
  const int y = std::clamp(static_cast<int>(v * g.height), 0, g.height - 1);
  const Color c = GetImageColor(g, x, y);
  if (c.a < 128) return true;
  const int mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b});
  const bool grey = mx - mn < 18;
  return grey && (mx < 110 || mx > 150);  // asphalt / runway (dark grey), paving / apron (light grey)
}

}  // namespace

void buildCanopy(CellCpu& c) {
  const Image& lc = c.detail.landcover;
  const int nx = c.tnx, ny = c.tny;
  if (!lc.data || nx < 2 || ny < 2 || c.tpos.size() < static_cast<size_t>(nx * ny * 3)) return;
  std::vector<unsigned char> inside(static_cast<size_t>(nx * ny), 0);
  bool any = false;
  for (int i = 0; i < ny; ++i)
    for (int j = 0; j < nx; ++j) {
      const float u = static_cast<float>(j) / (nx - 1), v = 1.0f - static_cast<float>(i) / (ny - 1);
      const float f = sampleChannel(lc, u, v, 0);
      const bool in = f > 0.5f && !openGround(c.ground, u, v);
      inside[static_cast<size_t>(i * nx + j)] = in ? 1 : 0;
      any |= in;
    }
  if (!any) return;
  auto In = [&](int i, int j) {
    return i >= 0 && j >= 0 && i < ny && j < nx && inside[static_cast<size_t>(i * nx + j)];
  };
  CellDetailCpu::Chunk ch;
  ch.mat = kMatCanopy;
  std::vector<int> vid(static_cast<size_t>(nx * ny), -1);
  for (int i = 0; i < ny; ++i)
    for (int j = 0; j < nx; ++j) {
      bool near = false;
      for (int di = -1; di <= 1 && !near; ++di)
        for (int dj = -1; dj <= 1 && !near; ++dj) near = In(i + di, j + dj);
      if (!near) continue;
      const size_t k = static_cast<size_t>(i * nx + j);
      const float x = c.tpos[k * 3], y = c.tpos[k * 3 + 1], z = c.tpos[k * 3 + 2];
      // crown height: stands of different age (smooth noise) and single crowns (per point)
      const float wx = x + static_cast<float>(c.anchor[1] * 111000.0), wy = y + static_cast<float>(c.anchor[0] * 111000.0);
      const float h = 13.0f + 9.0f * valueNoise(wx / 45.0f, wy / 45.0f) + 2.5f * (hash2(static_cast<int>(wx), static_cast<int>(wy)) - 0.5f);
      vid[k] = static_cast<int>(ch.pos.size() / 3);
      ch.pos.insert(ch.pos.end(), {x, y, In(i, j) ? z + h : z - 0.5f});
      ch.uv.insert(ch.uv.end(), {0.0f, 0.0f});
      const unsigned char shade = static_cast<unsigned char>(80 + 60 * hash2(static_cast<int>(wx * 0.37f), static_cast<int>(wy * 0.37f)));
      ch.col.insert(ch.col.end(), {shade, shade, shade, 255});
    }
  for (int i = 0; i < ny - 1; ++i)
    for (int j = 0; j < nx - 1; ++j) {
      const int a = vid[static_cast<size_t>(i * nx + j)], b = vid[static_cast<size_t>(i * nx + j + 1)];
      const int d = vid[static_cast<size_t>((i + 1) * nx + j)], e = vid[static_cast<size_t>((i + 1) * nx + j + 1)];
      if (a < 0 || b < 0 || d < 0 || e < 0) continue;
      if (!(In(i, j) || In(i, j + 1) || In(i + 1, j) || In(i + 1, j + 1))) continue;
      ch.idx.insert(ch.idx.end(), {static_cast<unsigned short>(a), static_cast<unsigned short>(b), static_cast<unsigned short>(e),
                                   static_cast<unsigned short>(a), static_cast<unsigned short>(e), static_cast<unsigned short>(d)});
    }
  if (ch.idx.empty()) return;
  // vertex normals from the faces
  const size_t nv = ch.pos.size() / 3;
  ch.nrm.assign(nv * 3, 0.0f);
  for (size_t t = 0; t + 2 < ch.idx.size(); t += 3) {
    const size_t A = ch.idx[t], B = ch.idx[t + 1], C = ch.idx[t + 2];
    const float ux = ch.pos[B * 3] - ch.pos[A * 3], uy = ch.pos[B * 3 + 1] - ch.pos[A * 3 + 1], uz = ch.pos[B * 3 + 2] - ch.pos[A * 3 + 2];
    const float vx = ch.pos[C * 3] - ch.pos[A * 3], vy = ch.pos[C * 3 + 1] - ch.pos[A * 3 + 1], vz = ch.pos[C * 3 + 2] - ch.pos[A * 3 + 2];
    const float n[3] = {uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx};
    for (size_t k : {A, B, C})
      for (int a = 0; a < 3; ++a) ch.nrm[k * 3 + a] += n[a];
  }
  for (size_t k = 0; k < nv; ++k) {
    float* n = &ch.nrm[k * 3];
    const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (l > 1e-9f)
      for (int a = 0; a < 3; ++a) n[a] /= l;
    else
      n[2] = 1.0f;
  }
  c.detail.chunks.push_back(std::move(ch));
}

}  // namespace rjc
