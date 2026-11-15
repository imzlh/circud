# HTTP RPC

The daemon listens on `127.0.0.1:9080` by default. `GET /health` is a liveness
check. Send JSON to `POST /rpc` as:

```json
{"method":"packages.list","params":{}}
```

Successful JSON responses use `{ "ok": true, "result": ... }`. Errors use
`{ "ok": false, "error": { "code": ..., "message": ... } }`; the HTTP
status reflects the error class. `POST /api/auth/logout` is the sole no-body
exception and returns HTTP 204. The complete, authoritative TypeScript contract
is [circud.d.ts](circud.d.ts); `public/circud-api.d.ts` is only the browser HTTP
subset used by the bundled Web UI. The generic RPC methods are:

- `system.info` — daemon/API version, public limits and status polling settings
- `events.list` — bounded in-memory management activity with incremental cursors
- `packages.list` — each package includes its runtime state and the latest
  persisted `status` snapshot (or `null` before the first collection)
- `package.remove` — `{id, purgeData?}`
- `package.start`, `package.stop`, `package.reload` — `{id}`
- `status.get` — `{id, refresh?}`; cached by default
- `call` — `{package, target, arguments?, timeoutMs?}`

Expected management errors use stable codes. Invalid envelopes and parameters
return `bad_request` with HTTP 400; missing packages and unknown methods return
`not_found` or `method_not_found` with HTTP 404; lifecycle conflicts return
`conflict` with HTTP 409. Envelopes and method parameter objects are strict;
unknown fields are rejected instead of silently ignored.

## Management events

`events.list` accepts the following strict parameter object:

```json
{
  "afterId": 0,
  "limit": 100,
  "level": "error",
  "category": "rpc",
  "package": "io.circud.demo"
}
```

Every field is optional. `limit` is `1..200`; `level` is `info`, `warning` or
`error`. The response includes `events`, `journalId`, `oldestId`, `latestId`,
`nextAfterId`, `droppedBeforeId`, `gap`, `reset` and `hasMore`. Continue from
`nextAfterId`. `gap` means the requested cursor predates retained entries;
`reset` means it is newer than this process's journal, normally because the
daemon restarted. Clients should also clear their cursor and query again from
zero when `journalId` changes; this detects a restart even when an old numeric
cursor happens to overlap the new process's ID range. Filters are exact matches.
A page advances the cursor to the
latest examined snapshot when no further matching event exists, so sparse
filters do not repeatedly scan old entries.

The journal is bounded by `events.capacity`, lives only in memory and is not a
general log service. It reports daemon/package lifecycle, install/removal,
status degradation/recovery, and failed or overloaded calls. It deliberately
does not include RPC arguments or results, environment values, archive data,
filesystem paths, or raw exception details.

Example:

```sh
curl -sS -b ./circud.cookies http://127.0.0.1:9080/rpc \
  -H 'content-type: application/json' \
  -H "X-Circud-CSRF: $CIRCUD_CSRF" \
  --data '{"method":"call","params":{"package":"io.circud.demo","target":"echo","arguments":{"hello":"world"}}}'
```

`POST /rpc` requires an `application/json` content type. The HTTP body limit is
configurable up to the 64 KiB JSON envelope limit. Package-to-package
calls may use structured-clone values and up to 16 shared/transferable binary
references; HTTP calls are JSON-only.

## Authentication

The fixed authentication routes are:

- `GET /api/auth/session` — public; returns whether authentication is disabled,
  or the anonymous/authenticated session union when it is enabled
- `POST /api/auth/login` — public JSON `{ "username": ..., "password": ... }`;
  returns the authenticated session and sets an HttpOnly session cookie
- `POST /api/auth/logout` — requires the session cookie and CSRF header;
  returns HTTP 204 without a body

Every session result contains the `authEnabled` discriminator. When it is
`false`, protected management routes bypass cookie and CSRF checks. When it is
`true`, an authenticated result contains `username`, `csrfToken`, and
`expiresAt` (Unix milliseconds). Send the cookie and the token in the fixed
`X-Circud-CSRF` header for `POST /rpc`, `POST /api/packages/install`, and
logout. Browsers should rely on the HttpOnly cookie rather than attempting to
read it. Login attempts, sessions, and authentication resources are bounded.

The listener still defaults to loopback and is not an Internet-facing identity
provider. Put an authenticating reverse proxy in front before exposing it on an
untrusted network.

The optional LuCI proxy is narrower than this native listener. It exposes only
circud's fixed management UI/static paths and the core `/health`, `/rpc`, and
`/api` subtree, through a buffered HTTP/1.0 `GET`/`HEAD`/`POST` compatibility
path. It rejects package scopes, SSE and upgrade requests, and extension
methods rather than silently downgrading them. It never forwards LuCI's session
cookie. For an authenticated circud listener it instead maps only a validated
fixed-format circud bearer through a separate path-limited cookie, rewrites
circud's session cookie response, and forwards a validated
`X-Circud-CSRF` header.

