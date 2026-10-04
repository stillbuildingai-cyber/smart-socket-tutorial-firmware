/**
 * Wemos D1 mini 智慧窗簾 - 教學番外版
 *
 * 示範重點：沒有位置感測器的情況下，怎麼用「時間換位置」的方式追蹤窗簾開合進度，
 * 並把結果回報給跟智慧插座完全同一套的後台骨架（machine/{serial}/curtain_position）。
 *
 * 跟 SmartSwitch-D1Mini 的對照關係：
 *   - publishHeartbeat()      -> 這裡拿掉了，教學簡化版不做，有興趣可以自己加回去
 *   - publishPowerEvent()     -> 這裡對應 publishPositionReport()
 *   - onMqttMessage() 的指令判斷 -> 多了 curtain_open / curtain_close / curtain_stop / curtain_set_position
 */

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

#include "config.h"

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

String topicCommand, topicAck, topicPosition;

// ---- 位置追蹤狀態 ----
int currentPosition = -1;       // -1 代表還不知道目前位置（開機後第一次，建議先手動校正）
unsigned long fullTravelMs = DEFAULT_FULL_TRAVEL_MS;

bool motorRunning = false;
int motorDirection = 0;         // 1 = 開（走向 100），-1 = 關（走向 0）
unsigned long motorStartedAt = 0;
unsigned long motorRunForMs = 0;
int motorStartPosition = 0;
int motorTargetPosition = 0;

unsigned long lastPositionReportAt = 0;
unsigned long lastMqttRetryAt = 0;

// ============================================================
//  LittleFS：開機後位置要記得，不然每次重開都要重新校正
// ============================================================
void loadState() {
  if (!LittleFS.begin()) return;
  if (!LittleFS.exists("/state.json")) return;

  File f = LittleFS.open("/state.json", "r");
  JsonDocument doc;
  if (deserializeJson(doc, f) == DeserializationError::Ok) {
    currentPosition = doc["position"] | -1;
    fullTravelMs = doc["full_travel_ms"] | DEFAULT_FULL_TRAVEL_MS;
  }
  f.close();
}

void saveState() {
  JsonDocument doc;
  doc["position"] = currentPosition;
  doc["full_travel_ms"] = fullTravelMs;
  File f = LittleFS.open("/state.json", "w");
  serializeJson(doc, f);
  f.close();
}

// ============================================================
//  馬達控制
// ============================================================
void motorStop() {
  digitalWrite(PIN_MOTOR_IN1, LOW);
  digitalWrite(PIN_MOTOR_IN2, LOW);
}

void motorRunOpen() {
  digitalWrite(PIN_MOTOR_IN1, HIGH);
  digitalWrite(PIN_MOTOR_IN2, LOW);
}

void motorRunClose() {
  digitalWrite(PIN_MOTOR_IN1, LOW);
  digitalWrite(PIN_MOTOR_IN2, HIGH);
}

/**
 * 核心邏輯：根據「目前位置」跟「目標位置」的差距，算出馬達要跑多久，
 * 然後非阻塞地開始跑（實際停止判斷放在 loop() 裡，不能用 delay()，
 * 否則跑馬達的這幾秒鐘整台裝置會斷線、收不到新指令）。
 */
void startMoveTo(int target) {
  target = constrain(target, 0, 100);

  if (currentPosition < 0) {
    Serial.println("[CURTAIN] 目前位置未知，請先完成校正（從全關或全開開始走一次）");
    // 未知狀態下，仍然允許移動，但移動完後的位置只是「盡力估計」，
    // 建議校正方式：手動確認窗簾在全關位置，送一次 curtain_close 指令，
    // 韌體會把 currentPosition 強制視為 0 再開始計算。
  }

  int from = (currentPosition < 0) ? ((target == 0) ? 100 : 0) : currentPosition;
  int diff = target - from;
  if (diff == 0) {
    Serial.println("[CURTAIN] 已經在目標位置，不用動");
    return;
  }

  motorDirection = (diff > 0) ? 1 : -1;
  motorRunForMs = (unsigned long)(abs(diff) / 100.0 * fullTravelMs);
  motorStartPosition = from;
  motorTargetPosition = target;
  motorStartedAt = millis();
  motorRunning = true;

  if (motorDirection > 0) motorRunOpen(); else motorRunClose();

  Serial.printf("[CURTAIN] 從 %d%% 移動到 %d%%，預估耗時 %lu ms\n", from, target, motorRunForMs);
}

