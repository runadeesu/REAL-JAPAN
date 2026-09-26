#include "rj/verify/provenance.hpp"

#include <algorithm>
#include <array>

namespace rj::verify {
namespace {

constexpr std::array<LicenseInfo, 6> kLicenses{{
    {"CC-BY-4.0", "Creative Commons Attribution 4.0", true, false, true},
    {"ODbL-1.0", "Open Database License 1.0 (OpenStreetMap)", true, true, true},
    // PLATEAU site policy §3: 公共データ利用規約(第1.0版) (PDL1.0), compatible with CC BY 4.0.
    {"PLATEAU-TOU", "PLATEAU Site Policy / PDL1.0 (CC BY 4.0 compatible)", true, false, true},
    // 国土地理院コンテンツ利用規約 (CC BY 4.0 compatible, attribution "国土地理院").
    {"GSI-TOU", "GSI Content Terms of Use (CC BY 4.0 compatible)", true, false, true},
    {"CC0-1.0", "Creative Commons Zero 1.0", false, false, true},
    {"PROJECT-ORIGINAL", "Original work of this project", false, false, true},
}};

const SourceRef* findSource(const Provenance& p, const std::string& id) {
  for (const auto& s : p.sources)
    if (s.source_id == id) return &s;
  return nullptr;
}

}  // namespace

std::string_view labelName(BuildingLabel l) {
  switch (l) {
    case BuildingLabel::VERIFIED_INTERIOR: return "VERIFIED_INTERIOR";
    case BuildingLabel::PARTIAL_INTERIOR: return "PARTIAL_INTERIOR";
    case BuildingLabel::VERIFIED_EXTERIOR: return "VERIFIED_EXTERIOR";
    case BuildingLabel::UNVERIFIED: return "UNVERIFIED";
  }
  return "?";
}

std::optional<LicenseInfo> knownLicense(std::string_view id) {
  for (const auto& l : kLicenses)
    if (l.id == id) return l;
  return std::nullopt;
}

BuildingLabel classify(const Provenance& p) {
  if (p.geometry != GeometryStatus::VerifiedExterior) return BuildingLabel::UNVERIFIED;
  if (p.interior == InteriorStatus::Verified && !p.interior_source.empty())
    return BuildingLabel::VERIFIED_INTERIOR;
  if (p.interior == InteriorStatus::Partial && !p.interior_source.empty())
    return BuildingLabel::PARTIAL_INTERIOR;
  return BuildingLabel::VERIFIED_EXTERIOR;
}

Presentation presentation(const Provenance& p) {
  Presentation out;
  switch (classify(p)) {
    case BuildingLabel::VERIFIED_INTERIOR:
      out = {true, true, false, "verify.badge.verified_interior"};
      break;
    case BuildingLabel::PARTIAL_INTERIOR:
      // Exterior is real; interior only partially sourced -> must say so.
      out = {true, true, true, "verify.badge.partial_interior"};
      break;
    case BuildingLabel::VERIFIED_EXTERIOR:
      out = {true, false, false, "verify.badge.verified_exterior"};
      break;
    case BuildingLabel::UNVERIFIED:
      out = {false, false, false, "verify.badge.unverified"};
      break;
  }
  if (p.interior == InteriorStatus::FictionalDisclosed) {
    out.interior_enterable = true;
    out.must_disclose_fictional = true;
  }
  return out;
}

std::vector<std::string> validate(const Provenance& p) {
  std::vector<std::string> err;
  for (const auto& s : p.sources) {
    if (!knownLicense(s.license_id))
      err.push_back("source '" + s.source_id + "': licence '" + s.license_id + "' not cleared (権利未確認)");
    else if (knownLicense(s.license_id)->attribution_required && s.attribution.empty())
      err.push_back("source '" + s.source_id + "': attribution text missing");
  }
  if (p.geometry == GeometryStatus::VerifiedExterior) {
    const SourceRef* g = findSource(p, p.geometry_source);
    if (!g) err.push_back("VERIFIED exterior without a listed geometry source");
    else if (g->usage == SourceUsage::ReferenceOnly)
      err.push_back("geometry derived from reference-only source '" + g->source_id + "'");
    if (p.last_verified.empty()) err.push_back("VERIFIED exterior without last_verified date");
  }
  if (p.interior == InteriorStatus::Verified || p.interior == InteriorStatus::Partial) {
    const SourceRef* i = findSource(p, p.interior_source);
    if (!i) err.push_back("interior marked VERIFIED/PARTIAL without an interior source");
    else if (i->usage == SourceUsage::ReferenceOnly)
      err.push_back("interior derived from reference-only source '" + i->source_id + "'");
  }
  if (p.confidence < 0.0 || p.confidence > 1.0) err.push_back("confidence out of range");
  return err;
}

}  // namespace rj::verify
