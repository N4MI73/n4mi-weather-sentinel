// Weather Sentinel — Navigation demo (static data, no server yet).
//
// 5 pages in a tap-to-advance cycle: Now, Wind & Rain, Lightning,
// NWS Alerts, Status. Settings is reached only by long-press (~1s) from
// any page, NOT part of the tap cycle. Auto-returns to Now after 45s
// idle on any other page. A persistent, condensed alert strip is docked
// at the bottom of every page except Now (which keeps its own existing
// full-detail footer/banner, already approved).
//
// The four secondary pages (Wind & Rain, Lightning, NWS Alerts, Status)
// are intentionally placeholders here -- per project convention, a new
// screen's real layout needs its own SVG mockup and approval before full
// implementation. This file validates the navigation MECHANISM only:
// tap-to-advance, long-press-for-settings, idle timeout, persistent
// alert awareness. Content design for each page is separate follow-up
// work.

#include <M5Unified.h>

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
    COLOR_WARNING_BG, COLOR_WARNING_TEXT_HEADLINE, COLOR_WARNING_TEXT_DETAIL;

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

// ---- Secondary pages: navigation placeholders only ----
// Real content/layout for each of these needs its own mockup + approval
// before full implementation -- this just proves tap-cycling reaches them.

void drawPlaceholderPage(const char *title) {
  M5.Display.fillScreen(COLOR_BG);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(COLOR_TEXT_DIM, COLOR_BG);
  M5.Display.setTextSize(1);
  M5.Display.drawString("placeholder -- layout pending", 16, 8);
  M5.Display.setTextColor(COLOR_TEXT_PRIMARY, COLOR_BG);
  M5.Display.setTextSize(3);
  M5.Display.drawString(title, 16, 90);
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
      drawPlaceholderPage("Wind & Rain");
      break;
    case PAGE_LIGHTNING:
      drawPlaceholderPage("Lightning");
      break;
    case PAGE_NWS_ALERTS:
      drawPlaceholderPage("NWS Alerts");
      break;
    case PAGE_STATUS:
      drawPlaceholderPage("Status");
      break;
    case PAGE_SETTINGS:
      drawSettingsPage();
      return;  // Settings doesn't carry the alert strip either
  }
  drawPersistentStrip();
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("=== Weather Sentinel -- Navigation demo ===");
  Serial.println("Tap to advance page. Long-press (~1s) for Settings.");

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
