// Weather Sentinel -- LED and power diagnostic (Session 15)
//
// A throwaway bench sketch for the M5Stack CoreS3 SE + M5GO Bottom3. It does
// not use Wi-Fi or the server, and it is NOT the real firmware. It answers
// the hardware questions the LED/battery feature depends on:
//
//   1. How many RGB LEDs are on GPIO5, and where is each index physically?
//      (M5Stack's docs say 10 in total; another source says 10 per side, so
//      this sketch drives 20 and you note which numbers actually light.)
//   2. Do the colours look right (red really red = correct colour order), and
//      which colours stay distinct at night brightness?
//   3. Does the power chip (AXP2101) see the M5GO battery and the loss of
//      USB power?
//   4. How long does the battery last, and at what voltage does it die?
//      (Logged to flash once a minute while on battery; shown at next boot.)
//   5. After a software power-off, does the device start by itself when USB
//      power returns?
//
// Library facts this relies on (checked in M5Unified 0.2.22 source):
//   - CoreS3/SE M5-Bus pin 8 (the M5GO RGB data line) is GPIO5.
//   - m5::LED_Strip_Class + m5::LedBus_RMT drive WS2812/SK6812-type LEDs,
//     colour order GRB by default; M5.Led.setLedInstance() hands them to
//     M5.Led. No extra library is needed.
//   - M5.Led.setBrightness() scales by (b+1)^2/65536, which wipes out the
//     smaller colour channel at low settings (amber turns red). So this
//     sketch leaves that at 255 (no scaling) and scales colours itself.

#include <M5Unified.h>
#include <utility/led/LED_Strip_Class.hpp>
#include <Preferences.h>

// ---- LEDs -----------------------------------------------------------------
const int LED_DATA_PIN = 5;     // M5-Bus pin 8 on the CoreS3/SE
const int LED_TEST_COUNT = 20;  // deliberately more than the expected 10

struct NamedColor { const char *name; uint8_t r, g, b; };
const NamedColor COLORS[] = {
  {"RED",    255,   0,   0},
  {"AMBER",  255,  96,   0},
  {"YELLOW", 255, 180,   0},
  {"GREEN",    0, 255,   0},
  {"CYAN",     0, 255, 255},
  {"BLUE",     0,   0, 255},
  {"PURPLE", 140,   0, 255},
  {"WHITE",  255, 255, 255},
};
const int NUM_COLORS = sizeof(COLORS) / sizeof(COLORS[0]);

// Brightest channel value sent to the LED (out of 255). Night will be near
// the bottom of this list, day somewhere in the middle.
const uint8_t LEVELS[] = {1, 2, 4, 8, 16, 32, 64, 128, 255};
const int NUM_LEVELS = sizeof(LEVELS) / sizeof(LEVELS[0]);
int levelIndex = 3;  // start at 8/255 -- gentle

enum Mode { MODE_OFF, MODE_WALK, MODE_COLOR };
Mode mode = MODE_OFF;
int walkIndex = 0;
int colorIndex = 0;

// Scale one channel to the current level, never letting a channel that is
// part of the colour drop to zero (so amber stays amber as long as possible).
uint8_t scaleChannel(uint8_t c) {
  if (c == 0) return 0;
  uint32_t v = (uint32_t)c * LEVELS[levelIndex] / 255;
  return v == 0 ? 1 : (uint8_t)v;
}

void showLeds() {
  for (int i = 0; i < LED_TEST_COUNT; i++) M5.Led.setColor(i, 0, 0, 0);
  if (mode == MODE_WALK) {
    // Walk in white so the colour order can't hide an LED.
    uint8_t w = scaleChannel(255);
    M5.Led.setColor(walkIndex, w, w, w);
  } else if (mode == MODE_COLOR) {
    const NamedColor &c = COLORS[colorIndex];
    for (int i = 0; i < LED_TEST_COUNT; i++)
      M5.Led.setColor(i, scaleChannel(c.r), scaleChannel(c.g), scaleChannel(c.b));
  }
  M5.Led.display();
}

bool setupLeds() {
  auto bus = std::make_shared<m5::LedBus_RMT>();
  auto bc = bus->getConfig();
  bc.pin_data = LED_DATA_PIN;
  bus->setConfig(bc);

  auto strip = std::make_shared<m5::LED_Strip_Class>();
  auto sc = strip->getConfig();
  sc.led_count = LED_TEST_COUNT;
  sc.byte_per_led = 3;
  strip->setConfig(sc);
  strip->setBus(bus);

  M5.Led.setLedInstance(strip);
  M5.Led.setAutoDisplay(false);
  bool ok = M5.Led.begin();
  M5.Led.setBrightness(255);  // no library scaling; see header comment
  return ok;
}

