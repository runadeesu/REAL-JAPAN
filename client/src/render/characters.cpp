#include "render/characters.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

#include "platform/paths.hpp"
#include "raymath.h"
#include "render/fbx_import.hpp"

namespace rjc {

namespace {

const char* const kBoneNames[kCharBones] = {
    "Hips", "Spine", "Spine1", "Spine2", "Neck", "Head",
    "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand", "LeftFingers", "LeftThumb",
    "RightShoulder", "RightArm", "RightForeArm", "RightHand", "RightFingers", "RightThumb",
    "LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToeBase", "RightUpLeg", "RightLeg", "RightFoot", "RightToeBase"};

constexpr float kMatCharacter = 39.0f;  // lit shader material ids: textured skin / clothes
constexpr float kMatCharHair = 42.0f;   // hair cards, lashes

struct Reader {
  const unsigned char* p;
  const unsigned char* end;
  bool ok = true;
  template <class T>
  T get() {
    T v{};
    if (p + sizeof(T) > end) {
      ok = false;
      return v;
    }
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return v;
  }
  const unsigned char* take(size_t n) {
    if (p + n > end) {
      ok = false;
      return nullptr;
    }
    const unsigned char* r = p;
    p += n;
    return r;
  }
};

template <class T>
T* copyOut(Reader& r, size_t count) {
  const unsigned char* src = r.take(count * sizeof(T));
  if (!src) return nullptr;
  T* dst = static_cast<T*>(MemAlloc(static_cast<unsigned int>(count * sizeof(T))));
  std::memcpy(dst, src, count * sizeof(T));
  return dst;
}

Quaternion rx(float a) { return QuaternionFromAxisAngle(Vector3{1, 0, 0}, a); }
Quaternion ry(float a) { return QuaternionFromAxisAngle(Vector3{0, 1, 0}, a); }
Quaternion rz(float a) { return QuaternionFromAxisAngle(Vector3{0, 0, 1}, a); }
// b first, then a
Quaternion mul(Quaternion a, Quaternion b) { return QuaternionMultiply(a, b); }

}  // namespace

int CharacterSet::load(const std::filesystem::path& dir, int max_count) {
  unload();
  max_count_ = max_count;
  const auto txt = readText(dir / "characters.txt");
  if (!txt) return 0;
  std::istringstream in(*txt);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ls(line);
    std::string name;
    if (!(ls >> name)) continue;
    Character c;
    const auto data = readFile(dir / (name + ".rjchr"));
    if (data && loadBytes(*data, name, c)) chars_.push_back(std::move(c));
    else TraceLog(LOG_WARNING, "RJ: character %s could not be loaded", name.c_str());
    if (max_count > 0 && static_cast<int>(chars_.size()) >= max_count) break;
  }
  TraceLog(LOG_INFO, "RJ: %d characters loaded", count());
  return count();
}

