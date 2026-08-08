# Slate — Design Document

A universal firmware for the Waveshare ESP32-S3-Touch-LCD-7 that renders a native LVGL dashboard from a declarative configuration fetched at runtime.

Core idea: flash once, never compile again. The dashboard is data, not firmware. Visual quality is the system's responsibility, not the user's.

## 0. Motivation

Two projects already occupy this space, and each solves half the problem.

**openHASP** solved the iteration loop — universal firmware, pages pushed over MQTT, no recompilation. But its configuration is a low-level description of LVGL primitives (`{"obj": "btn", "x": 10, "y": 20, "w": 180, "h": 80}`). That gives total freedom and turns every user into a UI designer. Most people aren't, and the results show.

**ESPHome** solved the visual side — full access to LVGL styles, themes, flex and grid layouts. Anything is achievable. But its YAML describes device firmware, so every layout change means validation, compilation and flashing a full image. That is the wrong loop for something you rearrange several times a day.

Slate aims for both: a runtime that accepts a declarative config and a curated set of components that are difficult to make ugly.

### Scope

This is a personal project published as open source. There is no commercial roadmap and no support commitment. Practical consequences:

- Milestones are ordered so a usable panel is on the wall as early as possible. The main risk to a project like this is abandonment, not missing features.
- The maintainer's own dashboard is the first test case. Edge cases surface on real data within the first week rather than in speculation.
- Hardware support is limited to one board. Others may be added only if someone brings the board and is willing to test.

## 1. Architecture decisions

Short ADRs. Each records a decision and its consequence so the discussion doesn't reopen.

### ADR-1: Firmware is fixed, the UI is data

One binary per board model. The UI definition arrives from outside as JSON and is parsed at runtime.

Consequence: no layout can be hardcoded. Every component must be built dynamically from a description and destroyed without leaking memory. This constraint shapes the entire runtime.

### ADR-2: Semantic components, not primitives

Configuration says `"type": "light"`, not "a button 180×80 with a label".

Consequence: improving a component in firmware improves every user's dashboard without touching their configuration. The design system ships over OTA, which makes OTA critical infrastructure rather than a convenience.

Second consequence: anything not anticipated cannot be built. This is intentional.

### ADR-3: The device is the only Home Assistant client

The browser talks exclusively to the device. The device holds the token, subscribes to entities, calls services and exposes its own API.

```
browser ⇄ device ⇄ Home Assistant
```

Consequence: CORS is configured on the device, where we have control. The Home Assistant token never passes through the browser after initial entry. The editor works with no backend of any kind.

### ADR-4: The device API is a public contract

Rather than requiring a Home Assistant add-on, the device exposes a documented, versioned API. The bundled editor is its first client, not its only one.

Consequence: anyone can build an alternative frontend, generate configuration from a script, write a CLI, or integrate a system other than Home Assistant. A HACS integration is a later convenience layer, never a dependency.

### ADR-5: The screen is the preview

There is no second renderer. The editor shows an abstract grid for arranging tiles; the result is viewed on the device in edit mode.

Consequence: preview divergence is impossible by construction. No Emscripten build, no pixel-parity maintenance, no duplicated font pipeline. A remote mirror (framebuffer snapshot) is deferred — see section 15.

### ADR-6: Assets are compiled into the firmware

A curated set of icons and fonts ships in the binary. Nothing is fetched over HTTP.

Consequence: no asset versioning, cache invalidation or flash management. New icons arrive via OTA alongside the components that need them.

## 2. Topology

```
┌──────────────────────────────────────────┐
│ browser                                  │
│ editor (static files served by device)   │
└───────────────┬──────────────────────────┘
                │ HTTP + WS  (API v1, device token)
                ▼
┌──────────────────────────────────────────┐
│ ESP32-S3-Touch-LCD-7                     │
│                                          │
│ UI Runtime      parser → LVGL tree       │
│ Components      light / cover / sensor…  │
│ State Store     last known entity state  │
│ HA Client       WS, subscriptions, calls │
│ Config Store    NVS + LittleFS           │
│ HAL             LCD, touch, backlight    │
└───────────────┬──────────────────────────┘
                │ WebSocket API (long-lived token)
                ▼
┌──────────────────────────────────────────┐
│ Home Assistant                           │
└──────────────────────────────────────────┘
```

A custom backend connects to the same device API as the editor. The firmware cannot tell whether the other side is a browser or a script.

## 3. Configuration format

### 3.1 Principles

- JSON, UTF-8, versioned via the `schema` field.
- Describes intent, not appearance.
- Unknown fields are ignored (forward compatibility).
- An unknown component type renders a placeholder tile, never a crash.
- Size limit: 64 KB.

### 3.2 Grid

The display is 800×480. A fixed 56 px system bar at the top shows the clock, page title and connection indicator; it is not configurable.

The content area is 800×424, divided into a 4 × 3 grid with a 12 px gap and 14 px margin. Each cell is 184×124 px — enough for an icon, a value and a label, with room for a comfortable touch target.

Permitted tile sizes: 1×1, 2×1, 1×2, 2×2, 4×1.

Position is `[column, row]`. Pixel coordinates do not exist in the format.

Twelve cells is also the performance answer, not only a layout choice: S-2 measured a saturated page at 66 % of the frame budget at p95, and the same scene on a 5 × 4 grid at 98 % at the worst frame. Widening the grid would therefore spend what is left of the budget as well as invalidating every existing configuration.

### 3.3 Schema

```json
{
  "schema": 1,
  "theme": "midnight",
  "home_page": "home",
  "settings": {
    "timezone": "Europe/Warsaw",
    "brightness_day": 100,
    "brightness_night": 20,
    "night_start": "22:30",
    "night_end": "06:30",
    "screen_off_after": 0,
    "wake_on_touch": true
  },
  "pages": [
    {
      "id": "home",
      "title": "Home",
      "tiles": [
        {
          "id": "t1",
          "type": "light",
          "pos": [0, 0],
          "size": [2, 1],
          "entity": "light.living_room",
          "label": "Living room"
        },
        {
          "id": "t2",
          "type": "cover",
          "pos": [2, 0],
          "size": [1, 2],
          "entity": "cover.living_room_blind"
        },
        {
          "id": "t3",
          "type": "sensor",
          "pos": [3, 0],
          "size": [1, 1],
          "entity": "sensor.living_room_temperature"
        },
        {
          "id": "t4",
          "type": "scene",
          "pos": [0, 2],
          "size": [4, 1],
          "entities": ["scene.relax", "scene.goodnight", "scene.away"]
        }
      ]
    }
  ]
}
```

Fields common to every tile: `id`, `type`, `pos`, `size`, plus optional `label` (overrides `friendly_name` from Home Assistant) and `icon` (overrides the component default).

`timezone` is a IANA zone name mapped to a POSIX TZ string in firmware; the clock and the night schedule depend on it. `screen_off_after` is in minutes; `0` means never.

### 3.4 Migration

The firmware supports schema versions up to N. A newer configuration is rejected with a "firmware update required" screen, and the previously active configuration stays live. This version check is contractual from schema 1.

