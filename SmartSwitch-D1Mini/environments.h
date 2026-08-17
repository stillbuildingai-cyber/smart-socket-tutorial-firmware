#pragma once

// ============================================================
//  環境（MQTT Broker）預設清單
// ============================================================
//
//  配網頁面會出現「連線環境」下拉選單，選了自動帶入 MQTT 主機與埠。
//
//  教學版預設只有一個環境：你自己電腦上跑的 star-cloud-tutorial 後台。
//  IP 部分只是範例，請改成你電腦的實際區網 IP（跟 config.h 的
//  DEFAULT_MQTT_HOST 改成一樣的值）。之後如果你要接第二顆插座、
//  或另外架一個測試站，可以在這個陣列裡再加一行。

struct EnvPreset {
  const char* name;
  const char* host;
  uint16_t    port;
};

static const EnvPreset ENV_PRESETS[] = {
  { "教學後台 (改成你電腦的區網IP)", "192.168.1.100", 1883 },
};

static const size_t ENV_PRESET_COUNT =
    sizeof(ENV_PRESETS) / sizeof(ENV_PRESETS[0]);
