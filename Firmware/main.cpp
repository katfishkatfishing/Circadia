#include <Arduino.h>
#include <math.h>
#include <TFT_eSPI.h>
#include <Adafruit_ahtx0.h>
#include <Wire.h>
#include <Adafruit_NeoPixel.h>

// pin definitions
#define


//constants
#define PRESENCE_TIMEOUT 5000 // milliseconds
#define AMBIENT_LUX 2000.0f //max lux for ambient light
#define GAMMA_FACTOR 2.2f // gamma correction factor
#define MIN_EMBER_BRIGHTNESS 6 // minimum brightness for ember effect
#define NUM_LEDS 30 // number of LEDs in the strip
#define BH1750_ADDRESS 0x23 // I2C address for BH1750 light sensor

// define seasons
enum Season {
  WINTER,
  SPRING,
  SUMMER,
  AUTUMN
};

// define RGBW struct
struct RGBW {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t w;
};

//global shared data
struct SharedTelemetry {
  float temperature;
  float humidity;
  float ambientLux;
  bool presenceActive;
  Season currentSeason;
};

//global variables
SharedTelemetry telemetry = {24.0f, 50.0f, 100.0f, false, AUTUMN };
SemaphoreHandle_t telemetryMutex = NULL;

Adafruit_AHTX0 aht;
Adafruit_NeoPixel strip(30, 5, NEO_GRBW + NEO_KHZ800); // 30 LEDs on pin 5
TFT_eSPI tft = TFT_eSPI(); // Create TFT object

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

  telemetryMutex = xSemaphoreCreateMutex();

  pinMode(x, INPUT);//placeholder for presence sensor pin
  pinMode(y, OUTPUT);//placeholder for LED pin
  analogWrite(y, 255); 

  Wire.begin(x, y, 400000); // Initialize I2C with specified pins and frequency

  if (!aht.begind(&Wire)) {
    Serial.println("Failed to find AHT sensor");
    while (1) delay(10);
  }
  initBH1750();

  strip.begin();
  strip.show(); // Initialize all pixels to 'off'

  //lcd init
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  //freeRTOS tasks
  xTaskCreatePinnedToCore(TaskRadar,"Task_Radar", 2048, NULL, 4, NULL, 0);
  xTaskCreatePinnedToCore(TaskSensor, "Task_Sensors", 3072, NULL, 2, NULL, 0);

  //output / rendering
  xTaskCreatePinnedToCore(TaskLighting, "Task_Lighting", 4096, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(TaskDisplay, "Task_Display", 4096, NULL, 1, NULL, 1);

  Serial.println("[BOOT] Circidia OS initialized");

}

void loop() {
  vTaskDelete(NULL); // Delete the loop task as we are using FreeRTOS tasks
}

// radar sensing
void TaskRadar(void *pvParameters) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  uint32_t last_motion_timestamp = 0;
  bool is_present = false;

  for (;;) {
    //read from sensor
    int raw_radar = digitalRead(PIN_RADAR_OUT);
    uint32_t now = millis();

    if (raw_radar == HIGH) {
      last_motion_timestamp = now;
      is_present = true;
    } else if (now - last_motion_timestamp > PRESENCE_TIMEOUT) { //check if presence timeout has passed
      is_present = false;
    }

    // commit state change
    if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      telemetry.presenceActive = is_present;
      xSemaphoreGive(telemetryMutex);
    }

    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(100)); // 10Hz update rate

  }
}

//sensors
void TaskSensor(void *pvParameters) {
  TickType_t xLastWakeTime = xTaskGetTickCount();

  for (;;) {
    //read temp and humidity
    sensors_event_t humidity, temp;
    aht.getEvent(&humidity, &temp);

    float currentLux = readBH1750Lux();

    if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      telemetry.temperature = temp-temperature;
      telemetry.humidity = humidity.relative_humidity;
      if (currentLux >= 0.00f) {
        telemetry.ambientLux = currentLux;
      }
      xSemaphoreGive(telemetryMutex);
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