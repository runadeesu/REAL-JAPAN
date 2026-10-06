#include "render/fbx_import.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

#include "platform/paths.hpp"
#include "raylib.h"

extern "C" int zsinflate(void* out, int cap, const void* mem, int size);  // raylib's sinfl (zlib streams)

namespace rjc {

namespace {

// ---------------------------------------------------------------------------------------------
// Binary FBX reader

struct FProp {
  char t = 0;
  int64_t i = 0;
  double d = 0;
  std::string s;
  const unsigned char* raw = nullptr;
  size_t raw_n = 0;
  std::vector<double> fa;   // f / d arrays
  std::vector<int64_t> ia;  // i / l / b arrays
};
struct FNode {
  std::string name;
  std::vector<FProp> props;
  std::vector<FNode> kids;
};

class FbxReader {
 public:
  FbxReader(const unsigned char* d, size_t n) : d_(d), n_(n) {}
  bool read(std::vector<FNode>& top, std::string& err) {
    if (n_ < 27 || std::memcmp(d_, "Kaydara FBX Binary  ", 20) != 0) {
      err = "not a binary FBX (ASCII FBX is not supported)";
      return false;
    }
    uint32_t ver;
    std::memcpy(&ver, d_ + 23, 4);
    wide_ = ver >= 7500;
    size_t off = 27;
    const size_t hdr = wide_ ? 25 : 13;
    while (off + hdr < n_) {
      FNode node;
      bool null_rec = false;
      if (!readNode(off, node, null_rec, 0)) {
        err = "broken FBX";
        return false;
      }
      if (null_rec) break;
      top.push_back(std::move(node));
    }
    return true;
  }

 private:
  template <class T>
  bool get(size_t& off, T& v) {
    if (off + sizeof(T) > n_) return false;
    std::memcpy(&v, d_ + off, sizeof(T));
    off += sizeof(T);
    return true;
  }
  bool readNode(size_t& off, FNode& node, bool& null_rec, int depth) {
    if (depth > 64) return false;
    uint64_t end = 0, nprop = 0, plen = 0;
    if (wide_) {
      if (!get(off, end) || !get(off, nprop) || !get(off, plen)) return false;
    } else {
      uint32_t e, np, pl;
      if (!get(off, e) || !get(off, np) || !get(off, pl)) return false;
      end = e, nprop = np, plen = pl;
    }
    uint8_t nl;
    if (!get(off, nl)) return false;
    if (end == 0) {
      null_rec = true;
      return true;
    }
    if (end > n_ || off + nl > n_) return false;
    node.name.assign(reinterpret_cast<const char*>(d_ + off), nl);
    off += nl;
    for (uint64_t k = 0; k < nprop; ++k) {
      FProp p;
      if (off >= n_) return false;
      p.t = static_cast<char>(d_[off++]);
      switch (p.t) {
        case 'Y': { int16_t v; if (!get(off, v)) return false; p.i = v; break; }
        case 'C': { uint8_t v; if (!get(off, v)) return false; p.i = v; break; }
        case 'I': { int32_t v; if (!get(off, v)) return false; p.i = v; p.d = v; break; }
        case 'F': { float v; if (!get(off, v)) return false; p.d = v; break; }
        case 'D': { double v; if (!get(off, v)) return false; p.d = v; break; }
        case 'L': { int64_t v; if (!get(off, v)) return false; p.i = v; break; }
        case 'f': case 'd': case 'l': case 'i': case 'b': {
          uint32_t cnt, enc, clen;
          if (!get(off, cnt) || !get(off, enc) || !get(off, clen) || off + clen > n_) return false;
          const size_t el = (p.t == 'd' || p.t == 'l') ? 8 : (p.t == 'b' ? 1 : 4);
          if (cnt > (1u << 28)) return false;
          std::vector<unsigned char> buf;
          const unsigned char* src = d_ + off;
          if (enc == 1) {
            buf.resize(cnt * el + 16);
            const int got = zsinflate(buf.data(), static_cast<int>(buf.size()), src, static_cast<int>(clen));
            if (got < static_cast<int>(cnt * el)) return false;
            src = buf.data();
          } else if (clen < cnt * el) {
            return false;
          }
          off += clen;
          if (p.t == 'f' || p.t == 'd') {
            p.fa.resize(cnt);
            for (uint32_t j = 0; j < cnt; ++j) {
              if (p.t == 'd') { double v; std::memcpy(&v, src + j * 8, 8); p.fa[j] = v; }
              else { float v; std::memcpy(&v, src + j * 4, 4); p.fa[j] = v; }
            }
          } else {
            p.ia.resize(cnt);
            for (uint32_t j = 0; j < cnt; ++j) {
              if (p.t == 'l') { int64_t v; std::memcpy(&v, src + j * 8, 8); p.ia[j] = v; }
              else if (p.t == 'i') { int32_t v; std::memcpy(&v, src + j * 4, 4); p.ia[j] = v; }
              else p.ia[j] = src[j];
            }
          }
          break;
        }
        case 'S': case 'R': {
          uint32_t len;
          if (!get(off, len) || off + len > n_) return false;
          if (p.t == 'S') p.s.assign(reinterpret_cast<const char*>(d_ + off), len);
          else p.raw = d_ + off, p.raw_n = len;
          off += len;
          break;
        }
        default:
          return false;
      }
      node.props.push_back(std::move(p));
    }
    while (off < end) {
      FNode kid;
      bool nr = false;
      if (!readNode(off, kid, nr, depth + 1)) return false;
      if (nr) break;
      node.kids.push_back(std::move(kid));
    }
    off = static_cast<size_t>(end);
    return true;
  }

