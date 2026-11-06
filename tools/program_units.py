"""Program TiTi Relay-8 units on the production line from the platform's labels CSV.

The maker panel exports the CSV (POST /api/platform/products/{id}/labels with credentials):
    identifier, serial_number, claim_code, qr, device_id, device_secret
For each unit plugged in over USB the script takes the next row not yet used, sends it over the
serial console (id, secret, server, api, qr, serial, save) and records it in <csv>.done, so a
row is never given to two units. Secrets are never printed.

    pip install pyserial
    python program_units.py --csv labels.csv --port COM5 --server mqtt.example.com:8883 --tls \
        --api https://hub.example.com
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import re
import sys
import time
from pathlib import Path

import serial  # pyserial

BAUD = 115200
BOOT_WAIT_S = 3.0          # opening the port resets the ESP32; wait for its banner
REPLY_TIMEOUT_S = 3.0


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as f:
        rows = list(csv.DictReader(f))
    need = {"identifier", "qr", "device_id", "device_secret"}
    if not rows or not need <= set(rows[0]):
        sys.exit(f"{path}: needs the columns {', '.join(sorted(need))} "
                 "(export the labels with credentials)")
    return rows


def used(done: Path) -> set[str]:
    if not done.exists():
        return set()
    return {line.split(",", 1)[0] for line in done.read_text(encoding="utf-8").splitlines() if line}


def command(port: serial.Serial, line: str, quiet: str | None = None) -> str:
    """Sends one console line, returns the module's answer ("> ..." lines)."""
    port.reset_input_buffer()
    port.write((line + "\n").encode())
    port.flush()
    answer, deadline = [], time.monotonic() + REPLY_TIMEOUT_S
    while time.monotonic() < deadline:
        raw = port.readline().decode("utf-8", "replace").strip()
        if raw.startswith(">"):
            answer.append(raw[1:].strip())
            deadline = time.monotonic() + 0.3        # a few more lines may follow
    text = " | ".join(answer)
    print(f"  {quiet or line} -> {text or 'no answer'}")
    return text


def program(port_name: str, row: dict[str, str], args: argparse.Namespace) -> str:
    with serial.Serial(port_name, BAUD, timeout=0.2) as port:
        time.sleep(BOOT_WAIT_S)
        banner = port.read(port.in_waiting or 1).decode("utf-8", "replace")
        mac = m.group(1) if (m := re.search(r"MAC ([0-9A-F:]{17})", banner)) else ""
        steps = [
            (f"id {row['device_id']}", None),
            (f"secret {row['device_secret']}", "secret ********"),
            (f"server {args.server_host} {args.server_port}{' tls' if args.tls else ''}", None),
        ]
        if args.api:
            steps.append((f"api {args.api}", None))
        if args.mode:
            steps.append((f"mode {args.mode}", None))
        steps.append(("save", None))
        steps.append((f"qr {row['qr']}", "qr <label link>"))
        if row.get("serial_number"):
            steps.append((f"serial {row['serial_number']}", None))
        for line, quiet in steps:
            answer = command(port, line, quiet)
            if not answer or answer.startswith(("refused", "unknown", "storage")):
                raise RuntimeError(f"the module refused: {quiet or line}: {answer or 'no answer'}")
        show = command(port, "show")
        if row["device_id"] not in show:
            raise RuntimeError("the module does not show the new device id")
        return mac


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--csv", required=True, type=Path)
    ap.add_argument("--port", required=True, help="serial port, e.g. COM5 or /dev/ttyUSB0")
    ap.add_argument("--server", required=True, help="platform MQTT broker host[:port]")
    ap.add_argument("--tls", action="store_true")
    ap.add_argument("--api", help="hub HTTP(S) address for config, rules and firmware")
    ap.add_argument("--mode", choices=["wifi", "rs485"], help="link to preset (else chosen in the portal)")
    args = ap.parse_args()
    host, _, port = args.server.partition(":")
    args.server_host, args.server_port = host, int(port or (8883 if args.tls else 1883))

    rows = read_rows(args.csv)
    done_file = args.csv.with_suffix(args.csv.suffix + ".done")
    while True:
        taken = used(done_file)
        row = next((r for r in rows if r["identifier"] not in taken), None)
        if row is None:
            print("All rows of the CSV are used.")
            return
        left = sum(1 for r in rows if r["identifier"] not in taken)
        answer = input(f"\nNext: {row['identifier']} (serial {row.get('serial_number') or '-'}), "
                       f"{left} left. Connect the unit and press Enter (q = quit): ")
        if answer.strip().lower() == "q":
            return
        try:
            mac = program(args.port, row, args)
        except (serial.SerialException, RuntimeError) as exc:
            print(f"FAILED: {exc}\nThe row stays free; fix the problem and try again.")
            continue
        with done_file.open("a", encoding="utf-8") as f:
            f.write(f"{row['identifier']},{row['device_id']},{mac},"
                    f"{dt.datetime.now().isoformat(timespec='seconds')}\n")
        print(f"OK: {row['identifier']} programmed (MAC {mac or '?'}). "
              f"Stick the label of serial {row.get('serial_number') or row['identifier']}.")


if __name__ == "__main__":
    main()
