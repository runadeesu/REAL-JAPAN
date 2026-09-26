#pragma once
// raylib's DrawMesh picks indexed vs. non-indexed drawing from `mesh.indices != NULL` (the element
// buffer itself lives in the VAO). Meshes built in std::vectors release their CPU arrays after
// UploadMesh, so a one-element placeholder is kept in `indices`: the mesh keeps drawing indexed and
// UnloadMesh frees the placeholder with RL_FREE like any other array.

#include "raylib.h"

namespace rjc {

inline void releaseCpuArrays(Mesh& m) {
  const bool indexed = m.indices != nullptr;
  m.vertices = m.normals = m.texcoords = m.texcoords2 = m.tangents = nullptr;
  m.colors = nullptr;
  m.indices = indexed ? static_cast<unsigned short*>(MemAlloc(sizeof(unsigned short))) : nullptr;
}

}  // namespace rjc
