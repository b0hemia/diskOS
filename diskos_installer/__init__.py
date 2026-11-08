"""diskOS installer - cross-platform (Linux/macOS) installer for the FiiO Snowsky Disc.

The whole tool is Python; the two bricking-sensitive operations (the Ingenic
mask-ROM USB flash and squashfs packing) delegate to PROVEN native binaries that
ship alongside this package (see ``bundle.py``) rather than being reimplemented.

Run from source with your own Python: ``./install.sh`` creates a local virtualenv and
installs the two pip dependencies (pyusb, pycryptodome), then ``./diskos-installer``
runs it. The GUI additionally needs the system's Tk; the flash step needs libusb-1.0.
"""

__version__ = "1.2.3"
