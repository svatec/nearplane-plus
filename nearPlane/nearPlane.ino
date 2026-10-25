// nearPlane+ - ADS-B tracker for the M5StickC Plus2
// Optional: GPS module (M5Stack GPS Unit v1.1, AT6668) on the Grove port gives the live position.
// Features: multiple aircraft (BtnB short press), CPA, alerts (MIL/EMERG/RARE), radar map,
//           daily statistics (NVS), aircraft type and airline names.
//
// Controls:
//   BtnA (front)         = next page (on the map: long press = detail of selected aircraft)
//   BtnB short press     = next aircraft
//   BtnB hold 5 s        = reset settings
//   BtnC (PWR, side)     = short press: change scale on the radar map page
//                          hold 0.75 s: buzzer on/off (saved)

#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include <esp_system.h>

// Forward declarations: the Arduino IDE generates function prototypes above the first
// function (including the SET_LOOP_TASK_STACK_SIZE macro), i.e. before the type definitions.
struct KV;
struct Aircraft;
struct Rel;
struct Blip;
struct DayStats;
class StreamReader;

#ifdef SET_LOOP_TASK_STACK_SIZE
SET_LOOP_TASK_STACK_SIZE(16 * 1024);   // TLS handshake + JSON need more than the default 8 kB
#endif

Preferences preferences;
WebServer server(80);
DNSServer dnsServer;
M5Canvas canvas(&M5.Display);

// Default User-Agent, used only when none is saved in the setup form.
// adsb.lol requires a User-Agent with real contact info (e-mail or URL).
const char* USER_AGENT = "nearPlane-M5Stick/1.0 (changeme@example.com)";
String userAgent = USER_AGENT;       // active value (from setup form / NVS key "ua")

// ---------------------------------------------------------------- pages
const int PAGE_TRACK = 6;   // arrow + distance + CPA
const int PAGE_MAP   = 7;   // radar map
const int PAGE_LIST  = 8;   // list of nearest aircraft
const int PAGE_STATS = 9;   // daily statistics
const int NUM_PAGES  = 10;

// ---------------------------------------------------------------- polling
const unsigned long SHORT_POLL_INTERVAL = 15000;        // aircraft found: 15 s
const unsigned long NO_AIRCRAFT_POLL_INTERVAL = 30000;  // no aircraft: 30 s
const unsigned long ERROR_POLL_INTERVAL = 60000;        // other error: 60 s
const unsigned long RATE_LIMIT_BACKOFF_MIN = 20000;     // after HTTP 429: 20 s, doubling...
const unsigned long RATE_LIMIT_BACKOFF_MAX = 120000;    // ...up to 120 s (Retry-After header wins)
unsigned long pollInterval = SHORT_POLL_INTERVAL;
const long RESET_HOLD_TIME_MS = 5000;
const long BTN_B_HOLD_MS = 1000;   // from this press length the reset countdown is shown

// ---------------------------------------------------------------- configuration
bool configMode = false;
String wifi_ssid, wifi_password, latitude, longitude, radius_km;
String api_url;
double homeLat = 0, homeLon = 0;
float radiusNm = 50;
unsigned long lastPollTime = 0;
int currentPage = 0;

unsigned long btnB_press_start_time = 0;
bool btnBDown = false;
bool isResetting = false;
unsigned long lastResetDraw = 0;

unsigned long lastActivityTime = 0;
const unsigned long INACTIVITY_TIMEOUT_MS = 300000;
const uint64_t DEEP_SLEEP_DURATION_S = 60;

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3 * 3600;
const int daylightOffset_sec = 0;

// ---------------------------------------------------------------- data types
struct Aircraft {
  char flight[9], reg[12], type[6], hex[8], squawk[6], emergency[14], category[4];
  char navModes[24], desc[26], dep[5], arr[5];
  bool routeTried, onGround, hasPos, hasTrack, hasHdg;
  uint8_t level;          // 0 = none, 1 = rare type, 2 = military, 3 = emergency
  uint16_t dbFlags;
  uint32_t hex32;
  double lat, lon, distNm, bearing;
  float gs, track, hdg, roll, mach, navHdg, rssi;
  int altBaro, altGeom, baroRate, ias, tas, navAlt, tat, oat, wd, ws;
};

struct Blip {             // lightweight version for the radar map
  uint32_t hex32;
  double lat, lon;
  float track, gs;
  bool hasTrack;
  uint8_t level;
};

struct Rel {              // aircraft position relative to us
  double dx, dy, dist, bearing;   // NM east / north, NM, degrees
};

struct DayStats {
  uint32_t day;           // yyyymmdd
  uint16_t count;
  float minKm;
  char minFl[9];
  int maxAlt;
  char altFl[9];
  int maxGs;
  char gsFl[9];
};

struct KV { const char* k; const char* v; };

// Block-wise HTTP stream reader (also supports Transfer-Encoding: chunked)
class StreamReader {
public:
  StreamReader(Stream& s, HTTPClient& h, bool chunked) : _s(s), _h(h), _chunked(chunked) {}
  int next() {
    if (!_chunked) return rawNext();
    if (_chunkLeft == 0) {
      if (_started) { rawNext(); rawNext(); }   // CRLF after the previous chunk
      _started = true;
      long size = 0;
      bool any = false, ext = false;
      int c;
      while ((c = rawNext()) >= 0) {
        if (c == '\n') { if (any) break; else continue; }
        if (c == '\r' || ext) continue;
        if (c == ';') { ext = true; continue; }
        int v = (c >= '0' && c <= '9') ? c - '0'
              : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (v >= 0) { size = size * 16 + v; any = true; }
      }
      if (c < 0 || size == 0) return -1;      // end of data
      _chunkLeft = size;
    }
    _chunkLeft--;
    return rawNext();
  }
private:
  int rawNext() {
    if (_pos >= _len) {
      _len = _pos = 0;
      unsigned long t0 = millis();
      while (true) {
        int a = _s.available();
        if (a > 0) {
          _len = _s.readBytes(_buf, a > (int)sizeof(_buf) ? (int)sizeof(_buf) : a);
          break;
        }
        if (!_h.connected() || millis() - t0 > 8000) return -1;
        delay(2);
      }
      if (_len <= 0) return -1;
    }
    return _buf[_pos++];
  }
  Stream& _s;
  HTTPClient& _h;
  bool _chunked;
  bool _started = false;
  long _chunkLeft = 0;
  uint8_t _buf[256];
  int _len = 0, _pos = 0;
};

// ---------------------------------------------------------------- global data
const int MAX_AC = 5;
const int MAX_BLIPS = 40;
const int SEEN_MAX = 256;

Aircraft ac[MAX_AC];
int acCount = 0;
int sel = 0;
unsigned long fixTime = 0;        // time of the last successful data load

Aircraft cand[MAX_AC];
float candKey[MAX_AC];
int candCount = 0;
Aircraft tmpA;

Blip blips[MAX_BLIPS];
int blipCount = 0;
Blip blipNext[MAX_BLIPS];
float blipKey[MAX_BLIPS];
int blipNextCount = 0;

JsonDocument objFilter;

DayStats stats;
uint32_t seenHex[SEEN_MAX];
int seenN = 0;
bool statsDirty = false;
bool statsActive = false;
unsigned long lastStatsSave = 0;

uint32_t lastNearestHex = 0;
uint32_t alertedHex[8] = {0};
int alertedIdx = 0;
unsigned long lastRedraw = 0;
bool routePending = false;          // route is fetched in loop(), outside the first TLS connection
bool firstPoll = true;               // first fetch right after connecting
unsigned long lastBeat = 0;
int scanObjs = 0, scanParsed = 0, scanNoPos = 0;
String scanLastErr = "";
uint32_t uiFg = TFT_WHITE;           // battery/clock colour (changes on alert banners)
bool marqueeActive = false;          // long "airline | type" line is scrolling
unsigned long lastMarquee = 0;
String api_url_alt;                  // fallback server (same JSON format)
unsigned long primaryBlockedUntil = 0;   // primary server rate-limited until this time
int consecutive429 = 0;
bool rateLimited = false;            // last request was rate limited (shows a status screen)
// ---- GPS (optional): M5Stack GPS Unit v1.1 (AT6668 + MAX2659) on the Grove port
HardwareSerial GPSSerial(1);
bool gpsEnabled = true;              // setting "Use GPS module if connected" (NVS key "gps")
bool gpsDetected = false;            // valid NMEA sentences are arriving
bool gpsEverFix = false;             // at least one valid position received
bool gpsGaveUp = false;              // no module found after several detection rounds
double gpsLat = 0, gpsLon = 0;
int gpsSats = 0;
unsigned long gpsLastFixMs = 0, gpsLastSeenMs = 0, gpsComboStart = 0;
int gpsCombo = 0, gpsCombosTried = 0;
char nmeaBuf[128];
int nmeaLen = 0;
double cfgLat = 0, cfgLon = 0;       // position saved in the setup form
bool posFromGps = false;             // current home position comes from the GPS
unsigned long lastHomeUpdate = 0;
const int GPS_PINS[2][2] = {{33, 32}, {32, 33}};   // {RX, TX}: both Grove wire orientations
const long GPS_BAUDS[2] = {115200, 9600};          // v1.1 (AT6668) = 115200, v1 (AT6558) = 9600
bool soundOn = true;                 // buzzer enabled (persisted in NVS)
bool btnCDown = false, btnCToggled = false;
unsigned long btnCStart = 0;
const unsigned long BTN_C_HOLD_MS = 750;   // hold BtnC this long to toggle the buzzer
unsigned long toastUntil = 0;        // on-screen message visible until this time
int mapScaleIdx = 0;                 // 0 = full configured radius, 1..3 = 25 / 12.5 / 7.5 km
bool btnADown = false, btnALong = false;
unsigned long btnAStart = 0;
const unsigned long BTN_A_LONG_MS = 600;

