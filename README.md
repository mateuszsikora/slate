# Slate

Universal firmware for the Waveshare ESP32-S3-Touch-LCD-7 that renders a native
LVGL dashboard from a declarative JSON configuration fetched at runtime. Flash
once, never compile again — the dashboard is data, not firmware.

The screen is 800×480. A fixed 56 px system bar carries the clock, the page
title and a connection indicator; the rest is a 4 × 3 grid of tiles. A tile is a
semantic component — a light, a cover, a sensor, a scene bar — not a rectangle
placed by pixel coordinates. You say what a tile *is* and what it is bound to,
and the firmware decides how it looks:

```json
{
  "id": "t1",
  "type": "light",
  "pos": [0, 0],
  "size": [2, 1],
  "binding": {"provider": "ha", "resource": "light.living_room"},
  "label": "Living room"
}
```

That is the whole idea. A dashboard is a document like the one above, pushed to
the panel over HTTP — from a drag-and-drop editor the panel itself serves, or
from a file — so rearranging it costs a request rather than a build. State and
actions come from providers: **Home Assistant**, over its WebSocket API, and
**direct**, which is any script that can POST JSON and read a WebSocket. Neither
is required by the other, and nothing outside the local network is involved.

The design document is [`docs/DESIGN.md`](docs/DESIGN.md): architecture, the
configuration schema, the device API contract, and the reasoning behind both.

## Project status

Slate works and is in daily use, but it is not yet packaged for people who did
not write it. Against the milestones in [§14 of the design
document](docs/DESIGN.md):

- **M0–M4 — done.** Board bring-up, partition table, WiFi station and setup
  access point, development OTA with rollback, remote logs and core dumps. The
  configuration and UI runtime, state store and action bus. The direct provider
  and the Home Assistant provider. The component set: light, cover, sensor,
  scene, in every size variant.
