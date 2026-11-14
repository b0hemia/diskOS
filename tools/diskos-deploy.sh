#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
# diskos-deploy.sh - push a locally built mq_ui to the device and hot-reload it, safely.
#
# This is for iterating on UI changes. A hand-deployed binary reverts to the flashed build on the
# next reboot (that is intentional). To bake a build in permanently, flash it with the installer.
#
# Usage:
#   DISKOS_IP=<device-ip> DISKOS_PW=<debug-mode-password> ./diskos-deploy.sh [path/to/mq_ui]
#
# Get the IP and a one-time SSH password from Debug Mode on the device (Settings > System). The
# password is regenerated on every enable and does not survive a reboot.
#
# Requires: sshpass, ssh, scp, md5sum. Default binary path: ./mq_ui
set -eu

IP="${DISKOS_IP:?set DISKOS_IP to the device IP (shown in Debug Mode)}"
export SSHPASS="${DISKOS_PW:?set DISKOS_PW to the Debug Mode SSH password}"
BIN="${1:-mq_ui}"
[ -f "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 1; }

# password via SSHPASS (sshpass -e), not the command line, so it is not visible in the process list
SSH=(sshpass -e ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=8)
MD5="$(md5sum "$BIN" | cut -d' ' -f1)"
echo ">> local mq_ui md5=$MD5 -> root@$IP"

# 1. stream to a staging path and verify the exact md5 before touching the live binary
REMOTE_MD5="$("${SSH[@]}" "root@$IP" \
  'cat > /usr/data/mq_ui.new && chmod 755 /usr/data/mq_ui.new && md5sum /usr/data/mq_ui.new | cut -d" " -f1' \
  < "$BIN")"
[ "$REMOTE_MD5" = "$MD5" ] || { echo "md5 mismatch (device=$REMOTE_MD5) - aborting, live binary untouched" >&2; exit 1; }

# 2. swap in and hot-reload, in a failure-aware order.
#    IMPORTANT: only ever touch mq_ui. NEVER kill mq_player - that frees the SD card and the
#    hardware MCU reboots the whole device (~10s).
#    The fiio_init watchdog does `pgrep -x mq_ui` (matches the full argv[0]); if we kill mq_ui
#    without immediately relaunching a DETACHED replacement it respawns the STOCK /usr/bin/mq_ui.
#    We relaunch with setsid </dev/null, then POLL until our build is confirmed alive, and only
#    THEN prune any stock instance. If our build never comes up (a bad binary), we do NOT prune -
#    the watchdog restores the stock UI as a working fallback.
# The remote block deliberately does NOT use `set -e`: it relies on `[ test ] && cmd` idioms and poll
# loops where a false test is normal, and `set -e` would abort the script on the first false test.
# Every step that must not fail is checked explicitly instead.
"${SSH[@]}" "root@$IP" 'sh -s' <<'REMOTE'
mv -f /usr/data/mq_ui.new /usr/data/mq_ui || { echo "ERROR: could not swap in the new binary" >&2; exit 1; }
killall -9 mq_ui 2>/dev/null
setsid /usr/data/mq_ui </dev/null >/usr/data/diskos_boot.log 2>&1 &
# poll up to ~10s. Sleep FIRST, then check, so a check is always the last action before we decide
# (no trailing sleep that could miss a process which came up in the final second).
ok=0
i=0
while [ "$i" -lt 10 ]; do
  sleep 1
  for p in $(pidof mq_ui 2>/dev/null); do
    if [ "$(readlink /proc/$p/exe 2>/dev/null)" = /usr/data/mq_ui ]; then ok=1; fi
  done
  if [ "$ok" = 1 ]; then break; fi
  i=$((i + 1))
done
if [ "$ok" != 1 ]; then
  echo "ERROR: /usr/data/mq_ui did not come up - NOT pruning; the watchdog will restore stock" >&2
  exit 1
fi
# our build is confirmed running -> prune ONLY a genuine stock instance (exe is EXACTLY /usr/bin/mq_ui)
# that the watchdog may have raced in. Never kill a PID we cannot classify: an empty readlink means the
# PID vanished (or was reused), so leaving it alone avoids killing an unrelated process.
for p in $(pidof mq_ui 2>/dev/null); do
  if [ "$(readlink /proc/$p/exe 2>/dev/null)" = /usr/bin/mq_ui ]; then kill -9 "$p" 2>/dev/null; fi
done
# verify: no stock instance survived the prune (a failed kill must not report success)
stock=0
for p in $(pidof mq_ui 2>/dev/null); do
  if [ "$(readlink /proc/$p/exe 2>/dev/null)" = /usr/bin/mq_ui ]; then stock=1; fi
done
if [ "$stock" = 1 ]; then
  echo "ERROR: a stock mq_ui is still running after prune (check the device)" >&2
  exit 1
fi
if pidof mq_player >/dev/null 2>&1; then
  echo ">> deployed OK: /usr/data/mq_ui running; mq_player alive"
else
  echo ">> deployed: /usr/data/mq_ui running, but mq_player is NOT running (check the device)" >&2
fi
REMOTE
