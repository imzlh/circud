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
    [ -f feeds.conf ] || cp feeds.conf.default feeds.conf
    sed -i '/^src-link circud /d' feeds.conf
    printf '%s\n' "src-link circud $source_root/packaging/openwrt" >> feeds.conf
    ./scripts/feeds update base packages luci circud
    # scripts/feeds resolves package names to their source recipe, so
    # script-utils installs the util-linux directory and ucode-mod-socket
    # installs the ucode directory.
    ./scripts/feeds install -p base script-utils zlib ca-certificates
    ./scripts/feeds install -p packages boost-json minizip \
        libsqlite3 libffi libexpat libnghttp2
    ./scripts/feeds install -p luci luci-base ucode-mod-socket
    ./scripts/feeds install -p circud circud luci-app-circud
    # Resolve each selected package through the feed index instead of
    # assuming a package-category layout. Snapshot and release feeds differ
    # (libffi was repository base in one SDK and packages in another).
    for package_name in \
        script-utils zlib ca-certificates boost-json minizip libsqlite3 \
        libffi libexpat libnghttp2 luci-base ucode-mod-socket \
        circud luci-app-circud
    do
        source_makefile=$(awk -v name="$package_name" '
            /^Package: / { package_name = substr($0, 10) }
            /^Source-Makefile: / && package_name == name {
                print substr($0, 18)
                exit
            }
        ' feeds/*.tmp/.packageinfo feeds/*.tmp/info/.packageinfo-* 2>/dev/null)
        if [ -z "$source_makefile" ] || [ ! -f "$source_makefile" ]; then
            printf 'feeds install did not provide a recipe for %s\n' \
                "$package_name" >&2
            exit 1
        fi
    done
    [ -f feeds/packages/net/curl/Makefile ] || {
        printf 'packages feed does not contain the curl recipe\n' >&2
        exit 1
    }
    openssl_recipe=
    for candidate in feeds/base/package/libs/openssl feeds/base/libs/openssl; do
        if [ -f "$candidate/Makefile" ]; then
            [ -z "$openssl_recipe" ] || {
                printf 'base feed contains ambiguous OpenSSL recipes\n' >&2
                exit 1
            }
            openssl_recipe=$candidate
        fi
    done
    [ -n "$openssl_recipe" ] || {
        printf 'base feed lacks a supported OpenSSL recipe layout\n' >&2
        exit 1
    }
    sh "$source_root/scripts/normalize-openwrt-depends.sh" \
        feeds/packages/net/curl/Makefile "$openssl_recipe/Makefile"
    mkdir -p package/feeds/base package/feeds/packages
    openssl_link=package/feeds/base/openssl
    if [ -L "$openssl_link" ]; then
        rm -f -- "$openssl_link"
    elif [ -e "$openssl_link" ]; then
        printf 'refusing to replace non-symlink %s\n' "$openssl_link" >&2
        exit 1
    fi
    ln -s "../../../$openssl_recipe" "$openssl_link"
    curl_link=package/feeds/packages/curl
    if [ -L "$curl_link" ]; then
        rm -f -- "$curl_link"
    elif [ -e "$curl_link" ]; then
        printf 'refusing to replace non-symlink %s\n' "$curl_link" >&2
        exit 1
    fi
    ln -s ../../../feeds/packages/net/curl "$curl_link"

    sdk_defconfig() {
        phase=$1
        defconfig_log=$work/defconfig-luci-$phase.log
        if make defconfig > "$defconfig_log" 2>&1; then
            :
        else
            status=$?
            cat "$defconfig_log" >&2
            printf 'LuCI SDK defconfig failed during %s (exit %s)\n' \
                "$phase" "$status" >&2
            exit 1
        fi
        cat "$defconfig_log"
        if grep -Fqi 'recursive dependency detected' "$defconfig_log"; then
            printf 'LuCI SDK defconfig reported a recursive dependency during %s\n' \
                "$phase" >&2
            exit 1
        fi
    }

    touch .config
    for config_name in ALL ALL_NONSHARED ALL_KMODS; do
        sed -i \
            "/^CONFIG_${config_name}=/d;/^# CONFIG_${config_name} is not set$/d" \
            .config
    done
    printf '%s\n' \
        '# CONFIG_ALL is not set' \
        '# CONFIG_ALL_NONSHARED is not set' \
        '# CONFIG_ALL_KMODS is not set' >> .config
    sdk_defconfig initial

    for config_name in \
        PACKAGE_circud PACKAGE_luci-app-circud PACKAGE_boost PACKAGE_boost-json \
        PACKAGE_libcurl PACKAGE_libopenssl PACKAGE_libnghttp2 PACKAGE_zlib \
        LIBCURL_WOLFSSL LIBCURL_GNUTLS LIBCURL_OPENSSL \
        LIBCURL_MBEDTLS LIBCURL_NOSSL LIBCURL_ZLIB LIBCURL_ZSTD \
        LIBCURL_THREADED_RESOLVER LIBCURL_LDAP LIBCURL_LIBIDN2 \
        LIBCURL_SSH2 LIBCURL_HTTP2 LIBCURL_HTTP3 LIBCURL_KERBEROS_AUTH \
        PACKAGE_libopenssl-afalg PACKAGE_libopenssl-devcrypto \
        PACKAGE_libopenssl-padlock \
        OPENSSL_WITH_COMPRESSION OPENSSL_ENGINE_BUILTIN_AFALG \
        OPENSSL_ENGINE_BUILTIN_DEVCRYPTO OPENSSL_ENGINE_BUILTIN_PADLOCK
    do
        sed -i \
            "/^CONFIG_${config_name}=/d;/^# CONFIG_${config_name} is not set$/d" \
            .config
    done
    printf '%s\n' \
        'CONFIG_PACKAGE_circud=m' \
        'CONFIG_PACKAGE_luci-app-circud=m' \
        'CONFIG_PACKAGE_boost=m' \
        'CONFIG_PACKAGE_boost-json=m' \
        'CONFIG_PACKAGE_libcurl=m' \
        'CONFIG_PACKAGE_libopenssl=m' \
        'CONFIG_PACKAGE_libnghttp2=m' \
        'CONFIG_PACKAGE_zlib=m' \
        'CONFIG_LIBCURL_OPENSSL=y' \
        'CONFIG_LIBCURL_HTTP2=y' \
        'CONFIG_LIBCURL_ZLIB=y' \
        '# CONFIG_LIBCURL_WOLFSSL is not set' \
        '# CONFIG_LIBCURL_GNUTLS is not set' \
        '# CONFIG_LIBCURL_MBEDTLS is not set' \
        '# CONFIG_LIBCURL_NOSSL is not set' \
        '# CONFIG_LIBCURL_ZSTD is not set' \
        '# CONFIG_LIBCURL_THREADED_RESOLVER is not set' \
        '# CONFIG_LIBCURL_LDAP is not set' \
        '# CONFIG_LIBCURL_LIBIDN2 is not set' \
        '# CONFIG_LIBCURL_SSH2 is not set' \
        '# CONFIG_LIBCURL_HTTP3 is not set' \
        '# CONFIG_LIBCURL_KERBEROS_AUTH is not set' \
        '# CONFIG_PACKAGE_libopenssl-afalg is not set' \
        '# CONFIG_PACKAGE_libopenssl-devcrypto is not set' \
        '# CONFIG_PACKAGE_libopenssl-padlock is not set' \
        '# CONFIG_OPENSSL_WITH_COMPRESSION is not set' \
        '# CONFIG_OPENSSL_ENGINE_BUILTIN_AFALG is not set' \
        '# CONFIG_OPENSSL_ENGINE_BUILTIN_DEVCRYPTO is not set' \
        '# CONFIG_OPENSSL_ENGINE_BUILTIN_PADLOCK is not set' >> .config
    sdk_defconfig circud
    for package_name in \
        luci-app-circud circud boost boost-json libcurl libopenssl libnghttp2 \
        zlib libsqlite3 minizip libexpat libffi
    do
        grep -Eq "^CONFIG_PACKAGE_${package_name}=[my]$" .config || {
            printf 'LuCI SDK defconfig did not select %s\n' "$package_name" >&2
            exit 1
        }
    done
    for config_name in LIBCURL_OPENSSL LIBCURL_HTTP2 LIBCURL_ZLIB; do
        grep -q "^CONFIG_${config_name}=y$" .config || {
            printf 'LuCI SDK defconfig did not retain CONFIG_%s=y\n' \
                "$config_name" >&2
            exit 1
        }
    done
    grep -q '^CONFIG_PACKAGE_luci-app-circud=m$' .config || {
        printf 'LuCI package was not selected\n' >&2
        exit 1
    }
    if ! make -j2 package/circud/compile package/luci-app-circud/compile \
        V=s LUCI_APP_CIRCUD_TEST=1; then
        printf '%s\n' '[circud-luci] parallel build failed; retrying serially' >&2
        make -j1 package/circud/compile package/luci-app-circud/compile \
            V=s LUCI_APP_CIRCUD_TEST=1
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