## Package endpoint scopes

The same asynchronous HTTP/1.1 listener also carries package-owned ordinary
HTTP responses, Server-Sent Events, and WebSocket upgrades. An installed
package owns `/<package-id>` and all of its descendants by default. A manifest
`prefix` replaces that default with another non-reserved subtree. For example,
`io.circud.demo` receives both `/io.circud.demo` and
`/io.circud.demo/hello`; the paths visible to the package are `/` and
`/hello`, respectively.

Management routes are resolved before package scopes. Package scopes are
resolved before the management Web document root. Prefixes therefore cannot
claim `/health`, `/rpc`, `/api`, or the reserved paths used by the bundled
Web application, and two installed package prefixes cannot be equal or nested.
All valid HTTP methods inside an owned scope go to its package, including
`PUT`, `DELETE`, `PATCH`, WebDAV methods such as `PROPFIND`, and extension
method tokens. The original method text is exposed as `event.method`; circud
does not infer a per-path handler or automatically look for a file. `CONNECT`
is delivered as well; because there is currently no tunnel takeover API, a
package must answer it with a non-2xx ordinary response and cannot serve a
static file for it.

The package handles requests using the `endpoint` event in the `circud.host`
EvBus namespace and chooses an ordinary response, SSE, WebSocket, or an
explicit static-file delegation. An ordinary `respondWith()` value may be a
native Promise, so waiting for package I/O does not block the daemon network
thread. There is no HTTP endpoint registration RPC and no runtime-global
JavaScript registration function. See [packages.md](packages.md) for the event
fields and response APIs.

SSE requires HTTP/1.1 and is emitted incrementally with chunked transfer
encoding. WebSocket text and binary messages are streamed in both directions.
These capabilities belong to the native listener and are not available through
the optional LuCI management proxy.
Connections, messages, bodies, queues, idle duration, and total lifetime are
bounded by the `endpoints` configuration. These endpoints share the listener's
security boundary. A package receives `event.isAuthorized`, but that value is
only advisory: the package decides whether a path is public or should return
401/403. Keep the listener on loopback or put an authenticating reverse proxy
in front of every reachable management and package path.

## ZIP upload

Install or upgrade a package by sending its ZIP bytes directly:

```sh
curl --fail-with-body -b ./circud.cookies \
  http://127.0.0.1:9080/api/packages/install \
  -H "X-Circud-CSRF: $CIRCUD_CSRF" \
  -H 'content-type: application/zip' \
  --data-binary @./demo.zip
```

`POST /api/packages/install` accepts `application/zip` (and the legacy
`application/x-zip-compressed`) and returns HTTP 201 with the installed package
manifest and runtime state. The body is streamed to disk and bounded by
`web.maxUploadBytes`; it does not consume the JSON RPC body budget. Invalid
archives return HTTP 400, unsupported media types return HTTP 415, and oversized
uploads return HTTP 413. Request bodies are written as bounded 64 KiB chunks
by the HTTP filesystem workers, so disk latency does not block the main
network/control loop. Temporary uploads are removed on both success and
failure.

The Web API deliberately does not accept a daemon-local path. Offline/local
path installation remains available through `circud install PACKAGE.zip`.
Each validated ZIP is placed in a new immutable release generation. When an
existing package is upgraded, circud starts the new generation before deleting
the previous files; initialization failure restores the previous database
record and runtime. The package's existing enabled/stopped state is preserved.
Orphaned staging and release directories left by an abnormal exit are reclaimed
on the next daemon start; the database-referenced active release and package
data directories are never part of that cleanup.

Removal is also transactional. Package files—and the data directory when
`purgeData` is true—are atomically staged under `stateDir/trash` before the
database record is deleted. A database failure restores them immediately. On
restart, a trash transaction is restored when the database still references the
package, or completed when the record has already been removed.

## Static Web files

After management routes and package-owned scopes have been checked, remaining
`GET` and `HEAD` requests are served from `web.publicDir` (or `--public-dir`).
`/` maps to `index.html`. This management Web document root is separate from
the files inside package ZIPs. The server supports MIME types, ETags and
`If-None-Match`; traversal and symlink escapes outside the configured root are
rejected. File sizes are bounded by `web.maxStaticFileBytes`. Static responses
include conservative content-type, referrer and content-security headers.

A package file is served only when that package receives a request in its own
scope and calls `event.serveStatic(relativePath)`. There is no automatic
package document root or directory listing.