// ---------------------------------------------------------------- lookup tables
const KV TYPE_NAMES[] = {
  {"A19N","Airbus A319neo"},{"A20N","Airbus A320neo"},{"A21N","Airbus A321neo"},
  {"A318","Airbus A318"},{"A319","Airbus A319"},{"A320","Airbus A320"},{"A321","Airbus A321"},
  {"A306","Airbus A300-600"},{"A310","Airbus A310"},{"A332","Airbus A330-200"},
  {"A333","Airbus A330-300"},{"A339","Airbus A330-900"},{"A342","Airbus A340-200"},
  {"A343","Airbus A340-300"},{"A346","Airbus A340-600"},{"A359","Airbus A350-900"},
  {"A35K","Airbus A350-1000"},{"A388","Airbus A380-800"},{"A400","Airbus A400M"},
  {"A3ST","Airbus Beluga"},{"A337","Airbus BelugaXL"},{"BCS1","Airbus A220-100"},
  {"BCS3","Airbus A220-300"},
  {"B712","Boeing 717"},{"B733","Boeing 737-300"},{"B734","Boeing 737-400"},
  {"B735","Boeing 737-500"},{"B736","Boeing 737-600"},{"B737","Boeing 737-700"},
  {"B738","Boeing 737-800"},{"B739","Boeing 737-900"},{"B37M","Boeing 737 MAX 7"},
  {"B38M","Boeing 737 MAX 8"},{"B39M","Boeing 737 MAX 9"},{"B744","Boeing 747-400"},
  {"B748","Boeing 747-8"},{"B74S","Boeing 747SP"},{"B752","Boeing 757-200"},
  {"B753","Boeing 757-300"},{"B762","Boeing 767-200"},{"B763","Boeing 767-300"},
  {"B764","Boeing 767-400"},{"B772","Boeing 777-200"},{"B77L","Boeing 777-200LR"},
  {"B773","Boeing 777-300"},{"B77W","Boeing 777-300ER"},{"B788","Boeing 787-8"},
  {"B789","Boeing 787-9"},{"B78X","Boeing 787-10"},{"BLCF","Boeing Dreamlifter"},
  {"E170","Embraer 170"},{"E75L","Embraer 175"},{"E75S","Embraer 175"},
  {"E190","Embraer 190"},{"E195","Embraer 195"},{"E290","Embraer E190-E2"},
  {"E295","Embraer E195-E2"},{"E145","Embraer 145"},
  {"CRJ2","CRJ-200"},{"CRJ7","CRJ-700"},{"CRJ9","CRJ-900"},{"CRJX","CRJ-1000"},
  {"AT72","ATR 72"},{"AT75","ATR 72-500"},{"AT76","ATR 72-600"},{"AT45","ATR 42-500"},
  {"DH8D","Dash 8 Q400"},{"DH8C","Dash 8-300"},{"SF34","Saab 340"},{"SU95","Sukhoi Superjet"},
  {"A124","Antonov An-124"},{"A225","Antonov An-225"},{"IL76","Ilyushin Il-76"},
  {"C130","C-130 Hercules"},{"C17","C-17 Globemaster"},{"C5M","C-5M Galaxy"},{"K35R","KC-135"},
  {"C172","Cessna 172"},{"C25A","Cessna CJ2"},{"C25B","Cessna CJ3"},{"C510","Cessna Mustang"},
  {"C525","Cessna CitationJet"},{"C550","Cessna Citation II"},{"C56X","Cessna Excel"},
  {"C680","Cessna Sovereign"},{"C750","Cessna Citation X"},{"PC12","Pilatus PC-12"},
  {"PC24","Pilatus PC-24"},{"TBM9","TBM 900"},{"SR22","Cirrus SR22"},{"PA28","Piper PA-28"},
  {"F900","Falcon 900"},{"F2TH","Falcon 2000"},{"FA7X","Falcon 7X"},{"FA8X","Falcon 8X"},
  {"GLF4","Gulfstream IV"},{"GLF5","Gulfstream V"},{"GLF6","Gulfstream G650"},
  {"GLEX","Global Express"},{"GL5T","Global 5000"},{"GL7T","Global 7500"},
  {"LJ45","Learjet 45"},{"H25B","Hawker 800"},{"E55P","Phenom 300"},{"E50P","Phenom 100"},
  {"R44","Robinson R44"},{"EC35","Airbus H135"},{"EC45","Airbus H145"},
  {"H60","UH-60 Black Hawk"},{"B06","Bell 206"}
};

const KV AIRLINES[] = {
  {"RYR","Ryanair"},{"WZZ","Wizz Air"},{"AUA","Austrian"},{"DLH","Lufthansa"},
  {"SWR","Swiss"},{"BAW","British Airways"},{"AFR","Air France"},{"KLM","KLM"},
  {"EZY","easyJet"},{"EJU","easyJet Europe"},{"EZS","easyJet Switzerland"},
  {"VLG","Vueling"},{"IBE","Iberia"},{"TAP","TAP Portugal"},{"LOT","LOT Polish"},
  {"CSA","Czech Airlines"},{"SAS","SAS"},{"FIN","Finnair"},{"THY","Turkish"},
  {"PGT","Pegasus"},{"UAE","Emirates"},{"QTR","Qatar Airways"},{"ETD","Etihad"},
  {"UAL","United"},{"DAL","Delta"},{"AAL","American"},{"ACA","Air Canada"},
  {"ELY","El Al"},{"AEE","Aegean"},{"ROT","Tarom"},{"TVS","Smartwings"},
  {"TVQ","Smartwings SK"},{"SXS","SunExpress"},{"NOZ","Norwegian"},{"BEL","Brussels"},
  {"EWG","Eurowings"},{"CFG","Condor"},{"TOM","TUI Airways"},{"TUI","TUI fly"},
  {"LGL","Luxair"},{"ICE","Icelandair"},{"ITY","ITA Airways"},{"AZA","Alitalia"},
  {"GEC","Lufthansa Cargo"},{"FDX","FedEx"},{"UPS","UPS"},{"BOX","AeroLogic"},
  {"DHK","DHL"},{"DHX","DHL"},{"CLX","Cargolux"},{"NJE","NetJets Europe"},
  {"ASL","Air Serbia"},{"AHY","Azerbaijan"},{"AIC","Air India"},{"SIA","Singapore"},
  {"CPA","Cathay Pacific"},{"ANA","ANA"},{"JAL","Japan Airlines"},{"KAL","Korean Air"},
  {"CCA","Air China"},{"CES","China Eastern"},{"CSN","China Southern"},
  {"ETH","Ethiopian"},{"MSR","EgyptAir"},{"RAM","Royal Air Maroc"},{"DAH","Air Algerie"},
  {"TAR","Tunisair"},{"BTI","airBaltic"},{"CTN","Croatia Airlines"},{"ADR","Adria"},
  {"BCS","European Air Transport"},{"SQP","Skyalps"},{"HVN","Vietnam Airlines"},
  {"RCH","USAF Air Mobility"},{"NATO","NATO"}
};

const char* RARE_TYPES[] = {
  "A388","B744","B748","B74S","A124","A225","BLCF","A3ST","A337","C5M","IL76","A346"
};

#define KV_COUNT(x) (sizeof(x) / sizeof(x[0]))

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width, initial-scale=1"><title>nearPlane+ Tracker Setup</title><style>body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;margin:0;background-color:#f5f5f7;color:#1d1d1f}.container{padding:25px;max-width:550px;margin:30px auto;background-color:#fff;border-radius:12px;box-shadow:0 4px 12px rgba(0,0,0,.1)}h1{color:#1d1d1f;text-align:center;margin-bottom:25px;font-weight:600}form{display:flex;flex-direction:column}label{margin-bottom:8px;color:#6e6e73;font-weight:500}input{padding:14px;margin-bottom:20px;border:1px solid #d2d2d7;border-radius:8px;font-size:16px;transition:border-color .2s,box-shadow .2s}input:focus{border-color:#007aff;box-shadow:0 0 0 3px rgba(0,122,255,.25);outline:none}button{background-color:#007aff;color:#fff;padding:16px;border:none;border-radius:8px;font-size:16px;font-weight:600;cursor:pointer;transition:background-color .2s}button:hover{background-color:#0056b3}.footer{text-align:center;margin-top:20px;color:#86868b;font-size:12px}</style></head><body><div class="container"><h1>nearPlane+ ADSB Tracker Setup</h1><p style="text-align:center;color:#6e6e73;margin:0 0 20px">%GPSMSG%</p><form action="/save" method="POST"><label for="ssid">WiFi Network (SSID)</label><input type="text" id="ssid" name="ssid" required><label for="password">WiFi Password</label><input type="text" id="password" name="password"><label for="lat">Your Latitude</label><input type="text" id="lat" name="lat" required placeholder="e.g., 41.015137" value="%LAT%"><label for="lon">Your Longitude</label><input type="text" id="lon" name="lon" required placeholder="e.g., 28.979530" value="%LON%"><label for="radius">Scan Radius (NM, max 250)</label><input type="number" id="radius" name="radius" value="50" min="1" max="250" required><label for="ua">User-Agent (must contain your e-mail or URL)</label><input type="text" id="ua" name="user_agent" value="nearPlane-M5Stick/1.0 (CHANGE-ME@example.com)" maxlength="120" pattern=".*(@|https?://).*" title="Include your e-mail or a URL" required><label style="display:flex;align-items:center;margin-bottom:20px"><input type="checkbox" name="gps" value="1" style="width:auto;margin:0 10px 0 0" %GPSCHK%>Use GPS module if connected (live position)</label><button type="submit">Save & Reboot</button></form></div><div class="footer">nearPlane+ - M5StickC Plus 2 Edition</div></body></html>
)rawliteral";

// ---------------------------------------------------------------- prototypes
void handleRoot();
void handleSave();
void handleNotFound();
void displayCurrentPage();
void startConfigMode();
void loadSettingsAndConnect();
void fetchAircraftData();
bool ensureRoute(int idx);
void playNewAircraftSound();
void handleButtons();
void drawResetScreen(int seconds_left);
void drawDegreeSymbol(int x, int y);
void drawBatteryStatus();
void goToSleep();
void drawClock();
void saveStats();
void showToast(const char* text);
bool gpsFixLive();
void gpsPoll();
void updateHomePosition();
void buildUrls();
bool toastActive();

// ---------------------------------------------------------------- helper functions
static void cpy(char* dst, size_t n, const char* src) {
  if (!src) { dst[0] = 0; return; }
  strncpy(dst, src, n - 1);
  dst[n - 1] = 0;
}

uint32_t levelColor(uint8_t level) {
  if (level >= 3) return TFT_RED;
  if (level == 2) return TFT_ORANGE;
  return TFT_CYAN;                       // RARE
}

// text colour on the banner of the given level (black on light background)
uint32_t levelTextColor(uint8_t level) {
  return (level >= 3) ? (uint32_t)TFT_WHITE : (uint32_t)TFT_BLACK;
}

const char* levelName(uint8_t level) {
  if (level >= 3) return "EMERGENCY";
  if (level == 2) return "MILITARY";
  return "RARE";
}

const char* lookupKV(const KV* tbl, size_t n, const char* key) {
  for (size_t i = 0; i < n; i++) {
    if (strcmp(tbl[i].k, key) == 0) return tbl[i].v;
  }
  return nullptr;
}

const char* typeName(const Aircraft& a) {
  const char* n = lookupKV(TYPE_NAMES, KV_COUNT(TYPE_NAMES), a.type);
  if (n) return n;
  if (a.desc[0]) return a.desc;
  return a.type[0] ? a.type : "N/A";
}

const char* airlineName(const Aircraft& a) {
  if (strlen(a.flight) >= 4 && isalpha(a.flight[0]) && isalpha(a.flight[1]) &&
      isalpha(a.flight[2]) && isdigit(a.flight[3])) {
    char key[4] = {a.flight[0], a.flight[1], a.flight[2], 0};
    return lookupKV(AIRLINES, KV_COUNT(AIRLINES), key);
  }
  return nullptr;
}

