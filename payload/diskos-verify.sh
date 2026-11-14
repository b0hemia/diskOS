#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 diskOS contributors
# diskOS update verifier - the security-critical core (POSIX sh, portable to the device's
# busybox+openssl 1.1.1). This is the reference logic that ports into S97 and the RO fexecve
# launcher. Hardening rules (per the 2026-08-31 red-team, UPDATE_DELIVERY_DESIGN.md section 9):
#   - verify the EXACT raw manifest bytes; never normalize/re-serialize.
#   - gate on `if openssl ...; then` (the exit STATUS), never on stdout or a stale $?.
#   - no eval/source/interpolation of manifest data into commands.
#   - line-based KEY=VALUE only (no JSON); reject duplicate/unknown/missing fields, control
#     chars, CRLF, oversize; strict field shapes (64-lc-hex sha, bounded decimals).
#   - quote every expansion; fixed absolute openssl path; operate on caller-staged copies.
# Two-level chain: baked RO root pubkey -> (root-signed) keyauthz authorizes a leaf pubkey ->
# (leaf-signed) update manifest authorizes an mq_ui binary. Anti-rollback epochs are baked floors.
#
# Usage: diskos-verify.sh <rootpub.pem> <MIN_KEY_EPOCH> <MIN_UI_EPOCH> <bundle_dir> <ui_binary>
#   bundle_dir must contain: keyauthz keyauthz.sig leafpub.pem update.manifest update.manifest.sig
# Exit 0 iff the whole chain verifies AND ui_binary matches the (leaf-signed) manifest.
# Exit 1 for established invalid content; 2 if verification cannot complete.
# Prints the verified fields, REJECT: <reason>, or RETRY: <reason>, respectively.

OPENSSL=/usr/bin/openssl
[ -x "$OPENSSL" ] || OPENSSL=openssl          # dev host fallback; device uses the absolute path
MAXBYTES=8192                                  # a manifest/authz is tiny; cap to bound parsing

reject() { echo "REJECT: $1" >&2; exit 1; }
incomplete() { echo "RETRY: $1" >&2; exit 2; }
for _tool in wc tr grep head sha256sum cut od "$OPENSSL"; do
    command -v "$_tool" >/dev/null 2>&1 || incomplete "missing verifier tool: $_tool"
done

# Capture the reader status before formatting; pipelines otherwise hide I/O/exec errors.
_size() { _v=$(wc -c < "$1") || incomplete "cannot size: $1"; printf '%s' "$_v" | tr -d ' '; }
_first() { head -n1 "$1" || incomplete "cannot read: $1"; }
_bytes() { _v=$(od "$@") || incomplete "cannot read ELF"; printf '%s' "$_v" | tr -d ' '; }

# --- generic strict readers -------------------------------------------------
# _sane_file <path> <maxbytes>: exists, regular, not a symlink, <= maxbytes. For ANY file
# (signatures + keys are BINARY DER/PEM, so NO text/NUL guard here).
_sane_file() {
    _f="$1"; _max="$2"
    [ -e "$_f" ] || reject "missing file: $_f"
    [ -L "$_f" ] && reject "refuse symlink: $_f"
    [ -f "$_f" ] || reject "not a regular file: $_f"
    _sz=$(_size "$_f") || exit $?
    case "$_sz" in ''|*[!0-9]*) incomplete "unsizable: $_f";; esac
    [ "$_sz" -le "$_max" ] || reject "oversize ($_sz > $_max): $_f"
    return 0
}
# _sane_text <path> <maxbytes>: as _sane_file, PLUS a strict charset guard - only for files we
# PARSE as text (the manifest + authz). Allowed bytes: newline (\012) and printable ASCII
# (\040-\176). This single check subsumes NUL, CR (CRLF injection), other control chars, and
# high-bit bytes - robustly, without relying on the shell preserving a CR/NUL through $()/case.
_sane_text() {
    _sane_file "$1" "$2"
    _hex=$(od -An -v -tx1 "$1") || incomplete "cannot read text: $1"
    # Hex preserves NUL bytes which command substitution would otherwise discard.
    for _byte in $_hex; do
        case "$_byte" in 0a|[2-6][0-9a-f]|7[0-9a-e]) ;;
            *) reject "non-ASCII/control byte in $1" ;;
        esac
    done
    return 0
}

