<p align="right">
  <a href="totp-app.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# FoloTOTP — TOTP Authenticator Application

FoloTOTP is the derivative application firmware for this repository: a
wearable TOTP (RFC 6238) authenticator with a gesture password, idle
locking, Wi-Fi provisioning, and LAN-based remote key management. It
replaces the hardware-test demo as the built firmware; the demo sources stay
in the repository only as reference and for host tests.

## Feature overview

| Requirement | Behavior |
| --- | --- |
| Gesture password | 8 steps of UP/DOWN (2^8 = 256 combinations), set on first boot. Only a salted hash is stored (NVS), never the sequence itself. |
| Idle lock | 15 minutes without a button press locks the device; unlock requires the password. 10 consecutive wrong attempts lock the device for 60 minutes (with an on-screen countdown). The failure counter and lockout deadline are persisted, so power-cycling does not bypass them. |
| Wi-Fi | SoftAP web provisioning (no phone app or Bluetooth needed) with automatic reconnect afterwards; SNTP syncs the clock, which TOTP and the lockout timer depend on. |
| Language | The device UI and the remote-management web pages follow the device language (Simplified Chinese by default). the **Language** menu item toggles instantly; the choice is persisted. |
| Remote management | After connecting, the menu starts a management session: the device shows the URL, a QR code of that URL, and a random 8-digit numeric code. Any device on the same LAN opens the URL, enters the code, and can add or delete TOTP entries. |
| TOTP display | Codes refresh every 30 seconds with a countdown bar; UP/DOWN page through entries. |

## Device controls

All on-device UI text is Simplified Chinese by default with an English
option (the **Language** menu item); every string — screens, hints, Wi-Fi
states, management-page text and server messages — comes from a single
bilingual table (`main/app_lang.c`), and the choice persists in NVS. The
bundled Source Han Sans SC subset covers both languages (see
`assets/README.md` for the character inventory).

| Screen | UP | DOWN | OK click | OK double | OK hold |
| --- | --- | --- | --- | --- | --- |
| Password setup / unlock | Enter one step | Enter one step | Delete last step | — | Clear input |
| Code list | Previous entry | Next entry | Open menu | — | Lock now |
| Menu | Move selection | Move selection | Activate item | — | Back |
| Wi-Fi | Move selection | Move selection | Activate item | — | Back |
| Wi-Fi scan | Move selection | Move selection | Next (AP / rescan / back) | — | Back |
| Password keyboard | Previous item | Next item | Enter character / open group / direct action | Backspace | Confirm and connect |
| Remote management | — | — | — | — | Exit session |
| Delete confirmation | Move selection | Move selection | Confirm | — | Back to list |

The battery level is shown in the top-right corner (blank when the gauge is
unavailable). After 30 seconds of inactivity the backlight dims to save
power.

## Wi-Fi provisioning

Provisioning happens entirely on the device; credentials are stored in the
application's NVS namespace and the device connects as a station via DHCP.

**Menu > Wi-Fi > Scan & connect**.

1. The device scans nearby 2.4 GHz networks and lists the strongest eight
   with their RSSI.
2. Select a network and press OK; a two-stage keyboard opens. Stage one
   picks a character group — `abc`, `ABC`, `123`, symbols — or a direct
   action (space, backspace, done). Stage two rolls within that group:
   UP/DOWN move one character, hold jumps five, OK enters the highlighted
   character and stays in place, and the left-arrow item returns to stage
   one. OK double-click always backspaces; OK hold always confirms and
   connects (empty password connects to open networks).
3. The device connects via DHCP and shows the status and IP on the Wi-Fi
   page. Wrong passwords stop retries with a status shown on screen.

Credentials are stored only in the application's NVS namespace. Clear them
any time via **Menu > Wi-Fi > Clear credentials**.

## Remote management flow

1. Connect Wi-Fi (the code list shows `Waiting for time sync` until SNTP
   completes; codes are never shown from a wrong clock).
2. **Menu > Remote Mgmt**. The screen shows `http://<device-ip>/`, a QR
   code of that URL, and the 8-digit code.
3. Scan the QR code or type the URL on any LAN device, enter the 8-digit
   code, and the page lists all entries with an add form (label, Base32
   secret, digits 6/8, period 30/60 s) and a delete button per entry.
4. Secrets are write-only over the network: the server never returns a
   secret or a code.

Management sessions end automatically when: you hold OK on the device, the
device locks (5 idle minutes or manual lock), the web session is idle for
15 minutes, 5 wrong login codes are entered, or the Wi-Fi connection drops.
While a session is active, authenticated web activity does not count as
device activity — the device still locks after 5 idle minutes.

## Label and character policy

The device UI is Simplified Chinese, rendered with the bundled
`app_font_sc_16` subset (Source Han Sans SC; printable ASCII plus the
GB2312 Level-1 common-character set and the application's fixed copy —
3872 glyphs, recorded in `assets/README.md`).

Entry labels accept UTF-8, including Chinese: at most 24 bytes (about eight
Chinese characters), no control characters, validated on the device. The
coverage set is finite: characters outside it (rare hanzi in SSIDs or
labels) render as placeholder boxes rather than being rejected. To change
the covered set, regenerate the font with the recorded command and update
the inventory in `assets/README.md`; see the
[Chinese font checklist](development/engineering/coding-conventions.md#chinese-fonts-and-missing-glyphs).

## Security notes and known limitations

- The gesture password space is 256 combinations by design. It protects
  against casual pickup, not against a determined attacker with unlimited
  time and flash access.
- The management web service is plain HTTP without TLS. Secrets are never
  transmitted, but use it on a trusted LAN.
- The lockout deadline relies on the SNTP-synced clock. Without any Wi-Fi
  connection the device stays locked with a "waiting for time sync" hint
  (fail-safe behavior).
- Login attempts are capped at 5 per session; after that the service shuts
  down until the device owner restarts it.

## Implementation map

| Module | Role |
| --- | --- |
| `main/app_totp.c` | Pure C: SHA-1/HMAC, HOTP, TOTP, Base32, entry serialization (RFC-vector tested on the host) |
| `main/app_kb.c` | Pure C: three-button roller keyboard model for on-device Wi-Fi password entry |
| `main/app_lock.c` | Pure C: gesture-password state machine, idle lock, 60-minute lockout |
| `main/app_store.c` | NVS persistence + in-memory entry cache (mutex-protected) |
| `main/app_wifi.c` | Station/provisioning manager, retries, SNTP |
| `main/app_provision.c` | SoftAP provisioning HTTP service (scan/connect/status) |
| `main/app_server.c` | Remote management HTTP service (login, sessions, add/delete) |
| `main/app_ui.c` | The application's own LVGL screens and 1 s state tick |
| `main/app_main.c` | Boot sequence and input dispatch |

Host tests: `tests/test_app_totp.c`, `tests/test_app_lock.c`, `tests/test_app_kb.c` (registered in
`tools/validate.sh`). Build and flash follow
[build and test](development/engineering/build-and-test.md); the validated
merged image is `build/FoloToy-AI-Passport-full.bin` flashed at `0x0`.