Upgrading older configurations (an in-firmware migrator that rewrites and persists) is only needed once a schema 2 exists, and is deferred until then — see section 15. What is not deferred: every schema change must ship with its migrator, so old configurations keep working without hand-editing.

## 4. Device API (contract v1)

Base: `http://<ip>/api/v1`. Everything except `/info` requires `Authorization: Bearer <device_token>`.

### 4.1 HTTP

| Method | Path               | Description |
|--------|--------------------|-------------|
| GET    | `/info`            | model, firmware version, `schema_max`, name, available themes, pairing state, current network state. No auth. |
| GET    | `/config`          | current UI configuration |
| PUT    | `/config`          | replace configuration; validates, rebuilds the UI, persists. With `?transient=1` (edit mode only) the rebuild happens in RAM and nothing is written to flash — this is what live preview uses, so a drag session does not wear the flash. |
| POST   | `/config/validate` | validate without saving — returns errors keyed by `tile.id` |
| GET    | `/entities`        | entities from HA: `entity_id`, `friendly_name`, `domain`, `area`, `state`, `supported_features` |
| GET    | `/areas`           | areas from HA |
| POST   | `/ha`              | set HA URL and token; connection is tested before saving |
| GET    | `/wifi/scan`       | nearby networks: `ssid`, `rssi`, `channel`, `auth`. Cached — see section 9.2 |
| POST   | `/wifi`            | set station credentials and, optionally, the IPv4 addressing; persist, then apply. Answers before the result is known (section 9.3) |
| DELETE | `/wifi`            | forget the credentials and raise the setup access point |
| GET    | `/status`          | network state, HA state, RSSI, uptime, free heap, reset reason, reboot counter, entity count |
| POST   | `/mode`            | `{"mode": "normal"\|"edit"}` |
| POST   | `/identify`        | flashes the screen — for telling panels apart |
| POST   | `/ota/upload`      | development OTA; raw `.bin` body (section 11.1) |
| GET    | `/coredump`        | last core dump, if any |
| POST   | `/factory_reset`   | wipes NVS and LittleFS |

Network state appears in `/info` as well as `/status`, because a browser that has just joined the setup access point has no token and still has to know what it is looking at:

```json
{"network": {"mode": "ap", "ssid": "slate-a1b2c3", "ip": "192.168.4.1", "sta_ssid": null,
             "ipv4": {"mode": "dhcp", "static": null}, "last_error": "bad_password"}}
```

`mode` is `sta` or `ap`; `ssid` is the network the device is currently on or offering; `sta_ssid` is the configured station network, which exists even while the access point is up. `last_error` is the reason the station is not connected, and it is the same string the setup screen prints — one vocabulary, so a report from the panel and a report from the API cannot disagree.

`ipv4.mode` is where the station's address came from, and it is reported rather than echoed: after a static configuration fails and the device falls back (section 9.6) the mode here is `dhcp`, because that is what the address on the screen actually is. A field that repeated the request instead would make the setup page show a number meaning two different things.

`ipv4.static` is the other half of that, and it is the stored configuration rather than the live one — what was asked for, and what became of it. It is `null` on a panel that has never been given one:

```json
{"ipv4": {"mode": "dhcp",
          "static": {"state": "gateway_unreachable", "address": "192.168.1.42/24",
                     "gateway": "192.168.1.1", "dns": ["192.168.1.1"]}}}
```

`state` is `pending`, `confirmed`, `gateway_unreachable` or `address_in_use` — section 9.6's trial, before and after. The last two are the case the two fields exist to describe together: the mode says `dhcp` because that is the address the panel is answering on, and `static` says what was typed, so the setup page can put it back in the form with the reason above it. A page that could see only the mode would have nothing to pre-fill and would be asking somebody to retype an address they have already typed once.

`address` carries its prefix, in the same CIDR form `POST /wifi` accepts, so what comes out of this endpoint can be sent straight back into that one.

The same object is what `POST /wifi` accepts:

```json
{"ssid": "home", "password": "...", "ipv4": {"mode": "dhcp"}}

{"ssid": "home", "password": "...",
 "ipv4": {"mode": "static", "address": "192.168.1.42/24",
          "gateway": "192.168.1.1", "dns": ["192.168.1.1"]}}
```

An absent or `null` `password` preserves the stored passphrase only when `ssid`
is unchanged. That is the recovery path: the setup page can correct a failed
static address without receiving or asking for the secret that remains in NVS.
For a different SSID there is no matching secret to preserve, so an absent
password means an open network. An explicit empty string always means open and
erases the stored passphrase, including when a router keeps its SSID while its
security changes.

An absent `ipv4` means `dhcp`. That default is what makes the field addable without a version — section 3.1's rule that unknown fields are ignored points the same way for a client written against a firmware that predates it. A request that says `dhcp` is a request to *stop* using a stored static address, not merely one that declines to set one: the stored configuration is erased, which is what makes the setup page's addressing control able to undo itself.

A static configuration submitted here is always on trial, whatever state a previous one reached. Section 9.6's stickiness is a property of a configuration that has proved itself, and something that has just been typed has not.

`POST /wifi` answers `202`, because the status has to say what the body cannot: the credentials are stored and are being applied, and section 9.3 is why the outcome is not knowable here. With a static address the gap is wider than it looks — the `202` says the configuration is stored, and section 9.6 may have reverted it fifteen seconds later. Its refusals are `400` unless noted: `empty_body`, `invalid_json`, `truncated`, `too_large` (`413`), `ssid_required`, `ssid_too_long`, `password_too_long`, `bad_ipv4`, `bad_ipv4_mode`, `bad_address`, `bad_gateway`, `bad_dns`, and `store_failed` (`500`). The addressing is validated whichever mode is asked for, so an address sent alongside `"dhcp"` is refused rather than quietly ignored.

A gateway must be a different host on the address's subnet. The subnet's network and broadcast addresses, and the panel's own address, are refused as `bad_gateway`: all three are valid dotted quads, but none can answer the ARP proof section 9.6 requires.

`GET /wifi/scan` serves the cache of section 9.2 and says how old it is. `age_s` is `null` when no sweep has been taken, which is a different thing from a room with no networks in it:

```json
{"networks": [{"ssid": "home", "rssi": -54, "channel": 6, "auth": "wpa2"}], "age_s": 12}
```

`auth` is `open`, `wep`, `wpa`, `wpa2`, `wpa3`, `enterprise` or `unknown` — the question a person picking a network is being asked is whether it wants a passphrase, not which key exchange it prefers. `?rescan=1` sweeps again before answering, and is the section 9.2 refresh button rather than something a client polls: the sweep makes the access point unresponsive while it runs.

The complete M1 response shapes are:

```json
{
  "model": "waveshare-s3-touch-7",
  "firmware_version": "1.0.0",
  "schema_max": 1,
  "name": "slate-a1b2c3",
  "themes": [],
  "pairing": "ready",
  "network": {"mode": "sta", "ssid": "home", "ip": "192.168.1.42", "sta_ssid": "home",
              "ipv4": {"mode": "dhcp"}, "last_error": null}
}
```

