/*
  Mohawk LED Controller v1.9.1 - Standalone Arduino/ESP32 Sketch
  ----------------------------------------------------------------
  Board target: ESP32 Dev Module or LoR Core V3 ESP32-compatible target
  LED strip: WS2812B, 18 LEDs, GRB, data pin GPIO21

  Libraries needed in Arduino IDE:
    - FastLED
    - WiFi
    - WebServer
    - Preferences

  Wi-Fi Access Point:
    SSID: MiniBot_LED_UI
    PASS: fifabot123
    URL:  http://192.168.4.1

  Robot state model:
    DISCONNECTED but powered on:
      - Rainbow wash animation
      - Bottom two U LEDs flash red if low battery is enabled

    CONNECTED to controller:
      - Team breath base animation stays visible
      - Team breath can be solid, diagonal, or rotating diagonal around the LED 4/13 midpoint
      - Optional pattern designer preview / pattern idle mode
      - Solenoid charge overlay
      - Solenoid firing overlay
      - Shot spam meter: 5 shots within the short window triggers a red lockout meter that reverses back to team idle color
      - 10 second game countdown overlay

  Main integration calls for the real robot later:

    mohawk_set_connected(true);             // call when controller connects
    mohawk_set_connected(false);            // call when controller disconnects
    mohawk_set_low_battery(true);           // bottom two LEDs flash red when disconnected
    mohawk_start_solenoid_charge();         // call while solenoid is charging/arming
    mohawk_stop_solenoid_charge();          // call if charge/arming ends or is cancelled
    mohawk_trigger_solenoid_animation();    // call exactly when the real solenoid fires; web UI test buttons call this too
    mohawk_start_countdown(10);             // call at game start for a 10 second countdown
    mohawk_reset_shot_meter();              // call when match resets or robot is serviced

  This sketch is standalone so the Mohawk LED system can be tested without the
  full robot firmware. The web UI is intentionally built as a tuning panel so
  you can design patterns, test every effect, save settings, and export a recipe.
*/

#include <Arduino.h>
#include <FastLED.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <math.h>

// ============================================================
// Hardware
// ============================================================

#define COLOR_ORDER GRB
#define CHIPSET WS2812B
#define MOHAWK_PIN 21
#define MOHAWK_LED_COUNT 18
#define LED_UPDATE_HZ 60
#define LED_UPDATE_MS (1000 / LED_UPDATE_HZ)

#define MOHAWK_AP_SSID "MiniBot_LED_UI"
#define MOHAWK_AP_PASSWORD "fifabot123"

// Optional physical solenoid test output.
// Leave at -1 for LED-only testing.
// Set this to the GPIO that drives the solenoid MOSFET if you want the web UI
// "Pulse Solenoid + LEDs" button to physically fire the solenoid for bench testing.
#define SOLENOID_TEST_PIN -1
#define SOLENOID_TEST_PULSE_MS 50
#define SOLENOID_TEST_ACTIVE_HIGH 1

// U-shape physical assumption:
//   LED 0..7   = left side, top to bottom
//   LED 8..9   = bottom bridge
//   LED 10..17 = right side, bottom to top
// Bottom two warning LEDs:
static const uint8_t BOTTOM_LED_A = 8;
static const uint8_t BOTTOM_LED_B = 9;

// Anti-spam shot meter. Each shot lights one symmetric pair near the top/tips:
//   Shot 1 = LED 0 and 17
//   Shot 2 = LED 1 and 16
//   Shot 3 = LED 2 and 15
//   Shot 4 = LED 3 and 14
//   Shot 5 = LED 4 and 13
// When the 5-shot limit is reached, the five charge pairs turn solid red,
// then the cooldown reverses those pairs back to the team idle color.
// LEDs 5..12 are deliberately never used for the lockout meter so the robot
// still clearly displays its team color through the center/bottom of the U.
static const uint8_t SHOT_PAIR_COUNT = 5;
static const uint8_t SHOT_PAIR_LEDS[SHOT_PAIR_COUNT][2] = {
  {0, 17},
  {1, 16},
  {2, 15},
  {3, 14},
  {4, 13}
};

static const uint8_t COOLDOWN_RAIL_LENGTH = 9;
static const uint8_t COOLDOWN_LEFT_RAIL[COOLDOWN_RAIL_LENGTH]  = {8, 7, 6, 5, 4, 3, 2, 1, 0};
static const uint8_t COOLDOWN_RIGHT_RAIL[COOLDOWN_RAIL_LENGTH] = {9, 10, 11, 12, 13, 14, 15, 16, 17};

// ============================================================
// Default recipe values
// Edit these after tuning from the web UI recipe generator.
// ============================================================

static const uint8_t  DEFAULT_GLOBAL_BRIGHTNESS = 190;
static const uint8_t  DEFAULT_CONNECTED = 0;             // 0 disconnected, 1 connected
static const uint8_t  DEFAULT_LOW_BATTERY = 0;           // 0 off, 1 on
static const uint8_t  DEFAULT_TEAM = 1;                  // 0 blue, 1 orange
static const uint8_t  DEFAULT_USE_SPATIAL = 1;           // 0 led-order effects, 1 xy-spatial effects
static const uint8_t  DEFAULT_FLIP_UI = 0;               // UI only

static const uint32_t DEFAULT_TEAM_BLUE = 0x0050FF;
static const uint32_t DEFAULT_TEAM_ORANGE = 0xFF5000;

static const uint8_t  DEFAULT_DISCONNECTED_SPEED = 36;
static const uint8_t  DEFAULT_DISCONNECTED_BRIGHTNESS = 150;

static const uint8_t  DEFAULT_TEAM_BREATH_SPEED = 28;
static const uint8_t  DEFAULT_TEAM_BREATH_MIN_BRIGHTNESS = 35;
static const uint8_t  DEFAULT_TEAM_BREATH_BRIGHTNESS = 130;
static const uint8_t  DEFAULT_TEAM_BREATH_ROTATE = 0;
static const uint8_t  DEFAULT_TEAM_BREATH_DIAGONAL = 1;

static const uint8_t  DEFAULT_CHARGE_MODE = 0;
static const uint8_t  DEFAULT_CHARGE_SPEED = 48;
static const uint8_t  DEFAULT_CHARGE_PIXELS = 6;
static const uint8_t  DEFAULT_CHARGE_BRIGHTNESS = 255;
static const uint32_t DEFAULT_CHARGE_COLOR = 0x00FFAA;

static const uint8_t  DEFAULT_FIRE_MODE = 1;
static const uint8_t  DEFAULT_FIRE_SPEED = 70;
static const uint8_t  DEFAULT_FIRE_PIXELS = 4;
static const uint8_t  DEFAULT_FIRE_FADE = 150;
static const uint8_t  DEFAULT_FIRE_BRIGHTNESS = 255;
static const uint32_t DEFAULT_FIRE_COLOR = 0xFF2A00;

static const uint8_t  DEFAULT_MAX_BURST_SHOTS = 5;
static const uint16_t DEFAULT_BURST_WINDOW_MS = 2500;
static const uint16_t DEFAULT_COOLDOWN_MS = 2000;
static const uint8_t  DEFAULT_COOLDOWN_BRIGHTNESS = 255;

// Per-shot pair colors for the anti-spam meter.
// Pair 1 = LEDs 0 and 17, Pair 2 = LEDs 1 and 16, ... Pair 5 = LEDs 4 and 13.
// The fifth shot forces all five pairs red during cooldown, but these colors
// are used while the shot meter is building from shot 1 to shot 4.
static const uint32_t DEFAULT_SHOT_PAIR_COLORS[SHOT_PAIR_COUNT] = {
  0x00E5FF, // shot 1
  0x00FF6A, // shot 2
  0xFFEA00, // shot 3
  0xFF7A00, // shot 4
  0xFF0000  // shot 5
};

static const uint8_t  DEFAULT_COUNTDOWN_MODE = 2;
static const uint8_t  DEFAULT_COUNTDOWN_SECONDS = 10;
static const uint8_t  DEFAULT_COUNTDOWN_SPEED = 50;
static const uint8_t  DEFAULT_COUNTDOWN_BRIGHTNESS = 240;
static const uint32_t DEFAULT_COUNTDOWN_COLOR_A = 0xFFBE00;
static const uint32_t DEFAULT_COUNTDOWN_COLOR_B = 0x00FF00;

static const uint8_t  DEFAULT_PATTERN_AS_IDLE = 0;
static const uint8_t  DEFAULT_PATTERN_BRIGHTNESS = 120;
static const uint8_t  DEFAULT_PATTERN_PREVIEW_SECONDS = 6;

static const uint32_t DEFAULT_PATTERN_COLORS[MOHAWK_LED_COUNT] = {
  0xFF5000, 0xFF5000, 0xFF5000, 0xFF5000, 0xFF5000, 0xFF5000,
  0xFF5000, 0xFF5000, 0xFF5000, 0xFF5000, 0xFF5000, 0xFF5000,
  0xFF5000, 0xFF5000, 0xFF5000, 0xFF5000, 0xFF5000, 0xFF5000
};

// ============================================================
// Types
// ============================================================

struct XY
{
  float x;
  float y;
};

enum RobotConnectionState
{
  ROBOT_DISCONNECTED = 0,
  ROBOT_CONNECTED = 1
};

enum TeamColorMode
{
  TEAM_BLUE = 0,
  TEAM_ORANGE = 1
};

enum ChargeAnimationMode
{
  CHARGE_SHOT_BAR = 0,
  CHARGE_RAILS_GATHER = 1,
  CHARGE_CAPACITOR_FILL = 2,
  CHARGE_HEAT_STACK = 3,
  CHARGE_PLASMA_CORE = 4,
  CHARGE_SCANNER_ARM = 5,
  CHARGE_ANIMATION_COUNT = 6
};

enum FireAnimationMode
{
  FIRE_FORWARD_WAVE = 0,
  FIRE_DUAL_RAIL_SHOT = 1,
  FIRE_MUZZLE_BURST = 2,
  FIRE_RECOIL_SHOCK = 3,
  FIRE_RAINBOW_LAUNCH = 4,
  FIRE_SPARKLE_BLAST = 5,
  FIRE_ANIMATION_COUNT = 6
};

enum CountdownAnimationMode
{
  COUNTDOWN_U_PROGRESS = 0,
  COUNTDOWN_STADIUM_PULSE = 1,
  COUNTDOWN_TRAFFIC_BUILD = 2,
  COUNTDOWN_COMET_TICK = 3,
  COUNTDOWN_SIDE_LAUNCH = 4,
  COUNTDOWN_ANIMATION_COUNT = 5
};

// ============================================================
// Globals
// ============================================================

CRGB mohawk_leds[MOHAWK_LED_COUNT];
CRGB patternColors[MOHAWK_LED_COUNT];
XY xyMap[MOHAWK_LED_COUNT];

static WebServer server(80);
static Preferences prefs;

static RobotConnectionState gConnectionState = (RobotConnectionState)DEFAULT_CONNECTED;
static bool gLowBattery = DEFAULT_LOW_BATTERY;
static TeamColorMode gTeam = (TeamColorMode)DEFAULT_TEAM;
static bool gUseSpatialMapping = DEFAULT_USE_SPATIAL;
static bool gFlipUi = DEFAULT_FLIP_UI;

static uint8_t gGlobalBrightness = DEFAULT_GLOBAL_BRIGHTNESS;
static CRGB gTeamBlue;
static CRGB gTeamOrange;

static uint8_t gDisconnectedSpeed = DEFAULT_DISCONNECTED_SPEED;
static uint8_t gDisconnectedBrightness = DEFAULT_DISCONNECTED_BRIGHTNESS;

static uint8_t gTeamBreathSpeed = DEFAULT_TEAM_BREATH_SPEED;
static uint8_t gTeamBreathMinBrightness = DEFAULT_TEAM_BREATH_MIN_BRIGHTNESS;
static uint8_t gTeamBreathBrightness = DEFAULT_TEAM_BREATH_BRIGHTNESS;
static bool gTeamBreathRotate = DEFAULT_TEAM_BREATH_ROTATE;
static bool gTeamBreathDiagonal = DEFAULT_TEAM_BREATH_DIAGONAL;

// Compatibility aliases for the original simple team-breath function.
// gIdleSpeed controls beats-per-minute style speed.
// gIdleIntensity controls the top of the breath brightness wave.
#define gIdleSpeed gTeamBreathSpeed
#define gIdleIntensity gTeamBreathBrightness

static ChargeAnimationMode gChargeMode = (ChargeAnimationMode)DEFAULT_CHARGE_MODE;
static uint8_t gChargeSpeed = DEFAULT_CHARGE_SPEED;
static uint8_t gChargePixels = DEFAULT_CHARGE_PIXELS;
static uint8_t gChargeBrightness = DEFAULT_CHARGE_BRIGHTNESS;
static CRGB gChargeColor;

