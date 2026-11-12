# TiTi Relay-8 firmware

8-channel relay **output module** (ESP32-WROOM-32) for the TiTiSwitch platform. The manufacturer
registers it as a product; the subscriber uses it. It talks to the platform over **Wi-Fi / MQTT**
(the hub's `native` device contract) or sits on an **RS-485** bus as a **Modbus RTU slave**
behind a central hub or a PLC.

## Flash a ready build

`release/TiTiRelay8-<version>/` (made by `tools/build_release.ps1`, not in git) holds everything a
production PC needs, with `esptool.exe` included:

| File | What |
|---|---|
| `TiTiRelay8-<v>-full.bin` | everything for a blank chip, written at `0x0`: bootloader, partitions, firmware and the web UI |
| `TiTiRelay8-<v>-littlefs.bin` | the web UI alone (`data/`, LittleFS at `0x3D0000`): portal System page with "web UI image" on |
| `TiTiRelay8-<v>-ota.bin` | firmware only: portal System page, or a firmware release in the maker panel |
| `flash.bat COM5` | erases the chip and writes `full.bin` (Windows) |
| `flash-web.bat COM5` | writes only the web UI (`littlefs.bin` at `0x3D0000`); settings and firmware stay |
| `flash-firmware.bat COM5` | writes only the firmware (`ota.bin` at `0x10000`); settings, identity and web UI stay |
| `README-fa.md`, `SHA256SUMS.txt`, `product-template.json` | Persian flashing guide, checksums, the product for the maker panel |

After flashing, the board starts the config portal (no mode chosen yet), or takes its identity
from `hub-provision` on the production line (below).

## Build (Arduino IDE 2)

> **Choose the partition scheme before compiling.** With the default scheme the IDE stops with
> `Sketch too big ... 112% ... text section exceeds available space in board`. The program is about
> 1.47 MB (74 % of the right scheme) and needs two OTA slots: *Tools → Partition Scheme →*
> **Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)**. `sketch.json` and `sketch.yaml` select it
> automatically in Arduino IDE 2 and arduino-cli; check that the Tools menu really shows it.
> (The same scheme must be used when uploading the LittleFS image: the web UI lives at `0x3D0000`.)

| Setting | Value |
|---|---|
| Board | ESP32 Dev Module (esp32 core 3.x, tested with 3.3.10) |
| Partition scheme | **Minimal SPIFFS (1.9MB APP with OTA / 190KB SPIFFS)**: two 1.9 MB app slots, 128 KB LittleFS at `0x3D0000` |
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

## Web UI on LittleFS (separate from the firmware)

The menu pages are **not** part of the firmware. They are the files of `data/`
(`index.html`, `static/qr.js`, the Persian font `static/fonts/vazirmatn-*.woff2`, and `ca.pem` when a
device CA is shipped) and live in the LittleFS
partition (`spiffs`, 128 KB at `0x3D0000`). You can change and upload them without touching the
firmware, settings or identity. Three ways:

1. **Release folder** (the normal way): `flash-web.bat COM5` writes `littlefs.bin` (made by `tools\build_release.ps1`
   from `data/`, plus `ca.pem` if `certs\ca.crt` exists).
2. **The portal**, System page: tick "This is the web UI image (LittleFS)" and choose `littlefs.bin`.

