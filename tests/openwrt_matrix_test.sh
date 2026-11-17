#!/bin/sh
set -eu

source_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
workflow=$source_root/.github/workflows/openwrt-packages.yml

for channel in ipk apk; do
    matrix=$(sh "$source_root/scripts/openwrt-matrix.sh" "$channel")
    printf '%s\n' "$matrix" | jq -e --arg channel "$channel" '
        length == 19 and
        all(.[];
            .channel == $channel and
            (.arch | test("^[A-Za-z0-9_.+-]+$")) and
            (.target | test("^[A-Za-z0-9_.+-]+$")) and
            (.subtarget | test("^[A-Za-z0-9_.+-]+$")))
    ' >/dev/null
    printf '%s\n' "$matrix" | jq -e \
        'group_by([.arch, .target, .subtarget]) | all(.[]; length == 1)' \
        >/dev/null
done

ipk=$(sh "$source_root/scripts/openwrt-matrix.sh" ipk)
apk=$(sh "$source_root/scripts/openwrt-matrix.sh" apk)
jq -cn --argjson ipk "$ipk" --argjson apk "$apk" \
    '$ipk + $apk | length == 38' | grep -Fx true >/dev/null

for arch in \
    aarch64_cortex-a53 \
    arm_cortex-a5_vfpv4 \
    i386_pentium4 \
    mips_24kc \
    mipsel_74kc \
    riscv64_generic
do
    sh "$source_root/scripts/openwrt-matrix.sh" apk |
        jq -e --arg arch "$arch" 'any(.[]; .arch == $arch)' >/dev/null
done

printf '%s\n' "$ipk" | jq -e '
    any(.[]; .arch == "riscv64_riscv64" and .target == "sifiveu" and
        .subtarget == "generic")
' >/dev/null
printf '%s\n' "$apk" | jq -e '
    any(.[]; .arch == "riscv64_generic" and .target == "sifiveu" and
        .subtarget == "generic")
' >/dev/null

# mt7620 and mt7621 deliberately retain separate SDK rows even though the
# packages carry the same OpenWrt architecture label.
printf '%s\n' "$ipk" | jq -e '
    ([.[] | select(.arch == "mipsel_24kc") | .subtarget] | sort) ==
        ["mt7620", "mt7621"]
' >/dev/null

grep -Fq 'fromJSON(needs.select.outputs.all)' "$workflow"
grep -Fq 'sh scripts/build-openwrt.sh "$CHANNEL"' "$workflow"
grep -Fq 'package architecture mismatch' "$source_root/scripts/build-openwrt.sh"
grep -Fq 'cancel-in-progress: true' "$workflow"
for build_script in \
    "$source_root/scripts/build-openwrt.sh" \
    "$source_root/scripts/build-luci-openwrt.sh"
do
    grep -Fq 'invocation_dir=$(pwd -P)' "$build_script"
    grep -Fq '/*) ;;' "$build_script"
    grep -Fq '*) output_dir=$invocation_dir/$output_dir ;;' "$build_script"
done
grep -Fq '[ -f feeds.conf ] || cp feeds.conf.default feeds.conf' \
    "$source_root/scripts/build-openwrt.sh"
grep -Fq './scripts/feeds update base packages circud' \
    "$source_root/scripts/build-openwrt.sh"
grep -Fq './scripts/feeds install -p base zlib ca-certificates' \
    "$source_root/scripts/build-openwrt.sh"
grep -Fq './scripts/feeds install -p packages boost-json minizip' \
    "$source_root/scripts/build-openwrt.sh"
grep -Fq 'libsqlite3 libffi libexpat libnghttp2' \
    "$source_root/scripts/build-openwrt.sh"
for build_script in \
    "$source_root/scripts/build-openwrt.sh" \
    "$source_root/scripts/build-luci-openwrt.sh"
do
    grep -Fq 'source_makefile = substr($0, 18)' "$build_script"
    grep -Fq 'if (package_name == name && source_makefile != "")' \
        "$build_script"
done
for boost_seed in \
    'CONFIG_PACKAGE_boost=m' \
    'CONFIG_PACKAGE_boost-json=m'
