#!/bin/sh
set -eu

source_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
channel=${1:?Usage: openwrt-matrix.sh ipk|apk}
targets_file=$source_root/config/openwrt-targets.tsv

case $channel in
    ipk|apk) ;;
    *) printf 'unknown OpenWrt channel: %s\n' "$channel" >&2; exit 2 ;;
esac

[ -r "$targets_file" ] || {
    printf 'target list is not readable: %s\n' "$targets_file" >&2
    exit 1
}

jq -Rnc --arg channel "$channel" '
    [
        inputs
        | select(length > 0 and (startswith("#") | not))
        | split("\t")
        | select(length == 4)
        | . as $row
        | ($row[0] | split("+"))[]
        | select(. == $ARGS.named.channel)
        | {
            channel: .,
            arch: $row[3],
            target: $row[1],
            subtarget: $row[2]
          }
    ]' "$targets_file"
