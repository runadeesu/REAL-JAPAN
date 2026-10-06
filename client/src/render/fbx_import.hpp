#pragma once
// In-game import of rigged characters: a binary FBX file (a Mixamo-style skeleton, embedded or
// neighbouring diffuse textures) becomes the .rjchr data CharacterSet loads - the same conversion
// as tools/import_characters.py, so players can drop their own FBX files into a folder. Pure CPU
// work (no GL calls): safe on a worker thread.

#include <filesystem>
#include <string>
#include <vector>

namespace rjc {

// fbx: the whole file; dir: its folder (for textures stored next to it, may be empty);
// max_tex: largest texture side kept. Returns false with err set when it cannot be used.
bool convertFbxCharacter(const std::vector<unsigned char>& fbx, const std::filesystem::path& dir, int max_tex, std::vector<unsigned char>& out,
                         std::string& err);

}  // namespace rjc
