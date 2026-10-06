#!/usr/bin/env python3
"""Import rigged characters (binary FBX, e.g. downloaded from Mixamo) into the game's character
format: game/data/characters/<name>.rjchr plus an index, characters.txt.

    python3 tools/import_characters.py <file.fbx | folder> ... [--out game/data/characters] [--tex 1024]

Only numpy and Pillow are needed (the FBX reader is below). What is kept of each model:
  - the skinned meshes in the bind pose, per material: positions, normals, UVs, triangles
  - the skeleton reduced to the bones the game animates (22 body bones, plus one "fingers" and one
    "thumb" bone per hand; the finger segments, eyes and end bones are merged into them), at most
    4 weights a vertex
  - the diffuse texture of each material, scaled down (JPEG, or PNG when it has an alpha cut-out
    such as hair cards); normal, specular and gloss maps are dropped
Units are metres, Y up, the character facing -Z (the game's "north" at yaw 0), its right on +X.

The characters are not part of the repository (their licences are their authors'); the packages
include them when game/data/characters exists at packaging time.
"""
import argparse
import io
import os
import struct
import sys
import zlib

import numpy as np
from PIL import Image

MAGIC = b"RJCHR1\0\0"

# ---------------------------------------------------------------------------------------------
# Binary FBX reader: nodes as (name, [properties], [children]); arrays become numpy arrays.

_ARR = {"f": "<f4", "d": "<f8", "l": "<i8", "i": "<i4", "b": "<u1"}


def read_fbx(path):
    data = open(path, "rb").read()
    if data[:20] != b"Kaydara FBX Binary  ":
        raise ValueError(f"{path}: not a binary FBX (ASCII FBX is not supported; re-export as binary)")
    ver = struct.unpack_from("<I", data, 23)[0]
    wide = ver >= 7500
    hdr = 25 if wide else 13

    def node(off):
        if wide:
            end, nprop, _plen = struct.unpack_from("<QQQ", data, off)
            off += 24
        else:
            end, nprop, _plen = struct.unpack_from("<III", data, off)
            off += 12
        nl = data[off]
        off += 1
        if end == 0:
            return None, off
        name = data[off:off + nl].decode("latin1")
        off += nl
        props = []
        for _ in range(nprop):
            t = chr(data[off])
            off += 1
            if t == "Y":
                props.append(struct.unpack_from("<h", data, off)[0]); off += 2
            elif t == "C":
                props.append(bool(data[off])); off += 1
            elif t == "I":
                props.append(struct.unpack_from("<i", data, off)[0]); off += 4
            elif t == "F":
                props.append(struct.unpack_from("<f", data, off)[0]); off += 4
            elif t == "D":
                props.append(struct.unpack_from("<d", data, off)[0]); off += 8
            elif t == "L":
                props.append(struct.unpack_from("<q", data, off)[0]); off += 8
            elif t in _ARR:
                n, enc, cl = struct.unpack_from("<III", data, off)
                off += 12
                raw = data[off:off + cl]
                off += cl
                if enc == 1:
                    raw = zlib.decompress(raw)
                props.append(np.frombuffer(raw, dtype=_ARR[t], count=n))
            elif t in "SR":
                n = struct.unpack_from("<I", data, off)[0]
                off += 4
                b = data[off:off + n]
                off += n
                props.append(b.decode("utf-8", "replace") if t == "S" else bytes(b))
            else:
                raise ValueError(f"{path}: unknown FBX property type {t!r}")
        kids = []
        while off < end - hdr + 1 and off < end:
            k, off2 = node(off)
            if k is None:
                break
            kids.append(k)
            off = off2
        return (name, props, kids), end

    top = []
    off = 27
    while off + hdr < len(data):
        n, off2 = node(off)
        if n is None:
            break
        top.append(n)
        off = off2
    return ver, top


def child(n, name):
    for k in n[2]:
        if k[0] == name:
            return k
    return None


def children(n, name):
    return [k for k in n[2] if k[0] == name]


def value(n, name, default=None):
    k = child(n, name)
    return k[1][0] if k is not None and k[1] else default


