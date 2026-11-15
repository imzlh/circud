# Packages

A package is a ZIP whose root contains `meta.json`. Only stored and deflated
entries are accepted. Encrypted entries, links, absolute paths,
backslashes, duplicate destinations and `..` components are rejected.
`meta.json` is limited to 64 KiB. Schema 1 is strict: unknown top-level or
permission fields are rejected so misspelled capability settings cannot be
silently ignored.

```json
{
  "schema": 1,
  "id": "io.example.demo",
  "name": "Demo",
  "version": "1.0.0",
  "entry": "management.js",
  "acceptPackageCalls": true,
  "shutdownTimeoutMs": 5000,
  "prefix": "/demo",
  "permissions": {
    "read": [], "write": [], "env": [],
    "net": false, "run": false, "ffi": false, "sys": false
  }
}
```

`prefix` gives the package ownership of one complete HTTP subtree. It is a
canonical absolute base path without a trailing slash. The example owns
`/demo` and every path below it; the package decides at request time which
paths serve ordinary HTTP, SSE, or a WebSocket upgrade. When `prefix` is
omitted, it defaults to `/<package-id>` (for example,
`/io.example.demo`). Both `/demo` and `/demo/...` are routed to the package;
the event path is `/` for the exact base and starts with `/` below it. circud
does not add an automatic trailing-slash redirect.

The prefix cannot be `/`, overlap management paths such as `/health`, `/rpc`,
or `/api`, or claim bundled Web assets such as `/index.html`, `/app.js`,
`/app.css`, `/circud-api.d.ts`, `/assets`, and `/favicon.ico`. URL encoding,
query or fragment characters, backslashes, empty path segments, `.` and `..`
segments are not accepted. Package-to-package prefix overlap is checked by the
core when packages are installed or routed; a single manifest cannot decide
it.

There is no process-global endpoint registration function and no per-route
manifest array. The manifest claims the subtree; handlers remain local to the
owning package runtime.

The entry is an ES module. It may import relative `.js` and `.json` files from
the package directory. The package-facing circud API is a native ES module:

```js
import { AbortSignal, call, info, reload } from 'circud';
```

`circud` is a real static module specifier, not an `import.meta.use()` alias.
Other libcjs-native facilities, including EvBus, retain their libcjs module
loading API. There is no `import.meta.use('circud')` or `circud:host` module
alias.

The complete, authoritative TypeScript contract for the package runtime and
HTTP management API is [circud.d.ts](circud.d.ts). The declaration in
`public/circud-api.d.ts` is only the browser-facing management subset used by
the bundled Web UI. Map the bare module name `"circud"` to `circud.d.ts` in the
package's TypeScript configuration for type checking. The declaration uses
`declare module "circud"` and does not patch libcjs globals.

```js
/** @typedef {import('circud').ManagementCallEvent} ManagementCallEvent */
/** @typedef {import('circud').EndpointEvent} EndpointEvent */

import { call } from 'circud';
const evbus = import.meta.use('evbus');

evbus.on('circud.host', 'call', /** @param {ManagementCallEvent} event */ event => {
  if (event.target === 'lifecycle.stop') {
    event.respondWith(stopAndWaitForChildren(event.arguments.reason));
    return;
  }
  if (event.target === 'status.get') {
    event.respondWith({ state: 'running' });
    return;
  }
  if (event.target === 'echo') event.respondWith(event.arguments);
});

// Calls another package. source, call id and generation are core-owned.
const result = await call('io.example.other', 'echo', ['hello']);
```

## Package HTTP endpoints

Requests in the package's claimed subtree are delivered through EvBus. The
core removes the manifest prefix before dispatch, so `/demo/hello?name=Ada`
arrives with `event.path === '/hello'` and `event.query === 'name=Ada'`.

