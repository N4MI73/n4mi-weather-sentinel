// Weather Sentinel — Navigation + secondary-page content (static/dummy
// data, no server yet).
//
// 5 pages in a tap-to-advance cycle: Now, Wind & Rain, Lightning,
// NWS Alerts, Status. Settings is reached only by long-press (~1s) from
// any page, NOT part of the tap cycle. Auto-returns to Now after 45s
// idle on any other page. A persistent, condensed alert strip is docked
// at the bottom of every page except Now (which keeps its own existing
// full-detail footer/banner, already approved) and Settings.
//
// Navigation mechanism (tap-cycle, long-press, idle timeout, persistent
// strip) is CONFIRMED WORKING on real hardware. The four secondary pages
// now have real content matching their approved mockups -- still using
// hardcoded/dummy values (no server exists yet), but the actual layouts
// Dan approved rather than placeholders.

#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include <time.h>
#include "wifi_credentials.h"

// Forward declaration: formatCurrentTime() is defined later (grouped with
// the rest of the NTP code, near connectWiFi()), but drawNowPage() above
// that point needs to call it.
String formatCurrentTime();
String formatEpochLocal(long epoch);  // defined next to formatCurrentTime()

extern uint8_t DAY_BRIGHTNESS;
extern uint8_t NIGHT_BRIGHTNESS;
extern int NIGHT_START_HOUR;
extern int NIGHT_START_MINUTE;
extern int NIGHT_END_HOUR;
extern int NIGHT_END_MINUTE;
extern int lastAppliedBrightnessMode;
uint8_t stepBrightness(uint8_t current, int direction);
void stepScheduleTime(int &hour, int &minute, int direction);
void applyCurrentBrightnessImmediately();

enum Page {
  PAGE_NOW = 0,
  PAGE_WIND_RAIN,
  PAGE_LIGHTNING,
  PAGE_NWS_ALERTS,
  PAGE_STATUS,
  PAGE_SETTINGS
};
static const int NUM_CYCLE_PAGES = 5;  // Settings excluded from the cycle

// Forward declaration: the Settings drawing code (below) needs this, but
// its real definition (the render-timing wrapper) lives later in the
// file -- same reason and pattern as the forward declarations above.
void renderPage(Page p);

static Page currentPage = PAGE_NOW;


// ---- Real fetched conditions (all five screens use this now) ----
bool tempestAvailable = false;
float tempestTempF = 0;
float tempestHumidityPct = 0;
float tempestWindMph = 0;
float tempestGustMph = 0;
float tempestRainRateInHr = 0;     // WeatherFlow-style rate: latest minute x 60
String tempestRainRateLevel = "";  // "none"/"very_light"/.../"extreme"; "" = not sent
bool tempestFeelsLikeValid = false;
float tempestFeelsLikeF = 0;
float tempestPressureInHg = 0;
String tempestWindDir = "";

bool lightningActive = false;
String lightningLevel = "none";
float lightningClosestDistanceMi = 0;
long lightningMostRecentEpoch = 0;  // seconds; 0 = none/unknown
// The server's own lightning scope, shown in the Lightning page label so
// the label can never disagree with what the server is actually filtering
// on (the radius can be widened for testing via a Portainer variable).
float lightningFilterRadiusMi = 10;
int lightningWindowMinutes = 10;
int lightningStrikeCountRecent = 0;

bool nwsAvailable = false;
int nwsAlertCount = 0;
String nwsFirstAlertEvent = "";
String nwsFirstAlertExpires = "";
String nwsFirstAlertInstruction = "";
String nwsFirstAlertId = "";
String nwsFirstAlertWhat = "";     // NWS's one-line headline, minus its time phrase
String nwsFirstAlertHazard = "";   // the "HAZARD..." / "WHAT..." line
bool nwsFirstAlertAcknowledged = false;
bool nwsFirstAlertNeedsAlert = false;

bool hasEverFetchedSuccessfully = false;
bool timeSynced = false;  // set by syncTimeNTP(), defined near connectWiFi()
bool lastFetchSucceeded = false;  // distinct from the above -- this can
                                    // flip back to false if the backend
                                    // goes down AFTER working once; used
                                    // for the Status page's Backend row
unsigned long lastSuccessfulFetchMillis = 0;

const unsigned long FETCH_INTERVAL_MS = 60000;  // 60s -- matches obs_st's
                                                  // own real-world cadence;
                                                  // polling faster wouldn't
                                                  // surface newer data
unsigned long lastFetchAttemptMillis = 0;

// ---- Data freshness (stale-data guardrail) ----
// When a fetch fails, the globals above keep their LAST GOOD values --
// which is right for showing an old temperature, but wrong for "no
// alerts" or "no lightning": those must only ever be shown from a
// current check. Everything that decides between clear / alert /
// unknown goes through the helpers below instead of reading the raw
// globals directly, so the Now page, the detail pages, and the
// persistent strip can never disagree with each other.
//
// Both values are PLACEHOLDERS to tune on real hardware.
// DATA_STALE_MS: fetches run every 60s, so 100s tolerates one missed
// fetch and flags after two in a row.
// LIGHTNING_HOLD_MS: how long a retained "lightning active" reading
// stays visible while the backend is unreachable (mirrors the server's
// 10-minute strike window); after that it becomes "unknown".
const unsigned long DATA_STALE_MS = 100000;
const unsigned long LIGHTNING_HOLD_MS = 600000;

unsigned long dataAgeMs() {
  if (!hasEverFetchedSuccessfully) return (unsigned long)-1;  // max value
  return millis() - lastSuccessfulFetchMillis;
}

bool backendDataFresh() {
  return hasEverFetchedSuccessfully && dataAgeMs() < DATA_STALE_MS;
}

enum NwsView { NWS_VIEW_UNKNOWN, NWS_VIEW_CLEAR, NWS_VIEW_ALERT, NWS_VIEW_ALERT_STALE };

// An active alert is ALWAYS shown, even when the NWS check isn't
// current (marked stale) -- hiding a possible warning is worse than
// showing one that may have just expired. "Clear" requires a fresh
// backend AND the server reporting its own NWS poll as successful.
NwsView currentNwsView() {
  bool checkCurrent = backendDataFresh() && nwsAvailable;
  if (nwsAlertCount > 0) return checkCurrent ? NWS_VIEW_ALERT : NWS_VIEW_ALERT_STALE;
  return checkCurrent ? NWS_VIEW_CLEAR : NWS_VIEW_UNKNOWN;
}

enum LightningView { LIGHTNING_UNKNOWN, LIGHTNING_CLEAR, LIGHTNING_ACTIVE };

// "Clear" requires fresh data AND Tempest reporting available, since
// the strike listener shares the same UDP source as the observations.
// NOTE: this is only as good as the server's own definition of
// tempest.available -- if that flag never goes false once data has
// been received, a silent hub still won't be caught here. To be
// checked against server code.
LightningView currentLightningView() {
  bool current = backendDataFresh() && tempestAvailable;
  if (current) return lightningActive ? LIGHTNING_ACTIVE : LIGHTNING_CLEAR;
  if (hasEverFetchedSuccessfully && lightningActive && dataAgeMs() < LIGHTNING_HOLD_MS) {
    return LIGHTNING_ACTIVE;
  }
  return LIGHTNING_UNKNOWN;
}

String formatAgeString() {
  if (!hasEverFetchedSuccessfully) {
    return "no data yet";
  }
  unsigned long elapsedSec = (millis() - lastSuccessfulFetchMillis) / 1000;
  if (elapsedSec < 60) {
    return String(elapsedSec) + "s ago";
  }
  return String(elapsedSec / 60) + "m ago";
}

