#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
# Sourced by install.sh. Probe Tk without opening a display.
macos_python_ok() {
    "$1" -c 'import sys, tkinter; sys.exit(0 if sys.version_info >= (3, 8) and tkinter.TkVersion >= 8.6 else 1)' 2>/dev/null
}
select_macos_python() {
    for candidate in /Library/Frameworks/Python.framework/Versions/Current/bin/python3 \
        /Library/Frameworks/Python.framework/Versions/3.*/bin/python3 \
        /opt/homebrew/bin/python3 /usr/local/bin/python3 python3 python; do
        resolved=$(command -v "$candidate" 2>/dev/null) || continue
        if macos_python_ok "$resolved"; then
            printf '%s\n' "$resolved"
            return 0
        fi
    done
    echo 'error: macOS needs Python 3.8+ with Tk >= 8.6.' >&2
    echo 'Install Python from https://www.python.org/downloads/macos/ and re-run ./install.sh.' >&2
    echo 'For Homebrew Python: brew install python-tk@<major.minor> (match your Python version).' >&2
    return 1
}
