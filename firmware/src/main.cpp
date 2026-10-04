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
#include <utility/led/LED_Strip_Class.hpp>  // M5GO Bottom3 LEDs (Session 15)
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>   // settings saved across restarts (v1.2)
#include <math.h>
#include <time.h>
#include "wifi_credentials.h"

// Shown on the Status page. Bump this with every flashed release, and tag
// the matching commit in GitHub with the same name (v1.0 = the build that
// passed the Session 13-14 acceptance tests, 2026-09-30).
const char *FIRMWARE_VERSION = "v1.2";  // Settings menu, saved settings, LED levels, Test Alerts page

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

// Audio (Session 11): same forward-declaration need, same reason.
extern uint8_t alarmVolume;
extern bool audioMuted;
extern unsigned long muteUntilMillis;
void applyAlarmVolumeImmediately();
void toggleMute();
void playTestTone();
String formatMuteUntil();

// Power loss and LEDs (Session 15): defined near the end of the file.
extern bool onBatteryPower;
bool powerScreenShouldShow();
void drawPowerLostPage();

// Alert History (Session 12): the fetch lives with the other HTTP code.
void fetchAlertHistory();
bool isNightTimeNow();
extern const int ALARM_VOLUME_STEP;

// ---- LED levels set from Settings (v1.2) ----
// Two user levels drive every LED state (approved in v1.2):
//   night: every state at ledNightLevel, except an unacknowledged
//          Critical at 4x (16 -> 64 by default, as in v1.1);
//   day:   ledDayLevel for flashes, lightning and on-battery, half of it
//          for steady glows and "unknown", and full 255 for unacknowledged
//          Warning/Critical regardless.
// Steps chosen with Dan; defaults are the v1.1 levels he approved.
const uint8_t LED_NIGHT_STEPS[] = {8, 12, 16, 24, 32};
const uint8_t LED_DAY_STEPS[] = {32, 48, 64, 96, 128};
const int LED_STEP_COUNT = 5;
uint8_t ledNightLevel = 16;
uint8_t ledDayLevel = 64;
// While adjusting, the LEDs show amber at the chosen level for a moment.
unsigned long ledPreviewUntilMillis = 0;
uint8_t ledPreviewLevel = 0;
const unsigned long LED_PREVIEW_MS = 3000;

uint8_t stepLedLevel(uint8_t current, const uint8_t *steps, int direction) {
  int idx = 0;
  for (int i = 0; i < LED_STEP_COUNT; i++) if (steps[i] <= current) idx = i;
  idx += direction;
  if (idx < 0) idx = 0;
  if (idx >= LED_STEP_COUNT) idx = LED_STEP_COUNT - 1;
  return steps[idx];
}
bool isLedStep(uint8_t v, const uint8_t *steps) {
  for (int i = 0; i < LED_STEP_COUNT; i++) if (steps[i] == v) return true;
  return false;
}

// Saved settings (v1.2): defined near setup(); declared here because
// the Settings pages call them.
void saveSettingsIfChanged();
bool postSimulation(const char *path, const char *scenario);
void fetchConditions();
void fetchSimulationStatus();

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
// Close lightning (v1.1.1): the server's "close" block. Close = a strike
// under closeRadius miles; the server holds it active for 30 minutes after
// the last close strike (NWS: wait 30 minutes after the last thunder), so
// it can outlast the 10-minute strike window above. Missing from an older
// server = inactive.
bool lightningCloseActive = false;
float lightningCloseRadiusMi = 10;
int lightningCloseHoldMinutes = 30;
float lightningCloseLastDistanceMi = 0;
long lightningCloseLastEpoch = 0;
int lightningCloseMinutesSince = 0;

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
// "critical"/"warning"/"watch"/"advisory"/"informational" -- the server
// has classified and sent this since Session 5; firmware only starts
// reading it now, for the alarm engine (Session 11).
String nwsFirstAlertLevel = "";

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
  bool any = lightningActive || lightningCloseActive;   // the close hold counts too
  if (current) return any ? LIGHTNING_ACTIVE : LIGHTNING_CLEAR;
  if (hasEverFetchedSuccessfully && any && dataAgeMs() < LIGHTNING_HOLD_MS) {
    return LIGHTNING_ACTIVE;
  }
  return LIGHTNING_UNKNOWN;
}