// ---- Power ----------------------------------------------------------------
const int16_t WALL_POWER_MV = 4000;   // VBUS above this = USB/wall power present

Preferences prefs;
bool onBattery = false;
uint32_t batteryStartMs = 0;
uint32_t lastDrainSaveMs = 0;

// From the previous battery stretch (saved to flash while on battery).
uint32_t lastDrainSec = 0;
int32_t lastDrainMv = 0;
int32_t lastDrainPct = -1;

void saveDrain(uint32_t sec, int32_t mv, int32_t pct) {
  prefs.putUInt("sec", sec);
  prefs.putInt("mv", mv);
  prefs.putInt("pct", pct);
}

const char *chargeText(m5::Power_Class::is_charging_t c) {
  switch (c) {
    case m5::Power_Class::is_charging:    return "CHARGING";
    case m5::Power_Class::is_discharging: return "not charging";
    default:                               return "unknown";
  }
}

// ---- Screen ---------------------------------------------------------------
const int BTN_Y = 192, BTN_H = 48, BTN_W = 80;
const char *BTN_LABELS[4] = {"WALK", "COLOR", "BRIGHT", "OFF"};
bool offArmed = false;
uint32_t offArmedMs = 0;

void drawButtons() {
  M5.Display.setTextSize(2);
  M5.Display.setTextDatum(middle_center);
  for (int i = 0; i < 4; i++) {
    int x = i * BTN_W;
    uint16_t fill = (i == 3 && offArmed) ? 0x8000 : 0x2104;
    M5.Display.fillRect(x + 2, BTN_Y + 2, BTN_W - 4, BTN_H - 4, fill);
    M5.Display.drawRect(x + 2, BTN_Y + 2, BTN_W - 4, BTN_H - 4, 0x7BEF);
    M5.Display.setTextColor(TFT_WHITE, fill);
    M5.Display.drawString(BTN_LABELS[i], x + BTN_W / 2, BTN_Y + BTN_H / 2);
  }
  M5.Display.setTextDatum(top_left);
}

void drawModeArea() {
  M5.Display.fillRect(0, 132, 320, 58, TFT_BLACK);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(3);
  char buf[40];
  if (offArmed) {
    M5.Display.setTextColor(TFT_RED, TFT_BLACK);
    M5.Display.drawString("TAP OFF AGAIN", 4, 134);
  } else if (mode == MODE_WALK) {
    snprintf(buf, sizeof(buf), "LED #%d", walkIndex + 1);
    M5.Display.drawString(buf, 4, 134);
  } else if (mode == MODE_COLOR) {
    M5.Display.drawString(COLORS[colorIndex].name, 4, 134);
  } else {
    M5.Display.drawString("LEDs off", 4, 134);
  }
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(0xBDF7, TFT_BLACK);
  snprintf(buf, sizeof(buf), "LED level %d/255 (%d of %d)",
           LEVELS[levelIndex], levelIndex + 1, NUM_LEVELS);
  M5.Display.drawString(buf, 4, 166);
}

