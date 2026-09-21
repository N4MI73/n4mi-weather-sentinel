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

enum Page {
  PAGE_NOW = 0,
  PAGE_WIND_RAIN,
  PAGE_LIGHTNING,
  PAGE_NWS_ALERTS,
  PAGE_STATUS,
  PAGE_SETTINGS
};
static const int NUM_CYCLE_PAGES = 5;  // Settings excluded from the cycle

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
    COLOR_LABEL, COLOR_STATUS_BAD;

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
                     const char *windStr, const char *gustStr, const char *rainStr) {
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(COLOR_TEXT_PRIMARY, COLOR_BG);
  M5.Display.setTextSize(4);
  M5.Display.drawString(tempStr, 16, 55);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(COLOR_TEXT_SECONDARY, COLOR_BG);
  M5.Display.drawString(feelsStr, 150, 58);
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
    if (tempestFeelsLikeValid) {
      snprintf(feelsStr, sizeof(feelsStr), "Feels %.0fF", tempestFeelsLikeF);
    } else {
      feelsStr[0] = '\0';
    }
    snprintf(humidStr, sizeof(humidStr), "%.0f%% humidity", tempestHumidityPct);
    snprintf(windStr, sizeof(windStr), "Wind %.0f mph", tempestWindMph);
    snprintf(gustStr, sizeof(gustStr), "Gust %.0f mph", tempestGustMph);
    // What is happening NOW: the rain RATE in Tempest's own words, not an
    // accumulation. Numeric detail lives on the Wind & Rain page.
    snprintf(rainStr, sizeof(rainStr), "Rain: %s", rainLevelLabel(tempestRainRateLevel));
    drawConditions(tempStr, feelsStr, humidStr, windStr, gustStr, rainStr);
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
  } else if (currentLightningView() == LIGHTNING_ACTIVE) {
    bg = COLOR_LIGHTNING_BG;
    textColor = COLOR_LIGHTNING_TEXT;
    msg = (lightningLevel == "frequent") ? "Frequent lightning nearby" : "Lightning nearby";
  } else if (nwsView == NWS_VIEW_UNKNOWN) {
    bg = COLOR_BG;
    textColor = COLOR_TEXT_DIM;
    msg = "NWS status unknown";
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

void drawSettingsPage() {
  M5.Display.fillScreen(COLOR_BG);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(COLOR_TEXT_DIM, COLOR_BG);
  M5.Display.setTextSize(1);
  M5.Display.drawString("placeholder -- layout pending", 16, 8);
  M5.Display.setTextColor(COLOR_TEXT_PRIMARY, COLOR_BG);
  M5.Display.setTextSize(3);
  M5.Display.drawString("Settings", 16, 90);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(COLOR_TEXT_SECONDARY, COLOR_BG);
  M5.Display.drawString("(tap to return)", 16, 140);
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

void renderPage(Page p) {
  unsigned long t0 = millis();
  renderPageInner(p);
  unsigned long dt = millis() - t0;
  if (dt > DIAG_SLOW_RENDER_MS) {
    Serial.printf("[diag] slow render: page %d took %lu ms\n", (int)p, dt);
  }
}

// Phase 2a: Wi-Fi connectivity only -- no HTTP fetch, no screen changes
// yet. Uses the wifi_credentials.h stopgap (real captive portal comes
// later, per project instructions). Success/failure reported to serial
// only for this step; the display keeps showing its existing
// static/dummy content exactly as before.
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;

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
    nwsFirstAlertAcknowledged = alerts[0]["acknowledged"];
    nwsFirstAlertNeedsAlert = alerts[0]["needs_alert"];
  } else {
    nwsFirstAlertEvent = "";
    nwsFirstAlertExpires = "";
    nwsFirstAlertInstruction = "";
    nwsFirstAlertId = "";
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
    } else {
      if (currentPage == PAGE_SETTINGS) {
        currentPage = PAGE_NOW;
      } else {
        currentPage = (Page)((currentPage + 1) % NUM_CYCLE_PAGES);
      }
      Serial.printf("Tap -- now on page %d\n", (int)currentPage);
      renderPage(currentPage);
    }
  }

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
