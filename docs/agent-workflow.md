# Agent workflow

Instructions for a coding agent picking up work on Slate. Follow them in order.

Everything you write that lands on GitHub — issue comments, branch names, commit messages, PR titles and bodies — is in **English**.

## 1. Read before doing anything

Read every Markdown file in the repository before touching code:

```bash
find . -name '*.md' -not -path './node_modules/*' -not -path './.git/*'
```

At minimum: `README.md`, `docs/DESIGN.md` (the full design — architecture decisions, config format, device API, milestones), and this file. `docs/DESIGN.md` is the source of truth; issues reference its sections. If an issue and the design document disagree, the design document wins — say so in the issue rather than silently following the issue.

## 2. Pick an issue

Several agents may be working the backlog at the same time. The `in-progress` label is what keeps two of them off the same issue, so read it before picking and set it as soon as you have picked.

```bash
gh issue list --state open --milestone M0 --search "-label:in-progress"
```

Rules:

- Work the **lowest open milestone first**. M0 spikes gate the partition table and the runtime architecture; M1 gates everything that follows. Do not start M3 work while an M1 issue it depends on is open.
- **Never pick an issue labelled `in-progress`** — another agent has already claimed it. Same for issues already assigned or with an open linked PR.
- One issue per PR. If an issue turns out to contain two independent pieces of work, say so and propose splitting it rather than shipping a double-sized PR.
- Claim the issue before doing anything else: add the `in-progress` label, assign yourself, and comment that you are starting.

  ```bash
  gh issue edit <issue-number> --add-label in-progress
  ```

  Re-check the issue right after claiming it (`gh issue view <issue-number> --comments`). If someone else labelled or assigned it in the meantime, drop your claim (`gh issue edit <issue-number> --remove-label in-progress`) and pick another issue.

- Remove the label if you stop working on the issue without opening a PR — when you are blocked on a question, hand the work back so it does not sit claimed by nobody. Say so in the comment you leave. Once the PR is merged the issue closes and the label goes with it.

## 3. Read the whole issue

The issue body is only half of it. Decisions get made in the comments — a pin gets fixed, a scope gets cut, a question gets answered — and the body is rarely edited to match. Read everything before you plan:

```bash
gh issue view <issue-number> --comments
```

- Read **every comment, oldest to newest**, including ones left by other agents on an earlier claim of the same issue.
- Where comments contradict the body, the **latest comment from the maintainer wins**. Say in your plan which part of the body you are overriding and why.
- Follow the trail: referenced issues (`#12`), linked PRs, and the `docs/DESIGN.md` sections the issue names. A closed linked PR usually explains why a first attempt was abandoned.
- If the issue was previously claimed and handed back, find the question that blocked it and check whether it has been answered. If it has not, do not re-claim the issue.
- Note any comment that answers a question you would otherwise have had to ask — quote it in your plan so a reviewer can see where the decision came from.

## 4. Analyse before implementing

Write a short plan as a comment on the issue:

- Which DESIGN.md sections govern this work.
- Which files you will add or change.
- How you will verify the "Done when" criteria — concretely, on hardware where the issue implies hardware.
- Anything in the issue that is ambiguous or that you believe is wrong.

**Ask questions when you need them.** Post them as an issue comment, remove the `in-progress` label so another agent can pick the issue up once it is answered, and stop. Do not guess on:

- pin assignments, partition sizes, or anything that forces a serial reflash to change later;
- the shape of a public API response — `docs/DESIGN.md` §4 is a contract, and changing it later breaks clients;
- anything requiring physical hardware access or credentials you do not have.

Proceed without asking when the design document already answers the question.

## 5. Implement

- Branch from `main`: `git checkout -b <type>/<issue-number>-<short-slug>`, e.g. `feat/12-dev-ota-upload`.
- Match the surrounding code: same naming, same error handling, same comment density. Where there is no surrounding code yet, follow ESP-IDF conventions for firmware and standard React/TypeScript conventions for the editor.
- Respect the architecture decisions in §1 of the design document. In particular: no layout is hardcoded (ADR-1), components are semantic not primitive (ADR-2), the browser talks only to the device (ADR-3), the device API is a public contract (ADR-4), there is no second renderer (ADR-5), assets are compiled in (ADR-6).
- Keep the diff scoped to the issue. Unrelated cleanups belong in a separate issue.
- Do not commit secrets. Tokens live in NVS on the device, never in the repository.

## 6. Verify

Do not open a PR on unverified work.

- Build cleanly: `idf.py build` for firmware, the project build script for the editor.
- Exercise the change the way the issue's "Done when" describes it. For firmware that means flashing (over `curl` once development OTA exists) and observing real behaviour on the panel — not just a successful compile.
- If you cannot verify because the hardware is not reachable, say so explicitly in the PR body. Do not describe unverified work as working.

## 7. Commit

**Commits must be signed.** Signing is preconfigured (`commit.gpgsign=true`, GPG smartcard); the key may need a physical touch. Verify before pushing:

```bash
git log --show-signature -1
```

A commit that shows no signature does not get pushed. If signing fails, stop and report it rather than committing unsigned with `--no-gpg-sign`.

Message format:

```
<type>: <imperative summary under 72 chars>

<why the change is needed and what it does, wrapped at 72 columns>

Refs #<issue-number>
```

Types: `feat`, `fix`, `docs`, `refactor`, `test`, `chore`, `spike`.

## 8. Open a pull request

```bash
gh pr create --base main --title "<type>: <summary>" --body "..."
```

The body contains:

- `Closes #<issue-number>`.
- What changed and why, in prose.
- **How this was verified** — the actual commands run and what was observed. If the panel was involved, say what appeared on the screen.
- Anything left open, any deviation from the design document, and any decision a reviewer should sanity-check.
- For a spike issue (M0), the written answer belongs in `docs/spikes/s<n>.md` in the same PR, with the measured numbers. A spike PR without numbers is not done.

Then report back: the PR URL, a one-paragraph summary, and any question still blocking.

## What not to do

- Do not close an issue without a merged PR.
- Do not start implementing from the issue body alone — the comments are part of the issue.
- Do not start work on an issue labelled `in-progress`, and do not leave the label on an issue you have abandoned.
- Do not push to `main`.
- Do not widen scope mid-PR — open a follow-up issue instead.
- Do not mark hardware behaviour as verified from a successful build alone.
