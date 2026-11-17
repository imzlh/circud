# AGENTS.md

## Project scope

`circud` is a small Linux management framework built on `circu.js/libcjs`.
Keep it simple and usable; do not turn it into a complete LuCI replacement,
container sandbox, package repository, or general distributed supervisor.

Work locally. Do not use Box or another cloud host for builds, tests, or source
transfer. Do not initialize a Git repository, create commits, or publish
anything unless the user explicitly asks.

## Repository relationship

The default libcjs source tree is the sibling repository:

```text
../cno-cli/circu.js
```

That repository may contain unrelated, uncommitted user work. Never reset,
checkout, clean, reformat, or otherwise discard it. Make changes there only
when circud genuinely needs a libcjs API or shared implementation that cannot
live in circud.

The public native event entry point is:

```c
JSValue TJS_EvBusEmit(JSContext *ctx,
                      const char *namespace_name,
                      const char *event_name,
                      JSValueConst payload);
```

Cross-runtime clone support is intentionally private to libcjs in
`src/message_clone.c` and `src/message_clone.h`. Worker messaging and the
native `circud` module must share it. Do not introduce a public
`TJSStructuredClone` API or duplicate the clone implementation in circud.

## Architecture invariants

- Use C++20 for circud and C for libcjs internals.
- Each running package owns one native thread and one independent
  `TJSRuntime`; never share a `JSContext` or `JSValue` across runtime threads.
- Cross-thread JavaScript values must use the libcjs message-clone component.
- `circud` is compiled into libcjs as a static native ES module. It is loaded
  with standard `import { ... } from "circud"` syntax and must not be exposed
  through `import.meta.use()`.
- Incoming management calls are dispatched through
  `TJS_EvBusEmit(ctx, "circud.host", "call", event)`.
- `respondWith()` must be called synchronously during event dispatch, at most
  once. Its value may be a Promise.
- Keep call queues, pending calls, message envelopes, transfer references,
  timeouts, and HTTP bodies bounded.
- `acceptPackageCalls: false` rejects calls originating from another package;
  core and Web management calls remain allowed.
- Package shutdown/reload sends the optional `lifecycle.stop` call and waits
  no longer than `shutdownTimeoutMs`.
- Status is polled periodically and persisted as a snapshot. Web reads use the
  snapshot unless an explicit refresh is requested. Concurrent refreshes for
  the same package must coalesce.
- The HTTP listener defaults to loopback and has no built-in authentication.
  Do not silently change the default to a public address.
- HTTP clients must not block one another indefinitely. Preserve the bounded
  worker pool, connection queue, and I/O deadlines.
- Package lifecycle operations must remain serialized. Shutdown must complete
  pending call futures and must not leak stale callbacks into a later restart.

## Permissions and trust model

Packages are trusted native-capable code. The libcjs capability hook prevents
accidental access; it is not a hostile-code security boundary.

- The installed package directory is read-only.
- The package data directory is readable and writable.
- Additional filesystem roots and environment names come from `meta.json`.
- Network, process, FFI, and low-level system access are opt-in.
- Preserve module-load capability checks as well as direct filesystem checks.
- Do not claim that path-prefix checks protect against malicious symlink,
  descriptor, native module, FFI, or TOCTOU behavior.

## Package installation

Use minizip, not libarchive, for ZIP packages. Preserve the installer checks
for archive and expansion limits, traversal, absolute paths, backslashes,
duplicate destinations, links, encryption, unsupported compression methods,
missing manifests, and missing entry modules.

Do not replace an existing installed version until a new archive has been
fully extracted and validated. Preserve package data unless removal explicitly
requests `purgeData`.

## Build and test

Required system libraries include Boost JSON, OpenSSL, SQLite, zlib, and
minizip. Boost.System is intentionally header-only so current OpenWrt snapshots
do not need the removed `boost-system` binary package.

Normal Release build:

```sh
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DCIRCUD_ENABLE_STRICT_WARNINGS=ON
cmake --build build-release -j2
ctest --test-dir build-release --output-on-failure
```

AddressSanitizer build:

```sh
cmake -S . -B build-asan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DCIRCUD_ENABLE_ASAN=ON \
  -DCIRCUD_ENABLE_STRICT_WARNINGS=ON
cmake --build build-asan -j2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  ctest --test-dir build-asan --output-on-failure
```

If llhttp has already been fetched, offline/local builds may reuse it with:

```sh
-DFETCHCONTENT_SOURCE_DIR_LLHTTP=/path/to/existing/llhttp-source
```

Generate and install the demo package with:

```sh
cmake --build build-release --target circud_demo_package
./build-release/circud --state-dir ./state install ./build-release/demo.zip
```

Before handing off a behavioral change, run the smallest relevant test first,
then the full CTest suite. Changes involving ownership, cross-thread calls,
reload, cancellation, or shutdown also require the ASan suite. Keep tests for:

- ZIP and manifest validation
- native capability, EvBus, and message-clone ABIs
- package data-directory access and denied outside access
- package-call policy
- `host.reload()` generation replacement
- pending-call completion during shutdown
- stop/start reuse without stale calls
- slow HTTP clients not blocking other clients

## Installation and documentation

`cmake --install` should install only circud-owned artifacts: the daemon,
OpenWrt init script, example configuration and Web static files. Keep
implementation dependencies excluded from the install manifest.

The optional `luci-app-circud` manages the daemon and
`/etc/circud/config.json`; it must not duplicate circud's package-management
UI. Keep its read and write ACLs distinct. Access to the authenticated proxy
for circud's own Web UI requires write permission because that UI can mutate
installed packages. Candidate configuration must be size-bounded, validated
by circud and atomically replaced before a restart is offered.
The proxy target must remain configuration-derived. Never forward the LuCI
session cookie upstream or allow an upstream `Set-Cookie` header to replace it.

The OpenWrt GitHub Actions build must clone circu.js independently with its
submodules. Keep 24.10 `.ipk` builds separate from snapshot `.apk` builds; the
two suffixes represent different package-manager generations, not interchangeable
archive names. The daemon target matrix is keyed by the SDK's official
`ARCH_PACKAGES` value, not a generic CPU family. Keep the SDK target/subtarget
in each target row because distinct SDKs can share a package architecture with
different CPU tuning. `config/openwrt-targets.tsv` is the single matrix source
for both `.ipk` and `.apk` channels; its data rows mirror the OpenWrt subset of
`cno-builds`. Do not vendor circu.js into this repository to make CI pass.

Update these files when their contracts change:

- `README.md` for build and run instructions
- `docs/packages.md` for package manifests and JavaScript APIs
- `docs/rpc.md` for HTTP RPC methods and envelopes

Prefer precise documentation of current behavior over speculative future
features. Preserve the warning that externally exposing the HTTP listener
requires an authenticating reverse proxy.
