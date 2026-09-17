# Picking the reading up where the other device left it

A reader finishes a chapter on their Kobo, picks up the CrossPoint, and expects the book to be at
the same page. This is what happens in between.

The behaviour is not invented here. It is the same contract the KOReader plugin implements as
**C17**, read from its source rather than from a hand-off:

| | |
|---|---|
| Repo | `runadev-arg/highlights-kobo` (Cinabrio) |
| File | `highlightsdetoto.koplugin/resumeflow.lua` |
| Protocol | `docs/toto-sync-v2-protocol.md`, §6 Progress directives and §7 Suggestions |

CrossPoint keeps the same three decisions, in `lib/TotoSync/TotoResumeFlow.{h,cpp}` — no Arduino, no
i18n, no storage, so all of it is covered by `test/toto_resume_flow` on the host.

## Ask before sending

`planConnect()` puts the pull first and the drain second, and a pull that failed cancels the drain.

That ordering is the feature, not a style choice. A reader that was offline has queued progress
events of its own. If those go up first, the hub records this reader's stale position as the newest
write for the book, classifies the other device's position as `keep_local`, and the question is
never asked — the reader simply loses the page they were on. `SyncScheduler` sets
`askBeforeSending` the moment Wi-Fi comes back and clears it only on a pull that came back `200`.

`SyncClient::syncOnce(pullOnly)` is what makes the first request an `/api/sync/v2/pull` even when
the outbox is not empty.

**With a book open, only that first question runs.** The reconnection's pull is one bounded
request; the queue has no deadline and waits until the book is closed. Without this the reader
would have to close the book before the reader could even be told their Kobo had moved.

## One question, and only when it means something

`shouldOffer()` returns a reason rather than a bare `false`, and the reasons are part of the
contract — the tests and the log use the same names:

| Reason | Why nothing is shown |
|---|---|
| `no_book` | Nothing open to compare against |
| `other_book` | The book changed between the event arriving and the question being asked |
| `other_account` | The suggestion was issued to the account that signed out |
| `no_locator` | No anchor, page or percentage: there is nowhere to go |
| `already_visible` | A question is already on screen |
| `same_position` | The two positions differ by ≤ 0.5 points |
| `already_resolved` | This position, or one that has not moved from it, was already answered |

`suggest_resume` and `suggest_jump` are the only directives that reach this path. `apply_initial`
and `merge_forward` are applied without asking, as the protocol says, and `own`, `equivalent` and
`keep_local` move nothing.

## What the question shows

`describeResume()` builds the card; `TotoSyncText.cpp` is the only place that turns it into `tr()`
strings, so the reader screen and the settings screen cannot word it differently.

```
Position from another device

On this reader: 30%
On Kobo de Ana: p. 240 of 400 - 60%

The location is approximate: it may land a few lines off.

              [ Stay here ]  [ Go to that position ]
```

The local side shows a percentage and no page on purpose. CrossPoint paginates per chapter and
repaginates whenever the font changes, so it reports position on a normalized 0–10000 scale
(`toto::NORMALIZED_PAGE_SCALE`) rather than in pages. "p. 12" here and "p. 240" on a Kobo are not
the same kind of number; the percentage is what both sides mean the same by. A remote report that
arrives on that same normalized scale — another CrossPoint — is shown as a percentage too.

* **Where it came from.** `origin_device.name`, falling back to `platform`, falling back to
  "another device". A name that is all hex and dashes is an id, not a name, and is not shown.
* **Both positions**, so the reader compares instead of trusting a number out of context. The
  anchor and the event id never appear: they say nothing to a person.
* **The two warnings that change the answer**: that the jump goes *backwards* in the book, and that
  the destination is *approximate* because no anchor came with it. A difference under 2 points is
  described as small rather than announced as a trip.
* **Both options are answers.** "Stay here" and "Go to that position" — not "Cancel" and "Confirm",
  which leave a reader guessing which position they just kept. Leaving the dialog with Back is
  "Stay here", the same as the option that says so.

A date is only shown when the event's `time_precision` is `exact` or `anchored`. A device that
spent a month with a flat battery reports an hour that never happened.

## The answer outlives the screen

Answers live in `/.crosspoint/toto/resume.log`, one line per book, at most 40 books, oldest
dropped first (`ResumeDecisionLog`). Each line records the position's **fingerprint** — the hub's
`event_id`, else its `server_sequence`, else the locator rounded to two decimals — plus the answer,
the percentage, the suggestion id and whether the hub has been told.

Before this the answer was a field in RAM, cleared when the screen was entered. Saying "stay here"
therefore lasted until the next visit, and a reboot lost it entirely: the same position was
proposed again and again. That is the insistence the contract rules out.

Because the fingerprint is compared with a 0.5-point tolerance, reconnecting ten times cannot
become ten identical questions. A position that genuinely moved *is* asked again, even when the
previous one was turned down.

**The answer never waits for Wi-Fi.** It is written, and the inbox entry is applied or dropped,
before anything is sent. Telling the hub (`POST /suggestions/:id/accept|dismiss`) is retried by
`SyncClient::flushPendingResolutions()` on later syncs; `404` and `409` count as delivered, because
a suggestion the hub has already closed is not worth retrying forever.

The log is account data — it names suggestions issued to one account — so
`DurableQueue::purgeAccountState()` wipes it along with the queues on every account change and
sign-out.

## Accepting, inside the book

`EpubReaderActivity` marks the inbox entry accepted *before* it applies it, so losing power between
the two reopens the book at the accepted position instead of losing it. Then
`ProgressMapper::toCrossPoint()` resolves the anchor (or the percentage, when no anchor came) to a
spine and page, `saveProgress()` persists it, and the section is repositioned in place — the book
never closes.

The reader's own `progress.changed` is not written by this path. `render()` already notices the
position moved and queues it, which is step 2 of the protocol's accept sequence and keeps one
writer for reading position instead of two.

Refusing changes nothing about where the reader is. The page in front of them stays exactly where
it was; only the suggestion leaves the inbox.

## Reading the inbox is an SD listing

The inbox is a directory on the card, so the reader checks it once when the book opens and then
only when `SyncScheduler::syncGeneration()` moves — that is, after a sync that actually completed.
Checking it every loop tick would put a directory read on the page-turn path.

## Not covered by host tests

`TotoResumeStore`, the inbox fields and the reader's jump need `Storage`, `WiFi` and an EPUB, so
they are verified on a device. The decision rules, the fingerprint, the connect order, the card and
the log's round trip through a reboot are all in `test/toto_resume_flow`.
