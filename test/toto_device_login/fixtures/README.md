# Contract fixtures — device login v1

These files are **verbatim copies** of the fixtures the hub publishes for the reader-login
contract. Do not reformat or trim them: the whole point is that refreshing the copy shows any
upstream change as a plain diff.

| | |
|---|---|
| Upstream repo | `runadev-arg/highlights-kobo` (Cinabrio) |
| Upstream path | `docs/contract-fixtures/device-login-v1/` |
| Contract document | `docs/device-login-v1.md` |
| Commit | `74f56186b8cac5a13a0dafec0d1f568c97451b7b` |

The same commit is pinned in code as `toto::DEVICE_LOGIN_CONTRACT_SHA`, and
`docs/toto-account-login.md` records what CrossPoint implements from it.

Each file wraps the exchange as `{name, description, request, response:{status, body}}`;
`TotoDeviceLoginTest.cpp` slices out `response.body` before feeding it to the parser.

Nothing here is real: UUIDs are `1111…`/`2222…`, tokens are masked with `X`, and passwords are
`********`. The hub's master key (`SYNC_API_KEY`) never appears in this contract, in clear or
derived, so these files carry no secret.

## Refreshing

```sh
cp <highlights-kobo>/docs/contract-fixtures/device-login-v1/*.json test/toto_device_login/fixtures/
```

Then update the commit above, `DEVICE_LOGIN_CONTRACT_SHA`, and re-run `TotoDeviceLoginTest`.
