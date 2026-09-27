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
  // Forest in the colour map stands a crown's height above the ground, as the streamed cells'
  // canopy does (no cliff where a loaded forest cell meets the far view).
  std::vector<float> canopy(static_cast<size_t>(nx_) * ny_, 0.0f);
  if (color_img_.data && color_img_.width >= 2 * nx_ - 1 && color_img_.height >= 2 * ny_ - 1) {
    for (int r = 0; r < ny_; ++r)
      for (int c = 0; c < nx_; ++c) {
        const Color k = GetImageColor(color_img_, 2 * c, 2 * r);
        const float d = std::sqrt(static_cast<float>((k.r - 40) * (k.r - 40) + (k.g - 58) * (k.g - 58) + (k.b - 32) * (k.b - 32))) / 255.0f;
        const float t = std::clamp((d - 0.04f) / 0.06f, 0.0f, 1.0f);
        canopy[static_cast<size_t>(r) * nx_ + c] = 15.0f * (1.0f - t * t * (3.0f - 2.0f * t));
      }
    // two 3 x 3 box passes: forest edges ramp over about 100 m instead of stepping from vertex to vertex
    for (int pass = 0; pass < 2; ++pass) {
      std::vector<float> src = canopy;
      for (int r = 1; r + 1 < ny_; ++r)
        for (int c = 1; c + 1 < nx_; ++c) {
          float sum = 0.0f;
          for (int dr = -1; dr <= 1; ++dr)
            for (int dc = -1; dc <= 1; ++dc) sum += src[static_cast<size_t>(r + dr) * nx_ + (c + dc)];
          canopy[static_cast<size_t>(r) * nx_ + c] = sum / 9.0f;
        }
    }
  }
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
          const float h = heightAt(r, c) + canopy[static_cast<size_t>(std::clamp(r, 0, ny_ - 1)) * nx_ + std::clamp(c, 0, nx_ - 1)];
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
    // Colours by building class (the boxes carry no style): low buildings are houses and shops with
    // tiled or metal pitched roofs, mid-rise concrete with flat grey roofs, towers glass and panels.
    const bool house = b.height < 11.0f;
    static const unsigned char kHouseWall[6][3] = {{176, 166, 150}, {120, 96, 74}, {196, 190, 178}, {92, 74, 60}, {160, 154, 146}, {134, 120, 104}};
    static const unsigned char kHouseRoof[6][3] = {{58, 60, 66}, {64, 66, 72}, {70, 72, 78}, {84, 70, 60}, {58, 68, 82}, {96, 62, 50}};
    static const unsigned char kMidWall[5][3] = {{206, 202, 192}, {182, 180, 174}, {214, 208, 194}, {160, 166, 172}, {196, 190, 180}};
    static const unsigned char kTowerWall[4][3] = {{128, 142, 156}, {150, 160, 168}, {176, 180, 184}, {110, 124, 138}};
    const unsigned char* wc = house ? kHouseWall[h % 6] : b.height < 40.0f ? kMidWall[h % 5] : kTowerWall[h % 4];
    const unsigned char* rc = house ? kHouseRoof[(h >> 8) % 6] : nullptr;
    const unsigned char wall[4] = {wc[0], wc[1], wc[2], static_cast<unsigned char>(house ? 250 : 255)};  // alpha < 1: no window grid
    const unsigned char roof[4] = {rc ? rc[0] : static_cast<unsigned char>(140 + (h >> 8) % 24), rc ? rc[1] : static_cast<unsigned char>(140 + (h >> 8) % 24),
                                   rc ? rc[2] : static_cast<unsigned char>(136 + (h >> 8) % 24), 255};
    rj::geo::Vec3d c[4];
    const double sx[4] = {-1, 1, 1, -1}, sy[4] = {-1, -1, 1, 1};
    for (int k = 0; k < 4; ++k) c[k] = {p.x + ex * b.hl * sx[k] + fx * b.hw * sy[k], p.y + ey * b.hl * sx[k] + fy * b.hw * sy[k], p.z - 1.0};
    const double top = p.z + b.height;
    auto quad = [&G](const rj::geo::Vec3d& A, const rj::geo::Vec3d& B, const rj::geo::Vec3d& C, const rj::geo::Vec3d& D, const unsigned char* col) {
      const double ux = B.x - A.x, uy = B.y - A.y, uz = B.z - A.z, vx = D.x - A.x, vy = D.y - A.y, vz = D.z - A.z;
      float n[3] = {static_cast<float>(uy * vz - uz * vy), static_cast<float>(uz * vx - ux * vz), static_cast<float>(ux * vy - uy * vx)};
      const float l = std::max(1e-6f, std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]));
      for (float& x : n) x /= l;
      const unsigned short base = G.nv();
      for (const rj::geo::Vec3d* q : {&A, &B, &C, &D}) G.vert(*q, n, 0, 0, col, 41.0f);
      G.idx.insert(G.idx.end(), {base, static_cast<unsigned short>(base + 1), static_cast<unsigned short>(base + 2), base,
                                 static_cast<unsigned short>(base + 2), static_cast<unsigned short>(base + 3)});
    };
    auto at = [](const rj::geo::Vec3d& v, double z) { return rj::geo::Vec3d{v.x, v.y, z}; };
    for (int k = 0; k < 4; ++k) quad(c[k], c[(k + 1) % 4], at(c[(k + 1) % 4], top), at(c[k], top), wall);
    if (!house) {
      quad(at(c[0], top), at(c[1], top), at(c[2], top), at(c[3], top), roof);
      continue;
    }
    // gable roof, ridge along the longer side (about 25 degrees), gable ends as triangles (degenerate quads)
    const bool along = b.hl >= b.hw;
    const double rise = (along ? b.hw : b.hl) * 0.47;
    const int i0 = along ? 0 : 1;  // the two eave edges run c[i0]->c[i0+1] and c[i0+2]->c[i0+3]
    const rj::geo::Vec3d e0 = at(c[i0], top), e1 = at(c[(i0 + 1) % 4], top), e2 = at(c[(i0 + 2) % 4], top), e3 = at(c[(i0 + 3) % 4], top);
    const rj::geo::Vec3d r0{(e0.x + e3.x) * 0.5, (e0.y + e3.y) * 0.5, top + rise}, r1{(e1.x + e2.x) * 0.5, (e1.y + e2.y) * 0.5, top + rise};
    quad(e0, e1, r1, r0, roof);
    quad(e2, e3, r0, r1, roof);
    quad(e1, e2, r1, r1, wall);
    quad(e3, e0, r0, r0, wall);
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
