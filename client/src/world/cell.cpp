#include "world/cell.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace rjc {
namespace {

class Reader {
 public:
  Reader(const std::vector<unsigned char>& d) : d_(d) {}
  bool ok() const { return ok_; }
  size_t pos() const { return p_; }
  template <class T>
  T get() {
    T v{};
    if (p_ + sizeof(T) > d_.size()) {
      ok_ = false;
      return v;
    }
    std::memcpy(&v, d_.data() + p_, sizeof(T));
    p_ += sizeof(T);
    return v;
  }
  bool bytes(void* dst, size_t n) {
    if (p_ + n > d_.size()) return ok_ = false;
    if (n) std::memcpy(dst, d_.data() + p_, n);
    p_ += n;
    return true;
  }
  void skip(size_t n) {
    if (p_ + n > d_.size()) ok_ = false;
    else p_ += n;
  }

 private:
  const std::vector<unsigned char>& d_;
  size_t p_ = 0;
  bool ok_ = true;
};

template <class T>
T* rlCopy(const std::vector<T>& v) {
  if (v.empty()) return nullptr;
  T* p = static_cast<T*>(MemAlloc(static_cast<unsigned int>(v.size() * sizeof(T))));
  std::memcpy(p, v.data(), v.size() * sizeof(T));
  return p;
}

// Keep only the index array (DrawMesh checks mesh.indices to pick glDrawElements).
void releaseVertexCopies(Mesh& m) {
  MemFree(m.vertices);
  MemFree(m.normals);
  MemFree(m.colors);
  MemFree(m.texcoords);
  m.vertices = m.normals = m.texcoords = nullptr;
  m.colors = nullptr;
}

}  // namespace

bool parseCell(const std::vector<unsigned char>& data, CellCpu& c, std::string& err) {
  Reader r(data);
  char magic[8];
  r.bytes(magic, 8);
  if (!r.ok() || std::memcmp(magic, "RJCELL01", 8) != 0) {
    err = "bad magic";
    return false;
  }
  char mesh[16];
  r.bytes(mesh, 16);
  c.mesh.assign(mesh, strnlen(mesh, 16));
  for (double& v : c.anchor) v = r.get<double>();
  for (double& v : c.bounds) v = r.get<double>();
  const uint32_t nb = r.get<uint32_t>(), nc = r.get<uint32_t>(), nfp = r.get<uint32_t>(), ns = r.get<uint32_t>();
  c.tnx = static_cast<int>(r.get<uint32_t>());
  c.tny = static_cast<int>(r.get<uint32_t>());
  c.tlat0 = r.get<double>();
  c.tlon0 = r.get<double>();
  c.tdlat = r.get<double>();
  c.tdlon = r.get<double>();
  const uint32_t png_len = r.get<uint32_t>();
  r.skip(12);
  if (!r.ok()) {
    err = "truncated header";
    return false;
  }
  std::string strings(ns, '\0');
  r.bytes(strings.data(), ns);
  c.buildings.resize(nb);
  for (auto& b : c.buildings) {
    const uint32_t io = r.get<uint32_t>(), il = r.get<uint32_t>(), no = r.get<uint32_t>(), nl = r.get<uint32_t>();
    if (io + il > ns || no + nl > ns) {
      err = "bad string ref";
      return false;
    }
    b.id = strings.substr(io, il);
    b.name = strings.substr(no, nl);
    b.usage = r.get<uint16_t>();
    b.bclass = r.get<uint16_t>();
    b.measured_height = r.get<float>();
    b.storeys_above = r.get<int16_t>();
    b.storeys_below = r.get<int16_t>();
    b.lod = r.get<uint8_t>();
    b.geometry_status = r.get<uint8_t>();
    b.interior_status = r.get<uint8_t>();
    r.get<uint8_t>();  // flags
    b.source_index = r.get<uint32_t>();
    b.ground_z = r.get<float>();
    for (float& v : b.bmin) v = r.get<float>();
    for (float& v : b.bmax) v = r.get<float>();
    b.fp_first = r.get<uint32_t>();
    b.fp_count = r.get<uint32_t>();
    r.skip(12);  // chunk, first_index, index_count (whole chunks are drawn)
  }
  c.footprints.resize(static_cast<size_t>(nfp) * 2);
  r.bytes(c.footprints.data(), c.footprints.size() * sizeof(float));
  c.chunks.resize(nc);
  for (auto& ch : c.chunks) {
    const uint32_t nv = r.get<uint32_t>(), ni = r.get<uint32_t>();
    if (!r.ok() || nv > 65535) {
      err = "bad chunk";
      return false;
    }
    ch.pos.resize(nv * 3);
    ch.nrm.resize(nv * 3);
    ch.col.resize(nv * 4);
    ch.idx.resize(ni);
    r.bytes(ch.pos.data(), ch.pos.size() * 4);
    r.bytes(ch.nrm.data(), ch.nrm.size() * 4);
    r.bytes(ch.col.data(), ch.col.size());
    r.bytes(ch.idx.data(), ch.idx.size() * 2);
    r.skip((4 - (ni * 2) % 4) % 4);
  }
  c.theight.resize(static_cast<size_t>(c.tnx) * static_cast<size_t>(c.tny));
  r.bytes(c.theight.data(), c.theight.size() * 4);
  std::vector<unsigned char> png(png_len);
  r.bytes(png.data(), png_len);
  if (!r.ok()) {
    err = "truncated body";
    return false;
  }
  if (png_len) c.ground = LoadImageFromMemory(".png", png.data(), static_cast<int>(png_len));
  c.bytes = data.size();
  return true;
}