`themes` is empty until #21 adds the first theme; it lists capabilities present in this firmware rather than work planned for a later milestone. `pairing` is `ready` when a usable device token is available and `degraded` when one is not. It does not mean that a particular browser has stored the token — the device cannot observe browser `localStorage` and does not invent a second pairing database to pretend otherwise.

```json
{
  "network": {"mode": "sta", "ssid": "home", "ip": "192.168.1.42", "sta_ssid": "home",
              "ipv4": {"mode": "dhcp"}, "last_error": null},
  "ha": "unconfigured",
  "rssi": -54,
  "uptime_s": 120,
  "heap_free": 294631,
  "lvgl_heap_free": null,
  "lvgl_heap_total": null,
  "lvgl_frag_pct": null,
  "reset_reason": "power_on",
  "reboot_count": 3,
  "entity_count": 0,
  "storage_reset": false
}
```

`ha` is `unconfigured`, `disconnected` or `connected`. The three LVGL values are numbers once the LVGL allocator exists and `null` before display bring-up or when it is unavailable; reporting zero would look like a completely exhausted allocator. `reset_reason` uses stable lowercase names rather than exposing ESP-IDF enum values. `storage_reset` says that boot recovery erased corrupt NVS or reformatted LittleFS, which is different from a factory-fresh empty store even though both may have no configuration.

`GET /coredump` is the one route whose success is not JSON. It answers `200 application/octet-stream` with the core dump exactly as the panic handler wrote it to flash — ESP-IDF's header, the ELF, and the trailing checksum — which is what `esp-coredump --core-format raw` reads and also what `idf.py coredump-info` pulls over a cable. One artifact for both transports rather than one per transport. Its refusals are §4's `{"error": "..."}`: `no_coredump` (`404`) when nothing has crashed since the partition was last erased, `corrupt_coredump` (`500`) when a dump is present and fails its checksum, `coredump_read_failed` (`500`) when flash or the partition table will not cooperate, and `out_of_memory` (`500`).

`no_coredump` and `corrupt_coredump` are separate answers because they are opposite news: one is a healthy panel, the other is a crash whose record cannot be believed — and S-3 watched ESP-IDF log "Core dump has been saved to flash" three lines after the write had failed, which is precisely the lie a client must not be handed as a body. There is deliberately no `DELETE`: a panic overwrites the partition rather than appending to it, so nothing accumulates, and a `GET` that does not consume the dump is one that can be retried when the first attempt crosses a weak WiFi link.

A transfer that stops part-way is the one outcome with no error document, because the `200` has already gone. It is a chunked response abandoned without its terminating chunk, which is what tells a client the file is incomplete; the device also gives up on its own after 300 s, so a link too slow to finish cannot hold the API past the health deadline of section 11.2. A short body is therefore always a failed transfer and never a short dump — the distinction matters, because the alternative is `espcoredump` deciding the panel is confused when it was the network.

### 4.2 WebSocket `/api/v1/ws`

Event channel for the editor and for remote diagnostics. Token sent in the first frame.

Device → client:

```json
{"type": "status",   "ha": "connected", "wifi": -54, "heap_free": 142000}
{"type": "log",      "level": "warn", "msg": "entity light.x unavailable"}
{"type": "reloaded", "schema": 1, "tiles": 7}
```

Client → device:

```json
{"type": "ping"}
{"type": "mode", "mode": "edit"}
```

Heartbeat every 15 s. No ping for 60 s while in edit mode returns the device to normal.

### 4.3 Authentication

A 32-character random device token is generated on first boot and stored in NVS. It reaches the browser through a QR code rendered on screen:

```
http://192.168.1.42/?t=Xk7p...
```

The editor persists it in `localStorage`. Requests without it receive 401. A new token is issued after `factory_reset` or on explicit request from the device screen.

This handles device discovery and authorization in a single step. Because the stored token outlives a DHCP lease, the device also advertises itself over mDNS as `slate-<mac>.local`; the editor falls back to it when the remembered IP stops answering, and the error screen always shows the current address. mDNS is advertised on the setup access point too, so the same name works before the panel has ever joined a network.

There is one exception to the token, and it is narrow. While the setup access point is up, the setup page itself and the three endpoints it needs — `GET /wifi/scan`, `POST /wifi`, `GET /info` — are served **without a token, on the access point interface only**. Everything else answers 401 there exactly as it does on the station interface. The reasoning is that a token the browser must be told, when the browser has just joined an open network whose name is printed on the same screen as the token, is a step that buys nothing and costs the one flow that must not have steps. The exposure this accepts is bounded and worth stating plainly: someone within radio range can move the panel to a different network. They cannot read the Home Assistant token, write a configuration or upload firmware. Section 12 carries the same point from the security side.

## 5. Home Assistant integration

### 5.1 Connection

Home Assistant's WebSocket API at `/api/websocket`, authenticated with a long-lived access token from the user profile. The token should belong to a dedicated account in the `system-users` group — see §12, which says why that group specifically and not the read-only one.

Reconnect with exponential backoff: 1 s → 2 → 4 → 8 → 15 → 30 s (ceiling).

### 5.2 State subscription

`subscribe_entities` with an explicit `entity_ids` list derived from the configuration — never the full instance state. It returns compressed diffs, which keeps both bandwidth and parsing cost negligible at typical dashboard sizes.

Re-subscription follows any `PUT /config` that changes the entity set.

### 5.3 Entity picker

`GET /entities` requires the registries: `config/entity_registry/list` and `config/area_registry/list`. Neither needs an administrator — S-4 measured both from a `system-users` and a `system-read-only` token and got payloads byte-identical to the administrator's, and no release checked, back to 2020.12.0, admin-gates a registry `list`. Only the mutating commands are gated.

The picker therefore degrades on **failure, not on privilege**: firmware issues the command and falls back if it fails, for whatever reason — an older or unusual instance, a transport error, a future Home Assistant that tightens this. It does not predict a permission in advance and it does not check one. The fallback is a flat list derived from `subscribe_entities` with no area grouping, which yields every field of §4.1 except `area`.

This path must be implemented, not assumed away — and it is needed more often than that reads. On the instance S-4 measured, 63 % of registry entries resolve to no area at all, so the flat ungrouped list is what the picker shows for the majority of a real installation regardless of permissions. It is a first-class presentation, not an error state, and the editor (§10) must make it look deliberate.

`area` resolves through the **device**, not the entity:

```
entity_registry.area_id
  ?? device_registry[entity.device_id].area_id
  → area_registry[area_id].name
```

Zero of 1 045 entities on the measured instance carried `area_id` directly; all 387 area assignments came from the device. A `GET /entities` that reads only the entity registry returns a null `area` for every entity and looks like a bug, so `config/device_registry/list` is a third mandatory fetch.

Registries are fetched once per connection and cached in RAM, not NVS. That RAM is PSRAM: the registry, device and state payloads together are hundreds of kilobytes on the wire and several times that once parsed, which does not fit internal RAM.

### 5.4 Service calls

```json
{"id": 42, "type": "call_service", "domain": "light", "service": "toggle",
 "target": {"entity_id": "light.living_room"}}
```

