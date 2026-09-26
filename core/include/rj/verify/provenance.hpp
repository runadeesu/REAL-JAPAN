#pragma once
// Verification System. Every real-world element carries provenance: where
// its geometry / interior / attributes came from, under which licence, when
// it was last verified and how confident we are.
//
// Hard rules enforced here (and tested):
//  * Only VERIFIED data may be presented as "real".
//  * A building interior without an interior source can never be labelled
//    as the real interior. A generated interior must be disclosed as
//    fictional in the UI.
//  * Reference-only sources (e.g. Google Maps/Earth, used for checking under
//    their terms) may never be the source of shipped geometry or interiors.
//  * Unknown licences block publication.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rj::verify {

enum class GeometryStatus : uint8_t { VerifiedExterior, Unverified };
enum class InteriorStatus : uint8_t { Verified, Partial, Unknown, FictionalDisclosed };

// Combined building label required by the spec.
enum class BuildingLabel : uint8_t { VERIFIED_INTERIOR, PARTIAL_INTERIOR, VERIFIED_EXTERIOR, UNVERIFIED };
std::string_view labelName(BuildingLabel l);

enum class SourceUsage : uint8_t { Asset, ReferenceOnly };

struct LicenseInfo {
  std::string_view id;
  std::string_view display;
  bool attribution_required;
  bool share_alike;
  bool redistributable;
};

// Licences cleared for use in shipped game data.
std::optional<LicenseInfo> knownLicense(std::string_view id);

struct SourceRef {
  std::string source_id;    // e.g. "plateau:13113_shibuya-ku_pref_2025_citygml_1_op"
  std::string title;
  std::string url;
  std::string license_id;   // e.g. "CC-BY-4.0", "ODbL-1.0", "GSI-TOU", "PLATEAU-TOU"
  std::string attribution;  // text that must appear in credits
  std::string retrieved_at; // ISO 8601 date
  SourceUsage usage = SourceUsage::Asset;
};

struct Provenance {
  std::vector<SourceRef> sources;
  std::string geometry_source;  // source_id that produced the geometry
  std::string interior_source;  // source_id that produced the interior (empty = none)
  std::string last_verified;    // ISO 8601 date
  std::optional<double> accuracy_m;  // positional accuracy if the source states it
  double confidence = 0.0;           // 0..1
  GeometryStatus geometry = GeometryStatus::Unverified;
  InteriorStatus interior = InteriorStatus::Unknown;
};

BuildingLabel classify(const Provenance& p);

struct Presentation {
  bool may_claim_real = false;         // may UI call this "real / 実在"?
  bool interior_enterable = false;     // may the player enter?
  bool must_disclose_fictional = false;// UI must show "架空の内部 / Fictional interior"
  std::string label_key;               // localisation key for the badge
};

Presentation presentation(const Provenance& p);

// Empty = publishable. Otherwise human-readable violations.
std::vector<std::string> validate(const Provenance& p);

}  // namespace rj::verify
