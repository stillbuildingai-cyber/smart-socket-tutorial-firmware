#pragma once

// ============================================================
//  Wemos D1 mini (ESP8266) 智慧電源開關 — 硬體與預設值設定
//  改這裡就好，主程式 (.ino) 不用動
// ============================================================

// ---- 韌體版本（會回報到後台 machines.firmware_version）----
#define FW_VERSION "1.0.1"

// ---- 腳位（用 D1 mini 的絲印代號，會自動對應到 GPIO）----
// ⚠ D1 mini 上 D3(GPIO0) / D4(GPIO2) / D8(GPIO15) 會影響開機模式，
//   繼電器一定要接不影響開機的腳位，D1(GPIO5) 最安全。
#define PIN_RELAY   D1   // GPIO5  → 繼電器 IN
#define PIN_LED     D4   // GPIO2  → 板載藍色 LED（低電位亮）
#define PIN_BUTTON  D3   // GPIO0  → 選配：外接按鍵到 GND，長按 5 秒清設定
                         //          板上已有上拉電阻，不接也不影響

// 板載 LED 是「低電位亮」；若改接外接 LED 多半要設成 false
#define LED_ACTIVE_LOW true

// 繼電器模組的觸發極性。注意這跟繼電器型號(例如 Songle SRD-05VDC-SL-C)無關，
// 是看模組上的驅動電路怎麼接：
//   true  = 低電位觸發（IN 拉低才吸合），多數光耦模組屬於這種
//   false = 高電位觸發（IN 拉高才吸合）
// 判斷方式：下「電源開啟」時模組的 LED 應該要亮、且聽得到繼電器「喀」一聲。
// 若剛好相反，把這行的 true/false 對調即可，燒錄前無法用軟體判斷，只能實測。
#define RELAY_ACTIVE_LOW true

// 開機時繼電器的初始狀態。false = 開機一律斷電（安全預設）
// 想要「斷電復電後回到斷電前狀態」就改成 true
#define RESTORE_STATE_ON_BOOT false

// ---- 進入配網模式的方式 ----
// D1 mini 沒有可用的使用者按鍵（板上只有 RST），所以預設用「連按兩次 RST」進配網：
// 按一下 RST → 5 秒內再按一下 RST → 開機直接進配網熱點。
#define ENABLE_DOUBLE_RESET     true
#define DOUBLE_RESET_WINDOW_MS  5000UL   // 兩次 RST 要在幾毫秒內
#define ENABLE_RESET_BUTTON     true     // 有外接按鍵才用得到，沒接留著也無妨

// ---- 配網熱點 ----
#define AP_SSID_PREFIX  "SmartSwitch-"  // 後面自動接 MAC 末四碼
#define AP_PASSWORD     "12345678"      // 至少 8 碼；設為 "" 則開放無密碼
#define AP_IDLE_TIMEOUT_MS 600000UL     // 配網模式閒置 10 分鐘自動重試連線

// ---- MQTT 預設值（配網頁面可覆寫，存進 LittleFS）----
// ⚠⚠ DEFAULT_MQTT_HOST 這裡的 IP 只是範例，一定要改成你自己執行
//    `docker compose up` 那台電腦的區網 IP（Windows 用 ipconfig、Mac/Linux 用 ifconfig
//    查 IPv4 位址），不能留著範例值，否則裝置連不到你的教學後台。
#define DEFAULT_MQTT_HOST "192.168.1.100"            // 改成你電腦的區網 IP
#define DEFAULT_MQTT_PORT 1883                       // 對應教學後台 docker-compose 的 EMQX 埠
#define DEFAULT_SERIAL_NO "SW-DEMO-001"              // 教學後台內建的示範機台序號
#define DEFAULT_API_TOKEN "tutorial-demo-token"      // 教學後台內建的示範機台 Token

// ---- 時間參數 ----
#define WIFI_CONNECT_TIMEOUT_MS 30000UL   // 連 WiFi 逾時 → 退回配網模式
#define HEARTBEAT_INTERVAL_MS   60000UL   // 心跳週期
#define MQTT_RETRY_INTERVAL_MS   5000UL   // MQTT 斷線重連間隔
#define MQTT_KEEPALIVE_SEC          30    // Broker 45 秒內沒收到就觸發 LWT
#define BUTTON_HOLD_RESET_MS     5000UL   // 外接鍵長按幾毫秒算「清除設定」

// ---- 用電量分析（選配）----
// 這是選配功能：只有真的加裝了 PZEM-004T v3.0 電力監測模組才需要打開。
// 沒有這顆模組的話，保持 false，其餘程式碼會整段跳過，不影響原本功能。
// 需要額外安裝函式庫：PZEM004Tv30（作者 mandulaj），透過 Library Manager 搜尋安裝即可。
//
// 接線：PZEM-004T 的 TX/RX 接到下面兩支腳位（用 SoftwareSerial 模擬序列埠），
//   PZEM TX → PIN_PZEM_RX
//   PZEM RX → PIN_PZEM_TX
// D2(GPIO4)、D6(GPIO12) 是安全的自由腳位，不影響開機模式，跟繼電器/LED 也不衝突。
#define ENABLE_POWER_MONITORING false
#define PIN_PZEM_RX  D2   // GPIO4  ← 接 PZEM 的 TX
#define PIN_PZEM_TX  D6   // GPIO12 → 接 PZEM 的 RX
#define POWER_REPORT_INTERVAL_MS 60000UL   // 用電量回報週期，跟心跳同頻率即可