**Fonts:** Persian text uses Vazirmatn (Google Fonts, SIL Open Font License, `docs/vazirmatn-OFL.txt`),
only its Persian/Arabic characters, regular and bold (about 21 KB each); English text and digits use Arial
from the phone or computer. Nothing is loaded from the internet (the board's own Wi-Fi has none).

**Size:** the partition is 128 KB. `data/` with the fonts is about 133 KB, so the release script
gzips `index.html` and the scripts (the board sends `*.gz` with `Content-Encoding: gzip`) and the image
holds about 70 KB. Uploading `data/` as it is (for example with the arduino-littlefs-upload plugin of
Arduino IDE 2) therefore no longer fits: use `flash-web.bat` or the portal. `mklittlefs` ends with exit code 0 even when the partition is full (the image then silently
lacks files); `build_release.ps1` therefore checks that every file is in the image and stops with an
error if not. Without the web UI the board shows a small page that can install a LittleFS image.

**Wear:** reading never wears flash. LittleFS is only read at run time (the pages in config mode, `ca.pem`
once per TLS connection); it is written only when you upload. What is written while the board runs is the
settings and the relay state in NVS (with wear leveling, and throttled: a state is stored only once it
has held 3 s and at most every 10 s).

## Production line

The maker panel exports the labels CSV of a batch with credentials
(`identifier, serial_number, claim_code, qr, device_id, device_secret`). One row belongs to one
unit: its platform identity and the QR printed on its label.

**`hub-provision`** (in the hub repository, `hub/provision.py`) programs one board per CSV row over
USB through the serial console below, and logs every row so it is never used twice:

```
hub-provision --csv labels.csv --port COM5 --broker mqtt.example.com:8883 --api https://hub.example.com --mode wifi --repeat
```

The CSV holds every unit's secret. Keep it off shared drives, and delete it when the batch is
done (it is in `.gitignore`).

**Serial console** by hand, at 115200 baud, one command per line (secrets and the claim code are
never printed back):

```
id d0123456789abcdefghij
secret <device secret>
server mqtt.example.com 8883 tls
api https://hub.example.com
wifi MyNetwork MyPassword
mode wifi
save
restart
```

Then `qr <label link>` and `serial <serial number>` store the product label. `show` prints the
settings. On this console `factory yes` (or `wipe yes`) erases everything, identity and label
included, so a board can be provisioned again (`hub-provision` refuses a board that still holds a
device id).

## Connecting a board to the server (TLS with a private CA)

1. **Labels with credentials.** In the maker panel, export the labels CSV with credentials. The
   printed label alone has only the identifier and the claim code. The CSV also has `device_id`
   and `device_secret` (shown once; exporting again rotates the secret).
2. **The device CA.** Copy `deploy/ca/ca.crt` from the server (the certificate only, never
   `ca.key`) to `certs\ca.crt` here and run `tools\build_release.ps1`: it goes into the web UI
   image as `/ca.pem`, so no board needs a per-board step. Or paste it in the portal (Connection
   page, "Custom CA certificate"); a pasted certificate wins over the shipped one.
3. **Flash** `flash.bat COM5`, then on the serial console (115200):

```
id <device_id>
secret <device_secret>
server <server address> 8883 tls
api https://<server address>
wifi <network> <password>
mode wifi
qr <the qr column of the CSV>
serial <serial number>
save
restart
```

4. The RUN LED blinks slowly; once the module is connected the DATA LED stays on. The customer
   adds the device in the app by scanning the label QR.

TLS needs a correct clock: the module waits up to 20 s for NTP before it connects. If NTP is not
reachable from the site, set another time server in the portal (Connection page).

## Product label (QR) in the portal

System page → **Product label**: the same QR as on the sticker, plus the serial number, the
identifier and the claim code (hidden until "Show code"), and a print button. The QR is drawn in
the browser by `data/static/qr.js` (a copy of the hub's `maker/js/qr.js`); the module only stores
the link (NVS, about 80 bytes). The claim code works once, for the first registration. The
sticker stays the main copy; this is the spare when it is lost or unreadable. Only someone
with the config switch and the admin password can see it.

**Lost admin password:** in the config portal, turn the config switch OFF and ON again 5 times
within 15 s. The module returns to factory settings. The customer's factory reset (this gesture
or the portal's System page) keeps the production identity (device id, platform server and
secret, hub address, CA) and the label, so the unit still reaches the platform afterwards. Only
the production console erases the identity.

## Source files (12)

| File | What |
|---|---|
| `config.h` | **All settings in one place**, with Persian comments: identity, pins, timings and limits of every part, the defaults, and the `Settings` structure |
| `core.h` | The interfaces between the parts (what each part offers to the others); light on purpose, no heavy libraries |
| `TiTiRelay8.ino` | Main program, plus the small basics: serial log, health/watchdog, inputs (DIP), LEDs, the production console |
| `settings.cpp` | Saving and loading the settings (versioned, update-safe), identity backup, label, CA |
| `relays.cpp` | Relays: command queue, interlock, pulse, spacing, state saving |
| `net.cpp` | Wi-Fi and the clock |
| `cloud.cpp` | MQTT link to the servers |
| `hubapi.cpp` | HTTPS to the hub: local key, rules, firmware update |
| `local.cpp` | Local control over the home Wi-Fi |
| `rules.cpp` | Offline rules |
| `modbus.cpp` | Modbus RTU slave |
| `portal.cpp` | The config portal (web) and the settings JSON |

To change a pin, a timing or a default, edit `config.h` only. (The files were merged from 30 to 12 so the project is quicker to build; heavy libraries such as ArduinoJson and the web server are included only by the parts that use them.)

## Pins (from the schematic)

| Function | GPIO |
|---|---|
| Relays K1..K8 (ULN2803, HIGH = ON) | 5, 18, 19, 21, 22, 23, 13, 12 |
| Relay K8 (GPIO12) | MTDI strapping pin, must be LOW at boot: the 10k pull-down on the ULN2803 input holds it, and the firmware drives it LOW first. `ENABLE_RELAY8_GPIO12 0` in `config.h` leaves it unused |
| LED DATA / RUN | 14 / 27 |
| RS-485 DE+RE (SE) | 4 |
| RS-485 RX ← RO (U2RXD) / TX → DI (U2TXD) | 16 / 17; "Swap RX/TX" in the portal if a board is wired the other way |
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
| `<prefix>d/<id>/state` | → hub | `{"state":"online","firmware":"1.0.0","local_addr":"192.168.1.50"}`, retained; LWT `{"state":"offline"}` |
| `<prefix>d/<id>/cmd` | hub → | `{"command_id","tag":"relay_3","value":true,"expires_at","pulse_s"}` |
| `<prefix>d/<id>/ack` | → hub | `received` at once, then `done` (with `value`) or `failed` (`not_fitted`, `interlock`, `expired`, `busy`, `superseded`, `unknown_tag`, `bad_value`) |
| `<prefix>d/<id>/pairing` | → hub | `{}` |
| `<prefix>d/<id>/config` | hub → | `{"type":"config","local":{"enabled","key","version","port"}}`: the local-control key |
| `<prefix>d/<id>/rules` | hub → | rules to run while cut off from the server |
| `<prefix>d/<id>/ota` | hub → | firmware offer `{job_id, version, sha256, size, url}` |
| `<prefix>d/<id>/ota/status` | → hub | `downloading`, `installed` (after the restart, by the new firmware), `failed` |
| `<prefix>d/<id>/local_log` | → hub | `{"entries":[{"at","tag","value","by"}]}`: what was switched without the server |

A command id is carried out once. A redelivered command (MQTT QoS 1) is only answered again.
Messages are kept to at most 4 per second (the hub drops more than 5).

**Hub address** (`apiUrl`, for example `https://hub.example.com`): after every connection the
module fetches `GET /ingest/v1/devices/<id>/config` and `/rules`, and it downloads firmware from
it. Requests carry `Authorization: Bearer <device secret>`.

### Local control (internet down, home Wi-Fi up)

The module serves the app's signed requests on port 8080 (`/local/v1/info`, `/nonce`, `/command`,
`/state`), as in `hub/platform/local_sign.py`. A nonce works once and for 30 s (16 open at most).
The signature is HMAC-SHA256 with the key the hub sends. The module announces itself over mDNS as
`_titiswitch._tcp` (TXT `id=<device_id>`) and puts `local_addr` in its state. Every local switch
is reported in `local_log` (at once when connected, otherwise after reconnecting).

