# canary

**A $4 dead man's switch for your home.** An ESP32 sends a heartbeat every minute. When the power goes out, the freezer warms up or water reaches the floor, the heartbeat stops or turns into a failure, and your heartbeat monitor alerts your phone.

[![CI](https://github.com/AndreaAcanfora/canary/actions/workflows/ci.yml/badge.svg)](https://github.com/AndreaAcanfora/canary/actions/workflows/ci.yml)

**[Install it from the browser →](https://andreaacanfora.github.io/canary/)** Plug the board in with USB, click Install, pick your Wi-Fi. The page also has a simulator, so you can see what happens without a board.

```
 ┌──────────┐  POST /ping/<uuid>       ┌──────────────────┐   push, email,
 │  canary  │ ───────────────────────▶ │ heartbeat server │ ─▶ Telegram, ntfy…
 │  ESP32   │  POST /ping/<uuid>/fail  │ (Healthchecks.io,│
 └──────────┘                          │  ohp serve, …)   │
   probes: power · temperature · leak  └──────────────────┘
```

## Why a heartbeat

A sensor that sends alerts cannot send one when it has no power, no Wi-Fi or a crashed firmware. That is exactly when you need one. canary inverts the check: it reports that all is well, and the server raises the alarm when the reports stop. The same device catches a power cut, a dead router, a frozen board and a real problem.

## What it watches

| Probe | Hardware | Reports a problem when |
|---|---|---|
| `alive` | nothing | never: silence is the alert (power cut, network down, crash) |
| `temp` | DS18B20 probe, ≈ $1.50 | the temperature leaves `min`…`max` for longer than `delay` |
| `contact` | leak sensor, float switch, door contact | the input is active for longer than `delay` |

Up to four probes per board, each with its own check URL. Problems and recoveries are sent at once. Otherwise canary sends one heartbeat per `interval`. After a reboot the first heartbeat carries the reason, `boot=power-on`, `boot=brownout`, `boot=watchdog` or `boot=crash`, so the server's log tells a power cut from a firmware bug.

## Servers

canary speaks the [Open Heartbeat Protocol](https://github.com/AndreaAcanfora/openheartbeat) (OHP). It formalizes the ping URLs that Healthchecks.io and similar services already use. Any of these works:

- [Healthchecks.io](https://healthchecks.io): paste the ping URL of a check. The free plan has 20 checks and alerts by email, Telegram, ntfy, Signal and more.
- Self-hosted [Healthchecks](https://github.com/healthchecks/healthchecks).
- The OHP reference server: `ohp serve`.
- Cronitor, Sentry Crons or Dead Man's Snitch, through the [OHP adapters](https://github.com/AndreaAcanfora/openheartbeat).

Set the period of the check to the probe's `interval` and give it a grace time of a few minutes.

## Hardware

- **Board:** ESP32-C3 SuperMini (≈ $3), or any ESP32, ESP32-C3 or ESP32-S3 board.
- **Power:** a USB charger. For the power-cut alarm, plug it into the socket you want to watch, not into a UPS.
- **Temperature:** DS18B20 data wire to a free GPIO, with a 4.7 kΩ resistor between data and 3.3 V (modules often have it). VCC to 3.3 V, GND to GND. One sensor per GPIO.
- **Contact:** a switch between a GPIO and GND, with `active=low` (internal pull-up). For a sensor that drives the line high, use `active=high` (internal pull-down).

Avoid the strapping pins: GPIO 2, 8 and 9 on the ESP32-C3, and GPIO 0, 2, 12 and 15 on the ESP32. On the C3 SuperMini, GPIO 3, 4 and 5 are good choices.

### Status LED

| Pattern | Meaning |
|---|---|
| slow blink (1 s) | waiting for Wi-Fi setup |
| fast blink | no network |
| two quick flashes every 2 s | a probe reports a problem |
| short flash every 3 s | all fine |

The LED is on GPIO 8 (C3 SuperMini, active low) or GPIO 2 (ESP32). Set `led.pin` to use another one.

## Setup

1. Open the [web installer](https://andreaacanfora.github.io/canary/) in Chrome or Edge on a computer, click **Install**, and choose the board's serial port. Then pick your Wi-Fi network. The installer sends it over USB ([Improv](https://www.improv-wifi.com/)).
2. Open the link it shows, or `http://canary.local`.
3. Add probes and paste check URLs. **Save and restart.**

The setup page accepts changes only for 15 minutes after power-on, after Wi-Fi setup or after pressing the BOOT button. Outside that window it shows the status read-only, with check URLs redacted. It also refuses forms posted from other web sites. A machine-readable status is at `/api/status`.

## Configuration

The setup page writes these keys; the Linux port reads them from a file.

| Key | Default | Meaning |
|---|---|---|
| `name` | `canary` | device name, mDNS host name (`name.local`) and `OHP-Host` header |
| `led` | `on` | status LED |
| `led.pin` | board default | GPIO of the status LED |
| `probeN.type` | `none` | `alive`, `temp` or `contact` (N = 1…4) |
| `probeN.url` | | check URL (a secret: anyone who has it can send signals) |
| `probeN.interval` | `60` | seconds between heartbeats (minimum 10) |
| `probeN.delay` | `0` | seconds a problem must last before it is reported |
| `probeN.pin` | | GPIO of the sensor |
| `probeN.min`, `probeN.max` | | temperature limits in °C, either or both |
| `probeN.hysteresis` | `0.5` | °C back inside the limits before the probe is fine again |
| `probeN.active` | `low` | contact level that means "problem" |
| `probeN.source`, `probeN.scale` | | Linux only: file to read the value from, and a multiplier |

Example of a garage freezer that may stay above −15 °C for at most 10 minutes:

```ini
name=garage
probe1.type=alive
probe1.url=https://hc-ping.com/1f0c…
probe2.type=temp
probe2.url=https://hc-ping.com/7a92…
probe2.pin=4
probe2.max=-15
probe2.delay=600
probe2.interval=300
```

## Raspberry Pi and other Linux machines

The same probes run on Linux as a small daemon with no build dependencies. It loads `libcurl` at run time. Values come from files: a DS18B20 on the 1-Wire bus, a GPIO, the CPU temperature.

```sh
make linux
sudo install build/canary /usr/local/bin/
sudo install -m 600 linux/dist/canary.conf.example /etc/canary.conf   # then edit it
canary -n -c /etc/canary.conf                                       # check it
sudo install -m 644 linux/dist/canary.service /etc/systemd/system/
sudo systemctl enable --now canary
```

Release builds for `amd64`, `arm64` and `armhf` (Raspberry Pi OS 32-bit) are attached to each [release](https://github.com/AndreaAcanfora/canary/releases).

## Build and flash

With [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) 5.4 or newer:

```sh
cd firmware
idf.py set-target esp32c3
idf.py build flash monitor
```

Or flash a release image with esptool: `esptool.py write_flash 0 canary-esp32c3.bin`.

## Repository layout

```
core/      portable C11: OHP client, probe rules, configuration, Improv, DS18B20
firmware/  ESP-IDF application: Wi-Fi, setup page, 1-Wire, LED, Improv over USB
linux/     the same probes as a Linux daemon
site/      web installer (ESP Web Tools) and simulator
tests/     host unit tests, Linux and QEMU end-to-end tests
```

## Tests

- `make test`: unit tests of the core with AddressSanitizer and UBSan.
- `tests/e2e_linux.sh`: the Linux daemon against the OHP reference server. It changes sensor files and checks that each change reaches the server.
- `tests/qemu_test.py`: the real ESP32-C3 firmware in Espressif's QEMU, on emulated Ethernet. It checks heartbeats against `ohp serve`, the Improv handshake that ESP Web Tools performs, the status page, the CSRF check and form validation.

CI runs all of them, and builds the firmware for the ESP32, ESP32-C3 and ESP32-S3.

## Status

Version 0.1. The firmware is tested in an emulator and the core with unit tests. Reports from real boards and sensors are very welcome: open an issue with your board and what you watch.

## License

MIT
