#!/bin/sh
set -eu

channel=${1:?Usage: build-luci-openwrt.sh ipk|apk ARCH SDK_TARGET SDK_SUBTARGET OUTPUT_DIR}
expected_arch=${2:?Missing package architecture}
sdk_target=${3:?Missing SDK target}
sdk_subtarget=${4:?Missing SDK subtarget}
output_dir=${5:?Missing output directory}

case $channel in ipk|apk) ;; *) printf 'invalid package channel: %s\n' "$channel" >&2; exit 2 ;; esac
source_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

for value in "$expected_arch" "$sdk_target" "$sdk_subtarget"; do
    case $value in ''|*[!A-Za-z0-9_.+-]*) printf 'invalid matrix value: %s\n' "$value" >&2; exit 2 ;; esac
done
for command_name in curl sha256sum tar make; do
    command -v "$command_name" >/dev/null 2>&1 || {
        printf 'required command not found: %s\n' "$command_name" >&2
        exit 1
    }
done

release=24.10
if [ "$channel" = ipk ]; then
    listing=$(curl --fail --location --show-error --silent --retry 3 --retry-all-errors \
        https://downloads.openwrt.org/releases/)
    release=$(printf '%s\n' "$listing" |
        sed -n 's/.*href="\(24\.10\.[0-9][0-9]*\)\/".*/\1/p' |
        sort -t. -k3,3n | tail -n 1)
    [ -n "$release" ] || { printf 'no OpenWrt 24.10 release found\n' >&2; exit 1; }
fi

work=$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/circud-luci.XXXXXX")
trap 'rm -rf -- "$work"' EXIT HUP INT TERM
if [ "$channel" = ipk ]; then
    sdk_base="https://downloads.openwrt.org/releases/$release/targets/$sdk_target/$sdk_subtarget"
else
    sdk_base="https://downloads.openwrt.org/snapshots/targets/$sdk_target/$sdk_subtarget"
fi
curl_args='--fail --location --show-error --silent --retry 3 --retry-all-errors'
# shellcheck disable=SC2086
curl $curl_args "$sdk_base/sha256sums" -o "$work/sha256sums"
# shellcheck disable=SC2086
curl $curl_args "$sdk_base/" -o "$work/index.html"
sdk_name=$(sed -n \
    's/.*href="\([^"]*openwrt-sdk-[^"]*\.tar\.\(zst\|xz\)\)".*/\1/p' \
    "$work/index.html" | sort -u | head -n 1)
[ -n "$sdk_name" ] || { printf 'no SDK archive found at %s\n' "$sdk_base" >&2; exit 1; }
case $sdk_name in */*|.*|*[!A-Za-z0-9_.+-]*) printf 'invalid SDK archive name\n' >&2; exit 1 ;; esac
expected_sdk_sha256=$(awk -v wanted="$sdk_name" '
    { name=$2; sub(/^\*/, "", name) }
    name == wanted { print $1; found++ }
    END { exit found != 1 }
' "$work/sha256sums") || {
    printf 'SDK checksum missing or ambiguous: %s\n' "$sdk_name" >&2
    exit 1
}
# shellcheck disable=SC2086
curl $curl_args "$sdk_base/$sdk_name" -o "$work/$sdk_name"
sdk_sha256=$(sha256sum "$work/$sdk_name" | awk '{print $1}')
[ "$sdk_sha256" = "$expected_sdk_sha256" ] || {
    printf 'SDK checksum mismatch: %s\n' "$sdk_name" >&2
    exit 1
}
case $sdk_name in
    *.tar.zst)
        command -v zstd >/dev/null 2>&1 || { printf 'zstd is required\n' >&2; exit 1; }
        tar --zstd -xf "$work/$sdk_name" -C "$work"
        ;;
    *.tar.xz) tar -xJf "$work/$sdk_name" -C "$work" ;;
    *) printf 'unsupported SDK archive: %s\n' "$sdk_name" >&2; exit 1 ;;
esac
sdk_dir=$(find "$work" -mindepth 1 -maxdepth 1 -type d -name 'openwrt-sdk-*' | head -n 1)
[ -n "$sdk_dir" ] && [ -d "$sdk_dir" ] || { printf 'cannot locate SDK\n' >&2; exit 1; }

(
    cd "$sdk_dir"
    ./scripts/feeds update -a
    printf '%s\n' "src-link circud $source_root/packaging/openwrt" >> feeds.conf
    ./scripts/feeds update circud
    ./scripts/feeds install -p base script-utils
    ./scripts/feeds install -p luci luci-base ucode-mod-socket
    ./scripts/feeds install -p circud luci-app-circud
    make defconfig
    sed -i \
        '/^# CONFIG_PACKAGE_luci-app-circud is not set$/d;/^CONFIG_PACKAGE_luci-app-circud=/d' \
        .config
    printf '%s\n' 'CONFIG_PACKAGE_luci-app-circud=m' >> .config
    make defconfig
    grep -q '^CONFIG_PACKAGE_luci-app-circud=m$' .config || {
        printf 'LuCI package was not selected\n' >&2
        exit 1
    }
    if ! make -j2 package/luci-app-circud/compile V=s LUCI_APP_CIRCUD_TEST=1; then
        printf '%s\n' '[circud-luci] parallel build failed; retrying serially' >&2
        make -j1 package/luci-app-circud/compile V=s LUCI_APP_CIRCUD_TEST=1
    fi
)

