# Slate

Universal firmware for the Waveshare ESP32-S3-Touch-LCD-7 that renders a native LVGL dashboard from a declarative JSON configuration fetched at runtime. Flash once, never compile again — the dashboard is data, not firmware.

## Hardware

Waveshare ESP32-S3-Touch-LCD-7 carrying the **ESP32-S3-WROOM-1-N16R8** module — 16 MB of flash and 8 MB of PSRAM. The partition table in `firmware/partitions.csv` is drawn for 16 MB and does not fit a board with 8 MB; check the module before flashing, because the vendor documentation for this board describes an N8R8 that at least some units are not.

See the [design document](docs/DESIGN.md).

Licenses and attribution for font software embedded in firmware images are in
[the third-party notices](THIRD_PARTY_NOTICES.md).
