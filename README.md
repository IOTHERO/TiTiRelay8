# TiTi Relay-8 firmware

8-channel relay **output module** (ESP32-WROOM-32) for the TiTiSwitch platform. The manufacturer
registers it as a product; the subscriber uses it. It talks to the platform over **Wi-Fi / MQTT**
(the hub's `native` device contract) or sits on an **RS-485** bus as a **Modbus RTU slave**
behind a central hub or a PLC.

## Build (Arduino IDE 2)

| Setting | Value |
|---|---|
| Board | ESP32 Dev Module (esp32 core 3.x, tested with 3.3.10) |
| Partition scheme | **Minimal SPIFFS (1.9MB APP with OTA / 190KB SPIFFS)** |
| Libraries | ArduinoJson 7, ESP Async WebServer 3.x, Async TCP 3.x (MQTT and Modbus are built in) |

1. Open `TiTiRelay8.ino`, choose the settings above, **Upload** (firmware).
2. Upload the web UI to flash: install the
   [arduino-littlefs-upload](https://github.com/earlephilhower/arduino-littlefs-upload) plugin,
   close the Serial Monitor, then `Ctrl+Shift+P` → **Upload LittleFS to Pico/ESP8266/ESP32**.
   It writes the `data/` folder (`index.html`). Without it the portal shows a fallback page that
   can install a LittleFS `.bin` image.
3. Later updates: System page → firmware `.bin`, or the LittleFS image with the
   "web UI image" switch on.

The serial log runs at 115200 baud.

## Pins (from the schematic)

| Function | GPIO |
|---|---|
| Relays K1..K7 (ULN2803, HIGH = ON) | 5, 18, 19, 21, 22, 23, 13 |
| Relay K8 | 12, **not used** (strapping pin MTDI). `ENABLE_RELAY8_GPIO12` in `board.h` |
| LED DATA / RUN | 14 / 27 |
| RS-485 DE+RE (SE) | 4 |
| RS-485 TX → DI / RX ← RO | 16 / 17 (net names ADM_TXD / ADM_RXD); "Swap RX/TX" in the portal |
| DIP AD1..AD7 (address, AD1 = bit 0) | 36, 39, 34, 35, 32, 33, 25 |
| DIP AD8 = config switch | 26 |

## Scenario

1. **Config switch (DIP 8) ON, power up** → config mode: RUN LED blinks fast. The board opens
   Wi-Fi `TiTi-R8-XXXX` (default password `titi1234`). Connect to it and open
   `http://192.168.1.100/`. A captive portal opens it by itself on most phones. Admin password:
   `admin` (change it on the System page).
2. **Connection** page: choose **Wi-Fi / MQTT** or **RS-485 / Modbus RTU**.
3. Wi-Fi: scan, pick the network, enter the password, optional backup network, DHCP or static IP,
   NTP. Then the servers: up to 4, tried in order. Each server has protocol, host, port, TLS,
   user/password, client id, topic prefix, keepalive and QoS. **Save**, then **Test Wi-Fi** /
   **Test** a server.
4. RS-485: address from the DIP switch (live) or manual, baud, parity, stop bits, response delay,
   RX/TX swap. The register map is on the page.
5. Both modes: **link loss** action (keep, all OFF, preset) after N seconds without server/master.
6. **Relays & test**: one button per relay, live state, pulse, all ON/OFF, walk test, identify,
   live DIP/config inputs. Relay settings: name, power-on behaviour (last state / OFF / ON),
   interlock pairs, minimum interval, coil stagger.
7. **Restart**: the dialog shows the config switch live and asks whether it was turned off.
   With it OFF the board starts in the chosen mode.

With no mode chosen yet, the portal opens even with the switch OFF (first start).
In Wi-Fi mode, turning the config switch ON for 3 s while running sends a **pairing** request.

## Platform (Wi-Fi) — native device contract

Username = device id, password = device secret (issued by the platform for each unit).

| Topic | Direction | Payload |
|---|---|---|
| `<prefix>d/<id>/telemetry` | → hub | `{"ts": <ms>, "values": {"relay_1": true, ..., "uptime_s": 123}}` on change and every N s |
| `<prefix>d/<id>/state` | → hub | `{"state":"online"}`, retained; LWT `{"state":"offline"}` |
| `<prefix>d/<id>/cmd` | hub → | `{"command_id","tag":"relay_3","value":true,"expires_at","pulse_s"}` |
| `<prefix>d/<id>/ack` | → hub | `received` at once, then `done` (with `value`) or `failed` (`not_fitted`, `interlock`, `expired`, `busy`, `superseded`, `unknown_tag`, `bad_value`) |
| `<prefix>d/<id>/pairing` | → hub | `{}` |

Generic MQTT (for example the hub's `mqtt_json` driver): `<base>/telemetry` (same JSON),
`<base>/status` (`online`/`offline`), `<base>/cmd` (`{"relay_1": true}`, `{"mask": 5}` or the
platform command), `<base>/ack`.

Reconnects follow the hub contract: the first try after 0..10 s, then doubling up to 5 min, ±20 %.
A failed try moves to the next server. Expired commands (`expires_at`, once NTP time is known) are
refused.

## Product in the platform

`platform/product-template.json` is the product template for the manufacturer panel (checked
with the hub's `ProductTemplate` validator). It has the tags `relay_1`..`relay_7` (digital outputs,
command write class) and `uptime_s`, plus the commands "all off" and "all on". It also has two
bindings:

- `native`: the Wi-Fi/MQTT mode (stale after 120 s);
- `modbus_rtu`: the RS-485 mode, coils 0..6 and input register 4 (uptime), polled every 500 ms.

Each unit then gets its device id and secret from the platform. Enter them in the portal
(Device ID, and the server's username/password).

## Modbus RTU map (0-based addresses)

| Table | Address | Meaning |
|---|---|---|
| Coils (FC1/5/15) | 0..7 | relays 1..8 |
| Discrete inputs (FC2) | 0..6 / 7 | DIP AD1..AD7 / config switch |
| Holding (FC3/6/16) | 0 | relay mask (bit 0 = relay 1) |
| | 1 | write 1 = all OFF, 2 = identify |
| | 2..9 | pulse relay 1..8, value × 0.1 s |
| Input (FC4) | 0 | relay mask now |
| | 1, 2, 3 | DIP raw, slave address, firmware version |
| | 4..5 | uptime s (high, low) |
| | 6, 7 | good frames, CRC errors |
| | 8, 9 | boot count, safe mode |
| | 10..25 | ON operations per relay, 32-bit (high, low) |

Also FC8 sub-function 0 (echo). Broadcast (address 0) writes are carried out without a reply.
Address 0 from the DIP switch is invalid: the board does not answer and the RUN LED shows a fault.

## Industrial behaviour

- **Commands survive a power cut**: the relay state is written to NVS on every change and restored
  per relay (last state / OFF / ON). A relay in a pulse is stored OFF.
- **Task watchdog** (8 s) on the main loop and the Modbus task. Reset reason logged.
  **3 abnormal resets in a row → safe mode**: outputs start OFF until a stable minute.
- **Free-heap watch**: a controlled restart if it stays low for 30 s.
- All relay changes go through one queue in the main loop: the per-relay minimum interval
  (anti-chatter), a coil stagger (inrush), interlock pairs (partner released first).
- Relay ON-cycle counters (relay wear), saved every 10 min and before a restart.
- Wi-Fi reconnect with back-off and a backup network; MQTT LWT, server failover.
- Modbus frames are found by length and line silence (t3.5); frames for other slaves are skipped.
- DIP inputs debounced; the config switch is read by majority vote at power-up.
- Serial log: level per setting, one line per event, repeats folded
  ("previous message repeated N times"). The last lines also show in the portal.
- Portal: login with lockout, session tokens, settings validation, backup/restore, OTA,
  factory reset (type `RESET`).

## LEDs

| RUN LED | Meaning |
|---|---|
| steady | starting |
| fast blink | config portal |
| slow blink (1 s) | link OK |
| double blink | no server / no Modbus master |
| triple blink | safe mode or invalid address |

DATA flashes on every message or frame. Identify flashes both LEDs.

## Not included yet

Local rules while offline (`d/<id>/rules`), signed local control over the home network, and
firmware offers from the platform (`d/<id>/ota`). Updates are done from the portal.
