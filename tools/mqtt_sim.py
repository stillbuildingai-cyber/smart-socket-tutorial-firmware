#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
智慧電源開關 — 純 Python MQTT 3.1.1 裝置模擬器（零相依，不用裝 paho）

用途：手上還沒有 D1 mini 硬體，或想確認後台這一段有沒有通時，
      用這支程式假扮成機台，行為與韌體完全一致：
        - 連線帶 LWT  machine/{serial}/status = {"status":"offline"} (retain)
        - 上線發 status online (retain)
        - 每 60 秒發 heartbeat
        - 訂閱 machine/{serial}/command，收到 power_on/power_off 回 ACK

用法：
    python3 mqtt_sim.py --host 192.168.1.100 --serial SW-DEMO-001 --token tutorial-demo-token
    python3 mqtt_sim.py --host 192.168.1.100 --serial SW-DEMO-001 --token tutorial-demo-token --once
"""

import argparse
import json
import socket
import struct
import sys
import threading
import time


# ---------- MQTT 3.1.1 封包組裝 ----------

def enc_len(n):
    out = b""
    while True:
        b_ = n % 128
        n //= 128
        if n > 0:
            b_ |= 0x80
        out += bytes([b_])
        if n == 0:
            return out


def enc_str(s):
    b = s.encode("utf-8")
    return struct.pack("!H", len(b)) + b


class MqttClient:
    def __init__(self, host, port, client_id, username, password,
                 will_topic=None, will_payload=None, keepalive=30):
        self.host, self.port = host, port
        self.client_id, self.username, self.password = client_id, username, password
        self.will_topic, self.will_payload = will_topic, will_payload
        self.keepalive = keepalive
        self.sock = None
        self.pid = 0
        self.running = False
        self.on_message = None

    def _next_pid(self):
        self.pid = (self.pid + 1) % 65535 or 1
        return self.pid

    def connect(self):
        self.sock = socket.create_connection((self.host, self.port), timeout=10)

        flags = 0x02                       # clean session
        payload = enc_str(self.client_id)
        if self.will_topic:
            flags |= 0x04 | 0x08 | 0x20    # will flag + will QoS1 + will retain
            payload += enc_str(self.will_topic) + enc_str(self.will_payload)
        if self.username:
            flags |= 0x80
            payload += enc_str(self.username)
        if self.password:
            flags |= 0x40
            payload += enc_str(self.password)

        vh = enc_str("MQTT") + bytes([0x04, flags]) + struct.pack("!H", self.keepalive)
        pkt = b"\x10" + enc_len(len(vh + payload)) + vh + payload
        self.sock.sendall(pkt)

        # 等 CONNACK
        hdr = self._read_packet()
        if not hdr or hdr[0] >> 4 != 2:
            raise RuntimeError("沒收到 CONNACK")
        rc = hdr[1][1]
        if rc != 0:
            raise RuntimeError("CONNACK 拒絕，return code = %d（帳密或序號不對，檢查有沒有跑過 mqtt:sync-auth）" % rc)
        self.running = True
        print("[MQTT] 已連線 %s:%d as %s" % (self.host, self.port, self.client_id))

    def publish(self, topic, payload, retain=False):
        b = payload.encode("utf-8")
        vh = enc_str(topic)
        head = 0x30 | (0x01 if retain else 0x00)     # QoS 0
        pkt = bytes([head]) + enc_len(len(vh + b)) + vh + b
        self.sock.sendall(pkt)
        print("[MQTT] -> %s %s%s" % (topic, payload, " (retain)" if retain else ""))

    def subscribe(self, topic, qos=1):
        vh = struct.pack("!H", self._next_pid())
        body = enc_str(topic) + bytes([qos])
        pkt = b"\x82" + enc_len(len(vh + body)) + vh + body
        self.sock.sendall(pkt)
        print("[MQTT] 訂閱 %s" % topic)

    def ping(self):
        self.sock.sendall(b"\xc0\x00")

    def disconnect(self):
        try:
            self.sock.sendall(b"\xe0\x00")   # 正常斷線，不觸發 LWT
            self.sock.close()
        except Exception:
            pass
        self.running = False

    def _read_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        return buf

    def _read_packet(self):
        b1 = self._read_exact(1)
        if not b1:
            return None
        mult, length = 1, 0
        while True:
            eb = self._read_exact(1)
            if not eb:
                return None
            length += (eb[0] & 127) * mult
            if not eb[0] & 128:
                break
            mult *= 128
        body = self._read_exact(length) if length else b""
        return (b1[0], body)

    def loop_forever(self):
        self.sock.settimeout(1.0)
        last_ping = time.time()
        while self.running:
            try:
                pkt = self._read_packet()
                if pkt is None:
                    print("[MQTT] 連線被關閉")
                    break
                ptype = pkt[0] >> 4
                if ptype == 3:                              # PUBLISH
                    body = pkt[1]
                    tlen = struct.unpack("!H", body[:2])[0]
                    topic = body[2:2 + tlen].decode("utf-8")
                    rest = body[2 + tlen:]
                    qos = (pkt[0] >> 1) & 0x03
                    if qos > 0:
                        rest = rest[2:]                     # 略過 packet id
                    if self.on_message:
                        self.on_message(topic, rest.decode("utf-8", "replace"))
            except socket.timeout:
                pass
            except Exception as e:
                print("[MQTT] 讀取錯誤：%s" % e)
                break

            if time.time() - last_ping >= self.keepalive / 2:
                last_ping = time.time()
                try:
                    self.ping()
                except Exception:
                    break


# ---------- 裝置行為 ----------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1", help="教學後台所在電腦的區網 IP")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--serial", default="SW-DEMO-001")
    ap.add_argument("--token", default="tutorial-demo-token")
    ap.add_argument("--fw", default="1.0.0")
    ap.add_argument("--once", action="store_true", help="發一次 online+heartbeat 就結束")
    args = ap.parse_args()

    s = args.serial
    t_status = "machine/%s/status" % s
    t_heart = "machine/%s/heartbeat" % s
    t_event = "machine/%s/event" % s
    t_cmd = "machine/%s/command" % s
    t_ack = "machine/%s/command/ack" % s

    c = MqttClient(args.host, args.port, "SC_" + s, s, args.token,
                   will_topic=t_status, will_payload='{"status":"offline"}')
    c.connect()

    relay = {"on": False}

    def on_msg(topic, payload):
        print("[MQTT] <- %s %s" % (topic, payload))
        if topic != t_cmd:
            return
        try:
            d = json.loads(payload)
        except Exception:
            print("       (不是 JSON，略過)")
            return
        cmd = d.get("command", "")
        cid = str(d.get("command_id", ""))

        if cmd in ("power_on", "power_off", "power_toggle"):
            relay["on"] = True if cmd == "power_on" else (False if cmd == "power_off" else not relay["on"])
            msg = "relay on" if relay["on"] else "relay off"
            c.publish(t_event, json.dumps({"event": "power_on" if relay["on"] else "power_off",
                                           "relay": 1 if relay["on"] else 0,
                                           "source": "command"}))
            c.publish(t_ack, json.dumps({"command_id": cid, "result": "success", "message": msg}))
            print("       ==> 繼電器現在是 %s" % ("ON" if relay["on"] else "OFF"))
        else:
            c.publish(t_ack, json.dumps({"command_id": cid, "result": "failed",
                                         "message": "unsupported command: " + cmd}))

    c.on_message = on_msg

    c.publish(t_status, '{"status":"online"}', retain=True)
    c.subscribe(t_cmd, 1)
    c.publish(t_heart, json.dumps({"firmware_version": args.fw}))

    if args.once:
        time.sleep(1)
        c.disconnect()
        print("[MQTT] 完成（--once）")
        return

    def heartbeat():
        while c.running:
            time.sleep(60)
            if c.running:
                try:
                    c.publish(t_heart, json.dumps({"firmware_version": args.fw}))
                except Exception:
                    break

    threading.Thread(target=heartbeat, daemon=True).start()

    print("模擬器執行中，Ctrl+C 結束。現在可以到後台「機台管理」對 %s 按電源開/關。" % s)
    try:
        c.loop_forever()
    except KeyboardInterrupt:
        print("\n收到 Ctrl+C，正常斷線（不觸發 LWT）")
        c.disconnect()


if __name__ == "__main__":
    main()
