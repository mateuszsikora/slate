# Contributing

Slate is a personal project published as open source. There is no commercial
roadmap and no support commitment, and hardware support is limited to one board
— the README says that on its first screen and this file does not soften it.
What it means for you is worth stating plainly rather than leaving you to infer
it: contributions are welcome, review is best-effort, and a change that needs a
panel to prove it may wait until there is one in front of the maintainer.

Everything below assumes you have one fix or one small feature. If you are a
coding agent working the milestone backlog, the document you want is
[`docs/agent-workflow.md`](docs/agent-workflow.md) — issue claiming, the
`in-progress` label, milestone order. It is an appendix to this file rather than
a replacement for it, and it is stricter in two places: it requires signed
commits and it refuses to open a pull request on unverified work. Both describe
an agent working this backlog on a machine with the signing key configured and a
panel on the desk. For a contributed pull request, this file is the one that
governs — see the two paragraphs below on hardware you do not have and on
signing.

## Before you write code

**A question is not a pull request.** "Will this work on the 4.3 inch board",
"how do I bind a template sensor", "the panel raised its access point again
after I changed routers" — open an issue for those, or read
[`docs/CONFIGURATION.md`](docs/CONFIGURATION.md) and
[`docs/API.md`](docs/API.md), which answer more of them than their length
suggests. A **suspected vulnerability goes nowhere near a public issue**:
[`SECURITY.md`](SECURITY.md) has the private channel, and also the list of
documented trades that look like findings and are not.

**A bug fix needs no permission.** Send it. A new component, a new provider, a
new endpoint or anything that changes the shape of the configuration document is
worth an issue first, because those are governed by
[`docs/DESIGN.md`](docs/DESIGN.md) and the answer may be that the design already
decided against it. That is a cheaper conversation before the code than after.

Two things constrain what can land, and neither is a matter of taste:

