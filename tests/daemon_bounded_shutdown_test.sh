#!/bin/sh
set -eu

circud_binary=$1
test_root=$(mktemp -d "${TMPDIR:-/tmp}/circud-bounded-shutdown.XXXXXX")
daemon_pid=

cleanup() {
    if [ -n "${daemon_pid}" ] && kill -0 "${daemon_pid}" 2>/dev/null; then
        kill -KILL "${daemon_pid}" 2>/dev/null || true
        wait "${daemon_pid}" 2>/dev/null || true
    fi
    rm -rf "${test_root}"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "${test_root}/package"
cat >"${test_root}/package/meta.json" <<'EOF'
{
  "schema": 1,
  "id": "io.example.blocked-owner",
  "name": "blocked owner shutdown test",
  "version": "1.0.0",
  "entry": "management.js",
  "shutdownTimeoutMs": 100,
  "permissions": { "sys": true }
}
EOF
cat >"${test_root}/package/management.js" <<'EOF'
import { info } from 'circud';
const fs = import.meta.use('fs');
const os = import.meta.use('os');
const { setTimeout } = import.meta.use('timers');

setTimeout(() => {
  fs.writeFile(info().dataDir + '/owner-blocked', new Uint8Array([1]));
  os.sleep(600000);
}, 10);
EOF

(cd "${test_root}/package" && cmake -E tar cf "${test_root}/blocked.zip" \
    --format=zip -- meta.json management.js)
"${circud_binary}" --state-dir "${test_root}/state" --no-web \
    install "${test_root}/blocked.zip" >/dev/null

"${circud_binary}" --state-dir "${test_root}/state" --no-web \
    --no-status-polling >"${test_root}/daemon.log" 2>&1 &
daemon_pid=$!

attempt=0
while [ "${attempt}" -lt 200 ]; do
    if find "${test_root}/state" -name owner-blocked -type f -print -quit |
            grep -q .; then
        break
    fi
    if ! kill -0 "${daemon_pid}" 2>/dev/null; then
        cat "${test_root}/daemon.log" >&2
        echo "daemon exited before the package owner blocked" >&2
        exit 1
    fi
    attempt=$((attempt + 1))
    sleep 0.01
done
if [ "${attempt}" -eq 200 ]; then
    cat "${test_root}/daemon.log" >&2
    echo "package owner did not enter the native blocking call" >&2
    exit 1
fi

kill -TERM "${daemon_pid}"
attempt=0
while kill -0 "${daemon_pid}" 2>/dev/null && [ "${attempt}" -lt 200 ]; do
    attempt=$((attempt + 1))
    sleep 0.01
done
if kill -0 "${daemon_pid}" 2>/dev/null; then
    cat "${test_root}/daemon.log" >&2
    echo "daemon waited indefinitely for a blocked package owner" >&2
    exit 1
fi

set +e
wait "${daemon_pid}"
status=$?
set -e
daemon_pid=
if [ "${status}" -ne 0 ]; then
    cat "${test_root}/daemon.log" >&2
    echo "daemon returned ${status} after bounded SIGTERM shutdown" >&2
    exit 1
fi
