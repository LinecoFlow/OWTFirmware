#include <Wire.h>
#include <math.h>

const uint8_t MS4525_ADDR = 0x28;

// For many PX4/MS4525 airspeed modules: ±1 PSI
const float PSI_RANGE = 1.0f;
const float PSI_TO_PA = 6894.757f;

// Air density, room temp approximation
const float AIR_DENSITY = 1.225f;

float zeroOffsetPa = 0;

bool readMS4525(float &pressurePa, float &tempC) {
  Wire.requestFrom(MS4525_ADDR, (uint8_t)4);

  if (Wire.available() < 4) return false;

  uint8_t b0 = Wire.read();
  uint8_t b1 = Wire.read();
  uint8_t b2 = Wire.read();
  uint8_t b3 = Wire.read();

  uint8_t status = (b0 >> 6) & 0x03;
  if (status != 0) return false;

  uint16_t rawPressure = ((uint16_t)(b0 & 0x3F) << 8) | b1;
  uint16_t rawTemp = ((uint16_t)b2 << 3) | (b3 >> 5);

  // MS4525 transfer function: 10% to 90% output span
  float pressurePsi = ((rawPressure - 1638.0f) * (2.0f * PSI_RANGE) / (14745.0f - 1638.0f)) - PSI_RANGE;
  pressurePa = pressurePsi * PSI_TO_PA;

  tempC = ((rawTemp * 200.0f) / 2047.0f) - 50.0f;

  return true;
}

void AirSpeedMeterCalibrateZero() {
  Serial.println("Calibrating zero pressure. Do not blow into the tube...");

  float sum = 0;
  int count = 0;

  for (int i = 0; i < 200; i++) {
    float p, t;
    if (readMS4525(p, t)) {
      sum += p;
      count++;
    }
    delay(10);
  }

  zeroOffsetPa = sum / count;

  Serial.print("Zero offset Pa: ");
  Serial.println(zeroOffsetPa);
}

// void setup() {
//   Serial.begin(115200);
//   delay(1000);

//   Wire.begin(21, 22);   // change if needed: SDA, SCL
//   Wire.setClock(100000);

//   AirSpeedMeterCalibrateZero();
// }

// void loop() {
//   float pressurePa, tempC;

//   if (readMS4525(pressurePa, tempC)) {
//     float q = pressurePa - zeroOffsetPa;

//     // Ignore tiny noise
//     if (fabs(q) < 2.0f) q = 0;

//     float airspeed = 0;
//     if (q > 0) {
//       airspeed = sqrt((2.0f * q) / AIR_DENSITY);
//     }

//     Serial.print("Pressure Pa: ");
//     Serial.print(q, 2);
//     Serial.print(" | Airspeed m/s: ");
//     Serial.print(airspeed, 2);
//     Serial.print(" | km/h: ");
//     Serial.print(airspeed * 3.6f, 2);
//     Serial.print(" | Temp C: ");
//     Serial.println(tempC, 2);
//   } else {
//     Serial.println("Read failed");
//   }

//   delay(200);
// }
