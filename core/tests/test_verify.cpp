#include "rj/verify/provenance.hpp"
#include "rj_test.hpp"

using namespace rj::verify;

namespace {
SourceRef plateau() {
  return {"plateau:13113_2025", "PLATEAU 渋谷区 2025", "https://www.geospatial.jp/ckan/dataset/plateau-13113-shibuya-ku-2025",
          "PLATEAU-TOU", "出典: 国土交通省 Project PLATEAU", "2026-09-26", SourceUsage::Asset};
}
}  // namespace

RJ_TEST(plateau_exterior_is_verified_exterior_only) {
  Provenance p;
  p.sources = {plateau()};
  p.geometry_source = "plateau:13113_2025";
  p.last_verified = "2026-09-26";
  p.confidence = 0.9;
  p.geometry = GeometryStatus::VerifiedExterior;
  RJ_CHECK(validate(p).empty());
  RJ_CHECK(classify(p) == BuildingLabel::VERIFIED_EXTERIOR);
  const Presentation pr = presentation(p);
  RJ_CHECK(pr.may_claim_real);
  RJ_CHECK(!pr.interior_enterable);  // no interior source: not enterable as "real"
}

RJ_TEST(interior_claims_require_interior_source) {
  Provenance p;
  p.sources = {plateau()};
  p.geometry_source = "plateau:13113_2025";
  p.last_verified = "2026-09-26";
  p.geometry = GeometryStatus::VerifiedExterior;
  p.interior = InteriorStatus::Verified;  // but no interior_source!
  RJ_CHECK(!validate(p).empty());
  RJ_CHECK(classify(p) == BuildingLabel::VERIFIED_EXTERIOR);  // never upgraded without a source

  // PLATEAU underground-mall LOD4 is an official interior source.
  p.interior_source = "plateau:13113_2025";
  RJ_CHECK(validate(p).empty());
  RJ_CHECK(classify(p) == BuildingLabel::VERIFIED_INTERIOR);
}

RJ_TEST(fictional_interior_must_be_disclosed) {
  Provenance p;
  p.sources = {plateau()};
  p.geometry_source = "plateau:13113_2025";
  p.last_verified = "2026-09-26";
  p.geometry = GeometryStatus::VerifiedExterior;
  p.interior = InteriorStatus::FictionalDisclosed;
  const Presentation pr = presentation(p);
  RJ_CHECK(pr.interior_enterable);
  RJ_CHECK(pr.must_disclose_fictional);
  RJ_CHECK(classify(p) == BuildingLabel::VERIFIED_EXTERIOR);
}

RJ_TEST(reference_only_and_unknown_licences_are_rejected) {
  Provenance p;
  SourceRef g{"google:maps", "Google Maps", "https://maps.google.com", "PROPRIETARY", "", "2026-09-26",
              SourceUsage::ReferenceOnly};
  p.sources = {g};
  p.geometry_source = "google:maps";
  p.last_verified = "2026-09-26";
  p.geometry = GeometryStatus::VerifiedExterior;
  const auto errs = validate(p);
  RJ_CHECK(errs.size() >= 2u);  // unknown licence + reference-only geometry
  Provenance none;
  RJ_CHECK(classify(none) == BuildingLabel::UNVERIFIED);
  RJ_CHECK(!presentation(none).may_claim_real);
  RJ_CHECK(knownLicense("ODbL-1.0")->share_alike);
}
