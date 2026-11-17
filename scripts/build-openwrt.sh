#!/bin/sh
set -eu

channel=${1:?Usage: build-openwrt.sh ipk|apk ARCH SDK_TARGET SDK_SUBTARGET OUTPUT_DIR}
expected_arch=${2:?Missing package architecture}
sdk_target=${3:?Missing SDK target}
sdk_subtarget=${4:?Missing SDK subtarget}
output_dir=${5:?Missing output directory}

source_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
matrix_file=$source_root/config/openwrt-targets.tsv

case $channel in ipk|apk) ;; *) printf 'invalid package channel: %s\n' "$channel" >&2; exit 2 ;; esac
for value in "$expected_arch" "$sdk_target" "$sdk_subtarget"; do
    case $value in ''|*[!A-Za-z0-9_.+-]*) printf 'invalid matrix value: %s\n' "$value" >&2; exit 2 ;; esac
done

awk -F '\t' -v channel="$channel" -v arch="$expected_arch" \
    -v target="$sdk_target" -v subtarget="$sdk_subtarget" '
    !/^#/ && NF {
        split($1, formats, "+")
        for (i = 1; i <= length(formats); i++) {
            if (formats[i] == channel && $2 == target &&
                $3 == subtarget && $4 == arch) found++
        }
    }
    END { exit found != 1 }
' "$matrix_file" || {
    printf 'target is not declared exactly once by %s: %s %s/%s %s\n' \
        "$matrix_file" "$channel" "$sdk_target" "$sdk_subtarget" \
        "$expected_arch" >&2
    exit 1
}

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
    release=$(printf '%s\n' "$listing" \
        | sed -n 's/.*href="\(24\.10\.[0-9][0-9]*\)\/".*/\1/p' \
        | sort -t. -k3,3n | tail -n 1)
    [ -n "$release" ] || {
        printf 'no published OpenWrt 24.10 release found\n' >&2
        exit 1
    }
fi

work=$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/circud-openwrt.XXXXXX")
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
    printf 'SDK checksum entry is missing or ambiguous: %s\n' "$sdk_name" >&2
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
sdk_dir=$(find "$work" -mindepth 1 -maxdepth 1 -type d -name 'openwrt-sdk-*' -print | head -n 1)
[ -n "$sdk_dir" ] && [ -d "$sdk_dir" ] || { printf 'cannot locate extracted SDK\n' >&2; exit 1; }

[ -x "$sdk_dir/scripts/feeds" ] || { printf 'SDK lacks scripts/feeds\n' >&2; exit 1; }
if ! command -v ar >/dev/null 2>&1; then
    printf 'ar is required to validate the IPK payload\n' >&2
    exit 1
fi

[ -f "$source_root/.ci/circu.js/src/message_clone.c" ] || {
    printf 'input checkout lacks circu.js message-clone ABI\n' >&2
    exit 1
}
[ -f "$source_root/.ci/llhttp/CMakeLists.txt" ] || {
    printf 'input checkout lacks llhttp\n' >&2
    exit 1
}

(
    cd "$sdk_dir"
    # Fresh SDK archives normally contain only feeds.conf.default. Create the
    # working copy before adding the local feed; appending to a missing file
    # would otherwise replace the SDK's pinned base and packages sources.
    [ -f feeds.conf ] || cp feeds.conf.default feeds.conf
    sed -i '/^src-link circud /d' feeds.conf
    printf '%s\n' "src-link circud $source_root/packaging/openwrt" >> feeds.conf
    ./scripts/feeds update base packages circud
    # Install leaf dependencies before circud so feeds metadata can resolve its
    # complete dependency set. Do not ask feeds to install curl: that follows
    # every conditional recipe edge (LDAP, SSH, HTTP/3, alternate TLS backends)
    # and builds unrelated packages. libcurl is linked directly below after
    # installing only its OpenSSL, zlib and HTTP/2 closure.
    ./scripts/feeds install -p base zlib ca-certificates
    ./scripts/feeds install -p packages boost-json minizip \
        libsqlite3 libffi libexpat libnghttp2
    ./scripts/feeds install -p circud circud
    for recipe in \
        package/feeds/base/zlib \
        package/feeds/base/ca-certificates \
        package/feeds/packages/boost \
        package/feeds/packages/sqlite3 \
        package/feeds/packages/minizip \
        package/feeds/packages/libffi \
        package/feeds/packages/expat \
        package/feeds/packages/nghttp2 \
        package/feeds/circud/circud
    do
        [ -f "$recipe/Makefile" ] || {
            printf 'feeds install did not produce recipe %s\n' "$recipe" >&2
            exit 1
        }
    done
)

cd "$sdk_dir"
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
    defconfig_log=$work/defconfig-$phase.log
    if make defconfig > "$defconfig_log" 2>&1; then
        :
    else
        status=$?
        cat "$defconfig_log" >&2
        printf 'SDK defconfig failed during %s (exit %s)\n' \
            "$phase" "$status" >&2
        exit 1
    fi
    cat "$defconfig_log"
    if grep -Fqi 'recursive dependency detected' "$defconfig_log"; then
        printf 'SDK defconfig reported a recursive dependency during %s\n' \
            "$phase" >&2
        exit 1
    fi
}

# Published SDKs default ALL to y, which expands to every package. Clear the
# three buildbot switches before the first defconfig; once expanded, their
# blanket PACKAGE_*=m selections are not reliably undone.
touch "$sdk_dir/.config"
for config_name in ALL ALL_NONSHARED ALL_KMODS; do
    sed -i \
        "/^CONFIG_${config_name}=/d;/^# CONFIG_${config_name} is not set$/d" \
        "$sdk_dir/.config"
