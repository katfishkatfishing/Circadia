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