  const unsigned char* d_;
  size_t n_;
  bool wide_ = false;
};

const FNode* child(const FNode& n, const char* name) {
  for (const auto& k : n.kids)
    if (k.name == name) return &k;
  return nullptr;
}
const FProp* value(const FNode& n, const char* name) {
  const FNode* k = child(n, name);
  return k && !k->props.empty() ? &k->props[0] : nullptr;
}
std::string valueS(const FNode& n, const char* name, const std::string& def = "") {
  const FProp* p = value(n, name);
  return p && (p->t == 'S') ? p->s : def;
}
std::map<std::string, std::vector<double>> props70(const FNode& n) {
  std::map<std::string, std::vector<double>> out;
  if (const FNode* p = child(n, "Properties70"))
    for (const auto& k : p->kids)
      if (k.name == "P" && !k.props.empty()) {
        std::vector<double> v;
        for (size_t i = 4; i < k.props.size(); ++i) v.push_back(k.props[i].t == 'L' || k.props[i].t == 'C' || k.props[i].t == 'Y' ? static_cast<double>(k.props[i].i) : k.props[i].d);
        out[k.props[0].s] = v;
      }
  return out;
}
std::string objName(const FNode& n) {
  if (n.props.size() < 2 || n.props[1].t != 'S') return "";
  const auto& s = n.props[1].s;
  const auto z = s.find('\0');
  return z == std::string::npos ? s : s.substr(0, z);
}
std::string objClass(const FNode& n) { return n.props.size() > 2 && n.props[2].t == 'S' ? n.props[2].s : ""; }

// ---------------------------------------------------------------------------------------------
// 4x4 (column vectors)

struct M4 {
  double m[4][4];
  static M4 id() {
    M4 r{};
    for (int i = 0; i < 4; ++i) r.m[i][i] = 1;
    return r;
  }
  M4 operator*(const M4& b) const {
    M4 r{};
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j)
        for (int k = 0; k < 4; ++k) r.m[i][j] += m[i][k] * b.m[k][j];
    return r;
  }
  std::array<double, 3> point(double x, double y, double z) const {
    return {m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3], m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3],
            m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3]};
  }
};
M4 fromFbx(const std::vector<double>& a) {  // FBX stores column-major
  M4 r = M4::id();
  if (a.size() < 16) return r;
  for (int c = 0; c < 4; ++c)
    for (int rr = 0; rr < 4; ++rr) r.m[rr][c] = a[static_cast<size_t>(c * 4 + rr)];
  return r;
}
M4 rot(int axis, double a) {
  M4 r = M4::id();
  const double c = std::cos(a), s = std::sin(a);
  const int i = (axis + 1) % 3, j = (axis + 2) % 3;
  r.m[i][i] = c, r.m[i][j] = -s, r.m[j][i] = s, r.m[j][j] = c;
  return r;
}
M4 euler(const std::vector<double>& deg, int order = 0) {
  static const char* seqs[] = {"xyz", "xzy", "yzx", "yxz", "zxy", "zyx"};
  const char* seq = seqs[std::clamp(order, 0, 5)];
  M4 r = M4::id();
  for (int k = 0; k < 3; ++k) {
    const int ax = seq[k] - 'x';
    const double a = deg.size() > static_cast<size_t>(ax) ? deg[static_cast<size_t>(ax)] * PI / 180.0 : 0.0;
    r = rot(ax, a) * r;
  }
  return r;
}
bool invert(const M4& a, M4& out) {  // general 4x4 inverse (Gauss-Jordan)
  double w[4][8];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 8; ++j) w[i][j] = j < 4 ? a.m[i][j] : (j - 4 == i ? 1.0 : 0.0);
  for (int c = 0; c < 4; ++c) {
    int p = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(w[r][c]) > std::fabs(w[p][c])) p = r;
    if (std::fabs(w[p][c]) < 1e-12) return false;
    for (int j = 0; j < 8; ++j) std::swap(w[c][j], w[p][j]);
    const double iv = 1.0 / w[c][c];
    for (int j = 0; j < 8; ++j) w[c][j] *= iv;
    for (int r = 0; r < 4; ++r)
      if (r != c) {
        const double f = w[r][c];
        for (int j = 0; j < 8; ++j) w[r][j] -= f * w[c][j];
      }
  }
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) out.m[i][j] = w[i][j + 4];
  return true;
}
std::vector<double> pget(const std::map<std::string, std::vector<double>>& p, const char* k, std::vector<double> def) {
  auto it = p.find(k);
  return it == p.end() || it->second.size() < def.size() ? def : it->second;
}
M4 localMatrix(const std::map<std::string, std::vector<double>>& p) {
  M4 t = M4::id();
  const auto tr = pget(p, "Lcl Translation", {0, 0, 0});
  t.m[0][3] = tr[0], t.m[1][3] = tr[1], t.m[2][3] = tr[2];
  const int order = static_cast<int>(pget(p, "RotationOrder", {0})[0]);
  const M4 r = euler(pget(p, "Lcl Rotation", {0, 0, 0}), order);
  const M4 pre = euler(pget(p, "PreRotation", {0, 0, 0}));
  M4 post = euler(pget(p, "PostRotation", {0, 0, 0})), post_i;
  if (!invert(post, post_i)) post_i = M4::id();
  M4 s = M4::id();
  const auto sc = pget(p, "Lcl Scaling", {1, 1, 1});
  s.m[0][0] = sc[0], s.m[1][1] = sc[1], s.m[2][2] = sc[2];
  return t * pre * r * post_i * s;
}