- **M5 — brightness, the night schedule and offline mode are in.** What is open
  is [#41](https://github.com/mateuszsikora/slate/issues/41): on this board the
  backlight is a binary output on the CH422G expander, so the panel takes a
  percentage and can only act on `0`. The schedule and the screen-off timer work
  as specified; a night brightness of 20 % lights the panel exactly as brightly
  as 100 % does. Smooth dimming needs a hardware modification, and that issue is
  where it is being decided.
- **M6 — done.** The web editor, served from the device, with the provider and
  resource pickers, JSON import/export, and a second theme.
- **M7 — partly.** The setup access point and its browser page, the pairing QR,
  the error screens and static addressing all work.
  [#35](https://github.com/mateuszsikora/slate/issues/35) is what is left:
  making those screens work for someone who has not read the design document,
  and adding the recovery paths that are not a `curl` — a long press on the
  panel to forget a network, factory reset without a browser.
- **M8 — not started.** The public release —
  [prebuilt binaries and a browser flasher](https://github.com/mateuszsikora/slate/issues/38),
  [release OTA with a signed manifest](https://github.com/mateuszsikora/slate/issues/37),
  and enclosure files.

The practical consequence of M8 being open is that **there is nothing to
download**. Getting a panel running today means building the firmware from this
repository and flashing it over a cable once, which is what the rest of this
file describes. After that first flash, updates travel over WiFi and the cable
goes back in the drawer.

This is a personal project published as open source. There is no commercial
roadmap and no support commitment, and hardware support is limited to the one
board below.

## Hardware

Waveshare ESP32-S3-Touch-LCD-7 carrying the **ESP32-S3-WROOM-1-N16R8** module —
16 MB of flash and 8 MB of PSRAM. The partition table in
`firmware/partitions.csv` is drawn for 16 MB and does not fit a board with
8 MB; check the module before flashing, because the vendor documentation for
this board describes an N8R8 that at least some units are not.

The panel draws up to about 1 A at 5 V. Power it from a supply and a cable that
can deliver that; a thin cable on a long run browns out the backlight before
anything reports an error.

## Building

You need:

- **ESP-IDF v5.5.5.** This is the version CI pins
  (`.github/workflows/ci.yml`) and the version every flash-size number in the
  design document was measured on. Install it with Espressif's
  [getting-started guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/get-started/),
  then `. $HOME/esp/esp-idf/export.sh` in the shell you build from.
- **Node.js 24**, for the two asset generators below. Python 3 is used by the
  font generator as well, and ESP-IDF installs one anyway. Neither is needed on
  the device or after the build.

Two kinds of build input are generated rather than committed: the LVGL fonts
(~1.1 MB of C per revision) and the editor bundle (a few hundred KB of minified
JavaScript). Nobody can review either in a diff, so neither is in git — which
means **a clean checkout cannot build the firmware until both have been
generated**. The firmware build fails without them, and the failure does not
explain itself. Run all three commands in this order:

```bash
tools/fonts/generate.sh          # → firmware/components/slate_theme/assets/
tools/editor/build.sh            # → editor/dist/index.html
cd firmware && idf.py build      # → firmware/build/slate.bin
```

The first two need network access on their first run: `generate.sh` downloads
pinned releases of Inter and Material Design Icons and verifies their SHA-256,
and `build.sh` runs `npm ci`. Both are quick and both are idempotent — re-run
them after changing `tools/fonts/icons.txt` or anything under `editor/src`.

`idf.py build` ends by printing the image size against the 6 MB application
slot. Note that `firmware/sdkconfig` is generated and gitignored:
`firmware/sdkconfig.defaults` is the file under version control, and ESP-IDF
reads it only when no `sdkconfig` exists yet. If a configuration change seems to
have no effect, delete `firmware/sdkconfig` and build again.

## Installing an image on a panel

**The first time, over a cable.** A blank device has no network and no token, so
there is no other way in:

```bash
cd firmware
idf.py -p /dev/cu.usbmodem<...> flash monitor   # macOS
idf.py -p /dev/ttyACM0 flash monitor            # Linux
```

`monitor` is optional but worth having the first time: the boot log prints the
device name, the address once WiFi is up, and whether the configuration
partition survived. Leave it with `Ctrl-]`.

**Every time after that, over WiFi.** The panel accepts an image on an
authenticated endpoint, which is what `tools/ota/upload.sh` posts to:

```bash
SLATE_TOKEN=<device token> tools/ota/upload.sh 192.168.1.42
```

The host can be an address, `slate-<mac6>.local`, or a full URL, and the image
defaults to `firmware/build/slate.bin`. The script waits for the panel to come
back and compares the version it reports against the version that was uploaded,
so a successful exit means the image booted rather than merely uploaded. A
freshly installed image that panics or never answers is rolled back to the
previous one by the bootloader.

This is a development mechanism and it says so: no manifest, no signature, no
HTTPS. The signed release path is [#37](https://github.com/mateuszsikora/slate/issues/37).

### The device token

Every write endpoint — including OTA — requires the device token that the panel
generates on first boot and keeps in NVS. It reaches a browser through the
pairing QR rendered on the screen, as a URL with the token in its query string:
scanning it opens the editor already authenticated. For a command line, read the
token out of that same URL. The firmware never logs it and the API never returns
it; the boot log prints only a fingerprint.

The token grants configuration and control of the panel. It changes on factory
reset.

## First run

1. A panel with no stored credentials raises its own access point,
   `slate-<mac6>` — open unless a passphrase has been set — and prints the name,
   the passphrase and the address to open on its screen.
2. Join it and open `http://192.168.4.1`. Phones usually open the page
   themselves, because the device answers every DNS query with its own address,
   but the address is on the screen because that prompt is easy to dismiss.
3. Pick a network, type the password, submit. **The result appears on the
   panel, not in the browser:** both radios share one channel, so the access
   point drops its clients at the instant the station associates. On success the
   screen shows the station address and the pairing QR; on failure the access
   point comes back with a named reason — wrong password, out of range, no
   address from the router.
4. Scan the pairing QR to open the editor already authenticated, choose a
   provider, and arrange the first page.

If the router later changes, none of this needs a cable: the panel raises the
access point again on its own whenever the station stays down. `DELETE
/api/v1/wifi` forgets the credentials, and `POST /api/v1/factory_reset` — also
a button in the editor — takes the tokens and the configuration with it.

## Home Assistant

Slate discovers local Home Assistant instances over mDNS and keeps manual URL
entry for installations on another subnet or VLAN. Configuration requires a
long-lived access token. Create it for a dedicated Home Assistant account in
the **`system-users`** group: `system-read-only` can render state but cannot call
services, while an administrator token grants more authority than Slate needs.

A long-lived token carries the full authority of its account. Slate stores it
only in device NVS, never returns it from the API, and tests authentication
before replacing working credentials.

Home Assistant is the first production provider, not a dependency. The `direct`
provider — `POST /api/v1/direct/state` for state, a WebSocket for actions —
drives the same components from any script. `tools/direct/publish.sh` publishes
one resource snapshot and `tools/direct/agent.py` answers the actions a tap
produces, which together are a complete round trip to copy from.

## Where to go next

- [`docs/DESIGN.md`](docs/DESIGN.md) — the architecture decisions, the
  configuration schema (§3), the device API contract (§4), the providers (§5)
  and the milestones (§14).
- [`docs/agent-workflow.md`](docs/agent-workflow.md) — how work on this
  repository is picked up and verified.
- [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) — licenses and attribution
  for the font software embedded in firmware images.

Slate itself is MIT-licensed; see [`LICENSE`](LICENSE).