```js
evbus.on('circud.host', 'endpoint', /** @param {EndpointEvent} event */ event => {
  if (event.protocol === 'websocket') {
    const options = event.offeredSubprotocols.includes('demo.v1')
      ? { subprotocol: 'demo.v1' }
      : {};
    const socket = event.acceptWebSocket(options);
    if (socket && event.path !== '/socket')
      socket.close(1008, 'unknown endpoint');
    return;
  }

  if (event.path === '/hello') {
    event.respondWith({
      status: 200,
      headers: { 'cache-control': 'no-store' },
      body: { message: 'hello', method: event.method },
    });
    return;
  }

  if (event.path === '/events') {
    const stream = event.acceptSse();
    stream.send('event: ready\ndata: {"ok":true}\n\n');
    stream.close();
    return;
  }

  if (event.path === '/') {
    event.serveStatic('static/index.html');
    return;
  }

  event.respondWith({ status: 404, body: 'not found\n' });
});
```

The `endpoint` event has these core-owned properties:

- `id`: numeric session identifier
- `protocol`: `"http"` or `"websocket"`; SSE begins as an HTTP request
- `method`, `path`, and `query`
- `headers`: an array of `[name, value]` pairs
- `body`: a `Uint8Array`
- `offeredSubprotocols`: WebSocket protocols offered by the client
- `remoteAddress`: peer address and port when available
- `isAuthorized`: whether circud recognized a valid management session
- `signal`: a runtime-owned, subscribable abort signal for endpoint completion

`isAuthorized` is advisory. circud does not reject an anonymous request in a
package-owned scope: the package decides which endpoints are public and which
return 401 or 403. Do not treat this hint as a separate sandbox boundary.

`signal` is an instance of the `AbortSignal` class exported by `circud` and
provides `aborted`, `reason`, `onabort`,
`addEventListener('abort', ...)`, `removeEventListener('abort', ...)`, and
`throwIfAborted()`. Therefore `event.signal instanceof AbortSignal` is true.
Packages cannot construct one themselves. Abortion fires at most once, and
listeners run on the package's owning runtime thread. Register the listener
during dispatch before beginning cancellable asynchronous work.

During synchronous dispatch the handler must choose exactly one of
`respondWith()`, `serveStatic()`, `acceptSse()`, or `acceptWebSocket()`.
Calling the selection method later or calling more than one is not supported.
`respondWith()` itself is called synchronously, but its value may be either a
response object or a native Promise which later resolves to one. Ordinary
thenable objects are not part of this contract. The endpoint idle
and maximum-lifetime limits remain in force while that Promise is pending. If
no listener handles the event, the core returns 404. A listener which handles
it but does not choose a response produces an error response.

`event.respondWith({status?, headers?, body?})` sends one ordinary HTTP
response. `status` is a finite integer from 200 through 599 and defaults to
200; informational statuses cannot be used as a final package response.
`headers` is an object. A string body is
sent as text, byte buffers are sent unchanged, and other supported values are
JSON encoded; a JSON body receives `application/json; charset=utf-8` unless a
content type was supplied. Response headers and bodies are validated and
bounded by the endpoint configuration. For asynchronous work, pass the Promise
immediately: `event.respondWith(loadData().then(data => ({body: data})))`.

Every valid HTTP method in the claimed subtree is dispatched to the package,
including `PUT`, `DELETE`, `PATCH`, WebDAV methods such as `PROPFIND`, and
extension method tokens. `event.method` preserves the method text received on
the wire. circud applies method restrictions only to its own reserved
management routes and to the separate management Web document root. `CONNECT`
is also delivered, but circud does not yet expose a tunnel takeover API, so an
ordinary response to it must be non-2xx and it cannot select `serveStatic()`.

`event.acceptSse({status?, headers?})` starts an HTTP/1.1 Server-Sent Events
response and returns a session handle. `send(text)` accepts a UTF-8 string that
is already framed according to the SSE format, including its terminating blank
line. It must be non-empty because a zero-length HTTP chunk is the chunked-body
terminator. It does not add `data:` or delimiters. `close()` writes the final HTTP
chunk. The core supplies `text/event-stream; charset=utf-8`, `no-cache`, and
an `X-Accel-Buffering: no` hint; the package may supply other validated
response headers. The status is fixed at 200; no-content and redirect statuses
cannot establish a circud SSE session.

