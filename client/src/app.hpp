#pragma once
// Application: screens, game session, save/load, settings, localisation.

#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "audio/audio.hpp"
#include "config/settings.hpp"
#include "game/aircraft.hpp"
#include "game/crowd.hpp"
#include "game/driving.hpp"
#include "game/ferries.hpp"
#include "game/jobs.hpp"
#include "game/pedestrians.hpp"
#include "game/player.hpp"
#include "game/town_sim.hpp"
#include "game/traffic.hpp"
#include "game/road_markings.hpp"
#include "game/shops.hpp"
#include "game/trains.hpp"
#include "game/traffic_signals.hpp"
#include "game/weather.hpp"
#include "i18n/i18n.hpp"
#include "render/farview.hpp"
#include "render/neartrees.hpp"
#include "render/renderer.hpp"
#include "rj/econ/ledger.hpp"
#include "rj/sim/calendar.hpp"
#include "ui/ui.hpp"
#include "world/world.hpp"

namespace rjc {

struct LaunchOptions {
  std::string screenshot;  // write a PNG after `frames` frames in `state`, then quit
  int frames = 120;
  std::string state;       // title | game | pause | phone:map | phone:town | phone:clock | settings | credits
  std::string time_jst;    // "YYYY-MM-DDTHH:MM"
  bool has_pos = false;
  double lat = 0, lon = 0;
  float yaw_deg = NAN, pitch_deg = NAN;
  std::string lang;
  bool fly = false;
  double alt = 0;
  int camera_mode = -1;
  std::string world;  // --world island|shibuya
  int time_scale = 0;  // override (0 = use settings)
  bool selftest = false;  // run automated checks against the real game systems, then quit
  float autowalk = 0.0f;  // test aid: walk forward for N seconds
  std::string walk;       // test aid: scripted legs "yaw_deg:seconds,yaw_deg:seconds,..."
  int entrance = -1;      // --state interior: which entrance to use (-1 = nearest to spawn)
  std::string weather;    // force a weather state (clear, fair, thin_cloud, overcast, light_rain, rain, heavy_rain, thunder, fog, windy)
  bool dev = false;       // start with the developer overlay
  std::string drive;      // test aid (--state drive): "throttle:steer:seconds,..." (negative throttle = brake)
  int station = -1;       // test aid (--state ride): stand on this station's platform and board the next train
  float ride = 0.0f;      // test aid: ride this many seconds before the screenshot
  bool alight = false;    // test aid: after --ride, alight at the next stop
  int sim_speed = 1;      // test aid: transport simulation steps per frame (trains, ferries, aircraft)
  std::string fly_script; // test aid (--state fly): "throttle:elevator:aileron:seconds,..."
  std::string audio_wav;  // test aid: render the game's sound offline into this WAV file
};

class App {
 public:
  explicit App(LaunchOptions o) : opt_(std::move(o)) {}
  int run();
  // Android: the system may end a paused app without warning, so pausing saves the autosave slot.
  void autosave();

 private:
  enum class Screen { Boot, Loading, Title, Settings, Slots, Credits, Game, Pause, Phone, Fatal };
  enum class PhoneApp { Home, Map, Clock, Wallet, Town, Work, Hobby, Bag, Flat };

  bool boot();
  void shutdown();
  void update(float dt);
  void draw();

  void drawWorldView(const Camera3D& cam);
  void drawBootScreen();
  void drawLoading();
  void drawTitle();
  void drawSettings();
  void drawSlots();
  void drawCredits();
  void drawHud();
  void drawBuildingInfo();
  void drawWalkerInfo();
  void drawPause();
  void drawPhone();
  void drawMap(Rectangle r, double half_extent_m, bool labels);
  void drawFatal();
  void drawToast(float dt);

  void newGame();
  bool loadSlot(int slot);
  bool saveSlot(int slot);
  void endSession();
  void setLanguage(const std::string& code);
  void applyWindowMode();
  void saveSettings();
  void toast(const std::string& msg);
  void takeUserScreenshot();
  void applyLaunchOverrides();
  void runSelfTest();
  int exit_code_ = 0;

  rj::sim::CivilDateTime jst() const { return clock_.jst(); }
  std::string dateTimeString() const;
  std::string tr(const std::string& k) const { return i18n_.tr(k); }
  Camera3D titleCamera() const;

