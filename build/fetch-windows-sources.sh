#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
# Fetch release source assets outside the checkout; retain failed downloads.
set -euo pipefail
runtime_source=msys2-runtime-3.6.10-6.src.tar.zst
runtime_sha256=1d06b1482199ca8adaea770c4b596ec3adb2749345ef81a5d111cb792f0f1643
runtime_url=https://repo.msys2.org/msys/sources
verify_runtime_source() {
    local actual
    actual=$(sha256sum -- "$1") || return 1
    [[ ${actual%% *} == "$runtime_sha256" ]] || {
        echo 'Runtime source SHA256 does not match the release pin' >&2
        return 1
    }
}
verify_runtime_status() {
    # A good signature by an unknown/untrusted key does not authenticate MSYS2.
    awk '
        $1 == "[GNUPG:]" && $2 == "VALIDSIG" { valid++ }
        $1 == "[GNUPG:]" && ($2 == "TRUST_FULLY" || $2 == "TRUST_ULTIMATE") { trusted++ }
        $1 == "[GNUPG:]" && $2 ~ /^(BADSIG|ERRSIG|EXPSIG|EXPKEYSIG|REVKEYSIG|NO_PUBKEY)$/ { bad++ }
        END { exit !(valid == 1 && trusted == 1 && !bad) }
    ' <<< "$1" || {
        echo 'Runtime source signature is not valid and fully trusted' >&2
        return 1
    }
}
verify_runtime_signature() {
    local status
    [[ -n ${MSYS2_GNUPGHOME:-} && -d $MSYS2_GNUPGHOME ]] || {
        echo 'Set MSYS2_GNUPGHOME to an independently trusted MSYS2 pacman keyring' >&2
        return 1
    }
    [[ -s $1 && -s $1.sig ]] || return 1
    # Use the build host's system GnuPG installation, never PATH or a shell
    # function. Clear inherited environment and ignore all gpg.conf options.
    status=$(command /usr/bin/env -i PATH=/usr/bin:/bin LC_ALL=C \
        /usr/bin/gpg --no-options --batch --no-auto-key-retrieve \
        --homedir "$MSYS2_GNUPGHOME" --status-fd=1 --verify "$1.sig" "$1") || return 1
    verify_runtime_status "$status" || return 1
    printf '%s\n' "$status"
}
check_output_dir() {
    local target
    target=$(realpath -m -- "$1") || return 1
    case "$target/" in
        "$root/"*) echo 'Source assets must stay outside the checkout' >&2; return 1 ;;
    esac
}
mode=fetch
if [[ ${1:-} == --verify ]]; then mode=verify; shift; fi
[[ $# -le 1 && ( $mode == fetch || $# == 1 ) ]] || {
    echo 'Usage: bash build/fetch-windows-sources.sh [OUTPUT_DIR | --verify OUTPUT_DIR]' >&2
    exit 1
}
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
# A fixed external temp root avoids a repository-local TMPDIR default.
out=${1:-}
if [[ -z $out ]]; then out=$(mktemp -d /tmp/diskos-windows-sources.XXXXXXXX); fi
if [[ $mode == fetch ]]; then check_output_dir "$out"; fi
if [[ $mode == verify ]]; then
    verify_runtime_source "$out/$runtime_source"
    [[ -s $out/$runtime_source.sig ]]
else
    mkdir -p -- "$out"
    if [[ ! -e $out/$runtime_source ]]; then
        archive=$(mktemp "$out/.runtime-source.XXXXXXXX")
        curl --fail --location --proto '=https' --proto-redir '=https' \
            --output "$archive" "$runtime_url/$runtime_source"
        verify_runtime_source "$archive"
        mv -n -- "$archive" "$out/$runtime_source"
    fi
    verify_runtime_source "$out/$runtime_source"
    if [[ ! -e $out/$runtime_source.sig ]]; then
        signature=$(mktemp "$out/.runtime-signature.XXXXXXXX")
        curl --fail --location --proto '=https' --proto-redir '=https' \
            --output "$signature" "$runtime_url/$runtime_source.sig"
        [[ -s $signature ]]
        mv -n -- "$signature" "$out/$runtime_source.sig"
    fi
    [[ -s $out/$runtime_source.sig ]]
fi
verify_runtime_signature "$out/$runtime_source"
printf 'PASS: pinned source and authenticated detached signature in %s\n' "$out"