Optimistic updates are mandatory. A tap immediately reflects the expected state and marks the tile pending with a subtle pulse. If no confirming state arrives within 3 s, the tile reverts and shows a brief error. Without this the panel feels broken on any latency.

## 6. Firmware

### 6.1 Stack

ESP-IDF 5.x, LVGL 9.3+, `esp_lcd` with an RGB panel, GT911 over I²C, CH422G as IO expander, `esp_websocket_client` for Home Assistant, `esp_http_server` for the API and for the setup page, `esp_wifi` in station and SoftAP modes with a small DNS responder for the captive portal (§9.2), cJSON for configuration, SNTP for time (the system bar clock and the night schedule are meaningless without it).

All LVGL access happens on one task; API handlers and the HA client post work to it through a queue rather than touching the tree directly.

### 6.2 Memory budget

- Framebuffer 800×480 RGB565 = 750 KB in PSRAM. **Two of them**, in LVGL's direct render mode, measured in S-2. The deciding figure is not tearing but internal SRAM: rendering straight into the PSRAM framebuffers needs no internal draw buffer, which returns **77 832 B** of the scarce memory in exchange for 750 KB of the abundant kind. Tearing with a single framebuffer was measurable at 5.8–11.2 torn frames per second and never visible on the panel, so it is not what buys the second buffer.
- The flush waits for VSYNC. Not an optimisation: without it the panel flickers visibly on anything that moves, because the RGB driver applies a framebuffer switch only at a frame boundary, and rendering faster than the panel scans then discards frames. Gating produces exactly one rendered frame per scan-out.
- Bounce buffer in internal SRAM — required, otherwise WiFi activity causes visible artifacts. The artifact is worth naming, because no counter on the CPU side shows it: the picture rolls vertically, with the bottom of the screen appearing at the top. That is the DMA losing its race to read the framebuffer out of PSRAM while the radio and the renderer compete for the same bus. S-2 measured frames as 4 % *cheaper* without the bounce buffer, and the display unusable.
- LVGL draw buffer: ~1/10 screen, internal SRAM — but only in a single-framebuffer configuration, which is not the one above. LVGL wants two such buffers so rendering and flushing overlap, and 2 × 76 800 B does not fit: S-2 measured 104 167 B of internal DMA-capable memory free once WiFi is up. Size any internal draw buffer from what is actually free after `esp_wifi_start()`, not from the screen.
- LVGL heap: 2 MB in PSRAM — the entire widget tree budget.
- State store: sized from the configuration, ~256 B per entity. Even a config saturating the 64 KB limit stays in the tens of KB.
- Configuration: ≤64 KB, parsed into structs then freed.
- The setup access point (§9) costs internal SRAM where there is least of it. S-2 measured 104 167 B of internal DMA-capable memory free with the station alone; `WIFI_MODE_APSTA` adds a second interface's buffers on top. It is raised on demand and torn down as soon as the station associates, never left running as a permanent second interface. `APSTA` is the intended mode and §9.4 depends on it: the station must keep trying while the access point is up, which is what lets an unattended panel recover on its own. If it does not fit, that is a budget problem to solve — not a behaviour to drop; §9.4 names the degraded shape it may not fall below.

### 6.3 Partition table

The target module is the **ESP32-S3-WROOM-1-N16R8**: 16 MB of quad flash and 8 MB of octal PSRAM. That is what the board reads out of the silicon — flash JEDEC device id `0x4018` — rather than what the vendor documentation describes, which claims N8R8. Boards with 8 MB of flash are welcome to work, but they are not a goal and the table below does not fit one.

The layout lives in `firmware/partitions.csv`:

| Partition  | Type / subtype         | Offset   | Size |
|------------|------------------------|---------:|-----:|
| `nvs`      | data / nvs             | 0x9000   | 80 KB |
| `otadata`  | data / ota             | 0x1D000  | 8 KB |
| `phy_init` | data / phy             | 0x1F000  | 4 KB |
| `ota_0`    | app                    | 0x20000  | 6 MB |
| `ota_1`    | app                    | 0x620000 | 6 MB |
| `littlefs` | data / littlefs (0x83) | 0xC20000 | 3.75 MB |
| `coredump` | data / coredump        | 0xFE0000 | 128 KB |

The sizes follow measurements rather than round numbers:

- **6 MB per application slot.** S-3 measured the skeleton at 1.34 MiB release and 1.48 MiB development (`-Og`), and §11.1 makes the development image the one that travels over the wire daily. That figure is a lower bound — no UI runtime, no component library, no Home Assistant client, no configuration parser — so the slot is sized for growth rather than for today's image.
- **3.75 MB of LittleFS.** The editor bundle (§10) targets under 400 KB gzipped and a configuration is capped at 64 KB (§3.1). The remainder is room for the deferred asset manager (§15), which would otherwise arrive as a reflash.
- **128 KB of coredump.** An ELF dump of a twelve-task image resembling M1's — WiFi, lwIP, HTTP server, plus the LVGL and Home Assistant tasks — measures 21 KB. A dump stores each task's *used* stack, so the ceiling is the sum of the allocated ones: around 50 KB for that task set, and still inside 128 KB once M1 fills them. §11.3 depends on the dump surviving a panic, and a partition that truncates it is worse than no partition at all. #14 measured the real thing across three forced panics in `app_main` at slightly different points in startup: **21 988 B to 26 020 B, 16.8 % to 19.9 % of the partition**, with up to thirteen tasks alive (`main`, both idles, `ipc0`, `ipc1`, `esp_timer`, `sys_evt`, `tiT`, `wifi`, `httpd`, `slate_wifi`, `slate_setup`, `ota_health`) — M1 without the display. The spread is the point: a dump stores each task's used stack, so *when* the panel crashes moves the number, and the ceiling is what the partition has to hold. The estimate above was the right shape.
- **`phy_init` is kept** even though `CONFIG_ESP_PHY_INIT_DATA_IN_PARTITION` is off by default. Four kilobytes now cost nothing; enabling that option later without the partition costs a serial flash.

NVS and LittleFS sit outside the application slots, so configuration and tokens survive an update (§11.4). Changing any of this later forces a full serial flash, which is why the table is settled before firmware code is written.

### 6.4 UI lifecycle

```
load config
  → validate
  → build LVGL tree
  → compute entity set → subscribe_entities
  → event loop

PUT /config
  → validate (on error: 400, existing UI untouched)
  → destroy tree
  → build new tree
  → update subscription
```

Rebuilds must be memory-idempotent. After 500 cycles the free LVGL heap returns to its starting value. This is spike S-1 and a precondition for the whole design.

### 6.5 Modes

| Mode    | Behaviour |
|---------|-----------|
| setup   | the device runs its own access point and serves the setup page. The screen shows the SSID, the password if one is set, the address and a pairing QR — section 9 |
| normal  | dashboard; touch controls entities |
| edit    | top bar reads "edit mode", touch does not call services, live preview of changes |
| offline | HA unreachable: tiles dimmed, indicator in the bar, last known values visible but clearly marked stale |
| error   | no configuration or incompatible schema: instructions and device address on screen |

## 7. Component library

Four components at launch. Each has variants driven by tile size.

### 7.1 light

