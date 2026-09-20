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
#include "wifi_credentials.h"

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

// Simulated "current condition" for this demo -- in the real system this
// comes from the server (Phase 1+). Fixed here so navigation can be
// exercised against a stable alert state. Change 0/1/2 to try the others.
static int simulatedCondition = 1;  // 0=clear, 1=lightning, 2=warning

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

// ---- Now screen (unchanged from the approved design) ----

void drawCommonHeader(const char *timeStr, const char *ageStr) {
  M5.Display.fillScreen(COLOR_BG);
  M5.Display.setTextDatum(top_right);
  M5.Display.setTextColor(COLOR_TEXT_DIM, COLOR_BG);
  M5.Display.setTextSize(1);
  M5.Display.drawString(ageStr, 308, 8);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(COLOR_TEXT_PRIMARY, COLOR_BG);
  M5.Display.setTextSize(2);
  M5.Display.drawString(timeStr, 16, 20);
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

void drawNwsFooterClear() {
  M5.Display.drawFastHLine(0, 194, 320, COLOR_SEPARATOR);
  M5.Display.fillCircle(20, 212, 4, COLOR_NWS_CLEAR_DOT);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(COLOR_NWS_CLEAR_TEXT, COLOR_BG);
  M5.Display.setTextSize(2);
  M5.Display.drawString("NO ACTIVE ALERTS", 34, 204);
}

void drawLightningBanner() {
  M5.Display.fillRoundRect(12, 150, 296, 34, 6, COLOR_LIGHTNING_BG);
  M5.Display.drawRoundRect(12, 150, 296, 34, 6, COLOR_LIGHTNING_BORDER);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(COLOR_LIGHTNING_TEXT, COLOR_LIGHTNING_BG);
  M5.Display.setTextSize(2);
  M5.Display.drawString("Lightning nearby", 26, 158);
}

void drawNwsWarningFooter() {
  M5.Display.fillRect(0, 196, 320, 44, COLOR_WARNING_BG);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(COLOR_WARNING_TEXT_HEADLINE, COLOR_WARNING_BG);
  M5.Display.setTextSize(2);
  M5.Display.drawString("SEVERE T-STORM WARNING", 16, 202);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(COLOR_WARNING_TEXT_DETAIL, COLOR_WARNING_BG);
  M5.Display.drawString("Until 11:45 PM -- tap for details", 16, 224);
}

void drawNowPage() {
  switch (simulatedCondition) {
    case 0:
      drawCommonHeader("10:47 PM", "updated 2m ago");
      drawConditions("73F", "feels 74F", "78% humidity", "Wind 4 mph", "Gust 9 mph", "Rain 0.00\"");
      drawNwsFooterClear();
      break;
    case 1:
      drawCommonHeader("10:52 PM", "updated 1m ago");
      drawConditions("72F", "feels 73F", "81% humidity", "Wind 6 mph", "Gust 14 mph", "Rain 0.02\"");
      drawLightningBanner();
      drawNwsFooterClear();
      break;
    case 2:
      drawCommonHeader("11:03 PM", "updated 30s ago");
      drawConditions("70F", "feels 71F", "85% humidity", "Wind 18 mph", "Gust 41 mph", "Rain 0.31\"");
      drawNwsWarningFooter();
      break;
  }
}

// ---- Persistent slim alert strip (every page except Now and Settings) ----
// A condensed one-line summary of the same condition, so an active alert
// is never lost while browsing -- without spending the vertical space
// Now's dedicated footer/banner uses, which secondary pages need for
// their own content instead.

void drawPersistentStrip() {
  const int stripY = 214;
  const int stripH = 26;
  uint16_t bg, textColor;
  const char *msg;

  switch (simulatedCondition) {
    case 1:
      bg = COLOR_LIGHTNING_BG;
      textColor = COLOR_LIGHTNING_TEXT;
      msg = "Lightning nearby";
      break;
    case 2:
      bg = COLOR_WARNING_BG;
      textColor = COLOR_WARNING_TEXT_HEADLINE;
      msg = "SEVERE T-STORM WARNING";
      break;
    default:
      bg = COLOR_BG;
      textColor = COLOR_NWS_CLEAR_TEXT;
      msg = "No active alerts";
      break;
  }

  M5.Display.fillRect(0, stripY, 320, stripH, bg);
  M5.Display.drawFastHLine(0, stripY, 320, COLOR_SEPARATOR);
  M5.Display.setTextDatum(middle_left);
  M5.Display.setTextColor(textColor, bg);
  M5.Display.setTextSize(1);
  M5.Display.drawString(msg, 12, stripY + stripH / 2);
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

  // Row 1: Wind + Gust
  M5.Display.setTextColor(COLOR_LABEL, COLOR_BG);
  M5.Display.setTextSize(1);
  M5.Display.drawString("WIND (NW)", 16, 48);
  M5.Display.drawString("GUST", 170, 48);

  M5.Display.setTextColor(COLOR_TEXT_PRIMARY, COLOR_BG);
  M5.Display.setTextSize(3);
  M5.Display.drawString("6 mph", 16, 64);
  M5.Display.drawString("14 mph", 170, 64);

  M5.Display.drawFastHLine(16, 100, 288, COLOR_SEPARATOR);

  // Row 2: Rain today + Rain rate
  M5.Display.setTextColor(COLOR_LABEL, COLOR_BG);
  M5.Display.setTextSize(1);
  M5.Display.drawString("RAIN TODAY", 16, 110);
  M5.Display.drawString("RAIN RATE", 170, 110);

  M5.Display.setTextColor(COLOR_TEXT_PRIMARY, COLOR_BG);
  M5.Display.setTextSize(3);
  M5.Display.drawString("0.02\"", 16, 126);
  M5.Display.drawString("0.00\"/hr", 170, 126);

  M5.Display.drawFastHLine(16, 162, 288, COLOR_SEPARATOR);

  // Row 3: Pressure + trend
  M5.Display.setTextColor(COLOR_LABEL, COLOR_BG);
  M5.Display.setTextSize(1);
  M5.Display.drawString("PRESSURE", 16, 172);

  M5.Display.setTextColor(COLOR_TEXT_SECONDARY, COLOR_BG);
  M5.Display.setTextSize(2);
  M5.Display.drawString("29.91 inHg", 16, 188);
  M5.Display.setTextColor(COLOR_NWS_CLEAR_TEXT, COLOR_BG);
  M5.Display.drawString("Falling", 190, 188);
}

void drawLightningPage() {
  drawSecondaryHeader("Lightning", PAGE_LIGHTNING);

  // Status badge -- same amber palette as the Now-screen banner.
  // Fixed height (50px) regardless of which classification text is
  // active, so the rest of the page doesn't shift between states.
  // "Frequent lightning nearby" doesn't fit on one line at size 2
  // (confirmed on real hardware -- it ran past the box edge), so long
  // text splits onto two lines; short text ("Lightning nearby") just
  // uses the first line and leaves the second blank.
  M5.Display.fillRoundRect(16, 48, 288, 50, 6, COLOR_LIGHTNING_BG);
  M5.Display.drawRoundRect(16, 48, 288, 50, 6, COLOR_LIGHTNING_BORDER);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(COLOR_LIGHTNING_TEXT, COLOR_LIGHTNING_BG);
  M5.Display.setTextSize(2);
  M5.Display.drawString("Frequent lightning", 26, 56);
  M5.Display.drawString("nearby", 26, 76);

  // Closest recent strike
  M5.Display.setTextColor(COLOR_LABEL, COLOR_BG);
  M5.Display.setTextSize(1);
  M5.Display.drawString("CLOSEST RECENT STRIKE", 16, 108);

  M5.Display.setTextColor(COLOR_TEXT_PRIMARY, COLOR_BG);
  M5.Display.setTextSize(3);
  M5.Display.drawString("2.3 mi", 16, 124);
  M5.Display.setTextColor(COLOR_TEXT_SECONDARY, COLOR_BG);
  M5.Display.setTextSize(2);
  M5.Display.drawString("40 sec ago", 130, 130);

  M5.Display.drawFastHLine(16, 162, 288, COLOR_SEPARATOR);

  // Activity count in filtered window
  M5.Display.setTextColor(COLOR_LABEL, COLOR_BG);
  M5.Display.setTextSize(1);
  M5.Display.drawString("ACTIVITY (LAST 10 MIN, WITHIN 10 MI)", 16, 172);

  M5.Display.setTextColor(COLOR_TEXT_PRIMARY, COLOR_BG);
  M5.Display.setTextSize(2);
  M5.Display.drawString("6 strikes", 16, 188);
}

// NWS instruction text: word-wrap + truncation.
//
// MAX_CHARS_PER_LINE is a HARDWARE-VERIFIED safe budget at text size 2
// in the 288px-wide instruction column -- not derived from a font-metrics
// API (LovyanGFX/M5GFX's own pixel-width measurement capability wasn't
// confirmed available, so this isn't built on that). Instead it's based
// on real evidence: the original 6 demo lines Dan already confirmed fit
// cleanly on real hardware had a longest line of 44 characters. 42 is
// used here as a small safety margin under that confirmed-working value.
const int NWS_MAX_CHARS_PER_LINE = 42;
const int NWS_MAX_INSTRUCTION_LINES = 6;

// Word-wraps `text` into `outLines` (capacity `maxLines`), breaking on
// word boundaries where possible. If the text is longer than maxLines
// can hold, the last line is trimmed and " ..." appended as a
// truncation indicator, per the agreed approach (truncate rather than
// scroll/paginate, to avoid colliding with tap-to-advance navigation).
// Returns the number of lines actually used.
int wrapInstructionText(const String &text, String outLines[], int maxLines) {
  int lineCount = 0;
  int pos = 0;
  int len = text.length();

  while (pos < len && lineCount < maxLines) {
    int remaining = len - pos;
    int take = min(remaining, NWS_MAX_CHARS_PER_LINE);

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

  if (pos < len && lineCount > 0) {
    String &lastLine = outLines[lineCount - 1];
    int maxLastLineLen = NWS_MAX_CHARS_PER_LINE - 4;
    if ((int)lastLine.length() > maxLastLineLen) {
      lastLine = lastLine.substring(0, maxLastLineLen);
    }
    lastLine += " ...";
  }

  return lineCount;
}

void drawNwsAlertsPage() {
  drawSecondaryHeader("NWS Alerts", PAGE_NWS_ALERTS);

  // Consolidated event + until block (v2 layout -- headline field
  // deliberately dropped in favor of more room for instruction text;
  // see project brief for the reasoning Dan approved).
  M5.Display.fillRect(16, 48, 288, 34, COLOR_WARNING_BG);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(COLOR_WARNING_TEXT_HEADLINE, COLOR_WARNING_BG);
  M5.Display.setTextSize(2);
  M5.Display.drawString("SEVERE T-STORM WARNING", 24, 54);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(COLOR_WARNING_TEXT_DETAIL, COLOR_WARNING_BG);
  M5.Display.drawString("Until 11:45 PM", 24, 72);

  // Instruction: full official text, word-wrapped and truncated with an
  // indicator if it runs longer than the available space -- this now
  // handles real (longer or shorter) NWS instruction text correctly,
  // not just this one hardcoded demo string.
  M5.Display.setTextColor(COLOR_TEXT_DIM, COLOR_BG);
  M5.Display.setTextSize(1);
  M5.Display.drawString("INSTRUCTION", 16, 92);

  String instructionText =
      "Torrential rainfall is occurring with this storm and may lead to "
      "flash flooding. Persons in low-lying areas should move to higher "
      "ground now. Do not walk or drive into flooded areas. Turn around, "
      "don't drown.";

  String wrappedLines[NWS_MAX_INSTRUCTION_LINES];
  int lineCount = wrapInstructionText(instructionText, wrappedLines, NWS_MAX_INSTRUCTION_LINES);

  M5.Display.setTextColor(COLOR_TEXT_SECONDARY, COLOR_BG);
  M5.Display.setTextSize(2);
  int y = 106;
  for (int i = 0; i < lineCount; i++) {
    M5.Display.drawString(wrappedLines[i], 16, y);
    y += 16;
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
    wifiValue = "Connected (" + String(WiFi.RSSI()) + " dBm)";
    wifiDotColor = COLOR_NWS_CLEAR_DOT;
  } else {
    wifiValue = "Disconnected";
    wifiDotColor = COLOR_STATUS_BAD;
  }

  struct StatusRow { const char *label; String value; uint16_t dotColor; };
  StatusRow rows[4] = {
    {"Wi-Fi",      wifiValue,             wifiDotColor},
    {"Backend",    "Reachable",           COLOR_NWS_CLEAR_DOT},
    {"Last sync",  "1 min ago",           COLOR_NWS_CLEAR_DOT},
    {"Time sync",  "OK (NTP)",            COLOR_NWS_CLEAR_DOT},
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
  M5.Display.drawString("3d 4h 12m", 16, y + 16);
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

void renderPage(Page p) {
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

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("=== Weather Sentinel -- Navigation demo ===");
  Serial.println("Tap to advance page. Long-press (~1s) for Settings.");

  connectWiFi();

  initColors();
  lastActivityTime = millis();
  renderPage(currentPage);
}

void loop() {
  M5.update();
  auto touch = M5.Touch.getDetail();

  if (touch.wasPressed()) {
    touchStartTime = millis();
    longPressHandled = false;
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
    if (currentPage == PAGE_SETTINGS) {
      currentPage = PAGE_NOW;
    } else {
      currentPage = (Page)((currentPage + 1) % NUM_CYCLE_PAGES);
    }
    Serial.printf("Tap -- now on page %d\n", (int)currentPage);
    renderPage(currentPage);
  }

  // Idle auto-return to Now
  if (currentPage != PAGE_NOW &&
      (millis() - lastActivityTime >= IDLE_RETURN_MS)) {
    Serial.println("Idle timeout -- returning to Now");
    currentPage = PAGE_NOW;
    lastActivityTime = millis();
    renderPage(currentPage);
  }

  delay(10);
}
