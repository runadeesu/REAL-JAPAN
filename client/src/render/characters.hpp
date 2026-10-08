#pragma once
// Rigged characters: game/data/characters/*.rjchr, made by tools/import_characters.py from FBX
// files (e.g. downloaded from Mixamo; they are not part of the repository). Each is a set of
// GPU-skinned meshes with its diffuse textures and a 26-bone skeleton in the bind (T) pose. The
// poses (standing, walking, running, sitting, holding a strap, talking, playing the guitar or the
// drums, swimming, waving, holding an umbrella) are made here procedurally - no animation clips.
//
// Skinning: v' = A_i (v - T_i) + P_i for bone i, where T_i is its joint in the bind pose, A_i the
// rotation it has accumulated down the chain and P_i where its joint ends up. The three rows of
// that affine map go to the lit / depth shaders' "bones" uniform (vec4 x 3 per bone).

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <future>
#include <vector>

#include "platform/jobs.hpp"
#include "raylib.h"

namespace rjc {

enum CharBone : int {
  kHips = 0, kSpine, kSpine1, kSpine2, kNeck, kHead,
  kLShoulder, kLArm, kLForeArm, kLHand, kLFingers, kLThumb,
  kRShoulder, kRArm, kRForeArm, kRHand, kRFingers, kRThumb,
  kLUpLeg, kLLeg, kLFoot, kLToe, kRUpLeg, kRLeg, kRFoot, kRToe,
  kCharBones
};

enum class CharPose : int { Stand = 0, Walk, Run, Sit, Strap, Talk, Guitar, Drum, Swim, Wave, Count };

struct CharAnim {
  CharPose pose = CharPose::Stand;
  float phase = 0;       // walk / run / swim cycle (radians)
  float time = 0;        // seconds (breathing, gestures, strumming)
  float seed = 0;        // per person variation 0..1
  float look = 0;        // head turn (radians, + to the left)
  bool umbrella = false; // the right hand holds an umbrella up
};

struct Character {
  struct Part {
    Mesh mesh{};
    int tex = -1;
    int kind = 0;  // 0 skin / clothes, 1 alpha-tested (hair cards, lashes)
  };
  std::string name;
  float height = 1.75f;
  Vector3 joint[kCharBones]{};  // bind-pose joints (model space, metres; y up, facing -z)
  int parent[kCharBones]{};
  std::vector<Part> parts;
  std::vector<Texture2D> textures;
  int triangles = 0;
};

struct CharSkin {
  float rows[kCharBones * 12];  // the "bones" uniform: 3 vec4 per bone
  Vector3 joint[kCharBones];    // posed joints (model space)
};

class CharacterSet {
 public:
  ~CharacterSet() { unload(); }
  CharacterSet() = default;
  CharacterSet(const CharacterSet&) = delete;
  CharacterSet& operator=(const CharacterSet&) = delete;
  // Loads the characters listed in dir/characters.txt (at most max_count, 0 = all). Returns how many.
  int load(const std::filesystem::path& dir, int max_count = 0);
  void unload();
  int count() const { return static_cast<int>(chars_.size()); }
  const Character& at(int i) const { return chars_[static_cast<size_t>(i)]; }
  int find(const std::string& name) const;  // -1: none
  // The skinning rows (and posed joints) for a pose.
  static void pose(const Character& c, const CharAnim& a, CharSkin& out);

  // The player's own characters: .fbx files (binary, a Mixamo-style skeleton) and .rjchr files in
  // `dirs`. Ready ones load at once; the FBX files not converted yet are converted on a worker
  // thread into cache_dir (kept until the FBX changes) and come in through pollImported (call it
  // every frame on the GL thread; it returns how many characters were added).
  void startUserImport(const std::vector<std::filesystem::path>& dirs, const std::filesystem::path& cache_dir, int max_tex = 1024,
                       int max_count = 0);
  int pollImported();
  bool importBusy() const { return import_left_.load() > 0; }
  int importLeft() const { return import_left_.load(); }
  const std::vector<std::string>& importErrors() const { return import_errors_; }  // "file: reason"

 private:
  bool loadBytes(const std::vector<unsigned char>& data, const std::string& name, Character& c);
  void addLoaded(Character&& c);
  std::vector<Character> chars_;
  int max_count_ = 0;
  std::future<void> import_done_;  // the conversion task (on pool_)
  std::atomic<bool> stop_{false};
  std::atomic<int> import_left_{0};
  std::mutex ready_mx_;
  std::vector<std::pair<std::string, std::vector<unsigned char>>> ready_;  // converted, waiting for the GL thread
  std::vector<std::string> pending_errors_;
  std::vector<std::string> import_errors_;
  JobPool pool_{1};  // (the FBX conversion thread; last member: destroyed first)
};

}  // namespace rjc
