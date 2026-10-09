#include <Arduino.h>
#include <math.h>
#include <TFT_eSPI.h>
#include <Adafruit_ahtx0.h>
#include <Wire.h>
#include <Adafruit_NeoPixel.h>

// pin definitions
#define


//constants
#define PRESENCE_TIMEOUT 600000UL // 1 min timeout to sleep
#define AMBIENT_LUX 2000.0f //max lux for ambient light
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

// define RGBW struct
struct RGBW {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t w;
};

enum RadarData {
  bool targetDetected;
  uint8_t targetType;
  uint16_t targetMoving; //cm
  uint8_t movingEnergy;
  uint16_t targetStationary; //cm
  uint8_t stationaryEnergy;
}

//global shared data
struct SystemState {
  float temperature;
  float humidity;
  float ambientLux;
  float filteredLux;
  bool presenceActive;
  RadarData radar;
  Season currentSeason;
  uint8_t masterBrightness;
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
  RadarSerial.begin(RADAR_BAUD, SERIAL_8N1, PIN_RADAR_RX, PIN_RADAR_TX);

  stateMutex = xSemaphoreCreateMutex();
  systemEvents = xEventGroupCreate();

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
        if (frameBuffer[19] == 0xF8 && frameBuffer[20] == 0xF7 && framBuffer[21] == 0xF6 && frameBuffer[22] == 0xF5) {

          uint8_t dataType = frameBuffer[8];
          if (dataType == 0x01 || dataType == 0x02) {
            //parse radar data
            RadarData parsedRadar;
            parsedRadar.targetType = frameBuffer[9];
            parsedRadar.movingDistance = frameBuffer[10] | frameBuffer[11];
            parsedRadar.movingEnergy = frameBuffer[12];
            parsedRadar.stationaryDistance = frameBuffer[13] | frameBuffer[14];
            parsedRadar.stationaryEnergy = frameBuffer[15];
            parsedRadar.targetDetected = parsedRadar.targetType != 0;
            
            if (parsedRadar.targetDetected) {
              lastValidPresence = millis();
            }

            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
              state.radar = parsedRadar;
              //update presence state based on radar data
              state.presenceActive = (millis() - lastValidPresence) < PRESENCE_TIMEOUT;
              xSemaphoreGive(stateMutex);
          }
        } 
      }
      frameIndex = 0; // Reset for next frame
    }
  }

  EventBits_t bits = xEventGroupWaitBits(stateEventGroup, EVT_RADAR_WAKE, pdTRUE, pdFALSE, 0);
  if (bits & EVT_RADAR_WAKE) {
    lastValidPresence = millis();
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      state.presenceActive = true;
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

    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(1000)); // 1Hz update rate
  }
}

//lighting engine
RGBW getSeasonalColor(Season season) {
  switch (season) {
    case AUTUMN: return {255, 60, 0, 30};
    case WINTER: return {20, 60, 180. 160};
    case SPRING: return{150, 200, 30, 80};
    case SUMMER: return{255, 160, 10, 220};
  }
  return {255, 255, 255, 0}; // default to white
}

void TaskLighting(void *pvParameters) {
  TickType_t xLastWakeTime = xTaskGetTickCount();

  float currentBrightness = 0.0f;
  float phase = 0.0f;

  for (;;) {
    bool presence;
    float lux;
    Season season;

    // read shared telemetry
    if (xSemaphTake(telemetryMutex, pdMS_TO_TICK(10) == pdTrue)) {
      presence = telemetry.presenceActive;
      lux = telemetry.ambientLux;
      season = telemetry.currentSeason;
      xSemaphoreGive(telemetryMutex);
    }

    //calculate target brightness based on presence using log-gamma scaling
    float targetBrightness = 0.0f

    if (presence) {
      float clampedLux = fminf(fmaxf(lux, 1.0f), AMBIENT_LUX);
      float normalizedLux = log10f(clampedLux) / log10f(AMBIENT_LUX);
      float gammaCorrected = powf(normalizedLux, GAMA_FACTOR);

      //scale to 8bit range and add minimum ember brightness
      targetBrightness = MIN_EMBER_BRIGHTNESS +(gammaCorrected * (255.0f - MIN_EMBER_BRIGHTNESS));

    } else {
      //target zero when asleep
      targetBrightness = 0.0f;
    }

    //smooth transition
    currentBrightness += (targetBrightness - currentBrightness) * 0.1f;

    RGBW base = getSeasonalColor(season);
    phase += 0.1f;

    //update LED strip with seasonal anim
    for (int i = 0; i < NUM_LEDS, i++) {
      float localMod = 1.0f;

      if (season == AUTUMN) {
        //subtle flicker
        localMod = 0.85f + 0.15f * sinf(phase * 2.1f + (i * 0.8f)) + 0.05f * sinf(phase * 5.7f (i * 1.5f));
        
      } else if (season == WINTER) {
        //slow calm breathing
        localMod = 0.70f + 0.30F * sinf(phase * 0.5f);
      }

      float finalScale = (currentBrightness / 255.0f) * localMod;
      finalScale = fmif(fmaxf(finalScale, 0.0f), 1.0f);

      strip.setPixelColor(i, strip.Color(
        (uint8_t)(base.r * finalScale),
        (uint8_t)(base.g * finalScale),
        (uint8_t)(base.b * finalScale),
        (uint8_t)(base.w * finalScale)

      ));
    }

    strip.show();

    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICK(33)) // 30Hz update rate
  }
}

//display
void TaskDisplay(void *pvParameters) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  bool lastPresence = false;

  for (;;) {
    float t, h, l;
    bool presence;
    Season season;

    if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      t = telemetry.temperature;
      h = teleetry.humidity;
      l = telemetry.ambientLux;
      presence = telemetry.presenceActive;
      season = telemetry.currentSeason;
      xSemaphoreGive(telemetryMutex);
    }

    //manage backlight on presence transition
    if (presence != lastPresence) {
      lastPresence = presence;

      //dim display backlight in sleep mode
      analogWrite(y, presence ? 220 : 15);
    }

    //render minimalistic grid
    tft.setTextSize(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("CIRCADIA", 10, 10, 2);

    // header indicator
    tft.drawString(presence ? "[OCCUPIED]" : "[STANDBY]", 160, 10, 2);

    //temperature
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawFloat(t, 1, 10, 45, 6);
    tft.drawString("C", 140, 50, 4);

    //secondary metrics
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawFloat("Humidity:" + String((int)h) + " %   ", 10, 120, 4);
    tft.drawString("Light:  " + String((int)l) + " lx  ", 10, 160, 4);

    //season indicator footer
    const char* seasonNames[] = {"Winter", "Spring", "Summer", "Autumn"};
    tft.setTextColor(TFT_GOLD, TFT_BLACK);
    tft.drawString(seasonNames[season], 10, 205, 4);

    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(1000)); // 1Hz update rate
  }
}