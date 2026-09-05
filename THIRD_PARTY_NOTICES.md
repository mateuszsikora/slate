# Third-party notices

A firmware image carries everything compiled into it. In a checkout that is
invisible — every dependency has its own license file sitting beside its source
— but a release hands `slate-<version>.bin` to somebody who will never clone
anything, and MIT asks for its notice in "all copies or substantial portions"
while the BSD licenses ask for it "in the documentation and/or other materials
provided with the distribution". This file is that documentation.

It covers one firmware image: the libraries linked into it (§6.1), the editor
bundle it seeds onto the filesystem (§10), the root certificates it trusts
(§11.4), and the font subsets it draws with (ADR-6). A release publishes three
further images beside it — `bootloader-<version>.bin`,
`partition-table-<version>.bin` and `ota-data-initial-<version>.bin` — and none
of them needs an entry of its own. The bootloader links Espressif's code and
nothing else, so the ESP-IDF entry below covers it; the other two are
generated, one from `firmware/partitions.csv` and one 8 KB of `0xff` saying
that no OTA slot has been chosen yet.

The last section is the exception. It is about the browser flashing page, which
is in no image and reaches nobody who does not open it.

Versions are the ones pinned in
[`firmware/dependencies.lock`](firmware/dependencies.lock),
[`editor/package-lock.json`](editor/package-lock.json) and
[`flasher/package-lock.json`](flasher/package-lock.json). What is listed as
linked was read out of the link map of a built image rather than off the
dependency files, because those name what the build may use and the map names
what the image actually contains.

## Libraries linked into firmware images

### LVGL 9.5.0

Copyright © 2025 LVGL Kft, licensed under the MIT license, whose complete text
is in [`LICENSES/LVGL-MIT.txt`](LICENSES/LVGL-MIT.txt).

Every widget in §6.2's 2 MB PSRAM heap is one of its objects, and it is the
largest single third-party contribution to an image — ahead of the radio
libraries, and ahead of the font subsets below.

Upstream: <https://github.com/lvgl/lvgl>

#### What LVGL brings with it

LVGL's own MIT license does not cover everything in its tree, which is why it
keeps a `LICENSE.txt` beside each vendored library and a `COPYRIGHTS.md` that
says so. Three of them reach a Slate image:

- **QR Code generator** — Copyright © Project Nayuki, MIT:
  [`LICENSES/QR-Code-generator-MIT.txt`](LICENSES/QR-Code-generator-MIT.txt).
  `CONFIG_LV_USE_QRCODE=y` draws §9's setup and editor-address codes on the
  panel rather than storing a bitmap. Upstream:
  <https://github.com/nayuki/QR-Code-generator>
- **mpaland/printf** — Copyright © 2014 Marco Paland, MIT:
  [`LICENSES/printf-MIT.txt`](LICENSES/printf-MIT.txt). It is LVGL's built-in
  `snprintf`, which arrives with the default standard-library backend rather
  than with an option anybody turned on. Upstream:
  <https://github.com/mpaland/printf>
- **TLSF** — Copyright © 2006-2016 Matthew Conte, 3-clause BSD:
  [`LICENSES/TLSF-BSD-3-Clause.txt`](LICENSES/TLSF-BSD-3-Clause.txt). The same
  allocator ESP-IDF's heap uses, listed again below; an image carries both
  copies, and this one manages §6.2's 2 MB pool.

Its built-in Montserrat face is compiled in as well; it is font software and it
is in the fonts section below.

### littlefs 2.11 and esp_littlefs 1.22.3

Two projects under two licenses arrive as one component: `joltwallet/littlefs`
is the ESP-IDF binding, and it vendors littlefs itself.

littlefs is Copyright © 2022 The littlefs authors and Copyright © 2017 Arm
Limited, licensed under the 3-clause BSD license —
[`LICENSES/littlefs-BSD-3-Clause.txt`](LICENSES/littlefs-BSD-3-Clause.txt).
Upstream: <https://github.com/littlefs-project/littlefs>

