#include <Arduino.h>
#include <math.h>
#include <TFT_eSPI.h>
#include <Adafruit_ahtx0.h>
#include <Wire.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include "ephemeris.h"

// pin definitions
// TODO: Define your hardware pins here (PIN_RADAR_RX, PIN_RADAR_TX, PIN_RADAR_OUT, PIN_DISPLAY_BACKLIGHT, PIN_I2C_SDA, PIN_I2C_SCL)

//constants
#define PRESENCE_TIMEOUT 600000UL // 1 min timeout to sleep
#define GAMMA_FACTOR 2.2f // gamma correction factor
#define MIN_EMBER_BRIGHTNESS 6 // minimum brightness for ember effect
#define NUM_LEDS 30 // number of LEDs in the strip
#define BH1750_ADDRESS 0x23 // I2C address for BH1750 light sensor
#define RADAR_BAUD 256000 // baud rate for radar sensor
#define LUX_SMOOTHING_FACTOR 0.08f // smoothing factor for lux readings

// define seasons
enum Season : uint8_t {
  WINTER = 0,
  SPRING = 1,
  SUMMER = 2,
  AUTUMN = 3
};

struct RadarData {
  bool targetDetected;
  uint8_t targetType;
  uint16_t movingDistance; //cm
  uint8_t movingEnergy;
  uint16_t stationaryDistance; //cm
  uint8_t stationaryEnergy;
};

//global shared data
struct SystemState {
  float temperature;
  float humidity;
  float ambientLux;
  float filteredLux;
  bool presenceActive;
  bool isAwake;
  RadarData radar;
  Season currentSeason;
  uint8_t masterBrightness;
  float vpdKpa;
  const char* comfortLabel;
  uint16_t statusColor;
  SolarMetrics solar;
};

//global variables
static SystemState state;

static SemaphoreHandle_t stateMutex = NULL;
static EventGroupHandle_t stateEventGroup = NULL;

#define EVT_RADAR_WAKE (1 << 0) // Event flag for radar wake-up
#define EVT_SENSOR_FAILED (1 << 1)// Event flag for sensor failure
#define EVT_NVS_SAVE_REQ (1 << 2)// Event flag for NVS save request

static Adafruit_AHTX0 aht;
static Adafruit_NeoPixel strip(30, 5, NEO_GRBW + NEO_KHZ800); // 30 LEDs on pin 5
static TFT_eSPI tft = TFT_eSPI(); // Create TFT object
static Preferences preferences; // NVS storage
static HardwareSerial radarSerial(1); // Use hardware serial 1 for radar

//forward declarations
void TaskRadar(void *pvParameters);
void TaskSensor(void *pvParameters);
void TaskLighting(void *pvParameters);
void TaskDisplay(void *pvParameters);
void IRAM_ATTR ISR_RadarWake();

static const uint8_t PROGMEM CIE1931[256] = {
  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   1,   1,   1,   1,   1,   1,
  1,   1,   2,   2,   2,   2,   2,   2,   3,   3,   3,   3,   4,   4,   4,   4,
  5,   5,   5,   6,   6,   6,   7,   7,   7,   8,   8,   9,   9,  10,  10,  11,
  11,  12,  12,  13,  13,  14,  15,  15,  16,  17,  17,  18,  19,  19,  20,  21,
  22,  23,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,
  37,  38,  40,  41,  42,  43,  45,  46,  47,  49,  50,  51,  53,  54,  56,  57,
  59,  60,  62,  64,  65,  67,  69,  70,  72,  74,  76,  78,  80,  82,  84,  86,
  88,  90,  93,  95,  97, 100, 102, 104, 107, 109, 112, 115, 117, 120, 123, 126,
  128, 131, 134, 137, 140, 143, 147, 150, 153, 156, 160, 163, 167, 170, 174, 177,
  181, 185, 189, 192, 196, 200, 204, 208, 212, 216, 221, 225, 229, 234, 238, 243,
  247, 252, 255
};

uint8_t calculateCIE(uint8_t inputLuminance) {
  return pgm_read_byte(&CIE1931[inputLuminance]);
}

// 1d perlin noise engine
float NoiseHash(int32_t x) {
  x = (x << 13) ^ x;
  return (1.0f - ((x * (x * x * 15731 + 789221) + 1376312589) & 0x7fffffff) / 1073741824.0f);
}

float InterpolatedNoise(float x) {
  int32_t integerX = (int32_t)floorf(x);
  float fractionalX = x - (float)integerX;

  float f = fractionalX * fractionalX * (3.0f - 2.0f * fractionalX);

  float v1 = NoiseHash(integerX);
  float v2 = NoiseHash(integerX + 1);

  return v1 + f * (v2 - v1);
}