do
    grep -Fq "$boost_seed" "$source_root/scripts/build-openwrt.sh"
    grep -Fq "$boost_seed" "$source_root/scripts/build-luci-openwrt.sh"
done
grep -Fq 'scripts/normalize-openwrt-depends.sh' \
    "$source_root/scripts/build-openwrt.sh"
grep -Fq 'libnghttp2' "$source_root/scripts/build-openwrt.sh"
if grep -Eq './scripts/feeds install[^[:space:]]*(.*[[:space:]])?libcurl([[:space:]]|$)' \
    "$source_root/scripts/build-openwrt.sh"; then
    printf 'build script must not install libcurl through scripts/feeds\n' >&2
    exit 1
fi
grep -Fq './scripts/feeds install -p circud circud' \
    "$source_root/scripts/build-openwrt.sh"
grep -Fq 'Validate IPK payload' "$workflow"
grep -Fq 'Validate LuCI IPK payload' "$workflow"
grep -Fq "name 'data.tar.zst'" "$workflow"
grep -Fq 'gzip -t "$package"' "$workflow"
grep -Fq 'gzip -dc "$package" > "$work/outer.tar"' "$workflow"
grep -Fq 'verify_payload_root' "$source_root/scripts/build-openwrt.sh"
grep -Fq 'verify_payload_root' "$source_root/scripts/build-luci-openwrt.sh"
grep -Fq 'name: circud-openwrt-${{ matrix.channel }}-${{ matrix.arch }}-${{ matrix.target }}-${{ matrix.subtarget }}' "$workflow"

# luci-app-circud depends on circud, so the LuCI job must prepare the same
# external source trees as the daemon jobs.
luci_job=$(awk '
    /^  luci:$/ { in_luci = 1 }
    in_luci && /^  [A-Za-z0-9_-]+:$/ && !/^  luci:$/ { exit }
    in_luci { print }
' "$workflow")
printf '%s\n' "$luci_job" | grep -Fq 'path: .ci/circu.js'
printf '%s\n' "$luci_job" | grep -Fq 'path: .ci/llhttp'
printf '%s\n' "$luci_job" | grep -Fq 'Verify circu.js ABI'
grep -Fq '[ -f feeds.conf ] || cp feeds.conf.default feeds.conf' \
    "$source_root/scripts/build-luci-openwrt.sh"
grep -Fq './scripts/feeds update base packages luci circud' \
    "$source_root/scripts/build-luci-openwrt.sh"
grep -Fq './scripts/feeds install -p base script-utils zlib ca-certificates' \
    "$source_root/scripts/build-luci-openwrt.sh"
grep -Fq './scripts/feeds install -p base ucode-mod-socket' \
    "$source_root/scripts/build-luci-openwrt.sh"
grep -Fq './scripts/feeds install -p packages boost-json minizip' \
    "$source_root/scripts/build-luci-openwrt.sh"
grep -Fq 'libsqlite3 libffi libexpat libnghttp2' \
    "$source_root/scripts/build-luci-openwrt.sh"
grep -Fq 'scripts/normalize-openwrt-depends.sh' \
    "$source_root/scripts/build-luci-openwrt.sh"
grep -Fq './scripts/feeds install -p luci luci-base' \
    "$source_root/scripts/build-luci-openwrt.sh"
grep -Fq './scripts/feeds install -p circud circud luci-app-circud' \
    "$source_root/scripts/build-luci-openwrt.sh"
grep -Fq 'CONFIG_PACKAGE_luci-app-circud=m' \
    "$source_root/scripts/build-luci-openwrt.sh"
grep -Fq 'package/circud/compile package/luci-app-circud/compile' \
    "$source_root/scripts/build-luci-openwrt.sh"
grep -Fq 'Source-Makefile:' \
    "$source_root/scripts/build-luci-openwrt.sh"
grep -Fq 'source_makefile = substr($0, 18)' \
    "$source_root/scripts/build-luci-openwrt.sh"
grep -Fq 'if (package_name == name && source_makefile != "")' \
    "$source_root/scripts/build-luci-openwrt.sh"

printf 'OpenWrt architecture matrix tests passed\n'