uint8_t interestLevel(const Aircraft& a) {
  if ((a.emergency[0] && strcmp(a.emergency, "none") != 0) ||
      strcmp(a.squawk, "7700") == 0 || strcmp(a.squawk, "7600") == 0 ||
      strcmp(a.squawk, "7500") == 0) return 3;
  if (a.dbFlags & 1) return 2;
  for (size_t i = 0; i < KV_COUNT(RARE_TYPES); i++) {
    if (strcmp(a.type, RARE_TYPES[i]) == 0) return 1;
  }
  return 0;
}

// ---------------------------------------------------------------- geometry
Rel relOf(double lat, double lon) {
  Rel r;
  r.dy = (lat - homeLat) * 60.0;
  r.dx = (lon - homeLon) * 60.0 * cos(((lat + homeLat) / 2.0) * DEG_TO_RAD);
  r.dist = sqrt(r.dx * r.dx + r.dy * r.dy);
  r.bearing = atan2(r.dx, r.dy) * RAD_TO_DEG;
  if (r.bearing < 0) r.bearing += 360.0;
  return r;
}

// Extrapolated position between two API polls (dead reckoning)
void posNow(double lat0, double lon0, float gs, float track, bool hasTrack,
            double& lat, double& lon) {
  float dt = (millis() - fixTime) / 1000.0f;
  if (dt > 120) dt = 120;
  lat = lat0;
  lon = lon0;
  if (hasTrack && gs > 0) {
    double dNm = gs * dt / 3600.0;
    double t = track * DEG_TO_RAD;
    lat += dNm * cos(t) / 60.0;
    lon += dNm * sin(t) / (60.0 * cos(lat0 * DEG_TO_RAD));
  }
}

Rel relNow(const Aircraft& a) {
  double lat, lon;
  posNow(a.lat, a.lon, a.gs, a.track, a.hasTrack, lat, lon);
  return relOf(lat, lon);
}

// radial speed in kt: negative = approaching, positive = receding
bool radialKt(const Aircraft& a, const Rel& r, double& radial) {
  if (!a.hasTrack || r.dist < 0.05) { radial = 0; return false; }
  radial = a.gs * cos((a.track - r.bearing) * DEG_TO_RAD);
  return true;
}

// Closest point of approach (CPA). false = aircraft already passed / no track
bool computeCpa(const Aircraft& a, const Rel& r, double& cpaNm, double& tSec) {
  if (!a.hasTrack || a.gs < 20) return false;
  double vx = a.gs * sin(a.track * DEG_TO_RAD) / 3600.0;   // NM/s
  double vy = a.gs * cos(a.track * DEG_TO_RAD) / 3600.0;
  double v2 = vx * vx + vy * vy;
  if (v2 <= 0) return false;
  double t = -(r.dx * vx + r.dy * vy) / v2;
  if (t <= 0) return false;
  double ex = r.dx + vx * t, ey = r.dy + vy * t;
  cpaNm = sqrt(ex * ex + ey * ey);
  tSec = t;
  return true;
}

// ---------------------------------------------------------------- GPS (optional)
// NMEA 0183 from the GPS unit. Wire orientation and baud rate are detected automatically.
void gpsStartCombo(int idx) {
  int pins = idx % 2, baud = (idx / 2) % 2;
  GPSSerial.end();
  GPSSerial.setRxBufferSize(1024);
  GPSSerial.begin(GPS_BAUDS[baud], SERIAL_8N1, GPS_PINS[pins][0], GPS_PINS[pins][1]);
  gpsCombo = idx;
  gpsComboStart = millis();
  nmeaLen = 0;
}

void gpsBegin() {
  gpsCombosTried = 0;
  gpsGaveUp = false;
  gpsStartCombo(0);
}

// "ddmm.mmmm" + hemisphere -> signed degrees
bool nmeaToDeg(const char* s, const char* hemi, double& out) {
  if (!s || !*s || !hemi || !*hemi) return false;
  double v = atof(s);
  int deg = (int)(v / 100.0);
  double d = deg + (v - deg * 100.0) / 60.0;
  if (hemi[0] == 'S' || hemi[0] == 'W') d = -d;
  out = d;
  return true;
}

void gpsSetFix(double lat, double lon) {
  gpsLat = lat;
  gpsLon = lon;
  gpsEverFix = true;
  gpsLastFixMs = millis();
}

void gpsHandleLine(char* line) {
  char* star = strchr(line, '*');
  if (!star) return;
  uint8_t cs = 0;
  for (char* q = line + 1; q < star; q++) cs ^= (uint8_t)*q;
  if ((uint8_t)strtoul(star + 1, nullptr, 16) != cs) return;   // bad checksum (wrong baud, noise)
  *star = 0;

  if (!gpsDetected) {
    Serial.printf("[GPS] module detected (combo %d: RX=%d TX=%d, %ld baud)\n", gpsCombo,
                  GPS_PINS[gpsCombo % 2][0], GPS_PINS[gpsCombo % 2][1], GPS_BAUDS[(gpsCombo / 2) % 2]);
  }
  gpsDetected = true;
  gpsLastSeenMs = millis();

  char* f[16];
  int n = 0;
  f[n++] = line;
  for (char* q = line; *q && n < 16; q++) {
    if (*q == ',') { *q = 0; f[n++] = q + 1; }
  }
  if (strlen(f[0]) < 6) return;
  const char* type = f[0] + 3;       // "$GNGGA" -> "GGA"
  double lat, lon;
  if (strncmp(type, "GGA", 3) == 0 && n >= 8) {          // time,lat,N/S,lon,E/W,quality,sats
    gpsSats = atoi(f[7]);
    if (atoi(f[6]) > 0 && nmeaToDeg(f[2], f[3], lat) && nmeaToDeg(f[4], f[5], lon)) gpsSetFix(lat, lon);
  } else if (strncmp(type, "RMC", 3) == 0 && n >= 7) {   // time,status,lat,N/S,lon,E/W
    if (f[2][0] == 'A' && nmeaToDeg(f[3], f[4], lat) && nmeaToDeg(f[5], f[6], lon)) gpsSetFix(lat, lon);
  }
}

void gpsFeed(int c) {
  if (c == '$') { nmeaLen = 0; nmeaBuf[nmeaLen++] = '$'; return; }
  if (nmeaLen == 0) return;
  if (c == '\r' || c == '\n') {
    nmeaBuf[nmeaLen] = 0;
    nmeaLen = 0;
    gpsHandleLine(nmeaBuf);
    return;
  }
  if (nmeaLen < (int)sizeof(nmeaBuf) - 1) nmeaBuf[nmeaLen++] = (char)c;
  else nmeaLen = 0;
}

void gpsPoll() {
  if (!gpsEnabled) return;
  while (GPSSerial.available() > 0) gpsFeed(GPSSerial.read());

  if (gpsDetected && millis() - gpsLastSeenMs > 10000) {   // module unplugged: search again
    gpsDetected = false;
    gpsGaveUp = false;
    gpsCombosTried = 0;
    gpsComboStart = millis();
    Serial.println("[GPS] no data for 10 s, searching again");
  }
  if (!gpsDetected && !gpsGaveUp && millis() - gpsComboStart > 1600) {
    gpsCombosTried++;
    if (gpsCombosTried >= 20) {          // 5 rounds over all pin/baud combinations
      gpsGaveUp = true;
      GPSSerial.end();
      Serial.println("[GPS] no module found (reboot to search again)");
    } else {
      gpsStartCombo((gpsCombo + 1) % 4);
    }
  }
}

// position updated within the last 5 s
bool gpsFixLive() {
  return gpsEnabled && gpsEverFix && millis() - gpsLastFixMs < 5000;
}

// Home position = GPS position (while a fix is available, up to 10 min old), else the saved one
void updateHomePosition() {
  bool useGps = gpsEnabled && gpsEverFix && millis() - gpsLastFixMs < 600000UL;
  if (useGps) { homeLat = gpsLat; homeLon = gpsLon; }
  else { homeLat = cfgLat; homeLon = cfgLon; }
  posFromGps = useGps;
}

void buildUrls() {
  String la = String(homeLat, 5), lo = String(homeLon, 5);
  // /v2/point returns all aircraft in range, we pick the nearest ourselves
  api_url = "https://api.adsb.lol/v2/point/" + la + "/" + lo + "/" + radius_km;
  // Fallback used while adsb.lol answers 429: adsb.fi open data (ADSBexchange v2 compatible,
  // free for personal non-commercial use, 1 request/s; please credit adsb.fi)
  api_url_alt = "https://opendata.adsb.fi/api/v2/lat/" + la + "/lon/" + lo + "/dist/" + radius_km;
}

// ---------------------------------------------------------------- statistics
void resetStats(uint32_t day) {
  memset(&stats, 0, sizeof(stats));
  stats.day = day;
  stats.minKm = 9999.0f;
  seenN = 0;
  statsDirty = true;
}

void loadStats() {
  resetStats(0);
  statsDirty = false;
  if (preferences.getBytesLength("stats") == sizeof(DayStats)) {
    preferences.getBytes("stats", &stats, sizeof(DayStats));
  }
  size_t len = preferences.getBytesLength("seen");
  if (len > 0 && len <= sizeof(seenHex) && len % 4 == 0) {
    preferences.getBytes("seen", seenHex, len);
    seenN = len / 4;
  }
}

void saveStats() {
  preferences.putBytes("stats", &stats, sizeof(stats));
  if (seenN > 0) preferences.putBytes("seen", seenHex, seenN * 4);
  statsDirty = false;
  lastStatsSave = millis();
}

void statsBeginScan() {
  struct tm t;
  statsActive = false;
  if (getLocalTime(&t, 0) && t.tm_year >= 120) {
    uint32_t key = (t.tm_year + 1900) * 10000UL + (t.tm_mon + 1) * 100UL + t.tm_mday;
    statsActive = true;
    if (stats.day != key) resetStats(key);
  }
}

void statsUpdate(const Aircraft& a) {
  if (!statsActive) return;
  bool seen = false;
  for (int i = 0; i < seenN; i++) {
    if (seenHex[i] == a.hex32) { seen = true; break; }
  }
  if (!seen) {
    if (seenN < SEEN_MAX) seenHex[seenN++] = a.hex32;
    stats.count++;
    statsDirty = true;
  }
  const char* name = a.flight[0] ? a.flight : (a.reg[0] ? a.reg : a.hex);
  float km = a.distNm * 1.852f;
  if (km < stats.minKm) {
    stats.minKm = km;
    cpy(stats.minFl, sizeof(stats.minFl), name);
    statsDirty = true;
  }
  if (!a.onGround && a.altBaro > stats.maxAlt && a.altBaro < 70000) {
    stats.maxAlt = a.altBaro;
    cpy(stats.altFl, sizeof(stats.altFl), name);
    statsDirty = true;
  }
  if ((int)a.gs > stats.maxGs && a.gs < 1000) {
    stats.maxGs = (int)a.gs;
    cpy(stats.gsFl, sizeof(stats.gsFl), name);
    statsDirty = true;
  }
}

