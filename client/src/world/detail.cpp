#include "world/detail.hpp"

#include <cstdlib>
#include <cstring>

#include "render/foliage.hpp"
#include "render/gpu_mesh.hpp"

namespace rjc {
namespace {

class Reader {
 public:
  Reader(const unsigned char* d, size_t n) : d_(d), n_(n) {}
  bool ok() const { return ok_; }
  template <class T>
  T get() {
    T v{};
    bytes(&v, sizeof(T));
    return v;
  }
  void bytes(void* dst, size_t n) {
    if (!ok_ || p_ + n > n_) {
      ok_ = false;
      return;
    }
    if (n) std::memcpy(dst, d_ + p_, n);
    p_ += n;
  }
  void skip(size_t n) {
    if (!ok_ || p_ + n > n_) ok_ = false;
    else p_ += n;
  }
  const unsigned char* here() const { return d_ + p_; }
  size_t remaining() const { return ok_ ? n_ - p_ : 0; }

 private:
  const unsigned char* d_;
  size_t n_, p_ = 0;
  bool ok_ = true;
};

float halfToFloat(uint16_t h) {
  const uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 0x1fu, m = h & 0x3ffu;
  uint32_t f;
  if (e == 0) {
    if (m == 0) {
      f = s << 31;
    } else {  // subnormal
      int ee = -1;
      uint32_t mm = m;
      do {
        ++ee;
        mm <<= 1;
      } while ((mm & 0x400u) == 0);
      f = (s << 31) | (static_cast<uint32_t>(127 - 15 - ee) << 23) | ((mm & 0x3ffu) << 13);
    }
  } else if (e == 31) {
    f = (s << 31) | 0x7f800000u | (m << 13);
  } else {
    f = (s << 31) | ((e + 112u) << 23) | (m << 13);
  }
  float out;
  std::memcpy(&out, &f, 4);
  return out;
}

}  // namespace

bool parseDetail(const std::vector<unsigned char>& file, CellDetailCpu& out, std::string& err) {
  if (file.size() < 12 || std::memcmp(file.data(), "RJDET001", 8) != 0) {
    err = "bad detail file";
    return false;
  }
  uint32_t blen = 0;
  std::memcpy(&blen, file.data() + 8, 4);
  int n = 0;
  unsigned char* body = DecompressData(file.data() + 12, static_cast<int>(file.size() - 12), &n);
  if (!body || static_cast<uint32_t>(n) != blen) {
    if (body) MemFree(body);
    err = "detail decompression failed";
    return false;
  }
  Reader r(body, static_cast<size_t>(n));
  const uint32_t nc = r.get<uint32_t>();
  for (uint32_t k = 0; k < nc && r.ok(); ++k) {
    CellDetailCpu::Chunk c;
    c.mat = static_cast<int>(r.get<uint32_t>());
    const uint32_t nv = r.get<uint32_t>(), ni = r.get<uint32_t>();
    float o[3], scale;
    for (float& v : o) v = r.get<float>();
    scale = r.get<float>();
    if (nv > 65535 || ni > 3u * 65535u * 4u) {
      err = "bad detail chunk";
      MemFree(body);
      return false;
    }
    std::vector<int16_t> q(nv * 3);
    r.bytes(q.data(), q.size() * 2);
    r.skip((4 - (nv * 6) % 4) % 4);
    std::vector<int8_t> qn(nv * 4);
    r.bytes(qn.data(), qn.size());
    std::vector<uint16_t> huv(nv * 2);
    r.bytes(huv.data(), huv.size() * 2);
    c.col.resize(nv * 4);
    r.bytes(c.col.data(), c.col.size());
    c.idx.resize(ni);
    r.bytes(c.idx.data(), ni * 2);
    r.skip((4 - (ni * 2) % 4) % 4);
    if (!r.ok()) break;
    c.pos.resize(nv * 3);
    c.nrm.resize(nv * 3);
    c.uv.resize(nv * 2);
    for (uint32_t v = 0; v < nv; ++v)
      for (int a = 0; a < 3; ++a) {
        c.pos[v * 3 + a] = o[a] + static_cast<float>(q[v * 3 + a]) * scale;
        c.nrm[v * 3 + a] = static_cast<float>(qn[v * 4 + a]) / 127.0f;
      }
    for (size_t i = 0; i < huv.size(); ++i) c.uv[i] = halfToFloat(huv[i]);
    if (c.mat == kMatSidewalk || c.mat == kMatIsland)
      for (uint32_t t = 0; t + 2 < ni; t += 3)
        for (int e = 0; e < 3; ++e)
          for (int a = 0; a < 3; ++a) out.walk.push_back(c.pos[c.idx[t + e] * 3u + a]);
    if (c.mat == kMatMarking)
      for (uint32_t t = 0; t + 2 < ni; t += 3)
        for (int a = 0; a < 2; ++a)
          out.marks.push_back((c.pos[c.idx[t] * 3u + a] + c.pos[c.idx[t + 1] * 3u + a] + c.pos[c.idx[t + 2] * 3u + a]) / 3.0f);
    out.chunks.push_back(std::move(c));
  }
  auto tris = [&](std::vector<float>& dst) {
    const uint32_t cnt = r.get<uint32_t>();
    if (!r.ok() || cnt > 20'000'000) return;
    const size_t base = dst.size();
    dst.resize(base + cnt * 9u);
    r.bytes(dst.data() + base, cnt * 36u);
  };
  tris(out.deck);  // extra walkable surfaces = bridge decks (drivable)
  const uint32_t nl = r.get<uint32_t>();
  for (uint32_t k = 0; k < nl && r.ok(); ++k) {
    StreetLight l{};
    for (float& v : l.pos) v = r.get<float>();
    l.range = r.get<float>();
    l.kind = r.get<uint32_t>();
    out.lights.push_back(l);
  }
  const uint32_t ns = r.get<uint32_t>();
  for (uint32_t k = 0; k < ns && r.ok(); ++k) {
    SignalHead s{};
    for (float& v : s.pos) v = r.get<float>();
    s.axis_yaw = r.get<float>();
    s.facing_yaw = r.get<float>();
    s.length = r.get<float>();
    s.kind = r.get<uint32_t>();
    s.group = r.get<int32_t>();
    s.phase = r.get<uint32_t>();
    out.signals.push_back(s);
  }
  tris(out.cross);
  const uint32_t ao_len = r.get<uint32_t>();
  if (r.ok() && ao_len > 0) {
    std::vector<unsigned char> png(ao_len);
    r.bytes(png.data(), ao_len);
    if (r.ok()) out.ao = LoadImageFromMemory(".png", png.data(), static_cast<int>(ao_len));
  }
  // Vegetation (appended section; absent in older packages).
  const uint32_t nt = r.get<uint32_t>();
  if (r.ok()) {
    for (uint32_t k = 0; k < nt && r.ok(); ++k) {
      TreeRec t{};
      for (float& v : t.base) v = r.get<float>();
      t.height = r.get<float>();
      t.crown = r.get<float>();
      t.kind = r.get<uint32_t>();
      out.trees.push_back(t);
    }
    tris(out.hedges);
    // Land cover (appended section; fictional worlds only).
    if (r.ok() && r.remaining() >= 4) {
      const uint32_t lc_len = r.get<uint32_t>();
      if (r.ok() && lc_len > 0 && lc_len <= r.remaining()) {
        std::vector<unsigned char> png(lc_len);
        r.bytes(png.data(), lc_len);
        if (r.ok()) {
          out.landcover = LoadImageFromMemory(".png", png.data(), static_cast<int>(lc_len));
          if (std::getenv("RJ_LC_DEBUG")) TraceLog(LOG_INFO, "RJ: land cover %dx%d fmt %d", out.landcover.width, out.landcover.height, out.landcover.format);
          if (out.landcover.data && out.landcover.format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8)
            ImageFormat(&out.landcover, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        }
      }
    }
  }
  if (!r.ok() && !out.chunks.empty()) {
    // tolerate packages without the vegetation section
    MemFree(body);
    out.present = true;
    return true;
  }
  MemFree(body);
  if (!r.ok()) {
    err = "truncated detail file";
    return false;
  }
  out.present = true;
  return true;
}

void uploadDetail(CellDetailCpu& cpu, CellDetailGpu& gpu) {
  for (auto& c : cpu.chunks) {
    const int nv = static_cast<int>(c.pos.size() / 3);
    if (nv == 0 || c.idx.empty()) continue;
    Mesh m{};
    m.vertexCount = nv;
    m.triangleCount = static_cast<int>(c.idx.size() / 3);
    m.vertices = c.pos.data();
    m.normals = c.nrm.data();
    m.texcoords = c.uv.data();
    m.colors = c.col.data();
    m.indices = c.idx.data();
    // texcoords2 = (material id, per-vertex variation) for the lit shader.
    std::vector<float> t2(static_cast<size_t>(nv) * 2);
    for (int v = 0; v < nv; ++v) {
      t2[static_cast<size_t>(v) * 2] = static_cast<float>(c.mat);
      t2[static_cast<size_t>(v) * 2 + 1] = 0.0f;
    }
    m.texcoords2 = t2.data();
    UploadMesh(&m, false);
    releaseCpuArrays(m);
    gpu.meshes.push_back(m);
    gpu.mats.push_back(c.mat);
  }
  cpu.chunks.clear();
  cpu.chunks.shrink_to_fit();
  unsigned seed = 1;
  for (const auto& t : cpu.trees) {
    CellDetailGpu::Tree g;
    const TreeMeshes m = buildTree(seed++ * 7919u + static_cast<unsigned>(t.base[0] * 13.0f), t.height, t.crown);
    g.bark = m.bark;
    g.leaves = m.leaves;
    for (int a = 0; a < 3; ++a) g.base[a] = t.base[a];
    gpu.trees.push_back(g);
  }
  if (!cpu.hedges.empty()) gpu.hedge = buildHedge(cpu.hedges.data(), static_cast<int>(cpu.hedges.size() / 9), 0.9f);
  if (cpu.landcover.data) {
    gpu.landcover = LoadTextureFromImage(cpu.landcover);
    SetTextureFilter(gpu.landcover, TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(gpu.landcover, TEXTURE_WRAP_CLAMP);
    UnloadImage(cpu.landcover);
    cpu.landcover = Image{};
  }
  if (cpu.ao.data) {
    gpu.ao = LoadTextureFromImage(cpu.ao);
    GenTextureMipmaps(&gpu.ao);
    SetTextureFilter(gpu.ao, TEXTURE_FILTER_TRILINEAR);
    SetTextureWrap(gpu.ao, TEXTURE_WRAP_CLAMP);
    UnloadImage(cpu.ao);
    cpu.ao = Image{};
  }
}

void unloadDetail(CellDetailGpu& gpu) {
  for (auto& m : gpu.meshes) UnloadMesh(m);
  gpu.meshes.clear();
  gpu.mats.clear();
  if (gpu.ao.id) UnloadTexture(gpu.ao);
  gpu.ao = Texture2D{};
  if (gpu.landcover.id) UnloadTexture(gpu.landcover);
  gpu.landcover = Texture2D{};
  for (auto& t : gpu.trees) {
    if (t.bark.vaoId) UnloadMesh(t.bark);
    if (t.leaves.vaoId) UnloadMesh(t.leaves);
  }
  gpu.trees.clear();
  if (gpu.hedge.vaoId) UnloadMesh(gpu.hedge);
  gpu.hedge = Mesh{};
}

}  // namespace rjc