bool CharacterSet::loadBytes(const std::vector<unsigned char>& bytes, const std::string& name, Character& c) {
  const std::vector<unsigned char>* data = &bytes;
  if (data->size() < 16) return false;
  Reader r{data->data(), data->data() + data->size()};
  const unsigned char* magic = r.take(8);
  if (!magic || std::memcmp(magic, "RJCHR1\0\0", 8) != 0) return false;
  if (r.get<uint32_t>() != 1) return false;
  c.name = name;
  c.height = r.get<float>();
  const uint32_t nb = r.get<uint32_t>();
  if (nb != kCharBones) return false;
  for (uint32_t i = 0; i < nb && r.ok; ++i) {
    const uint8_t len = r.get<uint8_t>();
    const unsigned char* s = r.take(len);
    if (!s || std::string(reinterpret_cast<const char*>(s), len) != kBoneNames[i]) return false;
    c.parent[i] = r.get<int32_t>();
    c.joint[i].x = r.get<float>();
    c.joint[i].y = r.get<float>();
    c.joint[i].z = r.get<float>();
    if (c.parent[i] >= static_cast<int>(i)) return false;  // parents come first
  }
  const uint32_t nt = r.get<uint32_t>();
  for (uint32_t i = 0; i < nt && r.ok; ++i) {
    const uint32_t len = r.get<uint32_t>();
    const unsigned char* blob = r.take(len);
    if (!blob) break;
    const bool png = len > 4 && blob[0] == 0x89 && blob[1] == 'P';
    Image img = LoadImageFromMemory(png ? ".png" : ".jpg", blob, static_cast<int>(len));
    Texture2D t{};
    if (img.data) {
      t = LoadTextureFromImage(img);
      UnloadImage(img);
      GenTextureMipmaps(&t);
      SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR);
      SetTextureWrap(t, TEXTURE_WRAP_REPEAT);
    }
    c.textures.push_back(t);
  }
  const uint32_t nm = r.get<uint32_t>();
  for (uint32_t i = 0; i < nm && r.ok; ++i) {
    Character::Part part;
    part.tex = r.get<int32_t>();
    part.kind = r.get<uint8_t>();
    const uint32_t nv = r.get<uint32_t>(), ni = r.get<uint32_t>();
    if (!r.ok || nv == 0 || nv > 65535 || ni % 3 != 0) return false;
    Mesh m{};
    m.vertexCount = static_cast<int>(nv);
    m.triangleCount = static_cast<int>(ni / 3);
    m.vertices = copyOut<float>(r, nv * 3);
    m.normals = copyOut<float>(r, nv * 3);
    m.texcoords = copyOut<float>(r, nv * 2);
    m.boneIds = copyOut<unsigned char>(r, nv * 4);
    m.boneWeights = copyOut<float>(r, nv * 4);
    m.indices = copyOut<unsigned short>(r, ni);
    if (!r.ok || !m.vertices || !m.normals || !m.texcoords || !m.boneIds || !m.boneWeights || !m.indices) {
      UnloadMesh(m);
      return false;
    }
    for (uint32_t k = 0; k < ni; ++k)
      if (m.indices[k] >= nv) {
        UnloadMesh(m);
        return false;
      }
    m.texcoords2 = static_cast<float*>(MemAlloc(nv * 2 * sizeof(float)));
    for (uint32_t k = 0; k < nv; ++k) {
      m.texcoords2[k * 2] = part.kind == 1 ? kMatCharHair : kMatCharacter;
      m.texcoords2[k * 2 + 1] = 0.0f;
    }
    UploadMesh(&m, false);
    // the GPU has it: keep only the indices (raylib draws indexed when they are there)
    for (float** p : {&m.vertices, &m.normals, &m.texcoords, &m.texcoords2, &m.boneWeights}) {
      MemFree(*p);
      *p = nullptr;
    }
    MemFree(m.boneIds);
    m.boneIds = nullptr;
    part.mesh = m;
    c.triangles += m.triangleCount;
    if (part.tex >= static_cast<int>(c.textures.size())) part.tex = -1;
    c.parts.push_back(part);
  }
  if (!r.ok || c.parts.empty()) {
    for (auto& p : c.parts) UnloadMesh(p.mesh);
    for (auto& t : c.textures)
      if (t.id) UnloadTexture(t);
    c.parts.clear();
    c.textures.clear();
    return false;
  }
  return true;
}

void CharacterSet::unload() {
  stop_ = true;
  if (worker_.joinable()) worker_.join();
  stop_ = false;
  import_left_ = 0;
  ready_.clear();
  for (auto& c : chars_) {
    for (auto& p : c.parts) UnloadMesh(p.mesh);
    for (auto& t : c.textures)
      if (t.id) UnloadTexture(t);
  }
  chars_.clear();
}

int CharacterSet::find(const std::string& name) const {
  for (size_t i = 0; i < chars_.size(); ++i)
    if (chars_[i].name == name) return static_cast<int>(i);
  return -1;
}

void CharacterSet::addLoaded(Character&& c) {
  for (const auto& o : chars_)
    if (o.name == c.name) c.name += "+";  // (the same name twice: keep both)
  chars_.push_back(std::move(c));
}

