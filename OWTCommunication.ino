
// OpenIOTCommunication.ino
//
// ThomasB
// Jul 5, 2026
//
//
// General Data Structure
// {
//  TimeStamp:;
//  Type: "";
//  Data: {
//  }
// }
//

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>

#define DEVICE_NAME "OpenWindTunnel" // CHANGE THIS

// Custom UUIDs
#define SERVICE_UUID        "a302c723-aef2-4e35-8c56-ed862688d9f3"
#define RX_CHAR_UUID        "e772e4c1-137e-4f21-b105-4957059a67ce" // Mac -> ESP32
#define TX_CHAR_UUID        "16fe65b0-eb2f-4452-9c7a-c48236e30dda" // ESP32 -> Mac

NimBLEServer* server = nullptr;
NimBLECharacteristic* txChar = nullptr;

bool deviceConnected = false;

void sendMessage(const String& message);
void SetupBLE();
bool buildOWTPacket(String& output, uint64_t timestamp, const String& type, JsonVariantConst data);
bool sendOWTPacket(uint64_t timestamp, const String& type, JsonVariantConst data);
bool sendOWTPacket(const String& type, JsonVariantConst data);
bool decodeOWTPacket(const String& json, JsonDocument& doc, uint64_t& timestamp, String& type, JsonObjectConst& data);

// ---------- Message receive callback ----------
class RxCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
    std::string value = characteristic->getValue();

    if (value.length() > 0) {
      Serial.print("Received from Mac: ");
      Serial.println(value.c_str());

      JsonDocument packetDoc;
      uint64_t timestamp = 0;
      String type;
      JsonObjectConst data;

      if (decodeOWTPacket(String(value.c_str()), packetDoc, timestamp, type, data)) {
        Serial.print("Decoded packet type: ");
        Serial.println(type);

        JsonDocument ackData;
        ackData["ReceivedType"] = type;
        ackData["ReceivedTimeStamp"] = timestamp;
        sendOWTPacket("Ack", ackData.as<JsonVariantConst>());
      } else {
        JsonDocument errorData;
        errorData["Message"] = "Invalid packet";
        sendOWTPacket("Error", errorData.as<JsonVariantConst>());
      }
    }
  }
};

// ---------- Connection callback ----------
class ServerCallback : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    deviceConnected = true;
    Serial.println("Mac connected.");

    // Ask for encrypted connection
    server->updateConnParams(connInfo.getConnHandle(), 24, 48, 0, 180);
  }

  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override {
    deviceConnected = false;
    Serial.println("Mac disconnected.");

    NimBLEDevice::startAdvertising();
  }

  uint32_t onPassKeyDisplay() override {
    uint32_t passkey = 123456;
    Serial.print("Passkey: ");
    Serial.println(passkey);
    return passkey;
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    if (!connInfo.isEncrypted()) {
      Serial.println("Encryption failed. Disconnecting.");
      NimBLEDevice::getServer()->disconnect(connInfo.getConnHandle());
      return;
    }

    Serial.println("Secure BLE connection established.");
  }
};

// ---------- Send message to Mac ----------
void sendMessage(const String& message) {
  if (!deviceConnected || txChar == nullptr) {
    Serial.println("Cannot send: no connected Mac.");
    return;
  }

  txChar->setValue(message.c_str());
  txChar->notify();

  Serial.print("Sent to Mac: ");
  Serial.println(message);
}

// ---------- BLE setup ----------
void SetupBLE() {
  NimBLEDevice::init(DEVICE_NAME);

  // Power level
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  // Security config
  NimBLEDevice::setSecurityAuth(
    true,   // bonding
    true,   // MITM protection
    true    // secure connection
  );

  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityPasskey(123456);

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallback());

  NimBLEService* service = server->createService(SERVICE_UUID);

  // RX: Mac writes to ESP32
  NimBLECharacteristic* rxChar = service->createCharacteristic(
    RX_CHAR_UUID,
    NIMBLE_PROPERTY::WRITE |
    NIMBLE_PROPERTY::WRITE_ENC
  );
  rxChar->setCallbacks(new RxCallback());

  // TX: ESP32 notifies Mac
  txChar = service->createCharacteristic(
    TX_CHAR_UUID,
    NIMBLE_PROPERTY::READ |
    NIMBLE_PROPERTY::NOTIFY |
    NIMBLE_PROPERTY::READ_ENC
  );

  txChar->setValue("ESP32 ready.");

  service->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setName(DEVICE_NAME);
  advertising->start();

  Serial.println("OpenIOTCommunication.SetupBLE succeed.");
}

// ---------- OWT packet helpers ----------
// Packet format:
// {
//   "TimeStamp": 123456,
//   "Type": "SensorReading",
//   "Data": { "anything": "anything" }
// }

bool buildOWTPacket(String& output, uint64_t timestamp, const String& type, JsonVariantConst data) {
  if (type.length() == 0 || data.isNull() || !data.is<JsonObjectConst>()) {
    Serial.println("OpenIOTCommunication.buildOWTPacket failed: invalid type or data");
    return false;
  }

  JsonDocument doc;
  doc["TimeStamp"] = timestamp;
  doc["Type"] = type;
  doc["Data"].set(data);

  output = "";
  serializeJson(doc, output);
  return true;
}

bool sendOWTPacket(uint64_t timestamp, const String& type, JsonVariantConst data) {
  if (!deviceConnected || txChar == nullptr) {
    Serial.println("Cannot send packet: no connected Mac.");
    return false;
  }

  String output;
  if (!buildOWTPacket(output, timestamp, type, data)) {
    return false;
  }

  sendMessage(output);
  return true;
}

bool sendOWTPacket(const String& type, JsonVariantConst data) {
  return sendOWTPacket((uint64_t)millis(), type, data);
}

bool decodeOWTPacket(const String& json, JsonDocument& doc, uint64_t& timestamp, String& type, JsonObjectConst& data) {
  DeserializationError error = deserializeJson(doc, json);

  if (error) {
    Serial.print("OpenIOTCommunication.decodeOWTPacket failed: ");
    Serial.println(error.c_str());
    return false;
  }

  if (!doc["TimeStamp"].is<uint64_t>() || !doc["Type"].is<const char*>() || !doc["Data"].is<JsonObjectConst>()) {
    Serial.println("OpenIOTCommunication.decodeOWTPacket failed: missing TimeStamp, Type, or Data");
    return false;
  }

  timestamp = doc["TimeStamp"].as<uint64_t>();
  type = doc["Type"].as<const char*>();
  data = doc["Data"].as<JsonObjectConst>();

  return true;
}

// // ---------- Arduino setup ----------
// void setup() {
//   Serial.begin(115200);
//   delay(1000);

//   Serial.println("Starting ESP32-C3 secure BLE accessory...");
//   SetupBLE();
// }

// // ---------- Arduino loop ----------
// void loop() {
//   static unsigned long lastSend = 0;

//   if (deviceConnected && millis() - lastSend > 5000) {
//     sendMessage("Ping from ESP32-C3");
//     lastSend = millis();
//   }

//   delay(20);
// }
