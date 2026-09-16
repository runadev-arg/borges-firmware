# Signing the reader into a Toto account

CrossPoint reaches the Toto hub with a **per-device credential**. The reader types the account's
username and password **once**; what it keeps afterwards is a token that belongs to this device
alone, that the owner can revoke from the web, and that expires on its own.

The exchange is not invented here. It is the hub's published contract:

| | |
|---|---|
| Repo | `runadev-arg/highlights-kobo` (Cinabrio) |
| Document | `docs/device-login-v1.md` |
| Commit | `74f56186b8cac5a13a0dafec0d1f568c97451b7b` |
| Endpoint | `https://highlights.runadev.com/api/devices/v1/login` |

That commit is pinned in code as `toto::DEVICE_LOGIN_CONTRACT_SHA`, and the fixtures published
alongside the document are copied verbatim into `test/toto_device_login/fixtures/`, where
`TotoDeviceLoginTest` replays every one of them. A contract change upstream therefore lands here as
a failing test and a visible diff, not as a surprise in the field.

## What the reader sends, and what it never sends

The body carries `username`, `password`, `platform` (`crosspoint-x4`), `external_id`,
`device_name`, `firmware_version`, `client_version`, `protocol_version` and the four scopes this
firmware actually spends: `device:self`, `sync:v2`, `library:read`, `kosync`.

It never carries `user_id`, `account_id` or `owner_id`. The reader does not name its account: the
account comes out of verifying the password, and sending an owner field is a `422` by contract
rather than a silent mis-binding.

`external_id` is `x4-<48 bits of the eFuse MAC>` — the same identifier the older code-pairing flow
uses, so both doors land on the same `(account, platform, external_id)` device row and signing in
twice leaves one device, not two.

## What is kept

| Kept | Where |
|---|---|
| `credential.token` | `/.crosspoint/toto.json`, XOR-obfuscated with the hardware key |
| `credential.username` (the device id) | same file |
| `account.username` | same file, for display and for detecting an account change |
| `expires_at` / `renew_after` | same file, to warn before the credential runs out |

**The account password is never stored.** It lives in the entry screen and in the request body, and
both are overwritten and released as soon as the request returns.

The token is spent three ways from one sign-in, exactly as the contract describes: `Bearer` for
sync v2, HTTP Basic for the OPDS catalogue, and KOSync's `md5(token)` — which KOReader computes
itself from the same value. Nothing asks for a second password.

## Transport

Every request to the hub goes through `toto::postJson` in `lib/TotoSync/TotoHttp.cpp`, which is the
only place that builds an HTTPS client. It pins ISRG Root X1 and refuses a base URL that is not
`https://`. There is no `setInsecure()` path and no custom-server field: the hub is built in.

## Changing account

A sign-in that lands on a different account than the stored one is a **switch**, and the two
accounts' data must not meet:

1. `classifyAccountTransition` compares the stored account key against the new one.
2. If the outbox still holds events for the old account, the reader asks before doing anything —
   those events cannot be delivered to the new account, and discarding somebody's highlights is
   their decision. Answering "cancel" drops the freshly issued credential and leaves the old
   session and its queue untouched.
3. On confirmation, `teardownCrossPointServices()` removes the old catalogue entry and KOSync
   credentials, `DurableQueue::purgeAccountState()` clears the outbox, inbox and cursor, and only
   then is the new session written and the services rebuilt.

A reader that arrived through code pairing has no account name, so it stores a `device:<id>`
sentinel instead. A later sign-in still reads as a switch, which is what makes the purge happen.
The same is true of a credential file written before this feature existed.

## Session state

`sessionState()` turns `renew_after` / `expires_at` into `ACTIVE`, `RENEW_DUE` or `EXPIRED`, and the
sync screen says which. When the clock is unknown the session is reported `ACTIVE`: an unset RTC
must not invent an expiry. The truth then arrives as a `401`, which the screen reports as
"re-enable this reader on the web".

**Not implemented on purpose:** automatic rotation against
`POST /api/devices/self/credentials/rotate`. The contract fixes the login response shape but not the
rotate one, and guessing it is how a reader ends up locked out on an upgrade. Until that shape is
published, an expiring credential is surfaced in the UI and refreshed by signing in again.

## Every failure names a next step

`nextStepFor()` maps each contract error to one instruction, so no screen ends on a bare "failed":

| Error | What the reader says |
|---|---|
| `invalid_credentials` | Check the username and password, then try again |
| `email_not_verified` | Confirm your email on the web, then sign in again |
| `device_revoked`, `scope_not_granted` | Re-enable this reader at highlights.runadev.com |
| `tls_required` | The server must be reached over https |
| `device_limit_reached` | Unlink a reader on the web, then try again |
| `invalid_request`, `owner_not_accepted` | The server refused this request; update the firmware |
| `rate_limited`, `login_unavailable`, unknown code | Try again in a few minutes |
| no answer / no Wi-Fi / unusable clock | No answer from the server; check Wi-Fi and try again |

An unrecognised code is deliberately *not* fatal and *not* a success: it waits. `403` is never
guessed from the status alone, because the contract puts three different causes behind it.

## The screen a reader actually sees

The Toto entry in Settings opens one list with five rows, and **each capability appears exactly
once** — there is no second door that looks like a different feature:

| Row | What it does |
|---|---|
| Account | Signs in when signed out; asks to confirm signing out when signed in |
| Sync now | One exchange with the hub, and reports what was sent and received |
| Library | Opens the account's catalogue in picker mode, ready to browse |
| Status and help | Read-only: what is pending, when sync last worked, what to quote in a report |
| Advanced | Diagnosis and recovery only |

`Sync now` and `Library` are dimmed while signed out rather than hidden: a menu that changes shape
is harder to learn than one with a greyed-out line.

**Advanced** holds pairing by code (the door for readers that cannot keep a stable identifier),
re-registering the services a sign-in sets up, and discarding a reading position that is waiting
for an answer. It chooses and returns; `TotoSyncActivity` is the only screen that connects to
Wi-Fi and spends the session, so a request can never be started from two places at once.

A **reading position left by another device is a question, not a menu row.** It is asked as a
confirmation when the screen opens and after any sync that pulls one. Backing out of the question
decides nothing: the suggestion stays in the inbox, the status line keeps saying so, and Advanced
still offers to drop it.

The rules behind all of this — which row is live, which sentence the status line shows, how old the
last sync is — live in `lib/TotoSync/TotoSyncMenu.{h,cpp}`, which has no Arduino, i18n or storage
dependency and is covered by `test/toto_sync_menu` on the host. `src/activities/settings/
TotoSyncText.*` is the only place that turns those decisions into `tr()` strings, so the two screens
cannot drift into describing one state differently.

### Last sync, and the report code

`credential.lastSyncAt` is written in `SyncClient::syncOnce()` on a `200`, dated by the hub's
`server_time` when it sends one and by the local clock otherwise. It is account-scoped: every
credential transition resets it, so a new account never inherits the previous one's success.

The report code is `<firmware version> / <device id>` and nothing else. The device id is the same
non-secret identifier the OPDS catalogue uses as a username; the session token is not a parameter of
`toto::reportCode()`, so no caller can leak it onto the screen.

> The screen points at `<hub host>/help` for the report form itself. That path is the one
> assumption here: the C13 form is owned upstream, and if it lands anywhere else only
> `STR_TOTO_HELP_REPORT` has to change.

### Translations

New copy is written in English and Spanish. `gen_i18n.py` fills the other 29 languages from English
and warns, which is the intended state until a translator gets to them — the protocol strings were
not touched.