// Formats an NWS timestamp such as "2026-09-21T16:30:00-04:00" for display
// next to the local clock: "4:30 PM" if it is today, "Tue 4:30 AM" if it
// is another day (an overnight alert must not read as if it ends "today").
// NWS already sends the zone's own local offset, so the digits are used as
// written -- no timezone math beyond the day-of-week lookup. `nowEpoch` is
// passed in (0 = clock not synced) so this can be tested exactly.
String formatAlertTime(const String &iso, time_t nowEpoch) {
  int t = iso.indexOf('T');
  if (t < 10 || (int)iso.length() < t + 6) return "unknown time";
  int year = iso.substring(0, 4).toInt();
  int mon  = iso.substring(5, 7).toInt();
  int day  = iso.substring(8, 10).toInt();
  int hh   = iso.substring(t + 1, t + 3).toInt();
  int mm   = iso.substring(t + 4, t + 6).toInt();
  if (year < 2000 || mon < 1 || mon > 12 || day < 1 || day > 31 || hh > 23 || mm > 59) {
    return "unknown time";
  }

  int h12 = hh % 12;
  if (h12 == 0) h12 = 12;
  char clock[20];
  snprintf(clock, sizeof(clock), "%d:%02d %s", h12, mm, hh >= 12 ? "PM" : "AM");

  if (nowEpoch <= 0) return String(clock);   // clock not synced: time only

  struct tm when = {};
  when.tm_year = year - 1900;
  when.tm_mon = mon - 1;
  when.tm_mday = day;
  when.tm_hour = hh;
  when.tm_min = mm;
  when.tm_isdst = -1;
  mktime(&when);                             // fills in tm_wday

  struct tm today;
  localtime_r(&nowEpoch, &today);
  if (today.tm_year == year - 1900 && today.tm_mon == mon - 1 && today.tm_mday == day) {
    return String(clock);
  }
  static const char *const DAYS[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  return String(DAYS[when.tm_wday]) + " " + String(clock);
}

String formatAlertTimeNow(const String &iso) {
  return formatAlertTime(iso, timeSynced ? time(nullptr) : (time_t)0);
}

String formatUptimeString() {
  // Purely local -- time since boot, needs neither the server nor NTP.
  unsigned long totalSec = millis() / 1000;
  unsigned long days = totalSec / 86400;
  unsigned long hours = (totalSec % 86400) / 3600;
  unsigned long mins = (totalSec % 3600) / 60;
  if (days > 0) {
    return String(days) + "d " + String(hours) + "h " + String(mins) + "m";
  } else if (hours > 0) {
    return String(hours) + "h " + String(mins) + "m";
  }
  return String(mins) + "m";
}

static const unsigned long LONG_PRESS_MS = 1000;
static const unsigned long IDLE_RETURN_MS = 45000;

static unsigned long touchStartTime = 0;
static bool longPressHandled = false;
static unsigned long lastActivityTime = 0;

// Colors (same palette as the approved Now-screen mockups)
static uint16_t COLOR_BG, COLOR_TEXT_PRIMARY, COLOR_TEXT_SECONDARY, COLOR_TEXT_DIM,
    COLOR_SEPARATOR, COLOR_NWS_CLEAR_DOT, COLOR_NWS_CLEAR_TEXT,
    COLOR_LIGHTNING_BG, COLOR_LIGHTNING_BORDER, COLOR_LIGHTNING_TEXT,
    COLOR_WARNING_BG, COLOR_WARNING_TEXT_HEADLINE, COLOR_WARNING_TEXT_DETAIL,
    COLOR_LABEL, COLOR_STATUS_BAD,
    // Feels-like temperature bands (Session 9). Cold colors and the
    // orange band are new; yellow/amber reuses the lightning color and
    // red reuses COLOR_STATUS_BAD, both already visually correct for
    // their meaning. An initial attempt reused COLOR_WARNING_TEXT_DETAIL
    // for orange, but rendered as a dusty pink too close in hue to red
    // to read as a distinct, graduated step -- replaced with a real
    // orange after a side-by-side render caught it. "Plain" (50-79F) is
    // just COLOR_TEXT_SECONDARY, unchanged -- no new constant needed.
    COLOR_FEELS_COLD, COLOR_FEELS_COOL, COLOR_FEELS_ORANGE,
    // Simulation-mode overlay. Deliberately a hue used nowhere else in
    // the palette (not red/amber/green), so it can never be confused
    // with a real alert color -- chosen on the server-driven design
    // that the device needs almost no simulation-specific rendering,
    // just this one unmistakable overlay.
    COLOR_SIMULATION;

void initColors() {
  COLOR_BG               = M5.Display.color565(0x04, 0x04, 0x04);
  COLOR_TEXT_PRIMARY     = M5.Display.color565(0xe8, 0xe8, 0xe8);
  COLOR_TEXT_SECONDARY   = M5.Display.color565(0xaa, 0xaa, 0xaa);
  COLOR_TEXT_DIM         = M5.Display.color565(0x4a, 0x4a, 0x4a);
  COLOR_SEPARATOR        = M5.Display.color565(0x1c, 0x1c, 0x1c);
  COLOR_NWS_CLEAR_DOT    = M5.Display.color565(0x3a, 0x6b, 0x3a);
  COLOR_NWS_CLEAR_TEXT   = M5.Display.color565(0x6a, 0x9a, 0x6a);
  COLOR_LIGHTNING_BG     = M5.Display.color565(0x2a, 0x1d, 0x05);
  COLOR_LIGHTNING_BORDER = M5.Display.color565(0x99, 0x66, 0x00);
  COLOR_LIGHTNING_TEXT   = M5.Display.color565(0xe0, 0xa8, 0x35);
  COLOR_WARNING_BG       = M5.Display.color565(0x5c, 0x0e, 0x0e);
  COLOR_WARNING_TEXT_HEADLINE = M5.Display.color565(0xff, 0xb3, 0xb3);
  COLOR_WARNING_TEXT_DETAIL   = M5.Display.color565(0xe0, 0x8a, 0x8a);
  COLOR_LABEL             = M5.Display.color565(0x6a, 0x6a, 0x6a);
  COLOR_STATUS_BAD        = M5.Display.color565(0xcc, 0x33, 0x33);
  COLOR_FEELS_COLD        = M5.Display.color565(0x5a, 0x9a, 0xe0);
  COLOR_FEELS_COOL        = M5.Display.color565(0x8a, 0xc8, 0xe0);
  COLOR_FEELS_ORANGE      = M5.Display.color565(0xe0, 0x7a, 0x1a);
  COLOR_SIMULATION        = M5.Display.color565(0xc0, 0x4e, 0xe0);
}

// ---- Now screen ----

// Small Wi-Fi fan (dot + three arcs), status-bar style. It means exactly
// ONE thing: the device is associated with the router. It says nothing
// about the server, Tempest, or NWS -- those have their own indicators
// (the data-age text turns red when the backend goes quiet; the NWS
// footer shows unknown). Dim green when connected, gray when not.
// Drawn from plain rectangles so it doesn't depend on any arc API.
void drawWifiGlyph(int cx, int cy, bool connected) {
  uint16_t color = connected ? COLOR_NWS_CLEAR_TEXT : COLOR_TEXT_DIM;
  M5.Display.fillCircle(cx, cy, 2, color);
  const int radii[3] = {6, 10, 14};
  for (int i = 0; i < 3; i++) {
    for (int a = -40; a <= 40; a += 4) {
      float rad = a * 0.0174533f;
      int px = cx + (int)lroundf(radii[i] * sinf(rad));
      int py = cy - (int)lroundf(radii[i] * cosf(rad));
      M5.Display.fillRect(px - 1, py - 1, 2, 2, color);
    }
  }
}

void drawCommonHeader(const char *timeStr, const char *ageStr) {
  M5.Display.fillScreen(COLOR_BG);
  M5.Display.setTextDatum(top_right);
  // Age text turns red once data is stale, so old numbers can't pass
  // for current ones at a glance.
  M5.Display.setTextColor(backendDataFresh() ? COLOR_TEXT_DIM : COLOR_STATUS_BAD, COLOR_BG);
  M5.Display.setTextSize(1);
  M5.Display.drawString(ageStr, 308, 8);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(COLOR_TEXT_PRIMARY, COLOR_BG);
  M5.Display.setTextSize(2);
  M5.Display.drawString(timeStr, 16, 20);
  // Top-right, on the clock row, below the data-age text.
  drawWifiGlyph(294, 36, WiFi.status() == WL_CONNECTED);
}

void drawConditions(const char *tempStr, const char *feelsStr, const char *humidStr,
                     const char *windStr, const char *gustStr, const char *rainStr,
                     uint16_t feelsColor) {
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(COLOR_TEXT_PRIMARY, COLOR_BG);
  M5.Display.setTextSize(4);
  M5.Display.drawString(tempStr, 16, 55);
  M5.Display.setTextSize(2);
  // Feels-like gets its own color (the band color); humidity stays plain.
  // Air temperature above is deliberately NOT colored -- Dan's Session 8
  // choice was to color feels-like specifically, since that is the number
  // that changes what you'd actually do before heading outside.
  M5.Display.setTextColor(feelsColor, COLOR_BG);
  M5.Display.drawString(feelsStr, 150, 58);
  M5.Display.setTextColor(COLOR_TEXT_SECONDARY, COLOR_BG);
  M5.Display.drawString(humidStr, 150, 78);
  M5.Display.drawFastHLine(16, 98, 288, COLOR_SEPARATOR);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(COLOR_TEXT_SECONDARY, COLOR_BG);
  M5.Display.drawString(windStr, 16, 106);
  M5.Display.drawString(gustStr, 168, 106);
  M5.Display.drawString(rainStr, 16, 128);
}

// Measured on the real display: text size 2 is 12px per character, size
// 1 is 6px. (Earlier layout code assumed narrower text and clipped.)
const int GLYPH_W_SIZE2 = 12;

// "Thunderstorm" is the common offender ("Severe Thunderstorm Warning"
// is 27 characters). NWS itself abbreviates it "T-storm".
String shortenEventName(const String &name) {
  String s = name;
  s.replace("Thunderstorm", "T-storm");
  return s;
}

// Size 2 if the text fits in maxWidth pixels, else size 1. A long alert
// name shrinks; it never runs off the screen.
int eventTextSize(const String &text, int maxWidth) {
  return ((int)text.length() * GLYPH_W_SIZE2 <= maxWidth) ? 2 : 1;
}

void drawNwsFooterClear() {
  M5.Display.drawFastHLine(0, 194, 320, COLOR_SEPARATOR);
  M5.Display.fillCircle(20, 212, 4, COLOR_NWS_CLEAR_DOT);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(COLOR_NWS_CLEAR_TEXT, COLOR_BG);
  M5.Display.setTextSize(2);
  M5.Display.drawString("NO ACTIVE ALERTS", 34, 204);
}

void drawLightningBanner(const String &levelText) {
  M5.Display.fillRoundRect(12, 150, 296, 34, 6, COLOR_LIGHTNING_BG);
  M5.Display.drawRoundRect(12, 150, 296, 34, 6, COLOR_LIGHTNING_BORDER);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(COLOR_LIGHTNING_TEXT, COLOR_LIGHTNING_BG);
  M5.Display.setTextSize(2);
  M5.Display.drawString(levelText, 26, 158);
}

// The Now-screen alert footer. Defined once and shared by the drawing code
// and the touch handler, so the region drawn and the region that reacts to
// a tap can never drift apart (same pattern as the acknowledge bar).
const int NOW_FOOTER_Y = 196;
const int NOW_FOOTER_H = 44;

void drawNwsWarningFooter(const String &eventText, const String &untilText) {
  M5.Display.fillRect(0, NOW_FOOTER_Y, 320, NOW_FOOTER_H, COLOR_WARNING_BG);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(COLOR_WARNING_TEXT_HEADLINE, COLOR_WARNING_BG);
  String eventName = shortenEventName(eventText);
  int eventSize = eventTextSize(eventName, 300);   // x = 16 .. 316
  M5.Display.setTextSize(eventSize);
  M5.Display.drawString(eventName, 16, eventSize == 2 ? 202 : 206);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(COLOR_WARNING_TEXT_DETAIL, COLOR_WARNING_BG);
  M5.Display.drawString(untilText, 16, 224);
}

void drawNwsFooterUnknown() {
  // Distinct from both "clear" (green) and "warning" (red), per the
  // project's own core guardrail: NWS being unreachable must NEVER be
  // shown as "NO ACTIVE ALERTS" -- that string is reserved for a real,
  // fresh, successful check. Neutral gray, not calming, not urgent.
  M5.Display.drawFastHLine(0, 194, 320, COLOR_SEPARATOR);
  M5.Display.fillCircle(20, 212, 4, COLOR_TEXT_DIM);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(COLOR_TEXT_DIM, COLOR_BG);
  M5.Display.setTextSize(2);
  M5.Display.drawString("NWS STATUS UNKNOWN", 34, 204);
}

// Feels-like colour bands, tuned by Dan to how these temperatures
// actually feel to him in Georgia (Session 9; supersedes the original
// Session 8 proposal). Bands: <=39 blue, 40-54 light blue, 55-74 plain
// (no color change), 75-86 yellow, 87-94 orange, >=95 red. A 1F buffer
// prevents flicker at a boundary: moving to an adjacent band requires
// crossing 1F PAST that band's edge, not just touching it.
//
// Dan's stated ranges overlapped by 1F at the yellow/orange edge ("75-87
// yellow", "87-94 orange"); resolved as orange starting at 87 (matching
// his explicit "87-94"), so yellow is 75-86. Flagged for Dan rather than
// silently guessed -- worth a one-degree correction if that's not what
// he meant, though the 1F buffer already makes a single degree here
// nearly imperceptible in practice.
//
// FEELS_BAND_COUNT bands, indices 0..5; FEELS_BAND_BOUNDARIES holds the
// 5 edges between them (40, 55, 75, 87, 95) -- each value is the first
// degree belonging to the band above it.
const int FEELS_BAND_COUNT = 6;
const float FEELS_BAND_BOUNDARIES[FEELS_BAND_COUNT - 1] = {40, 55, 75, 87, 95};
const float FEELS_BAND_BUFFER = 1.0f;

// Stateful on purpose: the caller passes in the previously-shown band
// and gets back the (possibly unchanged) new one. Kept as a plain
// function taking/returning an int, not a hidden static, so it can be
// tested directly with arbitrary sequences of readings.
int feelsLikeBand(float feelsF, int lastBand) {
  int band = lastBand;
  // Step down while clearly (by the buffer) below the current band's
  // lower edge; step up while clearly above its upper edge. Each step
  // re-checks against the NEW current band's own edge, so a large jump
  // (e.g. after a restart, or a big real swing) still lands correctly
  // rather than only ever moving one band per reading.
  while (band > 0 && feelsF < FEELS_BAND_BOUNDARIES[band - 1] - FEELS_BAND_BUFFER) {
    band--;
  }
  while (band < FEELS_BAND_COUNT - 1 && feelsF >= FEELS_BAND_BOUNDARIES[band] + FEELS_BAND_BUFFER) {
    band++;
  }
  return band;
}

uint16_t feelsLikeBandColor(int band) {
  switch (band) {
    case 0: return COLOR_FEELS_COLD;      // <=39F
    case 1: return COLOR_FEELS_COOL;      // 40-54F
    case 2: return COLOR_TEXT_SECONDARY;  // 55-74F, plain
    case 3: return COLOR_LIGHTNING_TEXT;  // 75-86F, yellow/amber (reused)
    case 4: return COLOR_FEELS_ORANGE;    // 87-94F, orange
    default: return COLOR_STATUS_BAD;     // >=95F, red (reused)
  }
}

// Tempest's rain-intensity words, from the server's rain_rate_level. An
// unknown or missing level shows "--" rather than a guess.
const char *rainLevelLabel(const String &level) {
  if (level == "none") return "None";
  if (level == "very_light") return "Very Light";
  if (level == "light") return "Light";
  if (level == "moderate") return "Moderate";
  if (level == "heavy") return "Heavy";
  if (level == "very_heavy") return "Very Heavy";
  if (level == "extreme") return "Extreme";
  return "--";
}

// Picks text size 3 if the string fits in maxWidth pixels, else size 2.
// Deliberately conservative: assumes 6px-wide glyphs at size 1 (18px at
// size 3), so it errs toward the smaller size rather than overflowing.
int fitTextSize(const char *text, int maxWidth) {
  return ((int)strlen(text) * 18 <= maxWidth) ? 3 : 2;
}

// Persisted across calls so feelsLikeBand()'s hysteresis has continuity
// from one fetch to the next; -1 means "no reading shown yet", which
// feelsLikeBand() treats as band 0 on the first real call (an initial
// snap to the correct band, not eased in -- there's nothing to flicker
// against yet).
int lastFeelsLikeBand = -1;

// Simulation / test mode (v1.0 requirement, server-driven -- see the
// server's own comment block for the full design). The device does
// almost nothing special: it reads one flag and draws one unmistakable
// overlay on top of whatever the page would show anyway. Approved
// treatment (Session 10 mockup): a magenta border framing the full
// screen plus a small corner tag, drawn as a pure overlay AFTER the
// page's own content so it can never disturb any page's existing
// layout math.
bool simulationActive = false;

void drawNowPage() {
  // Real clock now comes from NTP sync (syncTimeNTP(), called once at
  // boot) -- shows "--:--" if sync never succeeded, same placeholder as
  // before that was purely hardcoded.
  String ageStr = formatAgeString();
  String timeStr = formatCurrentTime();
  drawCommonHeader(timeStr.c_str(), ageStr.c_str());

  M5.Display.setTextDatum(top_left);
  if (!tempestAvailable) {
    // Distinct "no data" state -- never show zeros as if they were real.
    M5.Display.setTextColor(COLOR_TEXT_DIM, COLOR_BG);
    M5.Display.setTextSize(2);
    const char *msg = hasEverFetchedSuccessfully
                           ? "Tempest data unavailable"
                           : "Waiting for first data...";
    M5.Display.drawString(msg, 16, 60);
  } else {
    char tempStr[8], feelsStr[16], humidStr[20], windStr[20], gustStr[20], rainStr[28];
    snprintf(tempStr, sizeof(tempStr), "%.0fF", tempestTempF);
    // Feels-like comes from the server (WeatherFlow's own definition). If
    // the server didn't send one, the slot stays blank rather than
    // showing a made-up number.
    uint16_t feelsColor = COLOR_TEXT_SECONDARY;
    if (tempestFeelsLikeValid) {
      snprintf(feelsStr, sizeof(feelsStr), "Feels %.0fF", tempestFeelsLikeF);
      // First-ever reading (lastFeelsLikeBand == -1) starts the hysteresis
      // from band 0; every later call starts from the previously-shown
      // band, so a boundary must be genuinely cleared (not just touched)
      // to change color.
      // Color is decided from the ROUNDED value -- the same whole number
      // shown on screen -- not the raw decimal underneath. Confirmed
      // needed on real hardware (Session 9): a raw value like 95.6
      // rounds to display "96F" but had not yet cleared the buffered
      // 96.0 threshold, so the color stayed orange while the number on
      // screen said 96 -- confusing, since the two should always agree
      // on what "96" means. Rounding first makes them consistent.
      float feelsRounded = roundf(tempestFeelsLikeF);
      int fromBand = (lastFeelsLikeBand < 0) ? 0 : lastFeelsLikeBand;
      lastFeelsLikeBand = feelsLikeBand(feelsRounded, fromBand);
      feelsColor = feelsLikeBandColor(lastFeelsLikeBand);
    } else {
      feelsStr[0] = '\0';
    }
    snprintf(humidStr, sizeof(humidStr), "%.0f%% humidity", tempestHumidityPct);
    snprintf(windStr, sizeof(windStr), "Wind %.0f mph", tempestWindMph);
    snprintf(gustStr, sizeof(gustStr), "Gust %.0f mph", tempestGustMph);
    // What is happening NOW: the rain RATE in Tempest's own words, not an
    // accumulation. Numeric detail lives on the Wind & Rain page.
    snprintf(rainStr, sizeof(rainStr), "Rain: %s", rainLevelLabel(tempestRainRateLevel));
    drawConditions(tempStr, feelsStr, humidStr, windStr, gustStr, rainStr, feelsColor);
  }

  if (currentLightningView() == LIGHTNING_ACTIVE) {
    // "Frequent lightning nearby" is 25 characters = 300px at size 2 and
    // overflowed this banner; "nearby" is implied by the 10-mile scope.
    String bannerText = (lightningLevel == "frequent")
                             ? "Frequent lightning"
                             : "Lightning nearby";
    drawLightningBanner(bannerText);
  }

  NwsView nwsView = currentNwsView();
  if (nwsView == NWS_VIEW_UNKNOWN) {
    drawNwsFooterUnknown();
  } else if (nwsView == NWS_VIEW_CLEAR) {
    drawNwsFooterClear();
  } else {
    String untilText = "Until " + formatAlertTimeNow(nwsFirstAlertExpires) +
                        (nwsView == NWS_VIEW_ALERT_STALE ? " -- status not current"
                                                          : " -- tap for details");
    drawNwsWarningFooter(nwsFirstAlertEvent, untilText);
  }
}

// ---- Persistent slim alert strip (every page except Now and Settings) ----
// A condensed one-line summary of the same condition, so an active alert
// is never lost while browsing -- without spending the vertical space
// Now's dedicated footer/banner uses, which secondary pages need for
// their own content instead.

// Shared by drawPersistentStrip(), drawAckPrompt(), and the touch
// handler in loop() -- defined once so the drawn region and the
// touch-detection region can never drift out of sync with each other.
const int BOTTOM_BAR_Y = 214;
const int BOTTOM_BAR_H = 26;

void drawPersistentStrip() {
  uint16_t bg, textColor;
  String msg;

  // Priority order when more than one thing is true at once: an active
  // NWS alert outranks a local lightning event, matching the "official
  // alerts are more urgent than local sensor events" principle used
  // elsewhere in this project. NWS-unreachable gets its own neutral
  // state too, for the same reason the Now screen and NWS Alerts page
  // both distinguish it from a genuine "clear".
  NwsView nwsView = currentNwsView();
  if (nwsView == NWS_VIEW_ALERT || nwsView == NWS_VIEW_ALERT_STALE) {
    bg = COLOR_WARNING_BG;
    textColor = COLOR_WARNING_TEXT_HEADLINE;
    msg = nwsFirstAlertEvent;
    if (nwsView == NWS_VIEW_ALERT_STALE) msg += " (status not current)";
  } else if (WiFi.status() != WL_CONNECTED) {
    // Checked directly against WiFi.status(), not inferred from a stale
    // fetch -- so this can say "Wi-Fi" specifically rather than the
    // generic "unknown" a dead backend would also produce. An active
    // alert still always wins (above), matching the rule that an alert
    // is never hidden even when everything else is failing.
    bg = COLOR_BG;
    textColor = COLOR_STATUS_BAD;
    msg = "Wi-Fi disconnected";
  } else if (currentLightningView() == LIGHTNING_ACTIVE) {
    bg = COLOR_LIGHTNING_BG;
    textColor = COLOR_LIGHTNING_TEXT;
    msg = (lightningLevel == "frequent") ? "Frequent lightning nearby" : "Lightning nearby";
  } else if (nwsView == NWS_VIEW_UNKNOWN) {
    // Wi-Fi is fine (checked above) but the server or NWS itself isn't
    // answering -- a different problem to troubleshoot than "Wi-Fi
    // disconnected" above, so it keeps its own distinct wording rather
    // than collapsing into one generic "unknown" message.
    bg = COLOR_BG;
    textColor = COLOR_TEXT_DIM;
    msg = "Server unreachable";
  } else {
    bg = COLOR_BG;
    textColor = COLOR_NWS_CLEAR_TEXT;
    msg = "No active alerts";
  }

  M5.Display.fillRect(0, BOTTOM_BAR_Y, 320, BOTTOM_BAR_H, bg);
  M5.Display.drawFastHLine(0, BOTTOM_BAR_Y, 320, COLOR_SEPARATOR);
  M5.Display.setTextDatum(middle_left);
  M5.Display.setTextColor(textColor, bg);
  M5.Display.setTextSize(1);
  M5.Display.drawString(msg, 12, BOTTOM_BAR_Y + BOTTOM_BAR_H / 2);
}

void drawAckPrompt() {
  // Replaces the persistent strip's usual space on the NWS Alerts page
  // specifically while there's an active alert needing acknowledgement
  // -- the ack action is more useful here than the strip's normal
  // (redundant, since the alert's own detail is already on screen)
  // summary text. Same region as the strip so the touch handler in
  // loop() only needs one consistent rectangle to check against.
  M5.Display.fillRect(0, BOTTOM_BAR_Y, 320, BOTTOM_BAR_H, COLOR_WARNING_BG);
  M5.Display.drawFastHLine(0, BOTTOM_BAR_Y, 320, COLOR_SEPARATOR);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(COLOR_WARNING_TEXT_HEADLINE, COLOR_WARNING_BG);
  M5.Display.setTextSize(2);
  M5.Display.drawString("TAP TO ACKNOWLEDGE", 160, BOTTOM_BAR_Y + BOTTOM_BAR_H / 2);
}

// ---- Secondary pages: real content, matching approved mockups ----
// Settings has no approved mockup yet, so it stays a placeholder.

// Shared header for all four secondary pages: title + 5-dot page-position
// indicator (matches every approved mockup). pageIndex is 0-based across
// all 5 cycle pages (Now=0), so the dot for pageIndex lights up.
void drawSecondaryHeader(const char *title, int pageIndex) {
  M5.Display.fillScreen(COLOR_BG);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(COLOR_TEXT_PRIMARY, COLOR_BG);
  M5.Display.setTextSize(3);
  M5.Display.drawString(title, 16, 10);

  int dotX[5] = {240, 254, 268, 282, 296};
  for (int i = 0; i < 5; i++) {
    if (i == pageIndex) {
      M5.Display.fillCircle(dotX[i], 20, 3, COLOR_TEXT_PRIMARY);
    } else {
      M5.Display.fillCircle(dotX[i], 20, 3, COLOR_TEXT_DIM);
    }
  }

  M5.Display.drawFastHLine(16, 38, 288, COLOR_SEPARATOR);
}

void drawWindRainPage() {
  drawSecondaryHeader("Wind & Rain", PAGE_WIND_RAIN);

  M5.Display.setTextDatum(top_left);

  if (!tempestAvailable) {
    M5.Display.setTextColor(COLOR_TEXT_DIM, COLOR_BG);
    M5.Display.setTextSize(2);
    M5.Display.drawString(hasEverFetchedSuccessfully ? "Tempest data unavailable"
                                                       : "Waiting for first data...",
                           16, 60);
    return;
  }

  char windLabel[16], windStr[20], gustStr[20];
  snprintf(windLabel, sizeof(windLabel), "WIND (%s)", tempestWindDir.c_str());
  snprintf(windStr, sizeof(windStr), "%.0f mph", tempestWindMph);
  snprintf(gustStr, sizeof(gustStr), "%.0f mph", tempestGustMph);

  // Row 1: Wind + Gust
  M5.Display.setTextColor(COLOR_LABEL, COLOR_BG);
  M5.Display.setTextSize(1);
  M5.Display.drawString(windLabel, 16, 48);
  M5.Display.drawString("GUST", 170, 48);

  M5.Display.setTextColor(COLOR_TEXT_PRIMARY, COLOR_BG);
  M5.Display.setTextSize(3);
  M5.Display.drawString(windStr, 16, 64);
  M5.Display.drawString(gustStr, 170, 64);

  M5.Display.drawFastHLine(16, 100, 288, COLOR_SEPARATOR);

  // Row 2: rain RATE -- the Tempest word (None ... Extreme) plus the
  // number in inches per hour. No daily accumulation yet (a known,
  // documented gap), and none is implied here.
  const char *rainWord = rainLevelLabel(tempestRainRateLevel);
  char rateStr[16];
  if (tempestRainRateLevel == "very_light") {
    snprintf(rateStr, sizeof(rateStr), "<0.01 in/hr");   // rounds to 0.00 otherwise
  } else if (tempestRainRateLevel == "") {
    snprintf(rateStr, sizeof(rateStr), "--");
  } else {
    snprintf(rateStr, sizeof(rateStr), "%.2f in/hr", tempestRainRateInHr);
  }

  M5.Display.setTextColor(COLOR_LABEL, COLOR_BG);
  M5.Display.setTextSize(1);
  M5.Display.drawString("RAIN NOW", 16, 110);
  M5.Display.drawString("RATE", 170, 110);

  M5.Display.setTextColor(COLOR_TEXT_PRIMARY, COLOR_BG);
  M5.Display.setTextSize(fitTextSize(rainWord, 148));
  M5.Display.drawString(rainWord, 16, 126);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(COLOR_TEXT_SECONDARY, COLOR_BG);
  M5.Display.drawString(rateStr, 170, 130);

  M5.Display.drawFastHLine(16, 162, 288, COLOR_SEPARATOR);

  // Row 3: pressure. Station-only, not sea-level-adjusted (a known,
  // documented server-side gap) -- labeled honestly. Trend isn't
  // computed server-side yet, shown as "--" rather than fabricated.
  char pressureStr[16];
  snprintf(pressureStr, sizeof(pressureStr), "%.2f inHg", tempestPressureInHg);

  M5.Display.setTextColor(COLOR_LABEL, COLOR_BG);
  M5.Display.setTextSize(1);
  M5.Display.drawString("PRESSURE (STATION)", 16, 172);

  M5.Display.setTextColor(COLOR_TEXT_SECONDARY, COLOR_BG);
  M5.Display.setTextSize(2);
  M5.Display.drawString(pressureStr, 16, 188);
  M5.Display.setTextColor(COLOR_TEXT_DIM, COLOR_BG);
  M5.Display.drawString("--", 190, 188);
}

void drawLightningPage() {
  drawSecondaryHeader("Lightning", PAGE_LIGHTNING);

  // All decisions below use the freshness-aware view, never the raw
  // lightningActive global (see currentLightningView()).
  LightningView lv = currentLightningView();
  bool lActive = (lv == LIGHTNING_ACTIVE);

  // Badge: amber "active" treatment when lightning.active is true (same
  // fixed-height, two-line-capable box already confirmed working on
  // hardware); a new, NOT previously mocked/approved neutral "clear"
  // treatment otherwise, since the approved mockup only ever showed the
  // active state -- worth a look once this is on real hardware.
  if (lActive) {
    M5.Display.fillRoundRect(16, 48, 288, 50, 6, COLOR_LIGHTNING_BG);
    M5.Display.drawRoundRect(16, 48, 288, 50, 6, COLOR_LIGHTNING_BORDER);
    M5.Display.setTextDatum(top_left);
    M5.Display.setTextColor(COLOR_LIGHTNING_TEXT, COLOR_LIGHTNING_BG);
    M5.Display.setTextSize(2);
    if (lightningLevel == "frequent") {
      M5.Display.drawString("Frequent lightning", 26, 56);
      M5.Display.drawString("nearby", 26, 76);
    } else {
      M5.Display.drawString("Lightning nearby", 26, 66);
    }
  } else if (lv == LIGHTNING_UNKNOWN) {
    // Neutral gray -- data isn't current, so this must NOT read as an
    // all-clear.
    M5.Display.fillRoundRect(16, 48, 288, 50, 6, COLOR_BG);
    M5.Display.drawRoundRect(16, 48, 288, 50, 6, COLOR_SEPARATOR);
    M5.Display.setTextDatum(top_left);
    M5.Display.setTextColor(COLOR_TEXT_DIM, COLOR_BG);
    M5.Display.setTextSize(2);
    M5.Display.drawString("Lightning status", 26, 56);
    M5.Display.drawString("unknown", 26, 76);
  } else {
    M5.Display.fillRoundRect(16, 48, 288, 50, 6, COLOR_BG);
    M5.Display.drawRoundRect(16, 48, 288, 50, 6, COLOR_SEPARATOR);
    M5.Display.setTextDatum(top_left);
    M5.Display.setTextColor(COLOR_NWS_CLEAR_TEXT, COLOR_BG);
    M5.Display.setTextSize(2);
    M5.Display.drawString("No lightning nearby", 26, 66);
  }

  // Closest recent strike
  M5.Display.setTextColor(COLOR_LABEL, COLOR_BG);
  M5.Display.setTextSize(1);
  M5.Display.drawString("CLOSEST RECENT STRIKE", 16, 108);

  M5.Display.setTextColor(lActive ? COLOR_TEXT_PRIMARY : COLOR_TEXT_DIM, COLOR_BG);
  M5.Display.setTextSize(3);
  if (lActive) {
    char distStr[12];
    snprintf(distStr, sizeof(distStr), "%.1f mi", lightningClosestDistanceMi);
    M5.Display.drawString(distStr, 16, 124);
    M5.Display.setTextColor(COLOR_TEXT_SECONDARY, COLOR_BG);
    M5.Display.setTextSize(2);
    // Local time via the same NTP/TZ path as the Now-screen clock. (The
    // server's ISO string is UTC, which used to be shown as if local.)
    M5.Display.drawString(formatEpochLocal(lightningMostRecentEpoch), 150, 130);
  } else {
    M5.Display.drawString(lv == LIGHTNING_UNKNOWN ? "--" : "None", 16, 124);
  }

  M5.Display.drawFastHLine(16, 162, 288, COLOR_SEPARATOR);

  // Activity count in filtered window
  M5.Display.setTextColor(COLOR_LABEL, COLOR_BG);
  M5.Display.setTextSize(1);
  char activityLabel[48];
  snprintf(activityLabel, sizeof(activityLabel), "ACTIVITY (LAST %d MIN, WITHIN %g MI)",
           lightningWindowMinutes, lightningFilterRadiusMi);
  M5.Display.drawString(activityLabel, 16, 172);

  char countStr[16];
  snprintf(countStr, sizeof(countStr), "%d strikes", lightningStrikeCountRecent);
  M5.Display.setTextColor(lActive ? COLOR_TEXT_PRIMARY : COLOR_TEXT_DIM, COLOR_BG);
  M5.Display.setTextSize(2);
  M5.Display.drawString(lActive ? countStr
                                : (lv == LIGHTNING_UNKNOWN ? "--" : "0 strikes"),
                        16, 188);
}

// NWS instruction text: whitespace cleanup, word-wrap, two layouts.
//
// Measured on the real display: text size 2 is 12px per character, so the
// 288px instruction column holds 24 characters per line; size 1 holds 48.
// (The first version assumed 42 characters per line at size 2 -- 504px on
// a 320px screen -- and would have clipped real alerts.)
//
// Each alert picks its own layout: size 2 (large, 6 lines) if the whole
// text fits without truncation, otherwise size 1 (small, 10 lines) so a
// long official instruction is shown in full rather than cut off. Only
// text too long even for size 1 gets the " ..." truncation marker.
const int NWS_LINE_CHARS_LARGE = 24;
const int NWS_LINE_CHARS_SMALL = 48;
const int NWS_LINES_LARGE = 6;
const int NWS_LINES_SMALL = 10;

// NWS text arrives with embedded newlines and runs of spaces (it is
// hard-wrapped at ~69 columns upstream). Flatten to single spaces so our
// own wrapping decides the line breaks.
String collapseWhitespace(const String &in) {
  String out;
  bool lastWasSpace = true;   // also drops leading whitespace
  for (int i = 0; i < (int)in.length(); i++) {
    char c = in.charAt(i);
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (c == ' ') {
      if (lastWasSpace) continue;
      lastWasSpace = true;
    } else {
      lastWasSpace = false;
    }
    out += c;
  }
  out.trim();
  return out;
}

// Word-wraps `text` into `outLines` (capacity `maxLines`), breaking on
// word boundaries where possible (a single word longer than a line is
// hard-broken). Sets `truncated` and ends the last line with " ..." if
// the text didn't all fit. Returns the number of lines used.
int wrapInstructionText(const String &text, String outLines[], int maxLines,
                        int charsPerLine, bool &truncated) {
  int lineCount = 0;
  int pos = 0;
  int len = text.length();

  while (pos < len && lineCount < maxLines) {
    int remaining = len - pos;
    int take = min(remaining, charsPerLine);

    if (pos + take < len) {
      String chunk = text.substring(pos, pos + take);
      int lastSpace = chunk.lastIndexOf(' ');
      if (lastSpace > 0) {
        take = lastSpace;
      }
    }

    String line = text.substring(pos, pos + take);
    line.trim();
    outLines[lineCount] = line;
    lineCount++;
    pos += take;
    while (pos < len && text.charAt(pos) == ' ') pos++;
  }

  truncated = (pos < len);
  if (truncated && lineCount > 0) {
    String &lastLine = outLines[lineCount - 1];
    int maxLastLineLen = charsPerLine - 4;
    if ((int)lastLine.length() > maxLastLineLen) {
      lastLine = lastLine.substring(0, maxLastLineLen);
    }
    lastLine += " ...";
  }

  return lineCount;
}

// Picks the layout for one alert's instruction text and fills `lines`
// (capacity NWS_LINES_SMALL). Returns the line count (0 = no text).
int layoutInstruction(const String &raw, String lines[], int &textSize,
                      int &lineHeight, bool &truncated) {
  String text = collapseWhitespace(raw);
  textSize = 2;
  lineHeight = 16;
  truncated = false;
  if (text.length() == 0) return 0;

  int n = wrapInstructionText(text, lines, NWS_LINES_LARGE, NWS_LINE_CHARS_LARGE, truncated);
  if (truncated) {
    textSize = 1;
    lineHeight = 10;
    n = wrapInstructionText(text, lines, NWS_LINES_SMALL, NWS_LINE_CHARS_SMALL, truncated);
  }
  return n;
}

// ---- NWS page body: WHAT'S HAPPENING / HAZARD / INSTRUCTION ----
// (Approved layout "B".) Text is laid out by a pure function into a list
// of positioned lines, so the geometry can be tested for overflow and
// overlap without a display. Priority when space is tight: HAZARD is
// always kept, INSTRUCTION keeps at least 3 lines, and WHAT'S HAPPENING
// gets whatever remains (up to 3 lines) or is omitted.
const int NWS_BODY_TOP = 90;
const int NWS_BODY_BOTTOM = 212;        // the strip / acknowledge bar starts at y=214
const int NWS_SECTION_GAP = 4;
const int NWS_LABEL_H = 10;             // size-1 label line
const int NWS_INSTR_MIN_LINES = 3;
const int NWS_MAX_PAGE_LINES = 24;

enum LineRole { ROLE_LABEL = 0, ROLE_WHAT, ROLE_HAZARD, ROLE_INSTRUCTION };
struct PageLine {
  int y;
  int size;
  LineRole role;
  String text;
};

// Returns the number of lines placed in `out` (capacity NWS_MAX_PAGE_LINES),
// or 0 if the alert has neither WHAT nor HAZARD text -- the caller then
// uses the instruction-only layout, exactly as before.
int layoutAlertBody(const String &whatIn, const String &hazardIn, const String &instrIn,
                    PageLine out[]) {
  String what = collapseWhitespace(whatIn);
  String hazard = collapseWhitespace(hazardIn);
  String instr = collapseWhitespace(instrIn);
  if (what.length() == 0 && hazard.length() == 0) return 0;

  int n = 0;
  bool truncated = false;

  // HAZARD: large (2 lines) if it fits, else small (3 lines).
  String hazardLines[3];
  int hazardCount = 0, hazardSize = 2, hazardLineH = 16;
  if (hazard.length() > 0) {
    hazardCount = wrapInstructionText(hazard, hazardLines, 2, NWS_LINE_CHARS_LARGE, truncated);
    if (truncated) {
      hazardSize = 1;
      hazardLineH = 10;
      hazardCount = wrapInstructionText(hazard, hazardLines, 3, NWS_LINE_CHARS_SMALL, truncated);
    }
  }

  // Space left for WHAT after reserving HAZARD and a minimum INSTRUCTION.
  int total = NWS_BODY_BOTTOM - NWS_BODY_TOP;
  int hazardBlock = hazardCount ? NWS_LABEL_H + hazardCount * hazardLineH : 0;
  int instrMinBlock = instr.length() ? NWS_LABEL_H + NWS_INSTR_MIN_LINES * 10 : 0;
  int laterSections = (hazardCount ? 1 : 0) + (instr.length() ? 1 : 0);
  int reserved = hazardBlock + instrMinBlock + laterSections * NWS_SECTION_GAP;
  int whatLinesAllowed = (total - reserved - NWS_LABEL_H) / 10;
  if (whatLinesAllowed > 3) whatLinesAllowed = 3;

  String whatLines[3];
  int whatCount = 0;
  if (what.length() > 0 && whatLinesAllowed >= 1) {
    whatCount = wrapInstructionText(what, whatLines, whatLinesAllowed, NWS_LINE_CHARS_SMALL, truncated);
  }

  int y = NWS_BODY_TOP;
  if (whatCount > 0) {
    out[n].y = y; out[n].size = 1; out[n].role = ROLE_LABEL; out[n].text = "WHAT'S HAPPENING"; n++;
    y += NWS_LABEL_H;
    for (int i = 0; i < whatCount; i++) {
      out[n].y = y; out[n].size = 1; out[n].role = ROLE_WHAT; out[n].text = whatLines[i]; n++;
      y += 10;
    }
    y += NWS_SECTION_GAP;
  }
  if (hazardCount > 0) {
    out[n].y = y; out[n].size = 1; out[n].role = ROLE_LABEL; out[n].text = "HAZARD"; n++;
    y += NWS_LABEL_H;
    for (int i = 0; i < hazardCount; i++) {
      out[n].y = y; out[n].size = hazardSize; out[n].role = ROLE_HAZARD; out[n].text = hazardLines[i]; n++;
      y += hazardLineH;
    }
    y += NWS_SECTION_GAP;
  }
  if (instr.length() > 0) {
    int lines = (NWS_BODY_BOTTOM - (y + NWS_LABEL_H)) / 10;
    if (lines > NWS_LINES_SMALL) lines = NWS_LINES_SMALL;
    if (lines >= 1) {
      String instrLines[NWS_LINES_SMALL];
      int instrCount = wrapInstructionText(instr, instrLines, lines, NWS_LINE_CHARS_SMALL, truncated);
      out[n].y = y; out[n].size = 1; out[n].role = ROLE_LABEL; out[n].text = "INSTRUCTION"; n++;
      y += NWS_LABEL_H;
      for (int i = 0; i < instrCount; i++) {
        out[n].y = y; out[n].size = 1; out[n].role = ROLE_INSTRUCTION; out[n].text = instrLines[i]; n++;
        y += 10;
      }
    }
  }
  return n;
}

void drawNwsAlertsPage() {
  drawSecondaryHeader("NWS Alerts", PAGE_NWS_ALERTS);

  M5.Display.setTextDatum(top_left);

  NwsView nwsView = currentNwsView();

  if (nwsView == NWS_VIEW_UNKNOWN) {
    // Same "status unknown" language as the Now screen's footer -- never
    // let an unreachable NWS check look like a real, checked "clear".
    M5.Display.setTextColor(COLOR_TEXT_DIM, COLOR_BG);
    M5.Display.setTextSize(2);
    M5.Display.drawString("NWS status unknown", 16, 60);
    // One-line reason, so "the server is down" and "the server can't
    // reach NWS" aren't indistinguishable.
    M5.Display.setTextSize(1);
    M5.Display.drawString(backendDataFresh() ? "Server can't reach NWS"
                                             : "Server not responding",
                           16, 86);
    return;
  }

  if (nwsView == NWS_VIEW_CLEAR) {
    // No approved mockup for this state (the v2 layout only ever showed
    // an active alert) -- simple, calm treatment, consistent with the
    // Now screen's own "NO ACTIVE ALERTS" language and color.
    M5.Display.fillCircle(28, 66, 5, COLOR_NWS_CLEAR_DOT);
    M5.Display.setTextColor(COLOR_NWS_CLEAR_TEXT, COLOR_BG);
    M5.Display.setTextSize(2);
    M5.Display.drawString("No active alerts", 42, 58);
    return;
  }

  // Consolidated event + until block (v2 layout -- headline field
  // deliberately dropped in favor of more room for instruction text;
  // see project brief for the reasoning Dan approved). Now using the
  // real first active alert's event name and expiration.
  M5.Display.fillRect(16, 48, 288, 34, COLOR_WARNING_BG);
  M5.Display.setTextColor(COLOR_WARNING_TEXT_HEADLINE, COLOR_WARNING_BG);
  String eventName = shortenEventName(nwsFirstAlertEvent);
  int eventSize = eventTextSize(eventName, 272);   // x = 24 .. 296, inside the 16..304 box
  M5.Display.setTextSize(eventSize);
  M5.Display.drawString(eventName, 24, eventSize == 2 ? 54 : 58);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(COLOR_WARNING_TEXT_DETAIL, COLOR_WARNING_BG);
  // Crude time extraction, same placeholder as elsewhere -- real
  // relative/12-hour formatting needs NTP (a separate step).
  String untilText = "Until " + formatAlertTimeNow(nwsFirstAlertExpires);
  if (nwsView == NWS_VIEW_ALERT_STALE) untilText += "  -- status not current";
  M5.Display.drawString(untilText, 24, 72);

  // Instruction: full official text, word-wrapped and truncated with an
  // indicator if it runs longer than the available space. Now using the
  // real instruction text from the active alert.
  // Approved layout B: WHAT'S HAPPENING / HAZARD / INSTRUCTION, when the
  // server supplied a headline or hazard for this alert.
  PageLine pageLines[NWS_MAX_PAGE_LINES];
  int pageLineCount = layoutAlertBody(nwsFirstAlertWhat, nwsFirstAlertHazard,
                                      nwsFirstAlertInstruction, pageLines);
  if (pageLineCount > 0) {
    for (int i = 0; i < pageLineCount; i++) {
      uint16_t color = COLOR_TEXT_SECONDARY;
      if (pageLines[i].role == ROLE_LABEL) color = COLOR_TEXT_DIM;
      else if (pageLines[i].role == ROLE_HAZARD) color = COLOR_TEXT_PRIMARY;
      M5.Display.setTextColor(color, COLOR_BG);
      M5.Display.setTextSize(pageLines[i].size);
      M5.Display.drawString(pageLines[i].text, 16, pageLines[i].y);
    }
    return;
  }

  // Otherwise (no headline/hazard from the server): the instruction-only
  // layout, unchanged.
  M5.Display.setTextColor(COLOR_TEXT_DIM, COLOR_BG);
  M5.Display.setTextSize(1);
  M5.Display.drawString("INSTRUCTION", 16, 92);

  String wrappedLines[NWS_LINES_SMALL];
  int instrSize, instrLineH;
  bool instrTruncated;
  int lineCount = layoutInstruction(nwsFirstAlertInstruction, wrappedLines,
                                    instrSize, instrLineH, instrTruncated);

  int y = 106;
  if (lineCount == 0) {
    // Some NWS products carry no instruction text at all.
    M5.Display.setTextColor(COLOR_TEXT_DIM, COLOR_BG);
    M5.Display.setTextSize(1);
    M5.Display.drawString("(no instruction text provided)", 16, y);
    return;
  }

  M5.Display.setTextColor(COLOR_TEXT_SECONDARY, COLOR_BG);
  M5.Display.setTextSize(instrSize);
  for (int i = 0; i < lineCount; i++) {
    M5.Display.drawString(wrappedLines[i], 16, y);
    y += instrLineH;
  }
}

void drawStatusPage() {
  drawSecondaryHeader("Status", PAGE_STATUS);

  M5.Display.setTextDatum(top_left);

  // Wi-Fi row now reflects the real connection state from connectWiFi()
  // (Phase 2a) instead of a hardcoded dummy value -- the other three
  // rows are still dummy data, pending the HTTP fetch and NTP work.
  bool wifiConnected = (WiFi.status() == WL_CONNECTED);
  String wifiValue;
  uint16_t wifiDotColor;
  if (wifiConnected) {
    // Kept short: the longer "Connected (-70 dBm)" (228px) overwrote
    // the "Wi-Fi" label on the real display.
    wifiValue = "OK (" + String(WiFi.RSSI()) + " dBm)";
    wifiDotColor = COLOR_NWS_CLEAR_DOT;
  } else {
    wifiValue = "Disconnected";
    wifiDotColor = COLOR_STATUS_BAD;
  }

  // Backend row: reflects whether the MOST RECENT fetch attempt
  // succeeded, not just whether one ever has -- so this correctly flips
  // to "Unreachable" if the server goes down after working fine earlier,
  // rather than staying stuck on a stale "Reachable" forever.
  String backendValue = lastFetchSucceeded ? "Reachable" : "Unreachable";
  uint16_t backendDotColor = lastFetchSucceeded ? COLOR_NWS_CLEAR_DOT : COLOR_STATUS_BAD;

  String timeSyncValue = timeSynced ? "OK (NTP)" : "Not synced";
  uint16_t timeSyncDotColor = timeSynced ? COLOR_NWS_CLEAR_DOT : COLOR_STATUS_BAD;

  struct StatusRow { const char *label; String value; uint16_t dotColor; };
  StatusRow rows[4] = {
    {"Wi-Fi",      wifiValue,             wifiDotColor},
    {"Backend",    backendValue,          backendDotColor},
    {"Last sync",  formatAgeString(),     COLOR_NWS_CLEAR_DOT},
    {"Time sync",  timeSyncValue,         timeSyncDotColor},
  };

  int y = 52;
  for (int i = 0; i < 4; i++) {
    M5.Display.fillCircle(24, y + 6, 5, rows[i].dotColor);
    M5.Display.setTextColor(COLOR_TEXT_SECONDARY, COLOR_BG);
    M5.Display.setTextSize(2);
    M5.Display.drawString(rows[i].label, 40, y);
    M5.Display.setTextDatum(top_right);
    M5.Display.drawString(rows[i].value, 304, y);
    M5.Display.setTextDatum(top_left);
    y += 28;
  }

  M5.Display.drawFastHLine(16, y + 4, 288, COLOR_SEPARATOR);
  y += 16;

  M5.Display.setTextColor(COLOR_LABEL, COLOR_BG);
  M5.Display.setTextSize(1);
  M5.Display.drawString("UPTIME", 16, y);
  M5.Display.drawString("FIRMWARE", 170, y);

  M5.Display.setTextColor(COLOR_TEXT_SECONDARY, COLOR_BG);
  M5.Display.setTextSize(2);
  M5.Display.drawString(formatUptimeString(), 16, y + 16);
  M5.Display.drawString("v0.1.0-dev", 170, y + 16);
}

// ---- Settings: 4-page shell (approved mockup, Session 10) ----
//
// Nav model: a plain tap anywhere on a main page advances the cycle, but
// Settings pages are almost entirely covered by real controls, so that
// convention would collide with them constantly. Instead all Settings-
// level navigation lives in one place: the bottom bar, split three ways
// (PREV / DONE / NEXT). The header stays purely informational -- just
// the title and "n/4" -- matching how every other page's header already
// behaves, rather than inventing a new meaning for it.
//
// Colors reuse the existing palette exactly (approved mockup): amber
// (COLOR_LIGHTNING_TEXT) for interactive controls, green
// (COLOR_NWS_CLEAR_TEXT) for normal/off/exit, red (COLOR_STATUS_BAD) for
// mute-active. No new color constants needed.
//
// Geometry constants are shared between the drawing code and the touch
// handler below, so the region drawn and the region that reacts to a tap
// can never drift apart -- the same defensive pattern used for the Now
// footer and the acknowledge bar.
const int SETTINGS_NAV_Y = 210;
const int SETTINGS_NAV_H = 30;
const int SETTINGS_NAV_PREV_X0 = 0, SETTINGS_NAV_PREV_X1 = 64;
const int SETTINGS_NAV_DONE_X0 = 64, SETTINGS_NAV_DONE_X1 = 256;
const int SETTINGS_NAV_NEXT_X0 = 256, SETTINGS_NAV_NEXT_X1 = 320;

const int SETTINGS_ROW_STEPPER_H = 64;  // stepper row height + gap, matches the approved mockup
const int SETTINGS_ROW_BUTTON_H = 58;   // button row height + gap
const int SETTINGS_LABEL_ONLY_H = 18;   // a bare label line + gap

// Which of the 4 Settings sub-pages is currently showing (0-3). Reset to
// 0 every time Settings is entered via long-press, so it always starts
// on Volume + Test Tone rather than remembering where you left off.
int settingsSubPage = 0;
const int SETTINGS_NUM_SUBPAGES = 4;

void drawSettingsHeader(const char *title) {
  M5.Display.fillScreen(COLOR_BG);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(COLOR_TEXT_PRIMARY, COLOR_BG);
  M5.Display.setTextSize(3);
  M5.Display.drawString(title, 16, 10);
  char pageLabel[8];
  snprintf(pageLabel, sizeof(pageLabel), "%d/%d", settingsSubPage + 1, SETTINGS_NUM_SUBPAGES);
  M5.Display.setTextDatum(top_right);
  M5.Display.setTextColor(COLOR_TEXT_DIM, COLOR_BG);
  M5.Display.setTextSize(1);
  M5.Display.drawString(pageLabel, 304, 14);
  M5.Display.drawFastHLine(16, 38, 288, COLOR_SEPARATOR);
}

// A labelled -/+ stepper row. `value` is whatever the current control
// shows; this function doesn't know or care whether it's backed by a
// real, live setting yet.
void drawSettingsStepperRow(int y, const char *label, const String &value) {
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(COLOR_LABEL, COLOR_BG);
  M5.Display.setTextSize(1);
  M5.Display.drawString(label, 16, y);

  int boxY = y + 8;
  M5.Display.drawRoundRect(16, boxY, 36, 30, 4, COLOR_LIGHTNING_TEXT);
  M5.Display.drawRoundRect(268, boxY, 36, 30, 4, COLOR_LIGHTNING_TEXT);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(COLOR_LIGHTNING_TEXT, COLOR_BG);
  M5.Display.drawString("-", 34, boxY + 15);
  M5.Display.drawString("+", 286, boxY + 15);
  M5.Display.setTextColor(COLOR_TEXT_PRIMARY, COLOR_BG);
  M5.Display.drawString(value, 160, boxY + 15);
}

// A full-width rounded button, optionally filled (for an active/on
// state) with an optional one-line sub-caption below it (used for
// Mute's "Until 7:00 AM").
void drawSettingsButtonRow(int y, int h, const String &label, uint16_t color,
                           bool filled, const String &sub) {
  uint16_t fillColor = filled ? color : COLOR_BG;
  uint16_t textColor = filled ? COLOR_BG : color;
  if (filled) {
    M5.Display.fillRoundRect(16, y, 288, h, 6, fillColor);
  }
  M5.Display.drawRoundRect(16, y, 288, h, 6, color);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(textColor, fillColor);
  M5.Display.drawString(label, 160, y + h / 2);
  if (sub.length() > 0) {
    M5.Display.setTextDatum(top_left);
    M5.Display.setTextColor(COLOR_STATUS_BAD, COLOR_BG);
    M5.Display.setTextSize(1);
    M5.Display.drawString(sub, 16, y + h + 2);
  }
}

void drawSettingsNavBar() {
  M5.Display.fillRect(0, SETTINGS_NAV_Y, 320, SETTINGS_NAV_H, COLOR_BG);
  M5.Display.drawFastHLine(0, SETTINGS_NAV_Y, 320, COLOR_SEPARATOR);
  M5.Display.drawFastVLine(SETTINGS_NAV_PREV_X1, SETTINGS_NAV_Y, SETTINGS_NAV_H, COLOR_SEPARATOR);
  M5.Display.drawFastVLine(SETTINGS_NAV_DONE_X1, SETTINGS_NAV_Y, SETTINGS_NAV_H, COLOR_SEPARATOR);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(2);
  int midY = SETTINGS_NAV_Y + SETTINGS_NAV_H / 2;
  M5.Display.setTextColor(COLOR_LIGHTNING_TEXT, COLOR_BG);
  M5.Display.drawString("<", (SETTINGS_NAV_PREV_X0 + SETTINGS_NAV_PREV_X1) / 2, midY);
  M5.Display.drawString(">", (SETTINGS_NAV_NEXT_X0 + SETTINGS_NAV_NEXT_X1) / 2, midY);
  M5.Display.setTextColor(COLOR_NWS_CLEAR_TEXT, COLOR_BG);
  M5.Display.drawString("DONE", (SETTINGS_NAV_DONE_X0 + SETTINGS_NAV_DONE_X1) / 2, midY);
}

// 12-hour "H:MM AM/PM" formatting for the schedule page -- same
// convention as the clock and alert times elsewhere, but taking plain
// hour/minute integers rather than a timestamp, since a schedule time
// isn't tied to any particular day.
String formatHourMinute12(int hour, int minute) {
  int h12 = hour % 12;
  if (h12 == 0) h12 = 12;
  char buf[12];
  snprintf(buf, sizeof(buf), "%d:%02d %s", h12, minute, hour >= 12 ? "PM" : "AM");
  return String(buf);
}

// -- Page 1: Volume + Test Tone --
// Audio doesn't exist yet (next item after Settings), so this page is
// visually complete per the approved mockup but genuinely inert: no
// real volume variable exists to show or change yet. Tapping its
// controls is logged, not acted on -- an honest placeholder, not a
// control that silently does nothing without saying so.
void drawSettingsVolumePage() {
  drawSettingsHeader("Settings");
  drawSettingsStepperRow(46, "VOLUME", "--");
  drawSettingsButtonRow(46 + SETTINGS_ROW_STEPPER_H, 34, "TEST TONE", COLOR_LIGHTNING_TEXT, false, "");
  drawSettingsNavBar();
}

// -- Page 2: Mute + Run Test Alert --
// Same as above: Mute has nothing real to control until audio exists.
// Run Test Alert is left inert for this same pass -- it's a natural
// next step (it could call this device's own /api/simulation/start),
// but that's a separate piece of wiring not yet built here.
void drawSettingsMutePage() {
  drawSettingsHeader("Settings");
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(COLOR_LABEL, COLOR_BG);
  M5.Display.setTextSize(1);
  M5.Display.drawString("MUTE", 16, 46);
  drawSettingsButtonRow(46 + SETTINGS_LABEL_ONLY_H, 34, "OFF  (tap to mute)", COLOR_NWS_CLEAR_TEXT, false, "");
  drawSettingsButtonRow(46 + SETTINGS_LABEL_ONLY_H + SETTINGS_ROW_BUTTON_H, 34,
                        "RUN TEST ALERT", COLOR_LIGHTNING_TEXT, false, "");
  drawSettingsNavBar();
}

// -- Page 3: Brightness (day/night) --
// Shows the REAL current values from the night-dimming feature (Session
// 10) -- read-only for now. Values are the raw 0-255 setBrightness()
// inputs, not a percentage: the hardware only has 9 real distinguishable
// steps (confirmed from M5GFX source), so a smooth percentage would
// imply precision that doesn't physically exist. Adjusting these is the
// next piece of work, not this one.
void drawSettingsBrightnessPage() {
  drawSettingsHeader("Settings");
  drawSettingsStepperRow(46, "DAY BRIGHTNESS", String(DAY_BRIGHTNESS));
  drawSettingsStepperRow(46 + SETTINGS_ROW_STEPPER_H, "NIGHT BRIGHTNESS", String(NIGHT_BRIGHTNESS));
  drawSettingsNavBar();
}

// -- Page 4: Night Schedule (start/end) --
// Same story: shows the real current schedule, read-only for now.
void drawSettingsSchedulePage() {
  drawSettingsHeader("Settings");
  drawSettingsStepperRow(46, "NIGHT START",
                         formatHourMinute12(NIGHT_START_HOUR, NIGHT_START_MINUTE));
  drawSettingsStepperRow(46 + SETTINGS_ROW_STEPPER_H, "NIGHT END",
                         formatHourMinute12(NIGHT_END_HOUR, NIGHT_END_MINUTE));
  drawSettingsNavBar();
}

void drawSettingsPage() {
  switch (settingsSubPage) {
    case 0: drawSettingsVolumePage(); break;
    case 1: drawSettingsMutePage(); break;
    case 2: drawSettingsBrightnessPage(); break;
    default: drawSettingsSchedulePage(); break;
  }
}

// Settings-specific touch handling. Called instead of the generic
// tap-to-advance logic whenever currentPage == PAGE_SETTINGS -- nearly
// the whole screen is real controls here, so "tap anywhere advances"
// would constantly collide with them. Nav-bar taps always redraw
// (something visibly changed); content-area taps outside the nav bar
// are matched against each page's known hotspots and logged, since
// nothing on those pages is wired to a real value yet.
void handleSettingsTouch(int x, int y) {
  if (y >= SETTINGS_NAV_Y) {
    if (x < SETTINGS_NAV_PREV_X1) {
      settingsSubPage = (settingsSubPage + SETTINGS_NUM_SUBPAGES - 1) % SETTINGS_NUM_SUBPAGES;
      Serial.printf("[settings] prev -> subpage %d\n", settingsSubPage);
    } else if (x < SETTINGS_NAV_DONE_X1) {
      Serial.println("[settings] DONE -- returning to Now");
      currentPage = PAGE_NOW;
      renderPage(currentPage);
      return;
    } else {
      settingsSubPage = (settingsSubPage + 1) % SETTINGS_NUM_SUBPAGES;
      Serial.printf("[settings] next -> subpage %d\n", settingsSubPage);
    }
    renderPage(currentPage);
    return;
  }

  // Content-area tap. Brightness (subpage 2) and Night Schedule
  // (subpage 3) are wired to the real values from the night-dimming
  // feature; Volume and Mute (subpages 0-1) have nothing real to
  // control until audio exists, so taps there just log.
  //
  // Stepper box geometry matches drawSettingsStepperRow() exactly
  // (minus box x=16..52, plus box x=268..304, box height 30 starting
  // 8px below the row's y) -- shared numbers, not independently
  // guessed, so drawing and hit-testing can't drift apart.
  const int ROW0_Y = 46, ROW1_Y = 46 + SETTINGS_ROW_STEPPER_H;
  bool inMinusX = (x >= 16 && x < 52);
  bool inPlusX = (x >= 268 && x < 304);
  bool inRow0Y = (y >= ROW0_Y + 8 && y < ROW0_Y + 38);
  bool inRow1Y = (y >= ROW1_Y + 8 && y < ROW1_Y + 38);
  int direction = inMinusX ? -1 : (inPlusX ? 1 : 0);

  if (direction != 0 && settingsSubPage == 2) {
    if (inRow0Y) {
      DAY_BRIGHTNESS = stepBrightness(DAY_BRIGHTNESS, direction);
      Serial.printf("[settings] DAY_BRIGHTNESS -> %d\n", DAY_BRIGHTNESS);
    } else if (inRow1Y) {
      NIGHT_BRIGHTNESS = stepBrightness(NIGHT_BRIGHTNESS, direction);
      Serial.printf("[settings] NIGHT_BRIGHTNESS -> %d\n", NIGHT_BRIGHTNESS);
    } else {
      return;  // tap was in the stepper's x-range but not its y-range
    }
    applyCurrentBrightnessImmediately();
    renderPage(currentPage);
    return;
  }

  if (direction != 0 && settingsSubPage == 3) {
    if (inRow0Y) {
      stepScheduleTime(NIGHT_START_HOUR, NIGHT_START_MINUTE, direction);
      Serial.printf("[settings] NIGHT_START -> %02d:%02d\n", NIGHT_START_HOUR, NIGHT_START_MINUTE);
    } else if (inRow1Y) {
      stepScheduleTime(NIGHT_END_HOUR, NIGHT_END_MINUTE, direction);
      Serial.printf("[settings] NIGHT_END -> %02d:%02d\n", NIGHT_END_HOUR, NIGHT_END_MINUTE);
    } else {
      return;
    }
    renderPage(currentPage);
    return;
  }

  // Volume (0) and Mute (1): nothing real to wire yet. Logged so
  // testing can still confirm hit-testing works.
  Serial.printf("[settings] content tap at (%d,%d) on subpage %d -- not yet wired\n",
                x, y, settingsSubPage);
}

void renderPageInner(Page p) {
  switch (p) {
    case PAGE_NOW:
      drawNowPage();
      return;  // Now keeps its own full footer; no persistent strip added
    case PAGE_WIND_RAIN:
      drawWindRainPage();
      break;
    case PAGE_LIGHTNING:
      drawLightningPage();
      break;
    case PAGE_NWS_ALERTS:
      drawNwsAlertsPage();
      if (nwsFirstAlertNeedsAlert) {
        drawAckPrompt();
        return;  // replaces the normal strip only while there's
                  // something on this page to acknowledge
      }
      break;
    case PAGE_STATUS:
      drawStatusPage();
      break;
    case PAGE_SETTINGS:
      drawSettingsPage();
      return;  // Settings doesn't carry the alert strip either
  }
  drawPersistentStrip();
}

// Timing wrapper: logs any page draw slower than DIAG_SLOW_RENDER_MS, so
// a slow redraw can be told apart from a blocked network call when
// reading the serial log.
const unsigned long DIAG_SLOW_RENDER_MS = 100;
const unsigned long DIAG_LOOP_GAP_MS = 250;

// Drawn AFTER a page's own content, on every page including Settings,
// regardless of which screen is showing -- a pure overlay that never
// reads or depends on that page's own layout, so it carries zero risk
// of colliding with any page's carefully-tuned coordinates. A thin
// border frames the whole screen (visible continuously, not just a
// glance-and-miss detail) plus a small corner tag naming it explicitly.
void drawSimulationOverlay() {
  const int BORDER_W = 4;
  for (int i = 0; i < BORDER_W; i++) {
    M5.Display.drawRect(i, i, 320 - 2 * i, 240 - 2 * i, COLOR_SIMULATION);
  }
  // Corner tag geometry was checked against every page's real header, not
  // guessed: every secondary page's title starts at y=10 (drawSecondaryHeader,
  // size 3), so an 8px-tall tag (matching Font0's exact 6x8 size, confirmed
  // from M5GFX source) at y=0..7 leaves a real 2px gap before it -- not a
  // 1px razor's edge. Width 68px comfortably covers "SIMULATION" (10 chars
  // x 6px = 60px) with a few px of padding; the Now screen's own top-right
  // content (age text, Wi-Fi glyph) lives at x>=266, far clear of this
  // corner. Checked against an exact-pixel render of the worst case
  // (longest title, "Wind & Rain") before trusting this, not just by eye.
  M5.Display.fillRect(0, 0, 68, 8, COLOR_SIMULATION);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(COLOR_BG, COLOR_SIMULATION);
  M5.Display.setTextSize(1);
  M5.Display.drawString("SIMULATION", 4, 0);
}

void renderPage(Page p) {
  unsigned long t0 = millis();
  renderPageInner(p);
  if (simulationActive) {
    drawSimulationOverlay();
  }
  unsigned long dt = millis() - t0;
  if (dt > DIAG_SLOW_RENDER_MS) {
    Serial.printf("[diag] slow render: page %d took %lu ms\n", (int)p, dt);
  }
}

// ---- Night dimming (v1.0 requirement) ----
//
// Real hardware quirk confirmed from M5GFX's own source (Session 10),
// not assumed: setBrightness() takes 0-255, but the CoreS3's brightness-
// to-DLDO1-voltage formula ((b+641)>>5) collapses that whole range into
// only 9 real distinguishable physical steps -- inputs 1-29 all produce
// the SAME dimmest nonzero level, 223-255 all produce the SAME brightest.
// setBrightness(0) is NOT "very dim" -- it disables the backlight rail
// entirely (screen goes fully black), which is not what night mode wants
// (dim but still glanceable in a dark room). The constants below are
// real raw 0-255 inputs, not a pretend smooth percentage, because that
// precision doesn't physically exist on this hardware.
//
// Defaults, confirmed reasonable on real hardware for NIGHT_BRIGHTNESS
// (Session 10); the schedule matches the original planning brief's
// illustrative 22:00-07:00 example. All four are now adjustable from
// the Settings page (Session 10) -- in memory only for now, no flash
// persistence yet, so they reset to these defaults on every reboot.
uint8_t DAY_BRIGHTNESS = 255;
uint8_t NIGHT_BRIGHTNESS = 20;
int NIGHT_START_HOUR = 22;
int NIGHT_START_MINUTE = 0;
int NIGHT_END_HOUR = 7;
int NIGHT_END_MINUTE = 0;

// The 9 real distinguishable brightness levels on this hardware
// (Session 10 finding, derived from M5GFX's own (b+641)>>5 formula for
// the CoreS3's brightness-to-DLDO1-voltage mapping). The Settings
// brightness stepper moves between these 9 levels one at a time, not by
// an arbitrary raw-unit increment that might land inside the same
// physical step and visibly do nothing.
const uint8_t BRIGHTNESS_LEVELS[9] = {1, 31, 63, 95, 127, 159, 191, 223, 255};
const int BRIGHTNESS_LEVEL_COUNT = 9;

// Finds the closest entry in BRIGHTNESS_LEVELS to an arbitrary raw value
// (so a value that doesn't exactly match a level, e.g. an old default,
// still has a sensible "current" index to step from).
int nearestBrightnessLevelIndex(uint8_t raw) {
  int best = 0;
  int bestDiff = 256;
  for (int i = 0; i < BRIGHTNESS_LEVEL_COUNT; i++) {
    int diff = (int)raw - (int)BRIGHTNESS_LEVELS[i];
    if (diff < 0) diff = -diff;  // inlined, not abs() -- Arduino's abs() is a
                                 // macro with known double-evaluation pitfalls
    if (diff < bestDiff) {
      bestDiff = diff;
      best = i;
    }
  }
  return best;
}

// Steps a brightness value by one real level in the given direction
// (+1 or -1), clamped at the ends (does not wrap -- reaching max
// brightness and tapping + again should just stay at max, unlike the
// Settings page navigation, which wraps deliberately for a different
// reason).
uint8_t stepBrightness(uint8_t current, int direction) {
  int idx = nearestBrightnessLevelIndex(current) + direction;
  if (idx < 0) idx = 0;
  if (idx >= BRIGHTNESS_LEVEL_COUNT) idx = BRIGHTNESS_LEVEL_COUNT - 1;
  return BRIGHTNESS_LEVELS[idx];
}

// Steps a schedule time by 15 minutes (placeholder increment -- not yet
// decided with Dan) in the given direction, wrapping across midnight
// (23:50 + 15 min -> 00:05), since a time-of-day naturally wraps within
// a day, unlike brightness.
void stepScheduleTime(int &hour, int &minute, int direction) {
  int total = hour * 60 + minute;
  total += direction * 15;
  total = ((total % 1440) + 1440) % 1440;  // wraparound-safe for either direction
  hour = total / 60;
  minute = total % 60;
}

// If the brightness value for the CURRENTLY ACTIVE mode (day or night)
// was just changed in Settings, apply it to the hardware immediately --
// otherwise the change wouldn't be visible until the next day/night
// transition, which could be hours away. Adjusting the INACTIVE mode's
// value (e.g. changing night brightness while it's currently day) is a
// silent no-op here by design: it takes effect next time that mode
// starts, which handleNightDimming() already does on its own.
void applyCurrentBrightnessImmediately() {
  if (lastAppliedBrightnessMode == 1) {
    M5.Display.setBrightness(NIGHT_BRIGHTNESS);
  } else if (lastAppliedBrightnessMode == 0) {
    M5.Display.setBrightness(DAY_BRIGHTNESS);
  }
  // mode == -1 (not yet established) is left alone; handleNightDimming()
  // will set a real value on its very next pass regardless.
}

// -1 = not yet applied (forces the very first loop() pass to set a real
// brightness rather than silently trusting whatever M5.begin()'s own
// default happened to be); 0 = day applied; 1 = night applied.
int lastAppliedBrightnessMode = -1;

// If the clock isn't synced, we genuinely don't know whether it's night
// -- default to DAY (bright) in that case. Failing toward more visible
// is the safer direction for a device whose whole job is to be seen;
// failing toward dim is not.
bool isNightTimeNow() {
  if (!timeSynced) return false;
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  int nowMinutes = t.tm_hour * 60 + t.tm_min;
  int startMinutes = NIGHT_START_HOUR * 60 + NIGHT_START_MINUTE;
  int endMinutes = NIGHT_END_HOUR * 60 + NIGHT_END_MINUTE;
  if (startMinutes == endMinutes) return false;  // degenerate schedule: treat as always-day
  if (startMinutes < endMinutes) {
    return nowMinutes >= startMinutes && nowMinutes < endMinutes;
  }
  // Schedule wraps past midnight (e.g. 22:00 -> 07:00).
  return nowMinutes >= startMinutes || nowMinutes < endMinutes;
}

// Called every loop() pass but only writes to the display hardware when
// the day/night mode actually changes -- an I2C write on every ~10ms
// loop iteration would be pointless and wasteful when nothing changed.
void handleNightDimming() {
  bool night = isNightTimeNow();
  int mode = night ? 1 : 0;
  if (mode == lastAppliedBrightnessMode) return;
  uint8_t level = night ? NIGHT_BRIGHTNESS : DAY_BRIGHTNESS;
  M5.Display.setBrightness(level);
  Serial.printf("[brightness] switched to %s (raw %d)\n", night ? "NIGHT" : "DAY", level);
  lastAppliedBrightnessMode = mode;
}

// Phase 2a: Wi-Fi connectivity only -- no HTTP fetch, no screen changes
// yet. Uses the wifi_credentials.h stopgap (real captive portal comes
// later, per project instructions). Success/failure reported to serial
// only for this step; the display keeps showing its existing
// static/dummy content exactly as before.
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;

void syncTimeNTP();  // defined below; called from the reconnect logic ahead of its own definition

void connectWiFi() {
  Serial.println();
  Serial.printf("Connecting to Wi-Fi: %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startAttempt < WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    Serial.printf("Signal strength (RSSI): %d dBm\n", WiFi.RSSI());
  } else {
    Serial.println("Wi-Fi connection FAILED (timed out after 20s).");
    Serial.println("Check wifi_credentials.h has the correct SSID/password,");
    Serial.println("and that the network is 2.4GHz -- the CoreS3 SE's");
    Serial.println("Wi-Fi radio does not support 5GHz networks at all.");
  }
}

// ---- Reconnect logic (v1.0 requirement, §24) ----
//
// setup() calls connectWiFi()/syncTimeNTP() once, blocking, as before --
// this section covers everything AFTER that: what happens when Wi-Fi
// drops mid-operation, which a power outage makes likely (the router
// often comes back slower than this device does).
//
// Cadence, confirmed with Dan: retry at 10s, 30s, 60s after the drop is
// first noticed, then hold at 60s -- Wi-Fi outages are binary (the
// router is up or it isn't), so backing off further than 60s buys
// nothing. After WIFI_REBOOT_AFTER_MS (10 minutes) of continuous
// failure, reboot as a last resort -- a stuck radio/driver state is the
// scenario this guards against, not a slow router.
//
// NTP rides along with Wi-Fi: a resync is attempted right after any
// reconnect that follows a real disconnect, plus a periodic safety
// resync on its own timer regardless (continued background resync
// beyond the boot-time sync was only ever assumed, never confirmed --
// §17).
const unsigned long WIFI_RETRY_SCHEDULE_MS[] = {10000, 30000, 60000};
const int WIFI_RETRY_SCHEDULE_LEN = 3;
const unsigned long WIFI_RETRY_HOLD_MS = 60000;   // cadence once past the schedule
const unsigned long WIFI_REBOOT_AFTER_MS = 600000;  // 10 minutes, confirmed with Dan
const unsigned long NTP_PERIODIC_RESYNC_MS = 4UL * 60 * 60 * 1000;  // 4 hours, placeholder

bool wifiWasConnected = true;        // setup()'s blocking connect already ran
unsigned long wifiDownSinceMillis = 0;       // 0 = currently connected
unsigned long wifiLastRetryMillis = 0;
int wifiRetryIndex = 0;
unsigned long lastNtpAttemptMillis = 0;

// Non-blocking: kicks off one connection attempt (WiFi.begin) and returns
// immediately. Unlike connectWiFi(), this never delays the loop -- the
// actual connection happens in the background; wifiWasConnected below is
// what notices success on a later loop() pass.
void beginWifiReconnectAttempt() {
  Serial.println("[wifi] Reconnect attempt starting...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

// Called every loop(). Detects disconnects, retries on the schedule
// above without blocking, resyncs NTP after a real reconnect, does the
// periodic safety NTP resync, and reboots after prolonged failure.
void handleWifiAndNtpReconnect() {
  bool connectedNow = (WiFi.status() == WL_CONNECTED);
  unsigned long now = millis();

  if (connectedNow) {
    if (!wifiWasConnected) {
      // Just came back from a real outage.
      unsigned long downFor = now - wifiDownSinceMillis;
      Serial.printf("[wifi] Reconnected after %lu ms. RSSI %d dBm.\n",
                    downFor, WiFi.RSSI());
      wifiWasConnected = true;
      wifiDownSinceMillis = 0;
      wifiRetryIndex = 0;
      syncTimeNTP();  // resync -- an outage this long may have drifted the clock
      lastNtpAttemptMillis = now;
      // Force the next periodic conditions-fetch to happen on the very
      // next loop() pass, rather than waiting up to FETCH_INTERVAL_MS
      // (60s) for its own independent timer. Confirmed needed on real
      // hardware (Session 9): without this, Wi-Fi could report back
      // while Backend still showed "Unreachable" for up to another
      // minute, simply waiting its turn on an unrelated clock. Uses
      // subtraction (not a bare 0) so it stays correct even very soon
      // after boot, matching the wraparound-safe pattern used elsewhere.
      lastFetchAttemptMillis = now - FETCH_INTERVAL_MS - 1;
    } else if (now - lastNtpAttemptMillis >= NTP_PERIODIC_RESYNC_MS) {
      // Periodic safety resync even without a disconnect.
      Serial.println("[ntp] Periodic resync (no disconnect occurred).");
      syncTimeNTP();
      lastNtpAttemptMillis = now;
    }
    return;
  }

  // Not connected.
  if (wifiWasConnected) {
    // Just noticed the drop.
    Serial.println("[wifi] Connection LOST. Will retry at 10s, 30s, 60s, then every 60s.");
    wifiWasConnected = false;
    wifiDownSinceMillis = now;
    wifiRetryIndex = 0;
    wifiLastRetryMillis = now;
    return;  // first retry happens on a later pass, per the schedule below
  }

  unsigned long downFor = now - wifiDownSinceMillis;
  if (downFor >= WIFI_REBOOT_AFTER_MS) {
    // Last resort: 10 minutes of continuous failure. A stuck Wi-Fi
    // driver/radio state is what this guards against -- a genuine
    // router/NAS outage will still be down after the reboot too, and
    // the device will just keep retrying from a clean state instead of
    // silently sitting there.
    Serial.println("[wifi] Down for 10+ minutes. Rebooting as a last resort.");
    delay(100);  // let the serial line flush
    ESP.restart();
  }

  unsigned long dueAt;
  if (wifiRetryIndex < WIFI_RETRY_SCHEDULE_LEN) {
    dueAt = wifiDownSinceMillis + WIFI_RETRY_SCHEDULE_MS[wifiRetryIndex];
  } else {
    dueAt = wifiLastRetryMillis + WIFI_RETRY_HOLD_MS;
  }

  if (now >= dueAt) {
    Serial.printf("[wifi] Retry #%d (down %lu ms)\n", wifiRetryIndex + 1, downFor);
    beginWifiReconnectAttempt();
    wifiLastRetryMillis = now;
    if (wifiRetryIndex < WIFI_RETRY_SCHEDULE_LEN) wifiRetryIndex++;
  }
}

// Phase 2f: NTP time sync. Uses the POSIX TZ-string form (configTzTime)
// rather than the simpler fixed-UTC-offset form, specifically so US
// Eastern daylight saving transitions (2nd Sunday in March, 1st Sunday
// in November, per current US rule) are handled automatically by the C
// library instead of needing a manual seasonal flip twice a year.
// Not compiled/tested by me (same caveat as always for firmware) --
// configTzTime is a real, documented ESP32 Arduino-core function, but
// that's a statement about how standard the API is, not proof this
// exact call succeeds on real hardware.
const char *TZ_STRING = "EST5EDT,M3.2.0,M11.1.0";
const char *NTP_SERVER = "pool.ntp.org";
const unsigned long NTP_SYNC_TIMEOUT_MS = 15000;
const time_t NTP_SANITY_THRESHOLD = 1577836800;  // 2020-01-01, roughly --
                                                   // anything before this
                                                   // means sync hasn't
                                                   // happened yet (ESP32's
                                                   // clock defaults to
                                                   // epoch ~1970 at boot)

void syncTimeNTP() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ntp] Skipped -- Wi-Fi not connected");
    return;
  }

  Serial.println("[ntp] Requesting time sync...");
  configTzTime(TZ_STRING, NTP_SERVER);

  unsigned long startAttempt = millis();
  time_t now = time(nullptr);
  while (now < NTP_SANITY_THRESHOLD && millis() - startAttempt < NTP_SYNC_TIMEOUT_MS) {
    delay(300);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println();

  if (now >= NTP_SANITY_THRESHOLD) {
    timeSynced = true;
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &timeinfo);
    Serial.printf("[ntp] Synced! Current local time: %s\n", buf);
  } else {
    timeSynced = false;
    Serial.println("[ntp] Sync FAILED (timed out after 15s). Clock will show --:--.");
  }
}

