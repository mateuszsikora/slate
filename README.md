# Slate

Universal firmware for the Waveshare ESP32-S3-Touch-LCD-7 that renders a native LVGL dashboard from a declarative JSON configuration fetched at runtime. Flash once, never compile again — the dashboard is data, not firmware.

## Hardware

Waveshare ESP32-S3-Touch-LCD-7 carrying the **ESP32-S3-WROOM-1-N16R8** module — 16 MB of flash and 8 MB of PSRAM. The partition table in `firmware/partitions.csv` is drawn for 16 MB and does not fit a board with 8 MB; check the module before flashing, because the vendor documentation for this board describes an N8R8 that at least some units are not.

See the [design document](docs/DESIGN.md).

## Home Assistant

Slate discovers local Home Assistant instances over mDNS and keeps manual URL
entry for installations on another subnet or VLAN. Configuration requires a
long-lived access token. Create it for a dedicated Home Assistant account in
the **`system-users`** group: `system-read-only` can render state but cannot call
services, while an administrator token grants more authority than Slate needs.

A long-lived token carries the full authority of its account. Slate stores it
only in device NVS, never returns it from the API, and tests authentication
before replacing working credentials.

While the editor's resource picker is open, Slate relays the HA registries and
current states to the browser one response at a time; the browser assembles the
full catalog once and shares its in-memory cache between tile pickers. A stale
cache refreshes in the background instead of blocking the next tile. After
publishing, the panel keeps only the selected entity ids and subscribes to
compact updates for exactly those entities.

Open **Integrations** in the panel's embedded editor to discover an instance or
enter its URL manually, test the token, reconnect, or remove the integration.

## External API

The provider id `direct` is presented as **External API** in the editor. It is
the push integration for scripts, Node-RED and local applications: the client
publishes normalized resource snapshots to Slate and may keep a WebSocket open
to receive semantic panel actions. It does not poll arbitrary HTTP endpoints.

Create a named key under **Integrations → External API**. A key is shown once,
stored only as a SHA-256 digest, individually revocable, and scoped to
`POST /api/v1/direct/state` plus the direct action WebSocket flow. The editor
also provides a ready-to-copy publish example.

Licenses and attribution for font software embedded in firmware images are in
[the third-party notices](THIRD_PARTY_NOTICES.md).