The binding is Copyright 2020 Brian Pugh, licensed under the MIT license —
[`LICENSES/esp_littlefs-MIT.txt`](LICENSES/esp_littlefs-MIT.txt).
Upstream: <https://github.com/joltwallet/esp_littlefs>

Together they are the filesystem on §6.3's `storage` partition: the dashboard,
the editor bundle, and everything else an OTA is not allowed to take with it.

### ESP-IDF 5.5.5 and its Espressif components

Copyright © Espressif Systems (Shanghai) CO., LTD, licensed under the Apache
License, Version 2.0 — [`LICENSES/Apache-2.0.txt`](LICENSES/Apache-2.0.txt).
Upstream: <https://github.com/espressif/esp-idf>

The four components pinned beside it are Espressif's as well, under the same
license:

- `espressif/esp_lcd_touch` 1.2.1 and `espressif/esp_lcd_touch_gt911` 1.2.0~3 —
  the GT911 over I²C that `slate_touch` reads (§6.1).
- `espressif/esp_websocket_client` 1.8.0 — the Home Assistant connection (§5.5).
- `espressif/mdns` 1.11.3 — the `slate-<mac6>.local` name a panel answers to
  (§4.3).

The first two are published from <https://github.com/espressif/esp-bsp>, the
other two from <https://github.com/espressif/esp-protocols>.

The pre-compiled radio libraries ESP-IDF links — the WiFi stack, PHY,
coexistence and their baseband — are Apache-2.0 as well, each with a copy of the
license beside it in the ESP-IDF tree.

Apache-2.0 §4(d) passes on a NOTICE file where the licensed work ships one.
None of these ship one: ESP-IDF 5.5.5 has no NOTICE, and neither has any of the
four component archives the lock file's hashes name. The license text and the
attribution above are therefore the whole of what §4 asks for here.

### Third-party code inside ESP-IDF and the toolchain

ESP-IDF is not all Espressif's own work, and the `xtensa-esp-elf` toolchain
contributes a C library and a compiler runtime that no dependency file mentions.
What a Slate image links, with the license text for each:

- **FreeRTOS kernel** — Copyright © 2021 Amazon.com, Inc. or its affiliates,
  MIT: [`LICENSES/FreeRTOS-MIT.txt`](LICENSES/FreeRTOS-MIT.txt).
- **lwIP** — Copyright © 2001, 2002 Swedish Institute of Computer Science,
  3-clause BSD: [`LICENSES/lwIP-BSD-3-Clause.txt`](LICENSES/lwIP-BSD-3-Clause.txt).
- **Newlib** — the `libc` and `libm` the toolchain links, a collection with
  several copyright holders under BSD-family terms, reproduced in full in
  [`LICENSES/Newlib-COPYING.txt`](LICENSES/Newlib-COPYING.txt).
- **Mbed TLS** — Copyright © ARM Limited, Apache-2.0:
  [`LICENSES/Apache-2.0.txt`](LICENSES/Apache-2.0.txt). Every TLS connection a
  panel makes, and the SHA-256 that decides whether an update is the image the
  manifest named (§11.4).
- **wpa_supplicant** — Copyright © 2002-2022 Jouni Malinen and contributors,
  3-clause BSD:
  [`LICENSES/wpa_supplicant-BSD-3-Clause.txt`](LICENSES/wpa_supplicant-BSD-3-Clause.txt).
- **cJSON** — Copyright © 2009-2017 Dave Gamble and cJSON contributors, MIT:
  [`LICENSES/cJSON-MIT.txt`](LICENSES/cJSON-MIT.txt). §6.1's configuration
  parser, and the device API's answers.
- **http_parser** — based on NGINX, copyright Igor Sysoev, with changes
  copyright Joyent, Inc. and other Node contributors under MIT terms:
  [`LICENSES/http_parser-MIT.txt`](LICENSES/http_parser-MIT.txt). It is what
  `esp_http_server` reads a request line with.