void IRAM_ATTR ISR_RadarWake() {
  baseType_t xHigherPriorityTaskWoken = pdFALSE;
  xEventGroupSetBitsFromISR(stateEventGroup, EVT_RADAR_WAKE, &xHigherPriorityTaskWoken);
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void initBH1750() {
  Wire.beginTransmission(BH1750_ADDRESS); // BH1750 address
  Wire.write(0x10); // Continuously H-Resolution Mode
  Wire.endTransmission();
}

float readBH1750Lux() {
  Wire.beginTransmission(BH1750_ADDRESS);
  Wire.requestFrom(BH1750_ADDRESS, 2);
  if (Wire.available() >= 2) {
    uint16_t raw = Wire.read() << 8 | Wire.read();
    return (float)raw / 1.2f; // Convert to lux
  }
  return -1.0f; // Error reading
}

//setup
void setup() {
  Serial.begin(115200);
  radarSerial.begin(RADAR_BAUD, SERIAL_8N1, PIN_RADAR_RX, PIN_RADAR_TX);

  stateMutex = xSemaphoreCreateMutex();
  stateEventGroup = xEventGroupCreate();

  pinMode(PIN_RADAR_OUT, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(PIN_RADAR_OUT), ISR_RadarWake, RISING);

  //hardware pwm for display backlight
  ledcAttach(PIN_DISPLAY_BACKLIGHT, 5000, 8); // 5kHz PWM, 8-bit resolution
  ledcWrite(PIN_DISPLAY_BACKLIGHT, 220); // initial backlight brightness

  //i2c
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);

  //display
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setSwapBytes(true);

  //led
  strip.begin();
  strip.setBrightness(255);
  strip.show(); // Initialize all pixels to 'off'

  preferences.begin("circadia", false);
  state.currentSeason = (Season)preferences.getUChar("season", AUTUMN);
  preferences.end();

  //sensor check
  if (!aht.begin(&Wire)) {
    Serial.println("Failed to find AHT sensor");
    xEventGroupSetBits(stateEventGroup, EVT_SENSOR_FAILED);
  }

  //bh1750 init
  initBH1750();

  //default telemetry values
  state.temperature = 24.0f;
  state.humidity = 50.0f;
  state.ambientLux = 100.0f;
  state.filteredLux = 100.0f;
  state.presenceActive = true;
  state.isAwake = true;
  state.masterBrightness = 255;

  //freertos
  xTaskCreatePinnedToCore(TaskRadar, "Radar", 3072, NULL, 4, NULL, 0);
  xTaskCreatePinnedToCore(TaskSensor, "Sensor", 3072, NULL, 2, NULL, 0);

  xTaskCreatePinnedToCore(TaskLighting, "Lighting", 4096, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(TaskDisplay, "Display", 4096, NULL, 1, NULL, 1);

  vTaskDelete(NULL); // Delete the setup task as we are using FreeRTOS tasks
}

void loop() {}// Empty loop since we are using FreeRTOS tasks

void TaskRadar(void *pvParameters) {

  TickType_t xLastWakeTime = xTaskGetTickCount();
  uint8_t frameBuffer[64];
  uint8_t frameIndex = 0;
  uint32_t lastValidPresence = millis();

  for (;;) {
    //read from sensor
    while (radarSerial.available() > 0) {
      uint8_t byteIn = radarSerial.read();

      // Check for data fram header
      if (frameIndex == 0 && byteIn == 0xF4) continue;
      if (frameIndex == 1 && byteIn == 0xF3) {frameIndex = 0; continue;}
      if (frameIndex == 2 && byteIn == 0xF2) {frameIndex = 0; continue;}
      if (frameIndex == 3 && byteIn == 0xF1) {frameIndex = 0; continue;}

      frameBuffer[frameIndex++] = byteIn;

      //23 bytes
      if (frameIndex >= 23) {
        //verify end frame
        if (frameBuffer[19] == 0xF8 && frameBuffer[20] == 0xF7 && frameBuffer[21] == 0xF6 && frameBuffer[22] == 0xF5) {

          uint8_t dataType = frameBuffer[8];
          if (dataType == 0x01 || dataType == 0x02) {
            //parse radar data
            RadarData parsedRadar;
            parsedRadar.targetType = frameBuffer[9];
            parsedRadar.movingDistance = (uint16_t)frameBuffer[10] | ((uint16_t)frameBuffer[11] << 8);
            parsedRadar.movingEnergy = frameBuffer[12];
            parsedRadar.stationaryDistance = (uint16_t)frameBuffer[13] | ((uint16_t)frameBuffer[14] << 8);
            parsedRadar.stationaryEnergy = frameBuffer[15];
            parsedRadar.targetDetected = parsedRadar.targetType != 0;
            
            if (parsedRadar.targetDetected) {
              lastValidPresence = millis();
            }

            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
              state.radar = parsedRadar;
              //update presence state based on radar data
              state.presenceActive = (millis() - lastValidPresence) < PRESENCE_TIMEOUT;
              state.isAwake = state.presenceActive;
              xSemaphoreGive(stateMutex);
            }
          } 
        }
        frameIndex = 0; // Reset for next frame
      }
    }
  }

  EventBits_t bits = xEventGroupWaitBits(stateEventGroup, EVT_RADAR_WAKE, pdTRUE, pdFALSE, 0);
  if (bits & EVT_RADAR_WAKE) {
    lastValidPresence = millis();
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      state.presenceActive = true;
      state.isAwake = true;
      xSemaphoreGive(stateMutex);
    }
  }

  vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(20)); // 50Hz update rate
  }
}

