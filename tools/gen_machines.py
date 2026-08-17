#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
machines.h 產生器

從一份 TSV 清單產生韌體的機台預設清單，機台增減時不用手改 C 程式碼。
只有在你要接第二顆以上的插座、每顆都想放進配網頁面的下拉選單時才需要用這支，
只有一顆的話直接複製 machines.h.example 成 machines.h 手動填就夠了。

用法：
    python gen_machines.py machines.tsv > ../SmartSwitch-D1Mini/machines.h

TSV 格式（用 Tab 分隔，四欄，# 開頭為註解）：
    序號<TAB>API Token<TAB>備註<TAB>環境索引

    環境索引對應 environments.h 的 ENV_PRESETS[] 順序（教學版預設只有一個環境，索引固定填 0）：
        0 = 教學後台
        -1 = 不指定環境（選了只帶序號與 Token，不動主機/埠）

範例 machines.tsv：
    # 序號          Token                    備註       環境
    SW-DEMO-001     tutorial-demo-token      示範插座    0
    SW-DEMO-002     另一顆插座的token          客廳插座    0

⚠ machines.tsv 內含所有機台的 API Token，請勿提交進版控。
  已在 .gitignore 排除；若要備份請放在受控位置。
"""

import sys
import os

HEADER = '''#pragma once

// ============================================================
//  機台預設清單 —— 本檔由 tools/gen_machines.py 自動產生，請勿手改
// ============================================================
//
//  要增減機台請改 tools/machines.tsv 後重新產生：
//      python gen_machines.py machines.tsv > ../SmartSwitch-D1Mini/machines.h
//
//  「環境索引」對應 environments.h 的 ENV_PRESETS[] 順序（0 起算）。
//
//  ⚠ 一台實體裝置同一時間只能用一個序號。MQTT Client ID 是 SC_{序號}，
//    兩顆插座燒同一個序號會互相把對方踢下線，每顆要分配不同序號。
//
//  ⚠⚠ 清單裡所有 Token 都是明碼編進韌體，取得任一顆的 .bin 即可還原出
//     全部憑證。若要分送不同場域或分享給別人，建議每台燒各自的韌體
//     （TSV 只留該台那一筆），詳見 tools/gen_machines.py 的 --only 參數。

struct MachinePreset {
  const char* serial;
  const char* token;
  const char* note;
  int         env;      // ENV_PRESETS 的索引，-1 = 不指定
};

static const MachinePreset MACHINE_PRESETS[] = {
'''

FOOTER = '''};

static const size_t MACHINE_PRESET_COUNT =
    sizeof(MACHINE_PRESETS) / sizeof(MACHINE_PRESETS[0]);
'''


def c_escape(s):
    """轉義成 C 字串字面值可安全內含的形式。"""
    return s.replace('\\', '\\\\').replace('"', '\\"')


def main():
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        sys.exit(1)

    path = sys.argv[1]
    only = None
    if "--only" in sys.argv:
        i = sys.argv.index("--only")
        if i + 1 < len(sys.argv):
            only = sys.argv[i + 1]

    if not os.path.exists(path):
        sys.stderr.write("找不到清單檔：%s\n" % path)
        sys.exit(1)

    rows = []
    with open(path, encoding="utf-8") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.rstrip("\n").rstrip("\r")
            if not line.strip() or line.lstrip().startswith("#"):
                continue

            parts = [p.strip() for p in line.split("\t") if p.strip() != ""]
            if len(parts) < 2:
                sys.stderr.write("第 %d 行欄位不足（至少要序號與 Token）：%s\n" % (lineno, line))
                sys.exit(1)

            serial, token = parts[0], parts[1]
            note = parts[2] if len(parts) > 2 else ""
            try:
                env = int(parts[3]) if len(parts) > 3 else -1
            except ValueError:
                sys.stderr.write("第 %d 行的環境索引不是數字：%s\n" % (lineno, parts[3]))
                sys.exit(1)

            if only and serial != only:
                continue

            rows.append((serial, token, note, env))

    if not rows:
        sys.stderr.write("清單是空的（--only 篩選後沒有符合的機台？）\n")
        sys.exit(1)

    # 同序號同環境重複會導致設定頁出現兩筆一樣的選項，先擋掉
    seen = {}
    for serial, token, note, env in rows:
        key = (serial, env)
        if key in seen:
            sys.stderr.write("重複的序號＋環境組合：%s (env=%d)\n" % (serial, env))
            sys.exit(1)
        seen[key] = True

    # 對齊欄位讓產生出來的檔案好讀
    w_serial = max(len(r[0]) for r in rows) + 2
    w_token = max(len(r[1]) for r in rows) + 2
    w_note = max(len(r[2]) for r in rows) + 2

    out = [HEADER]
    last_env = None
    for serial, token, note, env in sorted(rows, key=lambda r: (r[3], r[0])):
        if env != last_env:
            out.append("\n  // ---- env %d ----\n" % env)
            last_env = env
        out.append('  { %s %s %s %d },\n' % (
            ('"%s",' % c_escape(serial)).ljust(w_serial + 3),
            ('"%s",' % c_escape(token)).ljust(w_token + 3),
            ('"%s",' % c_escape(note)).ljust(w_note + 3),
            env,
        ))
    out.append(FOOTER)

    sys.stdout.write("".join(out))
    sys.stderr.write("已產生 %d 台機台\n" % len(rows))


if __name__ == "__main__":
    main()