- **TLSF allocator** — Copyright © 2006-2016 Matthew Conte, 3-clause BSD:
  [`LICENSES/TLSF-BSD-3-Clause.txt`](LICENSES/TLSF-BSD-3-Clause.txt). ESP-IDF's
  heap is built on it. An image carries two copies of this code, because LVGL's
  built-in allocator is the same allocator; one license text covers both.
- **Xtensa HAL** — Copyright © 2003, 2006, 2010 Tensilica Inc., MIT:
  [`LICENSES/Xtensa-libhal-MIT.txt`](LICENSES/Xtensa-libhal-MIT.txt).
- **libgcc and libstdc++** — Copyright © Free Software Foundation, Inc.,
  GPLv3 with the GCC Runtime Library Exception. The exception is what allows an
  image built by GCC to be distributed under its own terms, and it is the term
  that applies here rather than the GPL:
  <https://www.gnu.org/licenses/gcc-exception-3.1.html>

Espressif's own copyright page is the complete list of what ESP-IDF vendors,
including the parts that reach an image only through the chip's mask ROM:
<https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/COPYRIGHT.html>

## The editor bundle

### React 19.2.8, React-DOM 19.2.8 and Scheduler 0.27.0

Copyright © Meta Platforms, Inc. and affiliates, licensed under the MIT license
— [`LICENSES/React-MIT.txt`](LICENSES/React-MIT.txt).

§10's editor builds to a single document, which
`firmware/components/slate_editor` compresses into the image and seeds onto
LittleFS. React therefore ships inside `slate-<version>.bin` exactly as the C
libraries above do, even though nothing in the firmware tree depends on it.

The notice is not inside that document. React's published files open with an
`@license` banner and the production build drops them: `grep -c '@license'
editor/dist/index.html` answers 0. Vite's minifier can be told to keep them, and
deliberately is not — the bundle is 289 KB of minified output whose longest line
runs to 130 KB, which is not somewhere a notice can be found. This file is.

Upstream: <https://github.com/facebook/react>

## The root certificate store

`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` compiles a set of certificate authority
roots into the image — 17 928 bytes of it, more than the face LVGL brings and a
quarter of what the hero subset costs — and §11.4's update check is what it
exists for. It is not Mbed TLS's code and the Apache-2.0 entry above does not
reach it; it only travels in the same archive.

The data is Mozilla's. ESP-IDF's `cacrt_all.pem` says where it came from in its
own header — "Certificate data from Mozilla as of: Tue Feb 25 04:12:03 2025
GMT", extracted from NSS's `certdata.txt` with curl's `mk-ca-bundle.pl` — and
`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN=y` narrows it to the common
authorities before the build embeds them.

`certdata.txt` carries a Mozilla Public License 2.0 header, and both curl and
Debian's `ca-certificates` pass MPL-2.0 on to what they generate from it. ESP-IDF
passes nothing on: the string "Mozilla Public License" appears nowhere in its
`mbedtls` component. Whether the MPL reaches the certificates themselves is
arguable — they are public documents issued by their own authorities, and the
license sits on the collection — so the honest thing is to name the provenance,
put the text in [`LICENSES/MPL-2.0.txt`](LICENSES/MPL-2.0.txt), and let a reader
decide rather than have this file decide by staying quiet.

Upstream: <https://curl.se/docs/caextract.html>

## Fonts embedded in firmware images

The generated C sources are build artifacts (ADR-6); these notices apply to the
subsets and to the firmware images that contain them. The first two faces are
Slate's own subsets; the third arrives already generated, inside LVGL.

### Inter 4.0

Copyright © 2016 The Inter Project Authors.

Inter is licensed under the SIL Open Font License, Version 1.1. The complete
license and copyright notice are in [`LICENSES/Inter-OFL-1.1.txt`](LICENSES/Inter-OFL-1.1.txt).

Upstream: <https://github.com/rsms/inter>

### Material Design Icons 7.4.47

