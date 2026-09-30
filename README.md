**English** · [简体中文](README.zh_CN.md)

# FoloTOTP — TOTP Authenticator Firmware

FoloTOTP turns the FoloToy AI Passport wearable (ESP32-C3, 8 MB flash,
240×320 display, three buttons) into a pocket TOTP (RFC 6238) authenticator
with a gesture password, on-device Wi-Fi provisioning, and LAN-based remote
key management. It is a derivative application of the
[FoloToy AI Passport](docs/README.md) baseline: the hardware-test demo is
kept only as reference; this firmware builds the application instead.

## Features

- **Gesture password** — 8 steps of UP/DOWN (2^8 = 256 combinations), set on
  first boot; only a salted hash is stored, never the sequence.
- **Locking policy** — 15 minutes without a button press locks the device;
  10 consecutive wrong attempts lock it for 60 minutes with an on-screen
  countdown. Failure counters and the lockout deadline are persisted, so a
  power cycle does not bypass them.
- **TOTP codes** — SHA-1/SHA-256-style RFC 6238 codes refresh every 30 s
  with a countdown bar; UP/DOWN page through entries.
- **On-device Wi-Fi provisioning** — the device scans nearby networks, you
  pick one and type the password on a two-stage three-button keyboard
  (abc / ABC / 123 / symbols groups); DHCP assigns the IP. No phone app, no
  Bluetooth, no hotspot.
- **Remote management** — after connecting, the device shows the management
  URL, a QR code of that URL, and a random 8-digit code. Any device on the
  same LAN can open the URL, enter the code, and add or delete TOTP entries.
  Secrets are write-only over the network.
- **Bilingual UI** — Simplified Chinese and English on the device and on the
  management web pages; the language switch lives in the menu and persists.
- **Extras** — battery percentage in the top-right corner, 30 s backlight
  dim, SNTP time sync (TOTP refuses to display codes from a wrong clock).

## Hardware

| Item | Requirement |
| --- | --- |
| Target | ESP32-C3 (RISC-V), 8 MB flash, no PSRAM |
| Display | ST7789P3 240×320 SPI, rounded corners |
| Buttons | Three buttons on one ADC pin (UP / DOWN / OK) |
| Battery gauge | CW2017 via I²C (optional; UI degrades gracefully) |

Pin assignments and hardware facts live in
[`components/bsp/include/bsp_pins.h`](components/bsp/include/bsp_pins.h).

## Build and flash

Requires [ESP-IDF 5.5.3](https://docs.espressif.com/projects/esp-idf/). From
an activated environment:

```bash
./tools/validate.sh --static    # repository checks + host tests
./tools/validate.sh --firmware  # ESP-IDF build + merged-image verification
./tools/validate.sh             # complete gate
```

Flash the verified merged image at offset `0x0`:

```bash
idf.py -p PORT flash            # incremental development
# or flash build/FoloToy-AI-Passport-full.bin at 0x0 for a full refresh
```

Host tests cover the pure-logic cores (TOTP/Base32 against RFC vectors,
gesture-password state machine, two-stage keyboard model, bilingual string
tables) and run on any C compiler — see `tools/validate.sh`.

## Quick start

1. First boot asks you to create the gesture password: enter 8 UP/DOWN
   steps, then repeat them.
2. Menu → Wi-Fi → scan & connect: pick your network, type the password,
   confirm with a long press. The device connects via DHCP and syncs time.
3. Menu → Remote Mgmt shows the URL, QR code, and 8-digit code; open it on
   any LAN device to manage entries.
4. Entry codes refresh every 30 s; hold OK anywhere to lock immediately.

The full manual — controls per screen, provisioning details, management
session rules, label/character policy, and security notes — is in
[docs/totp-app.md](docs/totp-app.md) ([简体中文](docs/totp-app.zh_CN.md)).

## Security notes

- The 256-combination gesture password protects against casual pickup, not
  a determined attacker.
- The management service is plain HTTP on your LAN; secrets never leave the
  device, but use it on a trusted network.
- Management sessions end automatically on device lock, 5 idle minutes,
  5 wrong codes, or Wi-Fi loss.

## Repository layout

| Path | Content |
| --- | --- |
| `main/` | Application: TOTP core, lock state machine, storage, Wi-Fi, HTTP services, UI, bilingual tables |
| `components/bsp/` | Board support (display, buttons, battery, audio) — reusable hardware layer |
| `assets/fonts/` | Bundled Source Han Sans SC subset (license and regeneration documented) |
| `tests/` | Host tests for all pure-logic modules |
| `docs/` | Product and development documentation, [app manual](docs/totp-app.md) |

Upstream baseline, hardware guide, and contribution rules are documented in
[AGENTS.md](AGENTS.md) and [docs/README.md](docs/README.md).

## License

MIT — see [LICENSE](LICENSE). The bundled font subset is generated from
Source Han Sans SC (SIL OFL 1.1).
