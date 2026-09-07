# Agent setup

Instructions for a coding agent asked to take a panel from a cable to a working
dashboard. It exists so that a request like this one is enough:

> The ESP32-S3-Touch is plugged into USB. Here is the Slate repository and here
> is a token for my Home Assistant. Set me up a dashboard with a light and a
> temperature on it.

This is not [`agent-workflow.md`](agent-workflow.md), which is about working the
issue backlog, and it is not [`CONTRIBUTING.md`](../CONTRIBUTING.md), which is
about sending a change. Nothing here modifies the repository. If you are a
person setting up your own panel, [`README.md`](../README.md) is the same ground
in the order a person wants it.

Everything the panel answers is a machine-readable contract, so every phase
below ends in a condition you can check rather than in something to look at.
[`API.md`](API.md) has the endpoints and their refusal codes,
[`CONFIGURATION.md`](CONFIGURATION.md) the dashboard document and its validation
vocabulary. Read both before starting; this page cites them rather than
repeating them.

- [Inputs](#inputs) · [Rules](#rules)
- [1. Find the board](#1-find-the-board) · [2. Get an image onto
  it](#2-get-an-image-onto-it) · [3. Learn the panel's
  name](#3-learn-the-panels-name) · [4. Put it on the
  network](#4-put-it-on-the-network) · [5. Open a session](#5-open-a-session)
- [6. Connect Home Assistant](#6-connect-home-assistant) · [7. Choose the
  entities](#7-choose-the-entities) · [8. Publish the
  dashboard](#8-publish-the-dashboard) · [9. Verify](#9-verify) ·
  [10. Report](#10-report)
- [What not to do](#what-not-to-do)

## Inputs

A request like the one above carries some of what a setup needs and not all of
it. Collect the rest **before** touching the board, because the middle of phase
4 is the worst place to discover a missing password — the machine running you
may be on the panel's own access point by then, with no route to the person you
want to ask.

| Input | Name it goes under | Where it comes from |
|-------|--------------------|---------------------|
| Serial port | — | detected, phase 1 |
| WiFi SSID | — | confirmed with the operator, and visible to the panel in `GET /wifi/scan` |
| WiFi passphrase | — | **ask.** It is not on the host in a form you should read |
| Administrator PIN | — | ask, or generate 4–12 digits and report it in phase 10 |
| Panel session credential | `SLATE_TOKEN` | `POST /session`, phase 5 |
| Home Assistant URL | `HA_URL` | `GET /ha/discover`, or ask |
| Home Assistant token | `HA_TOKEN` | given, and see [Rules](#rules) about where it may live |
| Which light, which sensor | — | phase 7 — **ask when more than one matches** |
| Timezone | — | the host's, e.g. `readlink /etc/localtime`; confirm it |

The three exported names are the ones every command below assumes, and
`SLATE_TOKEN` is already the repository's convention in `CONFIGURATION.md` and
`tools/ota/upload.sh`.

Ask for all of the missing ones in one message. Do not guess a WiFi passphrase,
do not invent a Home Assistant URL, and do not pick between two lights called
something like *Living room* and *Living room lamp* on the operator's behalf.

## Rules

- **The token stays out of argv and out of the repository.** Read it from a file
  or an environment variable and hand it to `curl` on stdin, the way
  [`tools/direct/publish.sh`](../tools/direct/publish.sh) does: `printf 'header
  = "Authorization: Bearer %s"\n' "$TOK" | curl --config - …`. On a shared
  machine `-H "Authorization: Bearer $TOK"` is visible in the process list. Never
  write a credential into a file inside the checkout, and never into a commit.
- **Validate before you publish.** `POST /config/validate` changes nothing and
  answers in the codes of
  [`CONFIGURATION.md`](CONFIGURATION.md#validation-errors). A `PUT /config` that
  fails validation also changes nothing, but a needless failed write is a
  needless flash cycle.
- **Confirm anything that touches the operator's machine or an existing panel.**
  Joining the host to a different WiFi network (phase 4) and erasing flash on a
  panel that has run Slate before (phase 2) both belong to the operator, not to
  you. Ask, and say what will happen.
- **Report a failure with its code.** Every refusal in this API is a stable
  string. `422 ha_auth_invalid` and `502 ha_unreachable` are different problems
  with different fixes; "Home Assistant did not connect" is neither.

## 1. Find the board

`esptool.py` comes with ESP-IDF, and this phase already needs it, so source the
environment before anything else. Phase 2 builds from the same shell and does
not repeat this:

```bash
. "$HOME/esp/esp-idf/export.sh"
```

Then find the port. List the whole directory and filter rather than globbing
several patterns at once — under `zsh` a pattern that matches nothing aborts the
entire command, so `ls /dev/cu.usbmodem* /dev/cu.usbserial*` can print nothing at
all on a machine where the first pattern would have matched:

```bash
ls /dev/cu.*                         # macOS — look for cu.usbmodem*
ls /dev                              # Linux — look for ttyACM* or ttyUSB*
```

Usually one port is new. Two is not a fault in itself — some carrier boards
expose a USB-serial bridge alongside the ESP32-S3's native USB — but if you
cannot tell which is which, ask rather than flashing the first: the other one
may be somebody's radio.

Then confirm the module, because `firmware/partitions.csv` is drawn for 16 MB
and a panel with 8 MB will accept the flash and fail later:

```bash
esptool.py --port /dev/cu.usbmodem1101 flash_id
```

**Done when** the chip reads as ESP32-S3 with `Detected flash size: 16MB`. The
same output names the PSRAM — `Features: WiFi, BLE, Embedded PSRAM 8MB` — so one
command confirms the whole N16R8 part without anyone reading the marking on the
module. Anything smaller: stop and say so. Do not flash it. If the port does not
appear at all, the usual cause is a charging-only USB-C cable.

## 2. Get an image onto it

Until a version is tagged there is nothing to download, so this phase is a build
from source. Check first — `gh release list` in the repository, or the manifest
the browser installer is published beside — and if a release exists, prefer its
`slate-<version>.bin` over building.

Building needs ESP-IDF v5.5.5 and Node.js 24, and the fonts and the editor
bundle are generated rather than committed: a clean checkout **cannot** build
the firmware until both exist, and the failure does not explain itself. In
order, from the repository root:

```bash
. "$HOME/esp/esp-idf/export.sh"
tools/fonts/generate.sh
tools/editor/build.sh
cd firmware && idf.py build
```

The first two need network access on their first run and both are idempotent.
Expect several minutes for a cold build. Then:

```bash
idf.py -p /dev/cu.usbmodem1101 flash
```

**Done when** `idf.py flash` exits `0` and the board resets on its own. Do not
attach `monitor`: it is interactive, it holds the port, and nothing in the boot
log is needed here — phase 3 gets the panel's identity from the hardware and
phase 4 gets its address from the API.

`idf.py erase-flash` is available and is almost never what you want. WiFi
credentials, the dashboard, External API keys and the Home Assistant token live
outside the firmware partitions and survive a reflash; an erase takes all four.
On a panel that has run Slate before, that is a data-loss action and needs the
operator to agree to it.

## 3. Learn the panel's name

The setup access point's SSID, the mDNS hostname and the name in `GET /info` are
one string, `slate-<mac6>`, and `<mac6>` is the last three bytes of the base MAC
in lowercase hex. That means you can derive it over the cable, before the panel
has finished booting and without reading anything off the screen:

```bash
esptool.py --port /dev/cu.usbmodem1101 read_mac   # MAC: 68:b6:b3:a1:b2:c3
```

gives `slate-a1b2c3`, which is both the network to join in phase 4 and
`slate-a1b2c3.local` afterwards.

**Done when** you have that string. Keep it; every later phase uses it.

## 4. Put it on the network

A panel with no stored credentials raises its own access point and prints the
name, the address and a QR on its screen. The screen is the primary channel here
and you cannot see it, so work from the API instead: on that access point
`GET /wifi/scan`, `POST /wifi` and `GET /info` answer without a session, and
everything else answers `401` exactly as it does on the station.

Getting onto that access point means moving the host's WiFi. **Find out what
that costs before offering to do it:**

```bash
route -n get default | awk '/interface/{print $2}'    # which interface has the route
networksetup -listallhardwareports                    # which one is Wi-Fi
```

If those two are the same interface, moving the WiFi takes the host off the
internet — which includes your own connection to whatever is running you. You
can still do it, but only as **one script that joins, provisions and returns
without stopping**: an agent that needs a round trip while the host is offline
cannot finish the phase it started. If the default route is on a wired
interface, as it often is on a desk machine, the WiFi radio is free and this
phase costs nothing.

**Offer the operator both paths and let them choose:**

- *They do it* — they join a phone or laptop to `slate-<mac6>`, open
  `http://192.168.4.1`, and provision the panel through the setup page. You wait
  and pick up at the end of this phase. This is the shorter path and the one that
  needs nothing from your host.
- *You do it* — confirm first, then, on macOS, using the Wi-Fi device from
  `-listallhardwareports` rather than assuming `en0`:

  ```bash
  networksetup -setairportnetwork en0 slate-a1b2c3       # open network, no password
  ipconfig getifaddr en0                                 # expect 192.168.4.x
  curl -sS http://192.168.4.1/api/v1/info                # confirm: network.mode == "ap"
  ```

  Do not try to read the network you are leaving with `networksetup
  -getairportnetwork`. On current macOS it answers `You are not associated with
  an AirPort network` even when the interface is associated and carrying
  traffic, because reporting the SSID is gated behind Location Services and a
  command-line tool does not have it. `networksetup
  -listpreferredwirelessnetworks <device>` does work — it reads stored
  configuration rather than the radio — and the network to return to is
  normally the one the operator named as the panel's target anyway.

  The panel is the better scanner in any case. `GET /wifi/scan` on the access
  point returns what the panel's own radio can see, with `auth` and `age_s`,
  which is the list that matters: an SSID the host can see and the panel cannot
  is not a network the panel can join.

  Submit the credentials, the PIN and the addressing in one request. Nothing on
  this interface needs a session, and the passphrase reaches `curl` on stdin
  rather than as an argument:

  ```bash
  curl -sS -X POST http://192.168.4.1/api/v1/wifi \
       -H 'Content-Type: application/json' \
       --data-binary @- -w '%{http_code}\n' <<JSON
  {"ssid":"home","password":"…","admin_pin":"1234","ipv4":{"mode":"dhcp"}}
  JSON
  ```

  `202` means accepted, not connected. **The outcome appears on the panel, not
  in the response** — there is one radio, and the access point drops you at the
  instant the station associates, so there is nothing to poll for from here.

  Then return the host to its own network, which needs more care than it looks
  like it should:

  ```bash
  networksetup -removepreferredwirelessnetwork en0 slate-a1b2c3
  networksetup -setairportpower en0 off && networksetup -setairportpower en0 on
  ipconfig waitall
  ipconfig getifaddr en0                                 # must not be empty
  ```

  Two things make the obvious version of this wrong. **Joining the panel's
  access point puts it at the top of the host's preferred networks**, and it is
  an open network that is still in range, so macOS will silently associate with
  it again the moment the radio comes back — a host that looks reconnected can
  be sitting on `192.168.4.2` instead. Removing it first is not tidiness, it is
  the fix. And **an explicit `networksetup -setairportnetwork <device> <home
  network>` fails non-interactively**, with `Error: -3900 tmpErr`, because the
  stored passphrase is not available to it in that context; each attempt leaves
  the interface `inactive`. Letting macOS auto-join after a radio cycle works.
  Do not declare this phase done until `ipconfig getifaddr` on the Wi-Fi device
  prints an address on the operator's own subnet.

Either way, find the panel again from the host's normal network:

```bash
curl -sS --retry 20 --retry-delay 3 --retry-all-errors \
     --connect-timeout 3 --max-time 90 http://slate-a1b2c3.local/api/v1/info
```

**Done when** `GET /info` reports `network.mode: "sta"` with an `ip` and
`network.last_error: null`. Record that address and use it from here on; the
mDNS name keeps working but a literal address survives a resolver that does not.

If the panel never answers, it either did not associate or mDNS did not reach
it, and those need different responses:

- **Not associated** — the panel raises its access point again on its own. Join
  it and read `network.last_error` from `GET /info`: `bad_password`,
  `not_found` (a 5 GHz-only SSID never appears in the scan — the radio is
  2.4 GHz), `no_ip`, `auth_timeout`, `gateway_unreachable`, `address_in_use`.
  Report the code and what it means; a wrong passphrase is the operator's to
  correct, not yours to retry.
- **Associated but unreachable** — multicast DNS does not always cross a VLAN.
  Look for the lease on the router, or ask the operator to read the address off
  the panel's screen. Here the person is a perfectly good sensor.

## 5. Open a session

```bash
curl -sS -X POST http://192.168.1.42/api/v1/session \
     -H 'Content-Type: application/json' --data-binary @- <<'JSON'
{"pin":"1234"}
JSON
```

Whether to send a body at all is not a guess: `GET /info` reports
`authentication` as `pin` or `open`, and an open panel takes `POST /session`
with no body. That field is what the editor uses to decide whether to show a PIN
prompt, and it is what you use here.

The response is `{"token":"…"}`. Export it as **`SLATE_TOKEN`**, which is the
name the rest of this document, `CONFIGURATION.md` and `tools/ota/upload.sh` all
use. Five failed PIN attempts answer `429 try_later` with `Retry-After: 30` —
wait it out rather than retrying into it. The credential is in-memory and rotates
on a PIN change or a factory reset.

**Done when** an authenticated request works: `GET /status` should answer `200`
with `providers` listing `ha` as `unconfigured`.

## 6. Connect Home Assistant

Check whether there is anything to do first — the operator may have connected it
themselves, and `GET /status` will already report the `ha` provider as `online`:

```bash
curl -sS --config <(printf 'header = "Authorization: Bearer %s"\n' "$SLATE_TOKEN") \
     http://192.168.1.42/api/v1/ha        # {"configured":true,"url":"http://…"}
```

A `configured: true` means this phase is done and you never see the token, which
changes how phase 7 works but does not block it. Do not reconfigure a working
connection to obtain one.

Find the instance, if the operator did not name it:

```bash
curl -sS http://192.168.1.42/api/v1/ha/discover      # {"instances":[{"name":…,"url":…}]}
```

An empty result is normal — mDNS does not always cross a VLAN — and manual entry
is the fallback. Ask rather than probing addresses.

Then configure it, with both the session credential and the long-lived token off
argv. This request is the one place in the runbook that needs a body *and* a
credential, so the header cannot come from a pipe: the heredoc claims stdin, and
`curl` ends up trying to parse the JSON body as a configuration file and fails
with `option --config: error encountered when reading a file` before it sends
anything. Process substitution gives the header its own descriptor:

```bash
curl -sS --config <(printf 'header = "Authorization: Bearer %s"\n' "$SLATE_TOKEN") \
     -X POST http://192.168.1.42/api/v1/ha \
     -H 'Content-Type: application/json' --data-binary @- -w '%{http_code}\n' <<JSON
{"url":"${HA_URL}","token":"${HA_TOKEN}"}
JSON
```

`HA_URL` and `HA_TOKEN` are the operator's two Home Assistant inputs, exported
under those names; `/dev/fd` keeps the token out of `argv` and off the disk.

**Done when** this answers `204`, which the panel sends only after testing the
credentials against the instance — existing working credentials survive a failed
attempt, so there is nothing to undo. The refusals are worth reporting verbatim:
`422 ha_auth_invalid` is a bad or expired token, `502 ha_unreachable` is a URL
or a network, `400 bad_url` is neither.

One thing to check before blaming the token: a token belonging to a
`system-read-only` account renders a perfect dashboard on which nothing responds
to a tap, and Home Assistant refuses the service call with a generic error
rather than an authorization one. A dedicated account in the **`system-users`**
group is what Slate wants — not an administrator, which grants more than it
needs.

## 7. Choose the entities

**If you hold the token, ask Home Assistant directly.** Its REST API answers in
one request, where the panel's `/ha/catalog` splits the same material into four
stages so an ESP32 never holds more than one small response at a time.

```bash
printf 'header = "Authorization: Bearer %s"\n' "$HA_TOKEN" |
curl -sS --config - "$HA_URL/api/states" > /tmp/states.json

jq -r '.[] | select(.entity_id | startswith("light."))
       | [.entity_id, .attributes.friendly_name] | @tsv' /tmp/states.json

jq -r '.[] | select(.entity_id | startswith("sensor."))
       | select(.attributes.device_class == "temperature")
       | [.entity_id, .attributes.friendly_name, .attributes.unit_of_measurement]
       | @tsv' /tmp/states.json
```

**If you do not hold the token, `/ha/catalog` is the only way through.** That
happens whenever the operator connected Home Assistant themselves — through the
editor, or before you arrived — because no endpoint returns a stored token. The
panel is then the only thing that can see the catalog, and the relay is what it
sees it through: `POST /ha/catalog` with `{"stage":"states"}` returns
`202 {"request":N}`, and `GET /ha/catalog?request=N` answers `{"status":
"pending"}` until the Home Assistant result is ready, which reading consumes.
One stage may be in flight, an unconsumed one expires after 30 s, and `states`
alone carries the entity ids and the attributes the filters below need. Skipping
phase 6 because Home Assistant is already configured is normal; it does not
leave you without a way to choose entities.

Only five Home Assistant domains can be bound at all — `light`, `cover`,
`sensor`, `binary_sensor`, `scene` — so filtering by domain is not a
convenience, it is the constraint. A `climate` entity reporting a temperature is
not bindable and should not be offered.

**Done when** you have one entity id per requested tile. If either query returns
more than one plausible answer, list them with their friendly names and ask.
If either returns none, say which one and stop: a dashboard bound to an entity
that does not exist renders a placeholder naming `provider:resource`, which is a
worse outcome than a question.

## 8. Publish the dashboard

The grid is 4 columns × 3 rows below a fixed system bar, `pos` is
`[column, row]` zero-based from the top left, and `size` is one of five
rectangles. A `light` at 2×1 renders a brightness slider; a `sensor` at 2×1
renders the value with an icon chosen from the entity's own device class. That
is the dashboard the request above asks for:

```json
{
  "schema": 1,
  "theme": "midnight",
  "home_page": "home",
  "settings": {"timezone": "Europe/Warsaw", "brightness_day": 100,
               "brightness_night": 100, "night_start": "22:30",
               "night_end": "06:30", "wake_on_touch": true},
  "pages": [
    {
      "id": "home",
      "title": "Home",
      "tiles": [
        {"id": "light-1", "type": "light", "pos": [0, 0], "size": [2, 1],
         "binding": {"provider": "ha", "resource": "light.living_room"},
         "label": "Living room"},
        {"id": "temp-1", "type": "sensor", "pos": [2, 0], "size": [2, 1],
         "binding": {"provider": "ha", "resource": "sensor.living_room_temperature"},
         "label": "Living room"}
      ]
    }
  ]
}
```

`theme` must be an id the running firmware carries — `GET /info` lists them
rather than leaving you to assume `midnight` exists. `settings.timezone` is an
IANA name and the clock is meaningless without it.

`brightness_night` is `100` above on purpose. On a board as it ships the
backlight is a binary output on the CH422G expander, so every value above zero
is full brightness and only `0` turns the screen off; a `20` there would be a
night dimming you then report in phase 10 and the operator never sees. Use `0`
if they want the screen dark at night, and mention
[`backlight-dimming.md`](backlight-dimming.md) if they wanted a level — it is a
wire and a build option, not a setting. Then validate, and only then publish:

```bash
printf 'header = "Authorization: Bearer %s"\n' "$SLATE_TOKEN" |
curl -sS --config - -X POST http://192.168.1.42/api/v1/config/validate \
     -H 'Content-Type: application/json' --data-binary @dashboard.json -w '%{http_code}\n'

printf 'header = "Authorization: Bearer %s"\n' "$SLATE_TOKEN" |
curl -sS --config - -X PUT http://192.168.1.42/api/v1/config \
     -H 'Content-Type: application/json' --data-binary @dashboard.json -w '%{http_code}\n'
```

**Done when** both answer `204`. A `400 invalid_config` carries `config_errors`
and `tile_errors` keyed by tile id, each with a JSON Pointer into the document
you submitted; fix the document against
[`CONFIGURATION.md`](CONFIGURATION.md#validation-errors) and validate again
rather than trying a different shape and hoping.

## 9. Verify

A `204` means the document was accepted, not that the tiles have anything to
show. Two more requests settle that:

```bash
printf 'header = "Authorization: Bearer %s"\n' "$SLATE_TOKEN" |
curl -sS --config - http://192.168.1.42/api/v1/status

printf 'header = "Authorization: Bearer %s"\n' "$SLATE_TOKEN" |
curl -sS --config - 'http://192.168.1.42/api/v1/resources?provider=ha'
```

**Done when** `/status` reports the `ha` provider as `online`, and `/resources`
returns a snapshot for each bound entity with `available: true` and a plausible
`state` — a `power` of `on` or `off` for the light, a number and a unit for the
sensor.

Give it a few seconds before believing an empty answer. The `204` means the
screen and the subscriptions were swapped; it does not mean Home Assistant has
answered yet, and a `/resources` read in the same breath as the `PUT` can come
back short. Poll rather than diagnose: re-read until both resources appear or
about fifteen seconds have passed. Only then is a missing entity a real finding
— the panel subscribes solely to what the active dashboard references, so an
entity absent after that is a binding matching nothing in Home Assistant, which
is almost always a typo in an entity id.

Touching a tile is the one thing you cannot verify from here, so ask the
operator to do it — but **read the light's state and keep it before you ask**.
Watching for "a change" without a recorded predecessor gives you a transition
you cannot read a direction from, and the first one is the one that matters.
Then re-read `/resources` while they touch it: a `power` that flips, or a
`brightness` that lands on a new number, has already been through the whole path
— touch, action bus, provider call, subscription, back into the panel's own
store. Reading the panel rather than Home Assistant is what makes that a proof
rather than a coincidence.

Prefer a dimmable light for this if the operator has one. A 2×1 tile bound to a
resource advertising `set_brightness` is a slider, and moving it exercises that
action; a plain relay only ever produces `toggle`.

## 10. Report

Give the operator, in one message:

- the panel's address and its `slate-<mac6>` name, and that the editor is at
  `http://<address>/` in any browser including a phone;
- **the administrator PIN**, if you generated it — it is not recoverable, and
  the only way past a forgotten one is a factory reset that also takes the WiFi
  credentials and the dashboard;
- which entities you bound, and what you had to choose between;
- what you could not verify, plainly: the tap, and anything you skipped.

Mention that the cable is done with — from here updates travel over WiFi, and
the dashboard is a document that can be edited by dragging rather than by
running any of this again.

## What not to do

- Do not erase flash, or call `POST /factory_reset`, on a panel that has run
  Slate before without the operator agreeing to it in that request. Both take
  the WiFi credentials, the dashboard, the External API keys and the Home
  Assistant token.
- Do not move the host's WiFi without asking first, and do not leave it on the
  panel's access point when the phase ends.
- Do not write any credential into the repository, a commit, or a command line
  on a shared machine.
- Do not guess a WiFi passphrase, a Home Assistant URL, or which of two similar
  entities was meant.
- Do not retry into `429 try_later`; honour the `Retry-After`.
- Do not report a panel as set up on a `204` alone — phase 9 is what makes that
  claim true.
- Do not describe the tap as verified. You did not see the screen.