def props70(n):
    out = {}
    p = child(n, "Properties70")
    if p is not None:
        for k in p[2]:
            if k[0] == "P" and k[1]:
                out[k[1][0]] = k[1][4:]
    return out


def obj_name(n):
    return n[1][1].split("\0")[0] if len(n[1]) > 1 and isinstance(n[1][1], str) else ""


# ---------------------------------------------------------------------------------------------
# Transforms (column vectors, 4x4)

def _rx(a):
    c, s = np.cos(a), np.sin(a)
    return np.array([[1, 0, 0, 0], [0, c, -s, 0], [0, s, c, 0], [0, 0, 0, 1]])


def _ry(a):
    c, s = np.cos(a), np.sin(a)
    return np.array([[c, 0, s, 0], [0, 1, 0, 0], [-s, 0, c, 0], [0, 0, 0, 1]])


def _rz(a):
    c, s = np.cos(a), np.sin(a)
    return np.array([[c, -s, 0, 0], [s, c, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]])


def _euler(deg, order=0):
    x, y, z = np.radians(deg)
    m = {"x": _rx(x), "y": _ry(y), "z": _rz(z)}
    seq = ["xyz", "xzy", "yzx", "yxz", "zxy", "zyx"][order]  # FBX eEulerXYZ .. eEulerZYX: first applied first
    r = np.eye(4)
    for a in seq:
        r = m[a] @ r
    return r


def local_matrix(p):
    t = np.eye(4)
    t[:3, 3] = p.get("Lcl Translation", (0, 0, 0))[:3]
    order = int(p.get("RotationOrder", (0,))[0]) if "RotationOrder" in p else 0
    r = _euler(p.get("Lcl Rotation", (0, 0, 0))[:3], order)
    pre = _euler(p.get("PreRotation", (0, 0, 0))[:3])
    post = _euler(p.get("PostRotation", (0, 0, 0))[:3])
    s = np.diag(list(p.get("Lcl Scaling", (1, 1, 1))[:3]) + [1.0])
    return t @ pre @ r @ np.linalg.inv(post) @ s


def mat16(a):
    return np.asarray(a, dtype=np.float64).reshape(4, 4).T  # FBX stores column-major


# ---------------------------------------------------------------------------------------------
# The game's skeleton: Mixamo bone names (any "mixamorigN:" prefix) -> kept bone.

KEEP = ["Hips", "Spine", "Spine1", "Spine2", "Neck", "Head",
        "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand", "LeftFingers", "LeftThumb",
        "RightShoulder", "RightArm", "RightForeArm", "RightHand", "RightFingers", "RightThumb",
        "LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToeBase",
        "RightUpLeg", "RightLeg", "RightFoot", "RightToeBase"]
PARENT = {"Hips": None, "Spine": "Hips", "Spine1": "Spine", "Spine2": "Spine1", "Neck": "Spine2", "Head": "Neck",
          "LeftShoulder": "Spine2", "LeftArm": "LeftShoulder", "LeftForeArm": "LeftArm", "LeftHand": "LeftForeArm",
          "LeftFingers": "LeftHand", "LeftThumb": "LeftHand",
          "RightShoulder": "Spine2", "RightArm": "RightShoulder", "RightForeArm": "RightArm", "RightHand": "RightForeArm",
          "RightFingers": "RightHand", "RightThumb": "RightHand",
          "LeftUpLeg": "Hips", "LeftLeg": "LeftUpLeg", "LeftFoot": "LeftLeg", "LeftToeBase": "LeftFoot",
          "RightUpLeg": "Hips", "RightLeg": "RightUpLeg", "RightFoot": "RightLeg", "RightToeBase": "RightFoot"}


def target_bone(name):
    """Kept bone a source bone's weights go to (None: walk up to the nearest kept ancestor)."""
    n = name.split(":")[-1]
    if n in KEEP:
        return n
    for side in ("Left", "Right"):
        if n.startswith(side + "HandThumb"):
            return side + "Thumb"
        if n.startswith(side + "Hand") and any(f in n for f in ("Index", "Middle", "Ring", "Pinky")):
            return side + "Fingers"
        if n.startswith(side + "Toe"):
            return side + "ToeBase"
        if n == side + "Eye":
            return "Head"
    if n in ("HeadTop_End",):
        return "Head"
    return None