- **The architecture decisions in [§1 of the design
  document](docs/DESIGN.md#1-architecture-decisions)** — no layout is hardcoded,
  components are semantic rather than primitive, the browser talks only to the
  device, the device API is a public contract, there is no second renderer,
  assets are compiled in. A change that contradicts one of these is not a bad
  patch, but it is a design conversation, and it happens in an issue.
- **One board.** The Waveshare ESP32-S3-Touch-LCD-7 with the
  ESP32-S3-WROOM-1-N16R8 module. Support for a second display is not a small
  patch here and there is nothing to verify it on.

Where an issue and the design document disagree, the design document wins. Say
so in the issue rather than silently following the issue.

## Build it before you change it

The full instructions are in [README §Building from
source](README.md#building-from-source). The part that catches everybody is
that **a clean checkout cannot build the firmware**: the LVGL fonts and the
editor bundle are generated rather than committed — nobody can review a
megabyte of generated C or a minified bundle in a diff — and the firmware build
fails without them in a way that does not explain itself. In order:

```bash
tools/fonts/generate.sh          # → firmware/components/slate_theme/assets/
tools/editor/build.sh            # → editor/dist/index.html
cd firmware && idf.py build      # → firmware/build/slate.bin
```

You need **ESP-IDF v5.5.5** — the version CI pins and every flash-size number
in the design document was measured on — and **Node.js 24**. If a configuration
change appears to have no effect, delete `firmware/sdkconfig` and build again:
it is generated and gitignored, `firmware/sdkconfig.defaults` is the file under
version control, and ESP-IDF reads the defaults only when no `sdkconfig` exists.

Working on the editor needs no ESP-IDF. It does need a panel to talk to, because
ADR-5 leaves nothing else to develop against: `SLATE_DEVICE=192.168.1.42 npm run
dev` in `editor/` serves the editor from your machine and proxies `/api` to that
address. What CI runs is `tools/editor/build.sh` and then `npm test` in
`editor/`, and neither needs a panel — `npm run check` is only the type-checking
half of the build, so a change that type-checks but breaks the bundle or pushes
it past the 400 KB gzipped budget the design document sets for it (§10) passes
locally and fails there.

## Verifying, and what to do when you cannot

**A green CI check is not verification of firmware behaviour.** CI compiles both
image variants, builds the editor and assembles the flashing page; it proves the
image links, not that the panel does anything. `.github/workflows/ci.yml` says
this in its own header comment, and it is the single most common way a pull
request here arrives looking finished and is not.

So the bar depends on what you touched:

- **Documentation and the host-side tools in `tools/`** — no panel involved. Run
  it, say what you ran.
- **The editor** — verified in a browser. `tools/editor/build.sh` and `npm test`
  run anywhere, and for a change that is only shape or logic they are the whole
  story; anything that renders device data wants the dev proxy pointed at a
  panel, which puts that half under the paragraph below.
- **Firmware** — verified by flashing a panel and watching it. The first flash
  is over a cable; after that, `SLATE_TOKEN=<session credential>
  tools/ota/upload.sh <address>` posts your build over WiFi and waits for the
  panel to come back, so the edit-to-screen loop is seconds and does not
  involve the cable. Say what appeared on the screen.

**If you have no panel, say so in the pull request, in plain words.** This is
the common case and it is not a reason to withhold a change. An unverified
change described as unverified is reviewable, and the honest thing to tell you
is what happens next: it waits until the maintainer can flash it here, and that
is a queue of one person with one panel. A change described as working when
nobody ran it costs more than it saves, because the review that follows is
conducted on a false premise. Fill in the "How this was verified" section of the
pull request template with the truth, including when the truth is "I could not
test this on hardware."

## Commits and pull requests

**Everything that lands on GitHub is in English** — issue comments, branch
names, commit messages, pull request titles and bodies, and code comments. This
is a language rule for the artifacts, not for you; discuss the work in whatever
language suits.

Branch from `main` as `<type>/<issue-number>-<short-slug>`, e.g.
`fix/41-backlight-pwm`. Commit messages:

```
<type>: <imperative summary under 72 chars>

<why the change is needed and what it does, wrapped at 72 columns>

Refs #<issue-number>
```

`feat`, `fix`, `docs`, `refactor`, `test`, `chore`, `spike`. The summary says
what the change does for somebody using the panel, not which file moved — the
log above your commit is the house style, and it is worth a look. Commit signing
is not a merge requirement; the maintainer's own commits are signed because of a
local key setup, and nothing rejects an unsigned contribution.

One issue per pull request, and keep the diff scoped to it. An unrelated cleanup
you noticed on the way is a separate issue — mentioning it there is more useful
than folding it in, because a reviewer reading a two-subject diff cannot tell
which half a regression came from. Do not widen scope mid-review either; open a
follow-up.

Match the surrounding code: same naming, same error handling, same comment
density. Where there is none yet, ESP-IDF conventions for firmware and standard
React/TypeScript conventions for the editor.

The [pull request template](.github/PULL_REQUEST_TEMPLATE.md) asks for what
changed and why, how it was verified, and what is left open. "Nothing" is a fine
answer to the last one; a blank second one is not.

**Do not commit secrets.** Tokens live in NVS on the device and belong in no
file here. Nothing about a home network's SSID, addresses or Home Assistant
instance needs to appear in a diff either — redact them in logs you paste.

## Dependencies and licensing

Slate is MIT-licensed. By opening a pull request you offer your contribution
under [the same license](LICENSE).

A new dependency that ends up in a firmware image is also a licensing change: a
release hands `slate-<version>.bin` to somebody who will never clone this
repository, which is what [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)
exists for. If your change links a library, seeds one into the image or adds a
font, add its entry there and its license text under `LICENSES/`, in the same
pull request. Dependencies are pinned — `firmware/dependencies.lock`,
`editor/package-lock.json`, `flasher/package-lock.json` — and CI fails a build
that rewrites the firmware lock without committing it.

## Conduct

[`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md) is the Contributor Covenant, and it
applies to issues, pull requests and reviews here. It names two private channels
for a report, one of which is read by people who are not the maintainer and can
act when the maintainer is the subject of it.
