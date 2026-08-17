# Wemos D1 mini 智慧電源開關（教學版韌體）

用 Wemos D1 mini（ESP8266）+ 繼電器做一顆可以手機配網、後台遠端開關的智慧插座／電源開關，
搭配 [`star-cloud-tutorial`](https://github.com/stillbuildingai-cyber/star-cloud-tutorial)
這套教學後台一起使用。這份 README 假設你已經先把後台跑起來了（`docker compose up`
那一套），如果還沒有，請先回去把後台裝好、記下你電腦的區網 IP，下面會用到。

---

## 1. 材料

| 料件 | 說明 |
|---|---|
| Wemos D1 mini | ESP8266 (ESP-12F)，或相容的 LOLIN D1 mini |
| 繼電器模組 | 1 路光耦繼電器模組，5V 供電、規格至少 250VAC 10A |
| 5V 電源 | USB 供電或 5V 變壓器（要帶負載就別只靠電腦 USB） |
| 端子台／外殼 | 接市電務必用有絕緣外殼與端子台，不可裸接 |
| （選配）按鍵 | 一顆輕觸開關接 D3–GND，長按 5 秒清設定。不接也能用「連按兩次 RST」 |

> 也可以直接買 **D1 mini Relay Shield**（1 路繼電器擴充板），疊上去就好，
> 但它預設用 **D1(GPIO5)** 當控制腳，剛好就是本韌體的預設值，不用改 `config.h`。

> ⚠️ **接市電（110V）前先斷電。** 高壓側（繼電器 COM / NO）與低壓側（D1 mini）不可裸露共處，
> 燒錄時不要同時接著市電。不確定就先用 12V 燈泡或小風扇測試，確認邏輯正確再上市電。

---

## 2. 接線

```
  Wemos D1 mini            繼電器模組                    市電（斷電後再接）
 ┌─────────────┐          ┌──────────┐
 │     5V      ├──────────┤ VCC      │
 │     G       ├──────────┤ GND      │          L(火線) ──┬── COM
 │  D1 (GPIO5) ├──────────┤ IN       │                   │
 └─────────────┘          │ COM  NO  ├── NO ─────────────┴── 負載 L
                          └──────────┘          N(中性線) ──── 負載 N

  選配重設鍵：  D3 (GPIO0) ──[按鍵]── G
```

- **D1（GPIO5）→ 繼電器 IN**（可在 `config.h` 改 `PIN_RELAY`）
- **不要**把繼電器接到 D3(GPIO0) / D4(GPIO2) / D8(GPIO15)，這三支會影響開機模式。
- **觸發極性跟繼電器型號無關**（Songle SRD-05VDC-SL-C 高低觸發的板子都有），
  是看模組上的驅動電路。`config.h` 的 `RELAY_ACTIVE_LOW` 預設 `true`（多數光耦模組適用）：
  下「電源開啟」時模組 LED 應該要**亮**且聽得到「喀」一聲；
  如果剛好相反（開→燈滅、關→燈亮），把這個值對調再燒錄一次即可。
- 開機瞬間 D1 是浮接的，若你的繼電器模組在這段時間會誤動作，
  加一顆 10kΩ 電阻把 IN 拉到「不動作」那一側即可壓住
  （低電位觸發 → 對 VCC 上拉；高電位觸發 → 對 GND 下拉）。
- LED 用板載的（D4/GPIO2），**低電位亮**，`config.h` 的 `LED_ACTIVE_LOW` 已處理。

**LED 燈號**

| 燈況 | 意義 |
|---|---|
| 快閃（0.15 秒） | 配網模式，等你用手機連 `SmartSwitch-XXXX` |
| 慢閃（0.5 秒） | 正在連 WiFi |
| 很慢閃（1 秒） | WiFi 通了、MQTT 還沒連上（多半是後台位址或序號填錯） |
| 恆亮 | 一切正常，已連上後台 |

**MQTT 連線失敗代碼**（序列埠 log 的 `state=`）

| state | 意義 | 怎麼處理 |
|---|---|---|
| `4` | 帳密錯（CONNACK rc=4） | 序號或 Token 跟後台對不上 |
| `5` | 未授權（rc=5） | 後台還沒對這個序號做 `mqtt:sync-auth`（教學後台的示範機台已經處理好了，如果是你自己新增的機台要記得手動跑一次） |
| `-2` | 連不到 broker | 後台 IP／埠填錯，或裝置跟你電腦不在同一個區網 |
| `-4` | 逾時 | broker 有回應但太慢，通常是網路品質問題 |

---

## 3. 燒錄（Arduino IDE）

1. **開發板支援**：`檔案 → 偏好設定 → 額外開發板管理員網址` 加入
   `https://arduino.esp8266.com/stable/package_esp8266com_index.json`，
   再到 `工具 → 開發板 → 開發板管理員` 安裝 **esp8266 by ESP8266 Community**。
2. **函式庫**：`工具 → 管理程式庫`，安裝
   - `PubSubClient`（Nick O'Leary）
   - `ArduinoJson`（Benoit Blanchon，v7）
3. 開啟 `SmartSwitch-D1Mini/SmartSwitch-D1Mini.ino`
4. `工具` 選：
   - **開發板**：`LOLIN(WEMOS) D1 R2 & mini`
   - **Flash Size**：`4MB (FS:2MB OTA:~1019KB)` ← **一定要選有 FS 的**，
     設定檔存在 LittleFS，選 `FS:none` 會存不進去
   - **連接埠**：對應的 COM 埠（D1 mini 用 CH340，沒認到就裝 CH340 驅動）
5. 按上傳（D1 mini 不用按任何鍵，會自動進燒錄模式）。
6. 開序列埠監控視窗，鮑率 **115200**，可以看到全部運作日誌。

---

## 4. 產生機台清單（第一次跑一定要做）

`SmartSwitch-D1Mini/machines.h` 不在版控裡（含 Token，被 `.gitignore` 排除），直接編譯會找不到
`MACHINE_PRESETS` 而失敗，clone 下來後先產生一份：

```bash
cp SmartSwitch-D1Mini/machines.h.example SmartSwitch-D1Mini/machines.h
```

教學版預設值就是後台內建的示範機台（序號 `SW-DEMO-001`、Token `tutorial-demo-token`），不用改就能用。

---

## 5. 手機配網

1. D1 mini 第一次開機（或沒有設定檔時）會自己開熱點：
   **SSID `SmartSwitch-XXXX`（XXXX = MAC 末四碼）／密碼 `12345678`**
2. 手機連上這個熱點後會**自動跳出設定頁**（Captive Portal）；
   沒跳出來就用瀏覽器開 `http://192.168.4.1`
3. 頁面上填：
   - **WiFi 名稱**：下拉選單會列出附近掃到的 SSID，選了會自動填進下面的輸入框
   - **WiFi 密碼**
   - **選擇機台**：下拉選單選示範機台，序號與 Token 會自動帶入
   - **連線環境**：**一定要重新點選一次**「教學後台」（下拉顯示為已選中不會自動觸發，
     一定要親手點一次，才會把主機/埠帶進去）
   - **MQTT 主機**：⚠️ **改成你電腦的區網 IP**（跑 `docker compose up` 那台電腦）。
     Windows 用 `ipconfig` 看 IPv4 位址，Mac/Linux 用 `ifconfig` 或 `ip addr`，
     要跟手機/裝置在同一個區網（同一個 WiFi）才連得到
   - **埠**：`1883`
4. 按「儲存並重新啟動」→ 設定寫入 LittleFS → 重開機自動連線，LED 恆亮就代表成功。

### 要換 WiFi 或重設時

D1 mini 板上**沒有可用的使用者按鍵**（只有 RST），所以提供兩種方式：

- **連按兩次 RST**（預設）：按一下 RST，**5 秒內**再按一下 RST，開機就直接進配網熱點。
- **外接按鍵**（選配）：D3 對 GND 接一顆輕觸開關，長按 5 秒清除設定。

連不上 WiFi 超過 2 分鐘也會自動退回配網模式。

---

## 6. 後台整合（跟教學後台之間怎麼溝通）

### 資料流

```
後台按「電源開啟」
  → RemoteCommand(status=pending)
  → Redis: mqtt_outgoing_commands
  → Go Gateway 發布 machine/{序號}/command
  → D1 mini 收到 → 切繼電器
  → D1 mini 發 machine/{序號}/command/ack
  → Gateway → Redis: mqtt_incoming_jobs
  → Laravel mqtt:listen → ProcessCommandAck
  → RemoteCommand 變 success
```

### MQTT 契約

| 項目 | 值 |
|---|---|
| Broker | 你的教學後台 IP，`1883`（TCP，無 TLS —— 教學用途，正式上線要換成 TLS 8883） |
| Client ID | `SC_{序號}` |
| Username / Password | `序號` / `api_token` |
| Keep-alive | 30 秒（Broker 45 秒判離線） |
| LWT | `machine/{序號}/status` = `{"status":"offline"}`，QoS1 + **retain** |
| 上線 | `machine/{序號}/status` = `{"status":"online"}`，retain |
| 心跳 | `machine/{序號}/heartbeat` = `{"firmware_version":"1.0.1"}`，每 60 秒 |
| 狀態事件 | `machine/{序號}/event` = `{"event":"power_on","relay":1,"source":"command"}` |
| 接收指令 | 訂閱 `machine/{序號}/command`，QoS1 |
| 回報 | `machine/{序號}/command/ack` = `{"command_id":"89","result":"success","message":"relay on"}` |

### 支援的指令

| command | 動作 |
|---|---|
| `power_on` | 繼電器吸合（通電） |
| `power_off` | 繼電器放開（斷電） |
| `power_toggle` | 反轉目前狀態 |
| `power_status` | 不動作，只回報目前狀態 |
| `reboot` / `reboot_force` | D1 mini 重開機 |

不認得的指令會回 `result: failed`，後台指令才不會卡在 pending。

---

## 7. 沒有硬體也能測：Python 模擬器

`tools/mqtt_sim.py` 是零相依（不用裝 paho）的純 Python MQTT 客戶端，行為跟韌體一模一樣。

```bash
# 把 192.168.1.100 換成你自己電腦的區網 IP
python3 tools/mqtt_sim.py --host 192.168.1.100 --serial SW-DEMO-001 --token tutorial-demo-token

# 只發一次上線+心跳就離開（驗證後台有沒有收到）
python3 tools/mqtt_sim.py --host 192.168.1.100 --once
```

跑起來後到後台「機台管理 → SW-DEMO-001 → 電源開啟」，模擬器會印出收到的指令並回 ACK。

---

## 8. 想接第二顆以上的插座

一台實體裝置只能對應一個序號（MQTT Client ID 是 `SC_{序號}`，序號重複會互踢下線）。

- 在後台「機台管理」新增一台機台，拿到新的序號與 Token
- 手動改 `machines.h`，或用 `tools/gen_machines.py` 從 TSV 批次產生（見該檔案開頭說明）
- 每顆插座燒錄各自的韌體

---

## 授權

MIT License，供課程學員學習使用。
