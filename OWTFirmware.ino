
#include <HX711.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <math.h>

#define HX711_DT   7
#define HX711_SCK  5
const float HX711_SCALE = 1067.3;
float HX711_EXTRA_OFFSET = 0.0f;

const int PWM_PIN = 4;
const int PWM_FREQ = 40;
const int PWM_RES = 14;

HX711 scale;

// AirSpeedMeter.ino is appended after this primary sketch by Arduino. Declare
// its shared interface here so setup() and loop() can use it before its
// definitions appear in the generated translation unit.
extern const float AIR_DENSITY;
extern float zeroOffsetPa;
bool readMS4525(float &pressurePa, float &tempC);
void AirSpeedMeterCalibrateZero();

// OWTCommunication.ino is also appended after this primary sketch. Declare its
// shared interface here so setup() and loop() can call it.
void SetupBLE();
void sendMessage(const String& message);
bool buildOWTPacket(String& output, uint64_t timestamp, const String& type, JsonVariantConst data);
bool sendOWTPacket(uint64_t timestamp, const String& type, JsonVariantConst data);
bool sendOWTPacket(const String& type, JsonVariantConst data);
bool decodeOWTPacket(const String& json, JsonDocument& doc, uint64_t& timestamp, String& type, JsonObjectConst& data);

void setPulseWidth(int pulse_us) {
  uint32_t maxDuty = (1UL << PWM_RES) - 1;
  // duty = pulse width / period
  uint32_t duty = (uint32_t)((pulse_us * (uint32_t)PWM_FREQ * maxDuty) / 1000000UL);
  Serial.print("pulse_us = ");
  Serial.print(pulse_us);
  Serial.print(" | duty = ");
  Serial.println(duty);
  ledcWrite(PWM_PIN, duty);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("OpenWindTunnel Starting...");

  // Weight Sensor
  scale.begin(HX711_DT, HX711_SCK);

  Serial.println("Checking HX711...");
  delay(1000);
  if (!scale.is_ready()) {
    Serial.println("HX711 not found. Check wiring.");
    while (1) {
      delay(1000);
    }
  }

  Serial.println("HX711 found.");

  Serial.println("Taring... remove all weight.");
  delay(2000);
  scale.tare();
  scale.set_scale(HX711_SCALE);  // CHANGE THIS

  float TotalWeight = 0.0f;
  for (int i = 0; i < 6; i++) {
      if (!scale.is_ready()) {
          delay(50);
      }
      TotalWeight += scale.get_units(2);
      delay(100);
  }
  HX711_EXTRA_OFFSET = TotalWeight / 5;

  Serial.println("Tare done.");

  // init PWM for motor
  bool ok = ledcAttach(PWM_PIN, PWM_FREQ, PWM_RES);

  if (!ok) {
    Serial.println("PWM attach failed.");
    while (1) delay(1000);
  }

  setPulseWidth(1500); // Stop width, CHANGE THIS
  Serial.println("PWM ready.");

  // Communication
  Serial.println("Starting ESP32-C3 secure BLE accessory...");
  SetupBLE();

  // MS4525 AirSpeedMeter
  Wire.begin(20, 21);  // SDA, SCL
  Wire.setClock(100000);

  AirSpeedMeterCalibrateZero();
}

void loop() {
  // LoopStart
  uint32_t StartTime = millis();

  // Weight
  if (!scale.is_ready()) {
      delay(20);
  }
  float grams = scale.get_units(2) - HX711_EXTRA_OFFSET;
  Serial.print("Weight g: ");
  Serial.println(grams);

  // AirSpeed
  float pressurePa = 0.0f;
  float tempC = 0.0f;

  if (readMS4525(pressurePa, tempC)) {
    float q = pressurePa - zeroOffsetPa;

    // Ignore tiny noise
    if (fabs(q) < 2.0f) q = 0;

    float airspeed = 0;
    if (q > 0) {
    airspeed = sqrt((2.0f * q) / AIR_DENSITY);
    }

    Serial.print("Pressure Pa: ");
    Serial.print(q, 2);
    Serial.print(" | Airspeed m/s: ");
    Serial.print(airspeed, 2);
    Serial.print(" | km/h: ");
    Serial.print(airspeed * 3.6f, 2);
    Serial.print(" | Temp C: ");
    Serial.println(tempC, 2);
  } else {
    Serial.println("Read failed");
  }

  // Serial Commands for PWM
  if (Serial.available()) {
    int speed = Serial.parseInt();
    if (speed >= 1200 && speed <= 1500) {
      Serial.print("Moving to ");
      Serial.println(speed);
      setPulseWidth(speed);
    } else {
      Serial.println("Invalid input (0-100)");
    }
    // Clear any remaining characters
    while (Serial.available()) {
      Serial.read();
    }
  }

  // Loop time monotoring
  uint32_t TimePassed = millis() - StartTime;
  Serial.print("LoopTime ms: ");
  Serial.println(TimePassed);
}