static FireAnimationMode gFireMode = (FireAnimationMode)DEFAULT_FIRE_MODE;
static uint8_t gFireSpeed = DEFAULT_FIRE_SPEED;
static uint8_t gFirePixels = DEFAULT_FIRE_PIXELS;
static uint8_t gFireFade = DEFAULT_FIRE_FADE;
static uint8_t gFireBrightness = DEFAULT_FIRE_BRIGHTNESS;
static CRGB gFireColor;

static uint8_t gMaxBurstShots = DEFAULT_MAX_BURST_SHOTS;
static uint16_t gBurstWindowMs = DEFAULT_BURST_WINDOW_MS;
static uint16_t gCooldownMs = DEFAULT_COOLDOWN_MS;
static uint8_t gCooldownBrightness = DEFAULT_COOLDOWN_BRIGHTNESS;
static CRGB gShotPairColors[SHOT_PAIR_COUNT];

static CountdownAnimationMode gCountdownMode = (CountdownAnimationMode)DEFAULT_COUNTDOWN_MODE;
static uint8_t gCountdownSeconds = DEFAULT_COUNTDOWN_SECONDS;
static uint8_t gCountdownSpeed = DEFAULT_COUNTDOWN_SPEED;
static uint8_t gCountdownBrightness = DEFAULT_COUNTDOWN_BRIGHTNESS;
static CRGB gCountdownColorA;
static CRGB gCountdownColorB;

static bool gPatternAsIdle = DEFAULT_PATTERN_AS_IDLE;
static uint8_t gPatternBrightness = DEFAULT_PATTERN_BRIGHTNESS;
static uint8_t gPatternPreviewSeconds = DEFAULT_PATTERN_PREVIEW_SECONDS;

static bool gChargeActive = false;
static uint32_t gChargeStartMs = 0;

static bool gFireActive = false;
static uint32_t gFireStartMs = 0;
static uint32_t gTotalShotsFired = 0;

static uint8_t gBurstShotCount = 0;
static uint32_t gBurstWindowStartMs = 0;
static uint32_t gCooldownUntilMs = 0;
static uint32_t gShotMeterHoldUntilMs = 0;

static bool gCountdownActive = false;
static uint32_t gCountdownStartMs = 0;
static uint8_t gCountdownDurationSec = DEFAULT_COUNTDOWN_SECONDS;

static uint32_t gPatternPreviewUntilMs = 0;
static int gIdentifyLed = -1;
static uint32_t gIdentifyUntilMs = 0;

// ============================================================
// Utility
// ============================================================

static uint8_t clampU8(int v, int lo = 0, int hi = 255)
{
  if (v < lo) return (uint8_t)lo;
  if (v > hi) return (uint8_t)hi;
  return (uint8_t)v;
}

static uint16_t clampU16(int v, int lo, int hi)
{
  if (v < lo) return (uint16_t)lo;
  if (v > hi) return (uint16_t)hi;
  return (uint16_t)v;
}

static CRGB scaledColor(CRGB c, uint8_t brightness)
{
  CRGB out = c;
  out.nscale8_video(brightness);
  return out;
}

static void blendLed(uint8_t led, CRGB c, uint8_t amount)
{
  if (led >= MOHAWK_LED_COUNT) return;
  mohawk_leds[led] = blend(mohawk_leds[led], c, amount);
}

static uint32_t crgbToHex(CRGB c)
{
  return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | (uint32_t)c.b;
}