`event.acceptWebSocket({headers?, subprotocol?})` is valid only for a WebSocket
upgrade. If selected, `subprotocol` must be one of `offeredSubprotocols`. The
returned session handle has `send(value)` and `close(code?, reason?)` methods;
strings produce text frames and an `ArrayBuffer`, typed array, or `DataView`
produces a binary frame. Package close codes are limited to RFC 6455 wire codes
1000-1003, 1007-1013, and 3000-4999; reserved codes such as 1005 and 1006 are
rejected. A peer Close frame without a status is reported to the package as
code 1005. WebSocket input and closure return to EvBus:

```js
evbus.on('circud.host', 'endpoint.message', event => {
  // event contains {id, session, data, binary}. Echo with the same frame kind.
  event.session.send(event.data);
});

evbus.on('circud.host', 'endpoint.close', event => {
  // event contains {id, session, code, reason}.
  // session is undefined if an ordinary response Promise was still pending.
});
```

The session handle is deliberately thin; EvBus is the registration and
dispatch surface. There is no `registerEndpoint()` API and no globally mutable
route table exposed to JavaScript.

`event.serveStatic(relativePath)` explicitly delegates one request to the
core. The path is resolved relative to that installed package version, must be
relative, must stay inside it after canonicalization, and must name a regular
file. File size is bounded by both the Web static-file limit and endpoint
response limit. Merely placing a file in the ZIP never makes it reachable:
every static request still needs the package to call `serveStatic()`.

Endpoint request bodies, responses, messages, output queues, global sessions,
per-package sessions, idle time, and maximum lifetime are bounded under the
`endpoints` configuration group. Socket I/O stays on the daemon's network
loop; each package continues to process EvBus events on its own runtime thread.
The listener defaults to loopback. circud authenticates its management routes,
but package endpoints enforce their own policy using the advisory
`isAuthorized` value. An authenticating reverse proxy is still required before
exposing the listener on an untrusted network.

`info()` returns `{packageId, packageDir, dataDir}`. The installed package
directory is readable and its per-package data directory is readable and
writable without listing them in the manifest. Extra `read` and `write`
entries are filesystem roots; `env` entries are exact environment-variable
names. `net`, `run`, `ffi`, and `sys` opt into network, child-process, dynamic
library, and low-level OS/worker capabilities respectively. These checks are
an accidental-access guard for trusted packages, not a hostile-code sandbox.

`reload()` asynchronously asks the core to restart the calling package.
`call()` rejects its returned Promise when the target is unavailable or
has `acceptPackageCalls: false`. Calls from the HTTP/core management surface
are still allowed when that flag is false.

For management `call` events, `respondWith()` must be called during synchronous
event dispatch and may be called once. Its value may be a native Promise;
ordinary thenable objects are not part of this contract. The call's `signal`
uses the same single-fire `AbortSignal` class described above. Calls default to
30 seconds and are bounded at five minutes. Structured envelopes
are limited to 64 KiB; large binary values use `SharedArrayBuffer` or explicit
ArrayBuffer transfer lists.

Before stopping or reloading a package, the core sends an optional
`lifecycle.stop` call and waits for its response Promise for at most
`shutdownTimeoutMs`. Packages that spawn processes should terminate and wait
for them there. If there is no listener, shutdown proceeds immediately.

If a trusted package remains stuck in native or FFI code after this deadline,
its generation is no longer routable and normal runtime management continues
without waiting for its owner thread. On final daemon SIGINT/SIGTERM shutdown,
circud completes its bounded network/control close and then exits the process;
it does not wait forever for an unkillable package thread.

The fixed status response is `{state, message?}` where `state` is one of
`starting`, `running`, `stopped`, `degraded`, `failed`, or `unknown`.
The core polls it every 30 seconds with a five-second timeout by default; both
values and polling itself are configurable. HTTP reads use the stored snapshot
unless `status.get` is called with `refresh: true`.

Packages that spawn children must terminate and wait for them during shutdown.