// ---------------------------------------------------------------------------------------------
// The game's skeleton (render/characters.hpp CharBone order)

const char* const kKeep[] = {"Hips", "Spine", "Spine1", "Spine2", "Neck", "Head",
                             "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand", "LeftFingers", "LeftThumb",
                             "RightShoulder", "RightArm", "RightForeArm", "RightHand", "RightFingers", "RightThumb",
                             "LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToeBase", "RightUpLeg", "RightLeg", "RightFoot", "RightToeBase"};
constexpr int kNB = 26;
const int kParent[kNB] = {-1, 0, 1, 2, 3, 4, 3, 6, 7, 8, 9, 9, 3, 12, 13, 14, 15, 15, 0, 18, 19, 20, 0, 22, 23, 24};

int keepIndex(const std::string& n) {
  for (int i = 0; i < kNB; ++i)
    if (n == kKeep[i]) return i;
  return -1;
}
std::string baseName(const std::string& n) {
  const auto c = n.rfind(':');
  return c == std::string::npos ? n : n.substr(c + 1);
}
bool startsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }
int targetBone(const std::string& full) {
  const std::string n = baseName(full);
  if (int k = keepIndex(n); k >= 0) return k;
  for (const char* side : {"Left", "Right"}) {
    const std::string sd = side;
    if (startsWith(n, sd + "HandThumb")) return keepIndex(sd + "Thumb");
    if (startsWith(n, sd + "Hand") && (n.find("Index") != std::string::npos || n.find("Middle") != std::string::npos ||
                                       n.find("Ring") != std::string::npos || n.find("Pinky") != std::string::npos))
      return keepIndex(sd + "Fingers");
    if (startsWith(n, sd + "Toe")) return keepIndex(sd + "ToeBase");
    if (n == sd + "Eye") return keepIndex("Head");
  }
  if (n == "HeadTop_End") return keepIndex("Head");
  return -1;
}
std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
std::string fileBase(std::string f) {
  std::replace(f.begin(), f.end(), '\\', '/');
  const auto s = f.rfind('/');
  return s == std::string::npos ? f : f.substr(s + 1);
}

struct Part {
  std::vector<float> pos, nrm, uv;  // 3, 3, 2 per vertex
  std::vector<float> w;             // kNB per vertex
  std::vector<uint32_t> tri;
  int tex = -1;
  int kind = 0;
};

void put(std::vector<unsigned char>& o, const void* p, size_t n) {
  const auto* b = static_cast<const unsigned char*>(p);
  o.insert(o.end(), b, b + n);
}
template <class T>
void putv(std::vector<unsigned char>& o, T v) {
  put(o, &v, sizeof v);
}

}  // namespace