# ---------------------------------------------------------------------------------------------

def layer_values(geo, layer, data_name, index_name, pvi, poly_of_corner, ncp):
    """Per polygon corner values of a LayerElement (normals, UVs)."""
    le = child(geo, layer)
    if le is None:
        return None
    mapping = value(le, "MappingInformationType", "ByPolygonVertex")
    ref = value(le, "ReferenceInformationType", "Direct")
    vals = value(le, data_name)
    if vals is None:
        return None
    width = 3 if layer == "LayerElementNormal" else 2
    vals = np.asarray(vals, dtype=np.float64).reshape(-1, width)
    idx = value(le, index_name)
    cp = np.where(pvi < 0, ~pvi, pvi)
    if mapping in ("ByPolygonVertex",):
        sel = np.asarray(idx) if ref == "IndexToDirect" and idx is not None else np.arange(len(pvi))
    elif mapping in ("ByControlPoint", "ByVertice", "ByVertex"):
        sel = np.asarray(idx)[cp] if ref == "IndexToDirect" and idx is not None else cp
    elif mapping == "ByPolygon":
        sel = np.asarray(idx)[poly_of_corner] if ref == "IndexToDirect" and idx is not None else poly_of_corner
    elif mapping == "AllSame":
        sel = np.zeros(len(pvi), dtype=np.int64)
    else:
        raise ValueError(f"unsupported mapping {mapping}")
    return vals[sel]


def texture_out(im, max_px, alpha):
    """Scaled-down texture file: PNG when the alpha cut-out is used (hair cards), JPEG otherwise."""
    im = im.convert("RGBA" if alpha else "RGB")
    w, h = im.size
    s = min(1.0, max_px / max(w, h))
    if s < 1.0:
        im = im.resize((max(1, int(w * s)), max(1, int(h * s))), Image.LANCZOS)
    out = io.BytesIO()
    if alpha:
        im.save(out, "PNG", optimize=True)
    else:
        im.save(out, "JPEG", quality=88, optimize=True)
    return out.getvalue()


def uses_alpha(im, uv, tri):
    """True when the mesh's surface samples transparent texels (a cut-out such as hair cards). The
    atlas background outside the UV islands is often transparent too, so sample where the mesh is:
    at the vertices and the triangle centres."""
    if im.mode not in ("RGBA", "LA", "P"):
        return False
    a = np.asarray(im.convert("RGBA"))[:, :, 3]
    h, w = a.shape
    pts = np.concatenate([uv, uv[tri].mean(axis=1)])
    x = np.clip((np.mod(pts[:, 0], 1.0) * w).astype(np.int64), 0, w - 1)
    y = np.clip(((1.0 - np.mod(pts[:, 1], 1.0)) * h).astype(np.int64), 0, h - 1)
    return bool((a[y, x] < 128).mean() > 0.01)


