# diskOS 1.2.1 release notes

Released 2026-10-14.

Adds Ogg Opus support and Windows installation, updates the installer experience and
startup animation, and fixes text, playback, Wi-Fi, screen wake and app-update handling.

1.2.0 users can install the signed app update from Settings > System > Update diskOS
if updates are allowed. Fresh installs and older versions use the installer.
App updates do not replace the boot install script. Fixes marked "Requires a full
flash" need the complete 1.2.1 installer.

Read the [installation guide](INSTALL.md) before flashing. Back up your music,
keep the saved stock image on separate storage, and keep the Disc connected and
computer awake until verification finishes. Recovery is not guaranteed.

### Added

- [Public website](https://b0hemia.github.io/diskOS/) with screenshots, a feature tour
  and installation links.
- Ogg Opus support for .opus, .oga and .ogg files: the scanner reads their tags and
  available lengths, and playback uses the Disc's stock player. When the player's
  title is empty or matches the filename, diskOS reads the file's TITLE tag and
  uses it when the read succeeds. If tags are absent or unreadable, the player's
  title is retained. Lengths the scanner cannot read remain unknown.
  The resolved title also reaches the current Up Next row, Song Info, lyrics and
  online artwork searches, and Last.fm.
- Playback progress around Home's now-playing border in the halo theme.
- Settings > System > Old tags: Auto / Western / Cyrillic for legacy music tags and
  CUE-sheet text. Auto is the default and guesses CP1251 Cyrillic; Western keeps
  Latin-1 decoding, and Cyrillic selects CP1251. Use Rescan Library after changing
  it. Valid UTF-8 and UTF-16 tags are unchanged.
- Windows 10/11 x64 installer support. Extract the complete
  `diskos-installer-windows-x86_64.zip` and open `diskos-installer.exe`.
  Keep `_internal`, `RECOVERY.txt` and the other files together. The program is
  unsigned; Windows SmartScreen may warn on first run.
- A Ready checklist before image preparation, with flash-mode detection, USB driver
  or permission hints, the firmware choice and installer tools. It shows items
  needing attention and a Refresh action. When no Disc is found, it explains how
  to power off, hold Volume Down and connect USB; the screen stays black.
  Checklist warnings do not block the install. Firmware and actual USB access
  are checked before flashing.
- A confirmation screen listing the firmware base, duration and update choice.
  Begin flash stays disabled until the acknowledgement is ticked.

### Changed

- The Default theme is now called halo.
- The installer on Windows, macOS and Linux uses the website's diskOS wordmark,
  Archivo and Michroma display lettering, a disc illustration, quieter sections
  and a rose primary button. Dynamic text uses the system font; no fonts are
  installed on the computer. Dev is under Advanced.
- The installer has dedicated preparation, flashing and result views. Flashing
  shows elapsed time, progress and a reminder to keep the Disc connected and
  computer awake. Details opens the log. Success explains the next step; errors
  show the code and recovery action. An unknown flash result shows the 25-minute
  wait and offers no Back action.
- The installer layout is compact for 1366x768 screens. Taller content scrolls
  inside the window, and result-screen Back and Close buttons remain outside
  the scrolling content.
- Restore stock firmware uses the saved stock image and offers a firmware ZIP
  picker only when no saved image is found. For a damaged saved backup, use
  `restore-stock --firmware <zip>` to rebuild it from matching stock firmware.
- Remove installer data clearly describes host cleanup. It removes saved state
  and owned GUI logs, while keeping the app and Windows USB driver binding.
  It does not remove diskOS from the Disc.
- A new startup animation: a light splits into two spectra across the disc,
  the diskOS wordmark appears in the hub, and the hub opens to reveal the
  interface. It lasts about 2.4 seconds; the glow follows the theme's accent.
  The wordmark uses Archivo, matching the website and installer.
- Settings > Display > Disc Colour is removed. It controlled only the previous
  startup drawing; the new animation uses the theme's accent.
- Theme details extend through lists, empty states, missing artwork and
  screensaver song displays: hi-fi's amber row lights, bauhaus's geometric
  bullets, blueprint's figure numbers and drafting marks, stone's pebble
  controls, and zine's tape and pressed-row underlines.
- Something has a blinking clock colon, dot arrows and a dotted Now Playing
  progress ring. Terminal has a blinking header cursor, aligned playlist
  durations and a highlighted Now Playing label in Up Next. Hi-fi uses bezel
  ticks for progress; stone uses a fading progress arc.
- Bauhaus Now Playing fills the left half with artwork, uses sharper covers
  when available and gives queue position its own line.
- Screensavers show song text while playing and hide it when paused. The Analog
  dial is smaller and higher. Vinyl hides song labels while showing a cover
  and keeps fallback text when no cover is available.
- Unchanged song and clock text no longer causes repeated text and layout work.
  Now Playing title scrolling stops off screen, and Vinyl rotation stops when
  there is no cover. With Covers only selected, idle cover preparation waits
  for work instead of waking every second.
- Screen-off drawing is paused and repainted before the backlight returns.
  Cover Flow and the Library scroll-letter timer stop checking when idle.
  Library row loading pauses on other screens and resumes on return.
  Sliding screens avoid repainting areas already covered by the moving panel.
- A-Z jumps wait for their target rows without building the remaining list
  at once; manual scrolling cancels a pending jump. Large playlists build
  rows incrementally. Folders are read in the background.
- Cover Flow prepares missing covers in the background, uses placeholders
  until they are ready and releases its cover memory after leaving.
  Library buffers are allocated as needed rather than all at startup.
  Album track-order reads are batched when possible, and artist queue
  membership checks use less work.
- Rescans skip embedded artwork when reading tags and look up existing files
  more efficiently. Cancelling or failing a rescan no longer forces an
  otherwise healthy library to reload.
- Unchanged settings and playlist records avoid unnecessary writes. Playlist
  counts are read together, and Last.fm avoids repeatedly rereading its queue
  just to count pending scrobbles.
- Wi-Fi checks back off while a connection remains down and return to faster
  checks after activity, wake, network recovery or a Wi-Fi setting change.
  Weather skips background downloads while offline, fetches after reconnecting
  and backs off repeated connected failures.
- Weather requests now use HTTPS.

### Fixed

- Keep or Go back is not asked again after changing theme, appearance, text size
  or language. A failed save leaves the question open for retry.
- Later app updates can complete their trial after an earlier update was kept,
  instead of rolling back because the running-build record outgrew its buffer.
- After confirmed playback, failed update-health saves are retried for a few
  minutes rather than abandoned after one attempt.
- A build previously rolled back on this Disc is refused before its bundle
  downloads, with an explanation.
- The boot install script no longer performs installation during shutdown.
  Requires a full flash.
- The 1.2.1 system image refuses saved app updates below its update limit.
  An app update does not change that image limit. Requires a full flash.
- Keeping an update waits until the previous saved version has actually been
  moved aside. If that move fails, the update remains on trial instead of
  being moved inside the previous version's directory. Requires a full flash.
- If a trial start cannot be recorded, that boot uses the previous accepted
  or flashed app and leaves the trial for a later start. Requires a full flash.
- A proven healthy update remains on trial if saving its approval fails,
  with promotion retried on a later start. Requires a full flash.
- On startup, an install log larger than 64 KB is trimmed to its newest
  300 lines. Requires a full flash.
- The boot installer removes its spare rejected-app copy after verifying
  the installed app, freeing space for updates. Requires a full flash.
- Returning from Now Playing to top-level Songs, Albums, Artists or Genres
  no longer rebuilds the list.
- Installer console output uses readable replacements for characters the
  terminal cannot display.
- Missing or unloadable firmware-decryption dependencies stop the installer
  before firmware work with E105 and corrective instructions.
- Unknown flash results explain the required wait: leave the Disc powered
  and connected, and the computer awake, for at least 25 minutes after the
  error. Do not retry or disconnect during that wait.
- USB flashing handles partial transfers and bounds stalled communication.
  Reconnection requires exactly one device at the same USB location.
- Chinese, Japanese and Korean names and titles use the stock font fallback
  across themes and text sizes. Latin-1 accented letters retain the zine and
  something title faces; zine also retains its face for Cyrillic U+0400-U+045F.
  Theme letter case handles Latin-1, Latin Extended-A and those Cyrillic pairs.
- V2.57 Wi-Fi skips the extra startup hold when Bluetooth is saved off.
  Other startup cases can still take about a minute.
- Headphone output can recover after a UI restart or boot that left the
  player routed to Bluetooth without a connected audio device (issue #9).
- On V2.57, one power-button press wakes diskOS Screen Off. Button wake
  restores the user's diskOS brightness.
- Now Playing accepts song details containing double quotes rather than
  retaining the previous song.
- diskOS briefly checks for a missed track advance near a track's end while
  the screen is lit, and checks once on wake. Advancing from one mono Opus file
  to another can still leave the old track displayed. Some mono FLAC, MP3 and Opus
  files still play slowed down in the stock player.
- Returning to Now Playing retries missing artwork after a theme restart or
  a temporary refusal of card access.
- Missing and blank artist tags appear under Unknown artist, and opening
  or playing that group finds its songs.
- Screensaver titles and artists refresh for tracks sharing a path, including
  CUE and SACD ISO tracks. Song displays leave room below the date and stay
  inside the round screen at different text sizes.
- Weather unavailable remains tappable on Home. Cancelling the city keyboard
  preserves the saved city; saving an empty city selects automatic location.
- Zine search text, placeholder and cursor remain readable. A populated
  search field stays clear of Back's touch area and the round screen's edge.
- The A-Z grid is readable in zine's dark appearance and dims the list behind
  it in every theme. Long translated Scan Library labels fit the round screen.
- V2.57 Custom EQ sliders visibly show their view-only state; tapping a band
  explains "Not on this firmware".
- Switching Wi-Fi networks renews the address for the selected network.
  Network details refresh during connection; IP and router values wait for
  a ready connection. Slow connections get about 75 seconds before failure,
  and status and scan checks have time limits.
- Saved Back swipe distances are clamped to 20-200 pixels when loaded.
- Delayed song details alone no longer trigger "Couldn't start playback".
  The playback-start check uses player progress and audio-output evidence;
  ambiguous brief starts retire quietly instead of producing a late error.

## Acknowledgements

- [carlosluqueancona](https://github.com/carlosluqueancona) traced silent headphone
  output after a UI restart to its cause and suggested the startup route check
  used by the fix ([issue #9](https://github.com/b0hemia/diskos/issues/9)).
- [ingvarr777](https://github.com/ingvarr777) contributed Ogg Opus support in
  [PR #19](https://github.com/b0hemia/diskos/pull/19); the 1.2.1 implementation
  builds on that work.
- [ingvarr777](https://github.com/ingvarr777) traced later update rollbacks to the
  run record outgrowing its buffer and proposed the 1024-byte fix used in 1.2.1
  ([issue #22](https://github.com/b0hemia/diskos/issues/22)).
