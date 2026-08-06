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
| GET    | `/info`            | model, firmware version, `schema_max`, name, available themes, pairing state. No auth. |
| GET    | `/config`          | current UI configuration |
| PUT    | `/config`          | replace configuration; validates, rebuilds the UI, persists. With `?transient=1` (edit mode only) the rebuild happens in RAM and nothing is written to flash — this is what live preview uses, so a drag session does not wear the flash. |
| POST   | `/config/validate` | validate without saving — returns errors keyed by `tile.id` |
| GET    | `/entities`        | entities from HA: `entity_id`, `friendly_name`, `domain`, `area`, `state`, `supported_features` |
| GET    | `/areas`           | areas from HA |
| POST   | `/ha`              | set HA URL and token; connection is tested before saving |
| GET    | `/status`          | WiFi state, HA state, RSSI, uptime, free heap, reset reason, reboot counter, entity count |
| POST   | `/mode`            | `{"mode": "normal"\|"edit"}` |
| POST   | `/identify`        | flashes the screen — for telling panels apart |
| POST   | `/ota/upload`      | development OTA; raw `.bin` body (section 11.1) |
| GET    | `/coredump`        | last core dump, if any |
| POST   | `/factory_reset`   | wipes NVS and LittleFS |

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

This handles device discovery and authorization in a single step. Because the stored token outlives a DHCP lease, the device also advertises itself over mDNS as `slate-<mac>.local`; the editor falls back to it when the remembered IP stops answering, and the error screen always shows the current address.

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

ESP-IDF 5.x, LVGL 9.3+, `esp_lcd` with an RGB panel, GT911 over I²C, CH422G as IO expander, `esp_websocket_client` for Home Assistant, `esp_http_server` for the API, cJSON for configuration, SNTP for time (the system bar clock and the night schedule are meaningless without it).

All LVGL access happens on one task; API handlers and the HA client post work to it through a queue rather than touching the tree directly.

### 6.2 Memory budget

- Framebuffer 800×480 RGB565 = 750 KB in PSRAM. Whether one framebuffer suffices or tearing forces a second (another 750 KB — affordable on 8 MB PSRAM) is measured in spike S-2, not guessed.
- Bounce buffer in internal SRAM — required, otherwise WiFi activity causes visible artifacts.
- LVGL draw buffer: ~1/10 screen, internal SRAM.
- LVGL heap: 2 MB in PSRAM — the entire widget tree budget.
- State store: sized from the configuration, ~256 B per entity. Even a config saturating the 64 KB limit stays in the tens of KB.
- Configuration: ≤64 KB, parsed into structs then freed.

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
- **128 KB of coredump.** An ELF dump of a twelve-task image resembling M1's — WiFi, lwIP, HTTP server, plus the LVGL and Home Assistant tasks — measures 21 KB. A dump stores each task's *used* stack, so the ceiling is the sum of the allocated ones: around 50 KB for that task set, and still inside 128 KB once M1 fills them. §11.3 depends on the dump surviving a panic, and a partition that truncates it is worse than no partition at all.
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
| setup   | WiFi wizard with on-screen keyboard, then a QR code with address and token |
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

## 9. First run

1. Flash from the browser using ESP Web Tools (Chromium-based browsers).
2. The device boots into setup. It scans for networks, lists them on screen, and accepts the password through an on-screen LVGL keyboard.
3. Once connected, the screen shows a QR code containing the address and token, along with the address in plain text.
4. Scanning with a phone, or typing the address, opens the editor.
5. The editor asks for the Home Assistant URL and a long-lived token. `POST /ha` verifies the connection before persisting.
6. The entity picker populates and the first page can be arranged.

Network failure handling: three failed connection attempts return the device to the wizard with a specific message (wrong password / network unavailable). Changing a router must never require reflashing.

The wizard remains reachable later: a 10-second press anywhere on the screen in error mode, or `POST /factory_reset`.

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

From this point the board can hang on a wall while development continues from a desk.

### 11.2 Rollback belongs to the same step

Without rollback, the first firmware that crashes on boot forces the panel off the wall and back onto USB — precisely what OTA was meant to avoid. This is not later hardening; it is a precondition for the scheme to work at all.

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. After a new image boots:

- WiFi connected and the HTTP server answers `/api/v1/info` within 60 s → `esp_ota_mark_app_valid_cancel_rollback()`
- otherwise reboot and automatic revert to the previous partition

The health check must not include the Home Assistant connection. If HA is down for maintenance, a perfectly good image would be rolled back. Health means "I can accept the next OTA", nothing more.

### 11.3 Logs and crashes without a cable

OTA solves flashing but not diagnostics. Without these three, the first boot loop sends you back to USB anyway:

- **Logs over WebSocket** — hook `esp_log_set_vprintf`, keep an 8 KB ring buffer, stream as `{"type": "log"}` on `/api/v1/ws`. Recent lines remain available after reconnect.
- **Core dump to flash** — `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y` plus `GET /coredump` returning the dump for `espcoredump.py`.
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
- No HTTPS on the device. A deliberate trade-off: a self-signed certificate on an ESP32 is a worse experience than its absence on a local network.

Rate limiting and origin allow-lists will be added if a concrete scenario requires them.

## 13. Spikes

Four experiments, each cheap, each capable of invalidating or reshaping the design.

### S-1 — Rebuild idempotency (critical)

Build an LVGL tree from JSON, destroy it, repeat 500 times, logging `lv_mem_monitor()`.

Pass: free heap returns to its starting value within 1%, with no downward trend. If it fails: the runtime architecture needs rethinking before anything else proceeds.

### S-2 — Render performance

20 tiles including 4 animated, with 10 entity updates per second. Measure frame time and scrolling smoothness.

Pass: no visible stutter when switching pages. Determines: the maximum tile count per page — which cannot be changed later without invalidating existing configurations — and whether a single framebuffer is enough or tearing demands a second one (section 6.2).

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
| M1 | Partition table, LCD, touch, backlight, WiFi, SNTP, LittleFS, device token, OTA, rollback, WS logs, core dump | new firmware installs over `curl`; a deliberately broken image rolls back; logs are visible remotely |
| M2 | HA client: auth, `subscribe_entities`, `call_service`, reconnect | live entity state on screen, toggle works, WiFi loss and recovery resumes automatically |
| M3 | UI runtime, light, sensor, one theme, `PUT /config` | a hand-written JSON pushed with `curl` rebuilds the screen. The panel goes on the wall. |

The device token is generated in M1, not later: every write endpoint — including development OTA — requires it from the first day the API exists.

M1 ends with a literal test: unplug the USB cable and put it away. Everything afterwards happens over the network. Reaching for the cable during M2 for firmware reasons means M1 was not finished.

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
| M7 | On-screen WiFi wizard, QR code, factory reset | the section 9 flow works with no cable and no ESP-IDF |
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