// NOTE: this is a one-time sync at boot, not a repeating one. The
// underlying ESP-IDF SNTP client is assumed to continue periodic
// background resync on its own once started, matching how most SNTP
// implementations behave by default -- but this is an ASSUMPTION, not
// something confirmed via extended runtime testing (would need days of
// uptime to check whether the clock stays accurate, or drifts).

String formatEpochLocal(long epoch) {
  if (!timeSynced || epoch <= 0) {
    return "--:--";
  }
  time_t t = (time_t)epoch;
  struct tm timeinfo;
  localtime_r(&t, &timeinfo);
  char buf[16];
  strftime(buf, sizeof(buf), "%I:%M %p", &timeinfo);
  String result(buf);
  if (result.charAt(0) == '0') {
    result = result.substring(1);   // "09:15 PM" -> "9:15 PM", same as the clock
  }
  return result;
}

String formatCurrentTime() {
  if (!timeSynced) {
    return "--:--";
  }
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char buf[16];
  strftime(buf, sizeof(buf), "%I:%M %p", &timeinfo);
  String result(buf);
  // Strip a leading zero from the hour (e.g. "09:15 PM" -> "9:15 PM"),
  // matching the style already used in the approved mockups.
  if (result.charAt(0) == '0') {
    result = result.substring(1);
  }
  return result;
}
// drawNowPage() actually renders, called on a repeating 60s timer from
// loop() rather than once at boot.
//
// Uses ArduinoJson v7 (pinned in platformio.ini). Not compiled/tested by
// me (same sandbox limitation as always for firmware) -- treat this the
// same as everything else that needs a real build to confirm.
//
// KNOWN LIMITATION, partly mitigated: http.GET() still blocks the main
// loop for its duration. On a healthy local-LAN request that is well
// under a second, but an unreachable server used to freeze touch for up
// to ~10s per attempt (Arduino-ESP32's defaults are 5s connect + 5s
// read -- confirmed against the 2.0.17 and 3.1.0 HTTPClient source).
// The explicit timeouts below cap that. Proper non-blocking HTTP (a
// background fetch task) is NOT built yet -- decide after reading the
// [fetch] / [diag] serial timing this version logs.
//
// The server address comes from wifi_credentials.h (gitignored), not
// from committed source -- see wifi_credentials.h.example.
#ifndef SERVER_BASE_URL
#error "SERVER_BASE_URL is not defined -- add it to wifi_credentials.h (see wifi_credentials.h.example)"
#endif
const char *SERVER_URL = SERVER_BASE_URL "/api/conditions";
const char *ACK_URL = SERVER_BASE_URL "/api/alerts/ack";

