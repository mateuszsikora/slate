# Third-party notices

Slate firmware embeds generated subsets of the font software below. The
generated C sources are build artifacts; those notices apply to the subsets and
to firmware images that contain them.

The browser flashing page redistributes one further component, which is not part
of any firmware image and reaches nobody who does not open that page.

## Fonts embedded in firmware images

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
