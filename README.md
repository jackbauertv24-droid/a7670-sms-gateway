# a7670-sms-gateway

Firmware for the **Waveshare ESP32-S3-A7670E-4G** that turns it into an SMS gateway: every SMS the SIM
receives is stored on the board, and (from milestone 3) forwarded to the channels you choose. Managed from an
admin page on the board's own Wi-Fi; updates itself from this repository's signed releases.

Spin-off of [esp32-portal-bridge](https://github.com/jackbauertv24-droid/esp32-portal-bridge), keeping only the
SMS features and applying its lessons (internal-RAM budget, private-by-design reports).

## Status by milestone

| # | Milestone | State |
|---|---|---|
| 1 | SMS store and manager, admin page with first-run setup, Wi-Fi with 4G fallback, **signed OTA** (dev mode by default), memory guard | building |
| 2 | Hourly log upload to a private repository (masked, never SMS text) | planned |
| 3 | Forwarding: Telegram and email, with queue, rules and templates | planned |
| 4 | Forwarding: signed webhook and MQTT | planned |
| 5 | Forwarding: ntfy, Pushover, Bark, Gotify, Discord, Slack, SMS to another number | planned |
| 6 | SIM extras: prepaid keep-alive, USSD balance, missed-call alerts, daily health message | planned |

## Updates

The board reads `manifest.json` of the latest release here and installs newer builds per **Settings > Updates**:

| Mode | Checks | Installs |
|---|---|---|
| `dev` (default) | every 60 s on Wi-Fi | at once |
| `auto` | hourly | at once |
| `nightly` | every 6 h | in a 2-hour night window |
| `notify` | every 6 h | when you press Install |
| `off` | never | manually (file upload) |

On 4G, checks happen at most every 60 minutes (configurable): each check costs about 10 KB.

Every image is **signed** (RSA-3072, ESP-IDF Secure Boot V2 scheme, without burning eFuses). A board only accepts
an update signed with the same key as the firmware it runs; the key is a repository secret and never in the code.
A new version must run for a minute before it is confirmed; if it does not, the bootloader goes back to the
previous one and the board skips that build.

## First install (USB cable, once)

1. Board switches: **4G ON**, **USB hub ON**, **USB route OFF** (modem USB to the ESP32, for 4G data).
2. Flash `a7670-sms-gateway-<version>-full.bin` at address **0x0** (Espressif Flash Download Tool, UART mode,
   or https://espressif.github.io/esptool-js/). Settings survive later cable flashes.
3. Join Wi-Fi **SMSGateway-XXXX** (password `smsgateway`), open `http://10.77.77.1` or `http://smsgateway.local`,
   log in as `admin` / `smsgateway`, set your own passwords.
4. **Settings > Internet**: your Wi-Fi; 4G data is the fallback.

## Privacy

Reports mask phone numbers, IMEI and SIM ids, MAC addresses and Wi-Fi names, and never include SMS text.
Nothing about a person, carrier or network is built into the firmware.

## Repository layout

- `firmware/` ESP-IDF 5.5 project (`main/` modules, `core/` plain C shared with the host tests, `main/www/` admin page)
- `tests/` host tests run in CI (PDU decoder and encoder, text helpers, call notifications, redaction)
- `.github/workflows/build.yml` tests, signed build, signature and memory checks, releases with `manifest.json`