  LaunchOptions opt_;
  Settings settings_;
  std::string saved_world_;  // world as stored in settings.ini (before a --world override)
  I18n i18n_;
  Ui ui_;
  World world_;
  Renderer renderer_;
  Player player_;
  rj::sim::GameClock clock_{0};
  std::unique_ptr<rj::econ::Ledger> ledger_;
  rj::econ::AccountId player_account_ = 0;
  TownSim town_;
  Pedestrians peds_;
  const Walker* hover_walker_ = nullptr;
  std::string inside_id_;  // interior the player is in (empty = outside)
  std::string prompt_;     // context action shown on the HUD (E key)
  const Interior* insideInterior() const { return inside_id_.empty() ? nullptr : world_.interior(inside_id_); }
  float underground_ = 0.0f;  // 0 = eye at/above the street surface, 1 = fully underground (lighting blend)
  rj::geo::Vec3d walk_start_;
  std::vector<std::pair<float, float>> walk_legs_;  // scripted walk (yaw_deg, seconds), test aid
  void updateInteriorAction();
  void drawInteriorInfo();
  std::filesystem::path slice_dir_;

  Screen screen_ = Screen::Boot;
  Screen settings_return_ = Screen::Title;
  Screen slots_return_ = Screen::Title;
  bool slots_saving_ = false;
  PhoneApp phone_app_ = PhoneApp::Home;
  bool in_session_ = false;
  bool cursor_locked_ = false;
  double play_seconds_ = 0;
  double autosave_timer_ = 0;
  std::string toast_;
  float toast_t_ = 0;
  float title_t_ = 0;
  float session_t_ = 0;
  std::string fatal_;
  int frame_ = 0;
  int shot_frames_ = -1;
  int shot_settle_frames_ = 0;    // --screenshot with --pos: frames without pending cell loads
  bool shot_alt_fixed_ = false;   // --fly --alt applied above the ground once it has loaded
  std::optional<World::Hit> hover_;
  bool quit_ = false;
  double map_half_extent_ = 350.0;
  Lighting lighting_;
  WeatherSim weather_;
  FacadeDetail facades_;
  TrafficSignals signals_;
  Traffic traffic_;
  RoadMarkings markings_;
  Trains trains_;
  Shops shops_;
  int shop_open_ = -1;                     // the shop whose counter menu is open
  std::map<std::string, int> inventory_;  // bought in shops: item key -> count
  struct TollPlaza {
    std::string name;
    rj::geo::Geodetic geo;
    double heading = 0, width = 8;
    rj::geo::Vec3d pos;
    float bar[2] = {0.0f, 0.0f};  // ETC bars of its two lanes, 0 down .. 1 up
    bool blocked = false;          // the player's card could not pay here: the bar stays down
  };
  // the ETC bars: up for a car coming through the lane, down across it otherwise (app_shop.cpp)
  struct TollLane {
    rj::geo::Vec3d pivot, tip;
    double arm_hd = 0;  // compass direction from pivot to tip
    double along = 0;   // (travel direction, degrees)
  };
  TollLane tollLane(const TollPlaza& t, int lane) const;
  void updateTollBars(float dt, std::vector<float>& walls);
  void drawTollBars(const Camera3D& cam);
  std::vector<TollPlaza> tolls_;  // expressway toll plazas (tolls.txt)
  int toll_entry_ = -1;           // the plaza the player's car came onto the expressway through
  int toll_last_ = -1;            // the plaza just passed (until the car is clear of it)
  Crowd crowd_;  // passengers in the cars and on the platforms near the camera
  int ride_train_ = -1, ride_car_ = 0;  // riding a train (id, car)
  int ride_board_station_ = -1;         // where the ride began (fare by distance at the end)
  FarView far_;                         // the country beyond the streamed cells (fictional world)
  NearTrees near_trees_;                // single forest trees near the camera (fictional world)
  bool near_trees_low_ = true;          // the camera near the ground (single trees drawn)
  void updateFarMapping();
  void updateSeason();
  float snowfall_ = 0.0f;  // 0 rain .. 1 snow (updateSeason)
  float ride_look_yaw_ = 0.0f, ride_look_pitch_ = 0.0f;  // view relative to the car ridden (its model frame)
  void updateTransportActions();
  Camera3D rideCamera() const;
  // --- on foot in the stations and the trains (app_transit.cpp) ---
  float ob_x_ = 0.0f, ob_y_ = 0.0f;  // where the player stands in the car ridden (model frame)
  int ob_seat_ = -1;                 // the seat sat on / being sat on (car_layout.hpp order)
  bool ob_sitting_ = false;
  float ob_sit_ = 0.0f;              // 0 standing .. 1 seated (the sit-down / stand-up movement)
  float ob_bob_ = 0.0f;
  int ob_prev_train_ = -1;           // (fare distance bookkeeping)
  double ob_prev_s_ = 0.0;
  std::vector<std::pair<float, float>> ob_path_;  // test aid: points to walk to in the car
  bool ob_test_seated_ = false, ob_test_looked_ = false;
  float ob_test_look_[2] = {0.0f, -0.05f};
  int aim_seat_ = -1;
  enum class AimIcon { None, Seat, Blocked, Stand, Enter, Hand };
  AimIcon aim_icon_ = AimIcon::None;  // what the crosshair offers (E / click), drawn at its side
  std::string aim_label_;
  std::string ride_info_;            // line, next station and speed while riding (a quiet line at the top)
  bool in_paid_ = false;             // inside the ticket gates (touched in)
  double paid_km_ = 0.0;             // ridden since touching in
  bool paid_shink_ = false;
  bool gate_owe_ = false;            // the exit gate shut once for a short card: let through next time
  std::vector<float> gate_walls_;    // shut gate flaps (collision, origin ENU)
  std::vector<float> crossing_walls_;  // lowered level crossing barrier arms (collision, origin ENU)
  int gate_in_ = -1, gate_lane_in_ = -1;
  int gate_closed_gate_ = -1, gate_closed_lane_ = -1;
  float gate_closed_t_ = 0.0f;
  int gate_flash_gate_ = -1, gate_flash_lane_ = -1;
  float gate_flash_t_ = 0.0f;
  bool gate_flash_ok_ = true;
  // ferry: the gangway (0 on deck .. 1 on the pier, -1 off it), the benches
  double ferry_gang_ = -1.0;
  bool ferry_paid_ = false;
  int ferry_seat_ = -1;
  bool ferry_sitting_ = false;
  float ferry_sit_ = 0.0f;
  bool ferry_test_walked_ = false;
  // airliner: the passenger stairs (0 at the door .. 1 on the apron, -1 off them), the cabin
  float jet_x_ = 0.0f, jet_y_ = 0.0f;  // standing in the cabin (model frame)
  double jet_stair_ = -1.0;
  int jet_seat_ = -1;
  bool jet_sitting_ = false;
  float jet_sit_ = 0.0f;
  bool jet_paid_ = false;
  std::vector<std::pair<float, float>> jet_path_;  // test aid
  bool jet_test_seated_ = false, jet_test_looked_ = false;
  bool ferryGangway(const struct Ferry& f, rj::geo::Vec3d& deck_end, rj::geo::Vec3d& pier_end, int& side, float& gy) const;
  void updateFerryAboard(float dt);
  void updateJetAboard(float dt);
  rj::geo::Vec3d jetToWorld(const struct Airliner& a, float x, float y, float z) const;
  void jetTestPilot();
  int aimSeat(const std::vector<struct SeatSlot>& seats, float px, float py, float look_yaw, float look_pitch, float eye_h, float reach) const;
  bool aimAt(const rj::geo::Vec3d& target, double max_dist, double max_deg) const;  // the crosshair is on target
  bool usePressed() const;
  void enterCar(int train, int car, float x, float y);
  void leaveCar(const rj::geo::Vec3d& w);
  void standInCar(int car);
  void updateOnBoard(float dt);
  void updateSeatChoice();
  void updateBoarding();
  void updateStationGates(float dt);
  void updateCrossingSafety();  // lowered barriers as walls; the player on the tracks stops trains
  void rideTestPilot();
  std::string lineName(int line) const;
  static int64_t railFare(bool shinkansen, double km);  // game fare (yen) for a ride of km
  std::string airportName(int i) const;
  Aviation aviation_;
  int ride_jet_ = -1;                    // aboard this scheduled flight (id)
  bool jet_flown_ = false;               // the flight has left the stand (alighting then ends the trip)
  float jet_look_yaw_ = -1.25f, jet_look_pitch_ = -0.12f;
  bool flying_ = false, fly_cockpit_ = false;
  float fly_look_yaw_ = 0.0f, fly_look_pitch_ = 0.0f;
  PlaneControls plane_in_;
  float crash_t_ = -1.0f;
  struct FlyLeg {
    float throttle, elevator, aileron, seconds;
  };
  std::vector<FlyLeg> fly_legs_;
  bool fly_test_pending_ = false;
  bool ride_place_pending_ = false;  // --state ride: stand on the platform once the stations are placed
  void placeRideTest();
  void updateAviationActions();
  Camera3D jetCamera() const;
  // --- work & hobbies (app_activities.cpp) ---
  Jobs jobs_;
  int drive_train_ = -1;  // the train the player drives (also ride_train_)
  int train_stops_ = 0;
  int64_t train_pay_ = 0;
  std::string train_msg_;
  float train_msg_t_ = 0;
  struct Till {
    bool on = false;
    int stage = 0;  // 0 scanning, 1 change
    std::vector<int> items;  // catalogue indices
    std::vector<bool> scanned;
    bool age_checked = false;
    int64_t tendered = 0, entered = 0;
    int served = 0, mistakes = 0;
    int64_t pay = 0;
    std::string msg;
    float msg_t = 0;
    uint32_t rng = 12345u;
  } till_;
  struct Fishing {
    int stage = 0;  // 0 idle, 1 waiting, 2 bite, 3 reeling
    double t = 0, wait = 0, tension = 0, dist = 0, strength = 1;
    int species = -1;
    float size = 0;
    rj::geo::Vec3d bobber;
    uint32_t rng = 777u;
  } fish_;
  std::map<std::string, float> fish_log_;  // species key -> best size (cm)
  int fish_count_ = 0;
  bool photo_mode_ = false, photo_request_ = false;
  float photo_fov_ = 0;
  std::set<std::string> photo_spots_;
  int photos_taken_ = 0;
  struct Worship {
    int stage = 0;  // 0 idle, 1.. sequence
    double t = 0;
    bool temple = false;
    std::string place;
    float pitch0 = 0;
  } worship_;
  std::string last_omikuji_;
  std::set<std::string> goshuin_;
  int worship_count_ = 0;
  float phone_scroll_ = 0;
  void updateActivities(float dt);
  void drawActivityHud();
  void drawPhoneWork(float cx, float yy, float cw, float bottom);
  void drawPhoneHobby(float cx, float yy, float cw, float bottom);
  void drawWorldMarkers(const Camera3D& cam);
  bool waterAhead(double& sea_z, rj::geo::Vec3d& spot) const;
  void startTrainDriving();
  void updateTrainDriving();
  Camera3D cabCamera() const;
  void tillNextCustomer();
  void takePhoto();
  Ferries ferries_;
  int ride_ferry_ = -1;                  // aboard this ferry (id)
  double ferry_x_ = 0, ferry_y_ = 0;     // position on its open deck (ship frame)
  float ferry_look_yaw_ = 0.0f;          // view relative to the ship's heading
  bool ferry_test_pending_ = false;      // --state ferry: put the player beside the docked ship
  void updateFerryActions();
  std::string pierName(int pier) const;
  Driving driving_;
  float drive_look_yaw_ = 0.0f, drive_look_pitch_ = 0.0f;
  bool drive_first_person_ = false;
  void updateDriveActions();
  void updateShopActions();  // at a shop counter: buy (menu)
  void loadTolls(const std::filesystem::path& file);
  void updateTolls();        // the player's car through a toll plaza: ETC entry / fare at the exit
  void drawShopMenu();
  void drawShopClerks(const Camera3D& cam);
  int shop_greeted_ = -1;  // the counter whose clerk said hello
  bool tsuyu_ = false, tsuyu_seen_ = false;
  // talking to people in the street (app_talk.cpp)
  void updateTalk(float dt);
  void startTalk(const Walker& w);
  void drawTalk();
  float talk_t_ = 0.0f;
  rj::geo::Vec3d talk_at_{};
  std::string talk_speaker_;
  std::vector<std::string> talk_lines_;
  std::map<size_t, int> talked_;  // walker -> times talked (this session)  // the rainy season (for its start / end message)
  // what the controller's (and on touch builds the on-screen) buttons mean now (app_input.cpp)
  void updateInputContext();
  int shrine_frame_ = -10;  // last frame the shrine prompt was up (its extra buttons)
  bool buyItem(const ShopItem& it);
  // the body and belongings (app_life.cpp): hunger, thirst, rain on the clothes, using things bought
  struct Life {
    float hunger = 80.0f, thirst = 80.0f;  // 0..100 (game values)
    float wet = 0.0f;                      // clothes 0 dry .. 1 soaked
    bool umbrella = false, flashlight = false;
    float battery = 1.0f;                  // the flashlight's
    float fuel = 0.7f, damage = 0.0f;      // the car being driven (tank 0..1, 0 as new .. 1 wrecked)
    bool has_home = false;                 // renting the flat (app_home.cpp)
    int64_t rent_paid_until = 0;           // game unix time
    int talks = 0;                         // conversations had
    int64_t last_unix = 0;                 // (game time of the last update)
  };
  Life life_;
  std::vector<std::string> notes_;  // the notebook
  std::string lifeString() const;
  void parseLife(const std::string& s);
  std::string notesString() const;
  void parseNotes(const std::string& s);
  void updateLife(float dt);
  bool canUseItem(const std::string& key) const;
  bool useItem(const std::string& key);
  void drawBag(float x, float y, float w);
  void drawLifeHud();
  // the car being driven: fuel, damage, fuel stations, road service (app_car.cpp)
  struct FuelStation {
    std::string name;
    rj::geo::Geodetic geo;
    rj::geo::Vec3d pos;
  };
  std::vector<FuelStation> fuel_stations_;
  int fuel_open_ = -1;     // the station whose menu is open
  int fuel_car_id_ = -1;   // the car the fuel level belongs to
  void loadFuelStations(const std::filesystem::path& file);
  void onEnterCar(const Vehicle& v);
  void updateCar(float dt);
  void updateFuelStation();
  bool nearFuelStation() const;
  bool payCar(int64_t yen, const std::string& memo);
  int64_t fuelCost() const;
  int64_t repairCost() const;
  void drawFuelMenu();
  void drawCarPhone(float x, float& y, float w);
  // the player's flat (app_home.cpp): rented from the phone, the front door locked until then, the
  // bed sleeps the night through
  struct HomeSpot {
    bool ok = false;
    rj::geo::Geodetic bed_geo, stand_geo, door_a_geo, door_b_geo;
    rj::geo::Vec3d bed, stand, door_a, door_b;
  };
  HomeSpot home_;
  float home_door_open_ = 0.0f;  // the door leaf 0 shut .. 1 open
  int64_t slept_until_ = 0;      // (last sleep: the wake-up message)
  void loadHome(const std::filesystem::path& file);
  void placeHome();
  bool homeLocked() const { return home_.ok && !life_.has_home; }
  bool playerOutsideHome() const;
  void addHomeDoorWall(std::vector<float>& walls) const;
  void updateHome(float dt);
  bool rentHome();
  void sleepAtHome();
  void drawHomeDoor(const Camera3D& cam);
  void drawPhoneFlat(float cx, float yy, float cw);
  void drawHomeOnMap(const std::function<Vector2(const rj::geo::Vec3d&)>& toScreen, double half);
  std::string inventoryString() const;
  void parseInventory(const std::string& s);
  // test aids (scripted drive / ride); the screenshot waits until they are finished
  struct DriveLeg {
    float throttle, steer, seconds;
  };
  std::vector<DriveLeg> drive_legs_;
  bool drive_spawn_pending_ = false;  // --state drive: put the car on the nearest lane once roads are placed
  float ride_test_t_ = -1.0f;  // >= 0: ride test running (seconds since boarding)
  bool ride_test_done_ = false;
  void updateRideTest();
  bool scriptBusy() const;
  int simSteps() const;  // transport simulation steps this frame (test aid --simspeed)
  bool traffic_placed_ = false;
  void placeRoads();  // road graph, signal groups, markings and the walk network in the current origin
  rj::geo::Vec3d roads_center_{1e30, 1e30, 0};  // where markings / walk network were last built (large worlds)
  int64_t weather_prev_unix_ = 0;
  float render_time_ = 0.0f;
  std::vector<PointLight> collectLights(const Camera3D& cam) const;