| Size | Content | Action |
|------|---------|--------|
| 1×1  | icon, name, state dot | tap → `toggle` |
| 2×1  | icon, name, brightness %, slider | slider → `turn_on` with `brightness_pct` |
| 2×2  | large icon, brightness slider, colour temperature if supported | as above |

Reads `supported_color_modes` and hides controls the device does not support.

### 7.2 cover

| Size | Content | Action |
|------|---------|--------|
| 1×1  | position-aware icon, name | tap → `toggle` |
| 1×2  | icon, up / stop / down buttons, position % | `open_cover` / `stop_cover` / `close_cover` |
| 2×1  | as above, horizontal | as above |

Movement shows an animated indicator until the state settles.

### 7.3 sensor

| Size | Content |
|------|---------|
| 1×1  | value (large), unit, name (small) |
| 2×1  | as above with a leading icon |
| 2×2  | as above plus a 24 h chart (deferred — see section 15) |

No actions. `device_class` selects the icon and value formatting.

### 7.4 scene

| Size | Content | Action |
|------|---------|--------|
| 1×1  | icon, name | `scene.turn_on` |
| 4×1  | bar of 2–5 scenes | as above |

Confirmation is a brief tile flash. Scenes are stateless.

### 7.5 Shared requirements

These determine whether dashboards look good on someone else's data, and are mandatory for every component:

- **Text overflow.** A name that does not fit is ellipsized or marquee-scrolled, never clipped mid-glyph.
- **Out-of-range values.** `1013.25` and `-12.4` must fit where `21.4` was designed for. The type scale steps down automatically for longer strings.
- **Entity unavailable.** `unavailable` / `unknown` renders dimmed with a dash, not an empty tile.
- **Entity missing.** A configuration referencing a deleted entity shows a placeholder containing the `entity_id`, so it can be located in the editor.
- **Touch targets ≥ 48 px** in both dimensions.
- **Pending state** visible for every action.

## 8. Theming

Appearance derives from tokens. Users choose a theme and optionally an accent colour; they do not set forty colours individually.

```json
{
  "id": "midnight",
  "bg": "0x101114",
  "surface": "0x1A1C21",
  "surface_alt": "0x22252B",
  "text_hi": "0xF2F5F9",
  "text_lo": "0x8A94A6",
  "accent": "0x6C8CFF",
  "warn": "0xF5A524",
  "radius": 18,
  "gap": 12,
  "pad": 14
}
```

Two themes at launch: Midnight (dark) and Minimal Light. Themes live in firmware, not in user configuration; `GET /info` lists the available ids so the editor never hardcodes them.

Three type steps: hero 44 px, body 20 px, caption 15 px. Fonts are rendered at `bpp: 4` — without antialiasing everything looks dated regardless of the rest.

Icons: 60–80 Material Design Icons glyphs selected for the component set, compiled as a font. Character coverage: Latin-1 plus Polish diacritics. Wider script support is deferred, but the coverage decision is made during S-3 because it drives flash usage.

## 9. First run and the setup access point

A panel that has never been configured and a panel whose router has gone away look identical from the outside: a screen that is on and a device that answers nothing. Slate treats them as the same state and resolves both the same way — **when the station is not connected, the device raises its own access point and serves a setup page over it.** There is no combination of circumstances in which a powered panel is unreachable, and no failure that is fixed by a USB cable.

This replaces the on-screen WiFi wizard the design originally called for. A phone keyboard beats an LVGL one at typing a WPA2 passphrase, the browser is already required for everything else the panel is configured with, and removing the wizard removes the only reason setup would depend on the touch controller working.

### 9.1 The flow

1. Flash from the browser using ESP Web Tools (Chromium-based browsers).
2. The device boots, finds no credentials in NVS, and comes up in setup mode. The screen shows the access point's SSID, its password if one is set, the address to open, and a `WIFI:` QR that joins the network in one scan. This is a different QR from the pairing one of §4.3 — that one carries a URL and a token, and there is nothing to pair with until there is a network.
3. Join `slate-<mac6>` from a phone or laptop and open `http://192.168.4.1`.
4. The setup page lists nearby networks. Pick one, type the password, submit.
5. The panel reports the outcome **on its own screen** — see §9.3 for why the browser cannot. On success it shows the station address and the pairing QR with the device token; on failure the access point comes back with the reason.
6. Scanning the pairing QR, or typing the address, opens the editor.
7. The editor asks for the Home Assistant URL and a long-lived token. `POST /ha` verifies the connection before persisting.
8. The entity picker populates and the first page can be arranged.

Steps 6–8 are M6 and later. From M1 the setup page carries the WiFi form and nothing else; it grows into the editor's pairing view rather than being replaced by it.

### 9.2 The access point

| | |
|---|---|
| SSID | `slate-<mac6>` — the last three bytes of the base MAC in lowercase hex, the same suffix as `slate-<mac>.local` (§4.3) and the device name (§16), so one panel is called one thing everywhere |
| Password | **none by default.** WPA2 can be set and is then printed on the setup screen next to the SSID. The 8-character floor is the standard's, not ours |
| Address | `192.168.4.1`, the `esp_netif` default, kept because it is the address people already recognise from every other device that does this |
| DHCP | served by the device, which is also the gateway |
| Portal | a DNS responder answering every query with the device address, so phones open the page unprompted |

The setup page is **compiled into the firmware**, not served from LittleFS. It has to work on a device that has never had a filesystem, and LittleFS is what a bad OTA or a first flash is most likely to leave empty. The cost is trivial against the flash S-3 measured — 22.4 % of a 6 MB slot — and the page is a gzipped single file with no external references, targeted under 24 KB.

The captive portal is best-effort and never the only way in. Answering every DNS query is how a captive portal is detected in the first place, so both iOS and Android will label the network as having no internet and offer to leave it. The address is printed on the screen precisely so that offer costs nothing.

Answering the query is half of it. The probe that follows arrives on this server at whatever path the phone asked for — `/hotspot-detect.html` and its equivalents — so an unmatched request that came in on the access point is redirected to the setup page, and the portal sheet opens on the page rather than on a 404. Everything under the API base keeps answering section 4's `{"error": "not_found"}` instead, redirect or not: a client that asked for a route this firmware does not have wants to be told so. The DHCP server also hands out the portal's URL directly (RFC 8910), which is what a client that understands it uses in preference to any of the above.

Scan results are cached from a sweep taken when the access point comes up, not gathered per request: `esp_wifi_scan_start()` on a radio that is also running an access point makes that access point unresponsive for the duration, which a browser mid-request experiences as the panel having crashed. The page has a refresh button that re-scans and says it will take a few seconds, and a field for typing an SSID that the sweep did not find.

### 9.3 `POST /wifi` cannot tell you whether it worked

There is one radio. The access point and the station share it and must sit on the same channel, so at the moment the station associates with the router the access point moves to the router's channel and drops every client attached to it — including the browser that submitted the form, at the exact instant of success. Polling for a result from that browser is not a thing that can be made to work.

So `POST /wifi` answers as soon as the credentials are stored and validated for shape, and the **result is reported on the panel**. This is not a consolation prize for a missing feature; it is the reason the screen shows the address in the first place, and it is why requirement and hardware agree here rather than fighting.

