
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

#define OWT_TYPE_STATUS         "Status"
#define OWT_TYPE_EVENTS         "Events"
#define OWT_TYPE_COMMANDS       "Commands"
#define OWT_TYPE_COMMAND_RESULT "CommandResult"

NimBLEServer* server = nullptr;
NimBLECharacteristic* txChar = nullptr;

bool deviceConnected = false;

void SetupBLE();
bool buildOWTPacket(String& output, uint64_t timestamp, const String& type, JsonVariantConst data);
bool sendOWTPacket(uint64_t timestamp, const String& type, JsonVariantConst data);
bool sendOWTPacket(const String& type, JsonVariantConst data);
bool decodeOWTPacket(const String& json, JsonDocument& doc, uint64_t& timestamp, String& type, JsonObjectConst& data);
bool isValidOWTType(const String& type);
bool sendOWTEvent(const String& eventName, const String& message);
bool sendWindTunnelStatus(JsonObjectConst statusValues);
bool sendCommandResult(JsonObjectConst commandData, bool success, const String& message);
bool handleCommand(JsonObjectConst commandData);
bool notifyPacketJson(const String& packetJson);
bool packetJsonHasRequiredFields(const String& packetJson);
void setPulseWidth(int pulse_us);

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

        if (type == OWT_TYPE_COMMANDS) {
          handleCommand(data);
        } else {
          JsonDocument eventData;
          eventData["Event"] = "PacketReceived";
          eventData["ReceivedType"] = type;
          eventData["ReceivedTimeStamp"] = timestamp;
          sendOWTPacket(OWT_TYPE_EVENTS, eventData.as<JsonVariantConst>());
        }
      } else {
        sendOWTEvent("DecodeError", "Invalid packet");
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

// ---------- Notify packet JSON to Mac ----------
bool packetJsonHasRequiredFields(const String& packetJson) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, packetJson);
  if (error) {
    return false;
  }

  if (!doc["TimeStamp"].is<uint64_t>() || !doc["Type"].is<const char*>() || !doc["Data"].is<JsonObjectConst>()) {
    return false;
  }

  return isValidOWTType(doc["Type"].as<const char*>());
}

bool notifyPacketJson(const String& packetJson) {
  if (!deviceConnected || txChar == nullptr) {
    Serial.println("Cannot send packet: no connected Mac.");
    return false;
  }

  if (!packetJsonHasRequiredFields(packetJson)) {
    Serial.println("Refused to send packet without required TimeStamp, Type, and Data.");
    return false;
  }

  txChar->setValue(packetJson.c_str());
  txChar->notify();

  Serial.print("Sent to Mac: ");
  Serial.println(packetJson);
  return true;
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

  JsonDocument readyData;
  readyData["Event"] = "FirmwareReady";
  readyData["Message"] = "ESP32 ready.";
  String readyPacket;
  if (buildOWTPacket(readyPacket, (uint64_t)millis(), OWT_TYPE_EVENTS, readyData.as<JsonVariantConst>())) {
    txChar->setValue(readyPacket.c_str());
  }

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
//   "Type": "Status" | "Events" | "Commands" | "CommandResult",
//   "Data": { "anything": "anything" }
// }

bool isValidOWTType(const String& type) {
  return type == OWT_TYPE_STATUS ||
         type == OWT_TYPE_EVENTS ||
         type == OWT_TYPE_COMMANDS ||
         type == OWT_TYPE_COMMAND_RESULT;
}

bool buildOWTPacket(String& output, uint64_t timestamp, const String& type, JsonVariantConst data) {
  if (!isValidOWTType(type) || data.isNull() || !data.is<JsonObjectConst>()) {
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
  String output;
  if (!buildOWTPacket(output, timestamp, type, data)) {
    return false;
  }

  return notifyPacketJson(output);
}

bool sendOWTPacket(const String& type, JsonVariantConst data) {
  return sendOWTPacket((uint64_t)millis(), type, data);
}

bool sendOWTEvent(const String& eventName, const String& message) {
  JsonDocument data;
  data["Event"] = eventName;
  data["Message"] = message;
  return sendOWTPacket(OWT_TYPE_EVENTS, data.as<JsonVariantConst>());
}

bool sendWindTunnelStatus(JsonObjectConst statusValues) {
  if (statusValues.isNull()) {
    Serial.println("OpenIOTCommunication.sendWindTunnelStatus failed: invalid data");
    return false;
  }

  JsonDocument data;
  data["StatusType"] = "WindTunnelStatus";
  for (JsonPairConst item : statusValues) {
    data[item.key().c_str()].set(item.value());
  }

  return sendOWTPacket(OWT_TYPE_STATUS, data.as<JsonVariantConst>());
}

bool sendCommandResult(JsonObjectConst commandData, bool success, const String& message) {
  if (commandData["CommandID"].isNull()) {
    sendOWTEvent("ProtocolError", "Cannot send CommandResult: missing CommandID");
    return false;
  }

  JsonDocument data;
  data["CommandID"].set(commandData["CommandID"]);
  data["Success"] = success;
  data["Message"] = message;

  return sendOWTPacket(OWT_TYPE_COMMAND_RESULT, data.as<JsonVariantConst>());
}

bool handleCommand(JsonObjectConst commandData) {
  if (commandData["CommandID"].isNull() || !commandData["Command"].is<const char*>()) {
    sendOWTEvent("ProtocolError", "Commands packet missing CommandID or Command");
    return false;
  }

  String command = commandData["Command"].as<const char*>();

  if (command == "SetPWM") {
    if (!commandData["PulseWidthUS"].is<int>()) {
      return sendCommandResult(commandData, false, "SetPWM missing PulseWidthUS");
    }

    int pulseWidthUS = commandData["PulseWidthUS"].as<int>();
    if (pulseWidthUS < 1000 || pulseWidthUS > 2000) {
      return sendCommandResult(commandData, false, "PulseWidthUS out of range");
    }

    setPulseWidth(pulseWidthUS);
    return sendCommandResult(commandData, true, "PWM updated");
  }

  String message = "Unknown command: ";
  message += command;
  return sendCommandResult(commandData, false, message);
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

  if (!isValidOWTType(type)) {
    Serial.println("OpenIOTCommunication.decodeOWTPacket failed: invalid Type");
    return false;
  }

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
//     sendOWTEvent("Ping", "Ping from ESP32-C3");
//     lastSend = millis();
//   }

//   delay(20);
// }