  // sound (app_sound.cpp): the scene heard at the camera, and events detected from the simulation
  Audio audio_;
  Camera3D listen_cam_{};
  bool listen_game_ = false;
  void updateSound(float dt);
  void updateDepartureBoards();  // LED boards on the platforms near the camera (app_sound.cpp)
  float boards_t_ = 0.0f;
  rj::geo::Vec3d snd_ear_prev_{};
  bool snd_ear_ok_ = false;
  int snd_car_keys_[4] = {-1, -1, -1, -1};
  int snd_rail_keys_[2] = {-1, -1};
  std::map<int, double> snd_dwell_;      // train id -> remaining stop time last frame
  std::map<int, int> snd_at_;            // train id -> station it stood at last frame
  std::map<int, double> snd_train_v_;    // train id -> speed last frame
  std::map<int, float> snd_train_acc_;   // train id -> smoothed acceleration
  std::map<int, int> snd_ferry_phase_;
  int snd_jet_phase_ = -1;
  float snd_jet_gear_ = 1.0f;
  float snd_chime_t_ = -1.0f;            // on-board chime countdown after leaving a station
  float snd_horn_t_ = 0.0f;
  bool snd_crashed_ = false;
  float snd_sea_ = 0.0f;
  float snd_bell_t_ = 0.0f;               // level crossing bell stroke timer
};

}  // namespace rjc
