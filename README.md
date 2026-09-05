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

- [What you need](#what-you-need) · [Install](#install) · [First run](#first-run)
- [Building a dashboard](#building-a-dashboard) · [Providers](#providers) ·
  [Updating](#updating) · [Security](#security)
- [Configuration reference](docs/CONFIGURATION.md) ·
  [Device API reference](docs/API.md) · [Design document](docs/DESIGN.md)

## Project status

Slate works and is in daily use. Against the milestones in [§14 of the design
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
- **M7 — done.** First-run setup, optional administrator PIN protection, clear
  network diagnostics, static addressing and the physical recovery gestures
  are in. The panel's QR contains only its address; authentication happens in
  the editor and never puts a credential in a URL.
- **M8 — the release path exists; the first release has not been tagged.**
  Pushing a `v*` tag builds the image, publishes it with checksums, and deploys
  the browser installer beside the manifest panels check for updates against.
  Enclosures are settled by pointing at a design that has been printed and
  fitted rather than by publishing one.

Until that first tag exists **there is nothing to download**, and the way to a
running panel is a build from source and one flash over a cable — the last
section of this file. After that first flash, updates travel over WiFi and the
cable goes back in the drawer.

This is a personal project published as open source. There is no commercial
roadmap and no support commitment, and hardware support is limited to the one
board below.

## What you need

- **A Waveshare ESP32-S3-Touch-LCD-7 carrying the ESP32-S3-WROOM-1-N16R8
  module** — 16 MB of flash and 8 MB of PSRAM. The partition table in
  `firmware/partitions.csv` is drawn for 16 MB and does not fit a board with
  8 MB; check the marking on the module before flashing, because the vendor
  documentation for this board describes an N8R8 that at least some units are
  not.
- **Power: about 1 A at 5 V**, from a supply and a cable that can actually
  deliver it. A thin cable on a long run browns out the backlight before
  anything reports an error. Mounting and power are the parts that are harder to
  change than code, so settle them before the panel goes on a wall.
- **A USB-C cable that carries data**, for the one flash at the beginning.
  Charging-only cables are the usual reason a board does not appear as a serial
  port.
- **A 2.4 GHz WiFi network.** The ESP32-S3 has no 5 GHz radio. On a router that
  presents both bands under one SSID this is normally fine; a 5 GHz-only SSID
  will never appear in the panel's scan.
- **A Chromium-based desktop browser** — Chrome or Edge — if you want to install
  from the browser. Firefox and Safari do not implement Web Serial. The editor
  afterwards works in any browser, including a phone.

## Install

### From the browser

Once a version is tagged, the installer lives at
**<https://mateuszsikora.github.io/slate/>**: connect the panel over USB, press
the button, pick the port. It needs no toolchain, no ESP-IDF and no command
line, and it writes the same four images `idf.py flash` would.

It also offers to erase the flash first. On a panel that has never run Slate
there is nothing to lose. On one that has, an erase takes the dashboard,
administrator security settings, External API keys and stored WiFi credentials
with it — those live outside the firmware partitions and otherwise survive both
a reflash and an update.

Every release also carries the images for `esptool`, their SHA-256 checksums,
and the `.elf` and `.map` that a crash report from that image is read against.

### From source

Building takes ESP-IDF and two asset generators, and is described in
[Building from source](#building-from-source) at the end of this file. It is
also the only path today, since nothing has been tagged yet.

## First run

1. A panel with no stored credentials raises its own access point,
   `slate-<mac6>` — open unless a passphrase has been set — and prints the name,
   the passphrase and the address to open on its screen, next to a QR that joins
   the network in one scan.
2. Join it and open `http://192.168.4.1`. Phones usually open the page
   themselves, because the device answers every DNS query with its own address,
   but the address is on the screen because that prompt is easy to dismiss.
3. Pick a network, type its password, optionally set a 4–12 digit administrator
   PIN, and submit. **The result appears on the
   panel, not in the browser:** both radios share one channel, so the access
   point drops its clients at the instant the station associates. On success the
   screen shows the station address and an address-only QR; on failure the access
   point comes back with a named reason — wrong password, out of range, no
   address from the router.
4. Scan the QR to open the editor. Enter the administrator PIN if one was set,
   choose a provider, and arrange the first page. Without a PIN the editor is
   intentionally open to anyone who can reach the panel on the LAN.

If the router later changes, none of this needs a cable: the panel raises the
access point again on its own whenever the station stays down. `DELETE
/api/v1/wifi` forgets the credentials, and `POST /api/v1/factory_reset` — also
a physical panel gesture and a button in the editor — takes the security
settings and configuration with it.

### Administrator access

The optional administrator PIN is the credential a person uses. A new browser
page asks for it on every visit and keeps the resulting session credential only
in memory. It is never shown, stored in `localStorage`, or added to the URL. In
open mode the same session begins without a prompt.

Internally, `POST /api/v1/session` exchanges the PIN for a device-session
credential used by the editor's authenticated requests. Command-line
administration can call the same endpoint and keep the returned value in an
environment variable for that shell. If the PIN is forgotten, use the physical
factory-reset gesture; there is no irreplaceable recovery token to preserve.

## Building a dashboard

The editor is served by the panel itself at `http://<address>/`: drag components
onto the grid, bind each to a resource its provider offers, and publish. While
you are dragging, the panel previews the change in RAM; the publish button is
what writes flash. Export and import are a plain JSON file, which is how a
layout survives a reflash or moves to a second panel.

The document behind the editor is the format described in
[`docs/CONFIGURATION.md`](docs/CONFIGURATION.md) — the grid, the components and
their size variants, the settings, and the validation vocabulary. Writing it by
hand is a supported path and not a fallback. Set `SLATE_TOKEN` to the `token`
returned by `POST /api/v1/session`, then:

```bash
curl -sS -X PUT http://192.168.1.42/api/v1/config \
     -H "Authorization: Bearer $SLATE_TOKEN" \
     -H 'Content-Type: application/json' \
     --data-binary @dashboard.json
```

## Providers

A provider is the adapter between one upstream system and the panel. The
components know nothing about either: a `light` tile renders the same way and
emits the same semantic actions whether a Home Assistant service call or a
three-line script is behind it.

### Home Assistant

Slate discovers local Home Assistant instances over mDNS and keeps manual URL
entry for installations on another subnet or VLAN. Configuration requires a
long-lived access token. Create it for a dedicated Home Assistant account in
the **`system-users`** group: `system-read-only` can render state but cannot call
services, while an administrator token grants more authority than Slate needs.

A long-lived token carries the full authority of its account. Slate stores it
only in device NVS, never returns it from the API, and tests authentication
before replacing working credentials.

Open **Integrations** in the editor to connect or disconnect Home Assistant.
When a resource picker first opens, the panel relays the HA registries and
current states to the browser in bounded responses. The browser assembles and
caches that catalog once for all tile pickers, then refreshes a stale cache in
the background. After publishing, the panel subscribes only to the small set of
entities used by the active dashboard.

### External API — anything that can POST JSON

Home Assistant is the first production provider, not a dependency. The `direct`
provider is always present and powers the editor's **External API** integration.
Create a named key there, copy it once, and bind a tile to it:

```json
{"id": "t1", "type": "light", "pos": [0, 0], "size": [2, 1],
 "binding": {"provider": "direct", "resource": "living-room"}}
```

Publish what that resource currently is — the panel holds state only for
resources the active dashboard references, so this is the order that works:

```bash
export SLATE_API_KEY=<external-api-key>

curl -sS -X POST http://192.168.1.42/api/v1/direct/state \
     -H "Authorization: Bearer $SLATE_API_KEY" \
     -H 'Content-Type: application/json' \
     -d '{"resource":"living-room","kind":"light","name":"Living room",
          "available":true,"state":{"power":"on","brightness":62},
          "capabilities":{"toggle":true,"set_brightness":{"min":0,"max":100}}}'
```

The tile now shows a light at 62 %. To make it do something when tapped, attach
one WebSocket client that answers `action` frames and publishes the resulting
state back:

```bash
tools/direct/agent.py 192.168.1.42
```

`tools/direct/agent.py` is that consumer in one file of standard-library Python
— attach, receive, apply, answer, publish — and `tools/direct/publish.sh`
is the request above with the key kept out of the process list. Between them
they are the reference adapter, and the shape a Node-RED flow or a home-grown
script takes. The contract they implement is in
[`docs/API.md`](docs/API.md#the-direct-provider).

One rule catches everybody once: answering an action is not confirming it. A
`success: true` acknowledges delivery; what clears the tile's pending state is
the next published snapshot showing the new value.

## Updating

**The panel offers releases, and installs one when you say so.** Once a day it
fetches a small manifest over HTTPS from the same place the browser installer is
published, and compares it with what it is running. Nothing is downloaded by
that check and nothing is installed by it: the editor's **Firmware updates**
panel is where an available release appears, and pressing *Install* is what
starts the download. The panel verifies the SHA-256 the manifest names before it
makes the new image bootable, then restarts — a wall panel does not get to
reboot itself in the middle of an evening. If the new firmware does not come
back, the bootloader returns to the previous one on its own.

The dashboard, authentication state, External API keys, Home Assistant
credentials and WiFi settings live outside the application partitions and are
kept across an update.

The same three things are available to a script:

```bash
curl -sS -H "Authorization: Bearer $TOKEN" http://192.168.1.42/api/v1/update
curl -sS -X POST -H "Authorization: Bearer $TOKEN" http://192.168.1.42/api/v1/update/check
curl -sS -X POST -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
     -d '{"version":"1.2.0"}' http://192.168.1.42/api/v1/update/install
```

Until a version is tagged there is nothing on that channel, and the panel says
exactly that — `no_release`, not an unreachable server.

**Over WiFi from your own build**, which is what development uses. The panel
accepts an image on an authenticated endpoint, which is what
`tools/ota/upload.sh` posts to:

```bash
SLATE_TOKEN=<administrator session credential> tools/ota/upload.sh 192.168.1.42
```

The host can be an address, `slate-<mac6>.local`, or a full URL, and the image
defaults to `firmware/build/slate.bin` — pass a downloaded `slate-<version>.bin`
instead to install a release. The script waits for the panel to come back and
compares the version it reports against the version that was uploaded, so a
successful exit means the image booted rather than merely uploaded. A freshly
installed image that panics or never answers is rolled back to the previous one
by the bootloader.

This path has no manifest, no signature and no HTTPS, and it is not meant to:
it is a development mechanism between a desk and a wall, on a LAN, behind the
device token. The channel above is what a panel updates itself from.

## Security

Slate is built for a LAN and its security posture says so plainly. Nothing below
is a gap waiting to be closed by a later version — these are the trades, and
they are worth knowing before the panel goes on a wall.

- **Administrator access is either PIN-protected or deliberately open.** A PIN
  is a salted hash in NVS, is rate-limited after failed attempts, and is asked
  for again on every new browser page. The internal session credential never
  appears in the QR, URL or persistent browser storage. A physical factory reset
  is the recovery path for a forgotten PIN.
- **External API keys have narrow authority.** A named key can publish direct
  state and receive direct actions, but cannot read or replace a dashboard,
  configure Home Assistant, fetch logs, upload firmware or reset the panel.
  Each key is shown once, stored only as a digest and individually revocable.
- **A long-lived Home Assistant token carries the full authority of its
  account.** Create a dedicated account in the **`system-users`** group rather
  than reusing your own. Not an administrator, which grants more than Slate
  needs — and explicitly not `system-read-only`, which cannot call services: the
  panel would render a perfect dashboard on which nothing responds to a tap, and
  the refusal comes back as a generic Home Assistant error rather than as
  "unauthorized", which makes it a slow thing to diagnose.
- **Credentials only go in.** The Home Assistant token and the WiFi passphrase
  live in NVS and no endpoint returns either. The passphrase in particular never
  enters the configuration document, because that document is something people
  export, import and share.
- **The device serves plain HTTP, deliberately.** A self-signed certificate on an
  ESP32 is a worse experience than its absence on a local network: browser
  warnings on every visit, and a private key on a device whose flash can be read
  over a cable. Anyone on your network can see the traffic between a browser and
  the panel, which includes session or API credentials in authentication frames
  and headers. Do not put
  the panel on a network you would not put a printer on, and do not expose it to
  the internet — there is no scenario in this design that requires it.
- **The setup access point is open by default and its setup page needs no
  existing administrator session.** Someone in radio range can move the panel
  to a different network and choose its future PIN. They do not gain
  provider credentials, the configuration, direct-provider publication or
  firmware upload: those answer `401` there exactly as they do everywhere else. A
  custom build can provision a WPA2 passphrase for the access point, which is
  then printed on the setup screen.
- **The core dump endpoint returns memory, so it is treated as the exception it
  is.** It is token-gated with no setup-access-point exception, the build
  excludes DRAM from dumps, and the code paths that hold a credential on a stack
  zero it when they are done.

PIN failures are rate-limited. There is no general API rate limit or origin
allow-list; either would be added if a concrete scenario asked for it.

[`SECURITY.md`](SECURITY.md) reads this section from the other side: which of
these trades are settled and will not move, what a finding outside them looks
like, and the private channel one goes to instead of a public issue.

## Building from source

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

### The first flash, over a cable

A blank device has no network, so there is no other way in:

```bash
cd firmware
idf.py -p /dev/cu.usbmodem<...> flash monitor   # macOS
idf.py -p /dev/ttyACM0 flash monitor            # Linux
```

`monitor` is optional but worth having the first time: the boot log prints the
device name, the address once WiFi is up, and whether the configuration
partition survived. Leave it with `Ctrl-]`. Everything after this is
[an update over WiFi](#updating).

### The flashing page

`flasher/` is the browser installer, and `tools/flasher/build.sh` assembles it
against a build directory:

```bash
tools/flasher/build.sh 1.0.0 firmware/build
python3 -m http.server --directory flasher/dist
```

The offsets in the manifest it writes come out of the build's own
`flash_project_args` and are checked back against `firmware/partitions.csv`, so
the page cannot drift away from the table. Serving it from `localhost` is enough
for a browser to allow Web Serial; anywhere else it needs HTTPS.
`.github/workflows/release.yml` is the same two commands on a tag, plus the
GitHub Release and the Pages deployment.

## Enclosures

This project publishes none of its own. What it can offer is a shorter list:
third-party designs that have been printed and had the panel offered up to
them, because an enclosure is verified by fitting it and not by reading its
model page.

- **[Tabletop case](https://www.printables.com/model/1425850-waveshare-esp32-s3-7inch-capacitive-touch-display)**
  by David Smith — a desk stand, and what the development panel currently sits
  in.

Printables carries a good number of other designs for this board, wall mounts
among them, so a different enclosure is a search rather than a modelling job.
Anything that gets fitted here joins the list.

## Where to go next

- [`docs/CONFIGURATION.md`](docs/CONFIGURATION.md) — the dashboard document:
  grid, components, settings, validation errors.
- [`docs/API.md`](docs/API.md) — the device API and the direct provider,
  endpoint by endpoint.
- [`docs/DESIGN.md`](docs/DESIGN.md) — the architecture decisions and the
  reasoning behind everything above, including the milestones (§14).
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — sending a change: how to build it, what
  counts as having verified it, and what happens to a firmware patch when you
  have no panel to test it on.
- [`docs/agent-workflow.md`](docs/agent-workflow.md) — the same ground for a
  coding agent working the milestone backlog, and only for one: claiming issues,
  the `in-progress` label, milestone order.
- [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) — licenses and attribution
  for everything a firmware image carries that is not Slate's own code.

Slate itself is MIT-licensed; see [`LICENSE`](LICENSE).
