#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
    printf '%s\n' \
        'Usage: normalize-openwrt-depends.sh CURL_MAKEFILE OPENSSL_MAKEFILE' >&2
    exit 2
fi

curl_makefile=$1
openssl_makefile=$2
[ -f "$curl_makefile" ] || {
    printf 'error: curl recipe not found: %s\n' "$curl_makefile" >&2
    exit 1
}
[ -f "$openssl_makefile" ] || {
    printf 'error: OpenSSL recipe not found: %s\n' "$openssl_makefile" >&2
    exit 1
}

count_token() {
    awk -v wanted="$2" '
        {
            for (field = 1; field <= NF; field++) {
                token = $field
                sub(/^[[:alnum:]_]+[+:?]?=/, "", token)
                if (token == wanted) count++
            }
        }
        END { print count + 0 }
    ' "$1"
}

reject_conditional_tokens() {
    recipe=$1
    label=$2
    prefix=$3
    awk -v label="$label" -v prefix="$prefix" '
        {
            for (field = 1; field <= NF; field++) {
                token = $field
                sub(/^[[:alnum:]_]+[+:?]?=/, "", token)
                if (index(token, "+" prefix "_") == 1 && index(token, ":") > 0) {
                    print "error: conditional " label \
                        " package dependency survived: " token > "/dev/stderr"
                    bad = 1
                }
            }
        }
        END { exit bad }
    ' "$recipe"
}

# A recipe may already have been normalized when a build is retried. Accept
# either the original conditional spelling or the single unconditional form,
# but reject duplicates and mixed states.
for token in +LIBCURL_OPENSSL:libopenssl +libopenssl; do
    count=$(count_token "$curl_makefile" "$token")
    [ "$count" -le 1 ] || {
        printf 'error: curl OpenSSL dependency %s occurs %s times\n' \
            "$token" "$count" >&2
        exit 1
    }
done
[ "$(( $(count_token "$curl_makefile" +LIBCURL_OPENSSL:libopenssl) + \
      $(count_token "$curl_makefile" +libopenssl) ))" -eq 1 ] || {
    printf 'error: curl must depend on OpenSSL exactly once\n' >&2
    exit 1
}
for token in +LIBCURL_ZLIB:zlib +zlib; do
    count=$(count_token "$curl_makefile" "$token")
    [ "$count" -le 1 ] || {
        printf 'error: curl zlib dependency %s occurs %s times\n' \
            "$token" "$count" >&2
        exit 1
    }
done
[ "$(( $(count_token "$curl_makefile" +LIBCURL_ZLIB:zlib) + \
      $(count_token "$curl_makefile" +zlib) ))" -eq 1 ] || {
    printf 'error: curl must depend on zlib exactly once\n' >&2
    exit 1
}
http2_count=$(( $(count_token "$curl_makefile" +LIBCURL_NGHTTP2:libnghttp2) + \
                 $(count_token "$curl_makefile" +LIBCURL_HTTP2:libnghttp2) + \
                 $(count_token "$curl_makefile" +libnghttp2) ))
[ "$http2_count" -eq 1 ] || {
    printf 'error: curl must depend on HTTP/2 exactly once (found %s)\n' \
        "$http2_count" >&2
    exit 1
}
[ "$(count_token "$curl_makefile" +ca-bundle)" -eq 1 ] || {
    printf 'error: curl recipe must contain +ca-bundle exactly once\n' >&2
    exit 1
}

for token in \
    +LIBCURL_WOLFSSL:libwolfssl \
    +LIBCURL_GNUTLS:libgnutls \
    +LIBCURL_MBEDTLS:libmbedtls \
    +LIBCURL_ZSTD:libzstd \
    +LIBCURL_THREADED_RESOLVER:libpthread \
    +LIBCURL_LDAP:libopenldap \
    +LIBCURL_LIBIDN2:libidn2 \
    +LIBCURL_SSH2:libssh2 \
    +LIBCURL_HTTP3:libnghttp3 \
    +LIBCURL_HTTP3:libngtcp2 \
    +LIBCURL_NGHTTP3:libnghttp3 \
    +LIBCURL_NGTCP2:libngtcp2 \
    +LIBCURL_KERBEROS_AUTH:krb5-libs
do
    count=$(count_token "$curl_makefile" "$token")
    [ "$count" -le 1 ] || {
        printf 'error: disabled curl recipe token %s occurs %s times\n' \
            "$token" "$count" >&2
        exit 1
    }
done

sed -i \
    -e 's|+LIBCURL_OPENSSL:libopenssl|+libopenssl|g' \
    -e 's|+LIBCURL_ZLIB:zlib|+zlib|g' \
    -e 's|+LIBCURL_NGHTTP2:libnghttp2|+libnghttp2|g' \
    -e 's|+LIBCURL_HTTP2:libnghttp2|+libnghttp2|g' \
    -e 's|+LIBCURL_WOLFSSL:libwolfssl||g' \
    -e 's|+LIBCURL_GNUTLS:libgnutls||g' \
    -e 's|+LIBCURL_MBEDTLS:libmbedtls||g' \
    -e 's|+LIBCURL_ZSTD:libzstd||g' \
    -e 's|+LIBCURL_THREADED_RESOLVER:libpthread||g' \
    -e 's|+LIBCURL_LDAP:libopenldap||g' \
    -e 's|+LIBCURL_LIBIDN2:libidn2||g' \
    -e 's|+LIBCURL_SSH2:libssh2||g' \
    -e 's|+LIBCURL_HTTP3:libnghttp3||g' \
    -e 's|+LIBCURL_HTTP3:libngtcp2||g' \
    -e 's|+LIBCURL_NGHTTP3:libnghttp3||g' \
    -e 's|+LIBCURL_NGTCP2:libngtcp2||g' \
    -e 's|+LIBCURL_KERBEROS_AUTH:krb5-libs||g' \
    "$curl_makefile"

for token in +libopenssl +zlib +libnghttp2 +ca-bundle; do
    count=$(count_token "$curl_makefile" "$token")
    [ "$count" -eq 1 ] || {
        printf 'error: normalized curl recipe token %s occurs %s times, expected 1\n' \
            "$token" "$count" >&2
        exit 1
    }
done
reject_conditional_tokens "$curl_makefile" curl LIBCURL

for token in \
    +OPENSSL_WITH_COMPRESSION:zlib \
    +OPENSSL_ENGINE_BUILTIN_AFALG:kmod-crypto-user \
    +OPENSSL_ENGINE_BUILTIN_DEVCRYPTO:kmod-cryptodev \
    +OPENSSL_ENGINE_BUILTIN_PADLOCK:kmod-crypto-hw-padlock
do
    count=$(count_token "$openssl_makefile" "$token")
    [ "$count" -le 1 ] || {
        printf 'error: disabled OpenSSL recipe token %s occurs %s times\n' \
            "$token" "$count" >&2
        exit 1
    }
done

sed -i \
    -e 's|+OPENSSL_WITH_COMPRESSION:zlib||g' \
    -e 's|+OPENSSL_ENGINE_BUILTIN_AFALG:kmod-crypto-user||g' \
    -e 's|+OPENSSL_ENGINE_BUILTIN_DEVCRYPTO:kmod-cryptodev||g' \
    -e 's|+OPENSSL_ENGINE_BUILTIN_PADLOCK:kmod-crypto-hw-padlock||g' \
    "$openssl_makefile"

reject_conditional_tokens "$openssl_makefile" OpenSSL OPENSSL
