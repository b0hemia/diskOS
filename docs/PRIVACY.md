# diskOS privacy and network disclosure

This describes network use by the installer and the diskOS UI (`mq_ui`). The UI source
is published under GPL-3.0-or-later in [`ui/`](../ui/).

## The installer (this repo)

- **`./install.sh` uses `pip`** to upgrade `pip` itself and download four packages - `pyusb`,
  `pycryptodome`, `certifi` and `platformdirs` - from **PyPI** into a local `.venv`. That `pip`/PyPI traffic is the only network
  activity in setup. Configure `pip` to use your own mirror or offline package files if you prefer.
- **Get firmware** checks the local cache first. If no matching verified ZIP is saved, it downloads
  the selected version over verified HTTPS from `fiio-firmware.fiio.net` or
  `fiio-firmware.oss-us-west-1.aliyuncs.com`. FiiO and its hosting provider receive your public IP
  address and the requested firmware URL. No device identifier, music library or telemetry is sent.
  There are no firmware requests at startup or merely when you change the selected version.
- After a download error, the installer may check the local **Downloads** folder for a matching
  file. **Check Downloads** also requests this check. The search is nonrecursive; it verifies
  candidate files locally and copies a match into its own cache, leaving your original untouched.
- **Open FiiO download page** opens the official forum in your browser only when you press it.
  The browser then contacts FiiO under its own privacy settings. If it cannot open, the installer
  displays the page address for you to copy.
- Verified firmware ZIPs are kept under the installer state folder until **Remove installer data**
  removes them, along with saved recovery images, leftover partial downloads and owned logs.
  Failed or cancelled acquisitions clean up their temporary downloads; interrupted transfers restart
  from zero. Files in Downloads or selected with **Browse...** are never removed by installer cleanup.
- A matching verified cache entry, a manually supplied ZIP, or a usable saved stock image can be
  used offline. Building, decrypting/extracting, saving the recovery image and flashing over USB
  are local. The installer sends no telemetry. See [firmware acquisition](INSTALL.md#get-firmware).

## The diskOS UI (`mq_ui`) on the device

diskOS is a local music player. Weather on Home is **on by default**; turning it off in Settings > Display > Weather on Home stops automatic fetches and hides the glance. The table below describes when each online feature sends a request. Each request reveals the device's public IP to the service it contacts.

| Feature | Endpoint | Protocol | What is sent |
|---|---|---|---|
| **Weather** | `wttr.in` | HTTPS | With Weather on Home on, diskOS fetches at UI startup and later refreshes or retries when Wi-Fi works. With it off, diskOS starts no weather request at all, including from the Weather app. A request already in flight may still finish after you turn it off. It sends your configured location, or no location so wttr.in can estimate one from your public IP. |
| **Online album art** | `itunes.apple.com`, `*.mzstatic.com` | HTTPS | When Online Album Art is enabled and a playing track has no cover, artist and album or title metadata, plus a storefront country selected from the player's language, are sent to search for artwork. Matching images are downloaded from Apple's image hosts. Off by default. |
| **Lyrics** | `lrclib.net` | HTTPS | Online Lyrics is on by default. Opening Lyrics for a song without local or embedded words sends the current track's **title and artist** to find matching lyrics, if Online Lyrics is enabled. |
| **Scrobbling (Last.fm)** | `ws.audioscrobbler.com` | HTTPS | Off by default. If enabled and connected: the device sends `track.updateNowPlaying` and scrobbles with track metadata (artist/title/album/duration, plus a timestamp for scrobbles), API key and session key to **your** Last.fm account. |
| **Update check and download** | `api.github.com`, `github.com` | HTTPS | Opening Update diskOS checks the latest release. Automatic Update Checks (off by default) also checks periodically over Wi-Fi when enabled. The 1.2.3 release image includes the diskOS release key by default; confirming a supported update downloads its signed app bundle. Switching Allow diskOS Updates off prevents download and staging but still permits a release check. |
| **Last.fm sign-in** | `ws.audioscrobbler.com/2.0/`, `www.last.fm/api/auth/`, `www.last.fm/api/account/create` | HTTPS | The device POSTs `auth.getToken` and polls `auth.getSession` at `https://ws.audioscrobbler.com/2.0/` with the API key and authorization token. It displays a QR approval URL; your phone opens that URL. The local setup page also links to `www.last.fm/api/account/create` for obtaining an API key. |
| **Beam and AirPlay Receiving** | Speakers, TVs and senders on your Wi-Fi | Local discovery and Cast/AirPlay protocols | Beam sends selected music, metadata and artwork to your receiver. Cast music and artwork use plain HTTP; Cast control uses TLS without authenticating the receiver. AirPlay Receiving accepts audio and metadata through the stock player. Use a trusted network. |

### Last.fm credential setup happens over your LOCAL network in plaintext

To connect Last.fm, diskOS runs a **temporary web server on the device** at
`http://<device-wifi-ip>:8080/<random-token>/`, shows a QR for it, and your phone posts your Last.fm
**API key and shared secret** to it. Important properties:

- It is **plain HTTP on your Wi-Fi LAN** - your API key/secret cross your local network unencrypted.
  Do this on a network you trust.
- The server is **transient** (runs only during setup), bound to the Wi-Fi interface, and gated by a
  random URL token.
- Your Last.fm **API key, secret, and session key are then stored on the device** under `/usr/data`
  (device config); **offline scrobbles are queued** in `/usr/data/lastfm.queue` until they can be sent.
  Nothing Last.fm-related is sent anywhere except your device, your phone (during setup), and Last.fm.

### The stock FiiO player still runs alongside diskOS

diskOS replaces only the UI; FiiO's original player process still runs underneath it. That stock
component is responsible for Bluetooth, FiiO firmware-update checks, and LAN media
endpoints (DLNA/AirPlay/Roon/QPlay-style discovery) the device may present on your local network. Those
behaviours are inherited from the stock firmware and are outside diskOS's code. On V2.57, the stock player also performs an `ipinfo.io` time-zone lookup and network time synchronization after Wi-Fi connects when Automatic Time is enabled; changing that setting takes effect after the player next starts.

## What diskOS does NOT do

- No analytics, telemetry, or usage tracking.
- It does not upload your music library, your listening history (beyond Last.fm scrobbles if you turn
  them on), or any personal files to an online service. Beam sends selected music and artwork to your chosen receiver on the local network.

## Reporting

Report anything that looks like unexpected data leaving the device via the process in
[`SECURITY.md`](../SECURITY.md).
