"""RJCELL v1: cooked streaming-cell package read by the game client.

One file per JIS 3rd-level mesh (~1 km). All geometry is in the cell's own
local ENU frame (x=east, y=north, z=up, metres) anchored at the cell centre,
exactly like rj::geo::LocalFrame, so the client can place any cell relative
to its floating origin. Little-endian throughout. The C++ reader lives in
client/src/world/cell_loader.cpp and must stay in sync with this file.

Layout (v2)
  header (struct HEADER)
  strings blob (UTF-8)
  buildings  n_buildings * BUILDING
  footprints n_footprint_pts * (f32 x, f32 y)
  chunks     n_chunks * [u32 nv, u32 ni, i32 page, f32 pos[3nv], f32 nrm[3nv], u8 rgba[4nv],
                         f32 uv[2nv], u16 idx[ni], pad4]      page = texture atlas index or -1
  terrain    f32 heights[nx*ny] (row-major, south->north rows, west->east columns), local-ENU z
  ground texture (PNG bytes, covers the cell bounds, north up)
  atlases    u32 n_pages, then n_pages * [u32 len, JPEG bytes]   (PLATEAU facade/roof photos)
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

import numpy as np

MAGIC = b"RJCELL02"
HEADER = struct.Struct("<8s16s3d4d6I4dI3I")
BUILDING = struct.Struct("<4I2Hf2h4BIf6f2I3I")

GEOM_VERIFIED_EXTERIOR = 0
GEOM_UNVERIFIED = 1
INTERIOR_VERIFIED, INTERIOR_PARTIAL, INTERIOR_UNKNOWN, INTERIOR_FICTIONAL = 0, 1, 2, 3


@dataclass
class BuildingRec:
    id: str
    name: str
    usage: int
    bclass: int
    measured_height: float
    storeys_above: int
    storeys_below: int
    lod: int
    geometry_status: int
    interior_status: int
    source_index: int
    ground_z: float
    bmin: tuple[float, float, float]
    bmax: tuple[float, float, float]
    footprint: list[tuple[float, float]]
    chunk: int = 0
    first_index: int = 0
    index_count: int = 0
    flags: int = 0


@dataclass
class Chunk:
    page: int = -1
    pos: list[np.ndarray] = field(default_factory=list)
    nrm: list[np.ndarray] = field(default_factory=list)
    col: list[np.ndarray] = field(default_factory=list)
    uv: list[np.ndarray] = field(default_factory=list)
    idx: list[np.ndarray] = field(default_factory=list)
    nv: int = 0
    ni: int = 0


class CellWriter:
    MAX_VERTS = 65535

    def __init__(self, mesh: str, anchor: tuple[float, float, float], bounds: tuple[float, float, float, float]):
        self.mesh = mesh
        self.anchor = anchor
        self.bounds = bounds
        self.buildings: list[BuildingRec] = []
        self.chunks: list[Chunk] = []
        self._open: dict[int, int] = {}  # page -> index of the chunk currently being filled
        self.terrain: np.ndarray | None = None
        self.terrain_grid = (0, 0, 0.0, 0.0, 0.0, 0.0)
        self.ground_png = b""
        self.atlas_jpegs: list[bytes] = []

    def add_geometry(self, pos: np.ndarray, nrm: np.ndarray, col: np.ndarray, idx: np.ndarray,
                     uv: np.ndarray | None = None, page: int = -1) -> tuple[int, int, int]:
        """Adds triangles to the open chunk of `page`. Returns (chunk, first_index, index_count)."""
        nv = len(pos)
        if nv > self.MAX_VERTS:
            raise ValueError("geometry exceeds one chunk")
        ci = self._open.get(page)
        if ci is None or self.chunks[ci].nv + nv > self.MAX_VERTS:
            self.chunks.append(Chunk(page=page))
            ci = self._open[page] = len(self.chunks) - 1
        c = self.chunks[ci]
        first = c.ni
        c.pos.append(pos.astype(np.float32))
        c.nrm.append(nrm.astype(np.float32))
        c.col.append(col.astype(np.uint8))
        c.uv.append((uv if uv is not None else np.zeros((nv, 2))).astype(np.float32))
        c.idx.append((idx + c.nv).astype(np.uint16))
        c.nv += nv
        c.ni += len(idx)
        return ci, first, len(idx)

    def set_terrain(self, heights: np.ndarray, lat0: float, lon0: float, dlat: float, dlon: float) -> None:
        self.terrain = heights.astype(np.float32)
        ny, nx = heights.shape
        self.terrain_grid = (nx, ny, lat0, lon0, dlat, dlon)

    def to_bytes(self) -> bytes:
        strings = bytearray()

        def put(s: str) -> tuple[int, int]:
            b = s.encode("utf-8")
            off = len(strings)
            strings.extend(b)
            return off, len(b)

        brecs = []
        fps: list[tuple[float, float]] = []
        for b in self.buildings:
            io, il = put(b.id)
            no, nl = put(b.name)
            brecs.append(
                BUILDING.pack(
                    io, il, no, nl, b.usage, b.bclass, b.measured_height, b.storeys_above, b.storeys_below,
                    b.lod, b.geometry_status, b.interior_status, b.flags, b.source_index, b.ground_z,
                    *b.bmin, *b.bmax, len(fps), len(b.footprint), b.chunk, b.first_index, b.index_count,
                )
            )
            fps.extend(b.footprint)
        chunks = [c for c in self.chunks if c.nv > 0]
        nx, ny, lat0, lon0, dlat, dlon = self.terrain_grid
        out = bytearray(
            HEADER.pack(
                MAGIC, self.mesh.encode().ljust(16, b"\0"), *self.anchor, *self.bounds,
                len(self.buildings), len(chunks), len(fps), len(strings), nx, ny,
                lat0, lon0, dlat, dlon, len(self.ground_png), 0, 0, 0,
            )
        )
        out += strings
        for r in brecs:
            out += r
        if fps:
            out += np.asarray(fps, dtype=np.float32).tobytes()
        for c in chunks:
            out += struct.pack("<2Ii", c.nv, c.ni, c.page)
            out += np.concatenate(c.pos).tobytes()
            out += np.concatenate(c.nrm).tobytes()
            out += np.concatenate(c.col).tobytes()
            out += np.concatenate(c.uv).tobytes()
            idx = np.concatenate(c.idx).tobytes()
            out += idx
            if len(idx) % 4:
                out += b"\0" * (4 - len(idx) % 4)
        if self.terrain is not None:
            out += self.terrain.tobytes()
        out += self.ground_png
        out += struct.pack("<I", len(self.atlas_jpegs))
        for j in self.atlas_jpegs:
            out += struct.pack("<I", len(j)) + j
        return bytes(out)


def read_cell(data: bytes) -> dict:
    """Reference reader (used by tests; mirrors the C++ loader)."""
    h = HEADER.unpack_from(data, 0)
    if h[0] != MAGIC:
        raise ValueError("bad magic")
    off = HEADER.size
    nb, nc, nfp, ns, nx, ny = h[9:15]
    png_len = h[19]
    strings = data[off : off + ns]
    off += ns
    buildings = []
    for _ in range(nb):
        f = BUILDING.unpack_from(data, off)
        off += BUILDING.size
        buildings.append(
            {
                "id": strings[f[0] : f[0] + f[1]].decode(),
                "name": strings[f[2] : f[2] + f[3]].decode(),
                "usage": f[4], "height": f[6], "lod": f[9], "geometry_status": f[10],
                "interior_status": f[11], "fp_first": f[21], "fp_count": f[22],
                "chunk": f[23], "first_index": f[24], "index_count": f[25],
            }
        )
    fps = np.frombuffer(data, dtype=np.float32, count=nfp * 2, offset=off).reshape(-1, 2)
    off += nfp * 8
    chunks = []
    for _ in range(nc):
        nv, ni, page = struct.unpack_from("<2Ii", data, off)
        off += 12
        pos = np.frombuffer(data, np.float32, nv * 3, off).reshape(-1, 3)
        off += nv * 12
        off += nv * 12  # normals
        off += nv * 4  # colors
        uv = np.frombuffer(data, np.float32, nv * 2, off).reshape(-1, 2)
        off += nv * 8
        idx = np.frombuffer(data, np.uint16, ni, off)
        off += ni * 2
        off += (4 - (ni * 2) % 4) % 4
        chunks.append({"nv": nv, "ni": ni, "page": page, "pos": pos, "uv": uv, "idx": idx})
    terrain = np.frombuffer(data, np.float32, nx * ny, off).reshape(ny, nx) if nx * ny else None
    off += nx * ny * 4
    png = data[off : off + png_len]
    off += png_len
    (npages,) = struct.unpack_from("<I", data, off)
    off += 4
    pages = []
    for _ in range(npages):
        (ln,) = struct.unpack_from("<I", data, off)
        pages.append(data[off + 4 : off + 4 + ln])
        off += 4 + ln
    if off != len(data):
        raise ValueError(f"trailing bytes: {len(data) - off}")
    return {"mesh": h[1].rstrip(b"\0").decode(), "anchor": h[2:5], "bounds": h[5:9], "buildings": buildings,
            "footprints": fps, "chunks": chunks, "terrain": terrain, "png": png, "pages": pages}
