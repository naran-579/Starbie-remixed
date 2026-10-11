/*
  Starbie: a tiny motion-controlled digital pet

  This is intentionally one file. Open this .ino file in Arduino IDE, edit
  the BEGINNER SETTINGS section, and upload it to a Seeed XIAO ESP32-C3.

  Install these libraries with Sketch > Include Library > Manage Libraries:
    - Adafruit GFX Library
    - Adafruit SSD1306
    - Adafruit MPU6050
    - DHT sensor library
  Arduino IDE will offer to install their dependencies too. Click Install All.
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_MPU6050.h>
#include <DHT.h>
#include <Preferences.h>
#include <math.h>

// =========================== BEGINNER SETTINGS ===========================
// Everything most builders will want to customize is in this one section.

// --- Your board's pins ----------------------------------------------------
// These are ESP32-C3 GPIO numbers, with the XIAO pin labels beside them.
const int I2C_SDA_PIN = 6;       // XIAO D4: OLED + MPU6050 SDA
const int I2C_SCL_PIN = 7;       // XIAO D5: OLED + MPU6050 SCL
const int DHT_PIN = 3;           // XIAO D1: DHT11 data
const int BUTTON_ONE_PIN = 4;    // XIAO D2: opens/confirms the radial menu
const int BUTTON_TWO_PIN = 5;    // XIAO D3: shows/hides stats
const int BATTERY_PIN = 2;       // XIAO D0: middle of the battery divider
const int LED_PIN = 10;          // XIAO D10: single-color status LED

// Set this to false if your submission does not have a DHT11.
const bool USE_DHT11 = true;

// Most parts use these addresses. Only change them if an I2C scanner says
// your part is different (common OLED alternative: 0x3D).
const uint8_t OLED_ADDRESS = 0x3C;
const uint8_t MPU6050_ADDRESS = 0x68;

// --- Battery (3 AA cells through the 100k / 47k divider) ------------------
// Set this to false if your build does not have the battery divider.
const bool USE_BATTERY_METER = true;

// The divider cuts the pack voltage to about a third, so the code multiplies
// it back up: (100k + 47k) / 47k = 3.13. Fine-tune it with a multimeter:
// new ratio = old ratio * (multimeter volts / volts shown in Serial Monitor).
const float BATTERY_DIVIDER_RATIO = 3.13f;

// Pack voltage limits. A reading below the first value means there are no
// batteries and the board is running from USB, so the battery icon is hidden.
const float BATTERY_NO_BATTERY_V = 1.5f;
const float BATTERY_LOW_V = 3.9f;        // pet gets drowsy and the icon blinks
const float BATTERY_CRITICAL_V = 3.75f;  // save once, then show the warning screen
const float BATTERY_RECOVERY_V = 0.10f;  // volts it must rise to leave critical
const uint32_t WARNING_SNOOZE_MS = 60000;  // a button press hides the warning this long

// Rough pack voltage to percent curve for 3 alkaline AAs. Change the numbers
// if you use another battery type. Keep both lists in the same order.
const float BATTERY_CURVE_VOLTS[] = {3.60f, 3.75f, 3.90f, 4.20f, 4.50f};
const int BATTERY_CURVE_PERCENT[] = {0, 10, 30, 70, 100};

const uint16_t BATTERY_READ_INTERVAL_MS = 5000;

// --- Status LED (one single-color LED) ------------------------------------
// Wiring: LED_PIN -> resistor -> LED long leg (anode), LED short leg -> GND.
// Set this to false if your build does not have the LED.
const bool USE_LED = true;

// --- Temperature reactions (needs the DHT11) ------------------------------
const bool USE_TEMPERATURE_REACTIONS = true;
const float HOT_TEMP_C = 28.0f;   // at or above this: sweat drops, triple LED blink
const float COLD_TEMP_C = 18.0f;  // at or below this: shivering "brr", stuttering LED

// --- Screen sleep (saves battery) -----------------------------------------
// The screen dims, then turns off, when nothing has happened for a while. Any
// button press or shake wakes it. The press that wakes a fully sleeping
// screen does nothing else, so you will not open the menu by accident.
const bool USE_IDLE_SLEEP = true;
const uint32_t IDLE_DIM_AFTER_MS = 60000;     // 1 minute: dim the screen
const uint32_t IDLE_SLEEP_AFTER_MS = 180000;  // 3 minutes: screen off

// --- Starting stats -------------------------------------------------------
// Stats are 0 to 100. They only change when you select an action or shake
// Starbie; nothing slowly drains while it sits on your desk.
const int STARTING_JOY = 70;
const int STARTING_ENERGY = 75;
const int STARTING_FULLNESS = 65;

// Set true, upload once, then set it back to false if you want a fresh pet.
const bool RESET_SAVED_PET_ON_BOOT = false;

// --- Radial menu ----------------------------------------------------------
// These four actions appear at TOP, RIGHT, BOTTOM, then LEFT in the menu.
// The last word controls the matching little visual reaction.
enum PetReaction {
  NAP_REACTION,
  JUMP_REACTION,
  HEART_REACTION,
  RUN_REACTION,
};

struct MenuItem {
  const char *label;
  int joyChange;
  int energyChange;
  int fullnessChange;
  PetReaction reaction;
};

const MenuItem MENU_ITEMS[] = {
  {"NAP",   1,  18, -4, NAP_REACTION},   // top: sleep and emit Zs
  {"PLAY", 12, -9, -5, RUN_REACTION},    // right: two fast laps + hearts
  {"FEED",  3,  2,  18, JUMP_REACTION},  // bottom: wiggle and jump
  {"PET",   7,  0,  0, HEART_REACTION},  // left: jump and emit hearts
};
const int MENU_ITEM_COUNT = sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]);

// --- Movement feel --------------------------------------------------------
// Bigger MENU_TILT_LIMIT makes the menu ball move less.
const float MENU_TILT_LIMIT = 6.0f;

// MPU6050 orientation: change these settings instead of rewiring your board.
// 1. If the sensor is turned 90 degrees, set SWAP_MPU_AXES to true.
// 2. If a direction feels backwards, change that direction from 1.0f to -1.0f.
const bool SWAP_MPU_AXES = false;
const float MENU_X_DIRECTION = 1.0f;
const float MENU_Y_DIRECTION = -1.0f;

// A small dead zone makes the ball rest in the center until the board is tilted.
const float MENU_CENTER_DEADZONE = 0.8f;
const float SHAKE_THRESHOLD = 7.0f;

// Shake is the only movement that has an effect outside the radial menu.
const int SHAKE_JOY_CHANGE = 5;
const int SHAKE_ENERGY_CHANGE = -2;
const int SHAKE_FULLNESS_CHANGE = -1;

// --- Your pet's bitmap and animation -------------------------------------
// This is a simplified 32 by 32, one-bit version of the supplied character.
// You can replace it with your own art later; keep these size values matched
// to the sprite array you paste in.
const int PET_SPRITE_WIDTH = 32;
const int PET_SPRITE_HEIGHT = 32;

// The pet wanders unless it is asleep. A non-nap action does a tiny shake,
// then a jump, except PLAY which runs two fast laps.
const uint16_t PET_WALK_PIXEL_MS = 70;
const uint16_t PET_PRE_JUMP_MS = 230;
const uint16_t PET_JUMP_MS = 430;
const int PET_JUMP_HEIGHT = 16;
const uint32_t NAP_DURATION_MS = 48000;  // 48 seconds: four times the old nap.
const uint16_t HEARTS_DURATION_MS = 1600;
const uint16_t PLAY_LAP_MS = 800;
const uint8_t PLAY_LAP_COUNT = 2;

// Each byte stores eight pixels, left to right. This sparse outline keeps the
// supplied creature readable on a tiny, one-bit OLED: head, eye, legs, tail,
// and a small flower-like ear accent all stay separate instead of becoming a blob.
const uint8_t PROGMEM PET_SPRITE[] = {
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x70, 0x0a, 0x00,
  0x00, 0xf8, 0x1f, 0x00,
  0x00, 0x8c, 0x3f, 0x80,
  0x00, 0x43, 0xff, 0x00,
  0x00, 0x43, 0xff, 0x00,
  0x00, 0x30, 0x7e, 0x00,
  0x00, 0x98, 0x01, 0x00,
  0x01, 0x98, 0x01, 0x80,
  0x03, 0x00, 0x10, 0xc0,
  0x03, 0x04, 0x00, 0xc0,
  0x01, 0x8b, 0x01, 0x80,
  0x01, 0xcb, 0x03, 0x80,
  0x03, 0xc0, 0x03, 0xc0,
  0x03, 0xc0, 0x03, 0xc0,
  0x01, 0xc0, 0x03, 0x80,
  0x01, 0x80, 0x01, 0x80,
  0x00, 0x60, 0x6c, 0x00,
  0x00, 0x3f, 0xfc, 0x00,
  0x00, 0x7f, 0xfe, 0x00,
  0x00, 0x70, 0x0e, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x70,
  0x00, 0x00, 0x00, 0x00,
};
// ===========================================================================
// You can read below without needing to understand every line. The rest of
// the sketch handles buttons, sensors, drawing, and saving automatically.

const int SCREEN_WIDTH = 128;
const int SCREEN_HEIGHT = 64;
const int MENU_CENTER_X = 64;
const int MENU_CENTER_Y = 32;
const int MENU_ITEM_X[] = {64, 108, 64, 20};
const int MENU_ITEM_Y[] = {12, 32, 53, 32};
const int MENU_BALL_X_RANGE = 37;
const int MENU_BALL_Y_RANGE = 22;
const uint16_t BUTTON_DEBOUNCE_MS = 30;
const uint16_t MPU_READ_INTERVAL_MS = 30;
const uint16_t DHT_READ_INTERVAL_MS = 2200;
const uint16_t SHAKE_COOLDOWN_MS = 650;
const float STANDARD_GRAVITY = 9.80665f;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Adafruit_MPU6050 mpu;
DHT dht(DHT_PIN, DHT11);
Preferences preferences;

struct PetState {
  int joy;
  int energy;
  int fullness;
};

struct ButtonState {
  int pin;
  bool stableState;
  bool lastRawState;
  uint32_t lastChangedAt;
};

enum View {
  PET_VIEW,
  MENU_VIEW,
  STATS_VIEW,
};

PetState pet = {STARTING_JOY, STARTING_ENERGY, STARTING_FULLNESS};
ButtonState buttonOne = {BUTTON_ONE_PIN, HIGH, HIGH, 0};
ButtonState buttonTwo = {BUTTON_TWO_PIN, HIGH, HIGH, 0};
View currentView = PET_VIEW;

bool mpuFound = false;
bool dhtFound = false;
float accelerationX = 0.0f;
float accelerationY = 0.0f;
float accelerationZ = STANDARD_GRAVITY;
float temperatureC = NAN;
float humidity = NAN;
float menuBallX = MENU_CENTER_X;
float menuBallY = MENU_CENTER_Y;
float menuCenterAccelerationX = 0.0f;
float menuCenterAccelerationY = 0.0f;
int selectedMenuItem = -1;
uint32_t lastMpuReadAt = 0;
uint32_t lastDhtReadAt = 0;
uint32_t lastShakeAt = 0;
uint32_t shakeAnimationEndsAt = 0;
uint32_t petJumpStartedAt = 0;
uint32_t nappingUntil = 0;
uint32_t heartAnimationEndsAt = 0;
uint32_t playRunStartedAt = 0;
int nappingPetX = 0;
float batteryVolts = NAN;
int batteryPercent = -1;
bool batteryPresent = false;
bool batteryLow = false;
bool batteryCritical = false;
uint32_t lastBatteryReadAt = 0;
uint32_t lastActivityAt = 0;
bool screenDimmed = false;
bool screenOff = false;
uint32_t warningSnoozedUntil = 0;
bool warningWasActive = false;

int petWalkingX(uint32_t now);
void noteActivity();

int keepInRange(int value, int smallest, int largest) {
  return constrain(value, smallest, largest);
}

void changePet(int joyChange, int energyChange, int fullnessChange) {
  pet.joy = keepInRange(pet.joy + joyChange, 0, 100);
  pet.energy = keepInRange(pet.energy + energyChange, 0, 100);
  pet.fullness = keepInRange(pet.fullness + fullnessChange, 0, 100);
}

void loadPet() {
  preferences.begin("starbie", false);
  if (RESET_SAVED_PET_ON_BOOT) {
    preferences.clear();
  }
  pet.joy = preferences.getInt("joy", STARTING_JOY);
  pet.energy = preferences.getInt("energy", STARTING_ENERGY);
  pet.fullness = preferences.getInt("full", STARTING_FULLNESS);
}

void writePet() {
  preferences.putInt("joy", pet.joy);
  preferences.putInt("energy", pet.energy);
  preferences.putInt("full", pet.fullness);
}

void savePet() {
  if (batteryCritical) {
    return;  // Once critical, only updateWarningMode() writes to flash.
  }
  writePet();
}

void setUpButton(ButtonState &button) {
  pinMode(button.pin, INPUT_PULLUP);
  button.stableState = digitalRead(button.pin);
  button.lastRawState = button.stableState;
  button.lastChangedAt = millis();
}

// Returns true exactly once for each press, even if the switch bounces.
bool wasPressed(ButtonState &button) {
  const bool rawState = digitalRead(button.pin);
  const uint32_t now = millis();

  if (rawState != button.lastRawState) {
    button.lastRawState = rawState;
    button.lastChangedAt = now;
  }

  if (rawState != button.stableState &&
      now - button.lastChangedAt >= BUTTON_DEBOUNCE_MS) {
    button.stableState = rawState;
    return button.stableState == LOW;
  }

  return false;
}

void updateMpu() {
  if (!mpuFound || millis() - lastMpuReadAt < MPU_READ_INTERVAL_MS) {
    return;
  }
  lastMpuReadAt = millis();

  sensors_event_t acceleration;
  sensors_event_t gyro;
  sensors_event_t sensorTemperature;
  mpu.getEvent(&acceleration, &gyro, &sensorTemperature);
  accelerationX = acceleration.acceleration.x;
  accelerationY = acceleration.acceleration.y;
  accelerationZ = acceleration.acceleration.z;
}

void updateDht() {
  if (!dhtFound || millis() - lastDhtReadAt < DHT_READ_INTERVAL_MS) {
    return;
  }
  lastDhtReadAt = millis();

  const float newHumidity = dht.readHumidity();
  const float newTemperatureC = dht.readTemperature();
  if (!isnan(newHumidity)) {
    humidity = newHumidity;
  }
  if (!isnan(newTemperatureC)) {
    temperatureC = newTemperatureC;
  }
}

// Averages 16 readings, then multiplies the divider back out to get the real
// battery pack voltage in volts.
float readBatteryVolts() {
  uint32_t totalMillivolts = 0;
  for (int sample = 0; sample < 16; sample++) {
    totalMillivolts += analogReadMilliVolts(BATTERY_PIN);
  }
  return (totalMillivolts / 16.0f) * BATTERY_DIVIDER_RATIO / 1000.0f;
}

int batteryPercentFromVolts(float volts) {
  const int pointCount = sizeof(BATTERY_CURVE_VOLTS) / sizeof(BATTERY_CURVE_VOLTS[0]);
  if (volts <= BATTERY_CURVE_VOLTS[0]) {
    return 0;
  }
  if (volts >= BATTERY_CURVE_VOLTS[pointCount - 1]) {
    return 100;
  }
  for (int point = 1; point < pointCount; point++) {
    if (volts <= BATTERY_CURVE_VOLTS[point]) {
      const float span = BATTERY_CURVE_VOLTS[point] - BATTERY_CURVE_VOLTS[point - 1];
      const float position = (volts - BATTERY_CURVE_VOLTS[point - 1]) / span;
      const int percentSpan = BATTERY_CURVE_PERCENT[point] - BATTERY_CURVE_PERCENT[point - 1];
      return BATTERY_CURVE_PERCENT[point - 1] + static_cast<int>(position * percentSpan);
    }
  }
  return 100;
}

void updateBattery(bool forceRead) {
  if (!USE_BATTERY_METER) {
    return;
  }
  if (!forceRead && millis() - lastBatteryReadAt < BATTERY_READ_INTERVAL_MS) {
    return;
  }
  lastBatteryReadAt = millis();

  const float newVolts = readBatteryVolts();
  // A little smoothing keeps the icon from flickering. The first reading, or
  // the first one after batteries are put in, is used as it is.
  if (isnan(batteryVolts) || !batteryPresent) {
    batteryVolts = newVolts;
  } else {
    batteryVolts = batteryVolts * 0.7f + newVolts * 0.3f;
  }

  batteryPresent = batteryVolts >= BATTERY_NO_BATTERY_V;
  batteryPercent = batteryPresent ? batteryPercentFromVolts(batteryVolts) : -1;
  batteryLow = batteryPresent && batteryVolts <= BATTERY_LOW_V;
  const bool wasCritical = batteryCritical;
  // Once critical, the voltage must rise a little before it counts as OK again.
  batteryCritical = batteryPresent &&
                    (batteryVolts <= BATTERY_CRITICAL_V ||
                     (wasCritical && batteryVolts <= BATTERY_CRITICAL_V + BATTERY_RECOVERY_V));
  if (batteryCritical && !wasCritical) {
    warningSnoozedUntil = 0;
    noteActivity();  // Wake the screen so the warning is seen.
  }

  // Handy for calibrating the divider ratio against a multimeter.
  if (Serial) {
    Serial.print("Battery: ");
    Serial.print(batteryVolts, 2);
    Serial.print(" V");
    if (batteryPresent) {
      Serial.print(" (");
      Serial.print(batteryPercent);
      Serial.print("%)");
    } else {
      Serial.print(" (no batteries, running from USB)");
    }
    Serial.println();
  }
}

// Any button press or shake counts as activity and wakes the screen.
void noteActivity() {
  lastActivityAt = millis();
  if (screenDimmed) {
    display.dim(false);
    screenDimmed = false;
  }
  if (screenOff) {
    display.ssd1306_command(SSD1306_DISPLAYON);
    screenOff = false;
  }
}

// Returns true if the screen was fully off. The press that wakes it should
// not also open the menu or change views.
bool wakeScreen() {
  const bool wasOff = screenOff;
  noteActivity();
  return wasOff;
}

void updateIdleSleep() {
  if (!USE_IDLE_SLEEP) {
    return;
  }
  const uint32_t idleFor = millis() - lastActivityAt;
  if (!screenOff && idleFor >= IDLE_SLEEP_AFTER_MS) {
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    screenOff = true;
  } else if (!screenOff && !screenDimmed && idleFor >= IDLE_DIM_AFTER_MS) {
    display.dim(true);
    screenDimmed = true;
  }
}

bool isWarningActive() {
  return batteryCritical && millis() >= warningSnoozedUntil;
}

// Saves the pet once each time the warning appears, including when it comes
// back after a snooze. Normal saving stays paused while the battery is critical.
void updateWarningMode() {
  const bool active = isWarningActive();
  if (active && !warningWasActive) {
    writePet();
  }
  warningWasActive = active;
}

bool isHot() {
  return USE_TEMPERATURE_REACTIONS && !isnan(temperatureC) && temperatureC >= HOT_TEMP_C;
}

bool isCold() {
  return USE_TEMPERATURE_REACTIONS && !isnan(temperatureC) && temperatureC <= COLD_TEMP_C;
}

int petWalkingX(uint32_t now) {
  const int farthestX = SCREEN_WIDTH - PET_SPRITE_WIDTH;
  const uint32_t roundTrip = static_cast<uint32_t>(farthestX) * 2;
  const uint32_t step = (now / PET_WALK_PIXEL_MS) % roundTrip;
  return step <= farthestX ? step : roundTrip - step;
}

uint32_t playRunDuration() {
  return static_cast<uint32_t>(PLAY_LAP_MS) * PLAY_LAP_COUNT;
}

bool isNapping() {
  return nappingUntil != 0 && millis() < nappingUntil;
}

bool isPlaying() {
  return playRunStartedAt != 0 && millis() - playRunStartedAt < playRunDuration();
}

int playRunX(uint32_t now) {
  const int farthestX = SCREEN_WIDTH - PET_SPRITE_WIDTH;
  const uint32_t lapAge = (now - playRunStartedAt) % PLAY_LAP_MS;
  const float progress = static_cast<float>(lapAge) / PLAY_LAP_MS;
  return progress < 0.5f ? static_cast<int>(progress * 2.0f * farthestX)
                         : static_cast<int>((1.0f - progress) * 2.0f * farthestX);
}

void updatePetTimers() {
  if (nappingUntil != 0 && !isNapping()) {
    nappingUntil = 0;  // The pet wakes naturally when its nap time is over.
  }
  if (playRunStartedAt != 0 && !isPlaying()) {
    playRunStartedAt = 0;  // PLAY is exactly two left-to-right-to-left laps.
  }
}

// True when `phase` falls inside a blink that starts at `start` and lasts
// `length` milliseconds.
bool inBlink(uint32_t phase, uint32_t start, uint32_t length) {
  return phase >= start && phase < start + length;
}

// One LED can only be on or off, so the pet's situation is shown as a blink
// pattern. The first match wins.
void updateLed() {
  if (!USE_LED) {
    return;
  }
  const uint32_t now = millis();
  bool ledOn = false;

  if (batteryCritical) {
    ledOn = (now / 125) % 2 == 0;  // rapid blinking, even with the screen off
  } else if (screenOff) {
    ledOn = false;
  } else if (batteryLow) {
    const uint32_t phase = now % 2000;  // two blinks every 2 seconds
    ledOn = inBlink(phase, 0, 100) || inBlink(phase, 250, 100);
  } else if (isNapping()) {
    ledOn = inBlink(now % 4000, 0, 80);  // one slow blink every 4 seconds
  } else if (now < heartAnimationEndsAt) {
    ledOn = true;  // solid while hearts float
  } else if (isHot()) {
    const uint32_t phase = now % 3000;  // three quick blinks every 3 seconds
    ledOn = inBlink(phase, 0, 80) || inBlink(phase, 160, 80) || inBlink(phase, 320, 80);
  } else if (isCold()) {
    const uint32_t phase = now % 3000;  // shivering: a quick stutter every 3 seconds
    ledOn = phase < 600 && (phase / 60) % 2 == 0;
  } else if (pet.joy >= 60) {
    ledOn = inBlink(now % 3000, 0, 80);  // happy: one blink every 3 seconds
  } else if (pet.joy >= 30) {
    ledOn = inBlink(now % 6000, 0, 80);  // okay: one blink every 6 seconds
  } else {
    ledOn = inBlink(now % 8000, 0, 400);  // sad: one long, slow glow
  }

  digitalWrite(LED_PIN, ledOn ? HIGH : LOW);
}

// While the menu is open, the MPU6050 acts like a tiny air mouse. The position
// at the instant Button 1 opens the menu becomes the center for that menu use.
void updateMenuBall() {
  float xTilt = accelerationX - menuCenterAccelerationX;
  float yTilt = accelerationY - menuCenterAccelerationY;

  if (SWAP_MPU_AXES) {
    const float oldXTilt = xTilt;
    xTilt = yTilt;
    yTilt = oldXTilt;
  }
  xTilt = constrain(xTilt * MENU_X_DIRECTION, -MENU_TILT_LIMIT, MENU_TILT_LIMIT);
  yTilt = constrain(yTilt * MENU_Y_DIRECTION, -MENU_TILT_LIMIT, MENU_TILT_LIMIT);
  const float targetX = MENU_CENTER_X + (xTilt / MENU_TILT_LIMIT) * MENU_BALL_X_RANGE;
  const float targetY = MENU_CENTER_Y + (yTilt / MENU_TILT_LIMIT) * MENU_BALL_Y_RANGE;

  // This small amount of smoothing makes the ball feel less jittery.
  menuBallX += (targetX - menuBallX) * 0.20f;
  menuBallY += (targetY - menuBallY) * 0.20f;

  // The menu has four full sectors, not four tiny targets. Any tilt direction
  // past the center dead zone selects its matching top/right/bottom/left action.
  if (fabsf(xTilt) < MENU_CENTER_DEADZONE && fabsf(yTilt) < MENU_CENTER_DEADZONE) {
    selectedMenuItem = -1;
  } else if (fabsf(xTilt) > fabsf(yTilt)) {
    selectedMenuItem = xTilt > 0.0f ? 1 : 3;  // right or left
  } else {
    selectedMenuItem = yTilt > 0.0f ? 2 : 0;  // bottom or top
  }
}

void checkForShake() {
  if (!mpuFound || currentView != PET_VIEW) {
    return;
  }

  const float magnitude = sqrtf(accelerationX * accelerationX +
                                accelerationY * accelerationY +
                                accelerationZ * accelerationZ);
  const uint32_t now = millis();
  if (fabsf(magnitude - STANDARD_GRAVITY) >= SHAKE_THRESHOLD &&
      now - lastShakeAt >= SHAKE_COOLDOWN_MS) {
    lastShakeAt = now;
    noteActivity();  // A shake also wakes a dimmed or sleeping screen.
    nappingUntil = 0;  // A shake wakes a sleeping pet right away.
    shakeAnimationEndsAt = now + 350;
    changePet(SHAKE_JOY_CHANGE, SHAKE_ENERGY_CHANGE, SHAKE_FULLNESS_CHANGE);
    savePet();
  }
}

void openMenu() {
  currentView = MENU_VIEW;
  selectedMenuItem = -1;
  menuBallX = MENU_CENTER_X;
  menuBallY = MENU_CENTER_Y;
  // Calibrate here so every menu starts from however the builder is holding it.
  menuCenterAccelerationX = accelerationX;
  menuCenterAccelerationY = accelerationY;
}

void chooseMenuItem() {
  if (selectedMenuItem == -1) {
    return;  // Keep the menu open until the ball reaches a choice.
  }

  const MenuItem &item = MENU_ITEMS[selectedMenuItem];
  changePet(item.joyChange, item.energyChange, item.fullnessChange);
  savePet();
  const uint32_t now = millis();
  nappingUntil = 0;
  heartAnimationEndsAt = 0;
  playRunStartedAt = 0;

  if (item.reaction == NAP_REACTION) {
    petJumpStartedAt = 0;
    nappingPetX = petWalkingX(now);
    nappingUntil = now + NAP_DURATION_MS;
  } else if (item.reaction == RUN_REACTION) {
    petJumpStartedAt = 0;
    playRunStartedAt = now;
    heartAnimationEndsAt = now + playRunDuration();
  } else {
    // A confirmed action gets a little anticipation shake before the jump.
    petJumpStartedAt = now;
    if (item.reaction == HEART_REACTION) {
      heartAnimationEndsAt = now + HEARTS_DURATION_MS;
    }
  }
  currentView = PET_VIEW;
}

void handleButtons() {
  if (wasPressed(buttonOne)) {
    if (wakeScreen()) {
      return;  // The first press only wakes a sleeping screen.
    }
    if (isWarningActive()) {
      warningSnoozedUntil = millis() + WARNING_SNOOZE_MS;
      return;  // A press hides the battery warning for a while.
    }
    if (currentView == MENU_VIEW) {
      chooseMenuItem();
    } else {
      openMenu();
    }
  }

  if (wasPressed(buttonTwo)) {
    if (wakeScreen()) {
      return;  // The first press only wakes a sleeping screen.
    }
    if (isWarningActive()) {
      warningSnoozedUntil = millis() + WARNING_SNOOZE_MS;
      return;  // A press hides the battery warning for a while.
    }
    // Button 2 never changes the pet. It only lets you peek at the values.
    currentView = currentView == STATS_VIEW ? PET_VIEW : STATS_VIEW;
  }
}

void drawSparkle(int x, int y) {
  display.drawPixel(x, y - 2, SSD1306_WHITE);
  display.drawPixel(x - 1, y - 1, SSD1306_WHITE);
  display.drawPixel(x + 1, y - 1, SSD1306_WHITE);
  display.drawPixel(x - 2, y, SSD1306_WHITE);
  display.drawPixel(x, y, SSD1306_WHITE);
  display.drawPixel(x + 2, y, SSD1306_WHITE);
  display.drawPixel(x - 1, y + 1, SSD1306_WHITE);
  display.drawPixel(x + 1, y + 1, SSD1306_WHITE);
  display.drawPixel(x, y + 2, SSD1306_WHITE);
}

void drawHeart(int x, int y) {
  // A tiny seven-pixel-wide heart that stays crisp on the OLED.
  display.fillRect(x - 2, y, 2, 2, SSD1306_WHITE);
  display.fillRect(x + 1, y, 2, 2, SSD1306_WHITE);
  display.fillRect(x - 3, y + 2, 7, 2, SSD1306_WHITE);
  display.fillRect(x - 2, y + 4, 5, 1, SSD1306_WHITE);
  display.fillRect(x - 1, y + 5, 3, 1, SSD1306_WHITE);
  display.drawPixel(x, y + 6, SSD1306_WHITE);
}

void drawHearts(uint32_t now, int petX, int petY) {
  if (now >= heartAnimationEndsAt) {
    return;
  }

  const float progress =
      1.0f - static_cast<float>(heartAnimationEndsAt - now) / HEARTS_DURATION_MS;
  const int rise = static_cast<int>(progress * 18.0f);
  drawHeart(petX + 10, petY - 3 - rise);
  drawHeart(petX + 22, petY - 9 - rise / 2);
}

void drawSleepZs(uint32_t now, int petX, int petY) {
  // The Zs float upward, then loop back down while the pet is napping.
  const int rise = static_cast<int>((now / 300UL) % 15UL);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(petX + 23, petY - 2 - rise);
  display.print("z");
  display.setCursor(petX + 28, petY - 8 - rise / 2);
  display.print("z");
}

// A small battery, 12 pixels wide and 7 tall. It blinks when the battery is
// low, and is hidden when no batteries are connected (running from USB).
void drawBatteryIcon(int x, int y) {
  if (!USE_BATTERY_METER || !batteryPresent) {
    return;
  }
  if (batteryLow && (millis() / 500) % 2 == 0) {
    return;
  }
  display.drawRect(x, y, 11, 7, SSD1306_WHITE);
  display.fillRect(x + 11, y + 2, 1, 3, SSD1306_WHITE);
  display.fillRect(x + 2, y + 2, map(batteryPercent, 0, 100, 0, 7), 3, SSD1306_WHITE);
}

void drawSweatDrop(int x, int y) {
  display.drawPixel(x, y, SSD1306_WHITE);
  display.fillRect(x - 1, y + 1, 3, 2, SSD1306_WHITE);
}

// Hot: sweat drops fall beside the head. Cold: the pet shivers and says brr.
void drawTemperatureReaction(uint32_t now, int petX, int petY) {
  if (isNapping()) {
    return;
  }
  if (isHot()) {
    const int fall = static_cast<int>((now / 120UL) % 10UL);
    drawSweatDrop(petX + 4, petY + 3 + fall);
    drawSweatDrop(petX + 26, petY + 8 + (fall + 5) % 10);
  } else if (isCold()) {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(constrain(petX + 2, 0, 108), petY - 8);
    display.print("brr");
  }
}

void drawPet() {
  display.clearDisplay();

  const uint32_t now = millis();
  int petX = isNapping() ? nappingPetX : petWalkingX(now);
  if (isPlaying()) {
    petX = playRunX(now);
  }
  int petY = SCREEN_HEIGHT - PET_SPRITE_HEIGHT;

  // The pet always wanders except during a nap; PLAY temporarily takes over.
  if (now < shakeAnimationEndsAt) {
    petX += static_cast<int>(sinf(now / 18.0f) * 3.0f);
  }
  if (isCold() && !isNapping()) {
    petX += (now / 60) % 2 == 0 ? 1 : -1;  // shiver
  }

  if (!isNapping() && petJumpStartedAt != 0) {
    const uint32_t animationAge = now - petJumpStartedAt;

    if (animationAge < PET_PRE_JUMP_MS) {
      petX += static_cast<int>(sinf(now / 16.0f) * 3.0f);
    } else if (animationAge < PET_PRE_JUMP_MS + PET_JUMP_MS) {
      const float jumpProgress =
          static_cast<float>(animationAge - PET_PRE_JUMP_MS) / PET_JUMP_MS;
      petY -= static_cast<int>(sinf(jumpProgress * PI) * PET_JUMP_HEIGHT);
    } else {
      petJumpStartedAt = 0;
    }
  }

  petX = constrain(petX, 0, SCREEN_WIDTH - PET_SPRITE_WIDTH);
  display.drawBitmap(petX, petY, PET_SPRITE, PET_SPRITE_WIDTH, PET_SPRITE_HEIGHT,
                     SSD1306_WHITE);
  if (!isNapping() && !batteryLow && (now / 250) % 2 == 0) {
  drawSparkle(petX - 4, petY + 18);
}

  if (isNapping() || batteryLow) {
    drawSleepZs(now, petX, petY);
  }
  drawHearts(now, petX, petY);
  drawTemperatureReaction(now, petX, petY);
  drawBatteryIcon(SCREEN_WIDTH - 14, 1);
}

void drawMenuItem(int item) {
  const int boxWidth = 33;
  const int boxHeight = 12;
  const int boxX = MENU_ITEM_X[item] - boxWidth / 2;
  const int boxY = MENU_ITEM_Y[item] - boxHeight / 2;
  const bool isSelected = item == selectedMenuItem;

  if (isSelected) {
    display.fillRoundRect(boxX, boxY, boxWidth, boxHeight, 3, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  } else {
    display.drawRoundRect(boxX, boxY, boxWidth, boxHeight, 3, SSD1306_WHITE);
    display.setTextColor(SSD1306_WHITE);
  }

  display.setTextSize(1);
  const int labelLength = strlen(MENU_ITEMS[item].label);
  display.setCursor(MENU_ITEM_X[item] - labelLength * 3, MENU_ITEM_Y[item] - 3);
  display.print(MENU_ITEMS[item].label);
  display.setTextColor(SSD1306_WHITE);
}

void drawMenu() {
  display.clearDisplay();
  // The two diagonal lines divide the screen into four big selection sectors.
  display.drawLine(0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1, SSD1306_WHITE);
  display.drawLine(0, SCREEN_HEIGHT - 1, SCREEN_WIDTH - 1, 0, SSD1306_WHITE);
  for (int item = 0; item < MENU_ITEM_COUNT; item++) {
    drawMenuItem(item);
  }

  display.drawCircle(MENU_CENTER_X, MENU_CENTER_Y, 11, SSD1306_WHITE);
  display.fillCircle(static_cast<int>(menuBallX), static_cast<int>(menuBallY), 4,
                     SSD1306_WHITE);
}

void drawStatBar(int y, const char *label, int value) {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(4, y);
  display.print(label);
  display.drawRect(48, y, 57, 8, SSD1306_WHITE);
  display.fillRect(49, y + 1, map(value, 0, 100, 0, 55), 6, SSD1306_WHITE);
  display.setCursor(109, y);
  display.print(value);
}

void drawStats() {
  display.clearDisplay();
  drawStatBar(5, "JOY", pet.joy);
  drawStatBar(20, "ENERGY", pet.energy);
  drawStatBar(35, "FULL", pet.fullness);

  display.setCursor(4, 53);
  if (!isnan(temperatureC)) {
    display.print("TEMP ");
    display.print(static_cast<int>(temperatureC));
    display.print("C");
  } else {
    display.print("TEMP --");
  }

  display.setCursor(76, 53);
  if (!isnan(humidity)) {
    display.print("H ");
    display.print(static_cast<int>(humidity));
    display.print("%");
  } else {
    display.print("H --");
  }

  drawBatteryIcon(114, 53);
}

// Shown when the battery is critical. A button press hides it for a minute.
void drawWarning() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  if ((millis() / 500) % 2 == 0) {
    display.drawRect(46, 3, 32, 18, SSD1306_WHITE);
    display.fillRect(78, 8, 3, 8, SSD1306_WHITE);
    display.setTextSize(2);
    display.setCursor(56, 4);
    display.print("!");
  }
  display.setTextSize(1);
  display.setCursor(16, 28);
  display.print("BATTERY CRITICAL");
  display.setCursor(10, 40);
  display.print("Pet progress saved");
  display.setCursor(16, 52);
  display.print("Change batteries");
}

void drawCurrentView() {
  if (isWarningActive()) {
    drawWarning();
  } else if (currentView == MENU_VIEW) {
    drawMenu();
  } else if (currentView == STATS_VIEW) {
    drawStats();
  } else {
    drawPet();
  }
  display.display();
}

void setup() {
  Serial.begin(115200);
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  setUpButton(buttonOne);
  setUpButton(buttonTwo);
  loadPet();

  if (USE_BATTERY_METER) {
    pinMode(BATTERY_PIN, INPUT);
    updateBattery(true);
  }
  if (USE_LED) {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
  }
  lastActivityAt = millis();

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("OLED not found. Check power, GND, SDA, SCL, and address.");
    while (true) {
      delay(10);
    }
  }

  mpuFound = mpu.begin(MPU6050_ADDRESS, &Wire);
  if (mpuFound) {
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  } else {
    Serial.println("MPU6050 not found. The menu will not move until it is connected.");
  }

  if (USE_DHT11) {
    dht.begin();
    dhtFound = true;
  }

  drawCurrentView();
}

void loop() {
  updateMpu();
  updateDht();
  updateBattery(false);
  updateWarningMode();
  updatePetTimers();
  handleButtons();
  updateIdleSleep();
  updateLed();

  if (currentView == MENU_VIEW) {
    updateMenuBall();
  } else {
    checkForShake();
  }

  if (!screenOff) {
    drawCurrentView();
  }
  delay(screenOff ? 50 : 16);  // Smooth display updates without making the code complicated. (I hope :D)
}