// Lightning by distance (v1.1.1). All three go through the freshness-aware
// view above, so stale data never shows as close (or as clear).
// Close strikes in the current 10-minute window:
bool lightningCloseInWindow() {
  return currentLightningView() == LIGHTNING_ACTIVE && lightningActive &&
         lightningClosestDistanceMi < lightningCloseRadiusMi;
}
// Close now: in the window, or within the 30-minute hold after it.
bool lightningCloseNow() {
  return currentLightningView() == LIGHTNING_ACTIVE &&
         (lightningCloseActive || lightningCloseInWindow());
}
// The hold only: no close strike in the window, but one within 30 minutes.
bool lightningCloseHoldOnly() {
  return lightningCloseNow() && !lightningCloseInWindow();
}
// Whole miles for the banner and strip; never "0 mi".
int lightningMiles(float mi) {
  int m = (int)lroundf(mi);
  return m < 1 ? 1 : m;
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
    // Amber tier for lower-level NWS alerts (Watch/Advisory/Statement),
    // approved by Dan in Session 12 -- red is reserved for Warning and
    // Critical. A SOLID, flat bar, so it stays distinct in shape from
    // lightning's outlined, rounded amber box even when both are shown.
    COLOR_ALERT_AMBER_BG, COLOR_ALERT_AMBER_HEADLINE, COLOR_ALERT_AMBER_DETAIL,
    // "ACKNOWLEDGED" confirmation (Session 13, approved mockup).
    COLOR_ACK_OK_BG, COLOR_ACK_OK_TEXT,
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
  COLOR_ALERT_AMBER_BG        = M5.Display.color565(0x4a, 0x33, 0x00);
  COLOR_ALERT_AMBER_HEADLINE  = M5.Display.color565(0xff, 0xd6, 0x80);
  COLOR_ALERT_AMBER_DETAIL    = M5.Display.color565(0xd9, 0xb0, 0x60);
  COLOR_ACK_OK_BG             = M5.Display.color565(0x10, 0x3a, 0x10);
  COLOR_ACK_OK_TEXT           = M5.Display.color565(0x8a, 0xd0, 0x8a);
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
  // Mute must be visible, not just set (v1.0 requirement; approved
  // mockup, Session 13): a plain word rather than an icon, so no one has
  // to guess. Amber, like other "attention, not alarm" states. Ends at
  // x=272, clear of the Wi-Fi glyph (x>=280) and of the longest clock
  // text ("12:59 PM" ends at x=112).
  if (audioMuted) {
    M5.Display.setTextDatum(top_right);
    M5.Display.setTextColor(COLOR_LIGHTNING_TEXT, COLOR_BG);
    M5.Display.setTextSize(2);
    M5.Display.drawString("MUTED", 272, 20);
    M5.Display.setTextDatum(top_left);
  }
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

// ---- Alert priority and severity treatment (v1.0 item 4, Session 12) ----
// The server sorts nws.alerts so alerts[0] is the one to show, sound and
// acknowledge (a sounding alert that still needs acknowledgement first,
// then highest level). Firmware adds only colour-by-level and a
// "+N more" count; it never re-sorts.

const int GLYPH_W_SIZE1 = 6;

struct AlertColors {
  uint16_t bg, headline, detail;
};

// Red for Warning and Critical; amber for Watch, Advisory and
// Informational (Special Weather Statements). A blank or unrecognised
// level falls back to RED -- if the level is ever missing, fail toward
// the more conspicuous treatment, never toward a calmer one.
AlertColors alertColorsForLevel(const String &level) {
  if (level == "watch" || level == "advisory" || level == "informational") {
    return {COLOR_ALERT_AMBER_BG, COLOR_ALERT_AMBER_HEADLINE, COLOR_ALERT_AMBER_DETAIL};
  }
  return {COLOR_WARNING_BG, COLOR_WARNING_TEXT_HEADLINE, COLOR_WARNING_TEXT_DETAIL};
}

AlertColors currentAlertColors() {
  return alertColorsForLevel(nwsFirstAlertLevel);
}

// How many active alerts are NOT the one on screen.
int nwsMoreCount() {
  return nwsAlertCount > 1 ? nwsAlertCount - 1 : 0;
}

String nwsMoreText() {
  return "+" + String(nwsMoreCount()) + " more";
}

// Cuts text to maxChars, ending in ".." if anything was removed.
String fitChars(const String &text, int maxChars) {
  if ((int)text.length() <= maxChars) return text;
  if (maxChars <= 2) return text.substring(0, maxChars);
  return text.substring(0, maxChars - 2) + "..";
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
  AlertColors c = currentAlertColors();
  M5.Display.fillRect(0, NOW_FOOTER_Y, 320, NOW_FOOTER_H, c.bg);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(c.headline, c.bg);
  String eventName = shortenEventName(eventText);
  int eventSize = eventTextSize(eventName, 300);   // x = 16 .. 316
  M5.Display.setTextSize(eventSize);
  M5.Display.drawString(eventName, 16, eventSize == 2 ? 202 : 206);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(c.detail, c.bg);
  M5.Display.drawString(untilText, 16, 224);
  if (nwsMoreCount() > 0) {
    // Right-aligned on the "Until" line. The longest until text
    // ("Until Wed 12:30 PM -- status not current", 40 chars) ends at
    // x=256, clear of a one-digit "+N more" (262..304).
    M5.Display.setTextColor(c.headline, c.bg);
    M5.Display.setTextDatum(top_right);
    M5.Display.drawString(nwsMoreText(), 304, 224);
    M5.Display.setTextDatum(top_left);
  }
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
    // overflowed this banner. Close lightning (v1.1.1, approved mockup)
    // shows its distance instead; at most 23 characters fit.
    char bannerText[32];
    if (lightningCloseInWindow()) {
      snprintf(bannerText, sizeof(bannerText), "%s %d mi",
               lightningLevel == "frequent" ? "Frequent lightning" : "Lightning",
               lightningMiles(lightningClosestDistanceMi));
    } else if (lightningCloseHoldOnly()) {
      snprintf(bannerText, sizeof(bannerText), "Lightning %d mi, %dm ago",
               lightningMiles(lightningCloseLastDistanceMi), lightningCloseMinutesSince);
    } else {
      snprintf(bannerText, sizeof(bannerText), "%s",
               lightningLevel == "frequent" ? "Frequent lightning" : "Lightning nearby");
    }
    drawLightningBanner(String(bannerText));
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
  bool showMore = false;
  if (nwsView == NWS_VIEW_ALERT || nwsView == NWS_VIEW_ALERT_STALE) {
    AlertColors c = currentAlertColors();
    bg = c.bg;
    textColor = c.headline;
    msg = nwsFirstAlertEvent;
    if (nwsView == NWS_VIEW_ALERT_STALE) msg += " (status not current)";
    if (nwsMoreCount() > 0) {
      // Leave room for a right-aligned "+N more" ending at x=308: the
      // message may use x=12..254, i.e. 40 characters at size 1. Shorten
      // in order of least information lost.
      showMore = true;
      const int maxChars = (254 - 12) / GLYPH_W_SIZE1;
      if ((int)msg.length() > maxChars) {
        // Trim the event NAME, never the staleness marker -- "not
        // current" is the part that must always survive.
        String suffix = (nwsView == NWS_VIEW_ALERT_STALE) ? " (not current)" : "";
        msg = fitChars(shortenEventName(nwsFirstAlertEvent),
                       maxChars - (int)suffix.length()) + suffix;
      }
    }
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
    char lt[56];
    if (lightningCloseInWindow()) {
      if (lightningLevel == "frequent") {
        snprintf(lt, sizeof(lt), "Frequent lightning, %d mi away",
                 lightningMiles(lightningClosestDistanceMi));
      } else {
        snprintf(lt, sizeof(lt), "Lightning %d mi away", lightningMiles(lightningClosestDistanceMi));
      }
    } else if (lightningCloseHoldOnly()) {
      snprintf(lt, sizeof(lt), "Close lightning %d mi, %d min ago -- stay in",
               lightningMiles(lightningCloseLastDistanceMi), lightningCloseMinutesSince);
    } else {
      snprintf(lt, sizeof(lt), "%s",
               lightningLevel == "frequent" ? "Frequent lightning nearby" : "Lightning nearby");
    }
    msg = lt;
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
  if (showMore) {
    M5.Display.setTextDatum(middle_right);
    M5.Display.drawString(nwsMoreText(), 308, BOTTOM_BAR_Y + BOTTOM_BAR_H / 2);
    M5.Display.setTextDatum(middle_left);
  }
}

void drawAckPrompt() {
  // Replaces the persistent strip's usual space on the NWS Alerts page
  // specifically while there's an active alert needing acknowledgement
  // -- the ack action is more useful here than the strip's normal
  // (redundant, since the alert's own detail is already on screen)
  // summary text. Same region as the strip so the touch handler in
  // loop() only needs one consistent rectangle to check against.
  // Same colour tier as the alert it acknowledges.
  AlertColors c = currentAlertColors();
  M5.Display.fillRect(0, BOTTOM_BAR_Y, 320, BOTTOM_BAR_H, c.bg);
  M5.Display.drawFastHLine(0, BOTTOM_BAR_Y, 320, COLOR_SEPARATOR);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(c.headline, c.bg);
  M5.Display.setTextSize(2);
  M5.Display.drawString("TAP TO ACKNOWLEDGE", 160, BOTTOM_BAR_Y + BOTTOM_BAR_H / 2);
}

// Brief feedback after a tap on "TAP TO ACKNOWLEDGE" (Session 13,
// approved mockup), drawn in the same bar: green "ACKNOWLEDGED" for 2 s
// when the server confirmed it, or "NOT SENT - TAP AGAIN" for 3 s (in the
// alert's own colour tier) when it didn't, after which the prompt returns.
// Timed by millis() and cleared from loop(), never by delay().
enum AckFeedback { ACK_FEEDBACK_NONE, ACK_FEEDBACK_OK, ACK_FEEDBACK_FAILED };
AckFeedback ackFeedback = ACK_FEEDBACK_NONE;
unsigned long ackFeedbackUntilMillis = 0;
const unsigned long ACK_FEEDBACK_OK_MS = 2000;
const unsigned long ACK_FEEDBACK_FAILED_MS = 3000;

void showAckFeedback(bool ok) {
  ackFeedback = ok ? ACK_FEEDBACK_OK : ACK_FEEDBACK_FAILED;
  ackFeedbackUntilMillis = millis() + (ok ? ACK_FEEDBACK_OK_MS : ACK_FEEDBACK_FAILED_MS);
}

void drawAckFeedback() {
  uint16_t bg, fg;
  const char *msg;
  if (ackFeedback == ACK_FEEDBACK_OK) {
    bg = COLOR_ACK_OK_BG;
    fg = COLOR_ACK_OK_TEXT;
    msg = "ACKNOWLEDGED";
  } else {
    AlertColors c = currentAlertColors();
    bg = c.bg;
    fg = c.headline;
    msg = "NOT SENT - TAP AGAIN";   // 20 chars = 240 px at size 2
  }
  M5.Display.fillRect(0, BOTTOM_BAR_Y, 320, BOTTOM_BAR_H, bg);
  M5.Display.drawFastHLine(0, BOTTOM_BAR_Y, 320, COLOR_SEPARATOR);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(fg, bg);
  M5.Display.setTextSize(2);
  M5.Display.drawString(msg, 160, BOTTOM_BAR_Y + BOTTOM_BAR_H / 2);
  M5.Display.setTextDatum(top_left);
}

// Called every loop() pass: when the feedback's time is up, clear it and
// redraw the NWS page so the strip (or the prompt again) comes back.
void handleAckFeedbackExpiry() {
  if (ackFeedback == ACK_FEEDBACK_NONE) return;
  if ((long)(millis() - ackFeedbackUntilMillis) < 0) return;
  ackFeedback = ACK_FEEDBACK_NONE;
  if (currentPage == PAGE_NWS_ALERTS) renderPage(currentPage);
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
    if (lightningCloseInWindow()) {
      char line2[24];
      snprintf(line2, sizeof(line2), "within %g miles", lightningCloseRadiusMi);
      M5.Display.drawString("Close lightning", 26, 56);
      M5.Display.drawString(line2, 26, 76);
    } else if (lightningCloseHoldOnly()) {
      char line2[24];
      snprintf(line2, sizeof(line2), "%d min ago, wait %d",
               lightningCloseMinutesSince, lightningCloseHoldMinutes);
      M5.Display.drawString("Close lightning", 26, 56);
      M5.Display.drawString(line2, 26, 76);
    } else if (lightningLevel == "frequent") {
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
  // During the close hold the distance shown is the last CLOSE strike
  // (which may be older than the 10-minute window), labelled as such.
  bool showHold = lightningCloseHoldOnly();
  M5.Display.drawString(showHold ? "LAST CLOSE STRIKE" : "CLOSEST RECENT STRIKE", 16, 108);

  M5.Display.setTextColor(lActive ? COLOR_TEXT_PRIMARY : COLOR_TEXT_DIM, COLOR_BG);
  M5.Display.setTextSize(3);
  if (lActive) {
    char distStr[12];
    snprintf(distStr, sizeof(distStr), "%.1f mi",
             showHold ? lightningCloseLastDistanceMi : lightningClosestDistanceMi);
    M5.Display.drawString(distStr, 16, 124);
    M5.Display.setTextColor(COLOR_TEXT_SECONDARY, COLOR_BG);
    M5.Display.setTextSize(2);
    // Local time via the same NTP/TZ path as the Now-screen clock. (The
    // server's ISO string is UTC, which used to be shown as if local.)
    M5.Display.drawString(formatEpochLocal(showHold ? lightningCloseLastEpoch
                                                    : lightningMostRecentEpoch), 150, 130);
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
  snprintf(countStr, sizeof(countStr), "%d strike%s", lightningStrikeCountRecent,
           lightningStrikeCountRecent == 1 ? "" : "s");
  M5.Display.setTextColor(lActive && lightningStrikeCountRecent > 0 ? COLOR_TEXT_PRIMARY
                                                                   : COLOR_TEXT_DIM, COLOR_BG);
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
      // Cut at the last space that fits, not mid-word ("...move to t ..."
      // was seen in Session 13 renders). A single very long word falls
      // back to a hard cut.
      String cut = lastLine.substring(0, maxLastLineLen);
      int lastSpace = cut.lastIndexOf(' ');
      if (lastSpace > 0) cut = cut.substring(0, lastSpace);
      lastLine = cut;
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

// The NWS Alerts page's coloured event/until block. Shared by the drawing
// code and the touch handler: while "+N more >" is shown, a tap anywhere
// in this block opens Alert History (Session 12).
const int NWS_HEADER_X = 16, NWS_HEADER_Y = 48, NWS_HEADER_W = 288, NWS_HEADER_H = 34;

// The body below an alert's coloured header block (y >= 90): layout B
// (WHAT'S HAPPENING / HAZARD / INSTRUCTION) when the server supplied a
// headline or hazard, else the instruction-only layout. Shared by the
// NWS Alerts page and the Alert History detail view (Session 12) so the
// two can never lay the same alert out differently.
void drawAlertBody(const String &what, const String &hazard, const String &instruction) {
  M5.Display.setTextDatum(top_left);
  // Instruction: full official text, word-wrapped and truncated with an
  // indicator if it runs longer than the available space.
  // Approved layout B: WHAT'S HAPPENING / HAZARD / INSTRUCTION, when the
  // server supplied a headline or hazard for this alert.
  PageLine pageLines[NWS_MAX_PAGE_LINES];
  int pageLineCount = layoutAlertBody(what, hazard,
                                      instruction, pageLines);
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
  int lineCount = layoutInstruction(instruction, wrappedLines,
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
  AlertColors c = currentAlertColors();
  M5.Display.fillRect(NWS_HEADER_X, NWS_HEADER_Y, NWS_HEADER_W, NWS_HEADER_H, c.bg);
  M5.Display.setTextColor(c.headline, c.bg);
  String eventName = shortenEventName(nwsFirstAlertEvent);
  int eventSize = eventTextSize(eventName, 272);   // x = 24 .. 296, inside the 16..304 box
  M5.Display.setTextSize(eventSize);
  M5.Display.drawString(eventName, 24, eventSize == 2 ? 54 : 58);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(c.detail, c.bg);
  bool showMore = nwsMoreCount() > 0;
  String untilText = "Until " + formatAlertTimeNow(nwsFirstAlertExpires);
  if (nwsView == NWS_VIEW_ALERT_STALE) {
    // The long form runs to x=270 and would collide with "+N more"
    // (right-aligned at 296), so use the short form when both show.
    untilText += showMore ? "  -- not current" : "  -- status not current";
  }
  M5.Display.drawString(untilText, 24, 72);
  if (showMore) {
    M5.Display.setTextColor(c.headline, c.bg);
    M5.Display.setTextDatum(top_right);
    // " >" marks it as tappable: the whole coloured block opens Alert
    // History (Session 12). 54 px wide, so it still clears the short
    // stale form of the until text (ends x=228).
    M5.Display.drawString(nwsMoreText() + " >", 296, 72);
    M5.Display.setTextDatum(top_left);
  }

  drawAlertBody(nwsFirstAlertWhat, nwsFirstAlertHazard, nwsFirstAlertInstruction);
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
  M5.Display.drawString(FIRMWARE_VERSION, 170, y + 16);
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
// Two-way bar (v1.2): left half BACK (or MUTE on the menu), right half DONE.
const int SETTINGS_NAV_HALF_X = 160;
// Three-way bar, kept only for the Alert History detail view (< BACK >).
const int SETTINGS_NAV_PREV_X0 = 0, SETTINGS_NAV_PREV_X1 = 64;
const int SETTINGS_NAV_DONE_X0 = 64, SETTINGS_NAV_DONE_X1 = 256;
const int SETTINGS_NAV_NEXT_X0 = 256, SETTINGS_NAV_NEXT_X1 = 320;

const int SETTINGS_ROW_STEPPER_H = 64;  // stepper row height + gap, matches the approved mockup
// Real bug found on hardware (Session 10) and fixed here (Session 11):
// the label and its stepper box had zero gap between them (box started
// at y+8, right where the size-1 label's own text ends), reading as
// visually crowded. Pulled into one named constant, shared between the
// drawing code and the touch hit-test below, so they can't drift apart
// the way two independently-hardcoded "8"s could have.
const int SETTINGS_STEPPER_BOX_Y_OFFSET = 12;
const int SETTINGS_ROW_BUTTON_H = 58;   // button row height + gap
const int SETTINGS_LABEL_ONLY_H = 18;   // a bare label line + gap

// v1.2: a Settings MENU (a grid of six tiles, approved mockup) replaces
// the old chain of < > pages. A long-press always opens the menu; each
// tile opens its page, and every page has the same BACK / DONE bar.
enum SettingsView {
  SV_MENU = 0, SV_SOUND, SV_SCREEN, SV_LEDS, SV_SCHEDULE, SV_HISTORY, SV_TEST
};
int settingsSubPage = SV_MENU;
const int SETTINGS_PAGE_HISTORY = SV_HISTORY;   // name kept for "+N more"

// Menu tile grid: 2 columns x 3 rows, shared by drawing and hit-testing.
const int MENU_TILE_X[2] = {16, 164};
const int MENU_TILE_Y[3] = {46, 100, 154};
const int MENU_TILE_W = 140, MENU_TILE_H = 48;

void drawSettingsHeader(const char *title) {
  M5.Display.fillScreen(COLOR_BG);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(COLOR_TEXT_PRIMARY, COLOR_BG);
  M5.Display.setTextSize(3);
  M5.Display.drawString(title, 16, 10);
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

  int boxY = y + SETTINGS_STEPPER_BOX_Y_OFFSET;
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

void drawSettingsNavBar(const char *centreLabel = "DONE") {
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
  M5.Display.drawString(centreLabel, (SETTINGS_NAV_DONE_X0 + SETTINGS_NAV_DONE_X1) / 2, midY);
}

// v1.2 two-way bar. leftFill != COLOR_BG gives a solid left half (the
// menu's red MUTED state).
void drawSettingsTwoWayBar(const String &leftLabel, uint16_t leftColor, uint16_t leftFill) {
  M5.Display.fillRect(0, SETTINGS_NAV_Y, 320, SETTINGS_NAV_H, COLOR_BG);
  if (leftFill != COLOR_BG) {
    M5.Display.fillRect(0, SETTINGS_NAV_Y + 1, SETTINGS_NAV_HALF_X, SETTINGS_NAV_H - 1, leftFill);
  }
  M5.Display.drawFastHLine(0, SETTINGS_NAV_Y, 320, COLOR_SEPARATOR);
  M5.Display.drawFastVLine(SETTINGS_NAV_HALF_X, SETTINGS_NAV_Y, SETTINGS_NAV_H, COLOR_SEPARATOR);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(2);
  int midY = SETTINGS_NAV_Y + SETTINGS_NAV_H / 2;
  M5.Display.setTextColor(leftColor, leftFill);
  M5.Display.drawString(leftLabel, SETTINGS_NAV_HALF_X / 2, midY);
  M5.Display.setTextColor(COLOR_NWS_CLEAR_TEXT, COLOR_BG);
  M5.Display.drawString("DONE", (SETTINGS_NAV_HALF_X + 320) / 2, midY);
}

void drawSettingsBackBar() {
  drawSettingsTwoWayBar("< BACK", COLOR_LIGHTNING_TEXT, COLOR_BG);
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

// ---- v1.2 Settings pages (approved mockup) ----

// "MUTED 8:42" for the menu bar: the end time without AM/PM so it fits
// the half-width bar; just "MUTED" if the clock isn't synced.
String muteBarLabel() {
  if (!timeSynced) return "MUTED";
  long remainingSec = (long)(muteUntilMillis - millis()) / 1000;
  if (remainingSec < 0) remainingSec = 0;
  time_t endT = time(nullptr) + remainingSec;
  struct tm t;
  localtime_r(&endT, &t);
  int h12 = t.tm_hour % 12;
  if (h12 == 0) h12 = 12;
  char buf[16];
  snprintf(buf, sizeof(buf), "MUTED %d:%02d", h12, t.tm_min);
  return String(buf);
}

void drawMenuTile(int i, const char *label, const String &sub, uint16_t subColor) {
  int x = MENU_TILE_X[i % 2], y = MENU_TILE_Y[i / 2];
  M5.Display.drawRoundRect(x, y, MENU_TILE_W, MENU_TILE_H, 6, COLOR_LIGHTNING_TEXT);
  M5.Display.setTextDatum(top_center);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(COLOR_LIGHTNING_TEXT, COLOR_BG);
  M5.Display.drawString(label, x + MENU_TILE_W / 2, y + 9);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(subColor, COLOR_BG);
  M5.Display.drawString(sub, x + MENU_TILE_W / 2, y + 30);
  M5.Display.setTextDatum(top_left);
}

void drawSettingsMenu() {
  drawSettingsHeader("Settings");
  char buf[32];
  snprintf(buf, sizeof(buf), "Volume %d", alarmVolume);
  drawMenuTile(0, "SOUND", buf, COLOR_TEXT_SECONDARY);
  snprintf(buf, sizeof(buf), "Day %d / Night %d", DAY_BRIGHTNESS, NIGHT_BRIGHTNESS);
  drawMenuTile(1, "SCREEN", buf, COLOR_TEXT_SECONDARY);
  snprintf(buf, sizeof(buf), "Day %d / Night %d", ledDayLevel, ledNightLevel);
  drawMenuTile(2, "LEDS", buf, COLOR_TEXT_SECONDARY);
  drawMenuTile(3, "SCHEDULE", formatHourMinute12(NIGHT_START_HOUR, NIGHT_START_MINUTE) + " - " +
                              formatHourMinute12(NIGHT_END_HOUR, NIGHT_END_MINUTE), COLOR_TEXT_SECONDARY);
  drawMenuTile(4, "HISTORY", "Last 24 hours", COLOR_TEXT_SECONDARY);
  if (simulationActive) drawMenuTile(5, "TEST", "Test running", COLOR_LIGHTNING_TEXT);
  else drawMenuTile(5, "TEST", "Simulations", COLOR_TEXT_SECONDARY);
  if (audioMuted) drawSettingsTwoWayBar(muteBarLabel(), COLOR_BG, COLOR_STATUS_BAD);
  else drawSettingsTwoWayBar("MUTE", COLOR_LIGHTNING_TEXT, COLOR_BG);
}

void drawSettingsSoundPage() {
  drawSettingsHeader("Sound");
  drawSettingsStepperRow(46, "VOLUME", String(alarmVolume));
  drawSettingsButtonRow(46 + SETTINGS_ROW_STEPPER_H, 34, "TEST TONE", COLOR_LIGHTNING_TEXT, false, "");
  drawSettingsBackBar();
}

// Brightness values are the raw 0-255 setBrightness() inputs, stepped
// through the 9 real hardware levels (Session 10 finding).
void drawSettingsScreenPage() {
  drawSettingsHeader("Screen");
  drawSettingsStepperRow(46, "DAY BRIGHTNESS", String(DAY_BRIGHTNESS));
  drawSettingsStepperRow(46 + SETTINGS_ROW_STEPPER_H, "NIGHT BRIGHTNESS", String(NIGHT_BRIGHTNESS));
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(COLOR_TEXT_DIM, COLOR_BG);
  M5.Display.setTextSize(1);
  String note = "Night = the schedule (" + formatHourMinute12(NIGHT_START_HOUR, NIGHT_START_MINUTE) +
                " - " + formatHourMinute12(NIGHT_END_HOUR, NIGHT_END_MINUTE) + ")";
  M5.Display.drawString(note, 16, 176);
  drawSettingsBackBar();
}

void drawSettingsLedPage() {
  drawSettingsHeader("LEDs");
  drawSettingsStepperRow(46, "DAY LEVEL", String(ledDayLevel));
  drawSettingsStepperRow(46 + SETTINGS_ROW_STEPPER_H, "NIGHT LEVEL", String(ledNightLevel));
  if ((long)(ledPreviewUntilMillis - millis()) > 0) {
    M5.Display.fillRoundRect(16, 168, 288, 34, 6, COLOR_LIGHTNING_BG);
    M5.Display.drawRoundRect(16, 168, 288, 34, 6, COLOR_LIGHTNING_BORDER);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(COLOR_LIGHTNING_TEXT, COLOR_LIGHTNING_BG);
    char buf[32];
    snprintf(buf, sizeof(buf), "Showing amber at %d ...", ledPreviewLevel);
    M5.Display.drawString(buf, 160, 185);
  } else {
    M5.Display.setTextDatum(top_left);
    M5.Display.setTextColor(COLOR_TEXT_DIM, COLOR_BG);
    M5.Display.setTextSize(1);
    M5.Display.drawString("Tap - or + to see the LEDs at that level.", 16, 172);
    M5.Display.drawString("Critical alerts at night: 4x the night level.", 16, 186);
  }
  drawSettingsBackBar();
}

void drawSettingsSchedulePage() {
  drawSettingsHeader("Schedule");
  drawSettingsStepperRow(46, "NIGHT START",
                         formatHourMinute12(NIGHT_START_HOUR, NIGHT_START_MINUTE));
  drawSettingsStepperRow(46 + SETTINGS_ROW_STEPPER_H, "NIGHT END",
                         formatHourMinute12(NIGHT_END_HOUR, NIGHT_END_MINUTE));
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(COLOR_TEXT_DIM, COLOR_BG);
  M5.Display.setTextSize(1);
  M5.Display.drawString("Dims the screen and LEDs; quiet hours too.", 16, 176);
  drawSettingsBackBar();
}

// ---- Test Alerts page (v1.2, approved mockup) ----
// Runs the bench test from the device: the server's existing simulation
// endpoints, with the status line read from GET /api/simulation/status.
enum SimStatusLoad { SIM_STATUS_UNKNOWN, SIM_STATUS_OK };
SimStatusLoad simStatusLoad = SIM_STATUS_UNKNOWN;
bool simStatusActive = false;
String simStatusScenario = "";
int simStatusStep = 0;
int simStatusTotal = 0;
String simStatusDescription = "";

const int TEST_BTN_ROW1_Y = 82, TEST_BTN_ROW2_Y = 128, TEST_BTN_H = 40;
const int TEST_END_Y = 174, TEST_END_H = 30;
const int TEST_BTN_LEFT_X = 16, TEST_BTN_RIGHT_X = 164, TEST_BTN_W = 140;

const char *scenarioDisplayName(const String &s) {
  if (s == "nws_lifecycle") return "ONE ALERT";
  if (s == "multi_alert") return "SEVERAL ALERTS";
  if (s == "lightning") return "LIGHTNING";
  return "TEST";
}

void drawTestButton(int x, int y, int w, int h, const char *label, uint16_t color) {
  M5.Display.drawRoundRect(x, y, w, h, 6, color);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(color, COLOR_BG);
  M5.Display.drawString(label, x + w / 2, y + h / 2);
}

void drawSettingsTestPage() {
  drawSettingsHeader("Test Alerts");
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(1);
  bool running = (simStatusLoad == SIM_STATUS_OK && simStatusActive);
  if (simStatusLoad != SIM_STATUS_OK) {
    M5.Display.setTextColor(COLOR_STATUS_BAD, COLOR_BG);
    M5.Display.drawString("TEST STATUS UNKNOWN", 16, 46);
    M5.Display.setTextColor(COLOR_TEXT_SECONDARY, COLOR_BG);
    M5.Display.drawString("Server not responding.", 16, 58);
  } else if (running) {
    char head[56];
    snprintf(head, sizeof(head), "RUNNING: %s - STEP %d OF %d",
             scenarioDisplayName(simStatusScenario), simStatusStep + 1, simStatusTotal);
    M5.Display.setTextColor(COLOR_LIGHTNING_TEXT, COLOR_BG);
    M5.Display.drawString(head, 16, 46);
    String lines[2];
    bool truncated = false;
    int n = wrapInstructionText(simStatusDescription, lines, 2, 48, truncated);
    M5.Display.setTextColor(COLOR_TEXT_PRIMARY, COLOR_BG);
    for (int i = 0; i < n; i++) M5.Display.drawString(lines[i], 16, 58 + i * 10);
  } else {
    M5.Display.setTextColor(COLOR_NWS_CLEAR_TEXT, COLOR_BG);
    M5.Display.drawString("NO TEST RUNNING", 16, 46);
    M5.Display.setTextColor(COLOR_TEXT_SECONDARY, COLOR_BG);
    M5.Display.drawString("Start one below. Real alerts always end a test.", 16, 58);
  }
  drawTestButton(TEST_BTN_LEFT_X, TEST_BTN_ROW1_Y, TEST_BTN_W, TEST_BTN_H, "ONE ALERT", COLOR_LIGHTNING_TEXT);
  drawTestButton(TEST_BTN_RIGHT_X, TEST_BTN_ROW1_Y, TEST_BTN_W, TEST_BTN_H, "SEVERAL", COLOR_LIGHTNING_TEXT);
  drawTestButton(TEST_BTN_LEFT_X, TEST_BTN_ROW2_Y, TEST_BTN_W, TEST_BTN_H, "LIGHTNING", COLOR_LIGHTNING_TEXT);
  drawTestButton(TEST_BTN_RIGHT_X, TEST_BTN_ROW2_Y, TEST_BTN_W, TEST_BTN_H, "NEXT STEP",
                 running ? COLOR_LIGHTNING_TEXT : COLOR_TEXT_DIM);
  drawTestButton(16, TEST_END_Y, 288, TEST_END_H, "END TEST", running ? COLOR_STATUS_BAD : COLOR_TEXT_DIM);
  drawSettingsBackBar();
}

// ---- Settings page 5: Alert History (Session 12, approved mockup) ----
// The last 24 hours of NWS alerts, newest first, from the server's
// GET /api/alerts/history. Fetched only when the page is opened (never on
// the 60-second poll), so the normal fetch stays small. Tapping a row opens
// that alert's full detail, drawn by the same drawAlertBody() the NWS
// Alerts page uses; < > then step through the list and BACK returns to it.
// This is also how the alerts behind a "+N more" can be read.

struct HistoryAlert {
  String id, event, level, what, hazard, instruction;
  bool active = false, acknowledged = false;
  long firstSeenEpoch = 0, endedEpoch = 0;
};

const int HISTORY_MAX = 12;           // matches the server's bound
HistoryAlert historyAlerts[HISTORY_MAX];
int historyCount = 0;

// NOT_LOADED while the request is in flight; FAILED is shown as
// "History unavailable", never as an empty list -- the same rule as
// "NO ACTIVE ALERTS": a failed check must never read as "nothing happened".
enum HistoryLoad { HISTORY_NOT_LOADED, HISTORY_OK, HISTORY_FAILED };
HistoryLoad historyLoad = HISTORY_NOT_LOADED;
bool historySimulation = false;

int historyListOffset = 0;    // first entry shown when paging "older"
int historyDetailIndex = -1;  // -1 = list view, else the entry shown

// List geometry, shared by drawing and hit-testing.
const int HISTORY_ROW_Y0 = 56;
const int HISTORY_ROW_H = 38;
const int HISTORY_ROWS = 4;          // visible slots (3 + a paging row when > 4)
const int HISTORY_TEXT_X = 28;
const int HISTORY_LINE_MAX_CHARS = (304 - HISTORY_TEXT_X) / GLYPH_W_SIZE1;   // 46

// Entries shown per page: all four slots when everything fits, otherwise
// three entries plus a paging row in the last slot.
int historyEntriesPerPage() {
  return historyCount <= HISTORY_ROWS ? HISTORY_ROWS : HISTORY_ROWS - 1;
}

// "3:10 PM", or "Yesterday 6:12 PM" for anything before today (the list
// only covers 24 hours). "--:--" if the clock isn't synced.
String formatHistoryTime(long epoch, bool allowDayPrefix) {
  String t = formatEpochLocal(epoch);
  if (!allowDayPrefix || !timeSynced || epoch <= 0) return t;
  time_t e = (time_t)epoch, now = time(nullptr);
  struct tm te, tn;
  localtime_r(&e, &te);
  localtime_r(&now, &tn);
  if (te.tm_year == tn.tm_year && te.tm_yday == tn.tm_yday) return t;
  return "Yesterday " + t;
}

// One status line per entry, fitted to maxChars: "Since 3:10 PM  ACTIVE
// not acknowledged" or "1:05 PM - 2:30 PM  ended". The acknowledgement
// note is dropped (never cut mid-word) if the line would not fit.
String historyStatusLine(const HistoryAlert &a, int maxChars) {
  if (a.active) {
    String base = "Since " + formatHistoryTime(a.firstSeenEpoch, true) + "  ACTIVE";
    String withAck = base + (a.acknowledged ? "  acknowledged" : "  not acknowledged");
    return (int)withAck.length() <= maxChars ? withAck : fitChars(base, maxChars);
  }
  String line = formatHistoryTime(a.firstSeenEpoch, true) + " - " +
                formatHistoryTime(a.endedEpoch, false) + "  ended";
  return fitChars(line, maxChars);
}

void drawHistoryRow(int y, const HistoryAlert &a) {
  AlertColors c = alertColorsForLevel(a.level);
  uint16_t nameColor = a.active ? c.headline : COLOR_TEXT_SECONDARY;
  uint16_t lineColor = a.active ? c.detail : COLOR_TEXT_DIM;
  M5.Display.fillRect(16, y + 3, 4, 30, a.active ? c.headline : c.detail);
  M5.Display.setTextDatum(top_left);
  String name = shortenEventName(a.event);
  int size = eventTextSize(name, 304 - HISTORY_TEXT_X);
  M5.Display.setTextColor(nameColor, COLOR_BG);
  M5.Display.setTextSize(size);
  M5.Display.drawString(fitChars(name, HISTORY_LINE_MAX_CHARS), HISTORY_TEXT_X,
                        size == 2 ? y + 3 : y + 7);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(lineColor, COLOR_BG);
  M5.Display.drawString(historyStatusLine(a, HISTORY_LINE_MAX_CHARS), HISTORY_TEXT_X, y + 23);
  M5.Display.drawFastHLine(16, y + 37, 288, COLOR_SEPARATOR);
}

void drawAlertHistoryList() {
  drawSettingsHeader("Alert History");
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(COLOR_TEXT_DIM, COLOR_BG);

  if (historyLoad == HISTORY_NOT_LOADED) {
    M5.Display.drawString("LAST 24 HOURS, NEWEST FIRST", 16, 44);
    M5.Display.setTextSize(2);
    M5.Display.drawString("Loading...", 16, 72);
  } else if (historyLoad == HISTORY_FAILED) {
    M5.Display.drawString("LAST 24 HOURS, NEWEST FIRST", 16, 44);
    M5.Display.setTextSize(2);
    M5.Display.drawString("History unavailable", 16, 72);
    M5.Display.setTextSize(1);
    M5.Display.drawString(WiFi.status() == WL_CONNECTED
                              ? "Server not responding -- this is NOT"
                              : "Wi-Fi disconnected -- this is NOT",
                          16, 96);
    M5.Display.drawString("the same as no alerts.", 16, 106);
  } else if (historyCount == 0) {
    M5.Display.drawString("LAST 24 HOURS, NEWEST FIRST", 16, 44);
    M5.Display.fillCircle(28, 80, 5, COLOR_NWS_CLEAR_DOT);
    M5.Display.setTextColor(COLOR_NWS_CLEAR_TEXT, COLOR_BG);
    M5.Display.setTextSize(2);
    M5.Display.drawString("No NWS alerts in", 42, 72);
    M5.Display.drawString("the last 24 hours", 42, 92);
  } else {
    M5.Display.drawString("LAST 24 HOURS, NEWEST FIRST -- TAP FOR DETAIL", 16, 44);
    int perPage = historyEntriesPerPage();
    for (int slot = 0; slot < perPage; slot++) {
      int i = historyListOffset + slot;
      if (i >= historyCount) break;
      drawHistoryRow(HISTORY_ROW_Y0 + slot * HISTORY_ROW_H, historyAlerts[i]);
    }
    if (perPage < HISTORY_ROWS) {
      int remaining = historyCount - (historyListOffset + perPage);
      String pager = "Newest -- tap to show";
      if (remaining > 0) pager = String(remaining) + " older -- tap to show";
      M5.Display.setTextDatum(top_left);
      M5.Display.setTextColor(COLOR_LIGHTNING_TEXT, COLOR_BG);
      M5.Display.setTextSize(2);
      M5.Display.drawString(pager, HISTORY_TEXT_X,
                            HISTORY_ROW_Y0 + (HISTORY_ROWS - 1) * HISTORY_ROW_H + 13);
    }
  }
  drawSettingsBackBar();
}

void drawAlertHistoryDetail() {
  const HistoryAlert &a = historyAlerts[historyDetailIndex];
  M5.Display.fillScreen(COLOR_BG);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(COLOR_TEXT_PRIMARY, COLOR_BG);
  M5.Display.setTextSize(3);
  M5.Display.drawString("History", 16, 10);
  M5.Display.setTextDatum(top_right);
  M5.Display.setTextColor(COLOR_TEXT_DIM, COLOR_BG);
  M5.Display.setTextSize(1);
  M5.Display.drawString(String(historyDetailIndex + 1) + " of " + String(historyCount), 304, 14);
  M5.Display.drawFastHLine(16, 38, 288, COLOR_SEPARATOR);

  // Same block, fonts and colour tier as the NWS Alerts page header.
  AlertColors c = alertColorsForLevel(a.level);
  M5.Display.fillRect(NWS_HEADER_X, NWS_HEADER_Y, NWS_HEADER_W, NWS_HEADER_H, c.bg);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(c.headline, c.bg);
  String name = shortenEventName(a.event);
  int size = eventTextSize(name, 272);
  M5.Display.setTextSize(size);
  M5.Display.drawString(name, 24, size == 2 ? 54 : 58);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(c.detail, c.bg);
  M5.Display.drawString(historyStatusLine(a, (296 - 24) / GLYPH_W_SIZE1), 24, 72);

  drawAlertBody(a.what, a.hazard, a.instruction);
  drawSettingsNavBar("BACK");
}

void drawAlertHistoryPage() {
  if (historyDetailIndex >= 0 && historyDetailIndex < historyCount) {
    drawAlertHistoryDetail();
  } else {
    historyDetailIndex = -1;
    drawAlertHistoryList();
  }
}

// Opens the list fresh: shows "Loading...", fetches (blocking, same
// timeouts as every other request), then redraws with the result.
void openAlertHistory() {
  historyLoad = HISTORY_NOT_LOADED;
  historyListOffset = 0;
  historyDetailIndex = -1;
  renderPage(currentPage);
  fetchAlertHistory();
  renderPage(currentPage);
}

// Touch handling for the History page. Returns true if it consumed the
// tap. The nav bar is only taken over in detail view (< > step, centre =
// BACK to the list); in the list view the usual BACK / DONE bar applies.
bool handleAlertHistoryTouch(int x, int y) {
  if (historyDetailIndex >= 0) {
    if (y < SETTINGS_NAV_Y) return true;   // reading; ignore stray taps
    if (x < SETTINGS_NAV_PREV_X1) {
      historyDetailIndex = (historyDetailIndex + historyCount - 1) % historyCount;
    } else if (x < SETTINGS_NAV_DONE_X1) {
      historyDetailIndex = -1;             // BACK to the list
    } else {
      historyDetailIndex = (historyDetailIndex + 1) % historyCount;
    }
    renderPage(currentPage);
    return true;
  }
  if (y >= SETTINGS_NAV_Y) return false;   // list view: normal Settings bar
  if (historyLoad != HISTORY_OK || historyCount == 0) return true;
  if (y < HISTORY_ROW_Y0 || y >= HISTORY_ROW_Y0 + HISTORY_ROWS * HISTORY_ROW_H) return true;

  int slot = (y - HISTORY_ROW_Y0) / HISTORY_ROW_H;
  int perPage = historyEntriesPerPage();
  if (slot >= perPage) {
    // The paging row: next page of older entries, wrapping to the newest.
    historyListOffset += perPage;
    if (historyListOffset >= historyCount) historyListOffset = 0;
  } else {
    int i = historyListOffset + slot;
    if (i >= historyCount) return true;
    historyDetailIndex = i;
  }
  renderPage(currentPage);
  return true;
}

void drawSettingsPage() {
  switch (settingsSubPage) {
    case SV_SOUND:    drawSettingsSoundPage(); break;
    case SV_SCREEN:   drawSettingsScreenPage(); break;
    case SV_LEDS:     drawSettingsLedPage(); break;
    case SV_SCHEDULE: drawSettingsSchedulePage(); break;
    case SV_HISTORY:  drawAlertHistoryPage(); break;
    case SV_TEST:     drawSettingsTestPage(); break;
    default:          settingsSubPage = SV_MENU; drawSettingsMenu(); break;
  }
}

// Opens a page from the menu (or directly, e.g. "+N more" -> History).
void openSettingsView(int view) {
  settingsSubPage = view;
  if (view == SV_HISTORY) { openAlertHistory(); return; }
  if (view == SV_TEST) {
    renderPage(currentPage);
    fetchSimulationStatus();
  }
  renderPage(currentPage);
}

// Leaving Settings for good (DONE, or the idle timeout).
void closeSettings() {
  saveSettingsIfChanged();
  settingsSubPage = SV_MENU;
  currentPage = PAGE_NOW;
  renderPage(currentPage);
}

// After a Test Alerts button: refresh the status and the conditions at
// once, rather than waiting up to 60 s for the next poll.
void refreshAfterTestAction() {
  fetchSimulationStatus();
  fetchConditions();
  lastFetchAttemptMillis = millis();
  renderPage(currentPage);
}

// Settings touch handling (v1.2). Geometry constants are shared with the
// drawing code above, so drawn and tappable regions can't drift apart.
void handleSettingsTouch(int x, int y) {
  if (settingsSubPage == SV_HISTORY && handleAlertHistoryTouch(x, y)) return;

  // Bottom bar: left half = MUTE (menu) or BACK (pages); right = DONE.
  if (y >= SETTINGS_NAV_Y) {
    if (x >= SETTINGS_NAV_HALF_X) {
      Serial.println("[settings] DONE -- returning to Now");
      closeSettings();
    } else if (settingsSubPage == SV_MENU) {
      toggleMute();
      renderPage(currentPage);
    } else {
      saveSettingsIfChanged();
      settingsSubPage = SV_MENU;
      renderPage(currentPage);
    }
    return;
  }

  if (settingsSubPage == SV_MENU) {
    for (int i = 0; i < 6; i++) {
      int tx = MENU_TILE_X[i % 2], ty = MENU_TILE_Y[i / 2];
      if (x >= tx && x < tx + MENU_TILE_W && y >= ty && y < ty + MENU_TILE_H) {
        static const int views[6] = {SV_SOUND, SV_SCREEN, SV_LEDS, SV_SCHEDULE, SV_HISTORY, SV_TEST};
        Serial.printf("[settings] menu -> view %d\n", views[i]);
        openSettingsView(views[i]);
        return;
      }
    }
    return;
  }

  // Stepper rows: shared geometry with drawSettingsStepperRow().
  const int ROW0_Y = 46, ROW1_Y = 46 + SETTINGS_ROW_STEPPER_H;
  bool inMinusX = (x >= 16 && x < 52);
  bool inPlusX = (x >= 268 && x < 304);
  bool inRow0Y = (y >= ROW0_Y + SETTINGS_STEPPER_BOX_Y_OFFSET && y < ROW0_Y + SETTINGS_STEPPER_BOX_Y_OFFSET + 30);
  bool inRow1Y = (y >= ROW1_Y + SETTINGS_STEPPER_BOX_Y_OFFSET && y < ROW1_Y + SETTINGS_STEPPER_BOX_Y_OFFSET + 30);
  int direction = inMinusX ? -1 : (inPlusX ? 1 : 0);

  if (settingsSubPage == SV_SOUND) {
    if (direction != 0 && inRow0Y) {
      int newVol = (int)alarmVolume + direction * ALARM_VOLUME_STEP;
      if (newVol < 0) newVol = 0;
      if (newVol > 255) newVol = 255;
      alarmVolume = (uint8_t)newVol;
      Serial.printf("[settings] alarmVolume -> %d\n", alarmVolume);
      applyAlarmVolumeImmediately();
      renderPage(currentPage);
      return;
    }
    if (x >= 16 && x < 304 && y >= 110 && y < 144) playTestTone();
    return;
  }

  if (settingsSubPage == SV_SCREEN && direction != 0 && (inRow0Y || inRow1Y)) {
    if (inRow0Y) DAY_BRIGHTNESS = stepBrightness(DAY_BRIGHTNESS, direction);
    else NIGHT_BRIGHTNESS = stepBrightness(NIGHT_BRIGHTNESS, direction);
    Serial.printf("[settings] brightness day %d night %d\n", DAY_BRIGHTNESS, NIGHT_BRIGHTNESS);
    applyCurrentBrightnessImmediately();
    renderPage(currentPage);
    return;
  }

  if (settingsSubPage == SV_LEDS && direction != 0 && (inRow0Y || inRow1Y)) {
    if (inRow0Y) {
      ledDayLevel = stepLedLevel(ledDayLevel, LED_DAY_STEPS, direction);
      ledPreviewLevel = ledDayLevel;
    } else {
      ledNightLevel = stepLedLevel(ledNightLevel, LED_NIGHT_STEPS, direction);
      ledPreviewLevel = ledNightLevel;
    }
    ledPreviewUntilMillis = millis() + LED_PREVIEW_MS;
    Serial.printf("[settings] LED levels day %d night %d (preview %d)\n",
                  ledDayLevel, ledNightLevel, ledPreviewLevel);
    renderPage(currentPage);
    return;
  }

  if (settingsSubPage == SV_SCHEDULE && direction != 0 && (inRow0Y || inRow1Y)) {
    if (inRow0Y) stepScheduleTime(NIGHT_START_HOUR, NIGHT_START_MINUTE, direction);
    else stepScheduleTime(NIGHT_END_HOUR, NIGHT_END_MINUTE, direction);
    Serial.printf("[settings] schedule %02d:%02d - %02d:%02d\n", NIGHT_START_HOUR,
                  NIGHT_START_MINUTE, NIGHT_END_HOUR, NIGHT_END_MINUTE);
    renderPage(currentPage);
    return;
  }

  if (settingsSubPage == SV_TEST) {
    bool running = (simStatusLoad == SIM_STATUS_OK && simStatusActive);
    auto in = [&](int bx, int by, int bw, int bh) {
      return x >= bx && x < bx + bw && y >= by && y < by + bh;
    };
    const char *startScenario = nullptr;
    if (in(TEST_BTN_LEFT_X, TEST_BTN_ROW1_Y, TEST_BTN_W, TEST_BTN_H)) startScenario = "nws_lifecycle";
    else if (in(TEST_BTN_RIGHT_X, TEST_BTN_ROW1_Y, TEST_BTN_W, TEST_BTN_H)) startScenario = "multi_alert";
    else if (in(TEST_BTN_LEFT_X, TEST_BTN_ROW2_Y, TEST_BTN_W, TEST_BTN_H)) startScenario = "lightning";
    if (startScenario) {
      postSimulation("/api/simulation/start", startScenario);
      refreshAfterTestAction();
      return;
    }
    if (running && in(TEST_BTN_RIGHT_X, TEST_BTN_ROW2_Y, TEST_BTN_W, TEST_BTN_H)) {
      postSimulation("/api/simulation/advance", nullptr);
      refreshAfterTestAction();
      return;
    }
    if (running && in(16, TEST_END_Y, 288, TEST_END_H)) {
      postSimulation("/api/simulation/stop", nullptr);
      refreshAfterTestAction();
      return;
    }
    return;
  }

  Serial.printf("[settings] content tap at (%d,%d) on view %d -- no hotspot there\n",
                x, y, settingsSubPage);
}

void renderPageInner(Page p) {
  switch (p) {
    case PAGE_NOW:
      // On battery with no NWS alert in effect, the POWER LOST screen
      // stands in for Now (Session 15). An alert keeps the normal Now
      // screen, since alerts outrank the power screen.
      if (powerScreenShouldShow()) {
        drawPowerLostPage();
        return;
      }
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
      if (ackFeedback != ACK_FEEDBACK_NONE) {
        drawAckFeedback();   // takes the bar for its 2-3 seconds
        return;
      }
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
  } else if (lastAppliedBrightnessMode == 0 || lastAppliedBrightnessMode == 2) {
    // 2 = night, temporarily woken (Critical alert or a tap) -- see
    // handleNightDimming().
    M5.Display.setBrightness(DAY_BRIGHTNESS);
  }
  // mode == -1 (not yet established) is left alone; handleNightDimming()
  // will set a real value on its very next pass regardless.
}

// ---- Audio: alarm tones by alert level (v1.0 requirement) ----
//
// API confirmed from real M5Stack/M5Unified source before writing any of
// this (Session 11): M5.Speaker.tone(freq, durationMs, channel,
// stop_current_sound) is NON-BLOCKING -- it hands the tone to a
// DMA-backed background task and returns immediately, so the whole
// pattern below is driven by comparing millis() against scheduled times,
// never by delay(), matching every other periodic subsystem in this
// file (Wi-Fi reconnect, night dimming). isPlaying()/stop() exist per
// real source but aren't needed here: tone()'s own duration parameter
// already handles stopping each individual tone. A real GitHub issue
// (m5stack/M5Unified #98) reports noisy output on this hardware below
// 1kHz, so every alarm tone here stays comfortably above that.
const int ALERT_TONE_CHANNEL = 0;  // dedicated channel; never shared with Test Tone's own playback

// Placeholder pattern parameters -- frequencies, durations and the
// Warning repeat count are all guesses pending a real daytime bench
// test, per the project's own guardrail on that. The Critical 2-minute
// cap and Watch/Warning/Critical tone-count-by-severity are Dan's
// explicit decisions (this session), not placeholders.
struct AlarmPatternParams {
  int toneCount;             // tones per group: 3=Critical, 2=Warning, 1=Watch
  int freqHz;
  int toneMs;
  int gapMs;                 // silence between tones within one group
  int groupPauseMs;          // silence between repeated groups
  int maxGroups;             // 0 = no sound; >=1 = stop after N groups; -1 = unlimited (time-capped instead)
  unsigned long maxDurationMs;  // 0 = no time cap (Warning/Watch self-limit via maxGroups instead)
};

AlarmPatternParams alarmParamsFor(const String &level) {
  if (level == "critical") {
    // Dan's explicit decision: an SOS-style 3-tone group, repeating,
    // capped at 2 minutes if never acknowledged -- past that, continuing
    // to sound accomplishes nothing (no one present, or busy preparing),
    // so it stops and leaves the alert visible on screen instead.
    return {3, 1800, 150, 100, 700, -1, 120000UL};
  }
  if (level == "warning") {
    return {2, 1400, 150, 100, 700, 5, 0UL};  // 5 groups is a placeholder "limited repeats"
  }
  if (level == "watch") {
    return {1, 1000, 300, 0, 0, 1, 0UL};  // a single soft chime, once
  }
  // advisory / informational / unrecognized: no sound, banner only,
  // matching the already-approved visual-only treatment for these tiers.
  return {0, 0, 0, 0, 0, 0, 0UL};
}

// Episode identity (which alert+level this state belongs to) is tracked
// SEPARATELY from whether the episode is still active -- a Watch chime
// that has already played its one tone, or a Warning/Critical that has
// hit its repeat/time cap, must stay silent for that same alert+level
// for as long as nothing about it changes, not restart on every
// subsequent loop() pass just because needs_alert is still true. Mute
// and quiet hours PAUSE ticking without ending the episode, so
// unmuting mid-Critical-alarm resumes it rather than silently losing it.
bool alarmEpisodeActive = false;
String alarmEpisodeAlertId = "";
String alarmEpisodeLevel = "";
AlarmPatternParams activeAlarmParams;
int alarmToneIndex = 0;
int alarmGroupsDone = 0;
bool alarmInGap = false;
unsigned long alarmEpisodeStartedAtMillis = 0;
unsigned long alarmNextActionAtMillis = 0;

// Volume: raw 0-255 M5Unified input, same "no false precision" reasoning
// as brightness -- unlike brightness, no hardware quirk has been found
// (or looked for) that collapses this into coarse steps, so it isn't
// stepped through a fixed level table the way brightness is; the
// Settings stepper just moves it by a flat increment. In memory only,
// like every other Settings value -- resets on reboot until persistence
// is built.
uint8_t alarmVolume = 200;
const int ALARM_VOLUME_STEP = 25;

// Mute: temporary by requirement (project rules: "Mute must be
// temporary and visibly indicated"). Duration is a placeholder -- Dan
// hasn't specified one -- fixed at 1 hour per tap rather than
// adjustable, simplest thing that satisfies "temporary."
bool audioMuted = false;
unsigned long muteUntilMillis = 0;
const unsigned long MUTE_DURATION_MS = 3600000UL;  // 1 hour placeholder

// Quiet hours reuse the SAME schedule as night dimming (isNightTimeNow())
// rather than a separate one -- Dan's policy (confirmed this session):
// Warnings and Critical always sound regardless of quiet hours; only a
// Watch chime is silenced overnight. This is an assumption, not
// separately confirmed with Dan -- flagged as such; a distinct
// quiet-hours schedule would be a small, contained change if wanted
// (a second NIGHT_START/END pair) rather than a redesign.
bool shouldSuppressForQuietHours(const String &level) {
  return level == "watch" && isNightTimeNow();
}

void applyAlarmVolumeImmediately() {
  M5.Speaker.setVolume(alarmVolume);
}

// A short, single representative tone so Dan can hear what the CURRENT
// volume setting sounds like without waiting for a real or simulated
// alert. Deliberately respects mute -- muted should mean muted, even
// for a deliberate test -- but always logs, so a tap during mute still
// has some confirmation it registered.
void playTestTone() {
  if (audioMuted) {
    Serial.println("[settings] Test Tone skipped -- currently muted");
    return;
  }
  M5.Speaker.setVolume(alarmVolume);
  M5.Speaker.tone(1000, 300, ALERT_TONE_CHANNEL, true);
  Serial.printf("[settings] Test Tone played at volume %d\n", alarmVolume);
}

// Formats when the current mute period ends, for the Mute button's
// sub-caption ("Until 7:42 PM"). Falls back to a plain label if the
// clock isn't synced, since a wall-clock time would be a guess.
String formatMuteUntil() {
  if (!timeSynced) return "Temporary (clock not synced)";
  long remainingSec = (long)(muteUntilMillis - millis()) / 1000;
  if (remainingSec < 0) remainingSec = 0;
  long targetEpoch = (long)time(nullptr) + remainingSec;
  return "Until " + formatEpochLocal(targetEpoch);
}

// Toggles mute from the Settings Mute button: OFF->ON starts a fresh
// MUTE_DURATION_MS window; ON->OFF (tapping to cancel early) clears it
// immediately. A currently-running alarm is stopped the same way an
// acknowledgement stops one -- see handleAlarm()'s own mute check below,
// which runs on the very next loop() pass regardless.
void toggleMute() {
  audioMuted = !audioMuted;
  if (audioMuted) {
    muteUntilMillis = millis() + MUTE_DURATION_MS;
    Serial.println("[settings] Mute ON");
  } else {
    Serial.println("[settings] Mute OFF (cancelled)");
  }
}

// Called every loop() pass. Auto-clears mute once its window elapses --
// "temporary" is a hard requirement, not just a UI label that happens to
// be true if someone remembers to tap it off.
void handleMuteExpiry() {
  if (audioMuted && millis() >= muteUntilMillis) {
    audioMuted = false;
    Serial.println("[settings] Mute expired -- audio re-enabled");
    // Take "MUTED" off the Now screen now, not at the next 60 s refresh.
    if (currentPage == PAGE_NOW) renderPage(currentPage);
  }
}

// Called every loop() pass. Drives the whole alarm pattern (tone
// timing, inter-tone gaps, group repeats, the Critical time cap) purely
// by comparing millis() against scheduled times -- see the module
// comment above for why this can never use delay().
void handleAlarm() {
  bool sameEpisodeAsBefore = (alarmEpisodeAlertId == nwsFirstAlertId &&
                              alarmEpisodeLevel == nwsFirstAlertLevel);

  if (!nwsFirstAlertNeedsAlert) {
    // Acknowledged, or no active alert needing sound: end whatever
    // episode might be in flight. A later alert (even a different one
    // that happens to reuse this same id after expiring) will be a
    // fresh episode by definition once it re-appears with needs_alert.
    if (alarmEpisodeActive) {
      alarmEpisodeActive = false;
      Serial.println("[alarm] episode ended (acknowledged or cleared)");
    }
    return;
  }

  if (!sameEpisodeAsBefore) {
    // A genuinely new alert, or the same alert escalated to a new level
    // (which per Dan's confirmed policy clears acknowledgement and
    // re-alerts) -- start a brand-new episode.
    activeAlarmParams = alarmParamsFor(nwsFirstAlertLevel);
    alarmEpisodeAlertId = nwsFirstAlertId;
    alarmEpisodeLevel = nwsFirstAlertLevel;
    if (activeAlarmParams.toneCount == 0) {
      alarmEpisodeActive = false;  // advisory/informational: silent, but the key is still "claimed"
      return;
    }
    alarmEpisodeActive = true;
    alarmToneIndex = 0;
    alarmGroupsDone = 0;
    alarmInGap = false;
    alarmEpisodeStartedAtMillis = millis();
    alarmNextActionAtMillis = millis();  // fire the first tone on this very pass
    Serial.printf("[alarm] starting '%s' for alert %s\n", alarmEpisodeLevel.c_str(), alarmEpisodeAlertId.c_str());
  }

  // This exact episode already ran its course (hit its cap or repeat
  // limit) or is silent by design -- do not restart on your own just
  // because needs_alert is still true on a later pass.
  if (!alarmEpisodeActive) return;

  // Mute and quiet hours PAUSE ticking rather than ending the episode --
  // unmuting (or morning arriving) resumes from exactly where it left
  // off, rather than losing an unacknowledged alarm to a temporary mute.
  // Mute silences everything EXCEPT Critical (Dan, Session 13) -- the
  // same rule quiet hours already follow. Mute itself stays on for every
  // other level.
  bool mutedForThisLevel = audioMuted && alarmEpisodeLevel != "critical";
  if (mutedForThisLevel || shouldSuppressForQuietHours(alarmEpisodeLevel)) return;

  unsigned long now = millis();
  if (now < alarmNextActionAtMillis) return;

  if (activeAlarmParams.maxDurationMs > 0 &&
      (now - alarmEpisodeStartedAtMillis) >= activeAlarmParams.maxDurationMs) {
    alarmEpisodeActive = false;
    Serial.println("[alarm] time cap reached -- episode over (the alert itself stays visible on screen)");
    return;
  }

  if (!alarmInGap) {
    M5.Speaker.setVolume(alarmVolume);
    M5.Speaker.tone(activeAlarmParams.freqHz, activeAlarmParams.toneMs, ALERT_TONE_CHANNEL, true);
    alarmToneIndex++;
    if (alarmToneIndex < activeAlarmParams.toneCount) {
      alarmNextActionAtMillis = now + activeAlarmParams.toneMs + activeAlarmParams.gapMs;
    } else {
      alarmToneIndex = 0;
      alarmGroupsDone++;
      if (activeAlarmParams.maxGroups > 0 && alarmGroupsDone >= activeAlarmParams.maxGroups) {
        alarmEpisodeActive = false;
        Serial.println("[alarm] reached its repeat limit -- episode over (the alert stays visible on screen)");
        return;
      }
      alarmInGap = true;
      alarmNextActionAtMillis = now + activeAlarmParams.toneMs + activeAlarmParams.groupPauseMs;
    }
  } else {
    alarmInGap = false;
    alarmNextActionAtMillis = now;  // play the next group's first tone immediately on the next check
  }
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
// ---- Night wake (Session 13) ----
// At night the screen is temporarily raised to DAY brightness in two
// cases, and drops back to night brightness on its own afterwards:
//  - a Critical alert that still needs acknowledgement (Dan: Critical
//    only, not Warnings) -- until it is acknowledged or ends;
//  - a tap on the dimmed screen -- for NIGHT_TAP_WAKE_MS after the last
//    touch. That waking tap is swallowed (does nothing else), so a
//    half-asleep tap can't advance a page or acknowledge an alert.
// Not on the Settings brightness page, where the person is judging the
// night level itself and needs to see it.
const unsigned long NIGHT_TAP_WAKE_MS = 30000;
unsigned long nightTapWakeUntilMillis = 0;
bool nightTapWakeActive = false;

bool criticalNeedsWake() {
  return nwsAlertCount > 0 && nwsFirstAlertLevel == "critical" && nwsFirstAlertNeedsAlert;
}

bool tapWakeAllowedHere() {
  return !(currentPage == PAGE_SETTINGS && settingsSubPage == SV_SCREEN);
}

void handleNightDimming();   // defined just below

// Called on every new touch. Returns true if this touch only woke the
// screen and must not be acted on.
bool nightWakeOnTouch() {
  if (!isNightTimeNow() || !tapWakeAllowedHere()) return false;
  bool wasDim = (lastAppliedBrightnessMode == 1);
  nightTapWakeActive = true;
  nightTapWakeUntilMillis = millis() + NIGHT_TAP_WAKE_MS;   // every touch extends it
  if (wasDim) {
    handleNightDimming();   // brighten now, not on the next loop pass
    Serial.println("[brightness] tap woke the dimmed screen (touch swallowed)");
  }
  return wasDim;
}

void handleNightDimming() {
  bool night = isNightTimeNow();
  if (nightTapWakeActive && (!tapWakeAllowedHere() ||
                             (long)(millis() - nightTapWakeUntilMillis) >= 0)) {
    nightTapWakeActive = false;
  }
  bool woken = night && (criticalNeedsWake() || nightTapWakeActive);
  int mode = night ? (woken ? 2 : 1) : 0;   // 0 day, 1 night, 2 night-but-woken
  if (mode == lastAppliedBrightnessMode) return;
  uint8_t level = (mode == 1) ? NIGHT_BRIGHTNESS : DAY_BRIGHTNESS;
  M5.Display.setBrightness(level);
  Serial.printf("[brightness] switched to %s (raw %d)\n",
                mode == 0 ? "DAY" : (mode == 1 ? "NIGHT" : "NIGHT-WOKEN"), level);
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
  // Not while on battery (Session 15): in a power cut the router is down
  // too, and a reboot would cost the clock (no NTP to get it back), reset
  // the on-battery timer and re-sound the power-loss chime every 10
  // minutes. The 10 minutes restart counting when wall power returns.
  if (downFor >= WIFI_REBOOT_AFTER_MS && !onBatteryPower) {
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
  } else {
    lightningStrikeCountRecent = 0;
  }
  JsonVariant close = doc["lightning"]["close"];
  lightningCloseActive = close["active"] | false;
  lightningCloseRadiusMi = close["radius_mi"] | 10.0f;
  lightningCloseHoldMinutes = close["hold_minutes"] | 30;
  if (lightningCloseActive) {
    lightningCloseLastDistanceMi = close["last_distance_mi"] | 0.0f;
    lightningCloseLastEpoch = close["last_strike_epoch"] | 0L;
    lightningCloseMinutesSince = close["minutes_since"] | 0;
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
    nwsFirstAlertLevel = alerts[0]["level"] | "";
  } else {
    nwsFirstAlertEvent = "";
    nwsFirstAlertExpires = "";
    nwsFirstAlertInstruction = "";
    nwsFirstAlertId = "";
    nwsFirstAlertWhat = "";
    nwsFirstAlertHazard = "";
    nwsFirstAlertAcknowledged = false;
    nwsFirstAlertNeedsAlert = false;
    nwsFirstAlertLevel = "";
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

const char *ALERT_HISTORY_URL = SERVER_BASE_URL "/api/alerts/history";

// Alert History (Session 12). Blocking, like every request here, with the
// same short timeouts; called only when the History page is opened. Any
// failure leaves historyLoad = HISTORY_FAILED ("History unavailable").
void fetchAlertHistory() {
  historyCount = 0;
  historyLoad = HISTORY_FAILED;
  historySimulation = false;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[history] skipped -- Wi-Fi not connected");
    return;
  }

  HTTPClient http;
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_READ_TIMEOUT_MS);
  http.begin(ALERT_HISTORY_URL);
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[history] GET failed: %d (%s)\n", httpCode,
                  HTTPClient::errorToString(httpCode).c_str());
    http.end();
    return;
  }
  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.printf("[history] JSON parse FAILED: %s\n", error.c_str());
    return;
  }

  historySimulation = doc["simulation"] | false;
  JsonArray alerts = doc["alerts"];
  for (JsonObject a : alerts) {
    if (historyCount >= HISTORY_MAX) break;
    HistoryAlert &h = historyAlerts[historyCount++];
    h.id = a["id"] | "";
    h.event = a["event"] | "";
    h.level = a["level"] | "";
    h.what = a["what"] | "";
    h.hazard = a["hazard"] | "";
    h.instruction = a["instruction"] | "";
    h.active = String(a["status"] | "") == "active";
    h.acknowledged = a["acknowledged"] | false;
    h.firstSeenEpoch = a["first_seen_epoch"] | 0L;
    h.endedEpoch = a["ended_epoch"] | 0L;
  }
  historyLoad = HISTORY_OK;
  Serial.printf("[history] loaded %d alert(s)%s\n", historyCount,
                historySimulation ? " (simulation)" : "");
}

// Phase 2g: alert acknowledgement. Same blocking-HTTP characteristic as
// fetchConditions() above, but user-triggered rather than on a 60s
// timer, so the impact is a single brief pause on tap rather than a
// recurring background one.
bool ackAlert(const String &alertId) {
  if (WiFi.status() != WL_CONNECTED || alertId.length() == 0) {
    Serial.println("[ack] Skipped -- no Wi-Fi or no alert id");
    return false;
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
  return httpCode == 200;
}

// Test Alerts page (v1.2): drives the server's simulation endpoints, the
// same real alert-lifecycle path the PowerShell bench test uses. Blocking,
// with the usual short timeouts. Returns true on HTTP 200.
bool postSimulation(const char *path, const char *scenario) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[test] skipped -- no Wi-Fi");
    return false;
  }
  HTTPClient http;
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_READ_TIMEOUT_MS);
  http.begin(String(SERVER_BASE_URL) + path);
  http.addHeader("Content-Type", "application/json");
  String body = "{}";
  if (scenario) {
    JsonDocument doc;
    doc["scenario"] = scenario;
    body = "";
    serializeJson(doc, body);
  }
  int httpCode = http.POST(body);
  Serial.printf("[test] POST %s %s -> %d\n", path, scenario ? scenario : "", httpCode);
  http.end();
  return httpCode == 200;
}

void fetchSimulationStatus() {
  simStatusLoad = SIM_STATUS_UNKNOWN;
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_READ_TIMEOUT_MS);
  http.begin(String(SERVER_BASE_URL) + "/api/simulation/status");
  int httpCode = http.GET();
  if (httpCode != 200) {
    Serial.printf("[test] status GET failed: %d\n", httpCode);
    http.end();
    return;
  }
  String payload = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, payload)) return;
  simStatusActive = doc["active"] | false;
  simStatusScenario = doc["scenario"] | "";
  simStatusStep = doc["step"] | 0;
  simStatusTotal = doc["total_steps"] | 0;
  simStatusDescription = doc["description"] | "";
  simStatusLoad = SIM_STATUS_OK;
}

// ---- Power loss and M5GO Bottom3 LEDs (post-v1.0 item 1, Session 15) ----
//
// Hardware facts, each confirmed on Dan's unit with the diagnostic sketch
// (diagnostics/led_power) on 2026-10-01, not just from documentation:
//  - 10 RGB LEDs on GPIO5 (M5-Bus pin 8), GRB order: 1-5 down the right
//    side, 6-10 up the left. All ten always show the same thing here.
//  - The AXP2101 power chip reads USB voltage ~5000 mV on wall power and
//    0 within about a second of unplugging; the M5GO battery keeps the
//    device and LEDs running and reads through the same chip.
//  - After M5.Power.powerOff() on battery, the device starts by itself
//    2-3 s after USB power returns -- the critical requirement.
//  - Visible LED levels (brightest channel out of 255): 16 is the night
//    floor with red and amber still distinct; 32-64 for daytime; 128-255
//    for urgent daytime.
// M5.Led.setBrightness() is left at 255 (no scaling): its own curve
// wipes out the smaller colour channel at low settings, so amber turns
// red. Colours are scaled here instead, keeping every lit channel >= 1.
//
// LED meaning (approved by Dan, Session 15). Off = all clear.
//   Unacknowledged Warning/Critical  red bursts, 3 (Critical) / 2 flashes
//   Unacknowledged Watch             amber, 1 flash per burst
//   On battery (power lost)          slow blue blink
//   Alert in effect, nothing needed  steady dim glow in its colour
//     (acknowledged, or an Advisory/Statement that never sounds)
//   Lightning within the radius      white double flicker every 10 s
//   Status unknown                   steady dim blue (NWS or lightning
//                                    can't be vouched for -- never dark)
//   Low battery                      5 fast blue flashes, then power-off
// The first matching line wins.

const int LED_DATA_PIN = 5;
const int LED_COUNT = 10;

struct LedColor { uint8_t r, g, b; };
const LedColor LED_RED   = {255, 0, 0};
const LedColor LED_AMBER = {255, 96, 0};
const LedColor LED_WHITE = {255, 255, 255};
const LedColor LED_BLUE  = {0, 0, 255};

// Levels (Dan, Session 15).
// Levels (v1.2): derived from the two Settings levels, see the top of
// the file. Defaults reproduce v1.1 exactly (16 / 64 / 32 / 64 / 255).
uint8_t ledLevelNight() { return ledNightLevel; }
uint8_t ledLevelNightCritical() {          // unacknowledged Critical only
  int v = ledNightLevel * 4;
  return v > 255 ? 255 : (uint8_t)v;
}
uint8_t ledLevelDayDim() { return ledDayLevel / 2; }   // steady glow / unknown
uint8_t ledLevelDay() { return ledDayLevel; }
const uint8_t LED_LEVEL_DAY_URGENT = 255;  // unacknowledged Warning/Critical

bool ledsReady = false;

bool setupLeds() {
  auto bus = std::make_shared<m5::LedBus_RMT>();
  auto bc = bus->getConfig();
  bc.pin_data = LED_DATA_PIN;
  bus->setConfig(bc);

  auto strip = std::make_shared<m5::LED_Strip_Class>();
  auto sc = strip->getConfig();
  sc.led_count = LED_COUNT;
  sc.byte_per_led = 3;
  strip->setConfig(sc);
  strip->setBus(bus);

  M5.Led.setLedInstance(strip);
  M5.Led.setAutoDisplay(false);
  bool ok = M5.Led.begin();
  M5.Led.setBrightness(255);   // no library scaling -- see above
  Serial.printf("[led] M5GO LEDs %s\n", ok ? "ready" : "FAILED to start");
  return ok;
}

uint8_t ledScale(uint8_t c, uint8_t level) {
  if (c == 0) return 0;
  uint32_t v = (uint32_t)c * level / 255;
  return v == 0 ? 1 : (uint8_t)v;
}

// Sends to the LEDs only when the output actually changes.
void writeLeds(const LedColor &c, uint8_t level, bool on) {
  static int lastR = -1, lastG = -1, lastB = -1;
  uint8_t r = on ? ledScale(c.r, level) : 0;
  uint8_t g = on ? ledScale(c.g, level) : 0;
  uint8_t b = on ? ledScale(c.b, level) : 0;
  if (r == lastR && g == lastG && b == lastB) return;
  lastR = r; lastG = g; lastB = b;
  for (int i = 0; i < LED_COUNT; i++) M5.Led.setColor(i, r, g, b);
  M5.Led.display();
}

// Repeating bursts of `flashes` flashes (200 ms on, 200 ms off), one
// burst every 3 s -- the same 3/2/1 count as the tones.
bool ledBurstOn(unsigned long now, int flashes) {
  unsigned long t = now % 3000;
  return t < (unsigned long)flashes * 400 && (t % 400) < 200;
}

// Two quick flickers every 10 s, like a strike: close lightning.
bool ledFlickerOn(unsigned long now) {
  unsigned long t = now % 10000;
  return t < 80 || (t >= 200 && t < 280);
}

// One flicker every 10 s: lightning 10-20 miles away (v1.1.1).
bool ledSingleFlickerOn(unsigned long now) {
  return now % 10000 < 80;
}

bool onBatteryPower = false;

// Called every loop() pass; cheap, and only touches the LEDs on a change.
// Blocking network calls (at most ~5 s) pause a flash pattern but never
// change what it means.
void handleLeds() {
  if (!ledsReady) return;
  unsigned long now = millis();
  // Settings preview (v1.2): amber at the level just chosen, briefly.
  if ((long)(ledPreviewUntilMillis - now) > 0) {
    writeLeds(LED_AMBER, ledPreviewLevel, true);
    return;
  }
  bool night = isNightTimeNow();
  NwsView v = currentNwsView();
  bool alertShown = (v == NWS_VIEW_ALERT || v == NWS_VIEW_ALERT_STALE);
  const String &lvl = nwsFirstAlertLevel;
  bool redTier = !(lvl == "watch" || lvl == "advisory" || lvl == "informational");  // blank -> red, as on screen
  bool sounding = (lvl == "critical" || lvl == "warning" || lvl == "watch");

  if (alertShown && nwsFirstAlertNeedsAlert && sounding) {
    bool critical = (lvl == "critical");
    int flashes = critical ? 3 : (lvl == "warning" ? 2 : 1);
    uint8_t level = night ? (critical ? ledLevelNightCritical() : ledLevelNight())
                          : (redTier ? LED_LEVEL_DAY_URGENT : ledLevelDay());
    writeLeds(redTier ? LED_RED : LED_AMBER, level, ledBurstOn(now, flashes));
  } else if (onBatteryPower) {
    writeLeds(LED_BLUE, night ? ledLevelNight() : ledLevelDay(), (now % 3000) < 1000);
  } else if (alertShown) {
    writeLeds(redTier ? LED_RED : LED_AMBER, night ? ledLevelNight() : ledLevelDayDim(), true);
  } else if (lightningCloseNow()) {
    // Close (under 10 mi, or within the 30-minute hold): double flicker.
    writeLeds(LED_WHITE, night ? ledLevelNight() : ledLevelDay(), ledFlickerOn(now));
  } else if (currentLightningView() == LIGHTNING_ACTIVE) {
    // 10-20 mi: a single flicker every 10 s.
    writeLeds(LED_WHITE, night ? ledLevelNight() : ledLevelDay(), ledSingleFlickerOn(now));
  } else if (v == NWS_VIEW_UNKNOWN || currentLightningView() == LIGHTNING_UNKNOWN) {
    writeLeds(LED_BLUE, night ? ledLevelNight() : ledLevelDayDim(), true);
  } else {
    writeLeds(LED_BLUE, 0, false);   // all clear: off
  }
}

// ---- Power state ----
// USB (wall) power is judged from the power chip's VBUS reading, sampled
// once a second; two readings in a row are needed to change state, so a
// single odd reading can't trigger anything. -1 (reading not supported)
// counts as wall power, so it can never raise a false alarm.
const int16_t WALL_POWER_MV = 4000;
const unsigned long POWER_SAMPLE_MS = 1000;
const int POWER_CHANGE_SAMPLES = 2;
// PROVISIONAL until the battery run-down test: shut down cleanly when the
// battery stays below this for LOW_BATTERY_SAMPLES readings in a row
// (10 s, so a brief dip while Wi-Fi transmits or a tone plays doesn't
// count). Judged by voltage, not the chip's %, which read 100% on battery
// at 4147 mV in the diagnostic.
const int16_t LOW_BATTERY_MV = 3500;
const int LOW_BATTERY_SAMPLES = 10;
const int POWER_CHIME_CHANNEL = 1;   // separate from the alarm's channel 0

unsigned long batteryStartMillis = 0;
int16_t batteryMv = -1;
unsigned long lastPowerSampleMillis = 0;
int powerChangeCount = 0;
int lowBatteryCount = 0;
int powerScreenShownMinute = -1;   // redraw the power screen when this changes

bool powerScreenShouldShow() {
  if (!onBatteryPower) return false;
  NwsView v = currentNwsView();
  return !(v == NWS_VIEW_ALERT || v == NWS_VIEW_ALERT_STALE);
}

// The approved Session 15 mockup (A: alerts unavailable; B: network
// still up, alerts still arriving). B only when the NWS check is current.
void drawPowerLostPage() {
  const uint16_t BLUE_BG = M5.Display.color565(0x0a, 0x24, 0x50);
  const uint16_t BLUE_HEAD = M5.Display.color565(0xb3, 0xd1, 0xff);
  M5.Display.fillScreen(COLOR_BG);
  M5.Display.fillRect(0, 0, 320, 44, BLUE_BG);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(BLUE_HEAD, BLUE_BG);
  M5.Display.setTextSize(3);
  M5.Display.drawString("POWER LOST", 160, 22);

  unsigned long mins = (millis() - batteryStartMillis) / 60000;
  powerScreenShownMinute = (int)mins;
  char buf[24];
  snprintf(buf, sizeof(buf), "On battery %lu:%02lu", mins / 60, mins % 60);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(COLOR_TEXT_PRIMARY, COLOR_BG);
  M5.Display.drawString(buf, 16, 54);
  if (batteryMv > 0) {
    snprintf(buf, sizeof(buf), "%.2f V", batteryMv / 1000.0f);
    M5.Display.setTextDatum(top_right);
    M5.Display.setTextColor(COLOR_TEXT_SECONDARY, COLOR_BG);
    M5.Display.drawString(buf, 304, 54);
    M5.Display.setTextDatum(top_left);
  }
  M5.Display.drawFastHLine(16, 78, 288, COLOR_SEPARATOR);

  bool alertsArriving = (currentNwsView() == NWS_VIEW_CLEAR);
  M5.Display.setTextColor(COLOR_TEXT_PRIMARY, COLOR_BG);
  M5.Display.drawString(alertsArriving ? "Alerts still arriving," : "Weather alerts are", 16, 88);
  M5.Display.drawString(alertsArriving ? "but may stop." : "UNAVAILABLE.", 16, 110);
  M5.Display.setTextColor(BLUE_HEAD, COLOR_BG);
  M5.Display.drawString(alertsArriving ? "Keep NOAA radio or" : "Use NOAA radio or", 16, 140);
  M5.Display.drawString(alertsArriving ? "phone alerts handy." : "phone alerts.", 16, 162);
  M5.Display.drawFastHLine(16, 188, 288, COLOR_SEPARATOR);

  M5.Display.setTextSize(1);
  const char *status;
  uint16_t statusColor = COLOR_WARNING_TEXT_DETAIL;
  if (!backendDataFresh()) status = "Server unreachable";
  else if (!nwsAvailable) status = "NWS unavailable";
  else { status = "Server connected"; statusColor = COLOR_NWS_CLEAR_TEXT; }
  M5.Display.setTextColor(statusColor, COLOR_BG);
  M5.Display.drawString(status, 16, 196);
  M5.Display.setTextColor(COLOR_LABEL, COLOR_BG);
  M5.Display.drawString("Shuts down safely when the battery is low.", 16, 210);
  M5.Display.drawString("Restarts by itself when power returns.", 16, 222);
}

// One short falling two-note chime. Plays even when muted and in quiet
// hours (Dan, Session 15): an outage means warnings may have stopped
// reaching the device, for any reason. Non-blocking (queued notes).
void playPowerLossChime() {
  M5.Speaker.setVolume(alarmVolume);
  M5.Speaker.tone(1500, 180, POWER_CHIME_CHANNEL, true);
  M5.Speaker.tone(1100, 300, POWER_CHIME_CHANNEL, false);
}

// Screen C, the 5 blue flashes, then a clean power-off. Blocking (~3 s)
// on purpose: nothing else matters at this point. Wall power returning
// during it cancels the shutdown.
void shutdownForLowBattery() {
  Serial.printf("[power] battery low (%d mV) -- shutting down\n", batteryMv);
  const uint16_t BLUE_HEAD = M5.Display.color565(0xb3, 0xd1, 0xff);
  M5.Display.fillScreen(COLOR_BG);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(BLUE_HEAD, COLOR_BG);
  M5.Display.setTextSize(3);
  M5.Display.drawString("BATTERY LOW", 160, 78);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(COLOR_TEXT_PRIMARY, COLOR_BG);
  M5.Display.drawString("Shutting down", 160, 116);
  M5.Display.setTextColor(COLOR_TEXT_SECONDARY, COLOR_BG);
  M5.Display.drawString("Restarts by itself when", 160, 162);
  M5.Display.drawString("power returns.", 160, 184);
  M5.Display.setTextDatum(top_left);

  uint8_t level = isNightTimeNow() ? ledLevelNight() : ledLevelDay();
  for (int i = 0; i < 5; i++) {
    writeLeds(LED_BLUE, level, true);
    delay(250);
    writeLeds(LED_BLUE, level, false);
    delay(250);
  }
  delay(500);

  int16_t vbus = M5.Power.getVBUSVoltage();
  if (vbus >= WALL_POWER_MV) {
    Serial.println("[power] wall power returned during shutdown -- cancelled");
    lowBatteryCount = 0;
    renderPage(currentPage);
    return;
  }
  Serial.println("[power] powering off");
  delay(100);   // let the serial line flush
  M5.Power.powerOff();
}

// ---- Close-lightning chime (v1.1.1) ----
// One short RISING chirp when lightning first comes inside 10 miles --
// distinct from the NWS tones (1000/1400/1800 Hz patterns) and from the
// falling power-loss chime. Once per storm: it can sound again only after
// the close state has fully ended (30 minutes without a close strike).
// Follows mute and quiet hours (Dan): the double flicker still shows.
// Never on the first data after a restart, and an "unknown" spell
// (server or Tempest down) changes nothing, so a reboot or a server
// restart in mid-storm can't make it chime again.
int lightningCloseLastState = -1;   // -1 = not yet established

void playCloseLightningChime() {
  M5.Speaker.setVolume(alarmVolume);
  M5.Speaker.tone(1200, 120, POWER_CHIME_CHANNEL, true);
  M5.Speaker.tone(1600, 180, POWER_CHIME_CHANNEL, false);
}

void handleLightningChime() {
  if (currentLightningView() == LIGHTNING_UNKNOWN) return;
  int closeNow = lightningCloseNow() ? 1 : 0;
  if (lightningCloseLastState == 0 && closeNow == 1) {
    if (audioMuted || isNightTimeNow()) {
      Serial.println("[lightning] close lightning -- chime skipped (muted or quiet hours)");
    } else {
      Serial.println("[lightning] close lightning -- chime");
      playCloseLightningChime();
    }
  }
  lightningCloseLastState = closeNow;
}

void enterBatteryPower(bool atBoot) {
  onBatteryPower = true;
  batteryStartMillis = millis();
  lowBatteryCount = 0;
  Serial.printf("[power] WALL POWER LOST%s -- on battery (%d mV)\n",
                atBoot ? " (at boot)" : "", batteryMv);
  if (atBoot) return;   // setup() draws the first screen itself
  playPowerLossChime();
  // The power screen (or an alert, which outranks it) takes over.
  currentPage = PAGE_NOW;
  lastActivityTime = millis();
  renderPage(currentPage);
}

void leaveBatteryPower() {
  onBatteryPower = false;
  Serial.printf("[power] WALL POWER BACK after %lu s on battery\n",
                (millis() - batteryStartMillis) / 1000);
  // Give the router the full 10 minutes from now before the Wi-Fi
  // last-resort reboot can fire (see handleWifiAndNtpReconnect()).
  if (!wifiWasConnected) wifiDownSinceMillis = millis();
  if (currentPage == PAGE_NOW) renderPage(currentPage);
}

// Called every loop() pass; samples once a second.
void handlePower() {
  unsigned long now = millis();
  if (now - lastPowerSampleMillis < POWER_SAMPLE_MS) return;
  lastPowerSampleMillis = now;

  int16_t vbus = M5.Power.getVBUSVoltage();
  batteryMv = M5.Power.getBatteryVoltage();
  bool wallNow = (vbus < 0) || (vbus >= WALL_POWER_MV);

  if (wallNow == onBatteryPower) {          // reading disagrees with the state
    if (++powerChangeCount >= POWER_CHANGE_SAMPLES) {
      powerChangeCount = 0;
      if (wallNow) leaveBatteryPower(); else enterBatteryPower(false);
    }
  } else {
    powerChangeCount = 0;
  }

  if (!onBatteryPower) { lowBatteryCount = 0; return; }

  if (batteryMv > 0 && batteryMv < LOW_BATTERY_MV) {
    if (++lowBatteryCount >= LOW_BATTERY_SAMPLES) shutdownForLowBattery();
  } else {
    lowBatteryCount = 0;
  }

  // Keep the on-battery time current (it shows whole minutes).
  if (currentPage == PAGE_NOW && powerScreenShouldShow() &&
      (int)((now - batteryStartMillis) / 60000) != powerScreenShownMinute) {
    renderPage(currentPage);
  }
}

// ---- Settings saved across restarts (v1.2) ----
// Stored in the ESP32's NVS flash through Preferences, namespace
// "settings". Written only when leaving a Settings page or Settings
// itself, and only the values that changed, to keep flash writes low.
// Mute is deliberately NOT saved: a mute must never outlast a restart or
// a power cut. Every value is range-checked on load, and anything
// missing or out of range keeps its built-in default.
Preferences settingsStore;

struct SavedSettings {
  uint8_t volume, dayBr, nightBr, ledDay, ledNight;
  uint8_t startH, startM, endH, endM;
};
SavedSettings lastSaved;

SavedSettings currentSettings() {
  return {alarmVolume, DAY_BRIGHTNESS, NIGHT_BRIGHTNESS, ledDayLevel, ledNightLevel,
          (uint8_t)NIGHT_START_HOUR, (uint8_t)NIGHT_START_MINUTE,
          (uint8_t)NIGHT_END_HOUR, (uint8_t)NIGHT_END_MINUTE};
}

bool validTime(int h, int m) { return h >= 0 && h < 24 && m >= 0 && m < 60 && m % 15 == 0; }

void loadSettings() {
  settingsStore.begin("settings", true);   // read-only
  int vol = settingsStore.getUChar("volume", alarmVolume);
  int dayBr = settingsStore.getUChar("dayBr", DAY_BRIGHTNESS);
  int nightBr = settingsStore.getUChar("nightBr", NIGHT_BRIGHTNESS);
  int ledDay = settingsStore.getUChar("ledDay", ledDayLevel);
  int ledNight = settingsStore.getUChar("ledNight", ledNightLevel);
  int sh = settingsStore.getUChar("startH", NIGHT_START_HOUR);
  int sm = settingsStore.getUChar("startM", NIGHT_START_MINUTE);
  int eh = settingsStore.getUChar("endH", NIGHT_END_HOUR);
  int em = settingsStore.getUChar("endM", NIGHT_END_MINUTE);
  settingsStore.end();

  alarmVolume = (uint8_t)vol;                       // any 0-255 is valid
  if (dayBr >= 1) DAY_BRIGHTNESS = (uint8_t)dayBr;   // 0 would turn the backlight off
  if (nightBr >= 1) NIGHT_BRIGHTNESS = (uint8_t)nightBr;
  if (isLedStep((uint8_t)ledDay, LED_DAY_STEPS)) ledDayLevel = (uint8_t)ledDay;
  if (isLedStep((uint8_t)ledNight, LED_NIGHT_STEPS)) ledNightLevel = (uint8_t)ledNight;
  if (validTime(sh, sm)) { NIGHT_START_HOUR = sh; NIGHT_START_MINUTE = sm; }
  if (validTime(eh, em)) { NIGHT_END_HOUR = eh; NIGHT_END_MINUTE = em; }
  lastSaved = currentSettings();
  Serial.printf("[settings] loaded: volume %d, screen %d/%d, LEDs %d/%d, night %02d:%02d-%02d:%02d\n",
                alarmVolume, DAY_BRIGHTNESS, NIGHT_BRIGHTNESS, ledDayLevel, ledNightLevel,
                NIGHT_START_HOUR, NIGHT_START_MINUTE, NIGHT_END_HOUR, NIGHT_END_MINUTE);
}

void saveSettingsIfChanged() {
  SavedSettings now = currentSettings();
  if (memcmp(&now, &lastSaved, sizeof(now)) == 0) return;
  settingsStore.begin("settings", false);
  if (now.volume != lastSaved.volume) settingsStore.putUChar("volume", now.volume);
  if (now.dayBr != lastSaved.dayBr) settingsStore.putUChar("dayBr", now.dayBr);
  if (now.nightBr != lastSaved.nightBr) settingsStore.putUChar("nightBr", now.nightBr);
  if (now.ledDay != lastSaved.ledDay) settingsStore.putUChar("ledDay", now.ledDay);
  if (now.ledNight != lastSaved.ledNight) settingsStore.putUChar("ledNight", now.ledNight);
  if (now.startH != lastSaved.startH) settingsStore.putUChar("startH", now.startH);
  if (now.startM != lastSaved.startM) settingsStore.putUChar("startM", now.startM);
  if (now.endH != lastSaved.endH) settingsStore.putUChar("endH", now.endH);
  if (now.endM != lastSaved.endM) settingsStore.putUChar("endM", now.endM);
  settingsStore.end();
  lastSaved = now;
  Serial.println("[settings] saved");
}

void setupPower() {
  ledsReady = setupLeds();
  // Status is unknown until the first fetch: say so from the first moment.
  if (ledsReady) writeLeds(LED_BLUE, ledLevelNight(), true);
  int16_t vbus = M5.Power.getVBUSVoltage();
  batteryMv = M5.Power.getBatteryVoltage();
  Serial.printf("[power] at boot: USB %d mV, battery %d mV\n", vbus, batteryMv);
  if (vbus >= 0 && vbus < WALL_POWER_MV) enterBatteryPower(true);
  lastPowerSampleMillis = millis();
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  Serial.begin(115200);
  delay(200);
  loadSettings();  // before the LEDs, speaker and brightness use them
  applyAlarmVolumeImmediately();
  setupPower();   // LEDs show "status unknown" through the blocking Wi-Fi/NTP start
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
    // Night: a tap on the dimmed screen only wakes it (Session 13).
    // Marking the touch "handled" blocks both the tap and the long-press
    // branches below for this one touch.
    if (nightWakeOnTouch()) {
      longPressHandled = true;
      lastActivityTime = millis();
    }
    Serial.printf("[touch] press x=%d y=%d t=%lu page=%d\n",
                  (int)touch.x, (int)touch.y, millis(), (int)currentPage);
  }

  if (touch.isPressed() && !longPressHandled &&
      (millis() - touchStartTime >= LONG_PRESS_MS)) {
    longPressHandled = true;
    lastActivityTime = millis();
    if (currentPage != PAGE_SETTINGS) {
      currentPage = PAGE_SETTINGS;
      settingsSubPage = SV_MENU;  // a long press always opens the menu
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

    // "+N more >" on the NWS Alerts page: a tap anywhere in the coloured
    // header block opens Alert History, where every alert can be read.
    bool tappedMoreAlerts = (currentPage == PAGE_NWS_ALERTS &&
                             nwsMoreCount() > 0 &&
                             (tapNwsView == NWS_VIEW_ALERT || tapNwsView == NWS_VIEW_ALERT_STALE) &&
                             touch.x >= NWS_HEADER_X && touch.x < NWS_HEADER_X + NWS_HEADER_W &&
                             touch.y >= NWS_HEADER_Y && touch.y < NWS_HEADER_Y + NWS_HEADER_H);

    if (tappedAckPrompt) {
      showAckFeedback(ackAlert(nwsFirstAlertId));
      renderPage(currentPage);  // shows ACKNOWLEDGED / NOT SENT in the bar
    } else if (tappedMoreAlerts) {
      Serial.println("Tap on '+N more' -- opening Alert History");
      currentPage = PAGE_SETTINGS;
      settingsSubPage = SETTINGS_PAGE_HISTORY;
      openAlertHistory();
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
  handleMuteExpiry();
  handleAckFeedbackExpiry();
  handleAlarm();
  handlePower();
  handleLeds();
  handleLightningChime();

  // Idle auto-return to Now
  if (currentPage != PAGE_NOW &&
      (millis() - lastActivityTime >= IDLE_RETURN_MS)) {
    Serial.println("Idle timeout -- returning to Now");
    lastActivityTime = millis();
    if (currentPage == PAGE_SETTINGS) {
      closeSettings();   // saves anything changed (v1.2)
    } else {
      currentPage = PAGE_NOW;
      renderPage(currentPage);
    }
  }

  // LED preview finished (v1.2): take the "Showing amber" box off.
  static bool previewWasOn = false;
  bool previewOn = (long)(ledPreviewUntilMillis - millis()) > 0;
  if (previewWasOn && !previewOn && currentPage == PAGE_SETTINGS && settingsSubPage == SV_LEDS) {
    renderPage(currentPage);
  }
  previewWasOn = previewOn;

  // Periodic re-fetch of real conditions. Only redraws if Now is
  // currently being shown -- doesn't interrupt whatever detail page
  // someone might be reading, even though the underlying data still
  // updates regardless, ready for whenever they cycle back to Now.
  if (millis() - lastFetchAttemptMillis >= FETCH_INTERVAL_MS) {
    lastFetchAttemptMillis = millis();
    fetchConditions();
    if (currentPage == PAGE_NOW) {
      renderPage(PAGE_NOW);
    } else if (currentPage == PAGE_SETTINGS && settingsSubPage == SV_TEST) {
      fetchSimulationStatus();   // e.g. an acknowledgement advanced the test
      renderPage(currentPage);
    }
  }

  delay(10);
}
