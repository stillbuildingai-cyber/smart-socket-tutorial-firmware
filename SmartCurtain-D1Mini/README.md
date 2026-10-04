# Wemos D1 mini 智慧窗簾（番外：裝置類型擴充示範）

這是「自己動手蓋雲端主機」系列之外，另一個番外示範：證明 `star-cloud-tutorial`
這套後台骨架不是只能管智慧插座，同一套 Machine／MQTT／RemoteCommand 架構，
換個裝置類型照樣撐得住。

跟 [`SmartSwitch-D1Mini`](../SmartSwitch-D1Mini) 的差異：
- 這份韌體刻意精簡掉配網熱點（WiFi provisioning）那一套，WiFi／MQTT 直接寫死在 `config.h`，
  把篇幅留給真正的示範重點：**沒有位置感測器，要怎麼知道窗簾現在開合到哪裡**
- 多了 `curtain_open` / `curtain_close` / `curtain_stop` / `curtain_set_position` 四種指令
- 跟後台之間多一條 `machine/{serial}/curtain_position` 回報通道

## 1. 材料

| 料件 | 說明 |
|---|---|
| Wemos D1 mini | 跟插座那份共用同一顆開發板 |
| L298N 馬達驅動板 | 或其他 H 橋驅動板，用來控制窗簾馬達的正反轉 |
| 直流馬達（或窗簾專用馬達） | 依你實際窗簾軌道規格選用 |
| 5V／12V 電源 | 依馬達規格決定，注意跟 D1 mini 分開供電，只共地 |

## 2. 接線

```
  Wemos D1 mini          L298N
 ┌─────────────┐        ┌──────────┐
 │ D1 (GPIO5)  ├────────┤ IN1      │
 │ D2 (GPIO4)  ├────────┤ IN2      │
 │     G       ├────────┤ GND      │          馬達電源另外接，
 └─────────────┘        │ ENA      │          只跟 D1 mini 共地
                         │ (跳線接5V常高)
                         │ OUT1 OUT2├── 接窗簾馬達
                         └──────────┘
```

## 3. 位置校正（第一次使用一定要做）

這份韌體沒有裝位置感測器，用「馬達跑多久 = 走了多少百分比」的方式估算位置，
所以第一次使用前要先量出窗簾從全關到全開實際要跑多久：

1. 手動把窗簾調整到完全關閉的位置
2. 送一次 `curtain_open` 指令，同時開始計時
3. 窗簾完全打開的瞬間，記下經過的毫秒數
4. 把這個數字填進 `config.h` 的 `DEFAULT_FULL_TRAVEL_MS`，重新燒錄

校正完成後，裝置會把目前位置存進 LittleFS，重開機也不會遺失。

## 4. 跟後台的 MQTT 契約

| 方向 | Topic | 內容 |
|---|---|---|
| 裝置 → 後台 | `machine/{serial}/curtain_position` | `{"position": 0-100}` |
| 後台 → 裝置 | `machine/{serial}/command` | `{"command": "curtain_open｜curtain_close｜curtain_stop｜curtain_set_position", "command_id": "...", "payload": {"position": 0-100}}`（只有 set_position 需要 payload） |
| 裝置 → 後台 | `machine/{serial}/command/ack` | `{"command_id": "...", "result": "success｜failed"}` |

對應的後台示範機台序號／Token 已經寫在 `database/seeders/SmartSocketDemoSeeder.php`
裡的 `DEMO_CURTAIN_SERIAL` / `DEMO_CURTAIN_TOKEN`，執行過 seeder 就可以直接用。
