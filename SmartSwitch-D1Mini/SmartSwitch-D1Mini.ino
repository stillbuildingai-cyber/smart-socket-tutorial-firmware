/*
 * ============================================================
 *  Wemos D1 mini (ESP8266) 智慧電源開關 — TaiwanStar star-cloud 整合版
 * ============================================================
 *
 *  功能：
 *    1. 手機配網：第一次開機 / 沒設定時開熱點 StarSwitch-XXXX，
 *       手機連進去會自動跳出設定頁（Captive Portal），
 *       選 SSID、填密碼、填機台序號與 API Token，存進 LittleFS 後重開機。
 *       之後要重設：連按兩次 RST（5 秒內）即可回到配網模式。
 *    2. 遠端開關：連上 star-cloud 的 EMQX，訂閱 machine/{序號}/command，
 *       收到 power_on / power_off / power_toggle 就切繼電器，並回 ACK。
 *    3. 狀態回報：上線 status=online(retain)、LWT status=offline(retain)、
 *       每 60 秒 heartbeat、繼電器狀態變更發 event。
 *
 *  需要的函式庫（Arduino IDE → 工具 → 管理程式庫）：
 *    - PubSubClient   by Nick O'Leary
 *    - ArduinoJson    by Benoit Blanchon (v7)
 *    開發板：esp8266 by ESP8266 Community
 *    開發板選 "LOLIN(WEMOS) D1 R2 & mini"，Flash Size 要選有 FS 的（例如 4MB FS:2MB）
 *
 *  ⚠ 安全提醒：接 110V/220V 市電前務必先斷電、線材與繼電器規格要夠，
 *    高壓側(COM/NO)與低壓側(D1 mini)不可裸露共處，成品請裝進絕緣外殼。
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "config.h"
#include "environments.h"   // 先載入環境，machines.h 的 env 索引指向這裡
#include "machines.h"

#if ENABLE_POWER_MONITORING
#include <SoftwareSerial.h>
#include <PZEM004Tv30.h>
SoftwareSerial pzemSerial(PIN_PZEM_RX, PIN_PZEM_TX);
PZEM004Tv30 pzem(pzemSerial);
uint32_t lastPowerReportAt = 0;
#endif

// ============================================================
//  全域狀態
// ============================================================

ESP8266WebServer httpServer(80);
DNSServer        dnsServer;
WiFiClient       netClient;
PubSubClient     mqtt(netClient);

struct DeviceConfig {
  String ssid;
  String pass;
  String serialNo;
  String apiToken;
  String mqttHost;
  uint16_t mqttPort;
  bool relay;
} cfg;

bool     provisioningMode = false;   // true = 開熱點跑設定頁
bool     relayOn          = false;   // 繼電器目前狀態
bool     fsReady          = false;   // LittleFS 掛載成功與否
uint32_t lastHeartbeatAt  = 0;
uint32_t lastMqttRetryAt  = 0;
uint32_t provisioningStartedAt = 0;
uint32_t buttonPressedAt  = 0;
uint32_t lastLedToggleAt  = 0;
uint32_t drdArmedUntil    = 0;       // 雙擊 RST 的判定視窗
bool     ledState         = false;
String   apSsid;

// MQTT topic（連線前組好，避免每次重組字串）
String topicHeartbeat, topicStatus, topicEvent, topicCommand, topicAck, topicPowerUsage;

const char* CONFIG_PATH = "/config.json";

// ============================================================
//  設定值存取（LittleFS，ESP8266 沒有 ESP32 的 Preferences）
// ============================================================

// 把 Token 遮成「頭4…尾4 (len=N)」印到 log。
// 不印全文，但足以分辨用到的是哪一個環境的 Token
// （例如 esp3…0001 = 130、demo…ken1 = demo）。
String maskToken(const String& t) {
  if (t.length() == 0) return "(空)";
  if (t.length() <= 8) return "(len=" + String(t.length()) + ")";
  return t.substring(0, 4) + "…" + t.substring(t.length() - 4) +
         " (len=" + String(t.length()) + ")";
}

void applyDefaults() {
  cfg.ssid     = "";
  cfg.pass     = "";
  cfg.serialNo = DEFAULT_SERIAL_NO;
  cfg.apiToken = DEFAULT_API_TOKEN;
  cfg.mqttHost = DEFAULT_MQTT_HOST;
  cfg.mqttPort = DEFAULT_MQTT_PORT;
  cfg.relay    = false;
}

void loadConfig() {
  applyDefaults();

  if (!fsReady || !LittleFS.exists(CONFIG_PATH)) {
    Serial.println("[CFG] 沒有設定檔，使用預設值");
    return;
  }

  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) {
    Serial.println("[CFG] 設定檔開啟失敗");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (err) {
    Serial.printf("[CFG] 設定檔解析失敗 (%s)，改用預設值\n", err.c_str());
    return;
  }

  cfg.ssid     = doc["ssid"]     | "";
  cfg.pass     = doc["pass"]     | "";
  cfg.serialNo = doc["serial"]   | DEFAULT_SERIAL_NO;
  cfg.apiToken = doc["token"]    | DEFAULT_API_TOKEN;
  cfg.mqttHost = doc["host"]     | DEFAULT_MQTT_HOST;
  cfg.mqttPort = doc["port"]     | (uint16_t) DEFAULT_MQTT_PORT;
  cfg.relay    = doc["relay"]    | false;

  Serial.printf("[CFG] 已載入：ssid=%s serial=%s host=%s:%u token=%s\n",
                cfg.ssid.c_str(), cfg.serialNo.c_str(),
                cfg.mqttHost.c_str(), cfg.mqttPort,
                maskToken(cfg.apiToken).c_str());
}

void saveConfig() {
  if (!fsReady) {
    Serial.println("[CFG] LittleFS 未就緒，無法儲存");
    return;
  }

  JsonDocument doc;
  doc["ssid"]   = cfg.ssid;
  doc["pass"]   = cfg.pass;
  doc["serial"] = cfg.serialNo;
  doc["token"]  = cfg.apiToken;
  doc["host"]   = cfg.mqttHost;
  doc["port"]   = cfg.mqttPort;
  doc["relay"]  = cfg.relay;

  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) {
    Serial.println("[CFG] 設定檔寫入失敗");
    return;
  }
  serializeJson(doc, f);
  f.close();
  Serial.println("[CFG] 已儲存");
}

void clearConfig() {
  if (fsReady && LittleFS.exists(CONFIG_PATH)) LittleFS.remove(CONFIG_PATH);
  Serial.println("[CFG] 設定已清除");
}

// ============================================================
//  連按兩次 RST 進配網（D1 mini 沒有使用者按鍵，改用 RTC 記憶體判定）
// ============================================================

#define DRD_RTC_SLOT  0            // RTC user memory 的 4-byte 區塊編號
#define DRD_MAGIC     0xD1D10001UL // 「剛剛才開機過」的標記

void drdWrite(uint32_t v) {
  ESP.rtcUserMemoryWrite(DRD_RTC_SLOT, &v, sizeof(v));
}

uint32_t drdRead() {
  uint32_t v = 0;
  ESP.rtcUserMemoryRead(DRD_RTC_SLOT, &v, sizeof(v));
  return v;
}

// 回傳 true = 這次是「短時間內第二次開機」＝使用者連按了兩下 RST
bool checkDoubleReset() {
  if (!ENABLE_DOUBLE_RESET) return false;

  if (drdRead() == DRD_MAGIC) {
    drdWrite(0);                      // 用掉了就清掉，避免第三次開機又觸發
    Serial.println("[DRD] 偵測到連按兩次 RST → 進入配網模式");
    return true;
  }

  drdWrite(DRD_MAGIC);                // 標記「剛開機」，視窗內再開機就算雙擊
  drdArmedUntil = millis() + DOUBLE_RESET_WINDOW_MS;
  return false;
}

// ============================================================
//  繼電器 / LED
// ============================================================

void ledWrite(bool on) {
  digitalWrite(PIN_LED, LED_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW));
}

void applyRelay() {
  // RELAY_ACTIVE_LOW = true 時，要吸合(ON)就輸出 LOW
  digitalWrite(PIN_RELAY, RELAY_ACTIVE_LOW ? (relayOn ? LOW : HIGH)
                                           : (relayOn ? HIGH : LOW));
}

void publishPowerEvent(const char* source);   // 前置宣告

void setRelay(bool on, const char* source) {
  bool changed = (relayOn != on);
  relayOn = on;
  applyRelay();
  if (RESTORE_STATE_ON_BOOT && changed) {
    cfg.relay = relayOn;
    saveConfig();
  }
  Serial.printf("[RELAY] %s (source=%s)\n", on ? "ON" : "OFF", source);
  if (changed) publishPowerEvent(source);
}

// ============================================================
//  配網模式：SoftAP + Captive Portal
// ============================================================

String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if      (c == '&')  out += F("&amp;");
    else if (c == '<')  out += F("&lt;");
    else if (c == '>')  out += F("&gt;");
    else if (c == '"')  out += F("&quot;");
    else if (c == '\'') out += F("&#39;");
    else                out += c;
  }
  return out;
}

// 給嵌在 <script> 裡的字串用。序號與 Token 目前都是英數，
// 但清單是人工維護的，還是把會破壞 JS 字串的字元擋掉比較保險。
String jsEscape(const char* s) {
  String out;
  for (const char* p = s; *p; p++) {
    char c = *p;
    if (c == 0x5C || c == 0x22 || c == 0x27) out += (char) 0x5C;
    out += c;
  }
  return out;
}

// ESP8266 的 heap 只有 ~40KB，整頁組成一個 String 容易破碎，
// 所以改用 chunked 分段送出。
void handleRoot() {
  httpServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  httpServer.send(200, "text/html; charset=utf-8", "");

  httpServer.sendContent(F(
    "<!doctype html><html lang=\"zh-Hant\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>智慧電源開關設定</title><style>"
    "body{font-family:-apple-system,'Noto Sans TC',sans-serif;background:#0f172a;color:#e2e8f0;margin:0;padding:20px}"
    ".card{max-width:460px;margin:0 auto;background:#1e293b;border-radius:16px;padding:22px}"
    "h1{font-size:19px;margin:0 0 4px}.sub{font-size:12px;color:#94a3b8;margin-bottom:18px}"
    "label{display:block;font-size:13px;margin:14px 0 6px;color:#cbd5e1}"
    "input,select{width:100%;box-sizing:border-box;padding:11px;border-radius:9px;border:1px solid #334155;"
    "background:#0f172a;color:#e2e8f0;font-size:15px}"
    "button{width:100%;margin-top:22px;padding:13px;border:0;border-radius:9px;background:#2563eb;"
    "color:#fff;font-size:16px;font-weight:700}"
    ".row{display:flex;gap:10px}.row>div{flex:1}"
    ".hint{font-size:11px;color:#64748b;margin-top:5px}"
    "a{color:#60a5fa;font-size:12px}"
    "</style></head><body><div class=\"card\">"
    "<h1>智慧電源開關設定</h1>"));

  httpServer.sendContent("<div class=\"sub\">裝置：" + apSsid + " ・ 韌體 " + F(FW_VERSION) + "</div>");
  httpServer.sendContent(F(
    "<form method=\"POST\" action=\"/save\">"
    "<label>WiFi 名稱 (SSID)</label>"
    "<select name=\"ssid_pick\" onchange=\"document.getElementById('ssid').value=this.value\">"
    "<option value=\"\">— 請選擇 —</option>"));

  // 掃描結果逐筆送出，不先組成一個大字串
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    httpServer.sendContent(F("<option value=\"\">（掃不到，請用下方手動輸入）</option>"));
  } else {
    for (int i = 0; i < n && i < 25; i++) {
      String s = WiFi.SSID(i);
      if (s.length() == 0) continue;
      String esc = htmlEscape(s);
      bool lock = (WiFi.encryptionType(i) != ENC_TYPE_NONE);
      httpServer.sendContent("<option value=\"" + esc + "\"" +
                             (s == cfg.ssid ? " selected" : "") + ">" + esc +
                             " (" + String(WiFi.RSSI(i)) + "dBm" +
                             (lock ? " &#128274;" : "") + ")</option>");
      yield();
    }
  }
  WiFi.scanDelete();

  httpServer.sendContent(F(
    "</select>"
    "<div class=\"hint\">選不到就在下面直接打（隱藏 SSID 也用這格）</div>"));
  httpServer.sendContent("<input id=\"ssid\" name=\"ssid\" value=\"" + htmlEscape(cfg.ssid) +
                         "\" placeholder=\"WiFi 名稱\">");
  httpServer.sendContent("<label>WiFi 密碼</label><input name=\"pass\" type=\"password\" value=\"" +
                         htmlEscape(cfg.pass) + "\" placeholder=\"密碼\">");

  // 機台預設清單：選了就自動把序號與 Token 填進下面兩格，不用手打長長的 Token
  if (MACHINE_PRESET_COUNT > 0) {
    httpServer.sendContent(F("<label>選擇機台</label>"
                             "<select id=\"preset\" onchange=\"pick(this.value)\">"
                             "<option value=\"-1\">— 手動輸入 —</option>"));
    for (size_t i = 0; i < MACHINE_PRESET_COUNT; i++) {
      const MachinePreset& mp = MACHINE_PRESETS[i];

      // 標籤組成：序號（備註・環境）——同序號不同環境才分得出來
      String detail;
      if (strlen(mp.note) > 0) detail = mp.note;
      if (mp.env >= 0 && (size_t) mp.env < ENV_PRESET_COUNT) {
        if (detail.length() > 0) detail += "・";
        detail += ENV_PRESETS[mp.env].name;
      }
      String label = String(mp.serial);
      if (detail.length() > 0) label += "（" + detail + "）";

      // 已選中的判定要「序號＋環境＋Token」三者全中。
      // 只比序號的話，同序號不同環境的兩筆都會被標成 selected；
      // 而且下拉顯示為已選中並不會觸發 onchange，使用者若直接按儲存，
      // Token 欄位會留著舊環境的值 → 連線被回 rc=4。
      // 所以只要 Token 對不上，就讓它停在「手動輸入」，逼使用者重選一次。
      bool envMatch = (mp.env < 0) ||
                      ((size_t) mp.env < ENV_PRESET_COUNT &&
                       cfg.mqttHost == ENV_PRESETS[mp.env].host &&
                       cfg.mqttPort == ENV_PRESETS[mp.env].port);
      bool fullMatch = (cfg.serialNo == mp.serial) && envMatch &&
                       (cfg.apiToken == mp.token);

      httpServer.sendContent("<option value=\"" + String((unsigned) i) + "\"" +
                             (fullMatch ? " selected" : "") +
                             ">" + htmlEscape(label) + "</option>");
      yield();
    }
    httpServer.sendContent(F("</select>"
                             "<div class=\"hint\">⚠ 換環境時<b>一定要重新點選一次</b>——"
                             "下拉沒被點過就不會更新下面的 Token。<br>"
                             "選了會一次帶入序號、Token 與該環境的 MQTT 主機；"
                             "清單裡沒有就選「手動輸入」自己填</div>"));
  }

  httpServer.sendContent("<label>機台序號 (serial_no)</label><input id=\"serial\" name=\"serial\" value=\"" +
                         htmlEscape(cfg.serialNo) + "\" placeholder=\"例如 SW000001\">");
  httpServer.sendContent("<label>API Token</label><input id=\"token\" name=\"token\" value=\"" +
                         htmlEscape(cfg.apiToken) + "\" placeholder=\"後台機台的 api_token\">");
  // 環境預設清單：選了自動帶入 MQTT 主機與埠，換場域不用重燒
  if (ENV_PRESET_COUNT > 0) {
    httpServer.sendContent(F("<label>連線環境</label>"
                             "<select id=\"env\" onchange=\"pickEnv(this.value)\">"
                             "<option value=\"-1\">— 手動輸入 —</option>"));
    for (size_t i = 0; i < ENV_PRESET_COUNT; i++) {
      bool match = (cfg.mqttHost == ENV_PRESETS[i].host && cfg.mqttPort == ENV_PRESETS[i].port);
      httpServer.sendContent("<option value=\"" + String((unsigned) i) + "\"" +
                             (match ? " selected" : "") + ">" +
                             htmlEscape(ENV_PRESETS[i].name) + "</option>");
      yield();
    }
    httpServer.sendContent(F("</select>"));
  }

  httpServer.sendContent("<div class=\"row\"><div><label>MQTT 主機</label><input id=\"host\" name=\"host\" value=\"" +
                         htmlEscape(cfg.mqttHost) + "\"></div><div><label>埠</label>"
                         "<input id=\"port\" name=\"port\" type=\"number\" value=\"" + String(cfg.mqttPort) +
                         "\"></div></div>");
  httpServer.sendContent(F(
    "<button type=\"submit\">儲存並重新啟動</button></form>"
    "<p style=\"margin-top:18px\"><a href=\"/\">重新掃描 WiFi</a></p>"
    "</div>"));

  // 預設清單的資料與帶入邏輯（放在最後，避免拖慢頁面顯示）
  if (MACHINE_PRESET_COUNT > 0) {
    httpServer.sendContent(F("<script>var P=["));
    for (size_t i = 0; i < MACHINE_PRESET_COUNT; i++) {
      httpServer.sendContent(String(i ? "," : "") + "[\"" +
                             jsEscape(MACHINE_PRESETS[i].serial) + "\",\"" +
                             jsEscape(MACHINE_PRESETS[i].token) + "\"," +
                             String(MACHINE_PRESETS[i].env) + "]");
      yield();
    }
    // 選機台時連同該機台所屬環境的主機/埠一起帶入，避免湊出「序號對、token 卻是別的環境」的組合
    httpServer.sendContent(F("];function pick(i){i=parseInt(i);if(isNaN(i)||i<0)return;"
                             "document.getElementById('serial').value=P[i][0];"
                             "document.getElementById('token').value=P[i][1];"
                             "var e=P[i][2];"
                             "if(e>=0&&typeof E!=='undefined'&&E[e]){"
                             "document.getElementById('host').value=E[e][0];"
                             "document.getElementById('port').value=E[e][1];"
                             "var s=document.getElementById('env');if(s)s.value=e;}}</script>"));
  }

  if (ENV_PRESET_COUNT > 0) {
    httpServer.sendContent(F("<script>var E=["));
    for (size_t i = 0; i < ENV_PRESET_COUNT; i++) {
      httpServer.sendContent(String(i ? "," : "") + "[\"" +
                             jsEscape(ENV_PRESETS[i].host) + "\"," +
                             String(ENV_PRESETS[i].port) + "]");
      yield();
    }
    httpServer.sendContent(F("];function pickEnv(i){i=parseInt(i);if(isNaN(i)||i<0)return;"
                             "document.getElementById('host').value=E[i][0];"
                             "document.getElementById('port').value=E[i][1];}</script>"));
  }

  httpServer.sendContent(F("</body></html>"));
  httpServer.sendContent("");   // 結束 chunked
}

void handleSave() {
  String ssid = httpServer.arg("ssid");
  if (ssid.length() == 0) ssid = httpServer.arg("ssid_pick");

  if (ssid.length() == 0) {
    httpServer.send(200, "text/html; charset=utf-8",
      F("<meta charset=\"utf-8\"><body style=\"font-family:sans-serif;padding:24px\">"
        "沒有填 WiFi 名稱。<a href=\"/\">回上一頁</a></body>"));
    return;
  }

  cfg.ssid     = ssid;
  cfg.pass     = httpServer.arg("pass");
  cfg.serialNo = httpServer.arg("serial");
  cfg.apiToken = httpServer.arg("token");
  cfg.mqttHost = httpServer.arg("host");
  cfg.mqttPort = (uint16_t) httpServer.arg("port").toInt();
  if (cfg.mqttHost.length() == 0) cfg.mqttHost = DEFAULT_MQTT_HOST;
  if (cfg.mqttPort == 0)          cfg.mqttPort = DEFAULT_MQTT_PORT;
  saveConfig();

  httpServer.send(200, "text/html; charset=utf-8",
    "<meta charset=\"utf-8\"><body style=\"font-family:sans-serif;background:#0f172a;color:#e2e8f0;padding:28px\">"
    "<h2>已儲存</h2><p>裝置將重新啟動並連上 <b>" + htmlEscape(cfg.ssid) + "</b>。</p>"
    "<p style=\"color:#94a3b8;font-size:13px\">連線成功後板載 LED 恆亮；若 30 秒內連不上會再開回設定熱點。</p>"
    "</body>");

  delay(1200);
  ESP.restart();
}

// 手機/電腦的連線偵測網址，一律導回設定頁 → 才會自動彈出視窗
void handleCaptiveRedirect() {
  httpServer.sendHeader("Location", "http://192.168.4.1/", true);
  httpServer.send(302, "text/plain", "");
}

void startProvisioning() {
  provisioningMode = true;
  provisioningStartedAt = millis();

  WiFi.mode(WIFI_AP_STA);   // AP_STA 才能一邊開熱點一邊掃描 WiFi
  if (strlen(AP_PASSWORD) >= 8) WiFi.softAP(apSsid.c_str(), AP_PASSWORD);
  else                          WiFi.softAP(apSsid.c_str());

  delay(300);
  IPAddress apIp = WiFi.softAPIP();
  Serial.printf("[PROV] 熱點已開啟 SSID=%s IP=%s\n", apSsid.c_str(), apIp.toString().c_str());

  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", apIp);   // 所有網域都導到自己 = Captive Portal

  httpServer.on("/",     HTTP_GET,  handleRoot);
  httpServer.on("/save", HTTP_POST, handleSave);
  httpServer.on("/generate_204",        handleCaptiveRedirect);  // Android
  httpServer.on("/gen_204",             handleCaptiveRedirect);
  httpServer.on("/hotspot-detect.html", handleCaptiveRedirect);  // iOS / macOS
  httpServer.on("/ncsi.txt",            handleCaptiveRedirect);  // Windows
  httpServer.on("/connecttest.txt",     handleCaptiveRedirect);
  httpServer.onNotFound(handleCaptiveRedirect);
  httpServer.begin();
}

void stopProvisioning() {
  httpServer.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  provisioningMode = false;
}

// ============================================================
//  WiFi 連線
// ============================================================

bool connectWifi() {
  if (cfg.ssid.length() == 0) return false;

  Serial.printf("[WIFI] 連線中: %s\n", cfg.ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);                    // 憑證自己存 LittleFS，不用寫爆 flash
  WiFi.setSleepMode(WIFI_NONE_SLEEP);        // 關省電，避免 MQTT 掉線
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    ledWrite((millis() / 250) % 2);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WIFI] 已連線，IP=%s RSSI=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  Serial.println("[WIFI] 連線逾時");
  return false;
}

// ============================================================
//  MQTT
// ============================================================

void publishHeartbeat() {
  if (!mqtt.connected()) return;
  JsonDocument doc;
  doc["firmware_version"] = FW_VERSION;
  String out;
  serializeJson(doc, out);
  mqtt.publish(topicHeartbeat.c_str(), out.c_str());
  Serial.printf("[MQTT] heartbeat -> %s\n", out.c_str());
}

#if ENABLE_POWER_MONITORING
void publishPowerUsage() {
  if (!mqtt.connected()) return;

  float watt = pzem.power();
  float kwhTotal = pzem.energy();

  // PZEM 讀取失敗時，power()/energy() 會回傳 NAN，這種情況直接跳過這次回報，
  // 不要把 NAN 送上雲端污染資料（下一個週期會再試一次）。
  if (isnan(watt) || isnan(kwhTotal)) {
    Serial.println("[PZEM] read failed, skip this report");
    return;
  }

  JsonDocument doc;
  doc["watt"] = watt;
  doc["kwh_total"] = kwhTotal;
  String out;
  serializeJson(doc, out);
  mqtt.publish(topicPowerUsage.c_str(), out.c_str());
  Serial.printf("[MQTT] power_usage -> %s\n", out.c_str());
}
#endif

void publishStatus(const char* status) {
  if (!mqtt.connected()) return;
  String payload = String("{\"status\":\"") + status + "\"}";
  mqtt.publish(topicStatus.c_str(), payload.c_str(), true);   // retain，和 Android app 一致
}

void publishPowerEvent(const char* source) {
  if (!mqtt.connected()) return;
  JsonDocument doc;
  doc["event"]  = relayOn ? "power_on" : "power_off";
  doc["relay"]  = relayOn ? 1 : 0;
  doc["source"] = source;
  doc["rssi"]   = WiFi.RSSI();
  String out;
  serializeJson(doc, out);
  mqtt.publish(topicEvent.c_str(), out.c_str());
  Serial.printf("[MQTT] event -> %s\n", out.c_str());
}

void publishAck(const String& commandId, const char* result, const String& message) {
  if (commandId.length() == 0 || !mqtt.connected()) return;
  JsonDocument doc;
  doc["command_id"] = commandId;
  doc["result"]     = result;          // "success" / "failed"
  doc["message"]    = message;
  String out;
  serializeJson(doc, out);
  mqtt.publish(topicAck.c_str(), out.c_str());
  Serial.printf("[MQTT] ack -> %s\n", out.c_str());
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  String raw;
  raw.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) raw += (char) payload[i];
  Serial.printf("[MQTT] 收到 %s : %s\n", topic, raw.c_str());

  JsonDocument doc;
  if (deserializeJson(doc, raw)) {
    Serial.println("[MQTT] JSON 解析失敗，略過");
    return;
  }

  String command   = doc["command"]    | "";
  String commandId = doc["command_id"] | "";

  if (command == "power_on") {
    setRelay(true, "command");
    publishAck(commandId, "success", "relay on");

  } else if (command == "power_off") {
    setRelay(false, "command");
    publishAck(commandId, "success", "relay off");

  } else if (command == "power_toggle") {
    setRelay(!relayOn, "command");
    publishAck(commandId, "success", relayOn ? "relay on" : "relay off");

  } else if (command == "power_status") {
    publishPowerEvent("query");
    publishAck(commandId, "success", relayOn ? "relay on" : "relay off");

  } else if (command == "reboot" || command == "reboot_force") {
    publishAck(commandId, "success", "rebooting");
    publishStatus("restarting");
    mqtt.loop();
    delay(600);
    ESP.restart();

  } else {
    // 不認得的指令回 failed，後台指令才不會一直卡 pending
    publishAck(commandId, "failed", "unsupported command: " + command);
  }
}

bool connectMqtt() {
  if (cfg.serialNo.length() == 0) {
    Serial.println("[MQTT] 尚未設定機台序號，不連線");
    return false;
  }

  String clientId = "SC_" + cfg.serialNo;           // 與 Android app 同慣例
  String willMsg  = "{\"status\":\"offline\"}";

  Serial.printf("[MQTT] 連線 %s:%u as %s\n",
                cfg.mqttHost.c_str(), cfg.mqttPort, clientId.c_str());

  bool ok = mqtt.connect(
      clientId.c_str(),
      cfg.serialNo.c_str(),           // username = 機台序號
      cfg.apiToken.c_str(),           // password = api_token
      topicStatus.c_str(), 1, true,   // LWT: QoS1 + retain
      willMsg.c_str(),
      true);                          // cleanSession

  if (!ok) {
    Serial.printf("[MQTT] 連線失敗，state=%d\n", mqtt.state());
    return false;
  }

  Serial.println("[MQTT] 連線成功");
  publishStatus("online");
  mqtt.subscribe(topicCommand.c_str(), 1);
  publishHeartbeat();
  publishPowerEvent("connected");
  lastHeartbeatAt = millis();
  return true;
}

void buildTopics() {
  topicHeartbeat   = "machine/" + cfg.serialNo + "/heartbeat";
#if ENABLE_POWER_MONITORING
  topicPowerUsage  = "machine/" + cfg.serialNo + "/power_usage";
#endif
  topicStatus    = "machine/" + cfg.serialNo + "/status";
  topicEvent     = "machine/" + cfg.serialNo + "/event";
  topicCommand   = "machine/" + cfg.serialNo + "/command";
  topicAck       = "machine/" + cfg.serialNo + "/command/ack";
}

// ============================================================
//  LED 指示 / 重設鍵
// ============================================================

void updateLed() {
  uint32_t interval;
  if (provisioningMode)                   interval = 150;   // 快閃 = 等你來配網
  else if (WiFi.status() != WL_CONNECTED) interval = 500;   // 慢閃 = 連 WiFi 中
  else if (!mqtt.connected())             interval = 1000;  // 很慢閃 = WiFi OK、MQTT 未連
  else {                                                     // 恆亮 = 一切正常
    ledWrite(true);
    return;
  }
  if (millis() - lastLedToggleAt >= interval) {
    lastLedToggleAt = millis();
    ledState = !ledState;
    ledWrite(ledState);
  }
}

void checkResetButton() {
  if (!ENABLE_RESET_BUTTON) return;

  if (digitalRead(PIN_BUTTON) == LOW) {          // 按下為 LOW（板上已有上拉）
    if (buttonPressedAt == 0) buttonPressedAt = millis();
    else if (millis() - buttonPressedAt >= BUTTON_HOLD_RESET_MS) {
      Serial.println("[BTN] 長按 → 清除設定並重開機");
      for (int i = 0; i < 10; i++) { ledWrite(i % 2); delay(80); }
      clearConfig();
      ESP.restart();
    }
  } else {
    buttonPressedAt = 0;
  }
}

// ============================================================
//  setup / loop
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(F("=== Wemos D1 mini 智慧電源開關 " FW_VERSION " ==="));

  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_LED,   OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  ledWrite(false);

  bool doubleReset = checkDoubleReset();

  fsReady = LittleFS.begin();
  if (!fsReady) {
    Serial.println("[FS] LittleFS 掛載失敗，嘗試格式化…");
    if (LittleFS.format() && LittleFS.begin()) {
      fsReady = true;
      Serial.println("[FS] 格式化完成");
    } else {
      Serial.println("[FS] 格式化失敗，設定將無法保存（檢查 Flash Size 有沒有選到含 FS 的選項）");
    }
  }

  loadConfig();
  relayOn = RESTORE_STATE_ON_BOOT ? cfg.relay : false;
  applyRelay();                       // 先把繼電器打到已知狀態，避免上電抖動

  uint8_t mac[6];
  WiFi.macAddress(mac);
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%02X%02X", mac[4], mac[5]);
  apSsid = String(AP_SSID_PREFIX) + suffix;

  buildTopics();
  mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(1024);           // 預設 256 太小，指令 payload 可能被截斷
  mqtt.setKeepAlive(MQTT_KEEPALIVE_SEC);

  if (doubleReset) {
    startProvisioning();              // 使用者連按兩次 RST，直接進設定
  } else if (!connectWifi()) {
    startProvisioning();              // 沒設定或連不上 → 開熱點讓手機來設定
  }
}

void loop() {
  // 雙擊 RST 的判定視窗過了就清掉標記，之後單獨按一次 RST 不會誤觸發
  if (drdArmedUntil && millis() > drdArmedUntil) {
    drdWrite(0);
    drdArmedUntil = 0;
  }

  checkResetButton();
  updateLed();

  if (provisioningMode) {
    dnsServer.processNextRequest();
    httpServer.handleClient();

    // 閒置太久就再試一次連線（例如路由器只是暫時斷電）
    if (cfg.ssid.length() > 0 && millis() - provisioningStartedAt > AP_IDLE_TIMEOUT_MS) {
      Serial.println("[PROV] 閒置逾時，重試 WiFi 連線");
      stopProvisioning();
      if (!connectWifi()) startProvisioning();
    }
    yield();
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    // WiFi 掉線：交給 setAutoReconnect 重連，逾時才退回配網模式
    static uint32_t droppedAt = 0;
    if (droppedAt == 0) droppedAt = millis();
    if (millis() - droppedAt > WIFI_CONNECT_TIMEOUT_MS * 4) {
      Serial.println("[WIFI] 長時間連不上，退回配網模式");
      droppedAt = 0;
      startProvisioning();
    }
    delay(50);
    return;
  }

  if (!mqtt.connected()) {
    if (millis() - lastMqttRetryAt >= MQTT_RETRY_INTERVAL_MS) {
      lastMqttRetryAt = millis();
      connectMqtt();
    }
  } else {
    mqtt.loop();
    if (millis() - lastHeartbeatAt >= HEARTBEAT_INTERVAL_MS) {
      lastHeartbeatAt = millis();
      publishHeartbeat();
    }
#if ENABLE_POWER_MONITORING
    if (millis() - lastPowerReportAt >= POWER_REPORT_INTERVAL_MS) {
      lastPowerReportAt = millis();
      publishPowerUsage();
    }
#endif
  }

  yield();   // ESP8266 要定期讓出 CPU 餵看門狗
}