### Offline rules

The module keeps the last rules message in flash. It runs the rules only after the server has been
unreachable for `offline_after_s` (60 s), so nothing runs twice. Supported: triggers `time`,
`interval` and `value` (with `for_s`), conditions and `window`, actions `write` and `delay`.
Calendar fields use Jalali days (weekday 0 = Saturday), months, month days, start/end and holidays,
as on the server. The clock comes from NTP, else from the rules' `server_time`. What a rule did is
reported in `local_log` (`by: "rule:<id>"`).

### Firmware from the platform

On an offer the module downloads the image from the hub. It checks the size and SHA-256, writes
it and restarts. The new firmware reports `installed` (or `failed` if the old one is still
running). Offers can be turned off in the portal.

Generic MQTT (for example the hub's `mqtt_json` driver): `<base>/telemetry` (same JSON),
`<base>/status` (`online`/`offline`), `<base>/cmd` (`{"relay_1": true}`, `{"mask": 5}` or the
platform command), `<base>/ack`.

Reconnects follow the hub contract: the first try after 0..10 s, then doubling up to 5 min, ±20 %.
A failed try moves to the next server. Expired commands (`expires_at`, once NTP time is known) are
refused.

## Product in the platform

`platform/product-template.json` is the product template for the manufacturer panel (checked
with the hub's `ProductTemplate` validator). It has the tags `relay_1`..`relay_8` (digital outputs,
command write class) and `uptime_s`, plus the commands "all off" and "all on". It also has two
bindings:

- `native`: the Wi-Fi/MQTT mode (stale after 120 s);
- `modbus_rtu`: the RS-485 mode, coils 0..7 and input register 4 (uptime), polled every 500 ms.

The tag keys the module reports are exactly `relay_1`..`relay_8` and `uptime_s`. A product defined
in the maker panel must use these keys.

Each unit then gets its device id and secret from the platform. Enter them in the portal
(Device ID, and the server's username/password).

## Modbus RTU map

"Address" is 0-based (as sent on the wire, and as Modbus Poll shows it). "PLC" is the 1-based
number most PLC tools and HMIs use (Siemens MB_MASTER / MB_CLIENT, Delta, Schneider).

| Table | Address | PLC | Access | Meaning |
|---|---|---|---|---|
| Coil (FC1/5/15) | 0..7 | 00001..00008 | read/write | relays 1..8 |
| Discrete input (FC2) | 0..6 | 10001..10007 | read | DIP AD1..AD7 |
| Discrete input | 7 | 10008 | read | config switch |
| Holding (FC3/6/16) | 0 | 40001 | read/write | relay mask (bit 0 = relay 1) |
| Holding | 1 | 40002 | write | 1 = all OFF, 2 = identify |
| Holding | 2..9 | 40003..40010 | write | pulse relay 1..8, value x 0.1 s |
| Holding | 10..19 | 40011..40020 | – | reserved (exception 02) |
| Holding | 20..27 | 40021..40028 | read/write | relay 1..8, one register each: 0 = OFF, 1 = ON (for PLCs without coil access) |
| Input (FC4) | 0 | 30001 | read | relay mask now |
| Input | 1, 2, 3 | 30002..30004 | read | DIP raw, slave address, firmware version |
| Input | 4..5 | 30005..30006 | read | uptime s (32-bit) |
| Input | 6, 7 | 30007, 30008 | read | good frames, CRC errors |
| Input | 8, 9 | 30009, 30010 | read | boot count, safe mode |
| Input | 10..25 | 30011..30026 | read | ON operations per relay, 32-bit each |

32-bit values are high word first by default; the portal's "low word first" switch reverses
them for PLCs that expect that order. The product template's `modbus_rtu` binding reads uptime with
the default order.

Also FC8 sub-function 0 (echo). Broadcast (address 0) writes are carried out without a reply.
Address 0 from the DIP switch is invalid: the board does not answer and the RUN LED shows a fault.

### Start addresses and PLC compatibility

Every table (Coil, Discrete, Input, Holding) has a **start address** in the portal (Connection →
RS-485). The address on the wire is `start address + register number`; the table in the portal
shows the result, in the 0-based form and in the 1-based PLC form (00001, 10001, 30001, 40001).

- Default: all four start at **0** (standard). Use this with the hub and with Modbus Poll.
- **"Start at 1" preset**: for PLCs or HMIs that count from 1 and put 1 on the wire for the first
  register (the usual "one register off" problem).
- **Custom**: any start (0..65535) per table, for a PLC that only reads inside a fixed range, for
  example Holding starting at 100.
- **Tolerate block reads**: a PLC that reads a block of which only part is in the map gets zeros for
  the part outside instead of exception 02. A request entirely outside the map still gets exception
  02, so a wrong address is noticed. Block writes ignore the part outside.

PLC tools write the address in the 1-based form with a prefix per table (0xxxx coils, 1xxxx discrete
inputs, 3xxxx input registers, 4xxxx holding registers) and subtract it again before sending. This is
the common convention (also for Siemens MB_CLIENT / MB_MASTER), but the vendors' own documents differ and
some tools are off by one, so test one read with Modbus Poll first and then on the PLC; if it is one
register off, use the "Start at 1" preset. Not verified against Siemens' manual here.

If the **hub** reads the board over RS-485 (the product template's `modbus_rtu` binding uses
addresses 0..), leave every start address at 0, or change the binding addresses to match.

## Industrial behaviour

- **Commands survive a power cut**: the relay state is stored in NVS and restored per relay (last
  state / OFF / ON). A state is stored once it has held 3 s, at most every 10 s, so a cycling
  timer cannot wear the flash out. A change in the last seconds before a power cut is not
  restored. A relay in a pulse is stored OFF.
- **Settings survive updates**: new settings fields are only ever appended. A new firmware reads
  the older, shorter block and gives the new fields their defaults. The production identity (device
  id, platform server and secret, hub address) and the main Wi-Fi are also kept in separate keys.
  If the settings block is ever unreadable, the unit still reaches the platform.
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

RUN shows the **health of the module**, DATA shows the **link**:

| LED | Meaning |
|---|---|
| RUN steady | config portal (and the first moments of start-up) |
| RUN slow blink (1 Hz) | healthy |
| RUN fast blink (5 Hz) | fault: safe mode, invalid Modbus address (DIP = 0), no server configured, free memory low |
| DATA steady ON | connected to the server (Wi-Fi mode) / a Modbus master was heard in the last 5 s (RS-485) |
| DATA off | no connection (RUN still blinks slowly: the module itself is fine) |
| DATA flicker | a message or frame: a short OFF while connected, a short ON while not |

Identify (portal, Modbus holding 1 = 2) flashes both LEDs alternately for 10 s.

## Not included

- HTTP and TCP transports of the device contract: MQTT only (the platform broker with the hub's
  authentication hooks is required).
- Signed SMS: the board has no GSM modem.
- Offline rules: security scenarios / sirens, and the `sun`, `once`, `offline` and `presence`
  triggers (the server does not send those to a module either).
- Automatic rollback when a new firmware does not start (needs the bootloader's rollback option).

## Hardware notes

- RS-485: the 120 Ω terminator (R52) and the 1 k bias resistors (R50/R51) are on every board. With
  several modules on one bus, fit them with jumpers and close them only at the two ends.
- Fit either the MAX485 or the ADM2682E, not both.
- The module reports the commanded relay state. It has no contact feedback and does not measure the
  24 V coil supply. A divider from +VREL to a free ADC pin would show a lost coil supply.
- No battery-backed RTC: offline time rules need the time from NTP or from the server first.
