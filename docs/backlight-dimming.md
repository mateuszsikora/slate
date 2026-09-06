# Backlight dimming

The panel Slate runs on cannot dim its backlight as it ships. This page is what
that means for a brightness percentage, the optional hardware modification that
makes those percentages real, and the one build option that firmware needs
afterwards.

You do not have to read it to use Slate. The night schedule, `screen_off_after`
and touch wake all work on an unmodified board; only the brightness *level* is
missing there.

- [What the stock board does](#what-the-stock-board-does)
- [The modification](#the-modification)
- [Building the firmware for it](#building-the-firmware-for-it)
- [What the firmware does with it](#what-the-firmware-does-with-it)
- [Checking that it worked](#checking-that-it-worked)

## What the stock board does

The backlight enable is EXIO2 on the CH422G I²C expander — the pin the
Waveshare wiki's pinout calls `DISP`. It is an ordinary output: high is lit and
low is dark, with nothing in between. No pin of the ESP32-S3 is wired to the
backlight driver's dimming input, so there is nothing for the SoC to modulate.

That is why [`CONFIGURATION.md`](CONFIGURATION.md#settings) says a brightness
percentage is currently on or off: firmware takes `brightness_night: 20`,
schedules it correctly, and then lights the panel exactly as brightly as `100`
would. Only `0` differs, and it is the value the schedule and the inactivity
timer actually depend on.

## The modification

**This voids whatever warranty the board has, and a shorted wire behind a wall
panel is worse than a bright screen at night.** It is optional in the strict
sense: firmware supports both boards, and nothing else in Slate changes.

The board carries a testpad on the backlight driver's dimming input. Bridging it
to a free GPIO gives the SoC something to modulate:

1. Solder a wire to the testpad. The [Home Assistant community thread][thread]
   this recipe comes from circles it in a photo, and
   [`inytar/waveshare-esp32-s3-touch-lcd-7-esphome`][esphome] shows a finished
   one in `brightness_solder.jpg`.
2. Route it to a GPIO the panel has not already spent. **GPIO16** is the common
   choice because it is reachable without soldering at the far end: it is the
   black wire of the RS485 jack on the back of the board, so a two-pin
   connector plugs straight in. GPIO6 works and is a tidier route for some
   people; any free output pin does.
3. **Isolate the wire.** The one warning everyone who has done this repeats.
   Electrical tape over the joint, and a route that cannot rub.

Do not use a pin this board has already spent — the sixteen RGB data lines,
HSYNC, VSYNC, DE, PCLK, the I²C pair (GPIO8/GPIO9), the touch interrupt
(GPIO4), GPIO26–37, where the module reaches its own flash and the PSRAM that
holds both framebuffers, or GPIO19/20, the USB Serial/JTAG pads. Firmware
refuses all of those by name at bring-up and falls back to on/off rather than
letting a backlight wire corrupt the picture, silence the touch controller or
take the framebuffers out from under the panel — but it cannot unsolder them
for you.

GPIO43/44 (UART0) is accepted with a warning rather than refused. It does work:
the pin is taken cleanly and the only casualty is the console the log lines
below appear on, which is the installer's call to make.

The CH422G output stays exactly where it was. It is still the enable, and the
backlight will not light without it — the dimming input does nothing while the
driver is disabled. Slate keeps driving it, so this is a wire you add rather
than a wire you move.

A photo of the modification on a Slate panel belongs here and is missing: nobody
has modified one yet. If you do it, a picture of your joint is a welcome pull
request.

[thread]: https://community.home-assistant.io/t/esp32-s3-7inch-capacitive-touch-display-adjust-brightness/771030
[esphome]: https://github.com/inytar/waveshare-esp32-s3-touch-lcd-7-esphome

## Building the firmware for it

The firmware cannot detect the wire. An unbridged pin reads exactly like a
bridged one — the far end is a driver input, not something that answers — so the
person who solders it is also the person who tells firmware about it, which they
do once, at build time:

```bash
cd firmware
idf.py menuconfig      # Component config → Slate display
idf.py build
```

| Option | Default | What it is |
|--------|---------|------------|
| `SLATE_BACKLIGHT_PWM` | off | the whole feature. Off is a board as it ships |
| `SLATE_BACKLIGHT_PWM_GPIO` | 16 | the pin the testpad is bridged to |
| `SLATE_BACKLIGHT_PWM_FREQ_HZ` | 1000 | PWM frequency. 1 kHz is what the ESPHome package for this board uses, which makes it the only value known to have driven this backlight. Raise it if yours whines audibly |
| `SLATE_BACKLIGHT_PWM_MIN_PERCENT` | 7 | the duty floor under a non-zero brightness, described below. The thread the ESPHome package cites recommends 0.3 rather than its 0.07, so expect to re-measure this |

Setting them in `menuconfig` writes `firmware/sdkconfig`, which is generated and
gitignored; putting the same lines in `firmware/sdkconfig.defaults` is what
survives deleting it.

A release image from the update channel is built without this option, so
installing one returns a modified panel to on/off until you flash your own build
again. Firmware-side that is a clean fall back — the schedule keeps working and
the levels stop being levels — but nobody has yet watched a modified board take
an OTA, and what the dimming input does with a pin left floating by firmware
that no longer drives it is untested. If yours comes back dark rather than
bright after a release update, that is the answer, and this document wants to
hear about it.

## What the firmware does with it

Nothing above `slate_display_brightness_set()` changes. The schedule, the
inactivity timer, the setup-card protection in §9.4 and the `settings` in a
dashboard document are the same on both boards; `brightness_day` and
`brightness_night` simply start meaning what they say.

Three details are worth knowing:

- **The enable still carries lit-or-dark.** Firmware asserts EXIO2 before a
  non-zero duty and releases it after a zero one. A firmware built with this
  option and installed on an *unmodified* board therefore behaves exactly like
  one built without it: the PWM pin drives nothing, and the enable does what it
  always did.
- **Zero is unlit, floor or no floor.** `screen_off_after` and a night
  brightness of `0` mean a dark panel, so the floor never lifts them.
- **Every other level is mapped above the floor.** The bottom of this
  backlight's range is dark rather than dim; the ESPHome package carries
  `min_power: 0.07` for the same reason, and the thread behind it argues for
  0.3. Brightness `1` is the floor exactly, and `100` is full duty. The
  schedule's log line reports the level that was asked for, not the duty it
  became — the floor is a property of this backlight's bottom end, not a refusal
  of the level.
- **More duty means brighter**, which is an assumption rather than a
  measurement: it is how the ESPHome package drives this pin, and nobody here
  has a modified board to confirm it on. A panel that gets darker as the number
  goes up has found the one thing this page cannot check.
- **A boot starts at full brightness.** The first frame lights the panel at
  100 % — that is what keeps uninitialised memory off the glass — and the
  schedule lowers it once it has a configuration and a synced clock. On an
  unmodified board nobody can see this, because 20 % is 100 % there anyway; on a
  modified one, a night-time boot or OTA is briefly bright.
  [#183](https://github.com/mateuszsikora/slate/issues/183) has the detail.

## Checking that it worked

The boot log says which variant is live. With the modification built in and the
channel up:

```
I (…) slate_display: backlight dimming on GPIO16: 1000 Hz, 10-bit duty, floor 7%; EXIO2 remains the enable
```

An error line *in its place* — a pin the panel already uses, or a channel that
would not configure — means firmware fell back to on/off, and the panel is
running as if the option were off. A warning line *above* it is the other case:
GPIO43/44 is yours if you want it, and the line is only there to say the console
it just printed on is the last one.

Then make it prove it, with a dashboard document whose day brightness is
somewhere in the middle:

```bash
curl -sS -X PUT http://<address>/api/v1/config \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"schema":1,"theme":"midnight","home_page":"home",
       "settings":{"brightness_day":30},
       "pages":[{"id":"home","title":"Home","tiles":[]}]}'
```

The schedule logs what it applied, and on a modified board the two halves agree:

```
I (…) brightness: day target 30% -> backlight 30% (pwm)
```

On an unmodified board the same document logs `-> backlight 100% (on/off)`,
which is the other half of the same sentence and is not a fault.
