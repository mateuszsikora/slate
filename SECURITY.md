# Security

Slate is firmware for a touch panel on a home network. It talks to a browser and
to a home automation system on the same LAN, and to two things outside it: an
HTTPS manifest, once a day, to learn whether a newer release exists, and — only
while the Tuya provider is configured — signed requests to the Tuya cloud that
provider's devices are driven through. There is no broker and no inbound path
from the internet in the design.

[§Security in the README](README.md#security) is the threat model, and it is
written as a list of trades rather than as a list of features. Read it before
reporting anything. What follows here is the part it does not say: which of those
trades are settled, what a report outside them looks like, and where one goes.

## The documented trades are not vulnerabilities

Each of these is deliberate, is argued in the README, and will not change because
it was reported. Reporting one costs you the write-up and costs the project the
triage, so this list exists to save both.

- **The device serves plain HTTP.** Session credentials and API keys travel in
  the clear between a browser and the panel, and anyone on the network can read
  them. A self-signed certificate on an ESP32 buys a browser warning on every
  visit and a private key sitting in flash that a cable can read. The mitigation
  is the network the panel is on, and the README says so: do not put it on a
  network you would not put a printer on, and do not expose it to the internet.
- **The setup access point is open by default and its setup page needs no
  existing administrator session.** Someone in radio range of a panel can move it
  to another network and choose its next PIN. That is the recovery story for a
  device with no keyboard, and the authority it grants stops there: provider
  credentials, the configuration, direct-provider publication, core dumps and
  firmware upload answer `401` on that interface exactly as they do on any other.
- **The core dump endpoint returns device memory.** It is token-gated with no
  setup-access-point exception, the build keeps DRAM out of dumps, and the code
  paths that hold a credential on a stack zero it. A crash log that is useless
  is not worth shipping, so the endpoint exists and is treated as the exception
  it is.
- **The development OTA endpoint takes an unsigned image over plain HTTP.** It is
  a desk-to-wall mechanism behind the device token, on a LAN, and it has no
  manifest and no signature because it is not the update channel. The channel a
  panel updates itself from fetches its manifest over HTTPS and verifies the
  SHA-256 the manifest names before the image is made bootable.
- **There is no general API rate limit and no origin allow-list.** PIN attempts
  are rate-limited; nothing else is. Either would be added for a concrete
  scenario, and "an attacker already on your LAN can send many requests" is not
  one, because that attacker has the panel either way.
- **A Home Assistant token carries the authority of its account.** Slate stores
  it and never returns it, but it cannot narrow it. The README asks for a
  dedicated `system-users` account for exactly this reason.
- **The Tuya provider is a cloud dependency, and it is optional.** While it is
  configured the panel sends signed HTTPS requests to the Tuya OpenAPI every
  five seconds. The region, Access ID, Access Secret and account UID live in
  NVS and are validated before they replace working ones. Authenticated reads
  return only configuration state, region, and UID; the Access ID and Access
  Secret are never returned. Browser-to-panel setup still uses Slate's plain
  LAN HTTP API, while panel-to-Tuya traffic uses verified HTTPS. A Tuya outage or an expired cloud plan
  stalls only that provider's tiles and reports its status as an error; it
  opens no inbound path, exposes nothing the Tuya cloud does not already hold,
  and a panel that never configures Tuya never sends it a byte.

The short form: an attacker with a foothold on the LAN, or with a radio in range
of a panel in setup mode, is *outside* this threat model. Slate does not claim to
resist them, and a demonstration that it does not resist them is not a finding.

## What is worth reporting

Anything that gets more than the trades above hand out. Concretely:

- Reaching a token-gated endpoint without a valid credential — reading or
  replacing the configuration, uploading firmware, fetching a core dump,
  triggering a factory reset.
- An External API key doing something [its authority does not
  cover](README.md#security): reading a dashboard, configuring Home Assistant,
  fetching logs, uploading firmware.
- A stored credential coming back out — the Home Assistant token, the Tuya
  cloud credentials or the WiFi passphrase appearing in an API response, an
  exported configuration, the logs or a core dump.
- Memory corruption reachable from a request, authenticated or not: the panel is
  a C firmware and a heap overflow on it is a real finding even when the request
  that causes it needs a token.
- The update path accepting an image it should have refused — a checksum that
  does not match the manifest, a downgrade the version check should have caught,
  a manifest fetched over something other than HTTPS.
- Recovering the administrator PIN from anything the device hands out, or getting
  past its rate limiting.
- Anything that works from *outside* the LAN. Nothing in this design opens an
  inbound path; if you have found one, that is the report this file exists for.

If you are not sure which side of the line something falls on, report it
privately. Guessing wrong in that direction costs nothing.

## Where a report goes

Use GitHub's private vulnerability reporting on this repository — the **Security**
tab, **Report a vulnerability** — which opens a private advisory draft visible
only to you and the maintainer:

<https://github.com/mateuszsikora/slate/security/advisories/new>

If that form is not available, the feature has not been enabled on the repository
yet; say so in a public issue *without the details* and it will be turned on.

Please do not open a public issue for a suspected vulnerability. Panels run on
home networks and are updated by hand by the people who own them, so the gap
between a public report and an installed fix is measured in however long it takes
those people to notice.

Useful in a report: what the panel was doing, the request or the steps, what came
back, and the firmware version — the editor's **Device** panel shows it, and so
does `curl http://<panel>/api/v1/info`. A proof of concept against your own panel
is worth more than a description of one.

## What you can expect back

This is a personal project published as open source, with no commercial roadmap
and no support commitment — the README says as much on the first screen. There is
no SLA here, no bounty and no security team: there is one maintainer who will
read your report and answer it.

What is committed to is the shape rather than the timing. A report gets an
acknowledgement and a verdict — a fix, or a reason it belongs on the list of
trades above. A fix lands in a release, and the advisory says which version
carries it. You are credited by whatever name you ask for, or not at all if you
prefer.

## Which versions get fixes

The current release, and only it. There is no long-term branch, no backport
policy, and updates are cheap: the panel offers a release and installs it when
you say so, and the bootloader returns to the previous image if the new one does
not come back.

Until the first `v*` tag exists there are no releases at all, and the version
that gets fixed is `main`.