def import_fbx(path, max_tex, height=None):
    _ver, top = read_fbx(path)
    objects = {}
    for n in child_top(top, "Objects")[2]:
        if n[1] and isinstance(n[1][0], int):
            objects[n[1][0]] = n
    oo_children, oo_parent, op = {}, {}, []
    for c in child_top(top, "Connections")[2]:
        if c[0] != "C":
            continue
        kind, src, dst = c[1][0], c[1][1], c[1][2]
        if kind == "OO":
            oo_children.setdefault(dst, []).append(src)
            oo_parent.setdefault(src, []).append(dst)
        elif kind == "OP":
            op.append((src, dst, c[1][3] if len(c[1]) > 3 else ""))
    gs = props70(child_top(top, "GlobalSettings"))
    unit = float(gs.get("UnitScaleFactor", (1.0,))[0]) / 100.0  # FBX: unit in cm
    up_axis = int(gs.get("UpAxis", (1,))[0])
    if up_axis != 1:
        raise ValueError(f"{path}: only Y-up FBX files are supported (UpAxis {up_axis})")

    # models, their globals (from the hierarchy) and the bind pose (global matrices) if present
    models = {i: n for i, n in objects.items() if n[0] == "Model"}
    glob = {}

    def global_of(i):
        if i in glob:
            return glob[i]
        m = local_matrix(props70(models[i]))
        par = [p for p in oo_parent.get(i, []) if p in models]
        g = global_of(par[0]) @ m if par else m
        glob[i] = g
        return g

    bind = {}
    for i, n in objects.items():
        if n[0] == "Pose" and value(n, "Type") == "BindPose":
            for pn in children(n, "PoseNode"):
                bind[value(pn, "Node")] = mat16(value(pn, "Matrix"))

    # source bones: every LimbNode; position of each in the bind pose
    limbs = {i: obj_name(n) for i, n in models.items() if len(n[1]) > 2 and n[1][2] in ("LimbNode", "Root", "Limb")}
    link_global = {}
    for i, n in objects.items():
        if n[0] == "Deformer" and len(n[1]) > 2 and n[1][2] == "Cluster":
            bones = [b for b in oo_children.get(i, []) if b in limbs]
            if bones:
                link_global[bones[0]] = mat16(value(n, "TransformLink"))

    def bone_pos(i):
        if i in link_global:
            return link_global[i][:3, 3]
        if i in bind:
            return bind[i][:3, 3]
        return global_of(i)[:3, 3]

    src_to_keep = {}
    for i, name in limbs.items():
        t = target_bone(name)
        j = i
        while t is None:
            par = [p for p in oo_parent.get(j, []) if p in limbs]
            if not par:
                break
            j = par[0]
            t = target_bone(limbs[j])
        src_to_keep[i] = t or "Hips"
    keep_pos = {}
    by_name = {target_bone(n): i for i, n in limbs.items() if target_bone(n) in KEEP and n.split(":")[-1] in KEEP}
    for b in KEEP:
        if b in by_name:
            keep_pos[b] = bone_pos(by_name[b])
    for side in ("Left", "Right"):  # pseudo bones: the knuckles, the thumb root
        knuckles = [bone_pos(i) for i, n in limbs.items()
                    if n.split(":")[-1] in (f"{side}HandIndex1", f"{side}HandMiddle1", f"{side}HandRing1", f"{side}HandPinky1")]
        thumb = [bone_pos(i) for i, n in limbs.items() if n.split(":")[-1] == f"{side}HandThumb1"]
        if knuckles:
            keep_pos[side + "Fingers"] = np.mean(knuckles, axis=0)
        if thumb:
            keep_pos[side + "Thumb"] = thumb[0]
    missing = [b for b in KEEP if b not in keep_pos]
    for b in missing:  # (a hand without fingers: at the hand)
        p = PARENT[b]
        if p in keep_pos and b.endswith(("Fingers", "Thumb")):
            keep_pos[b] = keep_pos[p]
    missing = [b for b in KEEP if b not in keep_pos]
    if missing:
        raise ValueError(f"{path}: not a Mixamo-style skeleton (missing {', '.join(missing)})")
    bone_index = {b: k for k, b in enumerate(KEEP)}

    # materials -> diffuse texture blobs (an image is embedded once even when several textures use it)
    embedded = {}
    for n in objects.values():
        if n[0] == "Video":
            content = value(n, "Content")
            fn = value(n, "Filename") or value(n, "RelativeFilename") or ""
            if isinstance(content, bytes) and len(content) > 0 and fn:
                embedded.setdefault(os.path.basename(fn.replace("\\", "/")), content)
    def diffuse_of(mat_id):
        for src, dst, prop in op:
            if dst == mat_id and prop in ("DiffuseColor", "Maya|baseColor", "3dsMax|Parameters|base_color_map") and src in objects:
                tex = objects[src]
                fn = value(tex, "FileName") or value(tex, "RelativeFilename")
                for v in oo_children.get(src, []):
                    if v in objects and objects[v][0] == "Video":
                        content = value(objects[v], "Content")
                        if isinstance(content, bytes) and len(content) > 0:
                            return content
                if fn and os.path.basename(fn.replace("\\", "/")) in embedded:  # the same image embedded once, for another texture
                    return embedded[os.path.basename(fn.replace("\\", "/"))]
                if fn:  # an external texture next to the FBX
                    for cand in (fn, os.path.join(os.path.dirname(path), os.path.basename(fn.replace("\\", "/")))):
                        if os.path.isfile(cand):
                            return open(cand, "rb").read()
        return None

    sources, tex_key = [], {}
    meshes = []
    for mid, mn in models.items():
        if len(mn[1]) < 3 or mn[1][2] != "Mesh":
            continue
        geos = [g for g in oo_children.get(mid, []) if g in objects and objects[g][0] == "Geometry"]
        if not geos:
            continue
        gid = geos[0]
        geo = objects[gid]
        mats = [m for m in oo_children.get(mid, []) if m in objects and objects[m][0] == "Material"]
        verts = np.asarray(value(geo, "Vertices"), dtype=np.float64).reshape(-1, 3)
        pvi = np.asarray(value(geo, "PolygonVertexIndex"), dtype=np.int64)
        ends = pvi < 0
        poly_of_corner = np.concatenate([[0], np.cumsum(ends)[:-1]]).astype(np.int64)
        cp = np.where(ends, ~pvi, pvi)
        nrm = layer_values(geo, "LayerElementNormal", "Normals", "NormalsIndex", pvi, poly_of_corner, len(verts))
        uv = layer_values(geo, "LayerElementUV", "UV", "UVIndex", pvi, poly_of_corner, len(verts))
        if uv is None:
            uv = np.zeros((len(pvi), 2))
        lm = child(geo, "LayerElementMaterial")
        if lm is not None and value(lm, "MappingInformationType") == "ByPolygon":
            pmat = np.asarray(value(lm, "Materials"), dtype=np.int64)
        else:
            pmat = np.zeros(int(ends.sum()), dtype=np.int64)
        # skin: weights per control point
        w_cp = np.zeros((len(verts), len(KEEP)), dtype=np.float64)
        mesh_global = None
        skins = [s for s in oo_children.get(gid, []) if s in objects and objects[s][0] == "Deformer"]
        for s in skins:
            for cl in oo_children.get(s, []):
                cn = objects.get(cl)
                if cn is None or len(cn[1]) < 3 or cn[1][2] != "Cluster":
                    continue
                bones = [b for b in oo_children.get(cl, []) if b in limbs]
                if not bones:
                    continue
                ix, wt = value(cn, "Indexes"), value(cn, "Weights")
                if mesh_global is None and value(cn, "Transform") is not None and value(cn, "TransformLink") is not None:
                    # Transform is stored as inverse(TransformLink) x (the mesh's global matrix at bind time)
                    mesh_global = mat16(value(cn, "TransformLink")) @ mat16(value(cn, "Transform"))
                if ix is None or wt is None or len(ix) == 0:
                    continue
                np.add.at(w_cp[:, bone_index[src_to_keep[bones[0]]]], np.asarray(ix, dtype=np.int64), np.asarray(wt, dtype=np.float64))
        if mesh_global is None:  # not skinned: rigid on its parent bone (or the hips)
            mesh_global = bind.get(mid, global_of(mid))
            par = [p for p in oo_parent.get(mid, []) if p in limbs]
            w_cp[:, bone_index[src_to_keep[par[0]]] if par else 0] = 1.0
        geom = np.eye(4)
        p = props70(mn)
        if "GeometricTranslation" in p or "GeometricRotation" in p or "GeometricScaling" in p:
            geom[:3, 3] = p.get("GeometricTranslation", (0, 0, 0))[:3]
            geom = geom @ _euler(p.get("GeometricRotation", (0, 0, 0))[:3]) @ np.diag(list(p.get("GeometricScaling", (1, 1, 1))[:3]) + [1.0])
        xf = mesh_global @ geom
        vw = (np.c_[verts, np.ones(len(verts))] @ xf.T)[:, :3]
        nxf = np.linalg.inv(xf[:3, :3]).T
        # corners -> unique vertices per material (position, normal, uv), triangles by fans
        corner_n = nrm @ nxf.T if nrm is not None else np.zeros((len(pvi), 3))
        ln = np.linalg.norm(corner_n, axis=1, keepdims=True)
        corner_n = corner_n / np.maximum(ln, 1e-9)
        starts = np.concatenate([[0], np.nonzero(ends)[0][:-1] + 1])
        counts = np.nonzero(ends)[0] + 1 - starts
        for mi in np.unique(pmat):
            sel_polys = np.nonzero(pmat == mi)[0]
            tris = []
            for k in range(1, counts.max() - 1):  # fan triangulation, vectorised by fan step
                ok = sel_polys[counts[sel_polys] > k + 1]
                if len(ok) == 0:
                    break
                s0 = starts[ok]
                tris.append(np.stack([s0, s0 + k, s0 + k + 1], axis=1))
            if not tris:
                continue
            tris = np.concatenate(tris)
            used = np.unique(tris)
            key = np.c_[cp[used], np.round(corner_n[used] * 1000).astype(np.int64), np.round(uv[used] * 8192).astype(np.int64)]
            _, first, inv = np.unique(key, axis=0, return_index=True, return_inverse=True)
            inv = inv.reshape(-1)
            corner_to_vert = np.full(len(pvi), -1, dtype=np.int64)
            corner_to_vert[used] = inv
            vsrc = used[first]
            mat_id = mats[mi] if mi < len(mats) else None
            blob = diffuse_of(mat_id) if mat_id is not None else None
            mname = obj_name(objects[mat_id]).lower() if mat_id is not None else ""
            ti = -1
            if blob is not None:
                h = hash(blob[:4096]) ^ len(blob)
                if h not in tex_key:
                    im = Image.open(io.BytesIO(blob))
                    im.load()
                    tex_key[h] = len(sources)
                    sources.append(dict(im=im, alpha=False))
                ti = tex_key[h]
            m = dict(pos=vw[cp[vsrc]], nrm=corner_n[vsrc], uv=uv[vsrc], w=w_cp[cp[vsrc]],
                     tri=corner_to_vert[tris], tex=ti, kind=1 if ("hair" in mname or "lash" in mname) else 0)
            if ti >= 0 and uses_alpha(sources[ti]["im"], m["uv"], m["tri"]):
                sources[ti]["alpha"] = True
                m["kind"] = 1
            meshes.append(m)
    if not meshes:
        raise ValueError(f"{path}: no meshes")
    textures = [texture_out(t["im"], max_tex, t["alpha"]) for t in sources]
    for m in meshes:  # (cut-out textures: every mesh drawing with one is alpha-tested)
        if m["tex"] >= 0 and sources[m["tex"]]["alpha"]:
            m["kind"] = 1

    # units, axes: metres, the character facing -Z (FBX characters face +Z: turn 180 degrees about Y)
    flip = np.array([-1.0, 1.0, -1.0])
    for m in meshes:
        m["pos"] = m["pos"] * unit * flip
        m["nrm"] = m["nrm"] * flip
    for b in keep_pos:
        keep_pos[b] = np.asarray(keep_pos[b]) * unit * flip
    lo = min(m["pos"][:, 1].min() for m in meshes)
    hi = max(m["pos"][:, 1].max() for m in meshes)
    for m in meshes:  # feet on y = 0
        m["pos"][:, 1] -= lo
    for b in keep_pos:
        keep_pos[b][1] -= lo
    # Some files are in other units than they say (a 3.8 m tall person): bring a height that is not
    # a person's to 1.75 m, or to the height asked for
    k = 1.0
    if height:
        k = height / (hi - lo)
    elif not 1.3 < hi - lo < 2.2:
        k = 1.75 / (hi - lo)
    if k != 1.0:
        for m in meshes:
            m["pos"] = m["pos"] * k
        for b in keep_pos:
            keep_pos[b] = keep_pos[b] * k
        hi, lo = lo + (hi - lo) * k, lo
    return dict(bones=[(b, KEEP.index(PARENT[b]) if PARENT[b] else -1, keep_pos[b]) for b in KEEP],
                textures=textures, meshes=meshes, height=hi - lo)