// PLACEHOLDERS -- tune after reading real timing from the serial log.
const int32_t HTTP_CONNECT_TIMEOUT_MS = 2000;
const uint16_t HTTP_READ_TIMEOUT_MS = 3000;

void fetchConditions() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[fetch] Skipped -- Wi-Fi not connected");
    lastFetchSucceeded = false;
    return;
  }

  Serial.printf("[fetch] Requesting /api/conditions... (t=%lu ms)\n", millis());
  unsigned long fetchStart = millis();
  HTTPClient http;
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_READ_TIMEOUT_MS);
  http.begin(SERVER_URL);
  int httpCode = http.GET();
  unsigned long getMs = millis() - fetchStart;

  if (httpCode != 200) {
    Serial.printf("[fetch] HTTP request FAILED, code: %d (%s), blocked %lu ms\n",
                  httpCode, HTTPClient::errorToString(httpCode).c_str(), getMs);
    http.end();
    lastFetchSucceeded = false;
    return;
  }

  String payload = http.getString();
  http.end();
  Serial.printf("[fetch] Got response, %d bytes (GET+read %lu ms)\n",
                payload.length(), millis() - fetchStart);

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    Serial.printf("[fetch] JSON parse FAILED: %s\n", error.c_str());
    lastFetchSucceeded = false;
    return;
  }

  tempestAvailable = doc["tempest"]["available"];
  if (tempestAvailable) {
    tempestTempF = doc["tempest"]["temperature_f"];
    tempestHumidityPct = doc["tempest"]["humidity_percent"];
    tempestWindMph = doc["tempest"]["wind_mph"];
    tempestGustMph = doc["tempest"]["gust_mph"];
    tempestRainRateInHr = doc["tempest"]["rain_rate_in_hr"] | 0.0f;
    tempestRainRateLevel = doc["tempest"]["rain_rate_level"] | "";
    JsonVariant feelsLike = doc["tempest"]["feels_like_f"];
    tempestFeelsLikeValid = !feelsLike.isNull();
    if (tempestFeelsLikeValid) {
      tempestFeelsLikeF = feelsLike.as<float>();
    }
    tempestPressureInHg = doc["tempest"]["pressure_inhg_station"];
    tempestWindDir = doc["tempest"]["wind_direction"].as<String>();
  }

  // Top-level flag, present on every real response too (as false) --
  // simulation is decided entirely server-side; the device only ever
  // reflects it.
  simulationActive = doc["simulation"] | false;
  if (simulationActive) {
    const char *scenario = doc["simulation_scenario"] | "?";
    int step = doc["simulation_step"] | -1;
    const char *desc = doc["simulation_step_description"] | "";
    Serial.printf("[fetch] SIMULATION active: scenario=%s step=%d (%s)\n", scenario, step, desc);
  }

  lightningActive = doc["lightning"]["active"];
  lightningLevel = doc["lightning"]["level"].as<String>();
  lightningFilterRadiusMi = doc["lightning"]["filter_radius_mi"] | 10.0f;
  lightningWindowMinutes = doc["lightning"]["window_minutes"] | 10;
  if (lightningActive) {
    lightningClosestDistanceMi = doc["lightning"]["closest_distance_mi"];
    lightningMostRecentEpoch = doc["lightning"]["most_recent_strike_epoch"] | 0L;
    lightningStrikeCountRecent = doc["lightning"]["strike_count_recent"];
  }

  nwsAvailable = doc["nws"]["available"];
  JsonArray alerts = doc["nws"]["alerts"];
  nwsAlertCount = alerts.size();
  if (nwsAlertCount > 0) {
    // "| """ (not .as<String>()) so a JSON null -- NWS often sends
    // "instruction": null -- becomes an empty string instead of risking
    // the literal text "null" appearing on screen.
    nwsFirstAlertEvent = alerts[0]["event"] | "";
    nwsFirstAlertExpires = alerts[0]["expires"] | "";
    nwsFirstAlertInstruction = alerts[0]["instruction"] | "";
    nwsFirstAlertId = alerts[0]["id"] | "";
    nwsFirstAlertWhat = alerts[0]["what"] | "";
    nwsFirstAlertHazard = alerts[0]["hazard"] | "";
    nwsFirstAlertAcknowledged = alerts[0]["acknowledged"];
    nwsFirstAlertNeedsAlert = alerts[0]["needs_alert"];
  } else {
    nwsFirstAlertEvent = "";
    nwsFirstAlertExpires = "";
    nwsFirstAlertInstruction = "";
    nwsFirstAlertId = "";
    nwsFirstAlertWhat = "";
    nwsFirstAlertHazard = "";
    nwsFirstAlertAcknowledged = false;
    nwsFirstAlertNeedsAlert = false;
  }

  hasEverFetchedSuccessfully = true;
  lastFetchSucceeded = true;
  lastSuccessfulFetchMillis = millis();

  Serial.printf("[fetch] State updated -- tempest.available=%s temp=%.1f wind=%.1f dir=%s\n",
                tempestAvailable ? "true" : "false", tempestTempF, tempestWindMph,
                tempestWindDir.c_str());
  Serial.printf("[fetch]   lightning.level=%s  nws.available=%s  alerts=%d\n",
                lightningLevel.c_str(), nwsAvailable ? "true" : "false", nwsAlertCount);
  Serial.printf("[fetch] OK -- total blocked %lu ms\n", millis() - fetchStart);
}

