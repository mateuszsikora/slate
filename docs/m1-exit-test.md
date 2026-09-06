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
| Router lost at runtime | Start in station mode, make the router unavailable without rebooting the panel, and wait five minutes. Restore the router after the fallback AP and banner appear. Attach to `/api/v1/ws` *before* switching the WLAN off: `station lost` carries the exact moment, and it does not survive in the 8 KiB ring to the end of the run. | **Pass** on `a9f72ce`, over an isolated WLAN that serves nothing else. The fallback appeared between 300.0 s and 332.5 s after the recorded loss, the backlight was driven to 100% by setup mode and stayed there, the station never stopped retrying underneath the access point, and restoring the WLAN brought the panel back with no touch, submission, reboot or cable. The measurement is below. |

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
off at the access point without touching the panel, and switched back on between
7 min 44 s and 8 min 47 s later — bounded below by the attempt at 570833 ms,
which still failed with `not_found`, and above by the attempt one backoff step
later that associated at 602618 ms. Wall-clock sampling from a second host puts
it at 8 min 19 s, inside that bound. Seven minutes was the intention, taken from
§9.4's router that comes back at minute seven; it is not what was measured.

Between those two moments the panel was not rebooted, not cabled and had nothing
submitted to it. It was touched once, to wake a blanked screen, and that touch
is timed and accounted for below.

**The threshold is measured from the panel's own clock, not from a stopwatch.**
The retained log ring gives failed association attempts at 309360, 341779,
374196 and 406614 ms of uptime, and setup mode raised at 406650 ms. The attempt
at 374196 ms did not cross the grace period and the attempt at 406614 ms did, so
the recorded loss falls in (74196, 106614] ms and the fallback appeared between
300.0 s and 332.5 s after it — never inside five minutes, and within one retry
cycle of them. That cycle is 32.4 s, not the 30 s of `BACKOFF_MS`: an attempt
that fails costs about 2.4 s before the backoff starts. The interval is a bound
rather than a reading because the direct record — `station lost` on
`slate_wifi.c:938`, emitted in the same breath as the `lost_at` stamp — had
already rolled out of the 8 KiB ring by the time anything read it.

Two qualifications on what that interval is anchored to. `lost_at` is stamped
when the panel *notices* the loss, one beacon timeout after the WLAN actually
stopped, so the physical switch-off precedes it and the true elapsed time is
slightly longer than 332.5 s. The direction is the safe one: the lower bound,
which is the claim that matters, only strengthens. The wall clock has the same
character and agrees anyway — the station stopped answering a second host at
22:10:05 and `slate-<mac6>` first accepted an association at 22:15:32, 5 min
27 s later. The two measurements are independent and they close: the uptime
bound puts boot between 22:08:18 and 22:08:51, which places 406650 ms between
22:15:05 and 22:15:38, and 22:15:32 falls inside that window.

**The backlight criterion had a deadline, and the panel let it pass.**
`screen_off_after` was set to one minute for the run so that the inactivity
timer would be armed rather than disabled — at the shipped default of never the
inactivity half of the suspension has nothing to act on. The screen had blanked
and was woken by hand at 390734 ms: `backlight woken by touch`, which the policy
logs only when the level actually rose from zero, and which restarts the timer.
The level it rose to is in the ring one millisecond earlier, at 390733 ms:
`brightness: day target 100% -> backlight 100% (on/off)`. The screen was
therefore lit, not dark, when setup mode arrived 15.9 s later, and the
`brightness: setup target 100% -> backlight 100% (on/off)` line at 406737 ms is
a change of *reason* from that `day` to `setup` rather than a change of level.
The policy logs either, so that line does not witness a dark screen being lit
and is not offered as one.

The result is the suspension itself. That restarted timer was due to blank the
screen at 450734 ms, 44.1 s after the banner appeared. It did not: from
406650 ms onward `service_locked()` re-stamps the activity clock on every pass
while setup mode is active, which is §9.4's suspension expressed in code. The
deadline went by and nothing dimmed, and that is a machine record rather than an
assurance — `slate_brightness.c:359` emits a line only when the target level or
the reason changes, and the next `brightness:` line in the ring is at 603738 ms,
after recovery. Three minutes sixteen of banner, one blanking deadline inside
it, no change.

The run proves the inactivity half of §9.4's suspension and not the other half.
`brightness: configured day 100%, night 100%, schedule disabled, screen off
1 min, wake on touch yes` records that no night window was configured, so the
claim that setup mode also overrides the night schedule remains untested here. A
panel left at the shipped `screen_off_after` of never, but carrying a night
schedule, can still fail this criterion — it is not a criterion that cannot
fail, only one that cannot fail for the reason this run exercised.

**Raising the access point did not stop the station.** Five further
`disconnected, reason 201 — not_found` / `retrying in 30000 ms` cycles are in
the ring at 439460, 472303, 505147, 537990 and 570833 ms, all with
`WIFI_MODE_APSTA` up. The retry cycle stretches from 32.4 s to 32.84 s once the
second interface is up, so sharing the radio costs the station about 425 ms per
attempt. `docs/DESIGN.md` §6.2 leaves the memory cost of that second interface
open; this run measures it. Free internal DMA-capable memory was 81847 B with
the access point up against 84827 B on the station alone — an `APSTA` cost of
2980 B.

**Recovery needed nobody.** The panel associated at 602618 ms, logged
`releasing the setup access point` at 602620 ms and `the station is back —
tearing the access point down` at 602630 ms. Those twelve milliseconds are the
decision, not the teardown: the mode change, the DNS responder and the overlay
come after it, and the completed teardown has its own stamp at 603136 ms, 518 ms
after association. The panel returned to the same address over DHCP with
`last_error` cleared.

What no log can settle is what reached the glass. The observer reported the
`NETWORK OFFLINE` banner over a still-visible dashboard, the join instruction
for `slate-<mac6>`, the `192.168.4.1` address, the absence of a setup passphrase
and the named failure for the configured WLAN, and reported the banner
disappearing on recovery. The log settles more of that than it might appear.
`present_setup_card()` derives both the logged reason and `.banner` from the
same value, and `.banner` is what makes the overlay a 164 px strip instead of a
full-screen card, so `setup mode: the network went away and has not come back`
is a deterministic record that banner geometry was requested rather than the
cold-boot card. A presentation that failed to appear would have logged `setup
presentation unavailable`, and no such warning is in the ring. Only the pixels
are unattested.

Every screen observation recorded here was made after the threshold. The one
touch of the run, at 390734 ms, came 15.9 s before the banner existed: it woke a
blanked screen to wait for the banner, not to read it. That it was the only one
is a record for as much of the outage as the ring still holds — `backlight woken
by touch` appears in it once, and the ring reaches back to 306945 ms — while the
first three minutes after the loss fall outside the ring and rest on the
operator's account. Waking a screen does not help a panel associate, so neither
the touch nor that gap weakens the unattended-recovery result.
