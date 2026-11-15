# circud

`circud` is a small Linux management framework built on
[circu.js/libcjs](../cno-cli/circu.js). Each installed package gets one native
thread and one QuickJS runtime. Packages decide what they supervise; the core
only provides calls, lifecycle, permissions, status snapshots, storage and an
HTTP management surface.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

For a warning-clean local build, add `-DCIRCUD_ENABLE_STRICT_WARNINGS=ON`.
This applies `-Wall -Wextra -Wpedantic -Werror` to circud without forcing
those flags onto bundled C dependencies.

The default libcjs source is `../cno-cli/circu.js`; override it with
`-DCIRCUD_LIBCJS_SOURCE=/path/to/circu.js`. Required system libraries are
Boost JSON, OpenSSL, SQLite, zlib and the small minizip library. Boost.System
is used in header-only mode.

## OpenWrt packages

The `OpenWrt packages` GitHub Actions workflow builds `circud` and its optional
`luci-app-circud` companion for
`x86_64`, AArch64, ARMv7, big-endian MIPS and little-endian MIPS. OpenWrt
24.10 SDKs produce `.ipk` files; current snapshot SDKs produce `.apk` files.
Each matrix job uses the official OpenWrt SDK container and uploads its package
as a workflow artifact. A `v*` tag also creates a GitHub release containing all
twelve packages: ten architecture-specific daemon packages and one
architecture-independent LuCI package for each OpenWrt series.

The workflow checks out circu.js independently, including its submodules. A
manual run can select another public repository and branch, tag or commit with
the `circu_repository` and `circu_ref` inputs. The selected circu.js revision
must contain the `TJS_EvBusEmit` ABI and private `message_clone` component used
by circud. llhttp is checked out at the release used by circu.js so package
builds do not download source during CMake configuration.

The OpenWrt feed recipe is
[`packaging/openwrt/circud/Makefile`](packaging/openwrt/circud/Makefile). It can
also be linked into a normal OpenWrt SDK feed; in that case place independent
circu.js and llhttp checkouts at `.ci/circu.js` and `.ci/llhttp` first.

On OpenWrt, circud reads `/etc/circud/config.json`; the file is preserved as a
package conffile. Install `luci-app-circud` to configure every supported field
through a tabbed form, generate an authentication password, start/stop/restart
circud, control autostart and open circud's own management UI without using a
shell. The management UI remains implemented and served by circud: LuCI exposes
it through an authenticated same-origin proxy instead of duplicating package
lifecycle controls. LuCI accounts with read-only access can inspect service
state and configuration, while saving, service actions and the management
proxy require the app's write ACL.
The proxy derives its loopback target from circud's validated configuration and
never forwards the LuCI session cookie. When circud authentication is enabled,
it maps only circud's fixed-format session bearer to a separate, path-limited
`luci_circud_session` cookie and forwards the validated CSRF header. It is
deliberately a
buffered HTTP/1.0 compatibility proxy for circud's own management UI and core
routes only: `/`, `/index.html`, `/app.js`, `/app.css`,
`/circud-api.d.ts`, `/favicon.ico`, `/assets` and its descendants, `/health`,
`/rpc`, and `/api` and its descendants.
It rejects package prefixes and does not proxy SSE, WebSocket, or arbitrary
HTTP methods. Use the native circud listener for the complete package endpoint
surface.

## Run

```sh
./build/circud --config ./config.example.json
```

Open `http://127.0.0.1:9080/`. On first start the local loopback listener is
usable without an account only when `auth.enabled` is false. For authenticated
management, generate a password hash with the TTY-only command below, place it
in `auth.passwordHash`, and enable authentication:

```sh
./build/circud hash-password
```

The command reads the password without echo and never accepts it on the command
line. Keep the listener on loopback or put an authenticating reverse proxy in
front before exposing it; the built-in cookie login is not an Internet-facing
identity provider.

The Web application lives in `public/` and is served as ordinary static files;
it is no longer embedded in the daemon. `--public-dir PATH` selects another
document root. The build-tree binary discovers the source `public/` directory,
and `cmake --install` installs it under `share/circud/public`.

Configuration is strict JSON. Relative `stateDir` and `web.publicDir` paths are
resolved from the configuration file's directory. Command-line options override
file values regardless of their position. Validate or inspect the effective
configuration without starting the daemon:

```sh
./build/circud --config ./config.example.json --check-config
./build/circud --config ./config.example.json --print-config
./build/circud --help
```

`--print-config` replaces a non-empty configured password hash with
`<redacted>`. Its output is a diagnostic view, not a restorable configuration
backup when authentication is enabled.

The configurable groups are:

- `listen`: address and port (loopback remains the default)
- `web`: enable flag, public directory, bounded background disk workers and
  connections, I/O deadline, RPC body limit, ZIP upload limit and static file
  limit
- `endpoints`: package HTTP/SSE/WebSocket enable flag, bounded sessions,
  bodies, messages, queues and lifetime deadlines