void CharacterSet::startUserImport(const std::vector<std::filesystem::path>& dirs, const std::filesystem::path& cache_dir, int max_tex,
                                   int max_count) {
  namespace fs = std::filesystem;
  if (worker_.joinable()) return;
  if (max_count > 0) max_count_ = max_count;
  struct Job {
    fs::path src, cache, stamp;
    std::string name, sig;
  };
  std::vector<Job> jobs;
  std::error_code ec;
  fs::create_directories(cache_dir, ec);
  for (const auto& d : dirs) {
    if (!fs::is_directory(d, ec)) continue;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(d, ec))
      if (e.is_regular_file(ec)) files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
      std::string ext = f.extension().string();
      for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      std::string name = f.stem().string();
      if (const auto k = name.find("_nonPBR"); k != std::string::npos) name.erase(k, 7);
      for (auto& ch : name)
        if (ch == ' ') ch = '_';
      if (ext == ".rjchr") {
        Character c;
        const auto data = readFile(f);
        if (data && loadBytes(*data, name, c)) addLoaded(std::move(c));
        else import_errors_.push_back(f.filename().string() + ": not a character file");
      } else if (ext == ".fbx") {
        const auto size = fs::file_size(f, ec);
        const auto when = fs::last_write_time(f, ec).time_since_epoch().count();
        const std::string sig = std::to_string(size) + " " + std::to_string(static_cast<long long>(when)) + " " + std::to_string(max_tex) + " v1";
        Job j{f, cache_dir / (name + ".rjchr"), cache_dir / (name + ".stamp"), name, sig};
        const auto st = readText(j.stamp);
        if (st && *st == sig) {  // converted before and unchanged since
          Character c;
          const auto data = readFile(j.cache);
          if (data && loadBytes(*data, name, c)) {
            addLoaded(std::move(c));
            continue;
          }
        }
        jobs.push_back(j);
      }
    }
  }
  if (jobs.empty()) return;
  import_left_ = static_cast<int>(jobs.size());
  TraceLog(LOG_INFO, "RJ: converting %d character FBX file(s) in the background", static_cast<int>(jobs.size()));
  worker_ = std::thread([this, jobs, max_tex]() {
    for (const auto& j : jobs) {
      if (stop_) break;
      std::vector<unsigned char> out;
      std::string err;
      const auto fbx = readFile(j.src);
      const bool ok = fbx && convertFbxCharacter(*fbx, j.src.parent_path(), max_tex, out, err);
      std::lock_guard<std::mutex> lk(ready_mx_);
      if (ok) {
        writeFileAtomic(j.cache, std::string(out.begin(), out.end()));
        writeFileAtomic(j.stamp, j.sig);
        ready_.push_back({j.name, std::move(out)});
      } else {
        pending_errors_.push_back(j.src.filename().string() + ": " + (fbx ? err : std::string("cannot be read")));
        TraceLog(LOG_WARNING, "RJ: character %s not imported: %s", j.src.filename().string().c_str(), fbx ? err.c_str() : "cannot be read");
      }
      --import_left_;
    }
  });
}

int CharacterSet::pollImported() {
  std::vector<std::pair<std::string, std::vector<unsigned char>>> got;
  {
    std::lock_guard<std::mutex> lk(ready_mx_);
    got.swap(ready_);
    for (auto& e : pending_errors_) import_errors_.push_back(std::move(e));
    pending_errors_.clear();
  }
  int added = 0;
  for (auto& [name, data] : got) {
    if (max_count_ > 0 && count() >= max_count_) break;
    Character c;
    if (loadBytes(data, name, c)) {
      addLoaded(std::move(c));
      ++added;
    }
  }
  if (!importBusy() && worker_.joinable()) worker_.join();
  return added;
}