done
printf '%s\n' \
    '# CONFIG_ALL is not set' \
    '# CONFIG_ALL_NONSHARED is not set' \
    '# CONFIG_ALL_KMODS is not set' >> "$sdk_dir/.config"
sdk_defconfig initial

for config_name in \
    PACKAGE_circud \
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
        "$sdk_dir/.config"
done
printf '%s\n' \
    'CONFIG_PACKAGE_circud=m' \
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
    '# CONFIG_OPENSSL_ENGINE_BUILTIN_PADLOCK is not set' >> "$sdk_dir/.config"
sdk_defconfig circud
for package_name in \
    circud libcurl libopenssl libnghttp2 zlib boost-json libsqlite3 minizip \
    libexpat libffi
do
    grep -Eq "^CONFIG_PACKAGE_${package_name}=[my]$" "$sdk_dir/.config" || {
        printf 'SDK defconfig did not select %s\n' "$package_name" >&2
        exit 1
    }
done
for config_name in LIBCURL_OPENSSL LIBCURL_HTTP2 LIBCURL_ZLIB; do
    grep -q "^CONFIG_${config_name}=y$" "$sdk_dir/.config" || {
        printf 'SDK defconfig did not retain CONFIG_%s=y\n' "$config_name" >&2
        exit 1
    }
done
for config_name in \
    LIBCURL_WOLFSSL LIBCURL_GNUTLS LIBCURL_MBEDTLS LIBCURL_NOSSL \
    LIBCURL_ZSTD LIBCURL_THREADED_RESOLVER LIBCURL_LDAP \
    LIBCURL_LIBIDN2 LIBCURL_SSH2 LIBCURL_HTTP3 LIBCURL_KERBEROS_AUTH \
    PACKAGE_libopenssl-afalg PACKAGE_libopenssl-devcrypto \
    PACKAGE_libopenssl-padlock OPENSSL_WITH_COMPRESSION \
    OPENSSL_ENGINE_BUILTIN_AFALG OPENSSL_ENGINE_BUILTIN_DEVCRYPTO \
    OPENSSL_ENGINE_BUILTIN_PADLOCK
do
    if grep -q "^CONFIG_${config_name}=" "$sdk_dir/.config"; then
        printf 'SDK defconfig selected disabled CONFIG_%s\n' \
            "$config_name" >&2
        exit 1
    fi
done

actual_arch=$(sed -n 's/^CONFIG_TARGET_ARCH_PACKAGES=//p' "$sdk_dir/.config" | tr -d '"' | head -n 1)
[ -n "$actual_arch" ] || { printf 'SDK has no CONFIG_TARGET_ARCH_PACKAGES\n' >&2; exit 1; }
[ "$actual_arch" = "$expected_arch" ] || {
    printf 'package architecture mismatch: requested %s, SDK reports %s\n' \
        "$expected_arch" "$actual_arch" >&2
    exit 1
}
grep -q '^CONFIG_PACKAGE_circud=m$' "$sdk_dir/.config" || { printf 'circud was not selected\n' >&2; exit 1; }

if ! make -j2 package/circud/compile V=s LUCI_APP_CIRCUD_TEST=1; then
    printf '%s\n' '[circud-openwrt] parallel build failed; retrying serially' >&2
    make -j1 package/circud/compile V=s LUCI_APP_CIRCUD_TEST=1
fi

candidates=$(find "$sdk_dir/bin/packages" -type f \
    \( -name 'circud_*.ipk' -o -name 'circud-*.apk' \) ! -name '*-dbg*' -print | sort)
[ -n "$candidates" ] || { printf 'SDK produced no circud %s package\n' "$channel" >&2; exit 1; }
[ "$(printf '%s\n' "$candidates" | wc -l)" -eq 1 ] || {
    printf 'SDK produced an unexpected number of circud packages:\n%s\n' "$candidates" >&2
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
            printf '%s\n' '[circud-openwrt] SDK public key did not verify the APK; extracting untrusted' >&2
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
    usr/bin/circud \
    etc/init.d/circud \
    etc/circud/config.json \
    usr/share/circud/circud.example.json \
    usr/share/circud/public/index.html
do
    [ -e "$verify_payload_root/$path" ] || {
        printf 'package payload is missing %s\n' "$path" >&2
        exit 1
    }
done
[ -x "$verify_payload_root/usr/bin/circud" ] || {
    printf 'package payload has non-executable usr/bin/circud\n' >&2
    exit 1
}

mkdir -p "$output_dir"
output_name="circud-openwrt-${channel}-${expected_arch}-${sdk_target}-${sdk_subtarget}.${channel}"
cp "$package" "$output_dir/$output_name"
{
    printf 'circud_version=%s\n' "$(sed -n 's/^PKG_VERSION:=//p' "$source_root/packaging/openwrt/circud/Makefile" | head -n 1)"
    printf 'openwrt_channel=%s\n' "$channel"
    printf 'openwrt_release=%s\n' "$release"
    printf 'openwrt_arch_packages=%s\n' "$actual_arch"
    printf 'openwrt_target=%s\n' "$sdk_target"
    printf 'openwrt_subtarget=%s\n' "$sdk_subtarget"
    printf 'openwrt_sdk=%s\n' "$sdk_name"
    printf 'openwrt_sdk_sha256=%s\n' "$sdk_sha256"
    printf 'circu_js_commit=%s\n' "$(git -C "$source_root/.ci/circu.js" rev-parse HEAD)"
} > "$output_dir/$output_name.buildinfo"

printf '[circud-openwrt] %s/%s (%s, %s/%s)\n' \
    "$output_dir" "$output_name" "$actual_arch" "$sdk_target" "$sdk_subtarget"
