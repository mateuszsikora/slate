# M1 exit test: untether the board

This document records the reproducible procedure and hardware evidence for
issue #15. It is deliberately a work in progress: M1 is not complete until one
continuous run starts with the USB data cable removed and finishes every row
below over the network.

## Preparation

The final serial operation installs the current bootloader, partition table and
application, and obtains the existing device token without printing it. After
that point, disconnect the USB data cable. Set `SLATE_TOKEN` in the test shell
and use a unique `PROJECT_VER` for every OTA image so rollback cannot be
mistaken for a successful update.

Never put the token or a WiFi passphrase in a command argument, log, issue
comment or this file. The repository tools pass the token to `curl` through
standard input.

## Procedure and current evidence

| Check | Network-only procedure | Current evidence |
|---|---|---|
| Token enforcement | Request `/api/v1/status` with no token, a wrong token and the device token. | **Pass:** HTTP 401, 401 and 200 respectively. |
| Healthy OTA | Build a uniquely versioned normal image and run `tools/ota/upload.sh <panel> firmware/build/slate.bin`. Verify `/api/v1/info` after reboot. | **Pass:** `m1-exit-normal` was accepted, rebooted and returned in station mode. |
| WebSocket logs | Connect to `/api/v1/ws`; send authentication as the first frame, first with a wrong token and then with the device token. | **Pass:** the wrong token returned `auth_invalid`; the valid token returned `auth_ok` followed by 20 retained boot-log frames. |
| Automatic rollback | Build with `SLATE_OTA_ROLLBACK_SELFTEST=1` and a unique version, then upload it over WiFi. | **Pass:** `m1-exit-rollback` was accepted into the other OTA slot, aborted while pending verification and returned unattended to `m1-exit-normal`. |
| Core dump | Fetch the rollback dump with `tools/coredump/fetch.sh`, passing the ELF of the image that crashed. | **Pass:** 10,596 bytes were fetched over WiFi and symbolicated to `ota_rollback_selftest()` in `firmware/main/main.c`. |
| Return to setup | Send authenticated `DELETE /api/v1/wifi`, join `slate-<mac6>`, and query `/api/v1/info` at `192.168.4.1`. Submit new credentials through `POST /api/v1/wifi`. | **Pass:** the panel named the unavailable original SSID on screen; the AP reported `mode: ap` at `192.168.4.1`; and a different target WLAN was provisioned without a cable. `/api/v1/info` then confirmed station mode, no error, and the requested static address `192.168.22.232/24` with gateway and DNS at `192.168.22.1`. |
| Router unavailable at boot | With known-good credentials stored, make the router unavailable and power-cycle the panel. Join its AP, perform an OTA at `192.168.4.1`, then restore the router. | **Pending:** verify the AP address on the physical screen, OTA without a station connection, no rollback of the healthy image, unattended station recovery and automatic dismissal of the setup presentation. |
| Router lost at runtime | Start in station mode, make the router unavailable without rebooting the panel, and wait five minutes. Restore the router after the fallback AP and banner appear. | **Pending:** verify that the dashboard remains visible below the `NETWORK OFFLINE` banner, the backlight stays on, and the banner and AP disappear after unattended station recovery. |

## Build evidence for the recovery presentation

The reviewed recovery-presentation changes build from a fresh ESP-IDF 5.5.5
directory. The application image is `0x14acb0` bytes; the 6 MiB OTA slot has
78% free. `git diff --check` reports no whitespace errors.

The final issue comment must distinguish build evidence, network observations
and physical-screen observations. Do not mark #15 complete while the pending
router-unavailable row or the runtime-loss banner check remains.