//sensors
void TaskSensor(void *pvParameters) {
  TickType_t xLastWakeTime = xTaskGetTickCount();

  for (;;) {
    //read temp and humidity
    sensors_event_t humidityEvent, tempEvent;

    float measuredLux = -1.0f;

    bool ahtSuccess = aht.getEvent(&humidityEvent, &tempEvent);

    Wire.beginTransmission(BH1750_ADDRESS);
    if (Wire.requestFrom(BH1750_ADDRESS, 2) == 2) {
      uint16_t rawLux = Wire.read() << 8 | Wire.read();
      measuredLux = (float)rawLux / 1.2f; // Convert to lux
    }
    Wire.endTransmission();

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (ahtSuccess) {
        state.temperature = tempEvent.temperature;
        state.humidity = humidityEvent.relative_humidity;
      }
      if (measuredLux >= 0.0f) {
        //apply smoothing to lux readings
        state.filteredLux = (LUX_SMOOTHING_FACTOR * measuredLux) + ((1.0f - LUX_SMOOTHING_FACTOR) * state.filteredLux);
      }
      xSemaphoreGive(stateMutex);
    }

    RespiratoryComfort comfort = EnvironmentalMath::computeVPD(tempEvent.temperature, humidityEvent.relative_humidity);

    SolarMetrics solar = EnvironmentalMath::computeSolar(37.7749f, -122.4194f, 283, 16.5f);

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      state.temperature = tempEvent.temperature;
      state.humidity = humidityEvent.relative_humidity;
      state.vpdKpa = comfort.vpdKpa;
      state.comfortLabel = comfort.comfortLabel;
      state.statusColor = comfort.statusColor;
      state.solar = solar;
      xSemaphoreGive(stateMutex);
    }

    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(1000)); // 1Hz update rate
  }
}

//lighting engine
void TaskLighting(void *pvParameters) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  float dynamicLuminance = 0.0f;
  float noiseCursor = 0.0f;

  for (;;) {
    bool awake;
    float lux;
    Season season;
    float cct = 0.0f;

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      awake = state.presenceActive;
      lux = state.filteredLux;
      season = state.currentSeason;
      cct = state.solar.circadianCctRatio; // 0.0 deep ember, 1.0 full daylight
      xSemaphoreGive(stateMutex);
    }

    //environmental lux 
    float targetLuminance = 0.0f;
    if (awake) {
      //log mapping
      float clampedLux = fminf(fmaxf(lux, 0.1f), 2000.0f);
      float logRatio = log10f(clampedLux + 1.0f) / log10f(2001.0f);

      //pass through cie1931 gamma correction
      uint8_t scaledInput = (uint8_t)(logRatio * 255.0f);
      targetLuminance = (float)calculateCIE(scaledInput);

      if (targetLuminance < 12.0f) targetLuminance = 12.0f;
    } else {
      //decay to off when in sleep mode
      targetLuminance = 0.0f;
    }

    //temporal smoothing filter
    dynamicLuminance += (targetLuminance - dynamicLuminance) * 0.06f;

    //procedural synthesis by season
    noiseCursor += 0.05f;
    float masterScalar = dynamicLuminance / 255.0f;
    
    for (int i = 0; i < NUM_LEDS; i++) {
      float r = 0, g = 0, b = 0, w = 0;
      float ledOffset = (float)i * 0.45f;

      switch (season) {
        case AUTUMN: {
          //fractional brownian motion for ember effect
          float n = InterpolatedNoise(noiseCursor + ledOffset);
          float emberMod = 0.65f + 0.35f * n; // modulate between 0.3 and 1.0

          //autumn colors
          r = 255.0f * emberMod;
          g = 55.0f * (emberMod * emberMod);
          b = 2.0f;
          w = 20.0f * emberMod;
          break;
        }

        case WINTER: {
          //winter colors 
          float breath = 0.5f + 0.5f * sinf(noiseCursor * 0.3f + (i * 0.1f));
          r = 10.0f * breath;
          g = 40.0f * breath;
          b = 180.0f * breath;
          w = 150.0f * (0.8f + 0.2f * breath);
          break;
        }

        case SPRING: {
          float shimmer = 0.85f + 0.15f * sinf(noiseCursor * 0.9f + i);
          r = 140.0f * shimmer;
          g = 220.0f * shimmer;
          b = 20.0f * shimmer;
          w = 60.0f;
          break;
        }

        case SUMMER: { //nothing special for summer because i hate summer
          r = 255.0f;
          g = 180.0f;
          b = 40.0f;
          w = 230.0f;
          break;
        }
      }

      //circadian CCT blend
      float targetR = r * (1.0f - 0.4f * cct);
      float targetW = w + (255.0f - w) * cct;

      //apply global brightness
      uint8_t finalR = (uint8_t)(fminf(fmaxf(targetR * masterScalar, 0.0f), 255.0f));
      uint8_t finalG = (uint8_t)(fminf(fmaxf(g * masterScalar, 0.0f), 255.0f));
      uint8_t finalB = (uint8_t)(fminf(fmaxf(b * masterScalar, 0.0f), 255.0f));
      uint8_t finalW = (uint8_t)(fminf(fmaxf(targetW * masterScalar, 0.0f), 255.0f));

      strip.setPixelColor(i, strip.Color(finalR, finalG, finalB, finalW));
    }

    strip.show();

    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(33));
  }
}