// ---------------------------------------------------------------- parsing
void buildFilter() {
  const char* keys[] = {
    "hex", "flight", "r", "t", "desc", "lat", "lon", "alt_baro", "alt_geom", "gs",
    "track", "true_heading", "baro_rate", "roll", "mach", "ias", "tas", "nav_modes",
    "nav_altitude_mcp", "nav_heading", "squawk", "emergency", "category", "tat", "oat",
    "wd", "ws", "rssi", "dbFlags"
  };
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) objFilter[keys[i]] = true;
}

void fillAircraft(JsonObjectConst o, Aircraft& a) {
  memset(&a, 0, sizeof(a));
  const char* h = o["hex"] | "";
  cpy(a.hex, sizeof(a.hex), h);
  a.hex32 = strtoul(h + (h[0] == '~' ? 1 : 0), nullptr, 16);
  cpy(a.flight, sizeof(a.flight), o["flight"] | "");
  for (int i = strlen(a.flight) - 1; i >= 0 && a.flight[i] == ' '; i--) a.flight[i] = 0;
  cpy(a.reg, sizeof(a.reg), o["r"] | "");
  cpy(a.type, sizeof(a.type), o["t"] | "");
  cpy(a.desc, sizeof(a.desc), o["desc"] | "");
  cpy(a.squawk, sizeof(a.squawk), o["squawk"] | "----");
  cpy(a.emergency, sizeof(a.emergency), o["emergency"] | "none");
  cpy(a.category, sizeof(a.category), o["category"] | "");

  a.hasPos = !o["lat"].isNull() && !o["lon"].isNull();
  a.lat = o["lat"] | 0.0;
  a.lon = o["lon"] | 0.0;

  JsonVariantConst alt = o["alt_baro"];
  if (alt.is<const char*>()) a.onGround = true;
  else a.altBaro = alt | 0;
  a.altGeom = o["alt_geom"] | 0;
  a.gs = o["gs"] | 0.0f;
  a.baroRate = o["baro_rate"] | 0;
  a.roll = o["roll"] | 0.0f;
  a.mach = o["mach"] | 0.0f;
  a.ias = o["ias"] | 0;
  a.tas = o["tas"] | 0;
  a.navAlt = o["nav_altitude_mcp"] | 0;
  a.navHdg = o["nav_heading"] | 0.0f;
  a.tat = o["tat"] | 0;
  a.oat = o["oat"] | 0;
  a.wd = o["wd"] | 0;
  a.ws = o["ws"] | 0;
  a.rssi = o["rssi"] | 0.0f;
  a.dbFlags = o["dbFlags"] | 0;

  if (!o["track"].isNull()) { a.track = o["track"] | 0.0f; a.hasTrack = true; }
  if (!o["true_heading"].isNull()) { a.hdg = o["true_heading"] | 0.0f; a.hasHdg = true; }
  else if (a.hasTrack) { a.hdg = a.track; a.hasHdg = true; }
  if (!a.hasTrack && a.hasHdg) { a.track = a.hdg; a.hasTrack = true; }

  String nm = "";
  JsonArrayConst arr = o["nav_modes"];
  for (JsonVariantConst v : arr) {
    nm += v.as<String>();
    nm += " ";
  }
  nm.trim();
  cpy(a.navModes, sizeof(a.navModes), nm.c_str());

  strcpy(a.dep, "-");
  strcpy(a.arr, "-");
}

void insertCand() {
  float key = (float)tmpA.distNm - (tmpA.level > 0 ? 1000.0f : 0.0f);
  if (candCount < MAX_AC) {
    cand[candCount] = tmpA;
    candKey[candCount] = key;
    candCount++;
  } else {
    int w = 0;
    for (int i = 1; i < MAX_AC; i++) if (candKey[i] > candKey[w]) w = i;
    if (key < candKey[w]) { cand[w] = tmpA; candKey[w] = key; }
  }
}

void insertBlip() {
  Blip b;
  b.hex32 = tmpA.hex32;
  b.lat = tmpA.lat;
  b.lon = tmpA.lon;
  b.track = tmpA.track;
  b.gs = tmpA.gs;
  b.hasTrack = tmpA.hasTrack;
  b.level = tmpA.level;
  float key = (float)tmpA.distNm;
  if (blipNextCount < MAX_BLIPS) {
    blipNext[blipNextCount] = b;
    blipKey[blipNextCount] = key;
    blipNextCount++;
  } else {
    int w = 0;
    for (int i = 1; i < MAX_BLIPS; i++) if (blipKey[i] > blipKey[w]) w = i;
    if (key < blipKey[w]) { blipNext[w] = b; blipKey[w] = key; }
  }
}

void processObject(const String& obj) {
  JsonDocument odoc;
  scanObjs++;
  DeserializationError derr = deserializeJson(odoc, obj, DeserializationOption::Filter(objFilter));
  if (derr) { scanLastErr = derr.c_str(); return; }
  scanParsed++;
  fillAircraft(odoc.as<JsonObjectConst>(), tmpA);
  if (!tmpA.hasPos) { scanNoPos++; return; }
  Rel r = relOf(tmpA.lat, tmpA.lon);
  tmpA.distNm = r.dist;
  tmpA.bearing = r.bearing;
  tmpA.level = interestLevel(tmpA);
  statsUpdate(tmpA);
  insertCand();
  insertBlip();
}

bool seekAcArray(StreamReader& r) {
  char w[11] = {0};
  int c, guard = 0;
  while ((c = r.next()) >= 0 && guard++ < 4000) {
    memmove(w, w + 1, 10);
    w[10] = (char)c;
    // adsb.lol uses "ac", adsb.fi may use "aircraft"
    if (memcmp(w + 6, "\"ac\":", 5) == 0 || memcmp(w, "\"aircraft\":", 11) == 0) {
      while ((c = r.next()) >= 0) {
        if (c == '[') return true;
        if (c != ' ' && c != '\n' && c != '\r') return false;
      }
      return false;
    }
  }
  return false;
}

// Streams the "ac" array one aircraft at a time (low memory use even with many aircraft)
bool scanResponse(HTTPClient& http) {
  bool chunked = http.header("Transfer-Encoding").indexOf("chunked") >= 0;
  Serial.printf("[AIRCRAFT] chunked=%d, content-length=%d\n", chunked, http.getSize());
  StreamReader r(http.getStream(), http, chunked);
  if (!seekAcArray(r)) return false;

  String obj;
  obj.reserve(1536);
  int depth = 0, c;
  bool inStr = false, esc = false;
  while ((c = r.next()) >= 0) {
    if (depth == 0) {
      if (c == '{') { depth = 1; obj = "{"; inStr = false; esc = false; }
      else if (c == ']') return true;
      continue;
    }
    if (obj.length() < 6000) obj += (char)c;
    if (inStr) {
      if (esc) esc = false;
      else if (c == '\\') esc = true;
      else if (c == '"') inStr = false;
    } else {
      if (c == '"') inStr = true;
      else if (c == '{' || c == '[') depth++;
      else if (c == '}' || c == ']') {
        depth--;
        if (depth == 0) processObject(obj);
      }
    }
  }
  return false;   // connection ended before the "ac" array was complete
}

// ---------------------------------------------------------------- alerts / sounds
void playNewAircraftSound() {
  if (!soundOn) return;
  M5.Speaker.tone(1400, 60);
  delay(70);
  M5.Speaker.tone(1800, 90);
}

void playAlertSound(uint8_t level) {
  if (!soundOn) return;
  if (level >= 3) {
    for (int i = 0; i < 3; i++) { M5.Speaker.tone(2600, 120); delay(170); }
  } else if (level == 2) {
    M5.Speaker.tone(900, 120); delay(160);
    M5.Speaker.tone(1500, 160); delay(200);
  } else {
    M5.Speaker.tone(1800, 80); delay(110);
    M5.Speaker.tone(2200, 80); delay(110);
  }
}

bool wasAlerted(uint32_t hex32) {
  for (int i = 0; i < 8; i++) if (alertedHex[i] == hex32) return true;
  return false;
}