void drawLine(int y, uint16_t color, const char *text) {
  M5.Display.fillRect(0, y, 320, 18, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(color, TFT_BLACK);
  M5.Display.drawString(text, 4, y);
}

void drawPower(int16_t vbus, int16_t batMv, int32_t pct,
               m5::Power_Class::is_charging_t chg) {
  char buf[48];
  bool wall = vbus >= WALL_POWER_MV;
  snprintf(buf, sizeof(buf), "Wall power: %s %dmV", wall ? "YES" : "NO ", vbus);
  drawLine(28, wall ? TFT_GREEN : TFT_RED, buf);

  snprintf(buf, sizeof(buf), "Battery: %dmV  %d%%", batMv, (int)pct);
  drawLine(48, TFT_WHITE, buf);

  snprintf(buf, sizeof(buf), "Charge: %s", chargeText(chg));
  drawLine(68, TFT_WHITE, buf);

  if (onBattery) {
    uint32_t s = (millis() - batteryStartMs) / 1000;
    snprintf(buf, sizeof(buf), "On battery %02u:%02u:%02u",
             (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    drawLine(88, TFT_YELLOW, buf);
  } else {
    drawLine(88, TFT_WHITE, "On battery: no");
  }

  if (lastDrainSec > 0) {
    snprintf(buf, sizeof(buf), "Last: %um on batt, %dmV",
             (unsigned)(lastDrainSec / 60), (int)lastDrainMv);
    drawLine(108, 0xBDF7, buf);
  } else {
    drawLine(108, 0xBDF7, "Last battery run: none");
  }
}

// ---- Arduino --------------------------------------------------------------
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  M5.Display.setBrightness(80);
  M5.Display.fillScreen(TFT_BLACK);

  prefs.begin("diag", false);
  lastDrainSec = prefs.getUInt("sec", 0);
  lastDrainMv = prefs.getInt("mv", 0);
  lastDrainPct = prefs.getInt("pct", -1);

  bool ledOk = setupLeds();
  showLeds();

  char buf[48];
  snprintf(buf, sizeof(buf), "board %d pmic %d led %s",
           (int)M5.getBoard(), (int)M5.Power.getType(), ledOk ? "ok" : "FAIL");
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Display.drawString("LED + POWER TEST", 4, 0);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(0x7BEF, TFT_BLACK);
  M5.Display.drawString(buf, 4, 18);

  drawModeArea();
  drawButtons();

  Serial.printf("[diag] %s\n", buf);
  Serial.printf("[diag] last battery run: %u s, %d mV, %d%%\n",
                (unsigned)lastDrainSec, (int)lastDrainMv, (int)lastDrainPct);
}

void handleTap(int x, int y) {
  if (y < BTN_Y) return;
  int b = x / BTN_W;
  if (b != 3) offArmed = false;
  switch (b) {
    case 0:  // WALK: first tap starts at LED 1, later taps step on
      if (mode == MODE_WALK) walkIndex = (walkIndex + 1) % LED_TEST_COUNT;
      else { mode = MODE_WALK; walkIndex = 0; }
      break;
    case 1:  // COLOR: first tap shows the first colour, later taps step on
      if (mode == MODE_COLOR) colorIndex = (colorIndex + 1) % NUM_COLORS;
      else { mode = MODE_COLOR; colorIndex = 0; }
      break;
    case 2:  // BRIGHT: next level, wrapping
      levelIndex = (levelIndex + 1) % NUM_LEVELS;
      break;
    case 3:  // OFF: needs a second tap within 3 s
      if (offArmed) {
        Serial.println("[diag] powering off");
        for (int i = 0; i < LED_TEST_COUNT; i++) M5.Led.setColor(i, 0, 0, 0);
        M5.Led.display();
        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        M5.Display.setTextSize(2);
        M5.Display.drawString("Powering off...", 4, 100);
        delay(800);
        M5.Power.powerOff();
        return;
      }
      offArmed = true;
      offArmedMs = millis();
      break;
  }
  showLeds();
  drawModeArea();
  drawButtons();
}

uint32_t lastPowerDrawMs = 0;

void loop() {
  M5.update();
  auto t = M5.Touch.getDetail();
  if (t.wasPressed()) handleTap(t.x, t.y);

  if (offArmed && millis() - offArmedMs > 3000) {
    offArmed = false;
    drawModeArea();
    drawButtons();
  }

  uint32_t now = millis();
  if (now - lastPowerDrawMs >= 1000) {
    lastPowerDrawMs = now;
    int16_t vbus = M5.Power.getVBUSVoltage();
    int16_t batMv = M5.Power.getBatteryVoltage();
    int32_t pct = M5.Power.getBatteryLevel();
    auto chg = M5.Power.isCharging();
    bool wall = vbus >= WALL_POWER_MV;

    if (!wall && !onBattery) {          // wall power just went away
      onBattery = true;
      batteryStartMs = now;
      lastDrainSaveMs = now;
      saveDrain(1, batMv, pct);          // start a fresh record
      Serial.println("[diag] WALL POWER LOST");
    } else if (wall && onBattery) {     // wall power came back
      onBattery = false;
      uint32_t s = (now - batteryStartMs) / 1000;
      saveDrain(s, batMv, pct);
      lastDrainSec = s; lastDrainMv = batMv; lastDrainPct = pct;
      Serial.printf("[diag] WALL POWER BACK after %u s\n", (unsigned)s);
    }
    if (onBattery && now - lastDrainSaveMs >= 60000) {
      lastDrainSaveMs = now;
      saveDrain((now - batteryStartMs) / 1000, batMv, pct);
    }

    drawPower(vbus, batMv, pct, chg);
    Serial.printf("[diag] vbus=%dmV bat=%dmV level=%d%% charge=%s\n",
                  vbus, batMv, (int)pct, chargeText(chg));
  }
  delay(10);
}