// Phase 2g: alert acknowledgement. Same blocking-HTTP characteristic as
// fetchConditions() above, but user-triggered rather than on a 60s
// timer, so the impact is a single brief pause on tap rather than a
// recurring background one.
void ackAlert(const String &alertId) {
  if (WiFi.status() != WL_CONNECTED || alertId.length() == 0) {
    Serial.println("[ack] Skipped -- no Wi-Fi or no alert id");
    return;
  }

  Serial.printf("[ack] Acknowledging alert %s... (t=%lu ms)\n", alertId.c_str(), millis());
  unsigned long ackStart = millis();
  HTTPClient http;
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_READ_TIMEOUT_MS);
  http.begin(ACK_URL);
  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  doc["id"] = alertId;
  String body;
  serializeJson(doc, body);

  int httpCode = http.POST(body);
  Serial.printf("[ack] POST returned %d after %lu ms\n", httpCode, millis() - ackStart);
  if (httpCode == 200) {
    Serial.println("[ack] Success.");
    // Optimistic local update -- the next periodic fetch (up to 60s
    // away) will reconcile with the server's authoritative state, but
    // there's no reason to make the person wait that long to see the
    // prompt disappear after they just tapped it.
    nwsFirstAlertAcknowledged = true;
    nwsFirstAlertNeedsAlert = false;
  } else {
    Serial.printf("[ack] FAILED, code: %d (%s) -- prompt stays up, can retry\n",
                  httpCode, HTTPClient::errorToString(httpCode).c_str());
  }
  http.end();
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("=== Weather Sentinel -- Navigation demo ===");
  Serial.println("Tap to advance page. Long-press (~1s) for Settings.");

  connectWiFi();
  syncTimeNTP();
  fetchConditions();
  lastFetchAttemptMillis = millis();

  initColors();
  lastActivityTime = millis();
  renderPage(currentPage);
}