bool convertFbxCharacter(const std::vector<unsigned char>& fbx, const std::filesystem::path& dir, int max_tex, std::vector<unsigned char>& out,
                         std::string& err) {
  std::vector<FNode> top;
  if (!FbxReader(fbx.data(), fbx.size()).read(top, err)) return false;
  const FNode* objects_node = nullptr;
  const FNode* conns = nullptr;
  const FNode* gsn = nullptr;
  for (const auto& n : top) {
    if (n.name == "Objects") objects_node = &n;
    else if (n.name == "Connections") conns = &n;
    else if (n.name == "GlobalSettings") gsn = &n;
  }
  if (!objects_node || !conns) {
    err = "FBX without objects";
    return false;
  }
  std::unordered_map<int64_t, const FNode*> objects;
  for (const auto& n : objects_node->kids)
    if (!n.props.empty() && n.props[0].t == 'L') objects[n.props[0].i] = &n;
  std::unordered_map<int64_t, std::vector<int64_t>> oo_children, oo_parent;
  struct OP { int64_t src, dst; std::string prop; };
  std::vector<OP> ops;
  for (const auto& c : conns->kids) {
    if (c.name != "C" || c.props.size() < 3) continue;
    if (c.props[0].s == "OO") {
      oo_children[c.props[2].i].push_back(c.props[1].i);
      oo_parent[c.props[1].i].push_back(c.props[2].i);
    } else if (c.props[0].s == "OP") {
      ops.push_back({c.props[1].i, c.props[2].i, c.props.size() > 3 ? c.props[3].s : ""});
    }
  }
  double unit = 0.01;  // FBX: centimetres
  if (gsn) {
    const auto gs = props70(*gsn);
    unit = pget(gs, "UnitScaleFactor", {1.0})[0] / 100.0;
    if (static_cast<int>(pget(gs, "UpAxis", {1})[0]) != 1) {
      err = "only Y-up FBX files are supported";
      return false;
    }
  }
  auto is = [&](int64_t id, const char* type) {
    auto it = objects.find(id);
    return it != objects.end() && it->second->name == type;
  };
  auto kidsOf = [&](int64_t id) -> const std::vector<int64_t>& {
    static const std::vector<int64_t> none;
    auto it = oo_children.find(id);
    return it == oo_children.end() ? none : it->second;
  };
  auto parentsOf = [&](int64_t id) -> const std::vector<int64_t>& {
    static const std::vector<int64_t> none;
    auto it = oo_parent.find(id);
    return it == oo_parent.end() ? none : it->second;
  };

  // models, globals from the hierarchy, the bind pose
  std::unordered_map<int64_t, M4> glob;
  std::function<M4(int64_t, int)> globalOf = [&](int64_t id, int depth) -> M4 {
    if (auto it = glob.find(id); it != glob.end()) return it->second;
    M4 m = localMatrix(props70(*objects[id]));
    if (depth < 128)
      for (int64_t p : parentsOf(id))
        if (is(p, "Model")) {
          m = globalOf(p, depth + 1) * m;
          break;
        }
    glob[id] = m;
    return m;
  };
  std::unordered_map<int64_t, M4> bind;
  for (const auto& [id, n] : objects)
    if (n->name == "Pose" && valueS(*n, "Type") == "BindPose")
      for (const auto& pn : n->kids)
        if (pn.name == "PoseNode") {
          const FProp* node = value(pn, "Node");
          const FProp* mat = value(pn, "Matrix");
          if (node && mat) bind[node->i] = fromFbx(mat->fa);
        }
  std::unordered_map<int64_t, std::string> limbs;
  for (const auto& [id, n] : objects)
    if (n->name == "Model") {
      const std::string cl = objClass(*n);
      if (cl == "LimbNode" || cl == "Root" || cl == "Limb") limbs[id] = objName(*n);
    }
  std::unordered_map<int64_t, M4> link_global;
  for (const auto& [id, n] : objects)
    if (n->name == "Deformer" && objClass(*n) == "Cluster")
      for (int64_t b : kidsOf(id))
        if (limbs.count(b)) {
          if (const FProp* tl = value(*n, "TransformLink")) link_global[b] = fromFbx(tl->fa);
          break;
        }
  auto bonePos = [&](int64_t id) -> std::array<double, 3> {
    const M4* m = nullptr;
    if (auto it = link_global.find(id); it != link_global.end()) m = &it->second;
    else if (auto jt = bind.find(id); jt != bind.end()) m = &jt->second;
    const M4 g = m ? *m : globalOf(id, 0);
    return {g.m[0][3], g.m[1][3], g.m[2][3]};
  };
  std::unordered_map<int64_t, int> src_to_keep;
  for (const auto& [id, name] : limbs) {
    int t = targetBone(name);
    int64_t j = id;
    for (int guard = 0; t < 0 && guard < 128; ++guard) {
      int64_t par = 0;
      for (int64_t p : parentsOf(j))
        if (limbs.count(p)) {
          par = p;
          break;
        }
      if (!par) break;
      j = par;
      t = targetBone(limbs[j]);
    }
    src_to_keep[id] = t < 0 ? 0 : t;
  }
  std::array<double, 3> keep_pos[kNB];
  bool have[kNB] = {};
  for (const auto& [id, name] : limbs)
    if (int k = keepIndex(baseName(name)); k >= 0) keep_pos[k] = bonePos(id), have[k] = true;
  for (const char* side : {"Left", "Right"}) {
    const std::string sd = side;
    std::array<double, 3> acc{0, 0, 0};
    int nk = 0;
    for (const auto& [id, name] : limbs) {
      const std::string b = baseName(name);
      if (b == sd + "HandIndex1" || b == sd + "HandMiddle1" || b == sd + "HandRing1" || b == sd + "HandPinky1") {
        const auto p = bonePos(id);
        for (int a = 0; a < 3; ++a) acc[a] += p[a];
        ++nk;
      }
      if (b == sd + "HandThumb1") keep_pos[keepIndex(sd + "Thumb")] = bonePos(id), have[keepIndex(sd + "Thumb")] = true;
    }
    if (nk) {
      for (auto& a : acc) a /= nk;
      keep_pos[keepIndex(sd + "Fingers")] = acc, have[keepIndex(sd + "Fingers")] = true;
    }
  }
  for (int b = 0; b < kNB; ++b)
    if (!have[b] && kParent[b] >= 0 && have[kParent[b]] && (b == 10 || b == 11 || b == 16 || b == 17)) keep_pos[b] = keep_pos[kParent[b]], have[b] = true;
  for (int b = 0; b < kNB; ++b)
    if (!have[b]) {
      err = std::string("not a Mixamo-style skeleton (no ") + kKeep[b] + ")";
      return false;
    }

  // embedded images by file name; materials -> their diffuse image
  std::unordered_map<std::string, std::pair<const unsigned char*, size_t>> embedded;
  for (const auto& [id, n] : objects)
    if (n->name == "Video") {
      const FProp* c = value(*n, "Content");
      std::string fn = valueS(*n, "Filename");
      if (fn.empty()) fn = valueS(*n, "RelativeFilename");
      if (c && c->t == 'R' && c->raw_n > 0 && !fn.empty()) embedded.emplace(fileBase(fn), std::make_pair(c->raw, c->raw_n));
    }
  std::vector<std::vector<unsigned char>> neighbour_files;  // (textures next to the FBX, kept alive here)
  auto diffuseOf = [&](int64_t mat) -> std::pair<const unsigned char*, size_t> {
    for (const auto& o : ops) {
      if (o.dst != mat || !(o.prop == "DiffuseColor" || o.prop == "Maya|baseColor" || o.prop == "3dsMax|Parameters|base_color_map")) continue;
      if (!is(o.src, "Texture")) continue;
      const FNode& tex = *objects[o.src];
      for (int64_t v : kidsOf(o.src))
        if (is(v, "Video")) {
          const FProp* c = value(*objects[v], "Content");
          if (c && c->t == 'R' && c->raw_n > 0) return {c->raw, c->raw_n};
        }
      std::string fn = valueS(tex, "FileName");
      if (fn.empty()) fn = valueS(tex, "RelativeFilename");
      if (fn.empty()) continue;
      if (auto it = embedded.find(fileBase(fn)); it != embedded.end()) return it->second;
      if (!dir.empty())
        if (auto f = readFile(dir / std::filesystem::path(fileBase(fn)))) {
          neighbour_files.push_back(std::move(*f));
          return {neighbour_files.back().data(), neighbour_files.back().size()};
        }
    }
    return {nullptr, 0};
  };

  struct Src {
    Image img{};
    bool alpha = false;
  };
  std::vector<Src> sources;
  std::map<const unsigned char*, int> tex_key;
  std::vector<Part> parts;
  for (const auto& [mid, mn] : objects) {
    if (mn->name != "Model" || objClass(*mn) != "Mesh") continue;
    int64_t gid = 0;
    for (int64_t g : kidsOf(mid))
      if (is(g, "Geometry")) {
        gid = g;
        break;
      }
    if (!gid) continue;
    const FNode& geo = *objects[gid];
    std::vector<int64_t> mats;
    for (int64_t m : kidsOf(mid))
      if (is(m, "Material")) mats.push_back(m);
    const FProp* vp = value(geo, "Vertices");
    const FProp* ip = value(geo, "PolygonVertexIndex");
    if (!vp || !ip || vp->fa.size() < 9 || ip->ia.size() < 3) continue;
    const auto& V = vp->fa;
    const auto& pvi = ip->ia;
    const size_t ncp = V.size() / 3, nc = pvi.size();
    std::vector<int64_t> cp(nc), poly_of(nc);
    std::vector<size_t> starts;
    int64_t poly = 0;
    starts.push_back(0);
    for (size_t k = 0; k < nc; ++k) {
      cp[k] = pvi[k] < 0 ? ~pvi[k] : pvi[k];
      if (cp[k] < 0 || static_cast<size_t>(cp[k]) >= ncp) {
        err = "broken mesh indices";
        return false;
      }
      poly_of[k] = poly;
      if (pvi[k] < 0) {
        ++poly;
        if (k + 1 < nc) starts.push_back(k + 1);
      }
    }
    const size_t npoly = static_cast<size_t>(poly);
    if (starts.size() > npoly) starts.resize(npoly);
    // per-corner layer values (normals 3, UVs 2)
    auto layer = [&](const char* le_name, const char* data_name, const char* index_name, int width) {
      std::vector<double> outv;
      const FNode* le = child(geo, le_name);
      if (!le) return outv;
      const std::string mapping = valueS(*le, "MappingInformationType", "ByPolygonVertex");
      const std::string ref = valueS(*le, "ReferenceInformationType", "Direct");
      const FProp* dp = value(*le, data_name);
      if (!dp || dp->fa.empty()) return outv;
      const FProp* xp = value(*le, index_name);
      const bool indexed = ref == "IndexToDirect" && xp && !xp->ia.empty();
      const size_t nv = dp->fa.size() / static_cast<size_t>(width);
      outv.resize(nc * static_cast<size_t>(width));
      for (size_t k = 0; k < nc; ++k) {
        int64_t sel;
        if (mapping == "ByPolygonVertex") sel = indexed ? (k < xp->ia.size() ? xp->ia[k] : 0) : static_cast<int64_t>(k);
        else if (mapping == "ByControlPoint" || mapping == "ByVertice" || mapping == "ByVertex")
          sel = indexed ? (static_cast<size_t>(cp[k]) < xp->ia.size() ? xp->ia[static_cast<size_t>(cp[k])] : 0) : cp[k];
        else if (mapping == "ByPolygon")
          sel = indexed ? (static_cast<size_t>(poly_of[k]) < xp->ia.size() ? xp->ia[static_cast<size_t>(poly_of[k])] : 0) : poly_of[k];
        else sel = 0;
        if (sel < 0 || static_cast<size_t>(sel) >= nv) sel = 0;
        for (int a = 0; a < width; ++a) outv[k * width + a] = dp->fa[static_cast<size_t>(sel) * width + a];
      }
      return outv;
    };
    std::vector<double> N = layer("LayerElementNormal", "Normals", "NormalsIndex", 3);
    std::vector<double> UV = layer("LayerElementUV", "UV", "UVIndex", 2);
    if (UV.empty()) UV.assign(nc * 2, 0.0);
    std::vector<int64_t> pmat(npoly, 0);
    if (const FNode* lm = child(geo, "LayerElementMaterial"); lm && valueS(*lm, "MappingInformationType") == "ByPolygon")
      if (const FProp* mp = value(*lm, "Materials"))
        for (size_t k = 0; k < npoly && k < mp->ia.size(); ++k) pmat[k] = mp->ia[k];
    // skin weights per control point
    std::vector<float> wcp(ncp * kNB, 0.0f);
    bool skinned = false;
    M4 mesh_global = M4::id();
    for (int64_t s : kidsOf(gid)) {
      if (!is(s, "Deformer")) continue;
      for (int64_t cl : kidsOf(s)) {
        if (!is(cl, "Deformer") || objClass(*objects[cl]) != "Cluster") continue;
        const FNode& cn = *objects[cl];
        int64_t bone = 0;
        for (int64_t b : kidsOf(cl))
          if (limbs.count(b)) {
            bone = b;
            break;
          }
        if (!bone) continue;
        const FProp* T = value(cn, "Transform");
        const FProp* TL = value(cn, "TransformLink");
        if (!skinned && T && TL) {
          mesh_global = fromFbx(TL->fa) * fromFbx(T->fa);  // Transform = inverse(TransformLink) x the mesh's bind matrix
          skinned = true;
        }
        const FProp* ix = value(cn, "Indexes");
        const FProp* wt = value(cn, "Weights");
        if (!ix || !wt) continue;
        const int kb = src_to_keep[bone];
        for (size_t q = 0; q < ix->ia.size() && q < wt->fa.size(); ++q)
          if (ix->ia[q] >= 0 && static_cast<size_t>(ix->ia[q]) < ncp) wcp[static_cast<size_t>(ix->ia[q]) * kNB + kb] += static_cast<float>(wt->fa[q]);
      }
    }
    if (!skinned) {  // rigid on its parent bone (or the hips)
      mesh_global = bind.count(mid) ? bind[mid] : globalOf(mid, 0);
      int kb = 0;
      for (int64_t p : parentsOf(mid))
        if (limbs.count(p)) {
          kb = src_to_keep[p];
          break;
        }
      for (size_t c = 0; c < ncp; ++c) wcp[c * kNB + kb] = 1.0f;
    }
    {
      const auto p = props70(*mn);
      if (p.count("GeometricTranslation") || p.count("GeometricRotation") || p.count("GeometricScaling")) {
        M4 g = M4::id();
        const auto gt = pget(p, "GeometricTranslation", {0, 0, 0});
        g.m[0][3] = gt[0], g.m[1][3] = gt[1], g.m[2][3] = gt[2];
        M4 gsc = M4::id();
        const auto gs = pget(p, "GeometricScaling", {1, 1, 1});
        gsc.m[0][0] = gs[0], gsc.m[1][1] = gs[1], gsc.m[2][2] = gs[2];
        mesh_global = mesh_global * g * euler(pget(p, "GeometricRotation", {0, 0, 0})) * gsc;
      }
    }
    M4 inv;
    if (!invert(mesh_global, inv)) inv = M4::id();
    // normals: the inverse-transpose of the linear part
    auto nxf = [&](double x, double y, double z) {
      std::array<double, 3> r{inv.m[0][0] * x + inv.m[1][0] * y + inv.m[2][0] * z, inv.m[0][1] * x + inv.m[1][1] * y + inv.m[2][1] * z,
                              inv.m[0][2] * x + inv.m[1][2] * y + inv.m[2][2] * z};
      const double l = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
      for (auto& c : r) c /= std::max(l, 1e-9);
      return r;
    };
    std::set<int64_t> mat_ids(pmat.begin(), pmat.end());
    for (int64_t mi : mat_ids) {
      Part part;
      struct Key {
        int64_t c;
        int32_t n[3];
        int32_t u[2];
        bool operator==(const Key& o) const { return c == o.c && std::memcmp(n, o.n, sizeof n) == 0 && std::memcmp(u, o.u, sizeof u) == 0; }
      };
      struct KH {
        size_t operator()(const Key& k) const {
          size_t h = std::hash<int64_t>()(k.c);
          for (int32_t v : k.n) h = h * 1000003u ^ static_cast<size_t>(v);
          for (int32_t v : k.u) h = h * 1000003u ^ static_cast<size_t>(v);
          return h;
        }
      };
      std::unordered_map<Key, uint32_t, KH> seen;
      auto vertexOf = [&](size_t k) -> uint32_t {
        double nx = 0, ny = 0, nz = 0;
        if (!N.empty()) {
          const auto n = nxf(N[k * 3], N[k * 3 + 1], N[k * 3 + 2]);
          nx = n[0], ny = n[1], nz = n[2];
        }
        Key key{cp[k],
                {static_cast<int32_t>(std::lround(nx * 1000)), static_cast<int32_t>(std::lround(ny * 1000)), static_cast<int32_t>(std::lround(nz * 1000))},
                {static_cast<int32_t>(std::lround(UV[k * 2] * 8192)), static_cast<int32_t>(std::lround(UV[k * 2 + 1] * 8192))}};
        auto it = seen.find(key);
        if (it != seen.end()) return it->second;
        const uint32_t idx = static_cast<uint32_t>(part.pos.size() / 3);
        const size_t c = static_cast<size_t>(cp[k]);
        const auto p = mesh_global.point(V[c * 3], V[c * 3 + 1], V[c * 3 + 2]);
        part.pos.insert(part.pos.end(), {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])});
        part.nrm.insert(part.nrm.end(), {static_cast<float>(nx), static_cast<float>(ny), static_cast<float>(nz)});
        part.uv.insert(part.uv.end(), {static_cast<float>(UV[k * 2]), static_cast<float>(UV[k * 2 + 1])});
        part.w.insert(part.w.end(), wcp.begin() + static_cast<long>(c * kNB), wcp.begin() + static_cast<long>((c + 1) * kNB));
        seen.emplace(key, idx);
        return idx;
      };
      for (size_t pi = 0; pi < npoly; ++pi) {
        if (pmat[pi] != mi) continue;
        const size_t s0 = starts[pi];
        const size_t e = pi + 1 < npoly ? starts[pi + 1] : nc;
        for (size_t k = 1; k + 1 < e - s0; ++k) {
          part.tri.push_back(vertexOf(s0));
          part.tri.push_back(vertexOf(s0 + k));
          part.tri.push_back(vertexOf(s0 + k + 1));
        }
      }
      if (part.tri.empty()) continue;
      const int64_t mat = mi >= 0 && static_cast<size_t>(mi) < mats.size() ? mats[static_cast<size_t>(mi)] : 0;
      const std::string mname = mat ? lower(objName(*objects[mat])) : "";
      part.kind = mname.find("hair") != std::string::npos || mname.find("lash") != std::string::npos ? 1 : 0;
      if (mat) {
        const auto blob = diffuseOf(mat);
        if (blob.first) {
          auto it = tex_key.find(blob.first);
          if (it == tex_key.end()) {
            const bool png = blob.second > 4 && blob.first[0] == 0x89 && blob.first[1] == 'P';
            const bool jpg = blob.second > 3 && blob.first[0] == 0xFF && blob.first[1] == 0xD8;
            const bool tga = !png && !jpg;
            Image img = LoadImageFromMemory(png ? ".png" : jpg ? ".jpg" : ".tga", blob.first, static_cast<int>(blob.second));
            (void)tga;
            if (img.data) {
              tex_key[blob.first] = static_cast<int>(sources.size());
              sources.push_back({img, false});
            } else {
              tex_key[blob.first] = -1;
            }
            it = tex_key.find(blob.first);
          }
          part.tex = it->second;
        }
      }
      // a cut-out (hair cards): the surface samples transparent texels
      if (part.tex >= 0) {
        Src& s = sources[static_cast<size_t>(part.tex)];
        if (s.img.format == PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 || s.img.format == PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA) {
          Image rgba = ImageCopy(s.img);
          ImageFormat(&rgba, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
          const auto* px = static_cast<const unsigned char*>(rgba.data);
          size_t clear = 0, total = 0;
          auto sample = [&](float u, float v) {
            u -= std::floor(u);
            v -= std::floor(v);
            const int x = std::clamp(static_cast<int>(u * rgba.width), 0, rgba.width - 1);
            const int y = std::clamp(static_cast<int>((1.0f - v) * rgba.height), 0, rgba.height - 1);
            clear += px[(static_cast<size_t>(y) * rgba.width + x) * 4 + 3] < 128;
            ++total;
          };
          for (size_t q = 0; q < part.uv.size() / 2; ++q) sample(part.uv[q * 2], part.uv[q * 2 + 1]);
          for (size_t t = 0; t + 2 < part.tri.size(); t += 3) {
            float u = 0, v = 0;
            for (int c = 0; c < 3; ++c) u += part.uv[part.tri[t + c] * 2], v += part.uv[part.tri[t + c] * 2 + 1];
            sample(u / 3, v / 3);
          }
          UnloadImage(rgba);
          if (total && clear * 100 > total) s.alpha = true;
        }
      }
      parts.push_back(std::move(part));
    }
  }
  if (parts.empty()) {
    for (auto& s : sources) UnloadImage(s.img);
    err = "no meshes";
    return false;
  }
  for (auto& p : parts)
    if (p.tex >= 0 && sources[static_cast<size_t>(p.tex)].alpha) p.kind = 1;

  // metres, the character facing -z (FBX characters face +z: turn half round), feet on y = 0
  float lo = 1e30f, hi = -1e30f;
  for (auto& p : parts) {
    for (size_t q = 0; q < p.pos.size(); q += 3) {
      p.pos[q] = static_cast<float>(-p.pos[q] * unit);
      p.pos[q + 1] = static_cast<float>(p.pos[q + 1] * unit);
      p.pos[q + 2] = static_cast<float>(-p.pos[q + 2] * unit);
      p.nrm[q] = -p.nrm[q];
      p.nrm[q + 2] = -p.nrm[q + 2];
      lo = std::min(lo, p.pos[q + 1]);
      hi = std::max(hi, p.pos[q + 1]);
    }
  }
  float joint[kNB][3];
  for (int b = 0; b < kNB; ++b) {
    joint[b][0] = static_cast<float>(-keep_pos[b][0] * unit);
    joint[b][1] = static_cast<float>(keep_pos[b][1] * unit) - lo;
    joint[b][2] = static_cast<float>(-keep_pos[b][2] * unit);
  }
  float k = 1.0f, h = hi - lo;
  if (!(h > 1.3f && h < 2.2f) && h > 0.01f) k = 1.75f / h;  // (a file in other units than it says)
  for (auto& p : parts)
    for (size_t q = 0; q < p.pos.size(); q += 3) {
      p.pos[q] *= k;
      p.pos[q + 1] = (p.pos[q + 1] - lo) * k;
      p.pos[q + 2] *= k;
    }
  for (auto& j : joint)
    for (float& c : j) c *= k;
  h *= k;

  // textures: scaled down, PNG (RGBA when it is a cut-out)
  std::vector<std::vector<unsigned char>> tex_out;
  for (auto& s : sources) {
    const int m = std::max(s.img.width, s.img.height);
    if (m > max_tex) ImageResize(&s.img, std::max(1, s.img.width * max_tex / m), std::max(1, s.img.height * max_tex / m));
    ImageFormat(&s.img, s.alpha ? PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 : PIXELFORMAT_UNCOMPRESSED_R8G8B8);
    int size = 0;
    unsigned char* png = ExportImageToMemory(s.img, ".png", &size);
    tex_out.emplace_back(png, png + std::max(0, size));
    if (png) MemFree(png);
    UnloadImage(s.img);
  }

  // pieces of at most 65535 vertices (16-bit indices)
  std::vector<Part> pieces;
  for (auto& p : parts) {
    if (p.pos.size() / 3 <= 65535) {
      pieces.push_back(std::move(p));
      continue;
    }
    size_t t = 0;
    while (t < p.tri.size()) {
      Part q;
      q.tex = p.tex, q.kind = p.kind;
      std::unordered_map<uint32_t, uint32_t> remap;
      for (; t + 2 < p.tri.size(); t += 3) {
        int fresh = 0;
        for (int c = 0; c < 3; ++c) fresh += !remap.count(p.tri[t + c]);
        if (remap.size() + static_cast<size_t>(fresh) > 65535) break;
        for (int c = 0; c < 3; ++c) {
          const uint32_t v = p.tri[t + c];
          auto it = remap.find(v);
          if (it == remap.end()) {
            const uint32_t ni = static_cast<uint32_t>(q.pos.size() / 3);
            remap.emplace(v, ni);
            q.pos.insert(q.pos.end(), p.pos.begin() + v * 3, p.pos.begin() + v * 3 + 3);
            q.nrm.insert(q.nrm.end(), p.nrm.begin() + v * 3, p.nrm.begin() + v * 3 + 3);
            q.uv.insert(q.uv.end(), p.uv.begin() + v * 2, p.uv.begin() + v * 2 + 2);
            q.w.insert(q.w.end(), p.w.begin() + static_cast<long>(v) * kNB, p.w.begin() + static_cast<long>(v + 1) * kNB);
            q.tri.push_back(ni);
          } else {
            q.tri.push_back(it->second);
          }
        }
      }
      pieces.push_back(std::move(q));
    }
  }

  // the .rjchr file (render/characters.cpp reads it; tools/import_characters.py writes the same)
  out.clear();
  put(out, "RJCHR1\0\0", 8);
  putv<uint32_t>(out, 1);
  putv<float>(out, h);
  putv<uint32_t>(out, kNB);
  for (int b = 0; b < kNB; ++b) {
    const uint8_t len = static_cast<uint8_t>(std::strlen(kKeep[b]));
    putv(out, len);
    put(out, kKeep[b], len);
    putv<int32_t>(out, kParent[b]);
    for (float c : joint[b]) putv(out, c);
  }
  putv<uint32_t>(out, static_cast<uint32_t>(tex_out.size()));
  for (const auto& t : tex_out) {
    putv<uint32_t>(out, static_cast<uint32_t>(t.size()));
    put(out, t.data(), t.size());
  }
  putv<uint32_t>(out, static_cast<uint32_t>(pieces.size()));
  for (const auto& p : pieces) {
    const uint32_t nv = static_cast<uint32_t>(p.pos.size() / 3);
    putv<int32_t>(out, p.tex);
    putv<uint8_t>(out, static_cast<uint8_t>(p.kind));
    putv<uint32_t>(out, nv);
    putv<uint32_t>(out, static_cast<uint32_t>(p.tri.size()));
    put(out, p.pos.data(), p.pos.size() * 4);
    put(out, p.nrm.data(), p.nrm.size() * 4);
    for (uint32_t v = 0; v < nv; ++v) {
      const float uv[2] = {p.uv[v * 2], 1.0f - p.uv[v * 2 + 1]};  // (images: top row first)
      put(out, uv, 8);
    }
    std::vector<unsigned char> ids(nv * 4);
    std::vector<float> ws(nv * 4);
    for (uint32_t v = 0; v < nv; ++v) {
      std::array<int, kNB> order;
      for (int b = 0; b < kNB; ++b) order[b] = b;
      const float* w = &p.w[static_cast<size_t>(v) * kNB];
      std::partial_sort(order.begin(), order.begin() + 4, order.end(), [&](int a, int b) { return w[a] > w[b]; });
      float sum = 0;
      for (int c = 0; c < 4; ++c) sum += std::max(0.0f, w[order[c]]);
      for (int c = 0; c < 4; ++c) {
        ids[v * 4 + c] = static_cast<unsigned char>(order[c]);
        ws[v * 4 + c] = sum > 1e-9f ? std::max(0.0f, w[order[c]]) / sum : (c == 0 ? 1.0f : 0.0f);
      }
      if (sum <= 1e-9f) ids[v * 4] = 0;
    }
    put(out, ids.data(), ids.size());
    put(out, ws.data(), ws.size() * 4);
    for (uint32_t i : p.tri) putv<uint16_t>(out, static_cast<uint16_t>(i));
  }
  return true;
}

}  // namespace rjc
