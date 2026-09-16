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