The Material Design Icons webfont is published by Pictogrammers and is licensed
under the Apache License, Version 2.0. The complete license is in
[`LICENSES/Apache-2.0.txt`](LICENSES/Apache-2.0.txt).

Upstream: <https://github.com/Templarian/MaterialDesign-Webfont>

### Montserrat 14 and Font Awesome 5 Free, by way of LVGL

`CONFIG_LV_FONT_MONTSERRAT_14=y` keeps one of LVGL's built-in faces in the
image, for LVGL's own internals and for any presentation built before
`slate_theme`'s faces exist. It is a generated file like the two above, and its
header names the two sources it was generated from — so both belong here:

- **Montserrat-Medium** — Copyright 2011 The Montserrat Project Authors, SIL
  Open Font License 1.1:
  [`LICENSES/Montserrat-OFL-1.1.txt`](LICENSES/Montserrat-OFL-1.1.txt).
  Upstream: <https://github.com/JulietaUla/Montserrat>
- **Font Awesome 5 Free** — Fonticons, Inc. The glyphs LVGL merged into that
  face are icons, which Font Awesome Free licenses under CC BY 4.0, while its
  font files are under SIL OFL 1.1 and its code under MIT; the download's whole
  license covers all three and is in
  [`LICENSES/FontAwesome5-Free.txt`](LICENSES/FontAwesome5-Free.txt).
  Attribution is what all three ask for, and this entry is it.
  Upstream: <https://fontawesome.com>

## Bundled by the flashing page

### ESP Web Tools 10.4.0

Copyright © Open Home Foundation and the ESP Web Tools contributors, licensed
under the Apache License, Version 2.0 — the same license text as above,
[`LICENSES/Apache-2.0.txt`](LICENSES/Apache-2.0.txt), and a copy travels beside
the bundle in every deployment of the page.

`tools/flasher/build.sh` copies it into `flasher/dist/esp-web-tools/` from the
version pinned in `flasher/package.json`. It is what talks to the board over Web
Serial; §9.1's flashing step is entirely its work.

Upstream: <https://github.com/esphome/esp-web-tools>

#### What the deployed bundle contains

What `build.sh` deploys is `dist/web`, ESP Web Tools' published build rather
than its source tree, and that build has its dependencies inlined — no bare
import is left in it, and no `@license` or `Copyright` survives the minifier
either. The `LICENSE` copied beside it is plain Apache-2.0 with no third-party
section, so this list is the only place these appear:

- **lit**, with `lit-html`, `lit-element` and `@lit/reactive-element` —
  Copyright © 2017 Google LLC, 3-clause BSD:
  [`LICENSES/lit-BSD-3-Clause.txt`](LICENSES/lit-BSD-3-Clause.txt). Upstream:
  <https://github.com/lit/lit>
- **`@material/web`** 2.5.0, **`esptool-js`** 0.6.1 and
  **`improv-wifi-serial-sdk`** 2.8.0 — Apache-2.0, the same text as above:
  [`LICENSES/Apache-2.0.txt`](LICENSES/Apache-2.0.txt). The per-chip flasher
  stubs the page loads on demand are esptool-js's.
- **pako** 2.2.0 — Copyright © 2014-2017 Vitaly Puzrin and Andrei Tuputcyn under
  MIT, with its zlib ports Copyright © 1995-2013 Jean-loup Gailly and Mark Adler
  under the zlib license; both notices are in
  [`LICENSES/pako-MIT-AND-Zlib.txt`](LICENSES/pako-MIT-AND-Zlib.txt).
- **atob-lite** 2.0.0 — MIT:
  [`LICENSES/atob-lite-MIT.txt`](LICENSES/atob-lite-MIT.txt).
- **tslib** 2.8.1 — Copyright © Microsoft Corporation, 0BSD:
  [`LICENSES/tslib-0BSD.txt`](LICENSES/tslib-0BSD.txt). It is the only one here
  that asks for nothing at all.

Versions are from [`flasher/package-lock.json`](flasher/package-lock.json).