//display
void TaskDisplay(void *pvParameters) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
 
  //allocate sprite buffer
  TFT_eSprite sprite = TFT_eSprite(&tft);
  sprite.setColorDepth(8); //8 bit palette
  sprite.createSprite(240, 240);

  bool previousWake = false;

  for (;;) {
    SystemState snap;

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      snap = state;
      xSemaphoreGive(stateMutex);
    }

    //manage display backlight
    if (snap.isAwake != previousWake) {
      previousWake = snap.isAwake;
      ledcWrite(PIN_DISPLAY_BACKLIGHT, snap.isAwake ? 190 : 0);
    }

    sprite.fillSprite(TFT_BLACK);

    // header bar
    sprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
    sprite.drawString("CIRCADIA", 12, 12, 2);

    if (snap.isAwake) {
      sprite.setTextColor(TFT_GREEN, TFT_BLACK);
      sprite.drawString("ACTIVE", 175, 12, 2);
    } else {
      sprite.setTextColor(TFT_MAROON, TFT_BLACK);
      sprite.drawString("SLEEP", 185, 12, 2);
    }

    sprite.drawFastHLine(10, 32, 220, 0x2104); // draw a horizontal line

    //temp 
    sprite.setTextColor(TFT_ORANGE, TFT_BLACK);
    sprite.drawFloat(snap.temperature, 1, 14, 46, 7);
    sprite.drawString("o", 172, 44, 2);
    sprite.drawString("C", 186, 52, 4);
    
    //humidity
    sprite.setTextColor(TFT_WHITE, TFT_BLACK);
    sprite.drawString("HUMIDITY", 16, 122, 2);
    sprite.drawString(String((int)snap.humidity) + "%", 16, 138, 4);

    sprite.drawString("AMBIENT", 130, 122, 2);
    sprite.drawString(String((int)snap.ambientLux) + "lx", 130, 138, 4);

    sprite.drawFastHLine(10, 172, 220, 0x2104);

    //radar and presence
    sprite.setTextColor(TFT_SILVER, TFT_BLACK);
    sprite.drawString("RADAR GATE ENERGIES", 16, 180, 1);

    //static presence bar
    sprite.drawRect(16, 184, 95, 8, TFT_DARKGREY);
    uint8_t statW = (uint8_t)((snap.radar.stationaryEnergy / 100.0f) * 91.0f);
    sprite.fillRect(18, 196, statW, 4, TFT_CYAN);

    //dynamic moving energy bar
    sprite.drawRect(128, 194, 95, 8, TFT_DARKGREY);
    uint8_t moveW = (uint8_t)((snap.radar.movingEnergy / 100.0f) * 91.0f);
    sprite.fillRect(130, 196, moveW, 4, TFT_YELLOW);

    //season
    const char* seasonTags[] = { "WINTER [FROST]", "SPRING [BLOOM]", "SUMMER [ZENITH]", "AUTUMN [HEARTH]" };
    sprite.setTextColor(0xFDA0, TFT_BLACK); // amber/gold
    sprite.drawCenterString(seasonTags[snap.currentSeason], 120, 216, 2);

    //push frame to display
    sprite.pushSprite(0, 0);

    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(500));
  }
}