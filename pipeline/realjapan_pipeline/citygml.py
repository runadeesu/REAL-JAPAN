"""Streaming parsers for PLATEAU CityGML (i-UR 3.x / CityGML 2.0).

Only what the game needs is extracted. Files can be >100 MB, so parsing is
done with iterparse and elements are cleared as soon as they are consumed.
Coordinates in PLATEAU files are EPSG:6697 (JGD2011 lat, lon + T.P. height).
"""

from __future__ import annotations

import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from typing import Iterator

NS = {
    "gml": "http://www.opengis.net/gml",
    "bldg": "http://www.opengis.net/citygml/building/2.0",
    "tran": "http://www.opengis.net/citygml/transportation/2.0",
    "core": "http://www.opengis.net/citygml/2.0",
    "gen": "http://www.opengis.net/citygml/generics/2.0",
    "uro": "https://www.geospatial.jp/iur/uro/3.2",
    "app": "http://www.opengis.net/citygml/appearance/2.0",
    "frn": "http://www.opengis.net/citygml/cityfurniture/2.0",
}


def q(tag: str) -> str:
    p, t = tag.split(":")
    return "{%s}%s" % (NS[p], t)


Ring = list[tuple[float, float, float]]  # (lat, lon, h)


@dataclass
class Polygon:
    exterior: Ring
    interiors: list[Ring] = field(default_factory=list)
    kind: str = "wall"  # roof | wall | ground | installation | lod1 | road | sidewalk | median
    ext_id: str = ""  # gml:id of the exterior LinearRing (texture lookup key)
    int_ids: list[str] = field(default_factory=list)


@dataclass
class Building:
    gml_id: str
    building_id: str = ""
    name: str = ""
    usage: int = 0
    bclass: int = 0
    measured_height: float = -1.0
    storeys_above: int = -1
    storeys_below: int = -1
    creation_date: str = ""
    survey_year: str = ""
    lod1: list[Polygon] = field(default_factory=list)
    lod2: list[Polygon] = field(default_factory=list)
    footprint: Ring = field(default_factory=list)

    @property
    def best_lod(self) -> int:
        return 2 if self.lod2 else (1 if self.lod1 else 0)


def _parse_poslist(text: str | None) -> Ring:
    if not text:
        return []
    v = text.split()
    pts = [(float(v[i]), float(v[i + 1]), float(v[i + 2])) for i in range(0, len(v) - 2, 3)]
    if len(pts) > 1 and pts[0] == pts[-1]:
        pts.pop()
    return pts


def _polygon(el: ET.Element, kind: str) -> Polygon | None:
    lr = el.find("gml:exterior/gml:LinearRing", NS)
    ext = lr.find("gml:posList", NS) if lr is not None else None
    if ext is None:
        return None
    ring = _parse_poslist(ext.text)
    if len(ring) < 3:
        return None
    holes, hole_ids = [], []
    for it in el.findall("gml:interior/gml:LinearRing", NS):
        pl = it.find("gml:posList", NS)
        h = _parse_poslist(pl.text if pl is not None else None)
        if len(h) >= 3:
            holes.append(h)
            hole_ids.append(it.get(q("gml:id"), ""))
    return Polygon(ring, holes, kind, lr.get(q("gml:id"), ""), hole_ids)


@dataclass
class Appearance:
    images: list[str] = field(default_factory=list)  # imageURI relative to the gml file
    ring_uv: dict[str, tuple[int, list[tuple[float, float]]]] = field(default_factory=dict)


def parse_appearance(path: str) -> Appearance:
    """Collect ParameterizedTexture targets: LinearRing gml:id -> (image index, texture coordinates)."""
    app = Appearance()
    index: dict[str, int] = {}
    for ev, el in ET.iterparse(path, events=("end",)):
        if el.tag != q("app:ParameterizedTexture"):
            continue
        uri = _text(el, "app:imageURI")
        if uri:
            if uri not in index:
                index[uri] = len(app.images)
                app.images.append(uri)
            ii = index[uri]
            for tc in el.findall("app:target/app:TexCoordList/app:textureCoordinates", NS):
                ring = (tc.get("ring") or "").lstrip("#")
                v = (tc.text or "").split()
                uv = [(float(v[k]), float(v[k + 1])) for k in range(0, len(v) - 1, 2)]
                if len(uv) > 1 and uv[0] == uv[-1]:
                    uv.pop()
                if ring:
                    app.ring_uv[ring] = (ii, uv)
        el.clear()
    return app


_SURFACE_KIND = {
    q("bldg:RoofSurface"): "roof",
    q("bldg:WallSurface"): "wall",
    q("bldg:GroundSurface"): "ground",
    q("bldg:OuterCeilingSurface"): "wall",
    q("bldg:OuterFloorSurface"): "roof",
    q("bldg:ClosureSurface"): "wall",
}


def _text(el: ET.Element, path: str) -> str:
    x = el.find(path, NS)
    return x.text.strip() if x is not None and x.text else ""