def child_top(top, name):
    for n in top:
        if n[0] == name:
            return n
    raise ValueError(f"FBX without {name}")


def top4(w):
    """At most 4 influences per vertex, normalised: ids (u8), weights (f32)."""
    idx = np.argsort(-w, axis=1)[:, :4]
    ww = np.take_along_axis(w, idx, axis=1)
    s = ww.sum(axis=1, keepdims=True)
    none = s[:, 0] <= 1e-9
    ww = np.where(s > 1e-9, ww / np.maximum(s, 1e-9), 0.0)
    ww[none, 0] = 1.0
    idx[none, 0] = 0
    return idx.astype(np.uint8), ww.astype(np.float32)


def split16(m):
    """Split a mesh into pieces of at most 65535 vertices (16-bit indices)."""
    tri = m["tri"]
    if len(m["pos"]) <= 65535:
        return [m]
    out = []
    start = 0
    while start < len(tri):
        seen = {}
        k = start
        while k < len(tri):
            new = [v for v in tri[k] if v not in seen]
            if len(seen) + len(new) > 65535:
                break
            for v in new:
                seen[v] = len(seen)
            k += 1
        ids = np.array(list(seen.keys()), dtype=np.int64)
        remap = np.vectorize(seen.get)(tri[start:k])
        out.append(dict({key: m[key][ids] for key in ("pos", "nrm", "uv", "w")}, tri=remap, tex=m["tex"], kind=m["kind"]))
        start = k
    return out