// ------------------------------------------------------------------------------------------------
// Poses. Every bone's rotation is given in the character's bind space (x right, y up, facing -z,
// the arms out level in the T pose) about its own joint, and applied down the chain. Signs:
//   legs, spine:    rx(+a) swings a leg forward, rx(-a) leans the spine forward, bends a knee
//   arms:           rz(+a) lowers the left arm (rz(-a) the right); rx(+a) then swings it forward
//   elbows:         ry(-a) bends the left forearm forward (ry(+a) the right)
//   fingers:        rz(+a) curls the left fingers (rz(-a) the right)
//   head:           ry(+a) turns it to the left, rx(-a) nods
void CharacterSet::pose(const Character& c, const CharAnim& a, CharSkin& out) {
  Quaternion D[kCharBones];
  for (auto& q : D) q = QuaternionIdentity();
  Vector3 hip{0, 0, 0};
  const float t = a.time, ph = a.phase, sd = a.seed;
  const float leg = std::max(0.3f, c.joint[kLUpLeg].y - c.joint[kLFoot].y);

  auto arms = [&](float downL, float downR, float swingL, float swingR, float elbowL, float elbowR) {
    D[kLArm] = mul(rx(swingL), rz(downL));
    D[kRArm] = mul(rx(swingR), rz(-downR));
    D[kLForeArm] = ry(-elbowL);
    D[kRForeArm] = ry(elbowR);
  };
  auto fingers = [&](float curl) {
    D[kLFingers] = rz(curl);
    D[kRFingers] = rz(-curl);
    D[kLThumb] = mul(ry(-0.25f * curl), rz(0.2f * curl));
    D[kRThumb] = mul(ry(0.25f * curl), rz(-0.2f * curl));
  };
  auto legs = [&](float swingL, float swingR, float kneeL, float kneeR, float ankleL, float ankleR, float spread) {
    D[kLUpLeg] = mul(rx(swingL), rz(-spread));
    D[kRUpLeg] = mul(rx(swingR), rz(spread));
    D[kLLeg] = rx(-kneeL);
    D[kRLeg] = rx(-kneeR);
    D[kLFoot] = rx(ankleL);
    D[kRFoot] = rx(ankleR);
  };
  auto spine = [&](float lean, float twist) {  // lean > 0 forward, spread over the three
    D[kSpine] = mul(ry(twist * 0.3f), rx(-lean * 0.4f));
    D[kSpine1] = mul(ry(twist * 0.3f), rx(-lean * 0.3f));
    D[kSpine2] = mul(ry(twist * 0.4f), rx(-lean * 0.3f));
  };
  auto head = [&](float turn, float nod) {
    D[kNeck] = mul(ry(turn * 0.4f), rx(-nod * 0.4f));
    D[kHead] = mul(ry(turn * 0.6f), rx(-nod * 0.6f));
  };
  switch (a.pose) {
    case CharPose::Walk:
    case CharPose::Run: {
      const bool run = a.pose == CharPose::Run;
      const float s = std::sin(ph), co = std::cos(ph);
      const float th = run ? 0.62f : 0.36f;
      auto knee = [&](float p) {
        const float k = std::max(0.0f, std::cos(p + (run ? 0.6f : 0.5f)));
        return (run ? 0.3f : 0.1f) + (run ? 1.55f : 0.95f) * std::pow(k, run ? 1.6f : 2.0f);
      };
      auto ankle = [&](float p) { return 0.1f * std::sin(p) - (run ? 0.3f : 0.2f) * std::max(0.0f, -std::sin(p + 0.4f)); };
      legs(th * s, -th * s, knee(ph), knee(ph + PI), ankle(ph), ankle(ph + PI), 0.03f);
      const float as = run ? 0.55f : 0.3f;
      const float e0 = run ? 1.45f : 0.25f;
      arms(1.36f, 1.36f, -as * s, as * s, e0 + 0.2f * std::max(0.0f, -s), e0 + 0.2f * std::max(0.0f, s));
      fingers(run ? 0.9f : 0.4f);
      D[kHips] = mul(ry(-0.08f * s), rz(0.03f * co));
      spine(run ? 0.2f : 0.06f, 0.18f * s);
      head(a.look + 0.06f * s, run ? -0.12f : -0.02f);
      if (run) hip.y = -0.07f + 0.04f * s * s;
      else hip.y = -leg * (1.0f - std::cos(th * std::fabs(s))) * 0.9f - 0.012f;
      if (a.umbrella) {
        D[kRArm] = mul(rx(0.3f), rz(-1.35f));
        D[kRForeArm] = ry(1.45f);
        D[kRFingers] = rz(-1.1f);
      }
      break;
    }
    case CharPose::Sit: {
      legs(1.5f, 1.5f, 1.5f, 1.5f, 0.0f, 0.0f, 0.06f);
      arms(1.2f, 1.2f, 0.35f, 0.35f, 1.0f, 1.0f);
      fingers(0.5f);
      spine(-0.05f, 0.0f);
      head(a.look, 0.05f * std::sin(t * 0.3f + sd * 5.0f));
      hip.y = 0.52f - c.joint[kLUpLeg].y;
      break;
    }
    case CharPose::Strap: {
      legs(0.0f, 0.0f, 0.04f, 0.04f, 0.0f, 0.0f, 0.07f);
      arms(1.3f, -1.2f, 0.05f, 0.25f, 0.2f, 0.3f);
      fingers(0.9f);
      D[kHips] = rz(0.02f * std::sin(t * 0.8f + sd * 4.0f));
      head(a.look, 0.02f);
      break;
    }
    case CharPose::Talk: {
      legs(0.0f, 0.0f, 0.05f, 0.05f, 0.0f, 0.0f, 0.05f);
      arms(1.25f, 1.15f, 0.15f, 0.35f + 0.1f * std::sin(t * 1.3f + sd), 0.5f + 0.25f * std::sin(t * 1.7f), 1.1f + 0.35f * std::sin(t * 2.3f + sd * 6.0f));
      fingers(0.35f);
      spine(0.02f, 0.05f * std::sin(t * 0.9f));
      head(a.look, 0.06f * std::sin(t * 2.7f));
      break;
    }
    case CharPose::Guitar: {
      legs(0.0f, 0.0f, 0.06f, 0.06f, 0.0f, 0.0f, 0.08f);
      // upper arms hanging, forearms level across the body: the left hand out at the neck, the
      // right strumming over the sound hole
      arms(1.05f, 1.3f, 0.3f, 0.12f, 1.2f, 1.45f);
      D[kRForeArm] = mul(ry(1.45f), rx(0.25f * std::sin(t * 9.0f)));
      fingers(0.7f);
      spine(0.08f, 0.0f);
      head(a.look + 0.15f, 0.12f + 0.04f * std::sin(t * 4.0f));
      break;
    }
    case CharPose::Drum: {
      legs(0.0f, 0.0f, 0.1f, 0.1f, 0.0f, 0.0f, 0.1f);
      const float hitL = std::max(0.0f, std::sin(t * 8.0f)), hitR = std::max(0.0f, std::sin(t * 8.0f + PI));
      arms(1.3f, 1.3f, 0.45f, 0.45f, 0.65f + 0.45f * hitL, 0.65f + 0.45f * hitR);  // sticks down onto the drum
      fingers(1.0f);
      spine(0.28f, 0.0f);
      head(a.look, 0.15f + 0.05f * std::sin(t * 8.0f));
      break;
    }
    case CharPose::Swim: {
      // the crawl: face down, the arms turning over, the legs kicking; the origin is the water
      // surface over the hips
      D[kHips] = mul(rx(-1.45f), ry(0.4f * std::sin(ph)));
      D[kLArm] = mul(rx(ph), rz(1.45f));
      D[kRArm] = mul(rx(ph + PI), rz(-1.45f));
      D[kLForeArm] = ry(-0.35f * std::max(0.0f, std::sin(ph)));
      D[kRForeArm] = ry(0.35f * std::max(0.0f, std::sin(ph + PI)));
      const float k = std::sin(ph * 3.0f);
      legs(0.22f * k, -0.22f * k, 0.2f + 0.15f * std::max(0.0f, k), 0.2f + 0.15f * std::max(0.0f, -k), -0.6f, -0.6f, 0.02f);
      fingers(0.15f);
      head(0.0f, -0.35f);
      hip.y = -c.joint[kHips].y - 0.12f;
      break;
    }
    case CharPose::Wave: {
      legs(0.0f, 0.0f, 0.04f, 0.04f, 0.0f, 0.0f, 0.06f);
      arms(1.28f, -1.0f, 0.04f, 0.2f, 0.15f, 0.0f);
      D[kRForeArm] = rz(0.6f + 0.35f * std::sin(t * 7.0f));
      fingers(0.1f);
      head(a.look, -0.05f);
      break;
    }
    case CharPose::Stand:
    case CharPose::Count:
    default: {
      const float down = 1.36f + 0.05f * (sd - 0.5f);
      legs(0.0f, 0.0f, 0.03f, 0.03f, 0.0f, 0.0f, 0.025f + 0.02f * sd);
      arms(down, down, 0.04f, 0.04f, 0.14f + 0.08f * sd, 0.14f + 0.08f * (1.0f - sd));
      fingers(0.35f);
      D[kHips] = rz(0.015f * std::sin(t * 0.35f + sd * 6.0f));
      hip.x = 0.012f * std::sin(t * 0.35f + sd * 6.0f);
      D[kSpine1] = rx(-0.012f * std::sin(t * 1.7f + sd * 3.0f));
      head(a.look, 0.02f * std::sin(t * 0.21f + sd * 9.0f));
      if (a.umbrella) {
        D[kRArm] = mul(rx(0.3f), rz(-1.35f));
        D[kRForeArm] = ry(1.45f);
        D[kRFingers] = rz(-1.1f);
      }
      break;
    }
  }
  D[kLShoulder] = mul(D[kLShoulder], rz(0.06f));
  D[kRShoulder] = mul(D[kRShoulder], rz(-0.06f));

  // forward kinematics, then the rows of v' = A (v - T) + P
  Quaternion A[kCharBones];
  Vector3 P[kCharBones];
  for (int i = 0; i < kCharBones; ++i) {
    const int p = c.parent[i];
    if (p < 0) {
      A[i] = D[i];
      P[i] = Vector3Add(c.joint[i], hip);
    } else {
      A[i] = QuaternionNormalize(mul(A[p], D[i]));
      P[i] = Vector3Add(P[p], Vector3RotateByQuaternion(Vector3Subtract(c.joint[i], c.joint[p]), A[p]));
    }
    const Matrix R = QuaternionToMatrix(A[i]);
    const Vector3 RT = Vector3Transform(c.joint[i], R);
    const Vector3 tr = Vector3Subtract(P[i], RT);
    float* o = out.rows + i * 12;
    o[0] = R.m0, o[1] = R.m4, o[2] = R.m8, o[3] = tr.x;
    o[4] = R.m1, o[5] = R.m5, o[6] = R.m9, o[7] = tr.y;
    o[8] = R.m2, o[9] = R.m6, o[10] = R.m10, o[11] = tr.z;
    out.joint[i] = P[i];
  }
}

}  // namespace rjc