void loop() {
  // Diagnostic: any loop iteration that starts much later than the last
  // one means something blocked (fetch, ack, a slow draw, serial).
  static unsigned long lastLoopMillis = 0;
  unsigned long loopNow = millis();
  if (lastLoopMillis != 0 && loopNow - lastLoopMillis > DIAG_LOOP_GAP_MS) {
    Serial.printf("[diag] loop gap %lu ms (t=%lu) -- UI was blocked\n",
                  loopNow - lastLoopMillis, loopNow);
  }
  lastLoopMillis = loopNow;

  M5.update();
  auto touch = M5.Touch.getDetail();

  if (touch.wasPressed()) {
    touchStartTime = millis();
    longPressHandled = false;
    Serial.printf("[touch] press x=%d y=%d t=%lu page=%d\n",
                  (int)touch.x, (int)touch.y, millis(), (int)currentPage);
  }

  if (touch.isPressed() && !longPressHandled &&
      (millis() - touchStartTime >= LONG_PRESS_MS)) {
    longPressHandled = true;
    lastActivityTime = millis();
    if (currentPage != PAGE_SETTINGS) {
      currentPage = PAGE_SETTINGS;
      settingsSubPage = 0;  // always start on Volume + Test Tone
      Serial.println("Long-press detected -- opening Settings");
      renderPage(currentPage);
    }
  }

  // Note: wasClicked() is this library's name for the release-edge event
  // (state == touch_end) -- there is no wasReleased(). Confirmed against
  // M5Unified's own touch_detail_t source before using it here.
  if (touch.wasClicked() && !longPressHandled) {
    lastActivityTime = millis();

    // Diagnostic: time since the previous click. Several page advances
    // from one physical tap would show up here as clicks only a few ms
    // apart.
    static unsigned long lastClickMillis = 0;
    Serial.printf("[touch] click x=%d y=%d t=%lu (+%lu ms since previous click)\n",
                  (int)touch.x, (int)touch.y, millis(),
                  lastClickMillis == 0 ? 0UL : millis() - lastClickMillis);
    lastClickMillis = millis();

    bool tappedAckPrompt = (currentPage == PAGE_NWS_ALERTS &&
                             nwsFirstAlertNeedsAlert &&
                             touch.y >= BOTTOM_BAR_Y &&
                             touch.y < BOTTOM_BAR_Y + BOTTOM_BAR_H);

    // The Now footer says "tap for details" -- so a tap on the footer band
    // must actually go to the NWS Alerts page (a plain tap elsewhere still
    // advances one page, as always).
    NwsView tapNwsView = currentNwsView();
    bool tappedNowAlertFooter = (currentPage == PAGE_NOW &&
                                 (tapNwsView == NWS_VIEW_ALERT || tapNwsView == NWS_VIEW_ALERT_STALE) &&
                                 touch.y >= NOW_FOOTER_Y);

    if (tappedAckPrompt) {
      ackAlert(nwsFirstAlertId);
      renderPage(currentPage);  // redraw -- prompt disappears if the ack succeeded
    } else if (tappedNowAlertFooter) {
      currentPage = PAGE_NWS_ALERTS;
      Serial.println("Tap on Now alert footer -- jumping to NWS Alerts");
      renderPage(currentPage);
    } else if (currentPage == PAGE_SETTINGS) {
      handleSettingsTouch((int)touch.x, (int)touch.y);
    } else {
      currentPage = (Page)((currentPage + 1) % NUM_CYCLE_PAGES);
      Serial.printf("Tap -- now on page %d\n", (int)currentPage);
      renderPage(currentPage);
    }
  }

  // Wi-Fi/NTP reconnect -- non-blocking, runs every pass regardless of
  // which page is showing (v1.0 requirement, §24).
  handleWifiAndNtpReconnect();
  handleNightDimming();

  // Idle auto-return to Now
  if (currentPage != PAGE_NOW &&
      (millis() - lastActivityTime >= IDLE_RETURN_MS)) {
    Serial.println("Idle timeout -- returning to Now");
    currentPage = PAGE_NOW;
    lastActivityTime = millis();
    renderPage(currentPage);
  }

  // Periodic re-fetch of real conditions. Only redraws if Now is
  // currently being shown -- doesn't interrupt whatever detail page
  // someone might be reading, even though the underlying data still
  // updates regardless, ready for whenever they cycle back to Now.
  if (millis() - lastFetchAttemptMillis >= FETCH_INTERVAL_MS) {
    lastFetchAttemptMillis = millis();
    fetchConditions();
    if (currentPage == PAGE_NOW) {
      renderPage(PAGE_NOW);
    }
  }

  delay(10);
}