# _field <manifest> <key>: echo the single value for KEY=VALUE. Rejects on missing OR duplicate.
# Value must contain no control chars (incl. CR); ^key=value$ anchored; no leading/trailing space.
_field() {
    _m="$1"; _k="$2"
    _n=$(grep -c "^$_k=" "$_m" 2>/dev/null); _nr=$?
    [ "$_nr" -le 1 ] || incomplete "cannot read fields: $_m"
    [ "$_n" = 1 ] || reject "field '$_k' count=$_n (want exactly 1)"
    _line=$(grep "^$_k=" "$_m") || incomplete "cannot read field: $_m"
    # CR (\r) or other control chars in the line -> reject (CRLF / injection guard)
    case "$_line" in *"$(printf '\r')"*) reject "CR in field '$_k'";; esac
    printf '%s' "${_line#"$_k="}"
}

# _check_schema <file> "<allowed+required keys>": strict KEY=VALUE structure of a signed text file, checked in the
# MAIN shell (a reject inside $(...) would only end the substitution). Line 1 is the magic (checked by the caller).
# Every later line must be `key=value` with a NON-EMPTY value and a key from the list; a key may appear once; every
# listed key is required. Blank lines, unknown keys, duplicates and missing keys all reject.
_check_schema() {
    _sf="$1"; _keys=" $2 "; _seen=" "; _ln=0
    while IFS= read -r _l || [ -n "$_l" ]; do
        _ln=$((_ln + 1))
        [ "$_ln" -eq 1 ] && continue
        case "$_l" in *=?*) ;; *) reject "$_sf line $_ln: not KEY=VALUE with a value";; esac
        _kk="${_l%%=*}"
        case "$_keys" in *" $_kk "*) ;; *) reject "$_sf: unknown field '$_kk'";; esac
        case "$_seen" in *" $_kk "*) reject "$_sf: duplicate field '$_kk'";; esac
        _seen="$_seen$_kk "
    done < "$_sf" || incomplete "cannot read schema: $_sf"
    for _kk in $2; do
        case "$_seen" in *" $_kk "*) ;; *) reject "$_sf: missing field '$_kk'";; esac
    done
    return 0
}

_is_hex64() { case "$1" in *[!0-9a-f]*) return 1;; esac; [ "${#1}" = 64 ]; }
_is_uint()  { case "$1" in ''|*[!0-9]*) return 1;; esac; [ "${#1}" -le 18 ]; }
_sha256() { _v=$(sha256sum "$1" 2>/dev/null) || incomplete "cannot hash: $1"; printf '%s' "${_v%% *}"; }

# _verify_sig <pubkey.pem> <sig> <signed_file>: 0 iff openssl asserts a valid P-256/SHA-256 sig.
# Success depends only on exit status; diagnostics classify failures conservatively.
_verify_sig() {
    if _sigout=$("$OPENSSL" dgst -sha256 -verify "$1" -signature "$2" "$3" 2>/dev/null); then
        return 0
    else
        _sigrc=$?
    fi
    # OpenSSL also returns failure for unreadable files and invocation/provider errors.
    # Only its explicit signature-mismatch result establishes invalid signed content.
    case "$_sigrc:$_sigout" in
        '1:Verification failure'|'1:Verification Failure') return 1 ;;
        *) incomplete "signature check could not complete (status $_sigrc)" ;;
    esac
}

# --- args -------------------------------------------------------------------
ROOTPUB="$1"; MIN_KEY_EPOCH="$2"; MIN_UI_EPOCH="$3"; BDIR="$4"; UIBIN="$5"
[ -n "$ROOTPUB" ] && [ -n "$MIN_KEY_EPOCH" ] && [ -n "$MIN_UI_EPOCH" ] && [ -n "$BDIR" ] && [ -n "$UIBIN" ] \
    || reject "usage: diskos-verify.sh rootpub MIN_KEY_EPOCH MIN_UI_EPOCH bundle_dir ui_binary"
_is_uint "$MIN_KEY_EPOCH" || reject "bad MIN_KEY_EPOCH"
_is_uint "$MIN_UI_EPOCH"  || reject "bad MIN_UI_EPOCH"
_sane_file "$ROOTPUB" 4096

