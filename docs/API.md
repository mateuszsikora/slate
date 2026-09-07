# Device API reference

Everything that configures or drives a panel goes through this API, including
the editor the panel serves and the direct provider. It is a public contract:
the browser talks to the device and to nothing else, and no cloud service sits
anywhere in the picture.

This page is what each endpoint answers. [`DESIGN.md`](DESIGN.md) §4, §5 and §11
are the same material with the reasoning attached and are the source of truth
where the two disagree. The document these endpoints carry is
[`CONFIGURATION.md`](CONFIGURATION.md).

- [Base and authentication](#base-and-authentication)
- [Errors](#errors)
- [Endpoints](#endpoints)
- [WebSocket](#websocket)
- [The direct provider](#the-direct-provider)
- [What is deliberately absent](#what-is-deliberately-absent)

## Base and authentication

```
http://<address>/api/v1
```

`<address>` is the panel's IP, or `slate-<mac6>.local` — it advertises itself
over mDNS, including on its own setup access point, so a changed DHCP lease does
not make the panel disappear.

`GET /info` and `POST /session` are the public browser bootstrap. The person
configuring WiFi may choose a 4–12 digit administrator PIN; without one, the
panel is deliberately open to anyone who can reach it on the LAN. Start a
session with one of:

```bash
curl -sS -X POST http://<address>/api/v1/session
curl -sS -X POST http://<address>/api/v1/session \
     -H 'Content-Type: application/json' -d '{"pin":"1234"}'
```

The response is `{"token":"<32-character session credential>"}`. The editor
keeps it only in page memory and sends it on authenticated requests:

```
Authorization: Bearer <32-character session credential>
```

It is an internal transport credential, not a recovery secret: it is never
shown to the person, placed in a URL or stored in `localStorage`. A new page or
mDNS-origin move starts a new session and asks for the PIN again. Five failed
PIN attempts return `429 try_later` with `Retry-After: 30`. Changing the PIN or
factory-resetting the panel rotates the credential.

The editor can also create named External API keys. Those keys are accepted
only by `POST /direct/state` and the direct action-consumer WebSocket role; they
cannot administer the panel. Requests without an accepted credential get
`401`.

**One exception, on the setup access point only.** While the panel is offering
its own network, the setup page and the three endpoints it needs —
`GET /wifi/scan`, `POST /wifi`, `GET /info` — answer without a session on that
interface. Everything else answers `401` there exactly as it does on the
station, so somebody in radio range can move the panel to another network and
cannot read credentials, publish state, write a configuration or upload
firmware.

## Errors

Failures are `{"error": "<stable code>"}` with a matching status. Codes are the
contract; the panel returns no presentation text. The ones that can arrive from
any endpoint that takes a body are `empty_body`, `invalid_json`, `too_large`
(413) and `out_of_memory` (500).

## Endpoints

| Method | Path | |
|--------|------|--|
| GET | `/info` | identity and network state. **No authentication** |
| POST | `/session` | exchange the optional administrator PIN for an in-memory session credential. **No authentication** |
| GET | `/config` | the active dashboard document |
| PUT | `/config` | replace it |
| POST | `/config/validate` | check one without saving |
| GET | `/resources?provider=<id>` | normalized resources for the picker |
| POST | `/direct/state` | publish one resource snapshot |
| GET | `/ha` | report whether Home Assistant is configured and its URL |
| POST | `/ha` | configure Home Assistant |
| DELETE | `/ha` | disconnect Home Assistant and erase its token |
| GET | `/ha/discover` | find Home Assistant instances over mDNS |
| POST, GET | `/ha/catalog` | relay one HA catalog stage to the editor |
| GET, POST, DELETE | `/integration-keys` | list, create or revoke External API keys |
| GET | `/wifi/scan` | nearby networks |
| POST | `/wifi` | set station credentials and addressing |
| DELETE | `/wifi` | forget them and raise the setup access point |
| GET | `/status` | network, providers, heap, uptime, reset reason |
| POST | `/mode` | `normal` or `edit` |
| POST | `/identify` | flash the screen, to tell two panels apart |
| POST | `/ota/upload` | install a firmware image |
| GET | `/update` | the release channel: what is running, what is offered |
| POST | `/update/check` | look at the manifest now |
| POST | `/update/install` | install the offered release and restart |
| POST | `/update/settings` | make the daily check, or stop making it |
| GET | `/coredump` | the last crash dump |
| POST | `/factory_reset` | erase everything and reboot |

### `GET /info`

One of the endpoints a browser can reach before it has a session, which is why it
carries the network state as well as the identity.

```json
{
  "model": "waveshare-s3-touch-7",
  "firmware_version": "1.0.0",
  "schema_max": 1,
  "name": "slate-a1b2c3",
  "themes": ["midnight", "minimal-light"],
  "authentication": "pin",
  "network": {"mode": "sta", "ssid": "home", "ip": "192.168.1.42",
              "sta_ssid": "home", "ipv4": {"mode": "dhcp"}, "last_error": null}
}
```

`themes` is what this firmware actually carries. `authentication` is `pin` or
`open`; it tells a new editor whether it should show the PIN prompt before
calling `/session`.

`network.mode` is `sta` or `ap`. `ssid` is the network the panel is on or
offering, `sta_ssid` the one it is configured for — which exists even while the
setup access point is up. `last_error` is why the station is not connected, in
the same vocabulary the screen prints: `bad_password`, `not_found`, `no_ip`,
`auth_timeout`, `gateway_unreachable`, `address_in_use`.

`ipv4.mode` is where the current address came from and is reported rather than
echoed: after a static configuration fails and the panel falls back, this says
`dhcp`, because that is what the address on the screen is. `ipv4.static` is the
stored configuration and what became of it — `state` is `pending`, `confirmed`,
`gateway_unreachable` or `address_in_use` — so a setup page can put a failed
address back in the form with the reason above it.

### `GET`, `PUT` and validate `/config`

`GET` returns the raw active document, which is the transient preview while one
is active and the persisted document otherwise, or `404 not_found` when neither
exists.

`PUT` returns `204` once the document has been validated, the screen and every
provider subscription have been swapped atomically, and the result has been
persisted. `?transient=1` requires edit mode and rebuilds in RAM only, which is
what live preview uses so that a drag session does not wear the flash; leaving
edit mode discards it. Refusals: `invalid_config` (400, with the detail
documented in [`CONFIGURATION.md`](CONFIGURATION.md#validation-errors)),
`edit_mode_required` (409), `invalid_query` (400), `apply_failed` and
`store_failed` (500).

`POST /config/validate` answers `204` or the same `invalid_config` document, and
never changes anything.

### Providers and `GET /resources`

Provider state arrives as part of `GET /status`, and as `status` frames on the
WebSocket:

```json
{"providers": [
  {"id": "direct", "status": "degraded", "resource_count": 2},
  {"id": "ha", "status": "unconfigured", "resource_count": 0}
]}
```

`status` is `unconfigured`, `connecting`, `online`, `degraded`, `offline` or
`error`. `degraded` means a provider can still serve part of its contract
without staling its resources — the direct provider is `degraded` while it can
accept state but has no WebSocket consumer attached to answer actions.

> `DESIGN.md` §4.1 also specifies a standalone `GET /providers` carrying the
> same entries without the device health around them. This firmware does not
> serve it: nothing needs it, because `/status` and the WebSocket already carry
> the list. Asking for it returns `404 not_found`.

`GET /resources?provider=<id>` returns `{"resources": [ … ]}` in the normalized
vocabulary below. It is the bounded runtime store: Home Assistant contains only
entities referenced by the active dashboard, and `direct` contains only bound
resources published since boot. The editor obtains the full HA picker catalog
through `/ha/catalog`, assembles it in browser memory once, shares the cache
between tile pickers and refreshes it in the background. Missing, unknown and
unconfigured providers answer `400 provider_required`, `404 provider_not_found`
and `409 provider_unconfigured`.

### Normalized resources

Every provider produces the same shape, and this is the whole of what a
component sees:

```json
{
  "provider": "direct",
  "resource": "living-room",
  "kind": "light",
  "name": "Living room",
  "area": "Downstairs",
  "available": true,
  "state": {"power": "on", "brightness": 62},
  "capabilities": {
    "toggle": true,
    "set_power": true,
    "set_brightness": {"min": 0, "max": 100}
  }
}
```

`provider`, `resource`, `kind`, `available` and `state` are required; `name`,
`area` and `capabilities` are optional, and absent capabilities mean read-only.
A snapshot always replaces the previous one in full.

`kind` is semantic rather than a native domain name:

| `kind` | `state` | Actions when advertised |
|--------|---------|-------------------------|
| `light` | `power` is `on`/`off`; optional `brightness`, `color_temperature` | `toggle`, `set_power`, `set_brightness`, `set_color_temperature` |
| `cover` | position and movement | `toggle`, `open`, `stop`, `close` |
| `sensor` | `value`, optional `unit`, `measurement` and `category` | none |
| `scene` | stateless | `activate` |

A sensor's two optional descriptions answer different questions.
`measurement` — `temperature`, `humidity`, `pressure` or `power` — says what
magnitude the number is and decides how the tile formats it. `category` says
what the reading is about and decides its icon: `temperature`, `humidity`,
`pressure`, `power`, `illuminance`, `air_quality`, `gas`, `sound`, `speed`,
`battery`, `connectivity`, `door`, `window`, `garage`, `motion`, `occupancy`,
`moisture`, `smoke`, `lock`, `plug`, `problem` or `running`. A door contact has
the second and not the first. Either may be omitted, and a name this firmware
does not know is ignored rather than refused.

The four measurements are also categories and imply them, so a snapshot that
states only `measurement` is read back from `GET /resources` with the matching
`category` filled in. State `category` explicitly to say something
`measurement` cannot — a battery percentage, a leak detector, a door.

An unavailable resource keeps its last values and renders stale. A provider
going `offline` stales only its own resources: an unreachable Home Assistant
must not dim tiles fed by a script.

### `GET /status`

```json
{
  "network": {"mode": "sta", "ssid": "home", "ip": "192.168.1.42",
              "sta_ssid": "home", "ipv4": {"mode": "dhcp"}, "last_error": null},
  "providers": [{"id": "direct", "status": "degraded", "resource_count": 0},
                {"id": "ha", "status": "unconfigured", "resource_count": 0}],
  "rssi": -54, "uptime_s": 120, "heap_free": 294631,
  "lvgl_heap_free": null, "lvgl_heap_total": null, "lvgl_frag_pct": null,
  "reset_reason": "power_on", "reboot_count": 3,
  "resource_count": 0, "storage_reset": false
}
```

The three LVGL figures are `null` before display bring-up rather than zero,
which would look like an exhausted allocator. `storage_reset` says boot recovery
erased corrupt NVS or reformatted the filesystem, which is a different thing
from a panel that was simply never configured.

### Home Assistant

```json
{"url": "http://homeassistant.local:8123", "token": "<long-lived access token>"}
```

`204` only after the credentials have been tested against the instance and
persisted atomically; existing working credentials survive every failed request.
Refusals are `400` (`url_required`, `token_required`, `url_too_long`,
`token_too_long`, `bad_url`), `422 ha_auth_invalid`, `502 ha_unreachable`,
`503 ha_busy` and `500` (`store_failed`, `reload_failed`). Any HTTP redirect on
the WebSocket URL is refused before the token is sent — the URL you configured
is the credential boundary, not the first hop toward one.

`GET /ha/discover` returns what the local network advertises as
`{"instances": [{"name": …, "uuid": …, "url": …}]}`. An empty result is normal:
multicast DNS does not always cross a VLAN, and manual entry remains available.

`GET /ha` returns `{"configured":true,"url":"http://…"}` or an unconfigured
response with a null URL. It never returns the long-lived token. `DELETE /ha`
answers `204`, removes that token and clears HA runtime state.

The editor fetches the full configuration catalog in four stages: `entities`,
`devices`, `areas`, then `states`. `POST /ha/catalog` with `{"stage":"entities"}`
returns a request id. `GET /ha/catalog?request=<id>` returns
`{"status":"pending"}` until it can return the corresponding HA WebSocket
result. Only one small response is resident on the ESP32 at a time; the browser
joins the stages and caches the normalized catalog. Normal panel operation does
not use this route and subscribes only to configured entity ids.

### External API keys

`GET /integration-keys` returns only non-secret metadata:
`{"keys":[{"id":"…","name":"Node-RED"}]}`. `POST /integration-keys` with
`{"name":"Node-RED"}` creates one and returns `201` with `id`, `name` and
`token`. That plaintext is shown once; NVS stores only its SHA-256 digest. Up to
four keys may exist. `DELETE /integration-keys?id=<id>` revokes one, answers
`204`, and immediately closes an active WebSocket authenticated with it.

### `POST /wifi`

```json
{"ssid": "home", "password": "…", "admin_pin": "1234",
 "setup_password": "optional WPA2 passphrase", "ipv4": {"mode": "dhcp"}}

{"ssid": "home", "password": "…",
 "ipv4": {"mode": "static", "address": "192.168.1.42/24",
          "gateway": "192.168.1.1", "dns": ["192.168.1.1"]}}
```

Answers `202`, and **the outcome appears on the panel, not in the response.**
There is one radio: at the instant the station associates, the setup access
point moves to the router's channel and drops the browser that submitted the
form. Polling for a result from that browser is not something that can be made
to work, so the screen reports success with the address-only QR, and
failure with a named reason.

An absent or `null` password keeps the stored one, but only when the SSID is
unchanged; that is what lets a setup page correct a failed static address
without handling the secret. An explicit empty string means an open network and
erases what was stored. An absent `ipv4` means `dhcp`, and an explicit `dhcp` is
a request to stop using a stored static address rather than merely a request not
to set one.

A static address is on trial when it is submitted: the gateway is proved over
ARP, and no confirmation within about fifteen seconds reverts to DHCP, keeps the
configuration stored and marked failed, and prints the reason. Once confirmed it
is sticky.

Refusals are `400` unless noted: `ssid_required`, `ssid_too_long`,
`password_too_long`, `truncated`, `bad_ipv4`, `bad_ipv4_mode`, `bad_address`,
`bad_gateway`, `bad_dns`, `store_failed` (500).

`GET /wifi/scan` serves a cache and says how old it is, because scanning makes
the access point unresponsive while it runs. `age_s` is `null` when no sweep has
ever been taken, which is not the same as a room with no networks in it.
`?rescan=1` sweeps first.

```json
{"networks": [{"ssid": "home", "rssi": -54, "channel": 6, "auth": "wpa2"}], "age_s": 12}
```

### Device controls

`POST /mode` takes `{"mode": "normal"|"edit"}`; edit mode falls back to normal
after 60 s without a client. `POST /identify` and `POST /factory_reset` take no
body and refuse one with `unexpected_body`. All three answer `204`. A factory
reset erases NVS and LittleFS, rotates authentication state, answers, and reboots.

### `POST /ota/upload`

The body is a raw `.bin`, and curl needs `-H 'Expect:'` to send one: it adds
`Expect: 100-continue` to a body this size, the panel's HTTP server never
answers that, and curl then waits out its own timeout before sending anything.
On success:

```json
{"partition": "ota_1", "bytes": 927040, "version": "1.0.0"}
```

The `200` arrives before the reboot, because afterwards there is nobody left to
answer. `version` is read out of the image that was just written, not the one
still running — "did the file I meant to send arrive" is the question a flash
asks. The new image gets one boot to answer `GET /info`; if it does not, the
bootloader returns to the previous slot on its own.

Refusals: `not_an_image`, `invalid_image`, `truncated`, `pending_verify`,
`no_ota_partition`, `empty_body`, `too_large`, `out_of_memory`. None of them
changes what the panel boots.

`tools/ota/upload.sh` is this endpoint with the checks worth having around it.
This is a development mechanism: no manifest, no versioning, no HTTPS. The
release channel below is the other one.

### Firmware updates

```json
{
  "current": "1.0.0",
  "manifest_url": "https://mateuszsikora.github.io/slate/ota/manifest.json",
  "scheduled": true,
  "state": "idle",
  "checked_s_ago": 3600,
  "available": {"version": "1.1.0", "url": "https://…/firmware/1.1.0/slate.bin",
                "sha256": "…"},
  "error": null,
  "progress": null
}
```

`GET /update` is the whole channel as the panel sees it. `state` is `idle`,
`checking`, `downloading` or `installed`; `progress` is `{"received": …,
"total": …}` while an image is arriving and `null` otherwise. `available` is an
**offer**, not a staged image: the panel has read a few hundred bytes of
manifest and compared them with itself. `error` is the last failure in the
vocabulary below, and survives beside an offer that is still valid — a check
that could not reach the server did not make the release stop existing.

Two fields say whether the panel looks on its own, and they are not the same
question. `manifest_url` is `null` on a build with no channel, which is what
`-DSLATE_UPDATE_MANIFEST_URL='""'` produces; that panel has nowhere to look and
no setting can give it one. `scheduled` is `false` on a panel that has a channel
and was told to stop using it, through `POST /update/settings` below — the daily
request is gone, everything else about the channel is still there. A build with
no channel reports `scheduled` as `false` too, because it is the answer to "does
this panel check on its own", and `manifest_url` is what tells the two apart.

The manifest is [`DESIGN.md`](DESIGN.md) §11.4's five fields, published by the
release workflow beside the browser installer:

```json
{"version": "1.1.0", "board": "waveshare-s3-touch-7",
 "url": "https://…/firmware/1.1.0/slate.bin", "sha256": "…", "min_schema": 1}
```

A release is offered only when `board` is this panel's model, `min_schema` is no
higher than its `schema_max`, `url` is `https://`, and `version` is newer than
the running one. A running version that is not `MAJOR.MINOR.PATCH[-suffix]` is a
development build, and any release is newer than one of those.

`POST /update/check` and `POST /update/install` both answer `202 Accepted` with
`{}` and do the work afterwards — the panel serves HTTP from one task, and a TLS
fetch on it would stop the rest of the API for seconds. Poll `GET /update` for
the outcome. `check` takes no body. `install` takes `{"version": "1.1.0"}`,
which must be the version the panel is currently offering, and an absent body
installs whatever that is. What the check buys is narrow and worth stating
exactly: a daily check landing between the moment a client read the offer and
the moment somebody pressed the button cannot silently redirect the install to a
different version. It is not a promise that the accepted release still exists —
the panel installs from the URL it cached at its last check, and a channel that
has since moved on answers `release_gone` (see below).

The download is verified before it can boot. The image's header must be an
ESP32-S3 application image, its own version must be the one the manifest
promised, and its SHA-256 must be the manifest's — checked over the stream, so
the boot partition is moved only for an image that passed. A failure here costs
the *other* firmware slot and never the running one, exactly as
`POST /ota/upload` does. The new image then gets one boot to answer `GET /info`
before the bootloader takes it back.

`POST /update/settings` takes `{"scheduled": false}` and answers `204`. It is a
device setting in NVS, not part of the configuration document — §10 makes that
file something people export, import and share between panels, and whether *this*
wall talks to the internet is not a property that should travel with a dashboard.
It survives a reboot and does not survive a factory reset, which erases NVS along
with the credentials and the PIN; the default is on.

What it switches off is the schedule and only the schedule. `POST /update/check`
works exactly as before while it is off — an explicit check is a request, not a
schedule — and so does `POST /update/install`, on an offer the panel is still
holding. Turning it back on resumes the daily check without a reboot, and checks
straight away if a day has passed meanwhile. A build with no channel refuses this
route `503 no_channel`, like the other two `POST`s: there is no schedule to have
an opinion about.

Refusals from the three `POST`s: `409` (`busy`, `no_update`, `version_mismatch`),
`503 no_channel`, `500 store_failed`, and the usual body errors
(`unexpected_body`, `empty_body`, `invalid_json`, `invalid_version`,
`invalid_scheduled`, `too_large`, `truncated`). Failures reported in `error`:
`offline`, `unreachable`, `no_release`, `release_gone`, `manifest_invalid`,
`board_mismatch`, `schema_too_new`, `insecure_url`, `download_failed`,
`checksum_mismatch`, `not_an_image`, `version_mismatch`, `invalid_image`,
`too_large`, `no_ota_partition`, `pending_verify`, `ota_failed`,
`out_of_memory`.

Three of those are about the channel and are worth telling apart, because they
look alike from a browser and are not alike at all. `unreachable` is a host that
did not answer. `no_release` is a manifest that is not there — a channel with
nothing published on it. A build re-pointed at a channel of its own with
`-DSLATE_UPDATE_MANIFEST_URL`, and a deployment that has not landed yet, both
look like this, and neither is a fault. `release_gone` is the image URL of a
cached offer answering `404`: the deployment carries one release at a time, so
an offer this panel read before the newest tag names a file that is no longer
published. The panel treats that as its own cue to look again, so the
current release is normally on screen by the time somebody reads the message.

Nothing on this path is automatic except the daily check, the check downloads
nothing, and the daily check is the one thing here that can be switched off.
Installation is always a request, and always ends in a restart the person who
asked for it chose the moment of.

### `GET /coredump`

The only route whose success is not JSON: `200 application/octet-stream` with
the dump exactly as the panic handler wrote it, which is what
`esp-coredump --core-format raw` and `idf.py coredump-info` both read.
`tools/coredump/fetch.sh` is the host side. Refusals are `404 no_coredump` — a
healthy panel — and `500 corrupt_coredump`, `coredump_read_failed`,
`out_of_memory`. A transfer that stops part-way has no error document, because
the `200` has already gone; a short body is always a failed transfer and never a
short dump.

## WebSocket

```
ws://<address>/api/v1/ws
```

The upgrade carries no credential — a token in the query string would land in
access logs, and requiring a header would exclude the browser WebSocket API. The
client authenticates with its first frame, within five seconds:

```json
{"type": "auth", "token": "<32-character session or External API credential>"}
```

With a device-session credential, `{"type":"auth_ok"}` is followed by the
retained log backlog and a current `status` frame. An External API key receives
only `auth_ok` and direct-provider `action` frames after it attaches; it cannot
receive logs, status or editor events. A bad credential, a malformed first frame
or silence for five seconds gets `{"type":"auth_invalid"}` and a 1008 close.
Nothing else is accepted before `auth_ok`.

Device → client:

```json
{"type": "status",   "providers": {"direct": "online", "ha": "unconfigured"},
                     "wifi": -54, "heap_free": 142000,
                     "lvgl_heap_free": 2088632, "lvgl_frag_pct": 1}
{"type": "log",      "level": "warn", "msg": "resource direct:living-room unavailable"}
{"type": "reloaded", "schema": 1, "tiles": 7}
{"type": "action",   "id": 42, "provider": "direct", "resource": "living-room",
                     "action": "toggle", "params": {}}
```

Client → device:

```json
{"type": "ping"}
{"type": "mode", "mode": "edit"}
{"type": "provider_attach", "provider": "direct"}
{"type": "action_result", "id": 42, "success": true}
```

`status` is the device's 15-second heartbeat. The JSON `ping` above is the
client's; WebSocket control frames stay a transport mechanism with no
application meaning. Sixty seconds without a client ping leaves edit mode, and
does not disconnect a diagnostics client that is otherwise healthy.

## The direct provider

`direct` is how anything that is not Home Assistant drives the same components:
a shell script, a Node-RED flow, a test fixture. It is always present, uses a
named External API key created by an administrator, and is the reference adapter for the
provider contract — there is no broker, callback URL or discovery protocol in
it, deliberately.

A round trip has three parts.

**1. Bind something to it.** The store holds only resources the active
dashboard references, so publish before binding and there is nowhere to put it:

```json
{"id": "t1", "type": "light", "pos": [0, 0], "size": [2, 1],
 "binding": {"provider": "direct", "resource": "living-room"}}
```

**2. Publish state over HTTP.** The body is a normalized snapshot without
`provider`, which the endpoint fixes to `direct` — a body that could name its
own provider is a body that could write into the Home Assistant half of the
store:

```bash
curl -sS -X POST http://192.168.1.42/api/v1/direct/state \
     -H "Authorization: Bearer $SLATE_API_KEY" \
     -H 'Content-Type: application/json' \
     -d '{"resource":"living-room","kind":"light","name":"Living room",
          "available":true,"state":{"power":"on","brightness":62},
          "capabilities":{"toggle":true,"set_brightness":{"min":0,"max":100}}}'
```

`202 {"resource":"living-room"}` means accepted and enqueued. The refusals are
worth reading rather than retrying: `404 resource_not_bound` (nothing binds that
id, which is also the bound that stops a LAN client filling PSRAM),
`409 kind_mismatch` (the tile expects a different kind), `400 invalid_state`.
None of them disturbs the last confirmed value.

`tools/direct/publish.sh` is this request with the key kept out of the process
list.

**3. Answer actions over the WebSocket.** One authenticated client attaches:

```json
{"type": "provider_attach", "provider": "direct"}
```

A second attachment gets `{"type":"error","error":"provider_busy"}`, because two
processes must not both operate the same light; re-attaching from the client
that already holds it is not a second process and is not an error. There is no
acknowledgement frame — attaching moves the provider from `degraded` to
`online`, and the `status` frame that follows carries exactly the fact the
client was asking about.

From then on a tap arrives as an `action` frame and is answered with
`action_result`. **Answering is not confirming.** `success: true` acknowledges
delivery; what clears the tile's pending state is the next published snapshot
showing the new value. A consumer that answers and never publishes leaves every
tap reverting after three seconds. `success: false` reverts immediately, which
is the better failure — a detached consumer fails actions at once rather than
making the panel wait out the timeout.

`tools/direct/agent.py` is a complete consumer in the standard library alone:
attach, receive, apply, answer, publish. Run it against a panel with one bound
light and every tap on that tile round-trips through it.

## What is deliberately absent

- **No HTTPS.** A self-signed certificate on an ESP32 is a worse experience than
  its absence on a local network, and the threat model here is a LAN.
- **No accounts, no cloud, no telemetry.** The browser talks to the device.
- **No general API rate limiting or origin allow-list** until a concrete
  scenario needs one. PIN attempts are rate-limited separately.
- **Upstream credentials only go in.** The Home Assistant token and WiFi
  passphrase are never returned — and the passphrase never enters the
  configuration document either. The two intentional credential responses are
  narrow: `/session` returns the internal credential to the current page, and a
  newly created External API key is returned once.