if [ "$channel" = ipk ]; then
    candidates=$(find "$sdk_dir/bin/packages" -type f -name 'luci-app-circud_*.ipk' -print | sort)
else
    candidates=$(find "$sdk_dir/bin/packages" -type f -name 'luci-app-circud*.apk' -print | sort)
fi
[ -n "$candidates" ] || { printf 'SDK produced no LuCI %s package\n' "$channel" >&2; exit 1; }
[ "$(printf '%s\n' "$candidates" | wc -l)" -eq 1 ] || {
    printf 'SDK produced an unexpected number of LuCI packages:\n%s\n' "$candidates" >&2
    exit 1
}
package=$candidates

verify_payload_root=$work/package-root
mkdir -p "$verify_payload_root"
if [ "$channel" = ipk ]; then
    extract_dir=$work/package-ipk
    mkdir -p "$extract_dir"
    package_abs=$(CDPATH= cd -- "$(dirname -- "$package")" && pwd)/$(basename -- "$package")
    if tar -tf "$package_abs" >/dev/null 2>&1; then
        tar -xf "$package_abs" -C "$extract_dir"
    elif ar t "$package_abs" >/dev/null 2>&1; then
        (cd "$extract_dir" && ar x "$package_abs")
    else
        printf 'IPK is neither an ar archive nor a supported tar archive\n' >&2
        exit 1
    fi
    data_archive=$(find "$extract_dir" -maxdepth 1 -type f \
        \( -name 'data.tar' -o -name 'data.tar.gz' -o -name 'data.tar.zst' \
           -o -name 'data.tar.xz' -o -name 'data.tar.bz2' \) \
        -print | head -n 1)
    [ -n "$data_archive" ] || { printf 'IPK has no supported data archive\n' >&2; exit 1; }
    case $data_archive in
        *.tar.zst) tar --zstd -xf "$data_archive" -C "$verify_payload_root" ;;
        *) tar -xf "$data_archive" -C "$verify_payload_root" ;;
    esac
else
    host_apk=$(find "$sdk_dir/staging_dir/host" "$sdk_dir/staging_dir/hostpkg" \
        \( -type f -o -type l \) -path '*/bin/apk' -print 2>/dev/null | head -n 1)
    [ -n "$host_apk" ] && [ -x "$host_apk" ] || {
        printf 'cannot find the SDK host apk tool\n' >&2
        exit 1
    }
    host_apk=$(CDPATH= cd -- "$(dirname -- "$host_apk")" && pwd)/$(basename -- "$host_apk")
    mkdir -p "$work/apk-keys"
    apk_key=
    for candidate in key-build.pub public-key.pem; do
        if [ -f "$sdk_dir/$candidate" ]; then
            apk_key=$candidate
            break
        fi
    done
    if [ -n "$apk_key" ]; then
        cp "$sdk_dir/$apk_key" "$work/apk-keys/$apk_key"
        if ! "$host_apk" --keys-dir "$work/apk-keys" extract --no-chown \
            --destination "$verify_payload_root" "$package"; then
            printf '%s\n' '[circud-luci] SDK public key did not verify the APK; extracting untrusted' >&2
            verify_payload_root=$work/package-root-untrusted
            mkdir -p "$verify_payload_root"
            "$host_apk" --allow-untrusted extract --no-chown \
                --destination "$verify_payload_root" "$package"
        fi
    else
        "$host_apk" --allow-untrusted extract --no-chown \
            --destination "$verify_payload_root" "$package"
    fi
fi

for path in \
    www/luci-static/resources/view/circud/overview.js \
    usr/share/luci/menu.d/luci-app-circud.json \
    usr/share/rpcd/acl.d/luci-app-circud.json \
    usr/share/rpcd/ucode/luci.circud \
    usr/share/ucode/luci/controller/circud.uc
do
    [ -e "$verify_payload_root/$path" ] || {
        printf 'LuCI package payload is missing %s\n' "$path" >&2
        exit 1
    }
done

mkdir -p "$output_dir"
output_name="circud-luci-${channel}.${channel}"
cp "$package" "$output_dir/$output_name"
{
    printf 'openwrt_channel=%s\n' "$channel"
    printf 'openwrt_release=%s\n' "$release"
    printf 'openwrt_arch_packages=%s\n' "$expected_arch"
    printf 'openwrt_target=%s\n' "$sdk_target"
    printf 'openwrt_subtarget=%s\n' "$sdk_subtarget"
    printf 'openwrt_sdk=%s\n' "$sdk_name"
    printf 'openwrt_sdk_sha256=%s\n' "$sdk_sha256"
} > "$output_dir/$output_name.buildinfo"

printf '[circud-luci] %s/%s\n' "$output_dir" "$output_name"