Failures are named, not generic, and the vocabulary is shared with `/info.network.last_error` (§4.1) so the screen and the API cannot tell different stories:

| `last_error` | On screen |
|---|---|
| `bad_password` | Wrong password for `<ssid>` |
| `not_found`    | `<ssid>` is not in range |
| `no_ip`        | Joined `<ssid>` but the router gave no address |
| `auth_timeout` | `<ssid>` did not answer |
| `gateway_unreachable` | Joined `<ssid>` but `192.168.1.1` did not answer |
| `address_in_use` | `192.168.1.42` is already taken on `<ssid>` |

The last two only arise with a static address and are what section 9.6 is for. With DHCP a router that refuses to lease is `no_ip`, and there is no gateway to be wrong about.

After three failed attempts the access point returns on the same SSID with the reason on screen and the credentials still in NVS, so a corrected password is one field, not a re-entry of everything.

### 9.4 When the network goes away later

A router reboot must not tear a working dashboard off the wall and replace it with a setup card. The two cases are deliberately different:

```
cold boot
  ├─ no credentials ─────────────────────────► setup AP immediately
  └─ credentials
       └─ associate, 3 attempts ──ok─────────► normal
                              └─ fail ───────► setup AP, credentials kept

station lost while running
  └─ reconnect with backoff 1 → 2 → 4 → 8 → 15 → 30 s
       ├─ back within 5 min ─────────────────► normal; only the bar indicator ever moved
       └─ still down after 5 min ────────────► setup AP raised *alongside* the dashboard
```

In the runtime case the dashboard stays on screen with its last known values marked stale — the `offline` presentation of §6.5, which exists for a Home Assistant outage and applies here for the same reason — and the setup details appear as a banner rather than a full-screen card.

**Raising the access point must never stop the station trying.** This is a requirement, not an implementation note, and it is what makes the five minutes above a safe number rather than a gamble. A router that comes back at minute seven has to find the panel waiting for it: the panel returns to normal, the access point is torn down without ceremony, and nobody had to be in the room. Without it the fallback is a trap — the panel survives the outage and then sits on its own access point indefinitely, needing a human for a fault that fixed itself.

The mode is `WIFI_MODE_APSTA`, and §6.2 records what it costs. Should it not fit alongside everything else M1 puts in internal SRAM, the answer is to find the memory, not to drop the retry. The floor — the degraded shape this may not fall below — is an access point that yields the radio back to the station periodically, at most a minute apart, long enough to attempt an association. That drops the setup page's clients for a few seconds each time, which is a bad experience but a recoverable one. A panel that has stopped trying is not recoverable without a person.

Two further consequences that are easy to miss:

- The screen must not blank or dim while setup details are on it. `screen_off_after` and the night schedule (§3.3) are suspended in setup mode, since the whole point of the mode is an address someone can read.
- The OTA health check must not equate health with a station connection — see §11.2, which this changes.

### 9.5 Getting back to setup

`DELETE /api/v1/wifi` forgets the credentials and raises the access point. `POST /factory_reset` does the same and takes the tokens and the configuration with it. From the panel itself: a 10-second press anywhere on the screen. Changing a router must never require reflashing, and after M1 it never requires a cable either.

### 9.6 A static address proves itself before it is kept

Nothing above needs a static address, and most networks never will — a DHCP reservation on the router pins an address without any firmware. A segment without a DHCP server is the case that has no answer at all, and `no_ip` being terminal is that gap showing.

Closing it introduces the one setting that can make a panel unreachable while everything reports success. Association is layer 2 and knows nothing about the address: with a static configuration `IP_EVENT_STA_GOT_IP` fires as soon as the address is assigned, because nothing was asked of the network. The attempt therefore always succeeds. The timeout that names "associated but never addressed" as `no_ip` becomes unreachable, section 9.4's fallback is keyed on the station *not* connecting and so never raises the access point, and the screen prints an address that answers nothing.

A wrong passphrase costs a minute and names itself. A wrong gateway would cost a trip to the router's admin page — for most people the one device in the house they are least willing to open — or a factory reset, which takes the tokens and the configuration with it in exchange for a typo. That is not a trade this design gets to offer.

So the device proves the configuration before it keeps it, the way network equipment has done it for decades:

- A **freshly submitted** static configuration is applied as pending, not committed.
- After association the gateway is resolved over **ARP, not ICMP**. Gateways that drop pings are common enough that pinging would revert configurations which work. ARP also answers the second question for free: an address already in use replies from the wrong MAC, which is duplicate address detection (RFC 5227) with no extra machinery.
- No confirmation within roughly fifteen seconds reverts to DHCP, keeps the static configuration stored and marked failed, and prints the reason. The panel comes back on the network at an address on the screen, reachable from the browser that is already open.
- If DHCP does not answer either — the DHCP-less segment this feature exists for — that is the existing `no_ip` path and section 9.4 raises the access point. No new terminal state, and no new name in the station state machine.

The trial covers only a configuration that has just been submitted. Once confirmed, a static address is sticky: a router that is down at some later boot is an ordinary retry. Discarding a working address because of a five-minute outage would be a worse bug than the one this section prevents.

The contract is M1 and the implementation is M7, alongside the recovery paths of section 9.5 that stand behind it.

One deviation from RFC 5227, forced by what lwIP exposes and recorded here because it is a real difference. The RFC probes with a sender address of `0.0.0.0` *before* claiming the address; `etharp_request()` always sends the interface's own address as the sender, and `etharp_input()` caches nothing at all while the interface has none, so an answer to such a probe cannot be seen from the application. What the firmware does instead is section 2.4's ongoing detection: claim the address, announce it, and watch for somebody answering for it. The panel therefore holds a possibly-duplicate address for the second or so the check takes — the same exposure a host defending its address already has, and bounded by a revert that is written anyway.

Its other limit is worth stating rather than discovering: an access point doing proxy ARP answers for addresses it does not own, and a free address on such a network reads as `address_in_use`. The panel is still reachable when that happens — it reverts to DHCP and says why — so the failure mode is a refusal to use a static address, not a panel that has gone quiet.

## 10. Editor

Static files served from the device's LittleFS. React with a drag-and-drop grid. No backend, no cloud, no accounts.

Views:

- **Grid** — abstract rectangles labelled with component type and entity, resize handles, snapping. It deliberately does not imitate the panel's appearance; the panel does that.
- **Inspector** — properties of the selected tile: type, entity (searchable picker filtered by area), label, icon.
- **Library** — the component set, draggable onto the grid.
- **Top bar** — pages, theme, publish button, device connection indicator.

Saving: while in edit mode the editor sends `PUT /config?transient=1` debounced at 300 ms for live preview — RAM-only, no flash writes. The publish button sends a plain `PUT /config`, which persists. Bundle size target: under 400 KB gzipped.

JSON import and export are required. They protect configurations across reflashes and let people share layouts.

## 11. OTA and untethered development

Two distinct things that are frequently conflated. The first is a development tool and lands in M1. The second is a product feature and can wait.