static CRGB uint32ToCRGB(uint32_t value)
{
  return CRGB((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
}

static String hexString(CRGB c)
{
  char buf[8];
  snprintf(buf, sizeof(buf), "%02X%02X%02X", c.r, c.g, c.b);
  return String(buf);
}

static int hexDigit(char ch)
{
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

static CRGB hexToCRGB(String s, CRGB fallback = CRGB::Black)
{
  s.trim();
  s.replace("#", "");
  s.replace("%23", "");
  if (s.length() != 6) return fallback;

  int values[6];
  for (int i = 0; i < 6; i++)
  {
    values[i] = hexDigit(s.charAt(i));
    if (values[i] < 0) return fallback;
  }

  uint8_t r = (values[0] << 4) | values[1];
  uint8_t g = (values[2] << 4) | values[3];
  uint8_t b = (values[4] << 4) | values[5];
  return CRGB(r, g, b);
}

static CRGB teamColor()
{
  return (gTeam == TEAM_BLUE) ? gTeamBlue : gTeamOrange;
}

static float speedTo01PerMs(uint8_t speed, float minRate, float maxRate)
{
  float t = (float)constrain(speed, 1, 100) / 100.0f;
  return minRate + ((maxRate - minRate) * t);
}

static float ledOrderPosition(uint8_t led)
{
  return (float)led / (float)(MOHAWK_LED_COUNT - 1);
}

static float spatialPosition(uint8_t led)
{
  // All normal animation travel is driven from the 2D U-shape map.
  // The path runs from LED 0 down the left rail, across the bottom bridge,
  // then up the right rail to LED 17. If spatial mapping is disabled, fall
  // back to simple LED index order for troubleshooting.
  if (!gUseSpatialMapping) return ledOrderPosition(led);
  if (led >= MOHAWK_LED_COUNT) return 0.0f;

  const float x = xyMap[led].x;
  const float y = xyMap[led].y;

  if (x < 0.40f)
  {
    // Left rail: LED 0 at top is path start, LED 7 near bottom.
    return constrain((7.0f - y) / 17.0f, 0.0f, 1.0f);
  }
  if (x < 1.00f)
  {
    // Bottom-left bridge LED, normally LED 8.
    return 8.0f / 17.0f;
  }
  if (x < 1.60f)
  {
    // Bottom-right bridge LED, normally LED 9.
    return 9.0f / 17.0f;
  }

  // Right rail: LED 10 starts at the bottom and LED 17 ends at the top.
  return constrain((10.0f + y) / 17.0f, 0.0f, 1.0f);
}

static uint8_t ledAtPathStep(uint8_t step)
{
  // Finds the LED closest to a normalized U-path step using the 2D map.
  // This lets progress/fill animations use the map instead of assuming raw LED order.
  if (step >= MOHAWK_LED_COUNT) step = MOHAWK_LED_COUNT - 1;
  const float target = (float)step / (float)(MOHAWK_LED_COUNT - 1);
  uint8_t bestLed = 0;
  float bestError = 999.0f;
  for (uint8_t i = 0; i < MOHAWK_LED_COUNT; i++)
  {
    const float e = fabsf(spatialPosition(i) - target);
    if (e < bestError)
    {
      bestError = e;
      bestLed = i;
    }
  }
  return bestLed;
}

static void buildXYMap()
{
  // Coordinates are only used for spatial effects and the UI recipe. The web UI
  // draws its own SVG-style positions for a cleaner professional layout.
  for (int i = 0; i < MOHAWK_LED_COUNT; i++)
  {
    if (i <= 7)
    {
      xyMap[i].x = 0.0f;
      xyMap[i].y = 7.0f - (float)i;
    }
    else if (i == 8)
    {
      xyMap[i].x = 0.8f;
      xyMap[i].y = 0.0f;
    }
    else if (i == 9)
    {
      xyMap[i].x = 1.2f;
      xyMap[i].y = 0.0f;
    }
    else
    {
      xyMap[i].x = 2.0f;
      xyMap[i].y = (float)(i - 10);
    }
  }
}

static void setFactoryDefaults(bool alsoSave)
{
  gConnectionState = (RobotConnectionState)DEFAULT_CONNECTED;
  gLowBattery = DEFAULT_LOW_BATTERY;
  gTeam = (TeamColorMode)DEFAULT_TEAM;
  gUseSpatialMapping = DEFAULT_USE_SPATIAL;
  gFlipUi = DEFAULT_FLIP_UI;

  gGlobalBrightness = DEFAULT_GLOBAL_BRIGHTNESS;
  gTeamBlue = uint32ToCRGB(DEFAULT_TEAM_BLUE);
  gTeamOrange = uint32ToCRGB(DEFAULT_TEAM_ORANGE);

  gDisconnectedSpeed = DEFAULT_DISCONNECTED_SPEED;
  gDisconnectedBrightness = DEFAULT_DISCONNECTED_BRIGHTNESS;

  gTeamBreathSpeed = DEFAULT_TEAM_BREATH_SPEED;
  gTeamBreathMinBrightness = DEFAULT_TEAM_BREATH_MIN_BRIGHTNESS;
  gTeamBreathBrightness = DEFAULT_TEAM_BREATH_BRIGHTNESS;
  gTeamBreathRotate = DEFAULT_TEAM_BREATH_ROTATE;
  gTeamBreathDiagonal = DEFAULT_TEAM_BREATH_DIAGONAL;

  gChargeMode = (ChargeAnimationMode)DEFAULT_CHARGE_MODE;
  gChargeSpeed = DEFAULT_CHARGE_SPEED;
  gChargePixels = DEFAULT_CHARGE_PIXELS;
  gChargeBrightness = DEFAULT_CHARGE_BRIGHTNESS;
  gChargeColor = uint32ToCRGB(DEFAULT_CHARGE_COLOR);

  gFireMode = (FireAnimationMode)DEFAULT_FIRE_MODE;
  gFireSpeed = DEFAULT_FIRE_SPEED;
  gFirePixels = DEFAULT_FIRE_PIXELS;
  gFireFade = DEFAULT_FIRE_FADE;
  gFireBrightness = DEFAULT_FIRE_BRIGHTNESS;
  gFireColor = uint32ToCRGB(DEFAULT_FIRE_COLOR);

  gMaxBurstShots = DEFAULT_MAX_BURST_SHOTS;
  gBurstWindowMs = DEFAULT_BURST_WINDOW_MS;
  gCooldownMs = DEFAULT_COOLDOWN_MS;
  gCooldownBrightness = DEFAULT_COOLDOWN_BRIGHTNESS;
  for (uint8_t p = 0; p < SHOT_PAIR_COUNT; p++)
  {
    gShotPairColors[p] = uint32ToCRGB(DEFAULT_SHOT_PAIR_COLORS[p]);
  }

  gCountdownMode = (CountdownAnimationMode)DEFAULT_COUNTDOWN_MODE;
  gCountdownSeconds = DEFAULT_COUNTDOWN_SECONDS;
  gCountdownSpeed = DEFAULT_COUNTDOWN_SPEED;
  gCountdownBrightness = DEFAULT_COUNTDOWN_BRIGHTNESS;
  gCountdownColorA = uint32ToCRGB(DEFAULT_COUNTDOWN_COLOR_A);
  gCountdownColorB = uint32ToCRGB(DEFAULT_COUNTDOWN_COLOR_B);

  gPatternAsIdle = DEFAULT_PATTERN_AS_IDLE;
  gPatternBrightness = DEFAULT_PATTERN_BRIGHTNESS;
  gPatternPreviewSeconds = DEFAULT_PATTERN_PREVIEW_SECONDS;

  for (int i = 0; i < MOHAWK_LED_COUNT; i++)
  {
    patternColors[i] = uint32ToCRGB(DEFAULT_PATTERN_COLORS[i]);
  }

  gChargeActive = false;
  gFireActive = false;
  gCountdownActive = false;
  gPatternPreviewUntilMs = 0;
  gIdentifyLed = -1;
  gBurstShotCount = 0;
  gBurstWindowStartMs = 0;
  gCooldownUntilMs = 0;
  gShotMeterHoldUntilMs = 0;

  FastLED.setBrightness(gGlobalBrightness);

  if (alsoSave)
  {
    prefs.begin("mohawkv191", false);
    prefs.clear();
    prefs.end();
  }
}

// ============================================================
// Optional physical solenoid bench-test helper
// ============================================================

static bool physicalSolenoidTestEnabled()
{
#if SOLENOID_TEST_PIN >= 0
  return true;
#else
  return false;
#endif
}

static void pulse_physical_solenoid_for_test()
{
#if SOLENOID_TEST_PIN >= 0
  const uint8_t activeLevel = SOLENOID_TEST_ACTIVE_HIGH ? HIGH : LOW;
  const uint8_t idleLevel = SOLENOID_TEST_ACTIVE_HIGH ? LOW : HIGH;

  digitalWrite(SOLENOID_TEST_PIN, activeLevel);
  delay(SOLENOID_TEST_PULSE_MS);
  digitalWrite(SOLENOID_TEST_PIN, idleLevel);
#else
  // LED-only mode. Nothing physical is fired.
#endif
}

// ============================================================
// Public integration functions
// ============================================================

void mohawk_set_connected(bool connected)
{
  gConnectionState = connected ? ROBOT_CONNECTED : ROBOT_DISCONNECTED;
}

void mohawk_set_low_battery(bool lowBattery)
{
  gLowBattery = lowBattery;
}

void mohawk_start_solenoid_charge()
{
  gChargeActive = true;
  gChargeStartMs = millis();
}

void mohawk_stop_solenoid_charge()
{
  gChargeActive = false;
}

void mohawk_reset_shot_meter()
{
  gTotalShotsFired = 0;
  gBurstShotCount = 0;
  gBurstWindowStartMs = 0;
  gCooldownUntilMs = 0;
  gShotMeterHoldUntilMs = 0;
}

void mohawk_trigger_solenoid_animation()
{
  const uint32_t now = millis();

  if (now < gCooldownUntilMs)
  {
    // Cooldown is active, so ignore the firing request but keep the warning visible.
    gShotMeterHoldUntilMs = gCooldownUntilMs;
    return;
  }

  if (gBurstWindowStartMs == 0 || (now - gBurstWindowStartMs) > gBurstWindowMs)
  {
    gBurstWindowStartMs = now;
    gBurstShotCount = 0;
  }

  gBurstShotCount++;
  if (gBurstShotCount > gMaxBurstShots) gBurstShotCount = gMaxBurstShots;
  gTotalShotsFired++;

  gFireActive = true;
  gFireStartMs = now;
  gChargeActive = false;
  gShotMeterHoldUntilMs = now + gBurstWindowMs;

  if (gBurstShotCount >= gMaxBurstShots)
  {
    // Lockout wins over the firing overlay so LEDs 5..12 stay as team color.
    gFireActive = false;
    gCooldownUntilMs = now + gCooldownMs;
    gShotMeterHoldUntilMs = gCooldownUntilMs;
  }
}

void mohawk_start_countdown(uint8_t seconds)
{
  if (seconds == 0) seconds = 10;
  gCountdownDurationSec = seconds;
  gCountdownStartMs = millis();
  gCountdownActive = true;
}

void mohawk_stop_countdown()
{
  gCountdownActive = false;
}

// ============================================================
// Rendering helpers
// ============================================================

static void renderDisconnectedBase(uint32_t now)
{
  float speed = speedTo01PerMs(gDisconnectedSpeed, 0.000025f, 0.00035f);
  uint8_t hueBase = (uint8_t)(now * speed * 255.0f);

  for (uint8_t i = 0; i < MOHAWK_LED_COUNT; i++)
  {
    uint8_t hue = hueBase + (uint8_t)(spatialPosition(i) * 120.0f);
    mohawk_leds[i] = CHSV(hue, 255, gDisconnectedBrightness);
  }

  if (gLowBattery)
  {
    bool on = ((now / 220) % 2) == 0;
    CRGB warn = on ? CRGB(255, 0, 0) : CRGB(25, 0, 0);
    mohawk_leds[BOTTOM_LED_A] = scaledColor(warn, 255);
    mohawk_leds[BOTTOM_LED_B] = scaledColor(warn, 255);
  }
}

static void render_idle_team_breath()
{
  const uint8_t bpm = 8 + (gIdleSpeed / 6);
  const CRGB team = teamColor();
  const uint8_t minBrightness = min(gTeamBreathMinBrightness, (uint8_t)(gIdleIntensity - 1));
  const uint8_t maxBrightness = max((uint8_t)(minBrightness + 1), gIdleIntensity);

  // Diagonal toggle OFF gives the original all-LED team breath, but with the
  // new adjustable minimum brightness so it does not disappear at the low point.
  if (!gTeamBreathDiagonal)
  {
    uint8_t wave = beatsin8(bpm, minBrightness, maxBrightness);
    CRGB c = team;
    c.nscale8_video(wave);
    fill_solid(mohawk_leds, MOHAWK_LED_COUNT, c);
    return;
  }

  // Rotation center is intentionally locked to the exact midpoint between
  // LED 4 and LED 13. With the current 2D map:
  //   LED 4  = left rail,  x=0, y=3
  //   LED 13 = right rail, x=2, y=3
  //   Center = x=1, y=3
  const float centerX = (xyMap[4].x + xyMap[13].x) * 0.5f;
  const float centerY = (xyMap[4].y + xyMap[13].y) * 0.5f;

  float angle = 0.785398f; // 45 degrees, obvious fixed diagonal
  if (gTeamBreathRotate)
  {
    // Slow clock-hand rotation. Higher idle speed rotates faster, but still smooth.
    const float rotationsPerMs = 0.00022f + ((float)gIdleSpeed * 0.0000045f);
    angle = ((float)millis() * rotationsPerMs);
  }

  const float cs = cosf(angle);
  const float sn = sinf(angle);

  for (uint8_t i = 0; i < MOHAWK_LED_COUNT; i++)
  {
    const float centeredX = xyMap[i].x - centerX;
    const float centeredY = xyMap[i].y - centerY;
    const float projection = (centeredX * cs) + (centeredY * sn);

    // Bigger phase scale makes the diagonal easier to see on only 18 LEDs.
    const uint8_t phaseOffset = (uint8_t)(projection * 92.0f);

    uint8_t wave = beatsin8(bpm, minBrightness, maxBrightness, 0, phaseOffset);

    CRGB c = team;
    c.nscale8_video(wave);
    mohawk_leds[i] = c;
  }
}

static void renderTeamBreathBase(uint32_t now)
{
  (void)now;
  render_idle_team_breath();
}

static void renderPattern(uint8_t brightness)
{
  for (uint8_t i = 0; i < MOHAWK_LED_COUNT; i++)
  {
    mohawk_leds[i] = scaledColor(patternColors[i], brightness);
  }
}

static void renderChargeOverlay(uint32_t now)
{
  if (!gChargeActive) return;

  // During lockout, the shot meter owns the top five LED pairs.
  // This keeps LEDs 5..12 clean for the team-breath color and avoids confusing
  // charge graphics fighting the cooldown return animation.
  if (gCooldownUntilMs > 0 && now < gCooldownUntilMs) return;

  const uint32_t elapsed = now - gChargeStartMs;
  const float rate = speedTo01PerMs(gChargeSpeed, 0.00018f, 0.00220f);
  const float phase = fmodf((float)elapsed * rate, 1.0f);
  const float width = constrain((float)gChargePixels / (float)MOHAWK_LED_COUNT, 0.05f, 0.75f);
  const CRGB c = scaledColor(gChargeColor, gChargeBrightness);

  switch (gChargeMode)
  {
    case CHARGE_SHOT_BAR:
    default:
    {
      // Clean arming indicator: pulse the bottom bridge/core only.
      // The shot-pair meter itself is rendered by renderShotMeter().
      const uint8_t pulse = beatsin8(18 + (gChargeSpeed / 4), 70, 255);
      blendLed(8, c, pulse);
      blendLed(9, c, pulse);
      blendLed(7, c, scale8(pulse, 120));
      blendLed(10, c, scale8(pulse, 120));
      break;
    }

    case CHARGE_RAILS_GATHER:
    {
      // Two symmetric rails gather toward the bottom bridge using the 2D map.
      // Top LEDs have y near 7, bottom LEDs have y near 0.
      const float headY = 7.0f * (1.0f - phase);
      const float railWidthY = 0.35f + (width * 5.5f);
      for (uint8_t i = 0; i < MOHAWK_LED_COUNT; i++)
      {
        if (i == 8 || i == 9)
        {
          blendLed(i, c, beatsin8(20, 120, 255));
          continue;
        }
        const float d = fabsf(xyMap[i].y - headY);
        if (d < railWidthY)
        {
          const uint8_t amount = (uint8_t)((1.0f - (d / railWidthY)) * 255.0f);
          blendLed(i, c, amount);
        }
      }
      break;
    }

    case CHARGE_CAPACITOR_FILL:
    {
      // Fill along the full mapped U-path, then drain/repeat.
      for (uint8_t i = 0; i < MOHAWK_LED_COUNT; i++)
      {
        const float pos = spatialPosition(i);
        if (pos <= phase)
        {
          blendLed(i, c, 210);
        }
        else if (pos < phase + width)
        {
          const uint8_t amount = (uint8_t)((1.0f - ((pos - phase) / width)) * 180.0f);
          blendLed(i, c, amount);
        }
      }
      break;
    }

    case CHARGE_HEAT_STACK:
    {
      // Heat rises from the bottom bridge upward on both sides.
      const float fillY = 7.0f * phase;
      for (uint8_t i = 0; i < MOHAWK_LED_COUNT; i++)
      {
        const float y = xyMap[i].y;
        if (y <= fillY)
        {
          uint8_t amount = (uint8_t)(120.0f + (135.0f * (1.0f - (y / 7.0f))));
          blendLed(i, c, amount);
        }
      }
      break;
    }

    case CHARGE_PLASMA_CORE:
    {
      // Bright pulsing core around the bottom bridge while the team color stays visible elsewhere.
      const uint8_t pulse = beatsin8(28 + (gChargeSpeed / 3), 90, 255);
      const uint8_t soft = scale8(pulse, 130);
      blendLed(8, c, pulse);
      blendLed(9, c, pulse);
      blendLed(7, c, soft);
      blendLed(10, c, soft);
      blendLed(6, c, scale8(soft, 120));
      blendLed(11, c, scale8(soft, 120));
      break;
    }

    case CHARGE_SCANNER_ARM:
    {
      // Rotating diagonal scanner line rendered from the 2D map.
      const float centerX = (xyMap[4].x + xyMap[13].x) * 0.5f;
      const float centerY = (xyMap[4].y + xyMap[13].y) * 0.5f;
      const float angle = phase * TWO_PI;
      const float cs = cosf(angle);
      const float sn = sinf(angle);
      const float lineWidth = 0.18f + (width * 1.6f);

      for (uint8_t i = 0; i < MOHAWK_LED_COUNT; i++)
      {
        const float dx = xyMap[i].x - centerX;
        const float dy = xyMap[i].y - centerY;
        const float distanceToLine = fabsf((dx * -sn) + (dy * cs));
        if (distanceToLine < lineWidth)
        {
          const uint8_t amount = (uint8_t)((1.0f - (distanceToLine / lineWidth)) * 220.0f);
          blendLed(i, c, amount);
        }
      }
      break;
    }
  }
}

static void renderShotMeter(uint32_t now)
{
  // If cooldown just finished, clear the lockout meter cleanly.
  if (gCooldownUntilMs > 0 && now >= gCooldownUntilMs)
  {
    mohawk_reset_shot_meter();
    return;
  }

  const bool cooldown = (gCooldownUntilMs > 0 && now < gCooldownUntilMs);

  if (!cooldown)
  {
    // If the player stops spamming shots, the short-window meter expires.
    if (gBurstShotCount > 0 && (now - gBurstWindowStartMs) > gBurstWindowMs)
    {
      gBurstShotCount = 0;
      gBurstWindowStartMs = 0;
      gShotMeterHoldUntilMs = 0;
    }

    if (gBurstShotCount == 0 && now > gShotMeterHoldUntilMs)
    {
      return;
    }
  }

  if (cooldown)
  {
    // Cooldown is now a reverse lockout meter back to team idle color.
    // At the instant the 5th shot lands, all five charge pairs are solid red:
    //   0+17, 1+16, 2+15, 3+14, 4+13
    // Then the overlay removes the red pairs in reverse order so those LEDs
    // fall back to the already-rendered team-breath idle color underneath.
    // LEDs 5..12 are never touched by this cooldown meter, so the middle and
    // bottom of the U-shape always keep showing the team color.
    uint32_t cooldownStartMs = (gCooldownUntilMs > gCooldownMs) ? (gCooldownUntilMs - gCooldownMs) : now;
    uint32_t elapsed = now - cooldownStartMs;
    float progress = (float)elapsed / (float)max((uint16_t)1, gCooldownMs);
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    const uint8_t totalPairs = constrain(gMaxBurstShots, 1, SHOT_PAIR_COUNT);
    uint8_t pairsReturned = (uint8_t)floorf(progress * (float)(totalPairs + 0.001f));
    if (pairsReturned > totalPairs) pairsReturned = totalPairs;
    uint8_t remainingPairs = totalPairs - pairsReturned;

    CRGB lockoutRed = scaledColor(CRGB(255, 0, 0), gCooldownBrightness);

    for (uint8_t p = 0; p < remainingPairs; p++)
    {
      uint8_t left = SHOT_PAIR_LEDS[p][0];
      uint8_t right = SHOT_PAIR_LEDS[p][1];
      // Assignment, not blending: at cooldown start these pairs are truly red.
      mohawk_leds[left] = lockoutRed;
      mohawk_leds[right] = lockoutRed;
    }

    return;
  }

  // Normal anti-spam meter. Each shot lights a mirrored pair using its own color.
  const uint8_t filledPairs = min((uint8_t)SHOT_PAIR_COUNT, gBurstShotCount);
  uint8_t livePulse = beatsin8(14, 175, 255);

  for (uint8_t p = 0; p < SHOT_PAIR_COUNT; p++)
  {
    uint8_t left = SHOT_PAIR_LEDS[p][0];
    uint8_t right = SHOT_PAIR_LEDS[p][1];
    CRGB pairColor = scaledColor(gShotPairColors[p], gChargeBrightness);

    if (p < filledPairs)
    {
      uint8_t amount = (p == (filledPairs - 1)) ? livePulse : 245;
      blendLed(left, pairColor, amount);
      blendLed(right, pairColor, amount);
    }
  }
}

static void renderFireOverlay(uint32_t now)
{
  if (!gFireActive) return;

  // During lockout cooldown, the shot-meter owns the top LED pairs and the
  // center LEDs must stay as team color. Suppress extra firing overlays here.
  if (gCooldownUntilMs > 0 && now < gCooldownUntilMs)
  {
    gFireActive = false;
    return;
  }

  uint16_t duration = map(constrain(gFireSpeed, 1, 100), 1, 100, 1200, 180);
  uint32_t elapsed = now - gFireStartMs;
  if (elapsed >= duration)
  {
    gFireActive = false;
    return;
  }

  float progress = (float)elapsed / (float)duration;
  uint8_t pixels = constrain(gFirePixels, 1, MOHAWK_LED_COUNT);
  float width = (float)pixels / (float)MOHAWK_LED_COUNT;
  CRGB mainC = scaledColor(gFireColor, gFireBrightness);

  switch (gFireMode)
  {
    case FIRE_FORWARD_WAVE:
    {
      for (uint8_t i = 0; i < MOHAWK_LED_COUNT; i++)
      {
        float d = fabsf(spatialPosition(i) - progress);
        if (d < width)
        {
          blendLed(i, mainC, 255);
        }
        else if (d < width + 0.20f)
        {
          uint8_t amount = (uint8_t)((1.0f - ((d - width) / 0.20f)) * gFireFade);
          blendLed(i, mainC, amount);
        }
      }
      break;
    }

    case FIRE_DUAL_RAIL_SHOT:
    {
      // Bottom core launches up both rails using the 2D map.
      // y=0 is the bottom bridge, y=7 is the top/tip area.
      for (uint8_t i = 0; i < MOHAWK_LED_COUNT; i++)
      {
        const float sideProgress = constrain(xyMap[i].y / 7.0f, 0.0f, 1.0f);
        const float d = fabsf(sideProgress - progress);

        if (d < width) blendLed(i, mainC, 255);
        else if (d < width + 0.18f) blendLed(i, mainC, (uint8_t)((1.0f - ((d - width) / 0.18f)) * gFireFade));
      }
      break;
    }

    case FIRE_MUZZLE_BURST:
    {
      uint8_t amount = (uint8_t)((1.0f - progress) * 255.0f);
      for (uint8_t i = 0; i < MOHAWK_LED_COUNT; i++)
      {
        blendLed(i, mainC, scale8(amount, 80));
      }
      blendLed(0, mainC, amount);
      blendLed(17, mainC, amount);
      blendLed(1, mainC, scale8(amount, 220));
      blendLed(16, mainC, scale8(amount, 220));
      break;
    }

    case FIRE_RECOIL_SHOCK:
    {
      float head = 1.0f - progress;
      for (uint8_t i = 0; i < MOHAWK_LED_COUNT; i++)
      {
        float p = spatialPosition(i);
        float d = fabsf(p - head);
        if (d < width) blendLed(i, mainC, 255);
        else if (d < width + 0.20f) blendLed(i, mainC, (uint8_t)((1.0f - ((d - width) / 0.20f)) * gFireFade));
      }
      break;
    }

    case FIRE_RAINBOW_LAUNCH:
    {
      for (uint8_t i = 0; i < MOHAWK_LED_COUNT; i++)
      {
        float d = fabsf(spatialPosition(i) - progress);
        if (d < width + 0.12f)
        {
          CRGB rainbow = CHSV((uint8_t)(progress * 255.0f + i * 12), 255, gFireBrightness);
          blendLed(i, rainbow, 220);

          if (d < width * 0.45f)
          {
            blendLed(i, mainC, 105);
          }
        }
      }
      break;
    }

    case FIRE_SPARKLE_BLAST:
    default:
    {
      uint8_t decay = (uint8_t)((1.0f - progress) * 255.0f);
      for (uint8_t i = 0; i < MOHAWK_LED_COUNT; i++)
      {
        if (random8() < map(decay, 0, 255, 20, 170))
        {
          blendLed(i, mainC, random8(100, 255));
        }
      }
      break;
    }
  }
}

static void renderCountdownOverlay(uint32_t now)
{
  if (!gCountdownActive) return;

  uint32_t durationMs = (uint32_t)gCountdownDurationSec * 1000UL;
  uint32_t elapsed = now - gCountdownStartMs;
  if (elapsed >= durationMs)
  {
    gCountdownActive = false;
    fill_solid(mohawk_leds, MOHAWK_LED_COUNT, scaledColor(gCountdownColorB, gCountdownBrightness));
    return;
  }

  uint8_t secondsLeft = gCountdownDurationSec - (elapsed / 1000);
  float progress = (float)elapsed / (float)durationMs;
  CRGB a = scaledColor(gCountdownColorA, gCountdownBrightness);
  CRGB b = scaledColor(gCountdownColorB, gCountdownBrightness);
  uint16_t tickPeriod = map(constrain(gCountdownSpeed, 1, 100), 1, 100, 1400, 300);
  uint8_t tickPulse = sin8((uint8_t)((elapsed % tickPeriod) * 255 / tickPeriod));

  switch (gCountdownMode)
  {
    case COUNTDOWN_U_PROGRESS:
    {
      fill_solid(mohawk_leds, MOHAWK_LED_COUNT, scaledColor(teamColor(), 20));
      uint8_t count = (uint8_t)((1.0f - progress) * MOHAWK_LED_COUNT);
      for (uint8_t step = 0; step < count; step++)
      {
        blendLed(ledAtPathStep(step), a, 220);
      }
      break;
    }

    case COUNTDOWN_STADIUM_PULSE:
    {
      CRGB pulseColor = (secondsLeft <= 1) ? b : a;
      fill_solid(mohawk_leds, MOHAWK_LED_COUNT, scaledColor(pulseColor, map(tickPulse, 0, 255, 45, gCountdownBrightness)));
      break;
    }

    case COUNTDOWN_TRAFFIC_BUILD:
    {
      fill_solid(mohawk_leds, MOHAWK_LED_COUNT, CRGB::Black);
      CRGB tickColor = (secondsLeft <= 3) ? b : ((secondsLeft <= 6) ? a : CRGB(255, 0, 0));
      tickColor = scaledColor(tickColor, gCountdownBrightness);
      uint8_t fillCount = map(gCountdownDurationSec - secondsLeft + 1, 0, gCountdownDurationSec, 1, MOHAWK_LED_COUNT);
      for (uint8_t step = 0; step < fillCount && step < MOHAWK_LED_COUNT; step++)
      {
        blendLed(ledAtPathStep(step), tickColor, 240);
      }
      blendLed(BOTTOM_LED_A, scaledColor(CRGB::White, gCountdownBrightness), scale8(tickPulse, 160));
      blendLed(BOTTOM_LED_B, scaledColor(CRGB::White, gCountdownBrightness), scale8(tickPulse, 160));
      break;
    }

    case COUNTDOWN_COMET_TICK:
    {
      fill_solid(mohawk_leds, MOHAWK_LED_COUNT, scaledColor(teamColor(), 18));
      uint8_t head = (uint8_t)(((elapsed % 1000) / 1000.0f) * MOHAWK_LED_COUNT);
      for (int o = -3; o <= 3; o++)
      {
        uint8_t step = (uint8_t)((head + o + MOHAWK_LED_COUNT) % MOHAWK_LED_COUNT);
        uint8_t led = ledAtPathStep(step);
        uint8_t amount = 255 - min(220, abs(o) * 58);
        blendLed(led, a, amount);
      }
      break;
    }

    case COUNTDOWN_SIDE_LAUNCH:
    default:
    {
      fill_solid(mohawk_leds, MOHAWK_LED_COUNT, scaledColor(teamColor(), 22));
      float p = (elapsed % 1000) / 1000.0f;
      for (uint8_t i = 0; i < MOHAWK_LED_COUNT; i++)
      {
        float side = spatialPosition(i);
        if (fabsf(side - p) < 0.10f || fabsf(side - (1.0f - p)) < 0.10f)
        {
          blendLed(i, (secondsLeft <= 2) ? b : a, 240);
        }
      }
      break;
    }
  }
}

static void renderIdentifyOverlay(uint32_t now)
{
  if (gIdentifyLed < 0 || gIdentifyLed >= MOHAWK_LED_COUNT) return;
  if (now >= gIdentifyUntilMs)
  {
    gIdentifyLed = -1;
    return;
  }

  bool on = ((now / 100) % 2) == 0;
  mohawk_leds[gIdentifyLed] = on ? CRGB::White : CRGB::Black;
}

void mohawk_tick()
{
  const uint32_t now = millis();

  FastLED.setBrightness(gGlobalBrightness);

  if (gConnectionState == ROBOT_DISCONNECTED)
  {
    renderDisconnectedBase(now);
  }
  else
  {
    if (gPatternAsIdle || now < gPatternPreviewUntilMs)
    {
      renderPattern(gPatternBrightness);
    }
    else
    {
      renderTeamBreathBase(now);
    }

    renderChargeOverlay(now);
    renderShotMeter(now);
    renderFireOverlay(now);
  }

  renderCountdownOverlay(now);
  renderIdentifyOverlay(now);

  FastLED.show();
}

// ============================================================
// Preferences
// ============================================================

static void saveSettings()
{
  prefs.begin("mohawkv191", false);

  prefs.putUChar("conn", (uint8_t)gConnectionState);
  prefs.putBool("low", gLowBattery);
  prefs.putUChar("team", (uint8_t)gTeam);
  prefs.putBool("spatial", gUseSpatialMapping);
  prefs.putBool("flipui", gFlipUi);

  prefs.putUChar("globB", gGlobalBrightness);
  prefs.putUInt("blue", crgbToHex(gTeamBlue));
  prefs.putUInt("orange", crgbToHex(gTeamOrange));

  prefs.putUChar("discS", gDisconnectedSpeed);
  prefs.putUChar("discB", gDisconnectedBrightness);

  prefs.putUChar("tbS", gTeamBreathSpeed);
  prefs.putUChar("tbMin", gTeamBreathMinBrightness);
  prefs.putUChar("tbB", gTeamBreathBrightness);
  prefs.putBool("tbRot", gTeamBreathRotate);
  prefs.putBool("tbDiag", gTeamBreathDiagonal);

  prefs.putUChar("chgM", (uint8_t)gChargeMode);
  prefs.putUChar("chgS", gChargeSpeed);
  prefs.putUChar("chgP", gChargePixels);
  prefs.putUChar("chgB", gChargeBrightness);
  prefs.putUInt("chgC", crgbToHex(gChargeColor));

  prefs.putUChar("fireM", (uint8_t)gFireMode);
  prefs.putUChar("fireS", gFireSpeed);
  prefs.putUChar("fireP", gFirePixels);
  prefs.putUChar("fireF", gFireFade);
  prefs.putUChar("fireB", gFireBrightness);
  prefs.putUInt("fireC", crgbToHex(gFireColor));

  prefs.putUChar("maxShot", gMaxBurstShots);
  prefs.putUShort("winMs", gBurstWindowMs);
  prefs.putUShort("coolMs", gCooldownMs);
  prefs.putUChar("coolB", gCooldownBrightness);
  prefs.putBytes("shotClr", gShotPairColors, sizeof(gShotPairColors));

  prefs.putUChar("cntM", (uint8_t)gCountdownMode);
  prefs.putUChar("cntSec", gCountdownSeconds);
  prefs.putUChar("cntS", gCountdownSpeed);
  prefs.putUChar("cntB", gCountdownBrightness);
  prefs.putUInt("cntA", crgbToHex(gCountdownColorA));
  prefs.putUInt("cntB2", crgbToHex(gCountdownColorB));

  prefs.putBool("patIdle", gPatternAsIdle);
  prefs.putUChar("patB", gPatternBrightness);
  prefs.putUChar("patPrev", gPatternPreviewSeconds);
  prefs.putBytes("pat", patternColors, sizeof(patternColors));

  prefs.end();
}

static void loadSettings()
{
  setFactoryDefaults(false);

  prefs.begin("mohawkv191", true);

  gConnectionState = (RobotConnectionState)prefs.getUChar("conn", (uint8_t)gConnectionState);
  gLowBattery = prefs.getBool("low", gLowBattery);
  gTeam = (TeamColorMode)prefs.getUChar("team", (uint8_t)gTeam);
  gUseSpatialMapping = prefs.getBool("spatial", gUseSpatialMapping);
  gFlipUi = prefs.getBool("flipui", gFlipUi);

  gGlobalBrightness = prefs.getUChar("globB", gGlobalBrightness);
  gTeamBlue = CRGB((prefs.getUInt("blue", crgbToHex(gTeamBlue)) >> 16) & 0xFF, (prefs.getUInt("blue", crgbToHex(gTeamBlue)) >> 8) & 0xFF, prefs.getUInt("blue", crgbToHex(gTeamBlue)) & 0xFF);
  gTeamOrange = CRGB((prefs.getUInt("orange", crgbToHex(gTeamOrange)) >> 16) & 0xFF, (prefs.getUInt("orange", crgbToHex(gTeamOrange)) >> 8) & 0xFF, prefs.getUInt("orange", crgbToHex(gTeamOrange)) & 0xFF);

  gDisconnectedSpeed = prefs.getUChar("discS", gDisconnectedSpeed);
  gDisconnectedBrightness = prefs.getUChar("discB", gDisconnectedBrightness);

  gTeamBreathSpeed = prefs.getUChar("tbS", gTeamBreathSpeed);
  gTeamBreathMinBrightness = prefs.getUChar("tbMin", gTeamBreathMinBrightness);
  gTeamBreathBrightness = prefs.getUChar("tbB", gTeamBreathBrightness);
  gTeamBreathRotate = prefs.getBool("tbRot", gTeamBreathRotate);
  gTeamBreathDiagonal = prefs.getBool("tbDiag", gTeamBreathDiagonal);
  if (gTeamBreathMinBrightness >= gTeamBreathBrightness)
  {
    gTeamBreathMinBrightness = max((uint8_t)0, (uint8_t)(gTeamBreathBrightness - 1));
  }

  gChargeMode = (ChargeAnimationMode)prefs.getUChar("chgM", (uint8_t)gChargeMode);
  gChargeSpeed = prefs.getUChar("chgS", gChargeSpeed);
  gChargePixels = prefs.getUChar("chgP", gChargePixels);
  gChargeBrightness = prefs.getUChar("chgB", gChargeBrightness);
  uint32_t chgHex = prefs.getUInt("chgC", crgbToHex(gChargeColor));
  gChargeColor = CRGB((chgHex >> 16) & 0xFF, (chgHex >> 8) & 0xFF, chgHex & 0xFF);

  gFireMode = (FireAnimationMode)prefs.getUChar("fireM", (uint8_t)gFireMode);
  gFireSpeed = prefs.getUChar("fireS", gFireSpeed);
  gFirePixels = prefs.getUChar("fireP", gFirePixels);
  gFireFade = prefs.getUChar("fireF", prefs.getUChar("fireT", gFireFade));
  gFireBrightness = prefs.getUChar("fireB", gFireBrightness);
  uint32_t fireHex = prefs.getUInt("fireC", crgbToHex(gFireColor));
  gFireColor = CRGB((fireHex >> 16) & 0xFF, (fireHex >> 8) & 0xFF, fireHex & 0xFF);

  gMaxBurstShots = prefs.getUChar("maxShot", gMaxBurstShots);
  gBurstWindowMs = prefs.getUShort("winMs", gBurstWindowMs);
  gCooldownMs = prefs.getUShort("coolMs", gCooldownMs);
  gCooldownBrightness = prefs.getUChar("coolB", gCooldownBrightness);
  if (prefs.getBytesLength("shotClr") == sizeof(gShotPairColors))
  {
    prefs.getBytes("shotClr", gShotPairColors, sizeof(gShotPairColors));
  }

  gCountdownMode = (CountdownAnimationMode)prefs.getUChar("cntM", (uint8_t)gCountdownMode);
  gCountdownSeconds = prefs.getUChar("cntSec", gCountdownSeconds);
  gCountdownSpeed = prefs.getUChar("cntS", gCountdownSpeed);
  gCountdownBrightness = prefs.getUChar("cntB", gCountdownBrightness);
  uint32_t cntA = prefs.getUInt("cntA", crgbToHex(gCountdownColorA));
  uint32_t cntB = prefs.getUInt("cntB2", crgbToHex(gCountdownColorB));
  gCountdownColorA = CRGB((cntA >> 16) & 0xFF, (cntA >> 8) & 0xFF, cntA & 0xFF);
  gCountdownColorB = CRGB((cntB >> 16) & 0xFF, (cntB >> 8) & 0xFF, cntB & 0xFF);

  gPatternAsIdle = prefs.getBool("patIdle", gPatternAsIdle);
  gPatternBrightness = prefs.getUChar("patB", gPatternBrightness);
  gPatternPreviewSeconds = prefs.getUChar("patPrev", gPatternPreviewSeconds);

  if (prefs.getBytesLength("pat") == sizeof(patternColors))
  {
    prefs.getBytes("pat", patternColors, sizeof(patternColors));
  }

  prefs.end();

  gMaxBurstShots = constrain(gMaxBurstShots, 1, 5);
  gChargeMode = (ChargeAnimationMode)constrain((int)gChargeMode, 0, CHARGE_ANIMATION_COUNT - 1);
  gFireMode = (FireAnimationMode)constrain((int)gFireMode, 0, FIRE_ANIMATION_COUNT - 1);
  gCountdownMode = (CountdownAnimationMode)constrain((int)gCountdownMode, 0, COUNTDOWN_ANIMATION_COUNT - 1);
}

// ============================================================
// Web UI
// ============================================================

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
<title>Mohawk LED Controller v1.9.1</title>
<style>
:root{
  --bg:#07111f; --panel:#101d31; --panel2:#132640; --line:#263b5d;
  --text:#eaf2ff; --muted:#91a5c5; --accent:#2be4ff; --accent2:#ffb000;
  --good:#00ff88; --bad:#ff335d; --warn:#ffbe00;
}
*{box-sizing:border-box}
body{margin:0;background:radial-gradient(circle at top left,#17304f 0,#07111f 42%,#030713 100%);color:var(--text);font-family:Inter,system-ui,-apple-system,Segoe UI,Arial,sans-serif}
header{position:sticky;top:0;z-index:10;background:rgba(7,17,31,.88);backdrop-filter:blur(12px);border-bottom:1px solid var(--line);padding:16px 20px;display:flex;align-items:center;gap:16px;justify-content:space-between;flex-wrap:wrap}
h1{margin:0;font-size:22px;letter-spacing:.3px}.sub{color:var(--muted);font-size:13px;margin-top:4px}.pill{border:1px solid var(--line);background:#091628;border-radius:999px;padding:8px 12px;font-size:13px;color:var(--muted)}.pill strong{color:var(--text)}
main{max-width:1280px;margin:0 auto;padding:18px;display:grid;grid-template-columns:390px 1fr;gap:18px}.card{background:linear-gradient(180deg,rgba(19,38,64,.96),rgba(9,20,36,.96));border:1px solid var(--line);border-radius:22px;box-shadow:0 18px 50px rgba(0,0,0,.28);padding:18px}.card h2{font-size:17px;margin:0 0 12px}.card h3{font-size:14px;margin:18px 0 8px;color:#bfd4f7}.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}.grid3{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:10px}.row{display:flex;align-items:center;justify-content:space-between;gap:12px;margin:10px 0}.label{font-size:13px;color:var(--muted)}.value{font-size:12px;color:var(--accent);font-variant-numeric:tabular-nums}select,input[type=range],input[type=number]{width:100%;accent-color:var(--accent)}select,input[type=number]{background:#071526;border:1px solid var(--line);border-radius:12px;color:var(--text);padding:10px}input[type=color]{width:100%;height:42px;border:1px solid var(--line);border-radius:12px;background:#071526;padding:4px}button{border:0;border-radius:14px;background:linear-gradient(135deg,#1ecbff,#3166ff);color:white;padding:11px 13px;font-weight:800;letter-spacing:.2px;cursor:pointer;box-shadow:0 10px 24px rgba(30,203,255,.16)}button.secondary{background:#152942;color:#d8e9ff;border:1px solid var(--line);box-shadow:none}button.danger{background:linear-gradient(135deg,#ff335d,#b4002a)}button.warn{background:linear-gradient(135deg,#ffbe00,#ff6b00)}button.good{background:linear-gradient(135deg,#00d084,#00aaff)}button:active{transform:translateY(1px)}.btnrow{display:flex;flex-wrap:wrap;gap:8px}.statusbar{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:10px;margin-bottom:12px}.status{border:1px solid var(--line);background:#08172b;border-radius:16px;padding:11px}.status .k{font-size:11px;color:var(--muted)}.status .v{font-size:18px;font-weight:900;margin-top:3px}.robotWrap{display:flex;justify-content:center;align-items:center;min-height:475px;background:radial-gradient(circle,#0f2a4e 0,#061020 62%);border:1px solid var(--line);border-radius:24px;position:relative;overflow:hidden}.robotWrap:before{content:"";position:absolute;inset:24px;border:1px dashed rgba(145,165,197,.25);border-radius:32px}.robot{position:relative;width:330px;height:430px}.led{position:absolute;width:46px;height:46px;border-radius:50%;display:grid;place-items:center;background:#0a172b;border:2px solid #28456e;color:#eaf2ff;font-weight:900;font-size:12px;box-shadow:0 0 0 4px rgba(0,0,0,.18),0 0 28px rgba(43,228,255,.12);cursor:pointer;transition:.12s}.led:hover{transform:scale(1.08);border-color:var(--accent)}.led.selected{outline:3px solid var(--accent2)}.section{border-top:1px solid rgba(145,165,197,.18);padding-top:14px;margin-top:14px}.note{font-size:12px;line-height:1.45;color:var(--muted)}.recipe{width:100%;min-height:220px;background:#040b15;color:#b8ffdf;border:1px solid var(--line);border-radius:16px;padding:12px;font-family:ui-monospace,SFMono-Regular,Consolas,monospace;font-size:12px}.tog{display:flex;align-items:center;gap:9px;color:var(--muted);font-size:13px}.tog input{width:18px;height:18px;accent-color:var(--accent)}.small{font-size:12px;color:var(--muted)}.hide{display:none}.mono{font-family:ui-monospace,SFMono-Regular,Consolas,monospace}@media(max-width:960px){main{grid-template-columns:1fr}.statusbar{grid-template-columns:repeat(2,minmax(0,1fr))}.robot{width:300px;height:400px}.led{width:42px;height:42px}}
</style>
</head>
<body>
<header>
  <div><h1>Mohawk LED Controller <span class="small">v1.9</span></h1><div class="sub">Disconnected state, 2D-map team breath, single-color firing, shot-meter test firing, red-to-team cooldown return, countdown, and pattern designer</div></div>
  <div class="btnrow"><span class="pill">AP: <strong>MiniBot_LED_UI</strong></span><span class="pill">URL: <strong>192.168.4.1</strong></span></div>
</header>
<main>
  <section class="card">
    <h2>Live Robot Layout</h2>
    <div class="statusbar">
      <div class="status"><div class="k">State</div><div class="v" id="stState">?</div></div>
      <div class="status"><div class="k">Shots</div><div class="v" id="stShots">0</div></div>
      <div class="status"><div class="k">Burst</div><div class="v" id="stBurst">0/5</div></div>
      <div class="status"><div class="k">Cooldown</div><div class="v" id="stCool">0.0s</div></div>
    </div>
    <div class="robotWrap"><div class="robot" id="robot"></div></div>
    <div class="section">
      <h3>Pattern Designer</h3>
      <div class="row"><span class="label">Brush color</span><input id="brush" type="color" value="#00ffaa"></div>
      <div class="grid">
        <button class="secondary" onclick="previewPattern()">Test Pattern</button>
        <button class="secondary" onclick="fillPattern()">Fill All With Brush</button>
      </div>
      <div class="grid" style="margin-top:8px">
        <button class="secondary" onclick="clearPattern()">Clear Pattern</button>
        <button class="secondary" onclick="randomPattern()">Random Spark Pattern</button>
      </div>
      <div class="row"><label class="tog"><input id="patternAsIdle" type="checkbox" onchange="setParam('patternAsIdle',this.checked?1:0)">Use pattern as connected idle</label></div>
      <div class="row"><span class="label">Pattern brightness</span><span class="value" id="patternBrightnessVal"></span></div><input id="patternBrightness" type="range" min="1" max="255" oninput="rangeSet(this,'patternBrightness')">
      <p class="note">Click any LED in the U-shape to paint it. This section is separated so you can prototype patterns without changing the main team-breath behavior unless you enable pattern idle.</p>
    </div>
  </section>

  <section class="card">
    <h2>Robot State and Global Settings</h2>
    <div class="grid3">
      <button class="good" onclick="test('connected')">Set Connected</button>
      <button class="warn" onclick="test('disconnected')">Set Disconnected</button>
      <button class="secondary" onclick="test('lowBattery')">Toggle Low Battery</button>
    </div>
    <div class="grid" style="margin-top:12px">
      <div><div class="label">Team</div><select id="team" onchange="setParam('team',this.value)"><option value="0">Blue Team</option><option value="1">Orange Team</option></select></div>
      <div><div class="label">Hardware brightness cap</div><input id="globalBrightness" type="range" min="1" max="255" oninput="rangeSet(this,'globalBrightness')"><div class="value" id="globalBrightnessVal"></div></div>
    </div>
    <div class="grid" style="margin-top:12px">
      <div><div class="label">Blue team color</div><input id="teamBlue" type="color" onchange="setParam('teamBlue',this.value)"></div>
      <div><div class="label">Orange team color</div><input id="teamOrange" type="color" onchange="setParam('teamOrange',this.value)"></div>
    </div>
    <div class="row"><label class="tog"><input id="useSpatial" type="checkbox" onchange="setParam('useSpatial',this.checked?1:0)">Use spatial LED mapping for effects</label></div>
    <div class="row"><label class="tog"><input id="flipUi" type="checkbox" onchange="setParam('flipUi',this.checked?1:0); drawRobot(lastStatus)">Flip UI left/right</label></div>

    <div class="section">
      <h2>Disconnected Mode</h2>
      <p class="note">Powered on, not connected to controller. Rainbow wash runs continuously. If low battery is enabled, the bottom two LEDs flash red.</p>
      <div class="grid">
        <div><div class="row"><span class="label">Rainbow speed</span><span class="value" id="disconnectedSpeedVal"></span></div><input id="disconnectedSpeed" type="range" min="1" max="100" oninput="rangeSet(this,'disconnectedSpeed')"></div>
        <div><div class="row"><span class="label">Disconnected brightness</span><span class="value" id="disconnectedBrightnessVal"></span></div><input id="disconnectedBrightness" type="range" min="1" max="255" oninput="rangeSet(this,'disconnectedBrightness')"></div>
      </div>
      <div class="btnrow" style="margin-top:10px"><button class="secondary" onclick="test('disconnected')">Test Disconnected</button><button class="secondary" onclick="test('lowBatteryOn')">Test Low Battery</button></div>
    </div>

    <div class="section">
      <h2>Connected Idle: Team Breath</h2>
      <p class="note">This is the normal connected state. It stays under the other effects so the robot still looks like its team even when charge or shot meter is visible.</p>
      <div class="grid">
        <div><div class="row"><span class="label">Speed</span><span class="value" id="teamBreathSpeedVal"></span></div><input id="teamBreathSpeed" type="range" min="1" max="100" oninput="rangeSet(this,'teamBreathSpeed')"></div>
        <div><div class="row"><span class="label">Minimum brightness</span><span class="value" id="teamBreathMinBrightnessVal"></span></div><input id="teamBreathMinBrightness" type="range" min="0" max="254" oninput="rangeSet(this,'teamBreathMinBrightness')"></div>
        <div><div class="row"><span class="label">Breath brightness peak</span><span class="value" id="teamBreathBrightnessVal"></span></div><input id="teamBreathBrightness" type="range" min="1" max="255" oninput="rangeSet(this,'teamBreathBrightness')"></div>
        <div class="row"><label class="tog"><input id="teamBreathDiagonal" type="checkbox" onchange="setParam('teamBreathDiagonal',this.checked?1:0)">Use diagonal breath wave</label></div>
        <div class="row"><label class="tog"><input id="teamBreathRotate" type="checkbox" onchange="setParam('teamBreathRotate',this.checked?1:0)">Rotate diagonal angle like a clock</label></div>
      </div>
      <p class="note">Minimum brightness keeps the darker part of the wave visible. Diagonal can be toggled off for the original all-LED breath. Rotate spins the diagonal around the exact midpoint between LED 4 and LED 13.</p>
      <div class="btnrow" style="margin-top:10px"><button class="secondary" onclick="test('connected')">Test Team Breath</button></div>
    </div>

    <div class="section">
      <h2>Solenoid Charge and 5-Shot Lockout Meter</h2>
      <p class="note">Shot meter behavior: shot 1 lights LEDs 0 and 17, shot 2 lights 1 and 16, shot 3 lights 2 and 15, shot 4 lights 3 and 14, and shot 5 lights 4 and 13. Each shot pair has its own color. On the fifth shot, all five shot pairs force red, then the cooldown reverses those red pairs back to the team-breath idle color. LEDs 5 through 12 stay reserved for team color so the robot never looks like it switched teams.</p>
      <div class="grid">
        <div><div class="label">Charge animation</div><select id="chargeMode" onchange="setParam('chargeMode',this.value)"><option value="0">Shot Pair Meter</option><option value="1">Rails Gather</option><option value="2">Capacitor Fill</option><option value="3">Heat Stack</option><option value="4">Plasma Core</option><option value="5">Scanner Arm</option></select></div>
        <div><div class="label">General charge color</div><input id="chargeColor" type="color" onchange="setParam('chargeColor',this.value)"></div>
      </div>
      <div class="grid3" style="margin-top:12px">
        <div><div class="row"><span class="label">Speed</span><span class="value" id="chargeSpeedVal"></span></div><input id="chargeSpeed" type="range" min="1" max="100" oninput="rangeSet(this,'chargeSpeed')"></div>
        <div><div class="row"><span class="label">Pixels</span><span class="value" id="chargePixelsVal"></span></div><input id="chargePixels" type="range" min="1" max="18" oninput="rangeSet(this,'chargePixels')"></div>
        <div><div class="row"><span class="label">Meter brightness</span><span class="value" id="chargeBrightnessVal"></span></div><input id="chargeBrightness" type="range" min="1" max="255" oninput="rangeSet(this,'chargeBrightness')"></div>
      </div>
      <h3>Shot Pair Colors</h3>
      <div class="grid3">
        <div><div class="label">Shot 1: LEDs 0 + 17</div><input id="shotColor0" type="color" onchange="setParam('shotColor0',this.value)"></div>
        <div><div class="label">Shot 2: LEDs 1 + 16</div><input id="shotColor1" type="color" onchange="setParam('shotColor1',this.value)"></div>
        <div><div class="label">Shot 3: LEDs 2 + 15</div><input id="shotColor2" type="color" onchange="setParam('shotColor2',this.value)"></div>
        <div><div class="label">Shot 4: LEDs 3 + 14</div><input id="shotColor3" type="color" onchange="setParam('shotColor3',this.value)"></div>
        <div><div class="label">Shot 5: LEDs 4 + 13</div><input id="shotColor4" type="color" onchange="setParam('shotColor4',this.value)"></div>
      </div>
      <div class="grid3" style="margin-top:12px">
        <div><div class="row"><span class="label">Max burst shots</span><span class="value" id="maxBurstShotsVal"></span></div><input id="maxBurstShots" type="range" min="1" max="5" oninput="rangeSet(this,'maxBurstShots')"></div>
        <div><div class="row"><span class="label">Burst window ms</span><span class="value" id="burstWindowMsVal"></span></div><input id="burstWindowMs" type="range" min="500" max="6000" step="100" oninput="rangeSet(this,'burstWindowMs')"></div>
        <div><div class="row"><span class="label">Cooldown ms</span><span class="value" id="cooldownMsVal"></span></div><input id="cooldownMs" type="range" min="500" max="5000" step="100" oninput="rangeSet(this,'cooldownMs')"></div>
      </div>
      <div class="row"><span class="label">Cooldown red overlay brightness</span><span class="value" id="cooldownBrightnessVal"></span></div><input id="cooldownBrightness" type="range" min="1" max="255" oninput="rangeSet(this,'cooldownBrightness')">
      <div class="btnrow" style="margin-top:12px">
        <button class="good" onclick="testFire()">Simulate One Solenoid Fire</button>
        <button class="warn" onclick="testBurst5()">Run 5-Shot Burst Test</button>
        <button class="secondary" onclick="testPhysicalFire()">Pulse Solenoid + LEDs</button>
        <button class="secondary" onclick="test('chargeStart')">Test Charge Start</button>
        <button class="secondary" onclick="test('cooldown')">Test Cooldown Return</button>
        <button class="secondary" onclick="test('chargeStop')">Stop Charge</button>
        <button class="danger" onclick="test('resetShots')">Reset Shot Count</button>
      </div>
      <p class="note" id="solenoidPinNote">Physical solenoid pulse is disabled unless SOLENOID_TEST_PIN is set in the sketch.</p>
    </div>

    <div class="section">
      <h2>Solenoid Firing</h2>
      <div class="grid">
        <div><div class="label">Firing animation</div><select id="fireMode" onchange="setParam('fireMode',this.value)"><option value="0">Forward Wave</option><option value="1">Dual Rail Shot</option><option value="2">Muzzle Burst</option><option value="3">Recoil Shock</option><option value="4">Rainbow Launch</option><option value="5">Sparkle Blast</option></select></div>
        <div><div class="label">Main firing color</div><input id="fireColor" type="color" onchange="setParam('fireColor',this.value)"></div>
      </div>
      <div class="btnrow" style="margin-top:12px"><button style="width:100%" onclick="testFire()">Test Fire / Add Shot</button></div>
      <div class="grid3" style="margin-top:12px">
        <div><div class="row"><span class="label">Trigger speed</span><span class="value" id="fireSpeedVal"></span></div><input id="fireSpeed" type="range" min="1" max="100" oninput="rangeSet(this,'fireSpeed')"></div>
        <div><div class="row"><span class="label">Pixels included</span><span class="value" id="firePixelsVal"></span></div><input id="firePixels" type="range" min="1" max="18" oninput="rangeSet(this,'firePixels')"></div>
        <div><div class="row"><span class="label">Fade strength</span><span class="value" id="fireFadeVal"></span></div><input id="fireFade" type="range" min="0" max="255" oninput="rangeSet(this,'fireFade')"></div>
      </div>
      <div class="row"><span class="label">Firing brightness</span><span class="value" id="fireBrightnessVal"></span></div><input id="fireBrightness" type="range" min="1" max="255" oninput="rangeSet(this,'fireBrightness')">
      <p class="note">Firing now uses one main color only. Fade strength controls how much of that same color trails behind the moving head.</p>
    </div>

    <div class="section">
      <h2>10 Second Countdown</h2>
      <div class="grid">
        <div><div class="label">Countdown animation</div><select id="countdownMode" onchange="setParam('countdownMode',this.value)"><option value="0">U Progress Drain</option><option value="1">Stadium Pulse</option><option value="2">Traffic Build</option><option value="3">Comet Tick</option><option value="4">Side Launch</option></select></div>
        <div><div class="row"><span class="label">Seconds</span><span class="value" id="countdownSecondsVal"></span></div><input id="countdownSeconds" type="range" min="3" max="15" oninput="rangeSet(this,'countdownSeconds')"></div>
      </div>
      <div class="grid" style="margin-top:12px">
        <div><div class="label">Countdown color A</div><input id="countdownColorA" type="color" onchange="setParam('countdownColorA',this.value)"></div>
        <div><div class="label">Countdown color B</div><input id="countdownColorB" type="color" onchange="setParam('countdownColorB',this.value)"></div>
      </div>
      <div class="grid" style="margin-top:12px">
        <div><div class="row"><span class="label">Speed/detail</span><span class="value" id="countdownSpeedVal"></span></div><input id="countdownSpeed" type="range" min="1" max="100" oninput="rangeSet(this,'countdownSpeed')"></div>
        <div><div class="row"><span class="label">Brightness</span><span class="value" id="countdownBrightnessVal"></span></div><input id="countdownBrightness" type="range" min="1" max="255" oninput="rangeSet(this,'countdownBrightness')"></div>
      </div>
      <div class="btnrow" style="margin-top:12px"><button class="secondary" onclick="test('countdown')">Test Countdown</button><button class="secondary" onclick="test('stopCountdown')">Stop Countdown</button></div>
    </div>

    <div class="section">
      <h2>Save and Recipe</h2>
      <div class="btnrow"><button class="good" onclick="saveSettings()">Save to ESP32</button><button class="secondary" onclick="loadRecipe()">Generate Recipe</button><button class="danger" onclick="factoryReset()">Factory Reset</button></div>
      <textarea id="recipe" class="recipe" placeholder="Click Generate Recipe after tuning."></textarea>
    </div>
  </section>
</main>
<script>
let lastStatus=null;
const ledBase=[
 [0,18,6],[1,18,17],[2,18,28],[3,18,39],[4,18,50],[5,18,61],[6,18,72],[7,18,83],
 [8,38,91],[9,62,91],
 [10,82,83],[11,82,72],[12,82,61],[13,82,50],[14,82,39],[15,82,28],[16,82,17],[17,82,6]
];
const sliders=['globalBrightness','disconnectedSpeed','disconnectedBrightness','teamBreathSpeed','teamBreathMinBrightness','teamBreathBrightness','chargeSpeed','chargePixels','chargeBrightness','maxBurstShots','burstWindowMs','cooldownMs','cooldownBrightness','fireSpeed','firePixels','fireFade','fireBrightness','countdownSeconds','countdownSpeed','countdownBrightness','patternBrightness'];
function qs(id){return document.getElementById(id)}
function hex(c){return '#'+c}
function rangeSet(el,name){qs(name+'Val').textContent=el.value; setParam(name,el.value)}
async function setParam(name,val){ await fetch('/api/set?'+encodeURIComponent(name)+'='+encodeURIComponent(val)); }
async function test(what){ await fetch('/api/test?what='+encodeURIComponent(what)); refresh(); }
async function sleep(ms){ return new Promise(resolve=>setTimeout(resolve,ms)); }
async function testFire(){ await fetch('/api/test?what=fire'); refresh(); }
async function testPhysicalFire(){ await fetch('/api/test?what=physicalFire'); refresh(); }
async function testBurst5(){
  await fetch('/api/test?what=resetShots');
  await sleep(120);
  for(let i=0;i<5;i++){
    await fetch('/api/test?what=fire');
    await refresh();
    await sleep(340);
  }
  await refresh();
}
async function saveSettings(){ await fetch('/api/save'); alert('Saved to ESP32 memory.'); }
async function factoryReset(){ if(confirm('Factory reset all Mohawk LED settings?')){ await fetch('/api/factory'); refresh(); } }
async function loadRecipe(){ const r=await fetch('/api/recipe'); qs('recipe').value=await r.text(); }
function drawRobot(s){
  const robot=qs('robot'); robot.innerHTML='';
  const flip=s?s.flipUi:false;
  for(const [led,x,y] of ledBase){
    const d=document.createElement('div'); d.className='led'; d.textContent=led;
    const px=(flip?(100-x):x); d.style.left=`calc(${px}% - 23px)`; d.style.top=`calc(${y}% - 23px)`;
    if(s && s.pattern && s.pattern[led]) d.style.background=s.pattern[led];
    d.onclick=()=>paintLed(led,d);
    d.oncontextmenu=(e)=>{e.preventDefault(); fetch('/api/test?what=identify&led='+led);};
    robot.appendChild(d);
  }
}
async function paintLed(led,el){
  const color=qs('brush').value;
  el.style.background=color;
  await fetch('/api/pattern?led='+led+'&color='+encodeURIComponent(color));
}
async function fillPattern(){ await fetch('/api/patternFill?color='+encodeURIComponent(qs('brush').value)); refresh(); }
async function clearPattern(){ await fetch('/api/patternFill?color=%23000000'); refresh(); }
async function randomPattern(){ await fetch('/api/patternRandom'); refresh(); }
async function previewPattern(){ await fetch('/api/test?what=pattern'); refresh(); }
function applyStatus(s){
  lastStatus=s;
  qs('stState').textContent=s.connected?'Connected':'Disconnected';
  qs('stShots').textContent=s.totalShots;
  qs('stBurst').textContent=s.burstShots+'/'+s.maxBurstShots;
  qs('stCool').textContent=(s.cooldownRemainingMs/1000).toFixed(1)+'s';
  const pinNote=qs('solenoidPinNote');
  if(pinNote){
    pinNote.textContent = s.solenoidTestEnabled
      ? ('Physical solenoid test is ENABLED on GPIO '+s.solenoidTestPin+' with '+s.solenoidPulseMs+' ms pulse. Keep hands clear before pressing Pulse Solenoid + LEDs.')
      : 'Physical solenoid pulse is disabled. Set SOLENOID_TEST_PIN near the top of the sketch to your MOSFET GPIO. Simulate One Solenoid Fire still tests the LED shot meter.';
  }
  qs('team').value=s.team;
  qs('teamBlue').value=hex(s.teamBlue); qs('teamOrange').value=hex(s.teamOrange);
  qs('chargeMode').value=s.chargeMode; qs('fireMode').value=s.fireMode; qs('countdownMode').value=s.countdownMode;
  qs('chargeColor').value=hex(s.chargeColor); qs('fireColor').value=hex(s.fireColor);
  qs('countdownColorA').value=hex(s.countdownColorA); qs('countdownColorB').value=hex(s.countdownColorB);
  if(s.shotPairColors){ for(let i=0;i<s.shotPairColors.length;i++){ if(qs('shotColor'+i)) qs('shotColor'+i).value=hex(s.shotPairColors[i]); } }
  qs('useSpatial').checked=!!s.useSpatial; qs('flipUi').checked=!!s.flipUi; qs('teamBreathDiagonal').checked=!!s.teamBreathDiagonal; qs('teamBreathRotate').checked=!!s.teamBreathRotate; qs('patternAsIdle').checked=!!s.patternAsIdle;
  for(const k of sliders){ if(qs(k)){ qs(k).value=s[k]; if(qs(k+'Val')) qs(k+'Val').textContent=s[k]; } }
  drawRobot(s);
}
async function refresh(){ try{ const r=await fetch('/status'); applyStatus(await r.json()); }catch(e){} }
setInterval(refresh,900); refresh();
</script>
</body>
</html>
)rawliteral";

static void sendOk(const String &msg = "ok")
{
  server.send(200, "text/plain", msg);
}

static void sendBad(const String &msg)
{
  server.send(400, "text/plain", msg);
}

static String jsonBool(bool v)
{
  return v ? "true" : "false";
}

static void jsonPairBool(String &s, const char *name, bool value, bool comma = true)
{
  s += "\"";
  s += name;
  s += "\":";
  s += jsonBool(value);
  if (comma) s += ",";
}

static void jsonPairInt(String &s, const char *name, uint32_t value, bool comma = true)
{
  s += "\"";
  s += name;
  s += "\":";
  s += String(value);
  if (comma) s += ",";
}

static void jsonPairColor(String &s, const char *name, CRGB value, bool comma = true)
{
  s += "\"";
  s += name;
  s += "\":\"";
  s += hexString(value);
  s += "\"";
  if (comma) s += ",";
}

static String statusJson()
{
  uint32_t now = millis();
  uint32_t remainingCooldown = (now < gCooldownUntilMs) ? (gCooldownUntilMs - now) : 0;

  String s;
  s.reserve(2600);
  s += "{";
  jsonPairBool(s, "connected", gConnectionState == ROBOT_CONNECTED);
  jsonPairBool(s, "lowBattery", gLowBattery);
  jsonPairBool(s, "useSpatial", gUseSpatialMapping);
  jsonPairBool(s, "flipUi", gFlipUi);
  jsonPairBool(s, "patternAsIdle", gPatternAsIdle);
  jsonPairInt(s, "team", (uint8_t)gTeam);
  jsonPairColor(s, "teamBlue", gTeamBlue);
  jsonPairColor(s, "teamOrange", gTeamOrange);
  jsonPairInt(s, "globalBrightness", gGlobalBrightness);
  jsonPairInt(s, "disconnectedSpeed", gDisconnectedSpeed);
  jsonPairInt(s, "disconnectedBrightness", gDisconnectedBrightness);
  jsonPairInt(s, "teamBreathSpeed", gTeamBreathSpeed);
  jsonPairInt(s, "teamBreathMinBrightness", gTeamBreathMinBrightness);
  jsonPairInt(s, "teamBreathBrightness", gTeamBreathBrightness);
  jsonPairBool(s, "teamBreathRotate", gTeamBreathRotate);
  jsonPairBool(s, "teamBreathDiagonal", gTeamBreathDiagonal);
  jsonPairInt(s, "chargeMode", (uint8_t)gChargeMode);
  jsonPairInt(s, "chargeSpeed", gChargeSpeed);
  jsonPairInt(s, "chargePixels", gChargePixels);
  jsonPairInt(s, "chargeBrightness", gChargeBrightness);
  jsonPairColor(s, "chargeColor", gChargeColor);
  jsonPairInt(s, "fireMode", (uint8_t)gFireMode);
  jsonPairInt(s, "fireSpeed", gFireSpeed);
  jsonPairInt(s, "firePixels", gFirePixels);
  jsonPairInt(s, "fireFade", gFireFade);
  jsonPairInt(s, "fireBrightness", gFireBrightness);
  jsonPairColor(s, "fireColor", gFireColor);
  jsonPairInt(s, "maxBurstShots", gMaxBurstShots);
  jsonPairInt(s, "burstWindowMs", gBurstWindowMs);
  jsonPairInt(s, "cooldownRemainingMs", remainingCooldown);
  jsonPairBool(s, "solenoidTestEnabled", physicalSolenoidTestEnabled());
  jsonPairInt(s, "solenoidTestPin", (int32_t)SOLENOID_TEST_PIN);
  jsonPairInt(s, "solenoidPulseMs", (uint32_t)SOLENOID_TEST_PULSE_MS);
  jsonPairInt(s, "cooldownMs", gCooldownMs);
  jsonPairInt(s, "cooldownBrightness", gCooldownBrightness);
  s += "\"shotPairColors\":[";
  for (uint8_t p = 0; p < SHOT_PAIR_COUNT; p++)
  {
    if (p) s += ",";
    s += "\"";
    s += hexString(gShotPairColors[p]);
    s += "\"";
  }
  s += "],";
  jsonPairInt(s, "countdownMode", (uint8_t)gCountdownMode);
  jsonPairInt(s, "countdownSeconds", gCountdownSeconds);
  jsonPairInt(s, "countdownSpeed", gCountdownSpeed);
  jsonPairInt(s, "countdownBrightness", gCountdownBrightness);
  jsonPairColor(s, "countdownColorA", gCountdownColorA);
  jsonPairColor(s, "countdownColorB", gCountdownColorB);
  jsonPairInt(s, "patternBrightness", gPatternBrightness);
  jsonPairInt(s, "patternPreviewSeconds", gPatternPreviewSeconds);
  jsonPairInt(s, "totalShots", gTotalShotsFired);
  jsonPairInt(s, "burstShots", gBurstShotCount);
  s += "\"pattern\":[";
  for (int i = 0; i < MOHAWK_LED_COUNT; i++)
  {
    if (i) s += ",";
    s += "\"#";
    s += hexString(patternColors[i]);
    s += "\"";
  }
  s += "]}";
  return s;
}

static void handleRoot()
{
  server.send_P(200, "text/html", INDEX_HTML);
}

static void handleStatus()
{
  server.send(200, "application/json", statusJson());
}

static void handleSet()
{
  for (uint8_t i = 0; i < server.args(); i++)
  {
    String n = server.argName(i);
    String v = server.arg(i);

    if (n == "team") gTeam = (TeamColorMode)constrain(v.toInt(), 0, 1);
    else if (n == "teamBlue") gTeamBlue = hexToCRGB(v, gTeamBlue);
    else if (n == "teamOrange") gTeamOrange = hexToCRGB(v, gTeamOrange);
    else if (n == "globalBrightness") gGlobalBrightness = clampU8(v.toInt(), 1, 255);
    else if (n == "useSpatial") gUseSpatialMapping = v.toInt() != 0;
    else if (n == "flipUi") gFlipUi = v.toInt() != 0;

    else if (n == "disconnectedSpeed") gDisconnectedSpeed = clampU8(v.toInt(), 1, 100);
    else if (n == "disconnectedBrightness") gDisconnectedBrightness = clampU8(v.toInt(), 1, 255);

    else if (n == "teamBreathSpeed") gTeamBreathSpeed = clampU8(v.toInt(), 1, 100);
    else if (n == "teamBreathMinBrightness")
    {
      gTeamBreathMinBrightness = clampU8(v.toInt(), 0, 254);
      if (gTeamBreathMinBrightness >= gTeamBreathBrightness) gTeamBreathBrightness = min((uint8_t)255, (uint8_t)(gTeamBreathMinBrightness + 1));
    }
    else if (n == "teamBreathBrightness")
    {
      gTeamBreathBrightness = clampU8(v.toInt(), 1, 255);
      if (gTeamBreathBrightness <= gTeamBreathMinBrightness) gTeamBreathMinBrightness = max((uint8_t)0, (uint8_t)(gTeamBreathBrightness - 1));
    }
    else if (n == "teamBreathRotate") gTeamBreathRotate = v.toInt() != 0;
    else if (n == "teamBreathDiagonal") gTeamBreathDiagonal = v.toInt() != 0;

    else if (n == "chargeMode") gChargeMode = (ChargeAnimationMode)constrain(v.toInt(), 0, CHARGE_ANIMATION_COUNT - 1);
    else if (n == "chargeSpeed") gChargeSpeed = clampU8(v.toInt(), 1, 100);
    else if (n == "chargePixels") gChargePixels = clampU8(v.toInt(), 1, MOHAWK_LED_COUNT);
    else if (n == "chargeBrightness") gChargeBrightness = clampU8(v.toInt(), 1, 255);
    else if (n == "chargeColor") gChargeColor = hexToCRGB(v, gChargeColor);

    else if (n == "fireMode") gFireMode = (FireAnimationMode)constrain(v.toInt(), 0, FIRE_ANIMATION_COUNT - 1);
    else if (n == "fireSpeed") gFireSpeed = clampU8(v.toInt(), 1, 100);
    else if (n == "firePixels") gFirePixels = clampU8(v.toInt(), 1, MOHAWK_LED_COUNT);
    else if (n == "fireFade") gFireFade = clampU8(v.toInt(), 0, 255);
    else if (n == "fireBrightness") gFireBrightness = clampU8(v.toInt(), 1, 255);
    else if (n == "fireColor") gFireColor = hexToCRGB(v, gFireColor);

    else if (n == "maxBurstShots") gMaxBurstShots = clampU8(v.toInt(), 1, 5);
    else if (n == "burstWindowMs") gBurstWindowMs = clampU16(v.toInt(), 500, 6000);
    else if (n == "cooldownMs") gCooldownMs = clampU16(v.toInt(), 500, 5000);
    else if (n == "cooldownBrightness") gCooldownBrightness = clampU8(v.toInt(), 1, 255);
    else if (n.startsWith("shotColor"))
    {
      int idx = n.substring(9).toInt();
      if (idx >= 0 && idx < SHOT_PAIR_COUNT) gShotPairColors[idx] = hexToCRGB(v, gShotPairColors[idx]);
    }

    else if (n == "countdownMode") gCountdownMode = (CountdownAnimationMode)constrain(v.toInt(), 0, COUNTDOWN_ANIMATION_COUNT - 1);
    else if (n == "countdownSeconds") gCountdownSeconds = clampU8(v.toInt(), 3, 15);
    else if (n == "countdownSpeed") gCountdownSpeed = clampU8(v.toInt(), 1, 100);
    else if (n == "countdownBrightness") gCountdownBrightness = clampU8(v.toInt(), 1, 255);
    else if (n == "countdownColorA") gCountdownColorA = hexToCRGB(v, gCountdownColorA);
    else if (n == "countdownColorB") gCountdownColorB = hexToCRGB(v, gCountdownColorB);

    else if (n == "patternAsIdle") gPatternAsIdle = v.toInt() != 0;
    else if (n == "patternBrightness") gPatternBrightness = clampU8(v.toInt(), 1, 255);
  }

  sendOk();
}

static void handleTest()
{
  String what = server.arg("what");

  if (what == "connected") mohawk_set_connected(true);
  else if (what == "disconnected") mohawk_set_connected(false);
  else if (what == "lowBattery") gLowBattery = !gLowBattery;
  else if (what == "lowBatteryOn") gLowBattery = true;
  else if (what == "chargeStart") mohawk_start_solenoid_charge();
  else if (what == "chargeStop") mohawk_stop_solenoid_charge();
  else if (what == "fire") mohawk_trigger_solenoid_animation();
  else if (what == "cooldown")
  {
    gBurstShotCount = gMaxBurstShots;
    gCooldownUntilMs = millis() + gCooldownMs;
    gShotMeterHoldUntilMs = gCooldownUntilMs;
  }
  else if (what == "physicalFire")
  {
    pulse_physical_solenoid_for_test();
    mohawk_trigger_solenoid_animation();
  }
  else if (what == "resetShots") mohawk_reset_shot_meter();
  else if (what == "countdown") mohawk_start_countdown(gCountdownSeconds);
  else if (what == "stopCountdown") mohawk_stop_countdown();
  else if (what == "pattern") gPatternPreviewUntilMs = millis() + ((uint32_t)gPatternPreviewSeconds * 1000UL);
  else if (what == "identify")
  {
    int led = server.arg("led").toInt();
    if (led >= 0 && led < MOHAWK_LED_COUNT)
    {
      gIdentifyLed = led;
      gIdentifyUntilMs = millis() + 1400;
    }
  }

  sendOk();
}

static void handlePattern()
{
  int led = server.arg("led").toInt();
  if (led < 0 || led >= MOHAWK_LED_COUNT)
  {
    sendBad("invalid led");
    return;
  }
  patternColors[led] = hexToCRGB(server.arg("color"), patternColors[led]);
  sendOk();
}

static void handlePatternFill()
{
  CRGB c = hexToCRGB(server.arg("color"), CRGB::Black);
  for (int i = 0; i < MOHAWK_LED_COUNT; i++) patternColors[i] = c;
  sendOk();
}

static void handlePatternRandom()
{
  for (int i = 0; i < MOHAWK_LED_COUNT; i++)
  {
    if (random8() < 110) patternColors[i] = CHSV(random8(), 210, 255);
    else patternColors[i] = CRGB::Black;
  }
  sendOk();
}

static void recipeLineU8(String &r, const char *name, uint32_t value)
{
  r += "static const uint8_t  ";
  r += name;
  r += " = ";
  r += String(value);
  r += ";\n";
}

static void recipeLineU16(String &r, const char *name, uint32_t value)
{
  r += "static const uint16_t ";
  r += name;
  r += " = ";
  r += String(value);
  r += ";\n";
}

static void recipeLineColor(String &r, const char *name, CRGB color)
{
  r += "static const uint32_t ";
  r += name;
  r += " = 0x";
  r += hexString(color);
  r += ";\n";
}

static String generateRecipe()
{
  String r;
  r.reserve(4200);
  r += "// Mohawk LED Controller v1.9.1 tuned recipe\n";
  recipeLineU8(r, "DEFAULT_GLOBAL_BRIGHTNESS", gGlobalBrightness);
  recipeLineU8(r, "DEFAULT_CONNECTED", (uint8_t)gConnectionState);
  recipeLineU8(r, "DEFAULT_LOW_BATTERY", (uint8_t)gLowBattery);
  recipeLineU8(r, "DEFAULT_TEAM", (uint8_t)gTeam);
  recipeLineU8(r, "DEFAULT_USE_SPATIAL", (uint8_t)gUseSpatialMapping);
  recipeLineU8(r, "DEFAULT_FLIP_UI", (uint8_t)gFlipUi);
  recipeLineColor(r, "DEFAULT_TEAM_BLUE", gTeamBlue);
  recipeLineColor(r, "DEFAULT_TEAM_ORANGE", gTeamOrange);
  recipeLineU8(r, "DEFAULT_DISCONNECTED_SPEED", gDisconnectedSpeed);
  recipeLineU8(r, "DEFAULT_DISCONNECTED_BRIGHTNESS", gDisconnectedBrightness);
  recipeLineU8(r, "DEFAULT_TEAM_BREATH_SPEED", gTeamBreathSpeed);
  recipeLineU8(r, "DEFAULT_TEAM_BREATH_MIN_BRIGHTNESS", gTeamBreathMinBrightness);
  recipeLineU8(r, "DEFAULT_TEAM_BREATH_BRIGHTNESS", gTeamBreathBrightness);
  recipeLineU8(r, "DEFAULT_TEAM_BREATH_ROTATE", (uint8_t)gTeamBreathRotate);
  recipeLineU8(r, "DEFAULT_TEAM_BREATH_DIAGONAL", (uint8_t)gTeamBreathDiagonal);
  recipeLineU8(r, "DEFAULT_CHARGE_MODE", (uint8_t)gChargeMode);
  recipeLineU8(r, "DEFAULT_CHARGE_SPEED", gChargeSpeed);
  recipeLineU8(r, "DEFAULT_CHARGE_PIXELS", gChargePixels);
  recipeLineU8(r, "DEFAULT_CHARGE_BRIGHTNESS", gChargeBrightness);
  recipeLineColor(r, "DEFAULT_CHARGE_COLOR", gChargeColor);
  recipeLineU8(r, "DEFAULT_FIRE_MODE", (uint8_t)gFireMode);
  recipeLineU8(r, "DEFAULT_FIRE_SPEED", gFireSpeed);
  recipeLineU8(r, "DEFAULT_FIRE_PIXELS", gFirePixels);
  recipeLineU8(r, "DEFAULT_FIRE_FADE", gFireFade);
  recipeLineU8(r, "DEFAULT_FIRE_BRIGHTNESS", gFireBrightness);
  recipeLineColor(r, "DEFAULT_FIRE_COLOR", gFireColor);
  recipeLineU8(r, "DEFAULT_MAX_BURST_SHOTS", gMaxBurstShots);
  recipeLineU16(r, "DEFAULT_BURST_WINDOW_MS", gBurstWindowMs);
  recipeLineU16(r, "DEFAULT_COOLDOWN_MS", gCooldownMs);
  recipeLineU8(r, "DEFAULT_COOLDOWN_BRIGHTNESS", gCooldownBrightness);
  r += "static const uint32_t DEFAULT_SHOT_PAIR_COLORS[SHOT_PAIR_COUNT] = {\n  ";
  for (uint8_t p = 0; p < SHOT_PAIR_COUNT; p++)
  {
    if (p) r += ", ";
    r += "0x";
    r += hexString(gShotPairColors[p]);
  }
  r += "\n};\n";
  recipeLineU8(r, "DEFAULT_COUNTDOWN_MODE", (uint8_t)gCountdownMode);
  recipeLineU8(r, "DEFAULT_COUNTDOWN_SECONDS", gCountdownSeconds);
  recipeLineU8(r, "DEFAULT_COUNTDOWN_SPEED", gCountdownSpeed);
  recipeLineU8(r, "DEFAULT_COUNTDOWN_BRIGHTNESS", gCountdownBrightness);
  recipeLineColor(r, "DEFAULT_COUNTDOWN_COLOR_A", gCountdownColorA);
  recipeLineColor(r, "DEFAULT_COUNTDOWN_COLOR_B", gCountdownColorB);
  recipeLineU8(r, "DEFAULT_PATTERN_AS_IDLE", (uint8_t)gPatternAsIdle);
  recipeLineU8(r, "DEFAULT_PATTERN_BRIGHTNESS", gPatternBrightness);
  r += "static const uint32_t DEFAULT_PATTERN_COLORS[MOHAWK_LED_COUNT] = {\n  ";
  for (int i = 0; i < MOHAWK_LED_COUNT; i++)
  {
    if (i) r += ", ";
    if (i == 9) r += "\n  ";
    r += "0x";
    r += hexString(patternColors[i]);
  }
  r += "\n};\n";
  return r;
}

static void handleRecipe()
{
  server.send(200, "text/plain", generateRecipe());
}

static void handleSave()
{
  saveSettings();
  sendOk("saved");
}

static void handleFactory()
{
  setFactoryDefaults(true);
  sendOk("factory reset");
}

static void setupWebUi()
{
  WiFi.mode(WIFI_AP);
  WiFi.softAP(MOHAWK_AP_SSID, MOHAWK_AP_PASSWORD);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/api/set", HTTP_GET, handleSet);
  server.on("/api/test", HTTP_GET, handleTest);
  server.on("/api/pattern", HTTP_GET, handlePattern);
  server.on("/api/patternFill", HTTP_GET, handlePatternFill);
  server.on("/api/patternRandom", HTTP_GET, handlePatternRandom);
  server.on("/api/save", HTTP_GET, handleSave);
  server.on("/api/factory", HTTP_GET, handleFactory);
  server.on("/api/recipe", HTTP_GET, handleRecipe);

  server.begin();
}

// ============================================================
// Setup and loop
// ============================================================

void setup_mohawk()
{
  pinMode(MOHAWK_PIN, OUTPUT);
  FastLED.addLeds<CHIPSET, MOHAWK_PIN, COLOR_ORDER>(mohawk_leds, MOHAWK_LED_COUNT);
  FastLED.setBrightness(gGlobalBrightness);
  FastLED.clear(true);
  buildXYMap();
}

void setup()
{
  Serial.begin(115200);
  delay(200);
  random16_add_entropy(analogRead(0));

  Serial.println();
  Serial.println("Mohawk LED Controller v1.9.1 starting...");

#if SOLENOID_TEST_PIN >= 0
  pinMode(SOLENOID_TEST_PIN, OUTPUT);
  digitalWrite(SOLENOID_TEST_PIN, SOLENOID_TEST_ACTIVE_HIGH ? LOW : HIGH);
  Serial.print("Physical solenoid test enabled on GPIO ");
  Serial.println(SOLENOID_TEST_PIN);
#else
  Serial.println("Physical solenoid test disabled. Set SOLENOID_TEST_PIN to enable web bench firing.");
#endif

  loadSettings();
  setup_mohawk();
  setupWebUi();

  Serial.print("AP SSID: ");
  Serial.println(MOHAWK_AP_SSID);
  Serial.print("AP Password: ");
  Serial.println(MOHAWK_AP_PASSWORD);
  Serial.print("Open: http://");
  Serial.println(WiFi.softAPIP());
}

void loop()
{
  server.handleClient();

  static uint32_t lastLedMs = 0;
  uint32_t now = millis();
  if (now - lastLedMs >= LED_UPDATE_MS)
  {
    lastLedMs = now;
    mohawk_tick();
  }
}