/**
 * 立刻停止，並根據「已經跑了多久」反推實際走到哪個位置 —— 這是 curtain_stop
 * 指令跟自然跑到終點，兩種情況共用的收尾邏輯。
 */
void stopAndUpdatePosition() {
  if (!motorRunning) return;
  motorStop();

  unsigned long elapsed = millis() - motorStartedAt;
  if (elapsed > motorRunForMs) elapsed = motorRunForMs;

  float progress = (motorRunForMs == 0) ? 1.0 : ((float)elapsed / (float)motorRunForMs);
  int moved = (int)round(progress * abs(motorTargetPosition - motorStartPosition));
  currentPosition = motorStartPosition + (motorDirection * moved);
  currentPosition = constrain(currentPosition, 0, 100);

  motorRunning = false;
  saveState();
  publishPositionReport();

  Serial.printf("[CURTAIN] 停止，目前位置估計為 %d%%\n", currentPosition);
}

void checkMotorProgress() {
  if (!motorRunning) return;
  if (millis() - motorStartedAt >= motorRunForMs) {
    motorStop();
    currentPosition = motorTargetPosition;
    motorRunning = false;
    saveState();
    publishPositionReport();
    Serial.printf("[CURTAIN] 到達目標位置 %d%%\n", currentPosition);
  }
}

// ============================================================
//  MQTT
// ============================================================
void publishPositionReport() {
  if (!mqtt.connected() || currentPosition < 0) return;
  JsonDocument doc;
  doc["position"] = currentPosition;
  String out;
  serializeJson(doc, out);
  mqtt.publish(topicPosition.c_str(), out.c_str());
  Serial.printf("[MQTT] curtain_position -> %s\n", out.c_str());
}

void publishAck(const String& commandId, const char* result) {
  if (commandId.length() == 0 || !mqtt.connected()) return;
  JsonDocument doc;
  doc["command_id"] = commandId;
  doc["result"] = result;
  String out;
  serializeJson(doc, out);
  mqtt.publish(topicAck.c_str(), out.c_str());
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  String raw;
  for (unsigned int i = 0; i < length; i++) raw += (char) payload[i];

  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return;

  String command = doc["command"] | "";
  String commandId = doc["command_id"] | "";

  if (command == "curtain_open") {
    startMoveTo(100);
    publishAck(commandId, "success");
  } else if (command == "curtain_close") {
    startMoveTo(0);
    publishAck(commandId, "success");
  } else if (command == "curtain_stop") {
    stopAndUpdatePosition();
    publishAck(commandId, "success");
  } else if (command == "curtain_set_position") {
    int target = doc["payload"]["position"] | -1;
    if (target >= 0) {
      startMoveTo(target);
      publishAck(commandId, "success");
    } else {
      publishAck(commandId, "failed");
    }
  } else {
    publishAck(commandId, "failed");
  }
}

void connectMqtt() {
  if (mqtt.connected()) return;
  if (millis() - lastMqttRetryAt < MQTT_RETRY_INTERVAL_MS) return;
  lastMqttRetryAt = millis();

  Serial.println("[MQTT] connecting...");
  if (mqtt.connect(DEVICE_SERIAL_NO, DEVICE_SERIAL_NO, DEVICE_API_TOKEN)) {
    Serial.println("[MQTT] connected");
    mqtt.subscribe(topicCommand.c_str(), 1);
    publishPositionReport();
  } else {
    Serial.printf("[MQTT] connect failed, rc=%d\n", mqtt.state());
  }
}

// ============================================================
//  Setup / Loop
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(PIN_MOTOR_IN1, OUTPUT);
  pinMode(PIN_MOTOR_IN2, OUTPUT);
  motorStop();

  loadState();

  topicCommand  = "machine/" + String(DEVICE_SERIAL_NO) + "/command";
  topicAck      = "machine/" + String(DEVICE_SERIAL_NO) + "/command/ack";
  topicPosition = "machine/" + String(DEVICE_SERIAL_NO) + "/curtain_position";

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WIFI] connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" connected, IP: " + WiFi.localIP().toString());

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);
  mqtt.setCallback(onMqttMessage);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    delay(500);
    return;
  }

  connectMqtt();
  mqtt.loop();
  checkMotorProgress();

  if (millis() - lastPositionReportAt >= POSITION_REPORT_INTERVAL_MS) {
    lastPositionReportAt = millis();
    publishPositionReport();
  }
}