### 11.1 Development OTA (M1)

A single endpoint. No manifest, no versioning, no HTTPS — but the device token still applies, like every write endpoint:

```
POST /api/v1/ota/upload    body: raw .bin
```

`esp_ota_begin` → `esp_ota_write` as the body streams in → `esp_ota_end` → `esp_ota_set_boot_partition` → reboot. Roughly a hundred lines. The host-side script is a `curl --data-binary @build/slate.bin` with the token header.

The answer comes before the reboot, because after it there is nobody left to answer:

```json
{"partition": "ota_1", "bytes": 927040, "version": "1.0.0-3-gd81fdc4"}
```

The 200 is what says the image was accepted and is about to boot; the body carries only what the status cannot, and degrades to `{}` if it cannot be built. There is deliberately no `status` field duplicating the code, and no announced reboot delay — the delay is a constant of the firmware, not a schedule the device can promise. `version` is read out of the image that has just been written, not the one that is running — "did the file I meant to send arrive" is the question a development flash asks, and the panel is the only party that can answer it. `partition` is the slot it went into, which is the only handle a client has on which of the two it is looking at. Failures are §4's `{"error": "..."}` with `empty_body`, `too_large`, `not_an_image`, `pending_verify` (§11.2), `no_ota_partition`, `out_of_memory`, `truncated` or `invalid_image`. No failure changes what the device boots. What a failure can cost is the *other* slot, and the line is `esp_ota_begin`: the first five are refused before it, so a wrong file costs nothing but the upload — the image header is read first, and `Content-Length` is what `esp_ota_begin` erases against rather than the whole slot. `truncated` mid-upload and `invalid_image` come after it, with the target slot already erased. §11.2 depends on that distinction: an interrupted flash leaves no spare image behind it.

From this point the board can hang on a wall while development continues from a desk.

### 11.2 Rollback belongs to the same step

Without rollback, the first firmware that crashes on boot forces the panel off the wall and back onto USB — precisely what OTA was meant to avoid. This is not later hardening; it is a precondition for the scheme to work at all.

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. After a new image boots:

- the HTTP server answers `/api/v1/info` within 60 s on whichever interface is up — the station, or the setup access point of §9 → `esp_ota_mark_app_valid_cancel_rollback()`
- otherwise reboot and automatic revert to the previous partition

Health means "I can accept the next OTA", nothing more, and the check must contain nothing else. Two exclusions follow from that and both are load-bearing:

- **Not the Home Assistant connection.** If HA is down for maintenance, a perfectly good image would be rolled back.
- **Not the station connection.** A device sitting on its own access point with the API answering can be flashed again — `POST /ota/upload` at `192.168.4.1` is the same endpoint. An image that boots while the router happens to be down is not a bad image, and rolling it back would be the same mistake as the first exclusion, arriving through a different door. Rolling back would also be actively wrong: the previous image is no more able to reach a router that is not there, so the device reboots into an identical state having thrown away the newer firmware.

### 11.3 Logs and crashes without a cable

OTA solves flashing but not diagnostics. Without these three, the first boot loop sends you back to USB anyway:

- **Logs over WebSocket** — hook `esp_log_set_vprintf`, keep an 8 KB ring buffer, stream as `{"type": "log"}` on `/api/v1/ws`. Recent lines remain available after reconnect.
- **Core dump to flash** — `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y` plus `GET /coredump` returning the dump for `espcoredump.py`. `tools/coredump/fetch.sh` is the host side, as `tools/ota/upload.sh` is for §11.1. The `.elf` it symbolicates against has to be the one that *crashed*, which §11.2 makes a real trap: a panicking image is rolled back, so the panel serving the dump is routinely not running the firmware that produced it. The device says which image it was — the boot report names the crashed task, the program counter and the ELF SHA-256 prefix of the image the dump came from, and whether that is the image now running.
- **Reset reason** — `esp_reset_reason()` and a reboot counter in `GET /status`, which immediately distinguishes a panic from a power cut.

### 11.4 Release OTA

Manifest at a stable URL:

```json
{
  "version": "1.2.0",
  "board": "waveshare-s3-touch-7",
  "url": "https://.../slate-1.2.0.bin",
  "sha256": "...",
  "min_schema": 1
}
```

`esp_https_ota` with checksum verification, a daily check, and installation only on explicit request — a wall panel must not reboot itself mid-evening.

Configuration and tokens must survive updates. They live in NVS and LittleFS, outside the application partitions. Every release is regression-tested for this.

## 12. Security

Minimal by design — the device sits on a LAN, not on the internet.

- The Home Assistant token lives only in NVS and is never returned by the API (masked in `GET /status`). It is the one secret that genuinely matters.
- The device token guards write endpoints.
- Documentation states plainly that a long-lived HA token carries full account privileges, and recommends a dedicated account in Home Assistant's **`system-users`** group. Not `system-admin`, which grants more than Slate needs, and explicitly **not `system-read-only`**, which does not work: S-4 measured that group reading every registry Slate needs while being refused `call_service`, so the panel renders a perfect dashboard on which nothing responds to a tap. The group has to be named, because "restricted" reads like "read-only" to anyone skimming. The failure is also quiet: a denied service call comes back as `home_assistant_error`, not `unauthorized`, so firmware matching on the error code classifies a permission problem as a transient fault and reverts the optimistic update (§5.4) on every tap, forever, with no hint that the account is the cause.
- The WiFi passphrase written by `POST /wifi` lives in NVS and is never returned by the API, and it never enters the configuration JSON — §10 makes that file something people export, import and share, and a credential does not belong in a document with those properties.
- **The setup access point is open by default**, and the setup page on it is served without a token (§4.3). This is the one place the token rule is relaxed, and the trade is worth naming rather than discovering. What an attacker in radio range gets is the ability to move the panel to a different network. What they do not get is the Home Assistant token, the configuration, or `/ota/upload` — those answer 401 on the access point exactly as they do on the station, and a panel moved to a hostile network still holds every secret behind a token that only the screen has shown. The threat model is a room, and the mitigation is the same one the whole pairing scheme rests on: the screen is in that room and the attacker is not. A WPA2 passphrase can be set for anyone whose radio range is a shared building rather than a house; it is then displayed on the setup screen, which is the same trade one layer down.
- No HTTPS on the device. A deliberate trade-off: a self-signed certificate on an ESP32 is a worse experience than its absence on a local network.
- **`GET /coredump` (§11.3) returns memory, so it is the one endpoint whose body is not a curated document.** An ELF core dump carries task stacks, which is where a secret is on its way to or from NVS. Three things keep the two rules above true rather than approximately true. The dump is token-gated like every write, with no setup-access-point exception. `CONFIG_ESP_COREDUMP_CAPTURE_DRAM` stays off, so `.bss`, `.data` and the heap are not in the dump — and the device token, which lives in `.bss`, is therefore not in it either. And the code paths that hold a passphrase or an HA token on a stack zero it as soon as they are done, for this reason and with this section named at the call site; that is a habit the firmware has to keep, not a property of the endpoint. Enabling `CAPTURE_DRAM` would break the arrangement, which is a second reason it is off.