AUTHZ="$BDIR/keyauthz"; AUTHZ_SIG="$BDIR/keyauthz.sig"; LEAFPUB="$BDIR/leafpub.pem"
MAN="$BDIR/update.manifest"; MAN_SIG="$BDIR/update.manifest.sig"

# --- 1. root authorizes the leaf key ---------------------------------------
_sane_text "$AUTHZ" "$MAXBYTES"; _sane_file "$AUTHZ_SIG" 4096; _sane_file "$LEAFPUB" 4096
_magic=$(_first "$AUTHZ") || exit $?
[ "$_magic" = "diskos-keyauthz-v1" ] || reject "authz: bad magic"
_verify_sig "$ROOTPUB" "$AUTHZ_SIG" "$AUTHZ" || reject "authz: root signature invalid"
_check_schema "$AUTHZ" "keyid keyepoch role pubkey_sha256"
KEYEPOCH=$(_field "$AUTHZ" keyepoch) || exit $?; _is_uint "$KEYEPOCH" || reject "authz: bad keyepoch"
[ "$KEYEPOCH" -ge "$MIN_KEY_EPOCH" ] || reject "authz: keyepoch $KEYEPOCH < floor $MIN_KEY_EPOCH (revoked key)"
ROLE=$(_field "$AUTHZ" role) || exit $?; [ "$ROLE" = "tier-a" ] || reject "authz: role '$ROLE' != tier-a"
# The leaf pubkey is bound by hash inside the root-signed authz (no PEM parsing in the signed doc).
PUBHASH=$(_field "$AUTHZ" pubkey_sha256) || exit $?; _is_hex64 "$PUBHASH" || reject "authz: bad pubkey_sha256"
_hash=$(_sha256 "$LEAFPUB") || exit $?
[ "$_hash" = "$PUBHASH" ] || reject "authz: leafpub hash mismatch (substituted key)"

# --- 2. leaf authorizes the mq_ui binary -----------------------------------
_sane_text "$MAN" "$MAXBYTES"; _sane_file "$MAN_SIG" 4096
_magic=$(_first "$MAN") || exit $?
[ "$_magic" = "diskos-update-v1" ] || reject "manifest: bad magic"
_verify_sig "$LEAFPUB" "$MAN_SIG" "$MAN" || reject "manifest: leaf signature invalid"
_check_schema "$MAN" "component epoch version size sha256 channel"
COMPONENT=$(_field "$MAN" component) || exit $?; [ "$COMPONENT" = "mq_ui" ] || reject "manifest: component '$COMPONENT' != mq_ui"
UEPOCH=$(_field "$MAN" epoch) || exit $?; _is_uint "$UEPOCH" || reject "manifest: bad epoch"
[ "$UEPOCH" -ge "$MIN_UI_EPOCH" ] || reject "manifest: epoch $UEPOCH < baked floor $MIN_UI_EPOCH (downgrade)"
MSIZE=$(_field "$MAN" size) || exit $?;   _is_uint "$MSIZE"  || reject "manifest: bad size"
MSHA=$(_field "$MAN" sha256) || exit $?;  _is_hex64 "$MSHA"  || reject "manifest: bad sha256"
VERSION=$(_field "$MAN" version) || exit $?

# --- 3. the binary matches the leaf-signed manifest ------------------------
_sane_file "$UIBIN" 33554432   # 32 MiB cap; mq_ui is ~3.4 MB
_usz=$(_size "$UIBIN") || exit $?
[ "$_usz" = "$MSIZE" ] || reject "binary size $_usz != manifest $MSIZE"
_hash=$(_sha256 "$UIBIN") || exit $?
[ "$_hash" = "$MSHA" ] || reject "binary sha256 mismatch"
# ELF32-LE MIPS sanity (defence in depth; matches S97's existing checks)
_elf=$(_bytes -An -tx1 -N4 "$UIBIN") || exit $?
[ "$_elf" = "7f454c46" ] || reject "binary: not ELF"
_elf=$(_bytes -An -tx1 -j4 -N2 "$UIBIN") || exit $?
[ "$_elf" = "0101" ]  || reject "binary: not ELF32-LE"
_elf=$(_bytes -An -tx1 -j18 -N2 "$UIBIN") || exit $?
[ "$_elf" = "0800" ] || reject "binary: not MIPS"

echo "OK component=$COMPONENT version=$VERSION epoch=$UEPOCH keyepoch=$KEYEPOCH"
exit 0