def write_rjchr(path, ch):
    with open(path, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<If", 1, ch["height"]))
        f.write(struct.pack("<I", len(ch["bones"])))
        for name, parent, p in ch["bones"]:
            nb = name.encode()
            f.write(struct.pack("<B", len(nb)) + nb + struct.pack("<i3f", parent, *[float(x) for x in p]))
        f.write(struct.pack("<I", len(ch["textures"])))
        for t in ch["textures"]:
            f.write(struct.pack("<I", len(t)) + t)
        pieces = [p for m in ch["meshes"] for p in split16(m)]
        f.write(struct.pack("<I", len(pieces)))
        for m in pieces:
            ids, ws = top4(m["w"])
            f.write(struct.pack("<iBII", m["tex"], m["kind"], len(m["pos"]), m["tri"].size))
            f.write(m["pos"].astype("<f4").tobytes())
            f.write(m["nrm"].astype("<f4").tobytes())
            uv = m["uv"].astype(np.float64).copy()
            uv[:, 1] = 1.0 - uv[:, 1]  # FBX v up; images (raylib) top row first
            f.write(uv.astype("<f4").tobytes())
            f.write(ids.tobytes())
            f.write(ws.astype("<f4").tobytes())
            f.write(m["tri"].astype("<u2").tobytes())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+", help="FBX files or folders of them")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "game", "data", "characters"))
    ap.add_argument("--tex", type=int, default=1024, help="largest texture side in pixels (default 1024)")
    a = ap.parse_args()
    files = []
    for p in a.inputs:
        if os.path.isdir(p):
            files += [os.path.join(p, f) for f in sorted(os.listdir(p)) if f.lower().endswith(".fbx")]
        else:
            files.append(p)
    os.makedirs(a.out, exist_ok=True)
    index = []
    ok = 0
    for path in files:
        stem = os.path.splitext(os.path.basename(path))[0]
        name = stem.replace("_nonPBR", "").replace(" ", "_")
        try:
            ch = import_fbx(path, a.tex)
        except Exception as e:  # report and go on with the rest
            print(f"  skipped {os.path.basename(path)}: {e}")
            continue
        out = os.path.join(a.out, name + ".rjchr")
        write_rjchr(out, ch)
        tris = sum(m["tri"].shape[0] for m in ch["meshes"])
        print(f"  {name}: {ch['height']:.2f} m, {tris} triangles, {len(ch['meshes'])} meshes, "
              f"{len(ch['textures'])} textures, {os.path.getsize(out) / 1048576:.1f} MB")
        index.append((name, ch["height"], tris))
        ok += 1
    with open(os.path.join(a.out, "characters.txt"), "w", encoding="utf-8") as f:
        f.write("# name height_m triangles  (tools/import_characters.py)\n")
        for name, h, t in sorted(index):
            f.write(f"{name} {h:.3f} {t}\n")
    print(f"{ok} of {len(files)} characters -> {os.path.normpath(a.out)}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