Rate limiting and origin allow-lists will be added if a concrete scenario requires them.

## 13. Spikes

Four experiments, each cheap, each capable of invalidating or reshaping the design.

### S-1 — Rebuild idempotency (critical)

Build an LVGL tree from JSON, destroy it, repeat 500 times, logging `lv_mem_monitor()`.

Pass: free heap returns to its starting value within 1%, with no downward trend. If it fails: the runtime architecture needs rethinking before anything else proceeds.

### S-2 — Render performance

A saturated page with 4 animated tiles, driven at 10 entity updates per second. Measure frame time and scrolling smoothness.

Pass: no visible stutter when switching pages. Determines: the maximum tile count per page — which cannot be changed later without invalidating existing configurations — and whether a single framebuffer is enough or tearing demands a second one (section 6.2).

Answered in `docs/spikes/s2.md`: **the maximum tile count is the grid**. Twelve tiles — §3.2 saturated — cost 17 443 µs at p95 against a 26 441 µs frame budget, with no frame over budget and nothing visible on the panel. **Two framebuffers**, for the reason recorded in §6.2. The original brief asked for 20 tiles, which §3.2 cannot express, so 20 was measured as an overload on a relaxed 5 × 4 grid rather than reported as a maximum; it reaches 98 % of the budget at the worst frame.

Two findings outrank the ones the spike was asked for, both in §6.2: the draw buffer specified there does not fit once WiFi is running, and LVGL's default 33 ms refresh period beats against this panel's 37.8 Hz and visibly judders — the frame rate has to be set by VSYNC, not by LVGL's timer.

### S-3 — Flash budget

Build a skeleton with LVGL, WebSocket, TLS, the three type steps of §8, the second icon size §7.1 needs, and the icon set — five faces, not two. Measure image size.

Answered in `docs/spikes/s3.md`: 1 409 680 B release, 1 547 600 B development, which is 22.4 % and 24.6 % of the 6 MB application slot in §6.3. The original criterion was a 3 MB slot on 8 MB flash; the board turned out to have 16 MB, so the criterion is recorded as met rather than binding.

### S-4 — Home Assistant registry permissions

Call `config/entity_registry/list` and `config/area_registry/list` with a non-admin token.

Answered in `docs/spikes/s4.md`: the area-aware picker is a primary feature. A `system-users` and a `system-read-only` token both read both registries in full, returning payloads byte-identical to an administrator's, and no release checked back to 2020.12.0 admin-gates a registry `list`. §5.3 and §12 are corrected accordingly. Two findings outrank the one the spike was asked for: `area` resolves entity → device → area, so `config/device_registry/list` is mandatory, and `system-read-only` cannot call services, which disqualifies it as the recommended account.

## 14. Milestones

Ordered so that a usable panel is mounted after M3 and everything afterwards improves something already in service. Each milestone ends in a state worth stopping at.

### Phase 1 — working panel

| | Scope | Done when |
|--|-------|-----------|
| M0 | Spikes S-1…S-4 | four written answers; board variant and partition sizes decided |
| M1 | Partition table, LCD, touch, backlight, WiFi station **and setup access point**, SNTP, LittleFS, device token, OTA, rollback, WS logs, core dump | a panel with no credentials opens its own access point and is pointed at a network from a phone; new firmware installs over `curl`; a deliberately broken image rolls back; logs are visible remotely |
| M2 | HA client: auth, `subscribe_entities`, `call_service`, reconnect | live entity state on screen, toggle works, WiFi loss and recovery resumes automatically |
| M3 | UI runtime, light, sensor, one theme, `PUT /config` | a hand-written JSON pushed with `curl` rebuilds the screen. The panel goes on the wall. |

The device token is generated in M1, not later: every write endpoint — including development OTA — requires it from the first day the API exists.

The setup access point is in M1 for the same kind of reason and it is not a comfort feature. Without it, M1's WiFi credentials arrive from a build-time configuration, which means the first change of network — a router swapped, an SSID renamed, a panel carried to another room — costs a serial flash. That is the exact failure M1 exists to eliminate, and it would sit in the middle of it until M7.

M1 ends with a literal test: unplug the USB cable and put it away. Everything afterwards happens over the network. Reaching for the cable during M2 for firmware reasons means M1 was not finished — and the network is included in "firmware reasons", which is what the access point buys.

After M3 the iteration loop exists: edit JSON, push, observe. That alone is faster than the ESPHome cycle.

### Phase 2 — maturing on real data

| | Scope | Done when |
|--|-------|-----------|
| M4 | cover, scene, all size variants, edge cases from 7.5 | a week of daily use with nothing that irritates |
| M5 | Brightness, night schedule, offline mode | nobody in the house complains about night-time glare; a Home Assistant restart does not freeze the panel |

M5 outranks the editor because a wall panel without a brightness schedule gets unplugged within days.

### Phase 3 — public release

| | Scope | Done when |
|--|-------|-----------|
| M6 | Web editor, entity picker, second theme | someone unfamiliar builds a page without reading the JSON format |
| M7 | Setup screen and portal polish, pairing QR, error mode, factory reset from the panel, static addressing (9.6) | the section 9 flow works with no cable and no ESP-IDF, for someone who has not read this document |
| M8 | Release OTA with manifest, prebuilt binary, browser flasher, README, enclosure files | someone without ESP-IDF gets a running panel and receives updates |

M0 is a weekend. M1–M3 carry the technical risk. M4–M5 are the bulk of the hours at the lowest risk. M6–M8 are conditional — they determine whether anyone else can use this.

## 15. Deferred

Ordered by when each is likely to become the limiting factor:

- **More components** — thermostat, media player, weather, energy, presence, alarm. Added one at a time as real dashboards demand them.
- **Remote preview** — `lv_snapshot_take()`, downscaled to 400×240 RGB565 (~190 KB), sent over WebSocket, drawn into a `<canvas>`. No second renderer, no WASM.
- **Historical charts** via `history/history_during_period`.
- **Schema migrator** — the version-check contract exists from schema 1 (section 3.4); the upgrade code is written when schema 2 is.
- **HACS integration** — a convenience layer over the same device API, available on all Home Assistant installations (unlike an add-on, which requires HA OS or Supervised).
- **Asset manager** — HTTP-fetched icons and fonts, once the built-in set stops sufficing.
- **Additional boards** — each has different pins, panel and touch controller.

## 16. Open questions

- **Backlight dimming.** On this board the CH422G controls the backlight as a binary output; smooth dimming requires bridging one pin to a GPIO. The firmware should detect both variants: the schedule works in on/off mode unmodified and dims smoothly after the modification, which is documented as optional. Decided before M5.
- **Power and mounting.** Budget roughly 1 A at 5 V with adequate conductor cross-section. Settled before M3, since it constrains where the panel can hang — harder to change than code.
- **Multiple panels.** Device name suffixed with the MAC address; `POST /identify` to distinguish them. Whether the editor manages several panels from one view is deferred until a second one exists.
- **Name collisions.** Before the repository goes public, check for an active project of the same name near ESP32 or Home Assistant. Slack's Slate and the editor libraries are unrelated fields and pose no practical conflict.
