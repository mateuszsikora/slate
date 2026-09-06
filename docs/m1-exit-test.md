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
| Return to setup | Send authenticated `DELETE /api/v1/wifi`, join `slate-<mac6>`, and query `/api/v1/info` at `192.168.4.1`. Submit new credentials through `POST /api/v1/wifi`. | **Pass:** the panel named the unavailable original SSID on screen; the AP reported `mode: ap` at `192.168.4.1`; and a different target WLAN was provisioned without a cable. `/api/v1/info` then confirmed station mode, no error, and the requested static address `192.168.22.231/24` with gateway and DNS at `192.168.22.1`. |
| Router unavailable at boot | With credentials for an unavailable WLAN stored, power-cycle the panel and recover through its setup AP without a cable. | **Pass:** on startup the panel named the unavailable original SSID, raised its setup AP and allowed a reachable target WLAN and static address to be configured. The panel then returned in station mode with no network error. |
| Router lost at runtime | Start in station mode, make the router unavailable without rebooting the panel, and wait five minutes. Restore the router after the fallback AP and banner appear. | **Pass** on `a9f72ce`, over an isolated WLAN that serves nothing else. The fallback appeared between 300.0 s and 332.4 s after the loss, the backlight was driven to 100% by setup mode, the station never stopped retrying underneath the access point, and restoring the WLAN brought the panel back with no touch, submission, reboot or cable. The measurement is below. |

OTA through the setup AP is not an M1 exit requirement. The developer's
cable-free update, rollback and core-dump loop is already exercised on the
station network. A normal user who has no router cannot discover that an update
exists; notification, manual update and automatic update belong to the later
on-panel update flow. Keeping the authenticated upload endpoint reachable at
`192.168.4.1` remains useful implementation behaviour, but it is not a product
acceptance test.

## Build evidence for the recovery presentation

The reviewed recovery-presentation changes build from a fresh ESP-IDF 5.5.5
directory. The application image is `0x14acb0` bytes; the 6 MiB OTA slot has
78% free. `git diff --check` reports no whitespace errors.

The final issue comment must distinguish build evidence, network observations
and physical-screen observations.

## Runtime loss: the measurement

Run on `a9f72ce` over an isolated WLAN that serves nothing else, so no unrelated
device was disturbed. The panel was provisioned onto it, the WLAN was switched
off at the access point without touching the panel, and it was switched back on
seven minutes later. The panel was never touched, rebooted or cabled between
those two moments.

**The threshold is measured from the panel's own clock, not from a stopwatch.**
The retained log ring gives failed association attempts at 309360, 341779,
374196 and 406614 ms of uptime, and setup mode raised at 406650 ms. The attempt
at 374196 ms did not cross the grace period and the attempt at 406614 ms did, so
the loss falls in (74196, 106614] ms and the fallback appeared between 300.0 s
and 332.4 s after it — never inside five minutes, and within one 30 s backoff
step of them. Wall-clock sampling from a second host agrees independently: the
station stopped answering at 22:10:05 and `slate-<mac6>` first accepted an
association at 22:15:32, 5 min 27 s later.

**The backlight is the one criterion a photograph would have proved weakly.**
`screen_off_after` was deliberately set to one minute for the run, so the screen
was already dark when the threshold arrived and the panel had to light it
itself. It did: `brightness: setup target 100% -> backlight 100% (on/off)` at
406737 ms, 87 ms after setup mode was raised. On a panel configured the way one
ships — `screen_off_after` at its default of never — this criterion cannot fail
and cannot be demonstrated either.

**Raising the access point did not stop the station.** Five further
`disconnected, reason 201 — not_found` / `retrying in 30000 ms` cycles are in the
ring at 439460, 472303, 505147, 537990 and 570833 ms, all with `WIFI_MODE_APSTA`
up. §6.2's cost of the second interface measured 81847 B of free internal
DMA-capable memory with the access point up against 84827 B on the station
alone.

**Recovery needed nobody.** `connected to` the test WLAN at 602618 ms, `releasing the setup
access point` at 602620 ms, `the station is back — tearing the access point
down` at 602630 ms — twelve milliseconds from association to teardown. The panel
returned to the same address over DHCP with `last_error` cleared.

One criterion rests on a reported screen observation rather than a machine
record: that the dashboard stayed visible under the `NETWORK OFFLINE` banner and
was not replaced by a full-screen setup card. The observer reported the banner,
the join instruction for `slate-<mac6>`, the `192.168.4.1` address, the absence
of a setup passphrase and the named failure for the configured WLAN, and
reported the banner disappearing on recovery. The log corroborates the shape
without proving the layout: the firmware took the runtime branch, which logs
`setup mode: the network went away and has not come back`, rather than the
cold-boot branch that renders the full-screen card. Reading the banner before
the threshold required a tap to wake the screen, because of the one-minute
`screen_off_after` above; waking the screen does not help the panel associate,
so it does not weaken the unattended-recovery result.
