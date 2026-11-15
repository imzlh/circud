#!/usr/bin/env bash

set -euo pipefail

target_root=${1:?target root is required}
source_root=${2:?circud source root is required}
loader="$target_root/lib/ld-musl-x86_64.so.1"
ucode="$target_root/usr/bin/ucode"

test -x "$loader"
test -x "$ucode"
test -f "$target_root/usr/lib/ucode/socket.so"

ucode_run=(
	"$loader"
	--library-path "$target_root/lib:$target_root/usr/lib"
	"$ucode"
	-L "$target_root/usr/lib/ucode"
	-L "$target_root/usr/share/ucode"
)
controller="$source_root/packaging/openwrt/luci-app-circud/ucode/controller/circud.uc"
rpcd="$source_root/packaging/openwrt/luci-app-circud/root/usr/share/rpcd/ucode/luci.circud"

compile_dir=$(mktemp -d)
upstream_pid=
upstream_log=
stop_upstream() {
	if [[ -n "$upstream_pid" ]] && kill -0 "$upstream_pid" 2>/dev/null; then
		kill "$upstream_pid" 2>/dev/null || true
		wait "$upstream_pid" 2>/dev/null || true
	fi
}
cleanup() {
	stop_upstream
	if [[ -n "$upstream_log" ]]; then
		rm -f -- "$upstream_log"
	fi
	rm -rf -- "$compile_dir"
}
trap cleanup EXIT INT TERM

rpcd_fixture_dir="$compile_dir/rpcd-root/etc/circud"
rpcd_fixture_path="$rpcd_fixture_dir/config.json"
rpcd_test_source="$compile_dir/luci.circud"
mkdir -p "$rpcd_fixture_dir"
sed "s|^const CONFIG_DIR = '/etc/circud';$|const CONFIG_DIR = '$rpcd_fixture_dir';|" \
	"$rpcd" >"$rpcd_test_source"
grep -Fq "const CONFIG_DIR = '$rpcd_fixture_dir';" "$rpcd_test_source"

"${ucode_run[@]}" -c -o "$compile_dir/controller.uc" "$controller"
"${ucode_run[@]}" -c -o "$compile_dir/rpcd.uc" "$rpcd"
"${ucode_run[@]}" -c -o "$compile_dir/rpcd-test-source.uc" \
	"$rpcd_test_source"
"${ucode_run[@]}" -c -o "$compile_dir/proxy-test.uc" \
	"$source_root/tests/luci_proxy_test.uc"
"${ucode_run[@]}" -c -o "$compile_dir/proxy-upstream-test.uc" \
	"$source_root/tests/luci_proxy_upstream_test.uc"
"${ucode_run[@]}" -c -o "$compile_dir/rpcd-test.uc" \
	"$source_root/tests/luci_rpcd_test.uc"

for mode in readonly invalid-cookie canonical-path unknown-api \
	package-endpoint credentials; do
	"${ucode_run[@]}" "$source_root/tests/luci_proxy_test.uc" \
		"$controller" "$mode"
done
"${ucode_run[@]}" "$source_root/tests/luci_rpcd_test.uc" \
	"$rpcd_test_source" "$rpcd_fixture_path"

for mode in upstream-get upstream-rpc upstream-upload; do
	upstream_log=$(mktemp)
	"${ucode_run[@]}" "$source_root/tests/luci_proxy_upstream_test.uc" \
		"$mode" >"$upstream_log" 2>&1 &
	upstream_pid=$!
	ready=0
	for _ in {1..100}; do
		if grep -Fq 'LuCI proxy upstream ready' "$upstream_log"; then
			ready=1
			break
		fi
		if ! kill -0 "$upstream_pid" 2>/dev/null; then
			break
		fi
		sleep 0.05
	done
	if [[ "$ready" -ne 1 ]]; then
		cat "$upstream_log" >&2
		wait "$upstream_pid" || true
		upstream_pid=
		exit 1
	fi
	if ! "${ucode_run[@]}" "$source_root/tests/luci_proxy_test.uc" \
		"$controller" "$mode"; then
		cat "$upstream_log" >&2
		exit 1
	fi
	if ! wait "$upstream_pid"; then
		cat "$upstream_log" >&2
		upstream_pid=
		exit 1
	fi
	upstream_pid=
	cat "$upstream_log"
	rm -f -- "$upstream_log"
	upstream_log=
done

printf 'LuCI target behavior tests passed\n'