def _building_from_element(b: ET.Element) -> Building:
    bl = Building(gml_id=b.get(q("gml:id"), ""))
    bl.name = _text(b, "gml:name")
    bl.building_id = _text(b, "uro:buildingIDAttribute/uro:BuildingIDAttribute/uro:buildingID")
    bl.creation_date = _text(b, "core:creationDate")
    bl.survey_year = _text(b, "uro:buildingDetailAttribute/uro:BuildingDetailAttribute/uro:surveyYear")
    try:
        bl.usage = int(_text(b, "bldg:usage") or 0)
        bl.bclass = int(_text(b, "bldg:class") or 0)
        bl.measured_height = float(_text(b, "bldg:measuredHeight") or -1)
        bl.storeys_above = int(_text(b, "bldg:storeysAboveGround") or -1)
        bl.storeys_below = int(_text(b, "bldg:storeysBelowGround") or -1)
    except ValueError:
        pass
    fp = b.find("bldg:lod0RoofEdge//gml:Polygon", NS)
    if fp is None:
        fp = b.find("bldg:lod0FootPrint//gml:Polygon", NS)
    if fp is not None:
        p = _polygon(fp, "ground")
        if p:
            bl.footprint = p.exterior
    for poly in b.findall("bldg:lod1Solid//gml:Polygon", NS):
        p = _polygon(poly, "lod1")
        if p:
            bl.lod1.append(p)
    for bb in b.findall("bldg:boundedBy", NS):
        for surf in bb:
            kind = _SURFACE_KIND.get(surf.tag, "wall")
            for poly in surf.findall(".//gml:Polygon", NS):
                p = _polygon(poly, kind)
                if p:
                    bl.lod2.append(p)
    for inst in b.findall("bldg:outerBuildingInstallation/bldg:BuildingInstallation", NS):
        for poly in inst.findall("bldg:lod2Geometry//gml:Polygon", NS):
            p = _polygon(poly, "installation")
            if p:
                bl.lod2.append(p)
    if not bl.footprint and bl.lod1:
        bl.footprint = min(bl.lod1, key=lambda p: sum(v[2] for v in p.exterior) / len(p.exterior)).exterior
    return bl


def iter_buildings(path: str) -> Iterator[Building]:
    """Yield top-level bldg:Building features (BuildingParts are merged into the parent)."""
    depth = 0
    for ev, el in ET.iterparse(path, events=("start", "end")):
        if el.tag == q("bldg:Building"):
            if ev == "start":
                depth += 1
            else:
                depth -= 1
                if depth == 0:
                    yield _building_from_element(el)
                    el.clear()


TRAFFIC_KIND = {1000: "road", 1010: "road", 1020: "road", 1030: "road", 1040: "road", 1050: "rail",
                1070: "road", 1130: "road", 2000: "sidewalk", 2010: "sidewalk", 2020: "sidewalk",
                2030: "sidewalk", 6000: "sidewalk", 7000: "road"}
AUX_KIND = {3000: "median", 3010: "median", 3020: "median", 5000: "planting", 5010: "planting",
            5020: "planting", 1060: "road", 1080: "median", 1090: "road", 1100: "road", 1110: "road",
            1120: "road", 4000: "sidewalk", 6000: "sidewalk", 7000: "road", 1000: "road"}


@dataclass
class Road:
    gml_id: str
    name: str = ""
    function: int = 0
    lod1: list[Polygon] = field(default_factory=list)
    detail: list[Polygon] = field(default_factory=list)  # LOD2 traffic / auxiliary areas


def iter_roads(path: str) -> Iterator[Road]:
    for ev, el in ET.iterparse(path, events=("end",)):
        if el.tag != q("tran:Road"):
            continue
        r = Road(gml_id=el.get(q("gml:id"), ""), name=_text(el, "gml:name"))
        try:
            r.function = int(_text(el, "tran:function") or 0)
        except ValueError:
            pass
        for poly in el.findall("tran:lod1MultiSurface//gml:Polygon", NS):
            p = _polygon(poly, "road")
            if p:
                r.lod1.append(p)
        for ta in el.findall("tran:trafficArea/tran:TrafficArea", NS):
            f = int(_text(ta, "tran:function") or 0)
            for poly in ta.findall("tran:lod2MultiSurface//gml:Polygon", NS):
                p = _polygon(poly, TRAFFIC_KIND.get(f, "road"))
                if p:
                    r.detail.append(p)
        for ta in el.findall("tran:auxiliaryTrafficArea/tran:AuxiliaryTrafficArea", NS):
            f = int(_text(ta, "tran:function") or 0)
            for poly in ta.findall("tran:lod2MultiSurface//gml:Polygon", NS):
                p = _polygon(poly, AUX_KIND.get(f, "median"))
                if p:
                    r.detail.append(p)
        yield r
        el.clear()


@dataclass
class Furniture:
    gml_id: str
    function: int = 0
    lod: int = 0
    polys: list[Polygon] = field(default_factory=list)


def iter_furniture(path: str) -> Iterator[Furniture]:
    """frn:CityFurniture (signals, lights, poles, fences, road markings ...). Highest LOD available."""
    for ev, el in ET.iterparse(path, events=("end",)):
        if el.tag != q("frn:CityFurniture"):
            continue
        f = Furniture(gml_id=el.get(q("gml:id"), ""))
        try:
            f.function = int(_text(el, "frn:function") or 0)
        except ValueError:
            pass
        for lod in (3, 2, 1):
            polys = [p for p in (_polygon(x, "furniture") for x in el.findall(f"frn:lod{lod}Geometry//gml:Polygon", NS)) if p]
            if polys:
                f.lod, f.polys = lod, polys
                break
        yield f
        el.clear()
