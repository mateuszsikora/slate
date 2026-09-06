# Configuration reference

A Slate dashboard is one JSON document. The panel fetches it, validates it,
builds the screen from it, and keeps it in flash; changing the dashboard is
replacing the document, not rebuilding firmware. Nothing in it is
provider-specific — the same tile definition drives a Home Assistant light and a
light published by a shell script.

This page is the format. [`DESIGN.md`](DESIGN.md) §3 is the same material with
the reasoning attached, and it is the source of truth where the two disagree.
The endpoints that carry the document are in [`API.md`](API.md).

- [The document](#the-document)
- [Settings](#settings)
- [The system bar](#the-system-bar)
- [The grid](#the-grid)
- [Tiles](#tiles)
- [Components](#components)
- [Bindings](#bindings)
- [Themes](#themes)
- [Things that are not errors](#things-that-are-not-errors)
- [Validation errors](#validation-errors)
- [Pushing a configuration](#pushing-a-configuration)

## The document

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
  "bar": [
    {"type": "clock", "slot": 0, "span": 2},
    {"type": "title", "slot": 2, "span": 4},
    {"type": "page_indicator", "slot": 6, "span": 1},
    {"type": "badge", "slot": 7, "span": 2, "provider": "ha", "label": "Home"},
    {"type": "sensor", "slot": 9, "span": 3,
     "provider": "ha", "resource": "sensor.hall_temperature", "label": "Hall"}
  ],
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
          "binding": {"provider": "direct", "resource": "living-room"},
          "label": "Living room"
        }
      ]
    }
  ]
}
```

| Field | Type | Required | Meaning |
|-------|------|----------|---------|
| `schema` | integer | yes | format version. `1` is the only one that exists. A document declaring a higher version is refused with `schema_too_new` and the panel says a firmware update is needed |
| `theme` | string | yes | a theme id the running firmware carries; `GET /api/v1/info` lists them |
| `home_page` | string | yes | the page shown on boot, and the fallback after a replacement removes the page that was visible |
| `settings` | object | no | below |
| `bar` | array | no | optional twelve-slot system-bar layout; omitting it keeps the compatible layout described below |
| `pages` | array | yes | at least one page |

UTF-8, and at most **64 KB** on the wire. A page has an `id` unique in the
document, a `title` shown in the system bar, and `tiles`. Swipe horizontally
over the content area to move to the previous or next page. Navigation does not
wrap. A replacement keeps the visible page when its `id` survives the edit and
otherwise selects `home_page`.

## Settings

Every setting is optional and an omitted one is not the same as a zero.

| Setting | Type | Meaning |
|---------|------|---------|
| `timezone` | string | IANA zone name, e.g. `Europe/Warsaw`. The clock and the night schedule are meaningless without it |
| `brightness_day` | 0–100 | daytime backlight |
| `brightness_night` | 0–100 | backlight between `night_start` and `night_end` |
| `night_start`, `night_end` | `"HH:MM"` | local time, and they may wrap midnight |
| `screen_off_after` | integer minutes | blank the screen after this much inactivity. `0` means never |
| `wake_on_touch` | boolean | whether a touch on a blanked screen wakes it instead of acting on a tile |

**A brightness percentage is on or off on a board as it ships.** The backlight
is a binary output on the CH422G expander there, so any value above zero is full
brightness and only `0` turns it off. The schedule and the screen-off timer work
as specified; a night brightness of 20 % lights the panel exactly as brightly as
100 % does.

Smooth dimming needs a wire soldered from a testpad to a free GPIO, and a
firmware built with the option that names that pin. Both are optional and both
are in [`backlight-dimming.md`](backlight-dimming.md); nothing in this document
changes when you do them, the percentages simply start meaning what they say.

The screen never blanks while the panel is in setup mode — the whole point of
that mode is an address somebody can read.

## The system bar

The top 56 px of the 800×480 display is reserved for a non-interactive system
bar. After 16 px margins on both sides, its usable 768 px are twelve gapless
64 px slots. Each configured item has a zero-based `slot` from 0 to 11 and a
`span` from 1 to 12; it must fit and may not overlap another item.

| Item `type` | Extra fields | Renders |
|-------------|--------------|---------|
| `clock` | none | local time from `settings.timezone` |
| `title` | none | current page title, falling back to its id |
| `page_indicator` | none | exact `current/total` text on multi-page dashboards; empty on one page |
| `badge` | `provider` required, `label` optional | provider caption and status dot |
| `sensor`, `light`, `cover` | `provider` and `resource` required, `label` optional | that resource's state, over its name |

A badge uses the theme accent while its provider is online, warning colour
while connecting, degraded or in error, and muted colour while offline or not
configured. `label` replaces the normal provider name.

### A bar item bound to a resource

An item named after a component carries a binding and shows that resource on
the bar, the way a tile shows it on the grid:

```json
{"type": "sensor", "slot": 9, "span": 3,
 "provider": "ha", "resource": "sensor.hall_temperature", "label": "Hall"}
```

`provider` and `resource` are the same pair a tile binds, and the component
name is what tells the panel which kind to expect — exactly as a tile's `type`
does. The three that have a bar presentation are:

| `type` | Shows |
|--------|-------|
| `sensor` | a number with its unit, in the same precision the sensor tile uses, or the word a textual sensor reports |
| `light` | `On` or `Off`, with a dot in the accent colour while it is on |
| `cover` | `Open`, `Closed`, or the position it reports, with a dot in the accent colour while it is open |

A textual reading is what a `binary_sensor` gives: a door contact reads `Open`
or `Closed` and carries no unit — see
[Which Home Assistant entities can be bound](#which-home-assistant-entities-can-be-bound).

`scene` is not one of them: a scene is stateless, so an item bound to one would
never show anything, and it is refused rather than reserved as a blank slot.

`label` is optional. Without it the item shows the name the provider reported,
and falls back to the resource id when there is none. A bound item needs at
least **two slots**: 64 px holds a caption or a reading, not both. A resource
that is unavailable, or whose provider is offline, shows a dash rather than
leaving a value that stopped being true; on a `light` or a `cover` the dot
turns the warning colour with it. A `sensor` has no dot — the dash is the whole
signal.

A bar binding counts as a binding everywhere else in this document: it holds a
place in the 256-pair limit, and it has to agree with every tile that names the
same pair about what kind of thing it is.

Omitting `bar` keeps the original clock, title, connection-status and page
number arrangement. Setting `"bar": []` deliberately leaves the whole bar
blank. An unknown item type is valid and reserves its slots while rendering
nothing; this keeps later layouts geometrically stable on older firmware. Bar
items never receive touch events.

The component names are not unknown types, which is what makes them a narrowing
of the format: firmware that predates this feature accepted `{"type":"scene"}`
in the bar, and a `sensor` in a single slot, as unknown items reserving space.
Both are now refused — `bar_item_required` and `bar_span_invalid` — because a
name this firmware understands should say what is wrong with it rather than
leave a slot permanently blank.

## The grid

Below the fixed 56 px system bar is a **4 columns × 3 rows** grid of 184×124 px
cells.

`pos` is `[column, row]`, zero-based from the top left. `size` is
`[width, height]` in cells. Pixel coordinates do not exist in this format, and
the twelve cells are not a placeholder for a denser grid: a saturated page
measures 66 % of the frame budget, and a 5 × 4 grid reached 98 % at its worst
frame.

Five rectangles are expressible:

```
1×1    2×1      1×2    2×2      4×1
┌─┐    ┌───┐    ┌─┐    ┌───┐    ┌───────┐
└─┘    └───┘    │ │    │   │    └───────┘
                └─┘    └───┘
```

Tiles may not overlap and may not extend past the grid.

## Tiles

| Field | Type | Required | Meaning |
|-------|------|----------|---------|
| `id` | string | yes | unique across the **whole document**, not merely within a page — it keys validation errors and editor selection |
| `type` | string | yes | a component name; see below |
| `pos` | `[column, row]` | yes | |
| `size` | `[width, height]` | yes | |
| `binding` | object | most types | the single resource this tile shows |
| `bindings` | array | `scene` | several resources, in order |
| `label` | string | no | overrides the name the provider reports |
| `icon` | string | no | a Material Design Icons name compiled into this firmware, such as `fire` or `thermometer-low` — the list is `tools/fonts/icons.txt`. A name the firmware does not carry renders the broken-image placeholder rather than an empty space |

## Components

Each component renders a different variant per tile size. `cover` and `scene`
refuse a size they have no variant for (`invalid_size`); `light` and `sensor`
render the nearest variant they do have.

### `light`

| Size | Renders | Touch |
|------|---------|-------|
| 1×1 | icon, name, state dot | `toggle` |
| 2×1 | icon, name, brightness %, slider | `set_brightness` |
| 2×2 | large icon, brightness, colour temperature where supported | `set_brightness`, `set_color_temperature` |

Controls the resource does not advertise are hidden rather than shown inert. A
light with no brightness capability is a tile that toggles.

### `cover`

| Size | Renders | Touch |
|------|---------|-------|
| 1×1 | position-aware icon, name | `toggle` |
| 1×2 | icon, name, up/stop/down, position % | `open`, `stop`, `close` |
| 2×1 | the same, horizontally | `open`, `stop`, `close` |

Movement animates until the state settles.

### `sensor`

| Size | Renders |
|------|---------|
| 1×1 | value, unit, name |
| 2×1 | the same with a leading icon |

No actions. The resource's `measurement` — `temperature`, `humidity`,
`pressure`, `power` — selects the formatting, and its `category` selects the
icon at 2×1 and larger. A measurement implies the matching category, so a
temperature reading gets a thermometer without anyone saying so twice; a door
contact has a category and no measurement, and gets a door. A tile's own `icon`
overrides whatever the resource reported. A 24 h chart variant is deferred.

### `scene`

| Size | Bindings | Renders |
|------|----------|---------|
| 1×1 | exactly 1 | icon and name |
| 4×1 | 2 to 5 | a bar of scenes |

Scenes are stateless: the only action is `activate` and the confirmation is a
brief flash of the tile.

## Bindings

A binding is always a provider and a resource:

```json
{"provider": "direct", "resource": "living-room"}
{"provider": "ha", "resource": "light.living_room"}
```

Both are opaque strings and are compared as strings. `light.living_room` means
something to the Home Assistant adapter; `living-room` may name the same lamp in
the direct provider. Firmware never infers a provider from punctuation or from
the component type. Provider ids are at most 15 bytes and resource ids at most
63, and one document may reference at most 256 distinct pairs.

The same pair may feed several tiles, as long as every known component type
using it agrees about what kind of thing it is. Binding `direct:living-room` as
both a `light` and a `sensor` is refused.

A tile whose resource arrives as the wrong kind renders an
incompatible-binding placeholder. A binding that has never produced a snapshot
renders a placeholder naming `provider:resource`, so it can be found in the
editor rather than guessed at.

### Which Home Assistant entities can be bound

The adapter maps five HA domains onto the four component types. An entity in
any other domain is not published and the editor's picker does not list it.
The component name is the same one a tile carries in `type` and a bound system
bar item carries in its own, so a domain that can back a tile can back a bar
item — with the one exception of `scene`, which is stateless and is refused on
the bar rather than reserving slots there.

| HA domain | Component type |
|-----------|----------------|
| `light` | `light` |
| `cover` | `cover` |
| `sensor` | `sensor` |
| `binary_sensor` | `sensor` |
| `scene` | `scene` |

A `binary_sensor` — a door, a window, a motion detector, a leak detector — is a
read-only `sensor` whose value is a word rather than a number. It has no unit
and no actions: `binary_sensor.front_door` bound to a `sensor` tile reads
`Open` or `Closed`, and tapping it does nothing, because nothing about a
contact is switchable.

The same entity on a `sensor` bar item reads the same word, which is what puts
a door on the bar without a tile:

```json
{"type": "sensor", "slot": 6, "span": 3,
 "provider": "ha", "resource": "binary_sensor.front_door", "label": "Front door"}
```

The word comes from the entity's `device_class` and is the one Home Assistant
itself shows:

| `device_class` | on | off |
|----------------|----|-----|
| `door`, `garage_door`, `opening`, `window` | Open | Closed |
| `motion`, `occupancy`, `smoke`, `gas`, `carbon_monoxide`, `sound`, `vibration`, `tamper` | Detected | Clear |
| `moisture` | Wet | Dry |
| `presence` | Home | Away |
| `lock` | Unlocked | Locked |
| `connectivity` | Connected | Disconnected |
| `problem` | Problem | OK |
| `safety` | Unsafe | Safe |
| `battery` | Low | Normal |
| `battery_charging` | Charging | Not charging |
| `cold` | Cold | Normal |
| `heat` | Hot | Normal |
| `light` | Detected | No light |
| `power` | Detected | No power |
| `moving` | Moving | Not moving |
| `running` | Running | Not running |
| `plug` | Plugged in | Unplugged |
| `update` | Update available | Up-to-date |
| absent, or a class this firmware does not know | On | Off |

An entity that reports `unknown` or `unavailable` is stale and renders a dash,
not `Off`. A contact that has not answered is not a closed door.

The same `device_class` also chooses the icon a `sensor` tile shows at 2×1 and
larger, so a door contact needs no `icon` of its own:

```json
{"id": "t1", "type": "sensor", "pos": [0, 0], "size": [2, 1],
 "binding": {"provider": "ha", "resource": "binary_sensor.front_door"},
 "label": "Front door"}
```

This table covers both sensor domains — a `battery` is drawn as a battery
whether Home Assistant reports it as `41` or as `Low` — and it is why a
`sensor` whose class is outside the four `measurement` names now has an icon
too:

| `device_class` | Icon |
|----------------|------|
| `temperature`, `cold`, `heat` | thermometer |
| `humidity` | humidity |
| `pressure`, `atmospheric_pressure` | gauge |
| `power`, `current`, `voltage`, `energy`, `power_factor` | lightning bolt |
| `illuminance`, `light` | sun |
| `aqi`, `pm1`, `pm25`, `pm10` | air filter |
| `gas`, `carbon_monoxide`, `carbon_dioxide`, `volatile_organic_compounds`, `nitrogen_dioxide`, `ozone`, `sulphur_dioxide` | CO₂ molecule |
| `sound`, `sound_pressure` | speaker |
| `speed`, `wind_speed` | speedometer |
| `battery`, `battery_charging` | battery |
| `connectivity`, `signal_strength` | Wi-Fi |
| `door`, `opening` | door |
| `window` | window |
| `garage_door` | garage |
| `motion`, `moving`, `vibration` | motion sensor |
| `occupancy`, `presence` | house |
| `moisture` | drop of water |
| `smoke` | flame |
| `lock` | padlock |
| `plug` | plug |
| `problem`, `safety`, `tamper` | warning circle |
| `running`, `update` | refresh arrow |
| absent, or a class this firmware has no icon for | question mark |

Classes share an icon where the panel has no reason to draw them differently.
The icon does not change with the value: an open door and a closed one show the
same door, and the word beside it is what tells them apart. The 1×1 size
carries no icon at all.

A tile's own `icon` still overrides all of this — set one when the panel's
choice is not the one you want, or when the entity's class is not in the table:

```json
{"id": "t1", "type": "sensor", "pos": [0, 0], "size": [2, 1],
 "binding": {"provider": "ha", "resource": "binary_sensor.cellar_hatch"},
 "label": "Cellar hatch", "icon": "garage"}
```

## Themes

Two ship in firmware: `midnight` (dark) and `minimal-light`. Themes are not part
of this document beyond the id — `GET /api/v1/info` lists what the running
firmware actually carries, which is why the editor never has to hardcode them.

## Things that are not errors

The format is deliberately forgiving in three places, so that a document written
against a newer firmware degrades instead of being rejected:

- **Unknown fields are ignored.** Anywhere.
- **An unknown `type`** renders a placeholder tile at whatever grid-level size it
  asked for. It is not a validation error and it reserves no provider state.
- **An unknown system-bar item `type`** renders empty but reserves its slots.
- **An unknown `provider`** is accepted and renders a missing-provider
  placeholder.

The one thing that is not forgiven is a `schema` the firmware does not know:
that is refused outright, and the previously active dashboard stays on screen.

## Validation errors

`POST /api/v1/config/validate` answers `204` for a valid document and a `400`
like this for an invalid one — there is no `200 {"valid": false}`:

```json
{
  "error": "invalid_config",
  "config_errors": [
    {"code": "home_page_not_found", "path": "/home_page"}
  ],
  "tile_errors": {
    "living-room": [
      {"code": "tile_overlap", "path": "/pages/0/tiles/2/pos"},
      {"code": "invalid_size", "path": "/pages/0/tiles/2/size"}
    ]
  }
}
```

`path` is a JSON Pointer into the document that was submitted. `tile_errors` is
keyed by tile id, and each value is an array because one tile can break more
than one rule. Codes and paths are the machine-readable contract; the firmware
returns no presentation text, so the editor can phrase and translate its own.

| Code | Meaning |
|------|---------|
| `schema_required`, `schema_invalid` | missing, or not a positive integer |
| `schema_too_new` | newer than this firmware understands |
| `theme_required`, `theme_not_found` | missing, or not a theme this firmware carries |
| `pages_required` | missing, not an array, or empty |
| `duplicate_page_id` | two pages share an `id` |
| `home_page_not_found` | `home_page` names no page |
| `bar_required` | `bar` is present but is not an array |
| `bar_item_required` | an item is not an object, has no non-empty string `type`, or names a component with no bar presentation (`scene`) |
| `bar_slot_invalid` | `slot` is not an integer from 0 to 11 |
| `bar_span_invalid` | `span` is not an integer from 1 to 12, is below the two slots a bound item needs, or the item extends past slot 11 |
| `bar_resource_required` | a bound bar item's `resource` is missing, empty, the wrong JSON type, or over the length limit |
| `bar_overlap` | two bar items reserve at least one of the same slots |
| `tile_id_required`, `duplicate_tile_id` | document-wide, because neither has an unambiguous tile to blame |
| `invalid_position` | `pos` is not two integers |
| `invalid_size` | `size` is not one of the five rectangles, or not one this component renders |
| `tile_out_of_bounds` | the rectangle leaves the 4 × 3 grid |
| `tile_overlap` | two tiles cover the same cell |
| `binding_required` | missing or malformed binding; also a scene with the wrong number of them, a pair used by two disagreeing component kinds — bar items included, where the path is the item — and more than 256 distinct pairs |
| `provider_required`, `resource_required` | missing, empty, the wrong JSON type, or over the length limit |

Document-level refusals that are not about content use the same envelope as the
rest of the API: `empty_body`, `invalid_json`, `too_large` (413) and
`out_of_memory` (500).

## Pushing a configuration

The editor the panel serves does this for you, and can export and import the
same document as a file. By hand, first call `POST /api/v1/session` with the
optional administrator PIN and set `SLATE_TOKEN` to the returned in-memory
session credential. Then:

```bash
# Check it without touching the panel's current dashboard.
curl -sS -X POST http://192.168.1.42/api/v1/config/validate \
     -H "Authorization: Bearer $SLATE_TOKEN" \
     -H 'Content-Type: application/json' \
     --data-binary @dashboard.json -w '%{http_code}\n'

# Replace it. 204 means the screen has already been rebuilt.
curl -sS -X PUT http://192.168.1.42/api/v1/config \
     -H "Authorization: Bearer $SLATE_TOKEN" \
     -H 'Content-Type: application/json' \
     --data-binary @dashboard.json -w '%{http_code}\n'

# Read back what is running.
curl -sS http://192.168.1.42/api/v1/config -H "Authorization: Bearer $SLATE_TOKEN"
```

`PUT` validates the whole document first, then swaps the screen and every
provider subscription atomically, and only then writes flash. A document that
fails validation changes nothing at all.