// returns true if a new alert was triggered (and that aircraft was selected)
bool checkAlerts() {
  for (int i = 0; i < acCount; i++) {
    if (ac[i].level > 0 && !wasAlerted(ac[i].hex32)) {
      alertedHex[alertedIdx] = ac[i].hex32;
      alertedIdx = (alertedIdx + 1) % 8;
      sel = i;
      currentPage = 0;
      playAlertSound(ac[i].level);
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------- flight route
bool ensureRoute(int idx) {
  if (idx < 0 || idx >= acCount) return false;
  Aircraft& a = ac[idx];
  if (a.routeTried || a.flight[0] == 0) return false;
  a.routeTried = true;

  JsonDocument requestDoc;
  JsonArray planes = requestDoc["planes"].to<JsonArray>();
  JsonObject plane = planes.add<JsonObject>();
  plane["callsign"] = a.flight;
  plane["lat"] = 0;
  plane["lng"] = 0;
  String requestBody;
  serializeJson(requestDoc, requestBody);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  http.setUserAgent(userAgent);
  if (!http.begin(client, "https://adsb.im/api/0/routeset")) {
    Serial.println("[ROUTE] ERROR: http.begin() failed");
    return true;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");

  int httpCode = http.POST(requestBody);
  Serial.printf("[ROUTE] %s HTTP %d, free heap: %u\n", a.flight, httpCode,
                (unsigned int)ESP.getFreeHeap());

  if (httpCode == HTTP_CODE_OK) {
    String responseBody = http.getString();
    JsonDocument responseDoc;
    DeserializationError error = deserializeJson(responseDoc, responseBody);
    if (!error && responseDoc.is<JsonArray>() && responseDoc.as<JsonArray>().size() > 0) {
      JsonObject flightInfo = responseDoc[0];
      if (flightInfo["_airport_codes_iata"].is<const char*>()) {
        String routeStr = flightInfo["_airport_codes_iata"].as<String>();
        int sep = routeStr.indexOf('-');
        if (sep > 0) {
          cpy(a.dep, sizeof(a.dep), routeStr.substring(0, sep).c_str());
          cpy(a.arr, sizeof(a.arr), routeStr.substring(sep + 1).c_str());
        }
      }
    }
  }
  http.end();
  return true;
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  Serial.begin(115200);
  unsigned long serialT0 = millis();
  while (!Serial && millis() - serialT0 < 3000) delay(10);
  delay(1000);
  Serial.println();
  Serial.println("nearPlane+: start (v4)");
  {
    int rr = (int)esp_reset_reason();
    const char* txt = rr == 1 ? "power-on" : rr == 3 ? "software restart" : rr == 4 ? "PANIC / crash"
                    : rr == 5 ? "interrupt watchdog" : rr == 6 ? "task watchdog" : rr == 7 ? "other watchdog"
                    : rr == 8 ? "deep sleep wake" : rr == 9 ? "BROWNOUT (low voltage)" : "other";
    Serial.printf("Reset reason: %d (%s)\n", rr, txt);
  }
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Power.begin();
  preferences.begin("adsb-config", false);
  soundOn = preferences.getBool("sound", true);
  gpsEnabled = preferences.getBool("gps", true);
  M5.Power.setExtOutput(gpsEnabled);    // 5V on the Grove port only when GPS is enabled
  if (gpsEnabled) gpsBegin();
  M5.Speaker.setVolume(255);
  if (soundOn) M5.Speaker.tone(2000, 100);
  M5.Display.setRotation(1);
  canvas.createSprite(M5.Display.width(), M5.Display.height());
  buildFilter();
  loadStats();
  wifi_ssid = preferences.getString("ssid", "");
  if (wifi_ssid == "") {
    configMode = true;
    startConfigMode();
  } else {
    configMode = false;
    loadSettingsAndConnect();
  }
  lastActivityTime = millis();
}

void loop() {
  M5.update();
  handleButtons();
  gpsPoll();
  if (millis() - lastHomeUpdate > 1000) {
    lastHomeUpdate = millis();
    updateHomePosition();
  }
  if (isResetting) { return; }
  if (configMode) {
    dnsServer.processNextRequest();
    server.handleClient();
  } else {
    if (millis() - lastBeat > 5000) {
      lastBeat = millis();
      long left = (long)pollInterval - (long)(millis() - lastPollTime);
      if (firstPoll) left = 0;
      const char* gs = !gpsEnabled ? "off" : gpsFixLive() ? "fix" : gpsDetected ? "no-fix" : "none";
      Serial.printf("[LOOP] wifi=%d, next poll in %ld s, aircraft=%d, heap=%u, gps=%s, pos=%.5f,%.5f%s\n",
                    (int)WiFi.status(), left > 0 ? left / 1000 : 0L, acCount,
                    (unsigned int)ESP.getFreeHeap(), gs, homeLat, homeLon, posFromGps ? " (GPS)" : "");
    }
    if (WiFi.status() == WL_CONNECTED && (firstPoll || millis() - lastPollTime > pollInterval)) {
      firstPoll = false;
      fetchAircraftData();
      lastPollTime = millis();
    }
    if (routePending && !isResetting) {
      routePending = false;
      if (ensureRoute(sel)) displayCurrentPage();
    }
    if (rateLimited && acCount == 0 && !toastActive() && millis() - lastRedraw > 1000) {
      lastRedraw = millis();                           // refresh the retry countdown
      displayCurrentPage();
    }
    if (toastUntil != 0 && millis() >= toastUntil) {   // message expired: redraw page
      toastUntil = 0;
      displayCurrentPage();
    }
    // Long airline/type name: smooth scrolling (about 15 frames/s)
    if (!toastActive() && marqueeActive && acCount > 0 && currentPage < PAGE_MAP && millis() - lastMarquee > 60) {
      lastMarquee = millis();
      lastRedraw = millis();
      displayCurrentPage();
    }
    // Pages with live position: redraw every second (dead reckoning)
    if (acCount > 0 && (currentPage == PAGE_TRACK || currentPage == PAGE_MAP ||
                        currentPage == PAGE_LIST) && !toastActive() && millis() - lastRedraw > 1000) {
      lastRedraw = millis();
      displayCurrentPage();
    }
    if (statsDirty && millis() - lastStatsSave > 600000UL) saveStats();
    if (millis() - lastActivityTime > INACTIVITY_TIMEOUT_MS) {
      goToSleep();
    }
  }
}

void goToSleep() {
  if (statsDirty) saveStats();
  canvas.fillScreen(BLACK);
  canvas.setTextColor(TFT_WHITE);
  canvas.setTextDatum(MC_DATUM);
  canvas.setFont(&fonts::FreeSansBold12pt7b);
  canvas.drawString("No Activity", 120, 50);
  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.drawString("Going to sleep...", 120, 80);
  canvas.pushSprite(0, 0);
  delay(3000);
  M5.Display.sleep();
  M5.Power.deepSleep(DEEP_SLEEP_DURATION_S * 1000000ULL);
}

// ---------------------------------------------------------------- icons
void drawBatteryStatus() {
  int batt_level = M5.Power.getBatteryLevel();
  bool is_charging = M5.Power.isCharging();
  uint32_t batt_color;

  if (is_charging) {
    batt_color = TFT_GREEN;
  } else if (batt_level < 15) {
    batt_color = TFT_RED;
  } else if (batt_level < 40) {
    batt_color = TFT_YELLOW;
  } else {
    batt_color = TFT_GREEN;
  }

  canvas.drawRect(5, 5, 20, 10, uiFg);
  canvas.drawRect(25, 7, 2, 6, uiFg);

  int fill_width = (18 * batt_level) / 100;
  if (fill_width > 0) {
    canvas.fillRect(6, 6, fill_width, 8, batt_color);
  }

  if (is_charging) {
    canvas.drawLine(14, 7, 11, 10, TFT_YELLOW);
    canvas.drawLine(11, 10, 14, 13, TFT_YELLOW);
  }

  if (!soundOn) {   // muted speaker icon next to the battery
    const int x = 31, y = 5;
    canvas.fillRect(x, y + 3, 3, 4, uiFg);
    canvas.fillTriangle(x + 3, y + 3, x + 7, y, x + 7, y + 9, uiFg);
    canvas.drawLine(x + 9, y + 2, x + 14, y + 8, TFT_RED);
    canvas.drawLine(x + 9, y + 8, x + 14, y + 2, TFT_RED);
  }

  if (gpsEnabled && gpsDetected) {   // GPS = live fix, GPS? = module found, no fix yet
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(uiFg);
    canvas.setTextDatum(TR_DATUM);
    canvas.drawString(gpsFixLive() ? "GPS" : "GPS?", 235, 15);
  }
}

void drawClock() {
  struct tm timeinfo;
  char timeBuffer[6];

  canvas.setFont(&fonts::Font0);
  canvas.setTextColor(uiFg);
  canvas.setTextDatum(TR_DATUM);

  if (!getLocalTime(&timeinfo, 0)) {
    canvas.drawString("--:--", 235, 5);
    return;
  }

  sprintf(timeBuffer, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  canvas.drawString(timeBuffer, 235, 5);
}

void drawDegreeSymbol(int x, int y) {
  canvas.drawCircle(x, y, 2, TFT_WHITE);
}

// ---------------------------------------------------------------- buttons
// short on-screen message drawn straight on the display (not on the canvas)
void showToast(const char* text) {
  M5.Display.fillRoundRect(30, 50, 180, 36, 6, TFT_DARKGREY);
  M5.Display.drawRoundRect(30, 50, 180, 36, 6, TFT_WHITE);
  M5.Display.setFont(&fonts::FreeSansBold12pt7b);
  M5.Display.setTextColor(TFT_WHITE, TFT_DARKGREY);
  M5.Display.setTextDatum(MC_DATUM);
  M5.Display.drawString(text, 120, 68);
  toastUntil = millis() + 1500;
}

bool toastActive() { return toastUntil != 0 && millis() < toastUntil; }

// map radius in NM for the given scale step
float scaleNm(int i) {
  if (i == 1) return 25.0f / 1.852f;
  if (i == 2) return 12.5f / 1.852f;
  if (i == 3) return 7.5f / 1.852f;
  return radiusNm;
}

void cycleMapScale() {
  mapScaleIdx = (mapScaleIdx + 1) % 4;
  // skip steps that are not smaller than the configured radius
  while (mapScaleIdx > 0 && scaleNm(mapScaleIdx) >= radiusNm) mapScaleIdx = (mapScaleIdx + 1) % 4;
}

bool inMapRange(const Aircraft& a) {
  return relNow(a).dist <= scaleNm(mapScaleIdx);
}

// next aircraft; on the map it skips those outside the current scale
void selectNextAircraft() {
  if (acCount < 2) return;
  int next = (sel + 1) % acCount;
  if (currentPage == PAGE_MAP) {
    for (int k = 1; k < acCount; k++) {
      int idx = (sel + k) % acCount;
      if (inMapRange(ac[idx])) { next = idx; break; }
    }
  }
  sel = next;
}

void handleButtons() {
  if (!isResetting && M5.BtnA.wasPressed()) {
    lastActivityTime = millis();
    if (currentPage != PAGE_MAP) {
      currentPage = (currentPage + 1) % NUM_PAGES;
      displayCurrentPage();
    } else {
      btnADown = true;          // on the map: short = next page, long = detail
      btnALong = false;
      btnAStart = millis();
    }
  }
  // BtnC: short press = radar map scale, hold 0.75 s = buzzer on/off
  if (!isResetting && M5.BtnPWR.wasPressed()) {
    btnCDown = true;
    btnCToggled = false;
    btnCStart = millis();
    lastActivityTime = millis();
  }
  if (btnCDown && !btnCToggled && M5.BtnPWR.isPressed() && millis() - btnCStart >= BTN_C_HOLD_MS) {
    btnCToggled = true;
    if (!configMode) {
      soundOn = !soundOn;
      preferences.putBool("sound", soundOn);
      if (soundOn) M5.Speaker.tone(2000, 100);
      displayCurrentPage();                       // refresh the mute icon
      showToast(soundOn ? "Sound ON" : "Sound OFF");
    }
  }
  if (M5.BtnPWR.wasReleased() && btnCDown) {
    btnCDown = false;
    if (!btnCToggled && !configMode && currentPage == PAGE_MAP) {
      cycleMapScale();
      displayCurrentPage();
    }
  }
  if (btnADown && !btnALong && M5.BtnA.isPressed() && millis() - btnAStart >= BTN_A_LONG_MS) {
    btnALong = true;
    currentPage = 0;            // detail of the selected aircraft
    displayCurrentPage();
  }
  if (M5.BtnA.wasReleased() && btnADown) {
    btnADown = false;
    if (!btnALong) {
      currentPage = (currentPage + 1) % NUM_PAGES;
      displayCurrentPage();
    }
  }

  if (M5.BtnB.wasPressed()) {
    btnB_press_start_time = millis();
    btnBDown = true;
    lastActivityTime = millis();
  }

  if (btnBDown && M5.BtnB.isPressed()) {
    unsigned long d = millis() - btnB_press_start_time;
    if (d >= (unsigned long)RESET_HOLD_TIME_MS) {
      canvas.fillScreen(TFT_ORANGE);
      canvas.setTextColor(TFT_WHITE);
      canvas.setFont(&fonts::FreeSansBold12pt7b);
      canvas.setTextDatum(MC_DATUM);
      canvas.drawString("Settings Reset!", 120, 67);
      canvas.pushSprite(0, 0);
      preferences.clear();
      delay(2500);
      ESP.restart();
    } else if (d >= (unsigned long)BTN_B_HOLD_MS) {
      isResetting = true;
      if (millis() - lastResetDraw > 200) {
        lastResetDraw = millis();
        drawResetScreen((int)ceil((RESET_HOLD_TIME_MS - d) / 1000.0));
      }
    }
  }

  if (M5.BtnB.wasReleased() && btnBDown) {
    unsigned long d = millis() - btnB_press_start_time;
    btnBDown = false;
    if (isResetting) {
      isResetting = false;       // hold cancelled
      displayCurrentPage();
    } else if (d < (unsigned long)BTN_B_HOLD_MS && acCount > 1) {
      selectNextAircraft();
      displayCurrentPage();
      if (ensureRoute(sel)) displayCurrentPage();   // route is fetched only now
    }
  }
}

void drawResetScreen(int seconds_left) {
  canvas.fillScreen(TFT_RED);
  canvas.setTextColor(TFT_WHITE);
  canvas.setTextDatum(MC_DATUM);
  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.drawString("Release to cancel", 120, 40);
  canvas.setFont(&fonts::Font4);
  canvas.drawString("Reset in " + String(seconds_left), 120, 90);
  canvas.pushSprite(0, 0);
}

// ---------------------------------------------------------------- configuration mode
void startConfigMode() {
  const char* ap_ssid = "nearPlane-ADSB-Tracker-Setup";
  canvas.fillScreen(TFT_BLUE);
  canvas.setTextColor(TFT_WHITE);
  canvas.setTextDatum(MC_DATUM);
  canvas.setFont(&fonts::FreeSansBold12pt7b);
  canvas.drawString("SETUP MODE", 120, 25);
  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.setTextDatum(ML_DATUM);
  canvas.drawString("1. Connect to WiFi:", 15, 55);
  canvas.setTextColor(TFT_YELLOW);
  canvas.drawString(ap_ssid, 35, 75);
  canvas.setTextColor(TFT_WHITE);
  canvas.drawString("2. Open browser to:", 15, 95);
  canvas.setTextColor(TFT_YELLOW);
  canvas.drawString("192.168.4.1", 35, 115);
  canvas.pushSprite(0, 0);
  WiFi.softAP(ap_ssid);
  dnsServer.start(53, "*", WiFi.softAPIP());
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleNotFound);
  server.begin();
}

void loadSettingsAndConnect() {
  wifi_password = preferences.getString("password", "");
  latitude = preferences.getString("lat", "0.0");
  longitude = preferences.getString("lon", "0.0");
  radius_km = preferences.getString("radius", "50");   // value is in NM (nautical miles)
  cfgLat = latitude.toDouble();
  cfgLon = longitude.toDouble();
  updateHomePosition();
  radiusNm = radius_km.toFloat();
  if (radiusNm < 1) radiusNm = 50;
  userAgent = preferences.getString("ua", "");
  userAgent.trim();
  if (userAgent.length() < 8) userAgent = USER_AGENT;   // nothing saved: use the built-in default
  buildUrls();
  canvas.fillScreen(BLACK);
  canvas.setTextColor(TFT_WHITE);
  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.setTextDatum(MC_DATUM);
  canvas.drawString("Connecting to:", 120, 45);
  canvas.setFont(&fonts::FreeSansBold9pt7b);
  canvas.drawString(wifi_ssid, 120, 65);
  canvas.drawRect(30, 85, 180, 10, TFT_WHITE);
  canvas.pushSprite(0, 0);
  WiFi.begin(wifi_ssid.c_str(), wifi_password.c_str());
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    canvas.fillRect(31, 86, (178 * attempts) / 30, 8, TFT_GREEN);
    canvas.pushSprite(0, 0);
    attempts++;
  }
  if (WiFi.status() != WL_CONNECTED) {
    canvas.fillScreen(TFT_RED);
    canvas.setTextColor(TFT_WHITE);
    canvas.setTextDatum(MC_DATUM);
    canvas.setFont(&fonts::FreeSansBold12pt7b);
    canvas.drawString("Connection Failed", 120, 45);
    canvas.setFont(&fonts::FreeSans9pt7b);
    canvas.drawString("Hold BtnB 5 sec to reset", 120, 90);
    canvas.pushSprite(0, 0);
  } else {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    canvas.fillScreen(TFT_DARKGREEN);
    canvas.setTextColor(TFT_WHITE);
    canvas.setTextDatum(MC_DATUM);
    canvas.setFont(&fonts::FreeSansBold12pt7b);
    canvas.drawString("Connected", 120, 55);
    canvas.setFont(&fonts::FreeSans9pt7b);
    canvas.drawString("Waiting for data...", 120, 85);
    canvas.pushSprite(0, 0);
    delay(2000);
  }
}

void handleRoot() {
  String html = FPSTR(index_html);
  String lat = "", lon = "", msg;
  bool gpsPosUsable = gpsEnabled && gpsEverFix && millis() - gpsLastFixMs < 600000UL;
  if (gpsPosUsable) {
    lat = String(gpsLat, 6);
    lon = String(gpsLon, 6);
    msg = "GPS fix found, coordinates prefilled (" + String(gpsSats) + " satellites).";
  } else if (!gpsEnabled) {
    msg = "GPS is disabled.";
  } else if (gpsDetected) {
    msg = "GPS module found, waiting for a satellite fix. Reload this page in a moment (typed values are lost).";
  } else {
    msg = "No GPS module detected (optional).";
  }
  html.replace("%GPSMSG%", msg);
  html.replace("%LAT%", lat);
  html.replace("%LON%", lon);
  html.replace("%GPSCHK%", gpsEnabled ? "checked" : "");
  server.send(200, "text/html", html);
}

void handleSave() {
  // User-Agent: single line only (no CR/LF header injection), must be a real contact
  String ua = server.arg("user_agent");
  ua.replace("\r", " ");
  ua.replace("\n", " ");
  ua.trim();
  if (ua.length() < 8 || ua.length() > 120 || ua.indexOf("CHANGE-ME") >= 0 ||
      ua.indexOf("changeme@example.com") >= 0 ||
      (ua.indexOf('@') < 0 && ua.indexOf("http://") < 0 && ua.indexOf("https://") < 0)) {
    server.send(400, "text/html",
                "<h1>User-Agent is not valid</h1><p>Enter your real e-mail or a URL (adsb.lol requires contact info).</p>"
                "<p><a href=\"/\">Back</a></p>");
    return;
  }
  preferences.putString("ua", ua);
  preferences.putBool("gps", server.hasArg("gps"));
  preferences.putString("ssid", server.arg("ssid"));
  preferences.putString("password", server.arg("password"));
  preferences.putString("lat", server.arg("lat"));
  preferences.putString("lon", server.arg("lon"));
  preferences.putString("radius", server.arg("radius"));
  server.send(200, "text/html", "<h1>Settings Saved!</h1><p>Device will reboot in 3 seconds.</p>");
  delay(3000);
  ESP.restart();
}

void handleNotFound() {
  server.sendHeader("Location", "http://192.168.4.1", true);
  server.send(302, "text/plain", "");
}

// ---------------------------------------------------------------- page rendering
void drawFooter(bool aircraftPage) {
  canvas.setFont(&fonts::Font0);
  canvas.setTextColor(TFT_DARKGREY);
  canvas.setTextDatum(BC_DATUM);
  canvas.drawString(String(currentPage + 1) + "/" + String(NUM_PAGES), 120, 134);
  if (aircraftPage && acCount > 0) {
    canvas.setTextDatum(BR_DATUM);
    canvas.drawString("AC " + String(sel + 1) + "/" + String(acCount), 236, 134);
    if (ac[sel].level > 0) {
      canvas.setTextDatum(BL_DATUM);
      canvas.setTextColor(levelColor(ac[sel].level));
      canvas.drawString(levelName(ac[sel].level), 4, 134);
    }
  }
}

// Page 7: arrow, distance, trend, CPA
void drawTrackPage() {
  Aircraft& a = ac[sel];
  Rel r = relNow(a);
  double km = r.dist * 1.852;
  double radial;
  bool known = radialKt(a, r, radial);

  uint32_t col = TFT_YELLOW;
  const char* trend = "---";
  if (known && radial < -10) { col = TFT_GREEN; trend = "APPROACHING"; }
  else if (known && radial > 10) { col = TFT_RED; trend = "RECEDING"; }
  else if (known) { trend = "STEADY"; }

  // Arrow (north up)
  const int cx = 55, cy = 100, rr = 26;
  canvas.drawCircle(cx, cy, rr + 2, TFT_DARKGREY);
  double ang = r.bearing * DEG_TO_RAD;
  double aL = ang + 145.0 * DEG_TO_RAD;
  double aR = ang - 145.0 * DEG_TO_RAD;
  int tx = cx + (int)(rr * sin(ang)),         ty = cy - (int)(rr * cos(ang));
  int lx = cx + (int)(rr * 0.8 * sin(aL)),    ly = cy - (int)(rr * 0.8 * cos(aL));
  int rx = cx + (int)(rr * 0.8 * sin(aR)),    ry = cy - (int)(rr * 0.8 * cos(aR));
  int nx = cx - (int)(rr * 0.25 * sin(ang)),  ny = cy + (int)(rr * 0.25 * cos(ang));
  canvas.fillTriangle(tx, ty, lx, ly, nx, ny, col);
  canvas.fillTriangle(tx, ty, rx, ry, nx, ny, col);

  // CPA
  char cpaLine[24], cpaTime[24];
  double cpaNm, tSec;
  cpaTime[0] = 0;
  if (!a.hasTrack || a.gs < 20) {
    strcpy(cpaLine, "CPA ---");
  } else if (!computeCpa(a, r, cpaNm, tSec)) {
    strcpy(cpaLine, "PASSED");
  } else {
    snprintf(cpaLine, sizeof(cpaLine), "CPA %.1f km", cpaNm * 1.852);
    int s = (int)tSec;
    if (s > 5999) strcpy(cpaTime, "in >99 min");
    else snprintf(cpaTime, sizeof(cpaTime), "in %d:%02d", s / 60, s % 60);
  }

  static const char* pts[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  int idx = ((int)((r.bearing + 22.5) / 45.0)) % 8;
  String bottom = String(cpaTime) + (cpaTime[0] ? "  " : "") + "BRG " + String((int)r.bearing) + " " + pts[idx];

  canvas.setTextDatum(TR_DATUM);
  canvas.setTextColor(TFT_WHITE);
  canvas.setFont(&fonts::FreeSansBold12pt7b);
  canvas.drawString(String(km, 1) + " km", 225, 68);
  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.setTextColor(col);
  canvas.drawString(trend, 225, 88);
  canvas.setTextColor(TFT_WHITE);
  canvas.drawString(cpaLine, 225, 104);
  canvas.setFont(&fonts::Font0);
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString(bottom, 225, 123);
}

// Page 8: radar map (north up, you are in the centre)
void drawMapPage() {
  const int cx = 120, cy = 76, R = 56;
  float rangeNm = scaleNm(mapScaleIdx);
  float scale = R / rangeNm;   // px per NM

  canvas.drawCircle(cx, cy, R, TFT_DARKGREY);
  canvas.drawCircle(cx, cy, R / 2, TFT_DARKGREY);
  canvas.drawFastHLine(cx - 4, cy, 9, TFT_GREEN);
  canvas.drawFastVLine(cx, cy - 4, 9, TFT_GREEN);

  float rangeKm = rangeNm * 1.852f;
  String rangeStr = String(rangeKm, fabs(rangeKm - roundf(rangeKm)) < 0.05f ? 0 : 1);
  canvas.setFont(&fonts::Font0);
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.setTextDatum(TC_DATUM);
  canvas.drawString("N", cx, 12);
  canvas.setTextDatum(BL_DATUM);
  canvas.setTextColor(TFT_DARKGREY);
  canvas.drawString("R " + rangeStr + " km", 4, 134);

  for (int i = 0; i < blipCount; i++) {
    Blip& b = blips[i];
    double lat, lon;
    posNow(b.lat, b.lon, b.gs, b.track, b.hasTrack, lat, lon);
    Rel r = relOf(lat, lon);
    int px = cx + (int)(r.dx * scale);
    int py = cy - (int)(r.dy * scale);
    double dpx = px - cx, dpy = py - cy;
    if (sqrt(dpx * dpx + dpy * dpy) > R + 3) continue;
    uint32_t col = b.level > 0 ? levelColor(b.level) : (uint32_t)TFT_WHITE;
    canvas.fillCircle(px, py, 2, col);
    if (b.hasTrack) {
      double t = b.track * DEG_TO_RAD;
      canvas.drawLine(px, py, px + (int)(7 * sin(t)), py - (int)(7 * cos(t)), col);
    }
  }

  // selected aircraft: yellow ring, or a dot on the rim if outside the scale
  if (acCount > 0) {
    Rel r = relNow(ac[sel]);
    if (r.dist * scale <= R + 3) {
      int px = cx + (int)(r.dx * scale);
      int py = cy - (int)(r.dy * scale);
      canvas.drawCircle(px, py, 5, TFT_YELLOW);
    } else {
      double b = r.bearing * DEG_TO_RAD;
      int ex = cx + (int)(R * sin(b));
      int ey = cy - (int)(R * cos(b));
      canvas.fillCircle(ex, ey, 3, TFT_YELLOW);
      canvas.setFont(&fonts::Font0);
      canvas.setTextColor(TFT_YELLOW);
      canvas.setTextDatum(BR_DATUM);
      canvas.drawString("SEL " + String(r.dist * 1.852, 1) + " km", 236, 134);
    }
  }
}

// Page 9: list of nearest aircraft
void drawListPage() {
  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.setTextDatum(TL_DATUM);
  for (int i = 0; i < acCount; i++) {
    Aircraft& a = ac[i];
    int y = 22 + i * 22;
    if (i == sel) canvas.fillRoundRect(2, y - 1, 236, 21, 3, TFT_NAVY);

    Rel r = relNow(a);
    double radial;
    bool known = radialKt(a, r, radial);
    uint32_t dcol = TFT_WHITE;
    if (known && radial < -10) dcol = TFT_GREEN;
    else if (known && radial > 10) dcol = TFT_RED;

    const char* name = a.flight[0] ? a.flight : (a.reg[0] ? a.reg : a.hex);
    canvas.setTextColor(a.level > 0 ? levelColor(a.level) : (uint32_t)TFT_WHITE);
    canvas.setTextDatum(TL_DATUM);
    canvas.drawString(name, 8, y + 1);

    canvas.setTextColor(dcol);
    canvas.setTextDatum(TR_DATUM);
    canvas.drawString(String(r.dist * 1.852, 1) + "km", 160, y + 1);

    String alt;
    if (a.onGround) alt = "GND";
    else if (a.altBaro >= 6000) alt = "FL" + String(a.altBaro / 100);
    else alt = String(a.altBaro) + "ft";
    canvas.setTextColor(TFT_LIGHTGREY);
    canvas.drawString(alt, 232, y + 1);
  }
}

// Page 10: daily statistics
void drawStatsPage() {
  canvas.setFont(&fonts::FreeSansBold9pt7b);
  canvas.setTextColor(TFT_CYAN);
  canvas.setTextDatum(TC_DATUM);
  canvas.drawString("TODAY", 120, 5);

  const char* labels[4] = {"Aircraft", "Closest", "Highest", "Fastest"};
  String values[4];
  values[0] = String(stats.count);
  values[1] = (stats.minKm < 9000) ? String(stats.minKm, 1) + " km " + stats.minFl : String("-");
  values[2] = (stats.maxAlt > 0) ? "FL" + String(stats.maxAlt / 100) + " " + stats.altFl : String("-");
  values[3] = (stats.maxGs > 0) ? String(stats.maxGs) + " kt " + stats.gsFl : String("-");

  for (int i = 0; i < 4; i++) {
    int y = 28 + i * 26;
    canvas.setFont(&fonts::FreeSans9pt7b);
    canvas.setTextColor(TFT_LIGHTGREY);
    canvas.setTextDatum(TL_DATUM);
    canvas.drawString(labels[i], 10, y);
    canvas.setFont(&fonts::FreeSansBold9pt7b);
    canvas.setTextColor(TFT_WHITE);
    canvas.setTextDatum(TR_DATUM);
    canvas.drawString(values[i], 232, y);
  }
}

void displayCurrentPage() {
  canvas.fillScreen(BLACK);
  marqueeActive = false;

  bool aircraftPage = (acCount > 0 && currentPage < PAGE_MAP);
  uiFg = TFT_WHITE;
  if (aircraftPage && ac[sel].level > 0) {
    canvas.fillRect(0, 0, 240, 40, levelColor(ac[sel].level));   // alert banner
    uiFg = levelTextColor(ac[sel].level);
  }
  drawBatteryStatus();
  drawClock();
  uiFg = TFT_WHITE;

  if (currentPage == PAGE_STATS) {
    drawStatsPage();
    drawFooter(false);
    canvas.pushSprite(0, 0);
    return;
  }

  if (acCount == 0) {
    canvas.setTextColor(TFT_WHITE);
    canvas.setFont(&fonts::FreeSansBold12pt7b);
    canvas.setTextDatum(MC_DATUM);
    if (rateLimited) {
      long left = (long)pollInterval - (long)(millis() - lastPollTime);
      if (left < 0) left = 0;
      canvas.drawString("Rate limited", 120, 60);
      canvas.setFont(&fonts::FreeSans9pt7b);
      canvas.drawString("Retry in " + String((left + 999) / 1000) + " s", 120, 90);
    } else {
      canvas.drawString("No aircraft nearby.", 120, 60);
      canvas.setFont(&fonts::FreeSans9pt7b);
      canvas.drawString("Checking again soon...", 120, 90);
    }
    canvas.pushSprite(0, 0);
    return;
  }

  if (currentPage == PAGE_MAP) {
    drawMapPage();
    canvas.pushSprite(0, 0);
    return;
  }

  if (currentPage == PAGE_LIST) {
    drawListPage();
    drawFooter(false);
    canvas.pushSprite(0, 0);
    return;
  }

  Aircraft& a = ac[sel];
  String flight = a.flight[0] ? a.flight : (a.reg[0] ? a.reg : a.hex);

  canvas.setTextColor(a.level > 0 ? levelTextColor(a.level) : (uint32_t)TFT_YELLOW);
  canvas.setFont(&fonts::Orbitron_Light_32);
  canvas.setTextDatum(TC_DATUM);
  canvas.drawString(flight, 120, 5);

  // line below the flight number: airline (or registration) | type
  const char* airline = airlineName(a);
  String line = String(airline ? airline : (a.reg[0] ? a.reg : "N/A")) + " | " + typeName(a);
  canvas.setTextColor(TFT_WHITE);
  canvas.setFont(&fonts::FreeSans9pt7b);
  int lw = canvas.textWidth(line);
  if (lw <= 228) {
    canvas.setTextDatum(TC_DATUM);
    canvas.drawString(line, 120, 45);
  } else {
    // long line: scrolls in a loop, pauses 1.5 s at the start
    marqueeActive = true;
    const int gap = 40;
    int period = lw + gap;
    unsigned long cycle = (unsigned long)period * 25UL + 1500UL;   // 40 px/s
    unsigned long ph = millis() % cycle;
    int off = (ph < 1500UL) ? 0 : (int)((ph - 1500UL) / 25UL);
    canvas.setClipRect(6, 43, 228, 22);
    canvas.setTextDatum(TL_DATUM);
    canvas.drawString(line, 6 - off, 45);
    canvas.drawString(line, 6 - off + period, 45);
    canvas.clearClipRect();
  }
  canvas.drawLine(10, 65, 230, 65, TFT_DARKGREY);

  switch (currentPage) {
    case 0: {
      canvas.setFont(&fonts::FreeSans9pt7b);
      canvas.setTextDatum(TL_DATUM);
      canvas.drawString("ALT", 15, 75);
      canvas.drawString("GND SPD", 15, 95);
      canvas.drawString("ROUTE", 15, 115);
      canvas.setFont(&fonts::FreeSansBold12pt7b);
      canvas.setTextDatum(TR_DATUM);
      canvas.drawString(a.onGround ? String("GND") : String(a.altBaro) + " ft", 225, 75);
      canvas.drawString(String(a.gs, 0) + " kt", 225, 95);
      canvas.drawString(String(a.dep) + " > " + String(a.arr), 225, 115);
      break;
    }
    case 1: {
      canvas.setFont(&fonts::FreeSans9pt7b);
      canvas.setTextDatum(TL_DATUM);
      canvas.drawString("HDG", 15, 75);
      canvas.drawString("V/S", 15, 95);
      canvas.drawString("ROLL", 15, 115);
      canvas.setFont(&fonts::FreeSansBold12pt7b);
      canvas.setTextDatum(TR_DATUM);
      canvas.drawString(String(a.hdg, 0), 218, 75);
      drawDegreeSymbol(222, 75 + 3);
      canvas.drawString(String(a.baroRate) + " ft/m", 225, 95);
      canvas.drawString(String(a.roll, 1), 218, 115);
      drawDegreeSymbol(222, 115 + 3);
      break;
    }
    case 2: {
      canvas.setFont(&fonts::FreeSans9pt7b);
      canvas.setTextDatum(TL_DATUM);
      canvas.drawString("MACH", 15, 75);
      canvas.drawString("IAS/TAS", 15, 95);
      canvas.drawString("GEOM ALT", 15, 115);
      canvas.setFont(&fonts::FreeSansBold12pt7b);
      canvas.setTextDatum(TR_DATUM);
      canvas.drawString(String(a.mach, 3), 225, 75);
      canvas.drawString(String(a.ias) + "/" + String(a.tas) + "kt", 225, 95);
      canvas.drawString(String(a.altGeom) + " ft", 225, 115);
      break;
    }
    case 3: {
      canvas.setFont(&fonts::FreeSans9pt7b);
      canvas.setTextDatum(TL_DATUM);
      canvas.drawString("AP ALT", 15, 75);
      canvas.drawString("AP HDG", 15, 95);
      canvas.drawString("AP MODE", 15, 115);
      canvas.setFont(&fonts::FreeSansBold12pt7b);
      canvas.setTextDatum(TR_DATUM);
      canvas.drawString(String(a.navAlt) + " ft", 225, 75);
      canvas.drawString(String(a.navHdg, 0), 218, 95);
      drawDegreeSymbol(222, 95 + 3);
      canvas.setFont(&fonts::FreeSansBold9pt7b);
      canvas.drawString(a.navModes, 225, 115);
      break;
    }
    case 4: {
      canvas.setFont(&fonts::FreeSans9pt7b);
      canvas.setTextDatum(TL_DATUM);
      canvas.drawString("SQK", 15, 75);
      canvas.drawString("ICAO", 15, 95);
      canvas.drawString("REG", 15, 115);
      canvas.setFont(&fonts::FreeSansBold12pt7b);
      canvas.setTextDatum(TR_DATUM);
      canvas.drawString(a.squawk, 225, 75);
      canvas.drawString(a.hex[0] ? a.hex : "N/A", 225, 95);
      canvas.drawString(a.reg[0] ? a.reg : "N/A", 225, 115);
      break;
    }
    case 5: {
      canvas.setFont(&fonts::FreeSans9pt7b);
      canvas.setTextDatum(TL_DATUM);
      canvas.drawString("OAT/TAT", 15, 75);
      canvas.drawString("WIND", 15, 95);
      canvas.drawString("RSSI", 15, 115);
      canvas.setFont(&fonts::FreeSansBold12pt7b);
      canvas.setTextDatum(TR_DATUM);
      int x_pos = 225;
      int y_pos = 75;
      String tat_val = String(a.tat);
      String oat_val = String(a.oat);
      canvas.drawString("C", x_pos, y_pos);
      x_pos -= canvas.textWidth("C");
      drawDegreeSymbol(x_pos, y_pos + 3);
      x_pos -= 4;
      canvas.drawString(tat_val, x_pos, y_pos);
      x_pos -= canvas.textWidth(tat_val);
      canvas.drawString("/", x_pos, y_pos);
      x_pos -= canvas.textWidth("/");
      canvas.drawString("C", x_pos, y_pos);
      x_pos -= canvas.textWidth("C");
      drawDegreeSymbol(x_pos, y_pos + 3);
      x_pos -= 4;
      canvas.drawString(oat_val, x_pos, y_pos);
      String windStr = String(a.wd);
      String speedStr = " / " + String(a.ws) + "kt";
      canvas.drawString(windStr, 225 - canvas.textWidth(speedStr), 95);
      drawDegreeSymbol(225 - canvas.textWidth(speedStr) + 4, 95 + 3);
      canvas.drawString(speedStr, 225, 95);
      canvas.drawString(String(a.rssi, 1) + " dBm", 225, 115);
      break;
    }
    case PAGE_TRACK: {
      drawTrackPage();
      break;
    }
  }
  drawFooter(true);
  canvas.pushSprite(0, 0);
}

// ---------------------------------------------------------------- data loading
// Takes the candidates from the last scan into ac[], keeps the selection and routes
void commitScan() {
  uint32_t selHex = (acCount > 0) ? ac[sel].hex32 : 0;

  // keep already fetched routes
  for (int i = 0; i < candCount; i++) {
    for (int j = 0; j < acCount; j++) {
      if (cand[i].hex32 == ac[j].hex32) {
        cand[i].routeTried = ac[j].routeTried;
        strcpy(cand[i].dep, ac[j].dep);
        strcpy(cand[i].arr, ac[j].arr);
        break;
      }
    }
  }

  // sort by actual distance
  for (int i = 1; i < candCount; i++) {
    Aircraft key = cand[i];
    int j = i - 1;
    while (j >= 0 && cand[j].distNm > key.distNm) { cand[j + 1] = cand[j]; j--; }
    cand[j + 1] = key;
  }

  for (int i = 0; i < candCount; i++) ac[i] = cand[i];
  acCount = candCount;
  for (int i = 0; i < blipNextCount; i++) blips[i] = blipNext[i];
  blipCount = blipNextCount;
  fixTime = millis();

  sel = 0;
  for (int i = 0; i < acCount; i++) {
    if (ac[i].hex32 == selHex) { sel = i; break; }
  }
}

// Wait time after an HTTP 429: exponential (20, 40, 80, 120 s), Retry-After wins if present
unsigned long rateLimitWaitMs(HTTPClient& http) {
  int n = consecutive429 > 0 ? consecutive429 - 1 : 0;
  if (n > 3) n = 3;
  unsigned long wait = RATE_LIMIT_BACKOFF_MIN << n;
  if (wait > RATE_LIMIT_BACKOFF_MAX) wait = RATE_LIMIT_BACKOFF_MAX;
  long sec = http.header("Retry-After").toInt();
  if (sec > 0) {
    if (sec < 5) sec = 5;
    if (sec > 300) sec = 300;
    wait = (unsigned long)sec * 1000UL;
  }
  return wait;
}

void fetchAircraftData() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[AIRCRAFT] ERROR: WiFi is not connected");
    return;
  }

  Serial.println();
  Serial.println("========== AIRCRAFT API ==========");
  updateHomePosition();
  buildUrls();
  Serial.printf("[AIRCRAFT] position %.5f, %.5f (%s)\n", homeLat, homeLon,
                posFromGps ? "GPS" : "saved in setup");

  if (acCount == 0 && !rateLimited) {
    canvas.fillScreen(TFT_BLACK);
    drawBatteryStatus();
    drawClock();
    canvas.setTextColor(TFT_WHITE);
    canvas.setTextDatum(MC_DATUM);
    canvas.setFont(&fonts::FreeSans9pt7b);
    canvas.drawString("Fetching aircraft...", 120, 70);
    canvas.pushSprite(0, 0);
  }

  bool usePrimary = millis() >= primaryBlockedUntil;

  // attempt 0 = preferred server; attempt 1 = fallback after a 429 from the primary
  for (int attempt = 0; attempt < 2; attempt++) {
    const String& url = usePrimary ? api_url : api_url_alt;
    Serial.print("[AIRCRAFT] URL: ");
    Serial.println(url);
    Serial.printf("[AIRCRAFT] heap before TLS: free=%u, largest block=%u\n",
                  (unsigned int)ESP.getFreeHeap(), (unsigned int)ESP.getMaxAllocHeap());

    WiFiClientSecure client;
    client.setInsecure();               // no certificate validation

    HTTPClient http;
    http.setTimeout(10000);
    http.setConnectTimeout(10000);
    http.setUserAgent(userAgent);
    http.setReuse(false);               // connection is fully closed after the request

    if (!http.begin(client, url)) {
      Serial.println("[AIRCRAFT] ERROR: http.begin() failed");
      return;
    }
    http.addHeader("Accept", "application/json");
    const char* hdrKeys[] = {"Transfer-Encoding", "Retry-After"};
    http.collectHeaders(hdrKeys, 2);

    int httpCode = http.GET();
    Serial.printf("[AIRCRAFT] HTTP status: %d, free heap: %u\n", httpCode,
                  (unsigned int)ESP.getFreeHeap());

    if (httpCode == 429) {
      consecutive429++;
      unsigned long wait = rateLimitWaitMs(http);
      Serial.printf("[AIRCRAFT] Rate limited (429) #%d, Retry-After='%s'\n", consecutive429,
                    http.header("Retry-After").c_str());
      if (usePrimary) {
        // block the primary server for a while and try the fallback right away
        primaryBlockedUntil = millis() + wait;
        usePrimary = false;
        http.end();
        Serial.println("[AIRCRAFT] switching to fallback server");
        continue;
      }
      // fallback is rate limited too: keep last data and wait
      pollInterval = wait;
      rateLimited = true;
      lastPollTime = millis();
      Serial.printf("[AIRCRAFT] retry in %lu s\n", wait / 1000);
      if (acCount == 0) displayCurrentPage();   // shows the "Rate limited" countdown
      http.end();
      break;
    }

    if (!usePrimary && httpCode != HTTP_CODE_OK) {
      // fallback failed (e.g. 403): do not wipe the screen, just wait and try again
      Serial.printf("[AIRCRAFT] fallback server failed (HTTP %d)\n", httpCode);
      Serial.println(http.getString());
      pollInterval = rateLimitWaitMs(http);
      rateLimited = true;
      lastPollTime = millis();
      if (acCount == 0) displayCurrentPage();
      http.end();
      break;
    }

    rateLimited = false;

    if (httpCode == HTTP_CODE_OK) {
      consecutive429 = 0;
      candCount = 0;
      blipNextCount = 0;
      scanObjs = scanParsed = scanNoPos = 0;
      scanLastErr = "";
      statsBeginScan();
      bool ok = scanResponse(http);
      Serial.printf("[AIRCRAFT] scan ok=%d, objects=%d, parsed=%d, no-pos=%d, candidates=%d, heap=%u\n",
                    ok, scanObjs, scanParsed, scanNoPos, candCount, (unsigned int)ESP.getFreeHeap());
      if (scanLastErr.length()) Serial.println("[AIRCRAFT] last JSON error: " + scanLastErr);

      if (!ok) {
        Serial.println("[AIRCRAFT] stream/JSON ERROR");
        canvas.fillScreen(TFT_DARKCYAN);
        drawBatteryStatus();
        drawClock();
        canvas.setTextColor(TFT_WHITE);
        canvas.setTextDatum(MC_DATUM);
        canvas.setFont(&fonts::FreeSansBold12pt7b);
        canvas.drawString("JSON PARSE ERROR", 120, 67);
        canvas.pushSprite(0, 0);
        pollInterval = ERROR_POLL_INTERVAL;
      } else {
        commitScan();

        if (acCount > 0) {
          lastActivityTime = millis();
          bool alert = checkAlerts();
          if (!alert && ac[0].hex32 != lastNearestHex) playNewAircraftSound();
          lastNearestHex = ac[0].hex32;
          displayCurrentPage();        // show first, route is fetched in loop()
          routePending = true;
          pollInterval = SHORT_POLL_INTERVAL;
        } else {
          Serial.println("[AIRCRAFT] no aircraft found.");
          lastNearestHex = 0;
          displayCurrentPage();
          pollInterval = NO_AIRCRAFT_POLL_INTERVAL;
        }
      }

    } else {
      Serial.println("[AIRCRAFT] HTTP REQUEST FAILED");
      Serial.println(http.getString());

      acCount = 0;
      blipCount = 0;
      sel = 0;
      lastNearestHex = 0;

      canvas.fillScreen(TFT_MAROON);
      drawBatteryStatus();
      drawClock();
      canvas.setTextColor(TFT_WHITE);
      canvas.setTextDatum(MC_DATUM);
      canvas.setFont(&fonts::FreeSansBold12pt7b);
      canvas.drawString("API ERROR", 120, 50);
      canvas.setFont(&fonts::FreeSans9pt7b);
      canvas.drawString("HTTP Code: " + String(httpCode), 120, 80);
      canvas.pushSprite(0, 0);

      pollInterval = ERROR_POLL_INTERVAL;
    }

    http.end();
    break;
  }

  Serial.println("========== END AIRCRAFT API ==========");
}
