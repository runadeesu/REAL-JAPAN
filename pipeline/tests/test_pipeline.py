"""Pipeline unit tests. Run: python3 -m pytest pipeline/tests -q
Fixtures below are tiny SYNTHETIC CityGML snippets written for the tests (not real data)."""

import os
import struct
import sys
import tempfile

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from realjapan_pipeline.atlas import Atlas  # noqa: E402
from realjapan_pipeline.citygml import iter_buildings, iter_roads, parse_appearance  # noqa: E402
from realjapan_pipeline.dem_gsi import decode_png, lonlat_to_tile  # noqa: E402
from realjapan_pipeline.geodesy import LocalFrame, Mesh, mesh3_from_latlon, meshes3_covering  # noqa: E402
from realjapan_pipeline.rjcell import BuildingRec, CellWriter, read_cell  # noqa: E402
from realjapan_pipeline.triangulate import triangulate  # noqa: E402

FIXTURE = """<?xml version="1.0" encoding="UTF-8"?>
<core:CityModel xmlns:core="http://www.opengis.net/citygml/2.0" xmlns:gml="http://www.opengis.net/gml"
 xmlns:bldg="http://www.opengis.net/citygml/building/2.0" xmlns:tran="http://www.opengis.net/citygml/transportation/2.0"
 xmlns:uro="https://www.geospatial.jp/iur/uro/3.2" xmlns:app="http://www.opengis.net/citygml/appearance/2.0">
 <app:appearanceMember><app:Appearance><app:surfaceDataMember><app:ParameterizedTexture>
  <app:imageURI>tex/wall.jpg</app:imageURI>
  <app:target uri="#p1"><app:TexCoordList>
   <app:textureCoordinates ring="#r1">0 0 1 0 1 1 0 1 0 0</app:textureCoordinates>
  </app:TexCoordList></app:target>
 </app:ParameterizedTexture></app:surfaceDataMember></app:Appearance></app:appearanceMember>
 <core:cityObjectMember><bldg:Building gml:id="b1">
  <gml:name>テストビル</gml:name>
  <bldg:usage>401</bldg:usage><bldg:measuredHeight uom="m">10.0</bldg:measuredHeight>
  <bldg:storeysAboveGround>3</bldg:storeysAboveGround>
  <bldg:lod0RoofEdge><gml:MultiSurface><gml:surfaceMember><gml:Polygon><gml:exterior><gml:LinearRing>
   <gml:posList>35.0 139.0 0 35.0 139.0001 0 35.0001 139.0001 0 35.0001 139.0 0 35.0 139.0 0</gml:posList>
  </gml:LinearRing></gml:exterior></gml:Polygon></gml:surfaceMember></gml:MultiSurface></bldg:lod0RoofEdge>
  <bldg:boundedBy><bldg:WallSurface><bldg:lod2MultiSurface><gml:MultiSurface><gml:surfaceMember>
   <gml:Polygon gml:id="p1"><gml:exterior><gml:LinearRing gml:id="r1">
    <gml:posList>35.0 139.0 5 35.0 139.0001 5 35.0 139.0001 15 35.0 139.0 15 35.0 139.0 5</gml:posList>
   </gml:LinearRing></gml:exterior></gml:Polygon>
  </gml:surfaceMember></gml:MultiSurface></bldg:lod2MultiSurface></bldg:WallSurface></bldg:boundedBy>
  <uro:buildingIDAttribute><uro:BuildingIDAttribute><uro:buildingID>13113-bldg-1</uro:buildingID></uro:BuildingIDAttribute></uro:buildingIDAttribute>
 </bldg:Building></core:cityObjectMember>
 <core:cityObjectMember><tran:Road gml:id="t1"><tran:function>3</tran:function>
  <tran:lod1MultiSurface><gml:MultiSurface><gml:surfaceMember><gml:Polygon><gml:exterior><gml:LinearRing>
   <gml:posList>35.0 139.0 0 35.0 139.001 0 35.0001 139.001 0 35.0 139.0 0</gml:posList>
  </gml:LinearRing></gml:exterior></gml:Polygon></gml:surfaceMember></gml:MultiSurface></tran:lod1MultiSurface>
  <tran:trafficArea><tran:TrafficArea><tran:function>2000</tran:function><tran:lod2MultiSurface><gml:MultiSurface>
   <gml:surfaceMember><gml:Polygon><gml:exterior><gml:LinearRing>
    <gml:posList>35.0 139.0 0 35.0 139.0002 0 35.00002 139.0002 0 35.0 139.0 0</gml:posList>
   </gml:LinearRing></gml:exterior></gml:Polygon></gml:surfaceMember></gml:MultiSurface></tran:lod2MultiSurface>
  </tran:TrafficArea></tran:trafficArea>
 </tran:Road></core:cityObjectMember>
</core:CityModel>
"""


def fixture_path():
    fd, p = tempfile.mkstemp(suffix=".gml")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write(FIXTURE)
    return p


