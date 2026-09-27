#include "render/farview.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <sstream>

#include "platform/paths.hpp"
#include "render/gpu_mesh.hpp"
#include "rj/geo/ellipsoid.hpp"
#include "rj/geo/mesh_code.hpp"
#include "world/coords.hpp"
#include "world/world.hpp"

namespace rjc {
namespace {

constexpr int kSub = 24;   // height samples per mesh cell side (pipeline: write_far)
constexpr int kStep = 2;   // far terrain vertices every other sample (~80-100 m)
constexpr float kSkirt = 40.0f;

struct Geo {
  std::vector<float> pos, nrm, uv, t2;
  std::vector<unsigned char> col;
  std::vector<unsigned short> idx;
  unsigned short nv() const { return static_cast<unsigned short>(pos.size() / 3); }
  void vert(const rj::geo::Vec3d& p, const float n[3], float u, float v, const unsigned char c[4], float mat) {
    pos.insert(pos.end(), {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)});
    nrm.insert(nrm.end(), {n[0], n[1], n[2]});
    uv.insert(uv.end(), {u, v});
    t2.insert(t2.end(), {mat, 0.0f});
    col.insert(col.end(), {c[0], c[1], c[2], c[3]});
  }
  Mesh upload() {
    Mesh m{};
    if (idx.empty()) return m;
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

uint32_t hashU(float a, float b) {
  uint32_t h = static_cast<uint32_t>(static_cast<int>(a * 7.0f)) * 374761393u + static_cast<uint32_t>(static_cast<int>(b * 7.0f)) * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}

}  // namespace

float FarView::heightAt(int row, int col) const {
  row = std::clamp(row, 0, ny_ - 1);
  col = std::clamp(col, 0, nx_ - 1);
  return static_cast<float>(height_[static_cast<size_t>(row) * nx_ + col]) / 20.0f - 100.0f;
}

bool FarView::load(const std::filesystem::path& dir) {
  auto txt = readText(dir / "far.txt");
  if (!txt) return false;
  std::istringstream in(*txt);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string k;
    ls >> k;
    if (k == "grid") ls >> lat0_ >> lon0_ >> lat1_ >> lon1_ >> nx_ >> ny_;
    else if (k == "frame") ls >> frame_lat_ >> frame_lon_;
  }
  if (nx_ < 2 || ny_ < 2) return false;
  // heights: "RJFARH01", nx, ny, raw length, raw DEFLATE of uint16 (h + 100) * 20, north row first
  auto hb = readFile(dir / "far_height.bin");
  if (!hb || hb->size() < 20 || std::memcmp(hb->data(), "RJFARH01", 8) != 0) return false;
  uint32_t hdr[3];
  std::memcpy(hdr, hb->data() + 8, 12);
  if (static_cast<int>(hdr[0]) != nx_ || static_cast<int>(hdr[1]) != ny_) return false;
  int n = 0;
  unsigned char* raw = DecompressData(hb->data() + 20, static_cast<int>(hb->size() - 20), &n);
  if (!raw || static_cast<uint32_t>(n) != hdr[2] || static_cast<size_t>(n) != static_cast<size_t>(nx_) * ny_ * 2) {
    if (raw) MemFree(raw);
    return false;
  }
  height_.resize(static_cast<size_t>(nx_) * ny_);
  std::memcpy(height_.data(), raw, static_cast<size_t>(n));
  MemFree(raw);
  if (auto bb = readFile(dir / "far_blds.bin"); bb && bb->size() >= 12 && std::memcmp(bb->data(), "RJFARB01", 8) == 0) {
    uint32_t cnt = 0;
    std::memcpy(&cnt, bb->data() + 8, 4);
    if (bb->size() >= 12 + static_cast<size_t>(cnt) * sizeof(Box)) {
      boxes_.resize(cnt);
      std::memcpy(boxes_.data(), bb->data() + 12, static_cast<size_t>(cnt) * sizeof(Box));
    }
  }
  const std::string cpath = pathToUtf8(dir / "far_color.png"), spath = pathToUtf8(dir / "snow.png");
  if (auto cb = readFile(dir / "far_color.png")) color_img_ = LoadImageFromMemory(".png", cb->data(), static_cast<int>(cb->size()));
  if (auto sb = readFile(dir / "snow.png")) snow_img_ = LoadImageFromMemory(".png", sb->data(), static_cast<int>(sb->size()));
  loaded_ = color_img_.data != nullptr;
  return loaded_;
}

void FarView::build() {
  if (!loaded_ || built_) return;
  color_ = LoadTextureFromImage(color_img_);
  GenTextureMipmaps(&color_);
  SetTextureFilter(color_, TEXTURE_FILTER_TRILINEAR);
  SetTextureWrap(color_, TEXTURE_WRAP_CLAMP);
  UnloadImage(color_img_);
  color_img_ = Image{};
  if (snow_img_.data) {
    snow_ = LoadTextureFromImage(snow_img_);
    SetTextureFilter(snow_, TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(snow_, TEXTURE_WRAP_CLAMP);
    UnloadImage(snow_img_);
    snow_img_ = Image{};
  }
  const double dlat = (lat1_ - lat0_) / (ny_ - 1), dlon = (lon1_ - lon0_) / (nx_ - 1);
  const int cells_y = (ny_ - 1) / kSub, cells_x = (nx_ - 1) / kSub;
  const double my = 110574.0 * dlat, mx = 111320.0 * std::cos((lat0_ + lat1_) * 0.5 * 3.14159265358979 / 180.0) * dlon;
  std::map<std::string, size_t> by_code;
  const unsigned char white[4] = {255, 255, 255, 255};
  for (int cy = 0; cy < cells_y; ++cy)
    for (int cx = 0; cx < cells_x; ++cx) {
      // grid rows of this cell (row 0 = north)
      const int r_s = (ny_ - 1) - cy * kSub, r_n = r_s - kSub;
      const int c_w = cx * kSub, c_e = c_w + kSub;
      float zmax = -1e9f;
      for (int r = r_n; r <= r_s; ++r)
        for (int c = c_w; c <= c_e; ++c) zmax = std::max(zmax, heightAt(r, c));
      if (zmax < 0.3f) continue;  // open sea (the ocean plane covers it)
      const double la_c = lat1_ - (r_n + kSub * 0.5) * dlat, lo_c = lon0_ + (c_w + kSub * 0.5) * dlon;
      Tile t;
      auto code = rj::geo::MeshCode::fromLatLon({la_c, lo_c}, 3);
      if (!code) continue;
      t.mesh = code->str();
      t.frame = rj::geo::LocalFrame({la_c, lo_c, 0.0});
      t.zmax = zmax;
      Geo g;
      const int n = kSub / kStep + 1;
      for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
          const int r = r_n + i * kStep, c = c_w + j * kStep;
          const double la = lat1_ - r * dlat, lo = lon0_ + c * dlon;
          const float h = heightAt(r, c);
          // smooth normal from the full-resolution samples
          const float dzdx = (heightAt(r, c + 1) - heightAt(r, c - 1)) / static_cast<float>(2.0 * mx);
          const float dzdy = (heightAt(r - 1, c) - heightAt(r + 1, c)) / static_cast<float>(2.0 * my);
          float nv[3] = {-dzdx, -dzdy, 1.0f};
          const float l = std::sqrt(nv[0] * nv[0] + nv[1] * nv[1] + 1.0f);
          for (float& q : nv) q /= l;
          g.vert(t.frame.geodeticToLocal({la, lo, h}), nv, static_cast<float>((lo - lon0_) / (lon1_ - lon0_)),
                 static_cast<float>((lat1_ - la) / (lat1_ - lat0_)), white, 0.0f);
        }
      for (int i = 0; i + 1 < n; ++i)
        for (int j = 0; j + 1 < n; ++j) {
          // rows run north to south: keep the triangles counter-clockwise seen from above (ENU)
          const auto a = static_cast<unsigned short>(i * n + j), b = static_cast<unsigned short>(i * n + j + 1);
          const auto d = static_cast<unsigned short>((i + 1) * n + j), e = static_cast<unsigned short>((i + 1) * n + j + 1);
          g.idx.insert(g.idx.end(), {a, d, e, a, e, b});
        }
      // skirts hanging down from the edges (no cracks against the detailed neighbouring cells)
      auto skirt = [&](int i0, int j0, int di, int dj) {
        for (int k = 0; k + 1 < n; ++k) {
          const int ia = (i0 + di * k) * n + (j0 + dj * k), ib = (i0 + di * (k + 1)) * n + (j0 + dj * (k + 1));
          const unsigned short base = g.nv();
          for (int v : {ia, ib}) {
            const rj::geo::Vec3d p{g.pos[static_cast<size_t>(v) * 3], g.pos[static_cast<size_t>(v) * 3 + 1],
                                   g.pos[static_cast<size_t>(v) * 3 + 2] - kSkirt};
            const float nn[3] = {g.nrm[static_cast<size_t>(v) * 3], g.nrm[static_cast<size_t>(v) * 3 + 1], g.nrm[static_cast<size_t>(v) * 3 + 2]};
            g.vert(p, nn, g.uv[static_cast<size_t>(v) * 2], g.uv[static_cast<size_t>(v) * 2 + 1], white, 0.0f);
          }
          g.idx.insert(g.idx.end(), {static_cast<unsigned short>(ia), static_cast<unsigned short>(ib), static_cast<unsigned short>(base + 1),
                                     static_cast<unsigned short>(ia), static_cast<unsigned short>(base + 1), base});
        }
      };
      skirt(0, 0, 0, 1);
      skirt(n - 1, 0, 0, 1);
      skirt(0, 0, 1, 0);
      skirt(0, n - 1, 1, 0);
      t.terrain = g.upload();
      t.radius = static_cast<float>(std::hypot(kSub * mx, kSub * my) * 0.5 + 50.0);
      by_code[t.mesh] = tiles_.size();
      tiles_.push_back(std::move(t));
    }
  // building boxes into the tiles they stand in
  const rj::geo::LocalFrame cf({frame_lat_, frame_lon_, 0.0});
  std::map<size_t, std::vector<Geo>> geos;
  for (const Box& b : boxes_) {
    const rj::geo::Geodetic g = rj::geo::ecefToGeodetic(cf.localToEcef({b.x, b.y, b.ground}));
    auto code = rj::geo::MeshCode::fromLatLon({g.lat_deg, g.lon_deg}, 3);
    if (!code) continue;
    auto it = by_code.find(code->str());
    if (it == by_code.end()) continue;
    auto& list = geos[it->second];
    if (list.empty() || list.back().nv() > 65000 - 24) list.emplace_back();
    Geo& G = list.back();
    const Tile& t = tiles_[it->second];
    const rj::geo::Vec3d p = t.frame.geodeticToLocal(g);
    const double ex = std::cos(b.yaw), ey = std::sin(b.yaw);
    const double fx = -ey, fy = ex;
    const uint32_t h = hashU(b.x, b.y);
    static const unsigned char kWall[5][3] = {{206, 202, 192}, {182, 180, 174}, {214, 208, 194}, {160, 166, 172}, {196, 190, 180}};
    const unsigned char* wc = kWall[h % 5];
    const unsigned char wall[4] = {wc[0], wc[1], wc[2], 255}, roof[4] = {128, 128, 126, 255};
    rj::geo::Vec3d c[4];
    const double sx[4] = {-1, 1, 1, -1}, sy[4] = {-1, -1, 1, 1};
    for (int k = 0; k < 4; ++k) c[k] = {p.x + ex * b.hl * sx[k] + fx * b.hw * sy[k], p.y + ey * b.hl * sx[k] + fy * b.hw * sy[k], p.z - 1.0};
    const double top = p.z + b.height;
    for (int k = 0; k < 4; ++k) {
      const rj::geo::Vec3d& A = c[k];
      const rj::geo::Vec3d& B = c[(k + 1) % 4];
      float nv[3] = {static_cast<float>(B.y - A.y), static_cast<float>(-(B.x - A.x)), 0.0f};
      const float l = std::max(1e-6f, std::hypot(nv[0], nv[1]));
      nv[0] /= l;
      nv[1] /= l;
      const unsigned short base = G.nv();
      G.vert(A, nv, 0, 0, wall, 41.0f);
      G.vert(B, nv, 0, 0, wall, 41.0f);
      G.vert({B.x, B.y, top}, nv, 0, 0, wall, 41.0f);
      G.vert({A.x, A.y, top}, nv, 0, 0, wall, 41.0f);
      G.idx.insert(G.idx.end(), {base, static_cast<unsigned short>(base + 1), static_cast<unsigned short>(base + 2), base,
                                 static_cast<unsigned short>(base + 2), static_cast<unsigned short>(base + 3)});
    }
    const float up[3] = {0, 0, 1};
    const unsigned short base = G.nv();
    for (int k = 0; k < 4; ++k) G.vert({c[k].x, c[k].y, top}, up, 0, 0, roof, 41.0f);
    G.idx.insert(G.idx.end(), {base, static_cast<unsigned short>(base + 1), static_cast<unsigned short>(base + 2), base,
                               static_cast<unsigned short>(base + 2), static_cast<unsigned short>(base + 3)});
  }
  for (auto& [ti, list] : geos)
    for (auto& G : list)
      if (!G.idx.empty()) tiles_[ti].boxes.push_back(G.upload());
  boxes_.clear();
  boxes_.shrink_to_fit();
  height_.clear();
  height_.shrink_to_fit();
  built_ = true;
  TraceLog(LOG_INFO, "RJ: far view: %d terrain tiles", static_cast<int>(tiles_.size()));
}

void FarView::unload() {
  for (auto& t : tiles_) {
    if (t.terrain.vaoId) UnloadMesh(t.terrain);
    for (auto& m : t.boxes) UnloadMesh(m);
  }
  tiles_.clear();
  if (color_.id) UnloadTexture(color_);
  if (snow_.id) UnloadTexture(snow_);
  color_ = snow_ = Texture2D{};
  if (color_img_.data) UnloadImage(color_img_);
  if (snow_img_.data) UnloadImage(snow_img_);
  color_img_ = snow_img_ = Image{};
  built_ = false;
}

void FarView::mapping(const World& world, Vector3& u, Vector3& v) const {
  // uv (0, 0) at the north-west corner, u east, v south; solve the affine map from raylib (x, z)
  const Vector3 A = enuToRl(world.toLocal({lat1_, lon0_, 0.0}));
  const Vector3 B = enuToRl(world.toLocal({lat1_, lon1_, 0.0}));
  const Vector3 C = enuToRl(world.toLocal({lat0_, lon0_, 0.0}));
  const double a = B.x - A.x, b = C.x - A.x, c = B.z - A.z, d = C.z - A.z;  // [x z] = [a b; c d] [u v]
  const double det = a * d - b * c;
  if (std::fabs(det) < 1e-9) return;
  const double i00 = d / det, i01 = -b / det, i10 = -c / det, i11 = a / det;
  u = {static_cast<float>(i00), static_cast<float>(i01), static_cast<float>(-(i00 * A.x + i01 * A.z))};
  v = {static_cast<float>(i10), static_cast<float>(i11), static_cast<float>(-(i10 * A.x + i11 * A.z))};
}

}  // namespace rjc
