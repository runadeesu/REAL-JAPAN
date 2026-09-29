#pragma once
// Procedural aircraft (no third-party assets; generic designs, no real type or airline):
//  * a twin-engine regional jet: fuselage with windows and cockpit glazing, swept low wing with
//    winglets, under-wing engines on pylons, conventional tail, retractable gear, navigation
//    lights; a cabin interior (2+2 seats, bins, window reveals) for riding at a window seat
//  * a high-wing four-seat light aircraft: fuselage, strut-braced wing, tail, fixed tricycle gear
//    with spats, two-blade propeller; a cockpit (panel with six flight instruments, yoke, seats,
//    door frames) for flying it.
// Model space: x right, y forward, z up; origins at the reference point / centre of gravity.

#include "raylib.h"

namespace rjc {

struct JetModel {
  Mesh fuselage{};  // outer shell (not drawn from inside)
  Mesh wings{};     // wings, engines, tail
  Mesh gear{};      // landing gear (hidden when retracted)
  Mesh cabin{};     // interior with window openings, the vestibule by the front left door
  Mesh door{};      // the front left door (closed; the opening is in the fuselage)
  Mesh stairs{};    // passenger stairs at the front left door (at the stand)
  Mesh nav_red{}, nav_green{}, nav_white{};
};

struct LightPlaneModel {
  Mesh fuselage{};  // cabin shell (not drawn from the cockpit)
  Mesh rest{};      // wing, struts, tail, gear
  Mesh prop{};      // two blades about model +y at the spinner
  Mesh cockpit{};   // interior: panel, seats, frames
  Mesh dial_marks{};
  Mesh needle{};    // pivot at the origin, pointing up (+z), facing -y
  Mesh yoke{};      // pivot at the column
  float prop_y = 2.35f;
  float dial[6][3]{};  // airspeed, attitude, altimeter, turn, heading, vertical speed
  float yoke_pos[3]{};
};

class AircraftModels {
 public:
  void build();
  void unload();
  bool ready() const { return ready_; }
  const JetModel& jet() const { return jet_; }
  const LightPlaneModel& light() const { return light_; }

 private:
  JetModel jet_;
  LightPlaneModel light_;
  bool ready_ = false;
};

// The cabin, its seats, the door and the stairs: game/deck_layout.hpp.

}  // namespace rjc