def test_mesh_codes_match_core():
    assert mesh3_from_latlon(35.658, 139.7016) == "53393586"
    assert mesh3_from_latlon(35.6812, 139.7671) == "53394611"
    b = Mesh("53393585").bounds()
    assert abs(b[0] - 35.65) < 1e-12 and abs(b[1] - 139.6875) < 1e-12
    assert sorted(meshes3_covering(35.6535, 139.6961, 35.6625, 139.7071)) == [
        "53393585", "53393586", "53393595", "53393596"]


def test_local_frame_origin_and_north():
    f = LocalFrame(35.658, 139.7016, 40.0)
    assert np.allclose(f.to_local(35.658, 139.7016, 40.0), (0, 0, 0), atol=1e-6)
    x, y, z = f.to_local(35.658 + 1000 / 110950, 139.7016, 40.0)
    assert abs(y - 1000) < 3 and -0.2 < z < 0


def test_citygml_building_road_appearance():
    p = fixture_path()
    try:
        bl = list(iter_buildings(p))
        assert len(bl) == 1
        b = bl[0]
        assert b.name == "テストビル" and b.usage == 401 and b.building_id == "13113-bldg-1"
        assert b.best_lod == 2 and len(b.footprint) == 4
        assert b.lod2[0].ext_id == "r1" and b.lod2[0].kind == "wall"
        roads = list(iter_roads(p))
        assert len(roads) == 1 and roads[0].detail[0].kind == "sidewalk"
        app = parse_appearance(p)
        assert app.images == ["tex/wall.jpg"]
        img, uv = app.ring_uv["r1"]
        assert img == 0 and len(uv) == 4  # closing coordinate dropped
    finally:
        os.remove(p)


def test_triangulate_faces_follow_normal():
    square = np.array([[0, 0, 0], [10, 0, 0], [10, 10, 0], [0, 10, 0]], dtype=float)
    hole = np.array([[4, 4, 0], [6, 4, 0], [6, 6, 0], [4, 6, 0]], dtype=float)
    v, t, n = triangulate(square, [hole])
    assert n[2] > 0.99 and len(v) == 8 and len(t) == 8
    a, b, c = v[t[:, 0]], v[t[:, 1]], v[t[:, 2]]
    assert np.all(np.cross(b - a, c - a)[:, 2] > 0)


def test_dem_png_decoding():
    rgb = np.zeros((1, 3, 3), dtype=np.uint8)
    rgb[0, 0] = (0, 0x27, 0x10)       # 10000 * 0.01 = 100.00 m
    rgb[0, 1] = (0x80, 0, 0)          # 2^23 = no data
    rgb[0, 2] = (0xFF, 0xFF, 0x9C)    # (2^24 - 100 - 2^24) * 0.01 = -1.00 m
    h = decode_png(rgb)
    assert abs(h[0, 0] - 100.0) < 1e-9 and np.isnan(h[0, 1]) and abs(h[0, 2] + 1.0) < 1e-9
    x, y = lonlat_to_tile(139.7016, 35.658, 15)
    assert int(x) == 29099 and int(y) == 12905


def test_atlas_packing_and_remap():
    imgs = {0: Image.new("RGB", (256, 128), (255, 0, 0)), 1: Image.new("RGB", (64, 64), (0, 255, 0))}
    a = Atlas(page=512, pad=2)
    a.build(imgs)
    assert set(a.rects) == {0, 1}
    x, y, w, h = a.rects[0]
    u0, v0 = a.remap(0, 0.0, 1.0)  # CityGML top-left corner
    assert abs(u0 - x / 512) < 1e-9 and abs(v0 - y / 512) < 1e-9
    px = a.image.getpixel((x + w // 2, y + h // 2))
    assert px[0] > 200 and px[1] < 50


def test_rjcell_roundtrip():
    w = CellWriter("53393586", (35.654, 139.706, 15.0), (35.65, 139.7, 35.658, 139.7125))
    pos = np.array([[0, 0, 0], [1, 0, 0], [0, 1, 0]], dtype=float)
    ch, first, cnt = w.add_geometry(pos, np.tile([0, 0, 1.0], (3, 1)), np.full((3, 4), 200), np.array([0, 1, 2]),
                                    np.array([[0, 0], [1, 0], [0, 1.0]]), page=0)
    w.buildings.append(BuildingRec("id-1", "名前", 401, 3001, 12.5, 3, 0, 2, 0, 2, 0, 0.0, (0, 0, 0), (1, 1, 12.5),
                                   [(0, 0), (1, 0), (1, 1)], ch, first, cnt))
    w.set_terrain(np.zeros((3, 3)), 35.65, 139.7, 0.004, 0.006)
    w.ground_png = b"\x89PNG-fake"
    w.atlas_jpegs.append(b"JPEG-fake")
    c = read_cell(w.to_bytes())
    assert c["mesh"] == "53393586" and c["buildings"][0]["name"] == "名前"
    assert c["chunks"][0]["page"] == 0 and c["chunks"][0]["nv"] == 3
    assert c["terrain"].shape == (3, 3) and c["pages"] == [b"JPEG-fake"]
    assert struct.unpack_from("<8s", w.to_bytes())[0] == b"RJCELL02"
