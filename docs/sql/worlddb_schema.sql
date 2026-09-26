-- PROJECT: REAL JAPAN — authoring World Database (PostgreSQL 16 + PostGIS 3)
-- STATUS: DESIGN DRAFT. Not executed or tested yet (NOT IMPLEMENTED).
-- The runtime never queries this database: data is cooked into RJCELL packages.
--
-- Conventions
--   * geometry in EPSG:6668 (JGD2011 geographic) with Z = T.P. height where relevant
--   * every real-world entity carries provenance columns (see "provenance" below)
--   * mesh codes are JIS X 0410 strings (3rd level = streaming cell)

CREATE EXTENSION IF NOT EXISTS postgis;

CREATE TYPE verification_label AS ENUM ('VERIFIED_INTERIOR', 'PARTIAL_INTERIOR', 'VERIFIED_EXTERIOR', 'UNVERIFIED');
CREATE TYPE interior_status   AS ENUM ('VERIFIED', 'PARTIAL', 'UNKNOWN', 'FICTIONAL_DISCLOSED');
CREATE TYPE source_usage      AS ENUM ('asset', 'reference_only');

CREATE TABLE license (
  id                    text PRIMARY KEY,          -- 'PLATEAU-TOU', 'GSI-TOU', 'ODbL-1.0', 'CC-BY-4.0' ...
  display               text NOT NULL,
  attribution_required  boolean NOT NULL,
  share_alike           boolean NOT NULL,
  redistributable       boolean NOT NULL,
  cleared_for_release   boolean NOT NULL DEFAULT false,  -- legal review done
  notes                 text
);

CREATE TABLE source (
  id            text PRIMARY KEY,                  -- 'plateau:13113_shibuya-ku_pref_2025_citygml_1_op'
  title         text NOT NULL,
  url           text,
  license_id    text NOT NULL REFERENCES license(id),
  attribution   text NOT NULL,
  retrieved_at  date NOT NULL,
  usage         source_usage NOT NULL DEFAULT 'asset'
);

-- Provenance columns shared by every real-world entity table (copy into each table).
-- source_ids text[] NOT NULL, geometry_source text REFERENCES source(id),
-- interior_source text REFERENCES source(id), last_verified date, accuracy_m real,
-- confidence real CHECK (confidence BETWEEN 0 AND 1)

CREATE TABLE prefecture (
  code          smallint PRIMARY KEY CHECK (code BETWEEN 1 AND 47),  -- JIS X 0401
  name_ja       text NOT NULL,
  name_en       text NOT NULL,
  region        text NOT NULL,                     -- Hokkaido .. KyushuOkinawa
  plane_zone    smallint NOT NULL,
  geom          geometry(MultiPolygon, 6668),
  source_ids    text[] NOT NULL DEFAULT '{}'
);

CREATE TABLE municipality (
  code          integer PRIMARY KEY,               -- JIS X 0402 5 digits (13113 = 渋谷区)
  prefecture    smallint NOT NULL REFERENCES prefecture(code),
  name_ja       text NOT NULL,
  geom          geometry(MultiPolygon, 6668),
  source_ids    text[] NOT NULL DEFAULT '{}'
);

CREATE TABLE district (                            -- 町丁目
  key           text PRIMARY KEY,                  -- e.g. 13113015005
  municipality  integer NOT NULL REFERENCES municipality(code),
  name_ja       text,
  geom          geometry(MultiPolygon, 6668),
  source_ids    text[] NOT NULL DEFAULT '{}'
);

CREATE TABLE building (
  id                text PRIMARY KEY,              -- PLATEAU uro:buildingID or project id
  municipality      integer REFERENCES municipality(code),
  mesh3             char(8) NOT NULL,
  name              text,                          -- official name attribute only
  usage_code        smallint,                      -- PLATEAU Building_usage codelist
  measured_height   real,
  storeys_above     smallint,
  storeys_below     smallint,
  lod               smallint NOT NULL,
  footprint         geometry(Polygon, 6668),
  label             verification_label NOT NULL DEFAULT 'UNVERIFIED',
  interior          interior_status NOT NULL DEFAULT 'UNKNOWN',
  source_ids        text[] NOT NULL,
  geometry_source   text REFERENCES source(id),
  interior_source   text REFERENCES source(id),
  last_verified     date,
  accuracy_m        real,
  confidence        real CHECK (confidence BETWEEN 0 AND 1),
  CHECK (interior NOT IN ('VERIFIED', 'PARTIAL') OR interior_source IS NOT NULL)
);
CREATE INDEX building_footprint_gix ON building USING gist (footprint);
CREATE INDEX building_mesh_idx ON building (mesh3);

