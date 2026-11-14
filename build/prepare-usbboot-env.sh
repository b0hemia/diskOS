#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
# Network-enabled preparation, separate from the offline build.
# Requires rootless Podman, curl, gpgv, and the host ubuntu-keyring package.
set -eu
export LC_ALL=C TZ=UTC
umask 022
fail() { echo "FAIL: $*" >&2; exit 1; }
snapshot= base_url= rootfs= out=
while [ "$#" -gt 0 ]; do
    [ "$#" -ge 2 ] || fail 'expected --snapshot TIMESTAMP --base-url URL --rootfs NAME --out DIR'
    case "$1" in
        --snapshot) snapshot=$2;; --base-url) base_url=$2;;
        --rootfs) rootfs=$2;; --out) out=$2;; *) fail "unknown argument: $1";;
    esac
    shift 2
done
printf '%s\n' "$snapshot" | grep -Eq '^[0-9]{8}T[0-9]{6}Z$' || fail 'snapshot must be YYYYMMDDTHHMMSSZ'
printf '%s\n' "$base_url" | grep -Eq '^https://cdimage\.ubuntu\.com/ubuntu-base/releases/24\.04[^ ]*/release/?$' \
    || fail 'expected an Ubuntu Base 24.04 release HTTPS directory'
printf '%s\n' "$rootfs" | grep -Eq '^ubuntu-base-24\.04(\.[0-9]+)?-base-amd64\.tar\.gz$' \
    || fail 'expected a dated Noble amd64 rootfs filename'
[ -n "$out" ] || fail 'output directory required'
[ "$(id -u)" != 0 ] || fail 'run as an ordinary user with rootless Podman'
[ ! -e "$out" ] && [ ! -L "$out" ] || fail 'refusing to overwrite evidence directory'
mkdir -- "$out"
out=$(CDPATH= cd -- "$out" && pwd)
cd "$out"
printf '%s\n' "$snapshot" > snapshot.txt
printf '%s/%s\n' "${base_url%/}" "$rootfs" > rootfs-url.txt
keyring=/usr/share/keyrings/ubuntu-archive-keyring.gpg
cd_key=843938DF228D22F7B3742BC0D94AA3F0EFE21092
cp "$keyring" ubuntu-archive-keyring.gpg
printf '%s\n' "$cd_key" > rootfs-signing-key.txt
for name in SHA256SUMS SHA256SUMS.gpg "$rootfs"; do
    curl --fail --location --proto '=https' --proto-redir '=https' \
        "${base_url%/}/$name" -o "$name"
done
gpgv --keyring "$out/ubuntu-archive-keyring.gpg" --status-fd 1 SHA256SUMS.gpg SHA256SUMS \
    > signature.status 2> signature.log
awk -v key="$cd_key" '$2 == "VALIDSIG" && ($3 == key || $NF == key) {ok=1} END {exit !ok}' \
    signature.status || fail 'rootfs signature is not from the pinned Ubuntu CD key'
awk -v name="$rootfs" '$2 == name || $2 == "*" name {print; n++} END {if(n!=1) exit 1}' \
    SHA256SUMS > rootfs.sha256
sha256sum -c rootfs.sha256
podman import --arch amd64 --os linux "$rootfs" > base-image.id
container="diskos-usbboot-env-$(id -u)-$$"
printf '%s\n' "$container" > container-name.txt
# Keep the container and all evidence for inspection, including on failure.
podman run --interactive --name "$container" --pull=never --network=host --user 0 \
    -e SNAPSHOT="$snapshot" -e LC_ALL=C -e TZ=UTC -e DEBIAN_FRONTEND=noninteractive \
    -v "$out:/evidence:rw" -v /etc/ssl/certs/ca-certificates.crt:/run/host-ca.crt:ro \
    --entrypoint /bin/sh "$(cat base-image.id)" -es \
    > prepare.log 2>&1 <<'PREPARE'
test "$(dpkg --print-architecture)" = amd64
# Disable every existing repository; use only the selected signed snapshot.
for f in /etc/apt/sources.list /etc/apt/sources.list.d/*.list /etc/apt/sources.list.d/*.sources; do
    [ ! -f "$f" ] || : > "$f"
done
cat > /etc/apt/sources.list.d/usbboot.sources <<APT
Types: deb
URIs: http://snapshot.ubuntu.com/ubuntu/$SNAPSHOT/
Suites: noble noble-updates noble-security
Components: main universe
Architectures: amd64
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
Check-Valid-Until: no
APT
# The snapshot host redirects to HTTPS; the host CA bundle is mounted read-only for
# transport only. apt still authenticates indexes and package hashes with the pinned
# keyring. Expired snapshots still require valid signatures.
cat > /etc/apt/apt.conf.d/99usbboot <<'APT'
Acquire::AllowInsecureRepositories "false";
Acquire::AllowDowngradeToInsecureRepositories "false";
APT::Get::AllowUnauthenticated "false";
Acquire::https::CaInfo "/run/host-ca.crt";
Binary::apt::APT::Keep-Downloaded-Packages "true";
APT::Keep-Downloaded-Packages "true";
APT
apt-get -o APT::Update::Error-Mode=any update
set -- libc6=2.39-0ubuntu8.8 libc6-dev=2.39-0ubuntu8.8 \
    libc-bin=2.39-0ubuntu8.8 libc-dev-bin=2.39-0ubuntu8.8 \
    gcc-13 make binutils file pkg-config dpkg-dev bzip2 xz-utils ca-certificates
apt-get -y --no-install-recommends --download-only --allow-downgrades install "$@"
mkdir /evidence/debs
cp /var/cache/apt/archives/*.deb /evidence/debs/
apt-get -y --no-install-recommends --no-download --allow-downgrades install "$@"
for p in libc6 libc6-dev libc-bin libc-dev-bin; do
    test "$(dpkg-query -W -f='${Version}' "$p")" = 2.39-0ubuntu8.8
done
for t in gcc-13 make strip readelf file pkg-config dpkg-source bzip2 xz; do command -v "$t"; done
dpkg -l > /evidence/dpkg-l.txt
dpkg-query -W -f='${binary:Package}\t${Version}\t${Architecture}\t${source:Package}\t${source:Version}\n' \
    > /evidence/packages.tsv
cp -a /var/lib/apt/lists /evidence/apt-lists
cp -a /var/log/apt /evidence/apt-logs
cp /etc/apt/sources.list.d/usbboot.sources /evidence/usbboot.sources
cp /usr/share/keyrings/ubuntu-archive-keyring.gpg /evidence/image-ubuntu-keyring.gpg
cd /evidence
sha256sum debs/*.deb > deb-sha256.txt
PREPARE
podman commit "$container" > build-image.id
podman save --format oci-archive -o build-image.oci.tar "$(cat build-image.id)"
sha256sum build-image.oci.tar > image-archive.sha256
printf 'Pinned build image and evidence: %s\n' "$out"
