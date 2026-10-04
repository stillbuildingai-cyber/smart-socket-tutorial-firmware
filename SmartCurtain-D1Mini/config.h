#pragma once

// ============================================================
//  Wemos D1 mini (ESP8266) 智慧窗簾 — 硬體與預設值設定
//
//  這份韌體是「番外：裝置類型擴充」的示範，刻意比 SmartSwitch-D1Mini 精簡：
//  WiFi/MQTT 連線用固定帳密，沒有做配網熱點那一套。
//  要接正式教學後台的完整配網流程，直接把 SmartSwitch-D1Mini 那份的
//  provisioning.h 風格搬過來即可，架構完全相容，這裡不重複寫一次，
//  省下來的篇幅拿來講清楚「沒有感測器，要怎麼追蹤窗簾位置」這個真正的重點。
// ============================================================

#define FW_VERSION "1.0.0"

// ---- WiFi / MQTT（教學簡化版，直接寫死）----
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define MQTT_HOST     "192.168.1.100"   // 改成你電腦的區網 IP
#define MQTT_PORT     1883
#define DEVICE_SERIAL_NO "CT-DEMO-001"           // 對應 SmartSocketDemoSeeder 建立的示範窗簾機台
#define DEVICE_API_TOKEN "tutorial-curtain-token"

// ---- 馬達控制腳位（L298N 這類 H 橋馬達驅動板）----
// ⚠ 跟 SmartSwitch-D1Mini 一樣，D3(GPIO0)/D4(GPIO2)/D8(GPIO15) 會影響開機模式，
//   馬達控制腳位一定要避開這三支。
#define PIN_MOTOR_IN1  D1   // GPIO5  → L298N IN1
#define PIN_MOTOR_IN2  D2   // GPIO4  → L298N IN2
// ENA 直接在硬體上跳線接 5V 常高即可（教學版不做速度控制，只控方向跟啟停）

// ---- 行程校正 ----
// 窗簾從全關走到全開，馬達總共要跑多久（毫秒）。
// 這個數字因每個人的窗簾軌道長度、馬達扭力不同，裝好後第一次使用務必自己實測校正，
// 校正方式：手動讓窗簾全關，送 curtain_open 指令並計時，直到窗簾完全打開為止，
// 把量到的毫秒數填進這裡（或透過校正用的 MQTT 指令，教學影片會示範）。
#define DEFAULT_FULL_TRAVEL_MS 15000UL

#define POSITION_REPORT_INTERVAL_MS 60000UL   // 跟智慧插座心跳同頻率
#define MQTT_RETRY_INTERVAL_MS       5000UL