void prepareTerrain(CellCpu& c) {
  const int nx = c.tnx, ny = c.tny;
  if (nx < 2 || ny < 2) return;
  const rj::geo::LocalFrame frame({c.anchor[0], c.anchor[1], c.anchor[2]});
  c.tpos.resize(static_cast<size_t>(nx * ny) * 3);
  c.tuv.resize(static_cast<size_t>(nx * ny) * 2);
  for (int i = 0; i < ny; ++i)
    for (int j = 0; j < nx; ++j) {
      const size_t k = static_cast<size_t>(i * nx + j);
      const auto p = frame.geodeticToLocal({c.tlat0 + i * c.tdlat, c.tlon0 + j * c.tdlon, c.anchor[2]});
      c.tpos[k * 3 + 0] = static_cast<float>(p.x);
      c.tpos[k * 3 + 1] = static_cast<float>(p.y);
      c.tpos[k * 3 + 2] = c.theight[k];
      c.tuv[k * 2 + 0] = static_cast<float>(j) / static_cast<float>(nx - 1);
      c.tuv[k * 2 + 1] = 1.0f - static_cast<float>(i) / static_cast<float>(ny - 1);  // image row 0 = north
    }
  c.tnrm.assign(c.tpos.size(), 0.0f);
  auto P = [&](int i, int j) {
    const size_t k = static_cast<size_t>(std::clamp(i, 0, ny - 1) * nx + std::clamp(j, 0, nx - 1)) * 3;
    return std::array<float, 3>{c.tpos[k], c.tpos[k + 1], c.tpos[k + 2]};
  };
  for (int i = 0; i < ny; ++i)
    for (int j = 0; j < nx; ++j) {
      const auto e = P(i, j + 1), w = P(i, j - 1), n = P(i + 1, j), s = P(i - 1, j);
      const float dx[3] = {e[0] - w[0], e[1] - w[1], e[2] - w[2]};
      const float dy[3] = {n[0] - s[0], n[1] - s[1], n[2] - s[2]};
      float nn[3] = {dx[1] * dy[2] - dx[2] * dy[1], dx[2] * dy[0] - dx[0] * dy[2], dx[0] * dy[1] - dx[1] * dy[0]};
      const float l = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
      const size_t k = static_cast<size_t>(i * nx + j) * 3;
      for (int a = 0; a < 3; ++a) c.tnrm[k + a] = l > 0 ? nn[a] / l : (a == 2 ? 1.0f : 0.0f);
    }
  c.tidx.clear();
  c.tidx.reserve(static_cast<size_t>((nx - 1) * (ny - 1) * 6));
  for (int i = 0; i < ny - 1; ++i)
    for (int j = 0; j < nx - 1; ++j) {
      const auto a = static_cast<unsigned short>(i * nx + j), b = static_cast<unsigned short>(i * nx + j + 1);
      const auto d = static_cast<unsigned short>((i + 1) * nx + j), e = static_cast<unsigned short>((i + 1) * nx + j + 1);
      c.tidx.insert(c.tidx.end(), {a, b, e, a, e, d});  // CCW seen from above (ENU)
    }
}

void uploadCell(CellCpu& cpu, CellGpu& gpu) {
  for (auto& ch : cpu.chunks) {
    Mesh m{};
    m.vertexCount = static_cast<int>(ch.pos.size() / 3);
    m.triangleCount = static_cast<int>(ch.idx.size() / 3);
    m.vertices = rlCopy(ch.pos);
    m.normals = rlCopy(ch.nrm);
    m.colors = rlCopy(ch.col);
    m.indices = rlCopy(ch.idx);
    UploadMesh(&m, false);
    releaseVertexCopies(m);
    gpu.chunks.push_back(m);
    ch = {};
  }
  cpu.chunks.clear();
  if (!cpu.tidx.empty()) {
    Mesh t{};
    t.vertexCount = static_cast<int>(cpu.tpos.size() / 3);
    t.triangleCount = static_cast<int>(cpu.tidx.size() / 3);
    t.vertices = rlCopy(cpu.tpos);
    t.normals = rlCopy(cpu.tnrm);
    t.texcoords = rlCopy(cpu.tuv);
    t.indices = rlCopy(cpu.tidx);
    UploadMesh(&t, false);
    releaseVertexCopies(t);
    gpu.terrain = t;
  }
  if (cpu.ground.data) {
    gpu.ground = LoadTextureFromImage(cpu.ground);
    GenTextureMipmaps(&gpu.ground);
    SetTextureFilter(gpu.ground, TEXTURE_FILTER_TRILINEAR);
    SetTextureWrap(gpu.ground, TEXTURE_WRAP_CLAMP);
    UnloadImage(cpu.ground);
    cpu.ground = Image{};
  }
  gpu.uploaded = true;
}

void unloadCell(CellGpu& gpu) {
  for (auto& m : gpu.chunks) UnloadMesh(m);
  gpu.chunks.clear();
  if (gpu.terrain.vaoId) UnloadMesh(gpu.terrain);
  gpu.terrain = Mesh{};
  if (gpu.ground.id) UnloadTexture(gpu.ground);
  gpu.ground = Texture2D{};
  gpu.uploaded = false;
}

}  // namespace rjc