CREATE TABLE interior_space (
  id              text PRIMARY KEY,
  building_id     text NOT NULL REFERENCES building(id),
  floor           smallint NOT NULL,
  kind            text NOT NULL,                   -- room, corridor, concourse, shop, platform ...
  geom            geometry(PolygonZ, 6668),
  status          interior_status NOT NULL,
  interior_source text REFERENCES source(id),
  CHECK (status NOT IN ('VERIFIED', 'PARTIAL') OR interior_source IS NOT NULL)
);

CREATE TABLE road_node (
  id    bigserial PRIMARY KEY,
  geom  geometry(PointZ, 6668) NOT NULL
);
CREATE TABLE road_link (
  id            bigserial PRIMARY KEY,
  from_node     bigint NOT NULL REFERENCES road_node(id),
  to_node       bigint NOT NULL REFERENCES road_node(id),
  road_class    text,                              -- expressway, national, prefectural, municipal ...
  name          text,
  lanes_fwd     smallint,
  lanes_bwd     smallint,
  oneway        boolean,
  maxspeed_kmh  smallint,
  structure     text,                              -- surface, bridge, tunnel, elevated, underground
  geom          geometry(LineStringZ, 6668) NOT NULL,
  source_ids    text[] NOT NULL
);
CREATE INDEX road_link_gix ON road_link USING gist (geom);

CREATE TABLE railway_line (
  id          text PRIMARY KEY,
  operator    text NOT NULL,
  name_ja     text NOT NULL,
  kind        text NOT NULL,                       -- jr, private, subway, shinkansen, monorail, tram, agt
  source_ids  text[] NOT NULL
);
CREATE TABLE track_segment (
  id          bigserial PRIMARY KEY,
  line_id     text NOT NULL REFERENCES railway_line(id),
  geom        geometry(LineStringZ, 6668) NOT NULL,
  source_ids  text[] NOT NULL
);
CREATE TABLE station (
  id          text PRIMARY KEY,
  name_ja     text NOT NULL,
  building_id text REFERENCES building(id),
  geom        geometry(Point, 6668) NOT NULL,
  source_ids  text[] NOT NULL
);

CREATE TABLE airport (
  icao        char(4) PRIMARY KEY,
  name_ja     text NOT NULL,
  geom        geometry(MultiPolygon, 6668),
  source_ids  text[] NOT NULL
);
CREATE TABLE port (
  id          text PRIMARY KEY,
  name_ja     text NOT NULL,
  geom        geometry(MultiPolygon, 6668),
  source_ids  text[] NOT NULL
);

CREATE TABLE business (
  id            text PRIMARY KEY,
  building_id   text REFERENCES building(id),
  floor         smallint,
  category      text NOT NULL,                     -- convenience_store, ramen, clinic ...
  display_name  text,                              -- generic unless a licence permits the real brand
  brand_cleared boolean NOT NULL DEFAULT false,
  source_ids    text[] NOT NULL
);

CREATE TABLE poi (
  id          text PRIMARY KEY,
  name        text,
  category    text,
  geom        geometry(Point, 6668) NOT NULL,
  source_ids  text[] NOT NULL
);

-- Simulation state (fictional people living in real places)
CREATE TABLE npc (
  id            bigint PRIMARY KEY,
  seed          bigint NOT NULL,
  home_building text REFERENCES building(id),
  work_building text REFERENCES building(id),
  occupation    text NOT NULL,
  profile       jsonb NOT NULL                     -- personality, hobbies, tastes ...
);
CREATE TABLE vehicle (
  id          bigint PRIMARY KEY,
  owner_npc   bigint REFERENCES npc(id),
  kind        text NOT NULL,
  spec        jsonb NOT NULL
);
CREATE TABLE weather_region (
  id          text PRIMARY KEY,
  geom        geometry(MultiPolygon, 6668),
  climatology jsonb                                -- from JMA normals once licensed/imported
);
CREATE TABLE economy_account (
  id          bigint PRIMARY KEY,
  kind        text NOT NULL,                       -- person, household, business, government, utility, external
  owner_ref   text
);
CREATE TABLE event (
  id          bigserial PRIMARY KEY,
  at          timestamptz NOT NULL,
  kind        text NOT NULL,
  payload     jsonb NOT NULL
);
