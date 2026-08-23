# Agent REST API

Optional HTTP/JSON control API for the agent, enabled with `--rest <port>`. It
is a *sibling* of the plaintext control channel on the agent port (the
`STATUS` / `BRIDGE_START` / `BRIDGE_STOP` / `RESTART` line protocol the TV
speaks) — the TV keeps using that channel unchanged; REST exists for tooling:
curl, scripts, dashboards, health checks.

```powershell
ctm-usbip agent --rest 48055                      # loopback-only, no auth
ctm-usbip agent --rest 48055 --rest-lan --rest-token sekrit
ctm-usbip install --rest 48055                    # service mode carries the same flags
```

Disabled by default; without `--rest` nothing changes. If the REST port cannot
be bound the agent fails startup (exit 4) rather than running with a silently
missing API.

## Endpoints

All responses are `application/json`, `Connection: close`.

| Method | Path | What |
|---|---|---|
| `GET` | `/api/v1/status` | agent version, transport, ports, uptime, session count |
| `GET` | `/api/v1/sessions` | all bridge sessions |
| `GET` | `/api/v1/sessions/{busid}` | one session, or 404 |
| `POST` | `/api/v1/sessions` | start a bridge session (like `BRIDGE_START`) |
| `DELETE` | `/api/v1/sessions/{busid}` | stop a session (like `BRIDGE_STOP`) |
| `POST` | `/api/v1/restart` | soft reset of all bridges, or hard service restart |

### GET /api/v1/status

```json
{"version":"1.4.0","transport":"tcp","control_port":48054,"usbip_port":3240,
 "uptime_seconds":812,"session_count":1}
```

`transport` is `"enet"` when the agent runs with `--enet`, else `"tcp"`.

### GET /api/v1/sessions

```json
[{"busid":"ctm-ds5-1","kind":"ds5","port":48100,"ready":true,"last_error":""}]
```

`ready` flips true once the session worker has loaded the map/profile, started
the transport and exported the device; `last_error` carries the most recent
worker failure (empty when none).

### POST /api/v1/sessions

Body (flat JSON object):

```json
{"kind":"ds5","port":48100,"busid":"ctm-ds5-1"}
```

- `kind`: one of `ds4`, `ds5`, `ds5_usb`, `ds5e_usb`, `hid`, `puck`, `xbox`
  (the same list `BRIDGE_START` accepts on the text channel — the two are
  kept in sync, and `tests/rest_parser_test.cpp` pins the REST side)
- `port`: 1024–65535 — the per-controller data port the TV client connects to
- `busid`: 1–31 printable ASCII characters

Replies `202 Accepted` with `{"status":"starting", ...}` — session bring-up is
asynchronous, exactly like `BRIDGE_START`'s `OK bridge starting`. Poll
`GET /api/v1/sessions/{busid}` for `ready` / `last_error`. Idempotent: posting
an existing `busid` leaves the running session untouched and still answers 202.

```bash
curl -s -X POST localhost:48055/api/v1/sessions \
     -H 'Content-Type: application/json' \
     -d '{"kind":"ds5","port":48100,"busid":"ctm-ds5-1"}'
```

### DELETE /api/v1/sessions/{busid}

`200 {"busid":"...","stopped":true}` after a full teardown (device removed
from the USB/IP server — Windows sees an unplug — backend stopped, worker
joined), or `404` if no such session.

### POST /api/v1/restart

Optional body `{"mode":"soft"}` (default) or `{"mode":"hard"}`.

- **soft** — tears down every bridge session and its USB/IP export; the agent
  keeps listening and rebuilds on the next start. `200`.
- **hard** — full process restart via the SCM. `202` in service mode, `409`
  when running interactively (same rule as `RESTART hard` on the text channel).

## Security

- **Bind address.** Loopback (`127.0.0.1`) by default. `--rest-lan` binds
  `0.0.0.0`; the installer's program-based firewall rule (private profile)
  already covers any port the exe listens on, so no extra rule is needed.
- **Auth.** `--rest-token <t>` requires `Authorization: Bearer <t>` on every
  endpoint (401 otherwise). Strongly recommended with `--rest-lan`. Token
  comparison is length-independent. In service mode the token is part of the
  service image path and visible via `sc qc` to anyone who can query the SCM —
  the same trust level as every other service argument. Note the plaintext
  control channel on the agent port remains unauthenticated by design (the TV
  needs it); the token protects the REST surface only.
- **No CORS headers, on purpose.** Browsers therefore cannot make cross-origin
  JSON POSTs or DELETEs to this port, which blocks drive-by CSRF against a
  loopback listener. Non-browser clients are unaffected. If you build a
  local-dashboard use case that needs CORS, add it deliberately (ideally
  together with the token) rather than defaulting it on.
- **Listener hardening.** `SO_EXCLUSIVEADDRUSE` (no local port hijack of the
  token-bearing listener), 2 s receive/send timeouts, 16 KiB head / 8 KiB body
  caps, chunked transfer encoding rejected.

## Threading model (read before extending)

Requests are handled **inline on the agent loop thread**, exactly like the
plaintext channel's `handle_agent_client`. That thread is the only one allowed
to start/stop sessions (see the reap-queue comment at the top of
`src/app/agent.inl`), which is what lets the handlers call
`start_bridge_session` / `stop_bridge_session` directly. If the REST listener
is ever moved to its own thread, session lifecycle changes must be queued back
to the loop (like `request_bridge_session_reap`) instead of called directly.

The trade-off is the same one the text channel already makes: a request is
served to completion before the loop returns to `select()`. The per-client
timeouts bound the worst case at a couple of seconds; the expected case
(loopback curl) is microseconds.

## Tests

The HTTP/JSON parsing half of `src/app/rest.inl` is covered by
`tests/rest_parser_test.cpp`, a suite in the shared test harness — build and
run all suites with:

```powershell
.\build-tests.ps1
```

The `REST_PARSER_ONLY` guard keeps winsock and agent state out of that
suite, so unlike the device-config tests it has no Windows dependency of its
own.