- `status`: polling enable flag, interval and timeout
- `calls`: default Web-to-package call timeout
- `runtimeStartTimeoutMs`: package runtime startup deadline
- `events`: enable flag and bounded in-memory activity capacity
- `auth`: management login, session limits and cookie policy

Every setting except `auth.passwordHash` has a matching command-line override;
the hash remains configuration-file-only so a secret never appears in the
process argument list. `--help` is the canonical list. Use `--no-web` for a
package-only daemon. See
[`config.example.json`](config.example.json) for a complete file and
[`docs/configuration.md`](docs/configuration.md) for field ranges and path
resolution rules.

The management page polls a compact event journal for package installation,
lifecycle, status and failed-call activity. This journal is bounded, is reset
on daemon restart and deliberately excludes RPC arguments/results, paths,
environment values and archive contents. It is operational context, not a
persistent logging platform.

SIGINT and SIGTERM perform a bounded shutdown: circud stops accepting network
work, revokes package generations, completes pending management calls, and
gives each package at most its `shutdownTimeoutMs` lifecycle window. A trusted
package can still block forever inside native or FFI code; after the bounded
network/control close has completed, the daemon exits the process instead of
waiting indefinitely for that owner thread. During normal operation, a timed
out package is removed from routing and handed to the background reaper, so it
does not block unrelated HTTP or lifecycle work. Fatal failures after daemon
startup use the same bounded revocation path and terminate with a failure
status; they also never unwind into an unbounded package-thread join.

A package archive can be installed with:

```sh
cmake --build build --target circud_demo_package
./build/circud --config ./config.example.json install ./build/demo.zip
```

The command-line installer is intended for initial/offline installation. While
the daemon is running, use the Web UI or upload the ZIP directly so the active
runtime is replaced as part of the same operation:

```sh
curl --fail-with-body -b ./circud.cookies \
  http://127.0.0.1:9080/api/packages/install \
  -H "X-Circud-CSRF: $CIRCUD_CSRF" \
  -H 'content-type: application/zip' \
  --data-binary @./build/demo.zip
```

Uploads are streamed to a bounded temporary file instead of being buffered in
RAM. A validated archive is published as a new immutable release generation;
the previous generation is removed only after the new runtime starts. Failed
runtime initialization rolls back to the previous generation.

Each state directory has a non-blocking process lock. A second daemon—or an
offline `circud install` aimed at a running daemon's state directory—fails
clearly instead of mutating package lifecycle state concurrently. Upgrades
preserve whether an existing package is enabled or stopped.

`POST /rpc` accepts `{ "method": "...", "params": {...} }`. The small generic
management API includes `packages.list`, `package.remove`,
`package.start`, `package.stop`, `package.reload`, `status.get`, and `call`.
`status.get` returns the cached snapshot by default; pass `refresh: true` to
wait for a fresh package response. Snapshot timestamps are Unix milliseconds.

## Package HTTP endpoints

Every package owns one complete URL subtree on the same HTTP/1.1 listener. By
default it is `/<package-id>`; a top-level `"prefix": "/demo"` in `meta.json`
replaces it. The scope, not individual routes, is declared in the manifest.
There is no runtime-global `registerEndpoint()` function. Requests are sent to
the owning runtime as the `endpoint` event in the `circud.host` EvBus namespace,
where the package synchronously chooses one action (an ordinary response value
may itself be a native Promise):

- `event.respondWith(...)` for an ordinary HTTP response
- `event.acceptSse()` for a bounded, incremental SSE stream
- `event.acceptWebSocket(...)` for text or binary WebSocket messages
- `event.serveStatic(relativePath)` for one explicit package-file response

Every valid HTTP method under the package scope is delivered unchanged,
including `PUT`, `PROPFIND`, and extension methods.

Files in a package ZIP are never exposed automatically. `serveStatic()` must
be selected for each request, and the core validates that its relative path
resolves to a regular file inside the installed package version. Prefixes may
not overlap management/Web paths or another package's scope.

The demo package uses the default `/io.circud.demo` scope. After installing it,
open `http://127.0.0.1:9080/io.circud.demo/` for the explicitly delegated page,
or try the HTTP and SSE endpoints directly:

```sh
curl http://127.0.0.1:9080/io.circud.demo/hello
curl -N http://127.0.0.1:9080/io.circud.demo/events
```

Its `/ws` path is a WebSocket echo endpoint; the demo page includes a small
browser client. The complete event and session API is documented in
[docs/packages.md](docs/packages.md), and HTTP routing is summarized in
[docs/rpc.md](docs/rpc.md).

Package endpoints receive an advisory `event.isAuthorized` value from the
built-in management session, but each package still decides whether its paths
are public or should return 401/403. The default loopback bind is intentional;
use an authenticating reverse proxy before making the listener reachable from
an untrusted network.

Package format and JavaScript APIs are documented in
[docs/packages.md](docs/packages.md); the HTTP API is listed in
[docs/rpc.md](docs/rpc.md).
