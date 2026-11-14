# Changelog

All notable changes to diskOS are documented here.

Entries follow the Keep a Changelog format, with Added, Changed, and Fixed categories where applicable.

## [1.2.3] - 2026-11-XX

<!-- Checks for the lead to resolve:
- [TV] Receiver volume: one step per Disc press.
- [TV] Cast receiver Stop and resume from the last usable position.
- [TV] V2.57 physical play/pause while beaming to Cast and AirPlay.
-->

Adds low-battery notices with a safe turn-off at empty, sort tags, featured artists as their own
artists, WAV tags, Gurmukhi, Devanagari, Urdu and Hebrew text shaping, each song's own cover art
and a Weather refresh, makes the Disc's background work, long library lists and screen wakes do
less work, and fixes Now Playing, the side key and Stop while beaming, power-key shutdown,
Bluetooth, Wi-Fi, Custom EQ on V2.57, idle power-off, long file paths, playlists, Last.fm,
resetting settings and installer handling.

1.2.2 users can install the signed app update from Settings > Device > Update diskOS if updates
are allowed. It includes the matching cover helper and delivers the 1.2.3 UI changes without a
full flash. App updates do not replace the boot script or the image's minimum update epoch. The
boot-log correction and newer image minimum come with a full flash using the 1.2.3 installer.
Fresh installs and older versions use the installer.

### Added

- Low battery: notices warn at 10% or below and 5% or below, and announce 100% after charging has
  been observed. On V2.40 and V2.57, a valid 0% reading while not charging shows a "Battery empty"
  dialog and starts a 30-second countdown ("The Disc turns off in 30 s to keep your music card
  safe."). "Turn off now" skips the countdown, then follows the same playback-stop and card-flush
  checks. Detecting charging, a reading above 0%, or unavailable battery evidence cancels the
  countdown. If the checks fail, diskOS keeps card access closed, withholds its power-off request
  and asks you to restart. Other firmware gets notices without diskOS's empty-battery shutdown.
  [?: Disc check of the 0% countdown, V2.40 and V2.57]
- Sort tags: artist, album artist and album sort values (ID3 TSOP/TSO2/TSOA and ALBUMARTISTSORT,
  Vorbis comments in FLAC/Ogg/Opus, MP4, WMA, APE, and ID3 tags in AAC, AIFF, DSF and WAV) order
  Artists, Albums, A-Z and Album Wall. Display names do not change, and untagged files sort as
  before. Settings > Library > Maintenance shows "Rescan for sort tags" (in place of Rescan
  Library) until they have been read once.
- Featured artists: "feat.", "ft." and "featuring" credits, including "(feat. X)", list the guest
  as their own artist. Album Artist names stay exactly as tagged.
- WAV files read their title, artist, album, genre and track tags (ID3 chunk first, then
  LIST/INFO), with album artist and disc number from ID3. Rescan the library to read them.
- Gurmukhi and Devanagari names are shaped correctly, and Urdu and Hebrew names read right to
  left, with long names scrolling in lists (#20). Latin and CJK text are unchanged.
- Now Playing shows each song's own embedded cover instead of the album's first cover.
- Weather shows when it last updated, with a Refresh button.
<!-- open: [?] the Disc check of the 0% countdown on V2.40 and V2.57 has not been done. -->

### Changed

- Long lists (songs, albums, artists, genres, playlists, Files) reuse a small pool of rows,
  reducing display memory.
- Waking the screen keeps an unchanged cover, with no loading icon and no second decode.
- Album covers share one cache on the card. The new shared cache is capped at 4 GiB and refuses
  growth that would leave less than 1 GiB free. Identical pictures are stored once, older covers
  (except protected ones, such as nearby covers) are removed when space is needed, and old cache
  files are retired during idle maintenance.
- Background album-cover preparation follows the Album Art Cache setting (When Idle, or Idle &
  Charging).
- With the screensaver off, Screen Off happens at the chosen time.
- Finished helper processes no longer wake the CPU every millisecond, and after a restart
  background preparation skips covers already prepared on the card.
- Wi-Fi, Bluetooth and several UI workers use event notifications and scheduled deadlines,
  reducing repeated background checks. Bluetooth restore at startup waits for the player's answer,
  and a Wi-Fi scan asked for during a connection hold runs after it. [?: Wi-Fi and Bluetooth
  reconnect, restart and fallback device checklist not done]
- Wi-Fi scanning, connection and radio work run in the background instead of blocking touch
  handling.
- The Vinyl screensaver starts at a new angle each time it opens.
- Beam: one Disc volume press moves the receiver by one step, not two.
- The AirPlay receiving screen stops redrawing while it is hidden or the screen is off.
- Repeated Cast artwork requests reuse one prepared cover.

### Fixed

- Now Playing: the play/pause icon holds steady while a seek lands; the mode button never starts a
  ring seek (#32); the timer no longer flashes 0:00 after a play mode change.
- Picking a chapter or an Up Next row no longer stacks extra copies of Now Playing; one Back
  returns to where you came from (#32).
- A touch that wakes the screen only wakes it, and cannot press what is underneath (#32).
- Idle power-off: only real volume key presses count as activity (V2.57), so a paused Disc powers
  off on time; a failed power-off is retried.
- Custom EQ on V2.57 edits the ten-band USER presets one band at a time like the stock editor,
  keeping the other bands and their Q values; advanced presets stay protected. Flat no longer
  reports "Couldn't apply EQ". The saved EQ is sent again when the player restarts, and a Gapless
  choice made in the stock UI is used until you choose one in diskOS (#32).
- Wi-Fi: a mistyped password is never saved, starting the UI no longer drops the connection, the
  details screen stays steady while it updates, Forget acts on the network you opened, and a
  connection whose settings could not be saved says so.
- Bluetooth: removing a device that is not playing keeps the current output; turning Bluetooth off
  says so if the output cannot switch back; a reconnect keeps the saved resume position. A
  temporary Bluetooth audio rebuild keeps a manual switch back to Analog.
- Library: a scan never commits after a card swap; a lost scan transaction keeps the library;
  Search and Cover Flow keep up with library reloads, and Cover Flow keeps the selected album;
  card writes are refused when the card is not mounted; navigation gestures wait for open dialogs.
- Playlists: export never overwrites a file it did not write; removing a song removes exactly the
  row pressed. Long playlist file names import under distinct names.
- Books: a seek made after a restart is kept; Chapters follow the playing chapter.
- Settings that use the shared save path no longer apply after a failed save, and an unconfirmed
  save says "Saved - not confirmed yet"; the time zone is kept when a reload is refused; manual
  clock saves wait for an earlier save.
- Reset diskOS Settings can restart a partly completed reset; when Bluetooth is off, it explains
  that clearing nicknames waits until Bluetooth is turned on.
- Theme changes wait for radio work to settle and restore the previous choices if they cannot
  complete.
- Last.fm keeps unsent plays on read errors and on account or key errors, and sends no late Now
  Playing after you pause or switch to AirPlay.
- Online Album Art retries a missing cover after Wi-Fi reconnects.
- Installed apps keep longer names and launch paths.
- Closing Lyrics stops an old lookup from starting another request.
- Lyrics skip a leading byte-order mark, and online lyrics are found when the reply has spaces
  around its fields.
- Favourites: a heart that fails to reach the player no longer shows success.
- Plays are recorded while the screen is off.
- Weather keeps the last reading when a new one is invalid, and does not fetch while off.
- The Vinyl screensaver keeps showing the playing song's cover.
- The screensaver keeps unsaved lock-screen edits and does not open over a dialog; an untouched
  colour slider saves nothing.
- A build whose size differs from the pending trial no longer shows that trial's Keep / Go back
  prompt.
- AirPlay: leaves its queue cleanly. Cast: rejects oversized positions, and the picker keeps found
  devices.
- Cast: a receiver Stop shows stopped on the Disc. Disc Play reloads the same song from its last
  usable reported position, which can be earlier than the actual stop.
- On V2.57, the Disc's play/pause key pauses and resumes the Cast or AirPlay receiver while
  beaming, without starting local playback. Other firmware keeps its previous key behavior.
- Power key: when the player announces shutdown, diskOS stops new card work, stops cover helpers
  and image previews, and attempts to drain and flush pending work before the player turns off. An
  existing shutdown is joined. If the player restarts, card access resumes and the library is
  checked again. Power-key shutdown also uses the closing animation when the screen is on.
- A shutdown failure while the screen is off shows its restart notice when the screen wakes.
- Long file paths: diskOS carries complete absolute track paths of up to 1023 bytes through the
  library, playback display, lyrics, Song Info, playlists, artwork and Beam, including audiobook
  resume and play history. Oversized paths are refused or skipped instead of cut short; an
  oversized current-track report shows Track unavailable / Path too long.
- Dialogs and menus own the whole touch that starts on them: with one open, a press no longer
  seeks Now Playing, scrolls the rim, drags Cover Flow or swipes the screen behind it, and a
  dialog that opens during a gesture cancels what the gesture started. The volume overlay allows
  seeking and scrolling with touches that start outside it.
- Artists: a stray control character between commas in an artist tag no longer shows up as its own
  blank artist.
- Installer: the flash check now rejects a page the Disc's memory reports as unreadable instead of
  accepting it.
- Installer (Linux): if the bundled squashfs tools fail their check (seen on Fedora 44), the
  installer checks a complete pair of system tools the same way and uses them (#33). [?: rebuilt
  musl tools in the package, confirm at packaging]
- Installer: during an update the boot log no longer says the stock UI will run; it says the old
  UI is being replaced and reports the outcome on its own line.
- Wi-Fi, Beam, EQ and power messages (low and full battery, the empty-battery dialog and shutdown
  messages), the sort-tag rescan label and "Couldn't apply the theme now" are translated, and the
  Vol Keys hint is translated in Italian and Russian.
- Small fixes: the receiver name under the volume arc wraps inside the arc, Source picker titles
  stop before the selection mark, the AirPlay receiving screen has no card edge ring, and Add to
  Playlist messages are never hidden by the picker.

## [1.2.2] - 2026-10-24

Adds Beam (play the Disc's music on Cast and AirPlay speakers and TVs), AirPlay receiving from an
iPhone or Mac, a custom screensaver image, editable device and Bluetooth names, optional update
checks, a swipe up to Home, a closing animation when the Disc powers itself off and a Get
firmware button in the installer, and fixes startup, Bluetooth, library, charging, Hebrew
text, Up Next and installer handling.

1.2.1 users can install the signed app update from Settings > System > Update diskOS
if updates are allowed. Fresh installs and older versions use the installer.
App updates do not replace the boot install script or the system image. Changes marked
"Requires a full flash" need the complete 1.2.2 installer.

### Added

- Beam: play what is on the Disc on speakers and TVs on your Wi-Fi. Open it from the Beam button
  on Now Playing, the first row of the Options page, or the Beam tile, which is off until you
  add it under Settings > Controls > Quick Settings. The picker lists Cast and AirPlay devices
  found on your network, each marked with its type. While you beam, the playback controls and
  the volume dial control the receiver, and the Disc stays quiet. Beam needs press, double press
  and long press volume-key actions set to Adjust Volume; switching them to track controls is
  unavailable while beaming.
- Beam to AirPlay: PIN pairing uses a keypad on the Disc and remembers successful
  pairings. If pairing fails before PIN entry, Beam keeps the failure on screen so you
  can retry. AirPlay 1 receivers (marked "AirPlay 1") are listed too; ones protected by a
  password are not supported. AirPlay beaming decodes mono or stereo MP3, FLAC and PCM WAV at
  8-384 kHz. The Disc converts AAC, ALAC, Opus, Ogg Vorbis, WMA, APE and DSD for the receiver as
  it plays. AirPlay output is 44.1 or 48 kHz at 16 or 24 bits, depending on receiver support;
  AirPlay 1 uses 44.1 kHz at 16 bits. The Beam screen shows the output rate and bit depth and
  offers a Volume button.
- Compatible AirPlay receivers show the track's cover, title, artist and progress.
  TV remotes can play, pause and skip to the next or previous track where supported.
- Beam to Cast supports MP3, AAC, Opus, Ogg Vorbis, and 16- or 24-bit FLAC and PCM WAV up to 96
  kHz, in mono or stereo. Google Home Mini receivers accept FLAC up to 48 kHz. ALAC, AIFF, WMA,
  APE, DSD, WavPack, SACD ISO files, CUE tracks and audiobooks cannot be beamed and say so.
- Beam follows library selections, favourites, playlists, play modes and Up Next. For AirPlay,
  hold an upcoming track to play it next or remove it. Unsupported tracks show a reason, and
  automatic queue progression reports skipped tracks.
- Beam offers Search again, Wi-Fi settings, Cancel while connecting and Stop beaming.
  When the current track can continue on the Disc, Stop beaming and Play on Disc
  resume its last playback position once the receiver is known to have stopped,
  without briefly playing from 0:00 or flashing 0:00 on the clock.
  If Cast has no track to resume, Stop beaming ends the session and leaves the
  Disc paused. AirPlay offers Forget pairing after a PIN
  or verification failure. Its Play on Disc confirmation explains when only the current
  track can continue or the receiver may still be playing.
- While beaming, lyrics follow the remote track, Favourite toggles for the playing track are unavailable, and Song
  Info hides the Disc's output sample rate. Cast listening can scrobble to Last.fm; AirPlay
  sending and receiving do not. Remote sessions hold off idle shutdown, and the sleep timer ends
  remote playback.
- AirPlay Receiving: pick it as a source (Quick Settings > Source, or Settings > Network > Audio
  Source) on V2.40 and V2.57, then choose the Disc on your iPhone or Mac. It appears as SNOWSKY
  DISC. Now Playing shows the sender's track, artist and cover. The Disc does not power off from
  idle while the receiving screen is up. Return to Local takes you back to local playback. The
  row is hidden on V2.09 and V2.28.
- AirPlay Receiving has a themed Now Playing view with playback controls, timing when supplied,
  connection retry and Wi-Fi settings. Return to Local asks for confirmation when needed;
  Options offers Return to Local and Source. A sleep timer ends receiving, and idle shutdown
  stays blocked even if receiving has failed or its state is uncertain.
- Custom screensaver image: Settings > Display > Saver Style > Custom shows your own JPG, PNG,
  BMP or animated GIF from the CustomCover folder on the SD card. GIFs must be 4 MiB or smaller
  and no larger than 364 x 364. Choose Image opens a preview screen (some images show a placeholder), and the saver can overlay
  Time, Date, Battery, Now Playing and Weather, in a font of your choice (Theme, Plain, Digital,
  Mono or Rounded) at the top, centre or bottom. If the image cannot be used, the saver falls
  back to Minimal.
- Custom starts with Time and Date on, the Theme font and Bottom position. Use Saved Image
  reuses the last image; Preview shows the complete result with labelled sample data where
  needed. Tap Save to keep overlay, font and position changes; leaving Custom without saving
  discards them. Weather uses the last reading, marks older readings Cached and hides them after
  two hours; Custom and Preview start no weather requests.
- Editable names: Settings > Network > Device name sets the Disc's network name (letters, digits
  and hyphens, up to 32 characters, starting and ending with a letter or digit) and the name
  Beam shows to receivers. It applies after a restart. Requires a full flash. Settings > Network
  > Bluetooth name sets the name other Bluetooth devices see (printable ASCII, up to 32
  characters and nine words). A paired device's details have Rename to give it a nickname on the
  Disc; Use original name restores it. Reset diskOS Settings clears both names and all
  nicknames, and keeps your pairings.
- Settings > Device > Automatic Update Checks (off by default). When on, the Disc looks for a
  new diskOS release now and then over Wi-Fi and shows a one-time "diskOS x.y.z is available"
  notice and a dot on the Update diskOS row. It never downloads or installs without your tap.
- Update downloads show a progress bar with a percentage and a Cancel button, and then the
  Verifying and Saving steps.
- Swipe up from the bottom edge of the screen to go Home. A swipe right from the left edge still
  goes back one page. Going Home closes the screens above it; Library next opens at its top
  level.
- Closing animation: when the sleep timer shuts the Disc down, or it powers off after being
  idle, with the screen on, the screen closes into the disc, a light travels once round the ring
  while the card is saved safely, and the screen fades to black when it is done. If the save
  cannot finish, the screen comes back. Turning the Disc off with its power key is unchanged.
- Installer: Get firmware downloads the official FiiO firmware for the version on your Disc,
  checks it against the installer's recorded checksum and keeps it on the computer for next
  time. You can still choose a firmware file you downloaded yourself. If the download fails, the
  installer offers FiiO's download page and checks your Downloads folder.

### Changed

- Added SIL OFL 1.1 licence notices for the Beam glyph and merged icon font data, and Chromium
  copyright notices for the Cast schema files.
- Settings is organised into eight pages: Playback, Audio, Network, Display, Library, Controls,
  Battery & Power and Device, with short section headings inside the longer pages. Choose a
  Settings page to reach its rows. Some rows open a picker or further controls. Battery & Power
  shows the battery's charge, health, cycles and temperature. Device Info now includes what
  About showed. Working Mode is now called Audio Source, Old tags is Tag Encoding and Update
  from SD Card is Stock Update Guide. The DSD Output placeholder is gone. The Quick Settings
  picker shows how many tiles are selected: up to six, or five when playback controls are shown.
- Settings switches sit on their rows. Gain, DRE and Gapless are now on/off switches instead of
  cycling rows, the Wi-Fi and Bluetooth rows in Settings > Network carry their own power switch,
  and Equalizer and Saver Style open a list of choices.
- Settings remembers each page's scroll position; choice lists open with the current selection
  in view, and setting shortcuts reveal the relevant row. Outdoor Mode changes return to its
  setting. Tapping an off Wi-Fi or Bluetooth row starts the radio; further taps wait until
  startup finishes.
- Settings rows no longer carry a line of help text under them, except for a few one-line hints
  such as Gain ("For demanding headphones") and ReplayGain ("Evens out track volume").
- Page titles are centred and use up to two lines. A title that still does not fit, such as a
  very long album name, ends with "...".
- Search has a wider field below its page title, and Custom EQ leaves room for longer titles.
- Shared information and confirmation dialogs now fit their text. Longer messages scroll above
  buttons that stay in view.
- Audiobook chapters and narrator details are read in the background. The chapter list shows
  "Loading chapters..." until they are ready.
- Bluetooth details load in the background with Checking and Unknown states, and offer Connect
  or Disconnect, Rename, Use for audio and Forget This Device. Connection failures identify the
  step that failed.
- Full-screen album art shows the current track's sharp 360 px cover in every theme, instead of
  the player's cached online-art file or a blurred backdrop. It is drawn edge to edge, and the
  border lines some themes drew around it are gone.
- Something keeps its clock colon steady on Home and the screensaver instead of blinking.
- The Quick Settings handle is at the bottom of the panel, where the swipe up that closes it
  starts.
- Reinstalling with the full installer starts the diskOS version inside that installer and
  clears downloaded diskOS updates. Settings and music are kept. Requires a full flash.
- Bluetooth headphones: on V2.57 diskOS no longer switches the Bluetooth codec a few seconds
  after the headphones connect. The stock player applies the codec chosen in Settings > Network
  > Bluetooth Codec, and changing that row sends the choice at once.

### Fixed

- Cast and AirPlay covers update when the track changes, including covers that become
  ready after playback starts. Beam waits up to 15 seconds for a missing cover.
- Beam to Cast: seeking, volume keys and the volume dial now control the receiver;
  elapsed time follows receiver reports, and returning to the Disc resumes that position.
- Beam keeps connection and pairing failures on screen with retry guidance, and opens
  Controls directly when volume-key settings need changing. The search ring moves only
  during discovery.
- Battery & Power keeps each battery reading on its own line, including temperature.

- macOS installation bundles Python, modern Tk, libusb and LZO tools, unblocks bundled tools and prevents sleep during flashing; Safari ZIP guidance is clearer.
- Wi-Fi keeps your saved on/off setting across startup and retries reconnecting automatically while showing when Wi-Fi is starting.
- Beam: searching for speakers and TVs no longer fails before the network search starts.
- Slow left-edge Back swipes work where another control does not own the gesture. Selecting a
  track in Up Next returns directly to the existing Now Playing screen.
- The Analog screensaver shows the correct hands immediately on entry.
- Playlist creation, additions, removals and imports, and favourite removals report failures.
  Failed playlist creation keeps the entered name ready to retry.
- Audiobooks report unavailable chapters and failed position saves.
- A failed Custom EQ read keeps the previous curve and slot, and refuses editing until a
  successful read.
- Last.fm retries account and session setting saves that could not finish.

- "Player didn't respond" no longer appears at startup. Automatic commands at boot wait until
  the player has answered, and a real failure is reported at most once per boot (issue #29).
- Computer audio: the USB DAC source (the Disc as a computer's sound card) sends the work mode
  that plays the computer's audio (issue #24).
- Library: one unreadable or hidden entry on the SD card no longer stops deleted songs and books
  from leaving the library. Only the affected folder keeps its rows (issue #27).
- Library: a tag read that could not finish no longer replaces a song's stored tags, and large
  Ogg, Opus and Vorbis comment blocks are read without loading embedded pictures. A busy library
  no longer shows as empty.
- Cover Flow drops albums deleted from the card after a rescan, and retries a failed reload.
- Hebrew titles, artists and albums no longer show boxes in Library and Now Playing. Letters are
  shown, but right-to-left ordering is not corrected (issue #20).
- Vinyl Now Playing draws a round record in every theme with a smaller centre label, and the
  centre label no longer overlaps the no-cover note.
- The Quick Settings Wi-Fi tile shows whether Wi-Fi is actually on, not only the saved choice.
- The charging icon follows the stock player's own charging report: it shows while the Disc
  charges and goes away when the battery is full. The Disc does not power off from idle while it
  is charging.
- The Disc does not power off from idle while Bluetooth Receiving is selected, while it is a computer's sound card (USB DAC), or while its
  card is open on a computer (USB Storage).
- Library: Back, and coming back from another screen, return you to where you were in the list,
  with the item you opened in view, instead of the top.
- Up Next and the Beam queue no longer show 1440:00 for a track whose length is not known yet; a
  real recording of 24 hours or more keeps its length.
- If a Bluetooth speaker or headphones disappear while playback is paused, the Disc returns its
  output to itself quietly instead of leaving the player routed to the missing device.
- Text entry over its length limit shows "Text too long" instead of being cut short.
- Windows: flashing no longer fails when the temporary folder is a short (8.3) path, for example
  with a user name that contains a space. A flash that cannot start because Windows validation
  refuses it reports E304 with no recovery wait (issue #26).
- macOS: rebuilding the native tools from source checks the linked Homebrew packages and their
  bundled libraries, and accepts a revision suffix or several installed versions (issue #23).
- A full flash gives each installer image its own identity, so flashing an older installer can
  no longer leave a newer saved app update selected. Requires a full flash.
- A saved app update is kept and checked again on the next boot when the boot script could not
  finish verifying it, instead of being deleted. Requires a full flash.

## [1.2.1] - 2026-10-14

Adds Ogg Opus support and Windows installation, updates the installer experience and
startup animation, and fixes text, playback, Wi-Fi, screen wake and app-update handling.

1.2.0 users can install the signed app update from Settings > System > Update diskOS
if updates are allowed. Fresh installs and older versions use the installer.
App updates do not replace the boot install script. Fixes marked "Requires a full
flash" need the complete 1.2.1 installer.

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

## [1.2.0] - 2026-10-02

Adds V2.57 firmware support, more library and display choices, and signed in-app diskOS updates.
Installing 1.2.0 requires the complete installer and a new flash.

### Added

- Install from unmodified official V2.57 firmware, alongside V2.09, V2.28, and V2.40. The
  installer checks the Disc's existing kernel and recovery against the image before writing.
- Seven themes alongside Default, dark and light appearance, Outdoor Mode, font sizes, and an
  interface language choice with English fallback for missing text.
- Up Next shows the current queue and lets you jump to an upcoming song. Artists and Genres can
  open albums or All Songs.
- Library indexing for AAC, OGG, APE, AIFF, WMA, DSF, DFF, DTS, external CUE sheets, and SACD ISO
  track rows. Indexing does not guarantee playback of every format.
- Song Info with available audio and file details; settings for Album Artist grouping, Play
  Through Folders, track numbers, online album art, and online lyrics. Local and embedded lyrics
  remain available when online lyrics are off; timed lyrics can follow playback.
- Song lengths in library song lists and Up Next, read from the file when the library is scanned.
  Songs whose length can't be read show no length.
- Manual date and time, time-zone choice, and Automatic Time. A new startup animation has a
  selectable Disc body colour.
- On V2.57, screen rotation, volume-key actions, idle power-off, a charging limit, and a choice
  to pause or request shutdown when the Sleep Timer ends.
- Bluetooth Codec selection on V2.57. On other firmware the row says "Not on this firmware".
- Device Info, battery/board temperature, and a reset for diskOS appearance and behavior
  settings that keeps music, network, Last.fm, EQ, and audio settings.
- Signed in-app diskOS updates for later releases. The 1.2.0 release image includes the diskOS
  release key by default; its GUI checkbox, "Allow diskOS updates over Wi-Fi", starts checked.
  Untick it or use `--no-ota` to omit the key; `--ota-key PATH` uses your own public key.
  Settings > System > Update diskOS updates only the diskOS app, not stock firmware or the whole
  image. Allow diskOS Updates can stop downloads; turning it off with a staged update offers
  Discard or Cancel. A trial update offers Keep or Go back and rolls back if it fails to start.
  The first update over Wi-Fi will be a release after 1.2.0.
- A macOS release package for Apple Silicon with prebuilt flash tools.

### Changed

- Album artwork is now decoded by a new helper, `diskos-artdec`, instead of the stock `ffmpeg`
  command (which stock V2.57 ships without image decoders). It runs as a separate short-lived
  process and covers embedded and same-folder artwork on every supported firmware. Cached covers
  are rebuilt once after the update.
- Restart is back in Settings. It waits for diskOS card work and a successful flush; if either
  cannot finish, it reports the problem and leaves the Disc running.
- Library rescans show progress and a Stop action. Cancelling keeps the previous library.
- Playlists retain their order and appear in the stock playlist registry. You can add a displayed
  album, artist, or genre song list, remove a song, and rename, export, or delete a playlist. M3U
  import searches card folders within limits and preserves a differing same-name playlist.
- V2.57 Favourites use the player's favourite queue and row identity. Gain is available on V2.57;
  its Custom EQ editor is view-only.
- Update from SD Card identifies a file named like a stock update and explains the stock route;
  diskOS does not run that update.
- Wi-Fi lists place the connected network first and handle encoded names, passwords, and
  connection feedback more carefully.
- Setup and tool builds now work on Arch, CachyOS, and Fedora as well as Debian and Ubuntu;
  `doctor` identifies the Linux distribution.

### Fixed

- The installer reports a USB-permission problem when it sees a mask-ROM Disc but cannot open it.
- Saving the clock no longer waits on the interface loop for the RTC.
- Album cover caching no longer depends on an inode that can change for the same card file.
  Saved accent colours can refresh when cover content changes.
- Playlist export uses a unique temporary file instead of a fixed name that could overwrite a
  user's file.
- A refused playback send no longer opens Now Playing as if playback started. Delayed local-player
  setup no longer interrupts a song that has already begun. CUE and SACD track replay avoids a
  timeout route reset that could stop a live stream.
- A back swipe starting on a control no longer also taps that control, which could go back two
  screens or change a setting.
- Long one-button messages are no longer covered by the OK button.
- With Weather on Home off, diskOS no longer makes a weather request at startup.

## [1.1.3] - 2026-09-24

A safety release. The stock FiiO player, which diskOS runs alongside, tries to unmount the microSD
card and then deletes everything at the card's mount point, without checking that the unmount
worked. If the card is still mounted, this can delete its contents. The flaw is confirmed in the
stock player on V2.09, V2.28, and V2.40. It has not been proven to be the cause of the two reported
card wipes. Every diskOS user should update; updating requires the complete installer and a new flash.

### Added

- A delete guard in the flashed image, at `/bin/rm` and first on the stock player's `PATH` (so the
  stock fallback and watchdog launches are covered too). It refuses `rm` on the card or anything on
  it; the card's folder can only be removed when it is empty and unmounted. It blocks this specific
  deletion path, not every possible way to lose data.
- A boot selector that runs before diskOS starts and decides once per boot between diskOS and stock.
  Any missing or invalid result boots stock.
- Hold Volume Up for about three seconds to force-close an external app. A message says if this is
  unavailable.
- Extra punctuation in the 14, 16, 18, and 20 px interface fonts, and eight more CJK characters in
  the fallback font. This is not full CJK support.

### Changed

- USB Storage now waits for diskOS to finish reading and writing the card, confirms the card really
  was handed over and really came back, and only then resumes artwork caching. A pending handoff
  survives a UI restart.
- USB Storage is refused during a library scan, and whenever diskOS cannot confirm that the card is
  protected or who owns it. If a handoff is left uncertain, diskOS keeps card access off and asks for
  a reboot.
- Library scans, browsing, artwork, lyrics, book details, playlists, and playback commands wait for
  these card checks. If protection is unavailable, diskOS keeps its card features off and says so;
  the stock player still starts.
- On V2.40, cold-boot card handling tries the direct mount before sending an insert event.
- First-boot setup uses only the UI built into the flashed image; the SD-card fallback copy is gone.
- Restart is temporarily removed from Settings until it can wait for card writes to finish.
- Bluetooth starts off when there is no saved diskOS preference. Saved preferences are kept.
  Turning it on from Quick Settings now waits for it to become ready.
- Home, Now Playing, and Quick Settings share the same transport controls. The play/pause icon
  responds at once. Quick Settings skips 15 s back and 30 s forward in audiobooks.
- Quick Settings allows the transport row only with five or fewer tiles enabled.
- Settings values sit below their labels, controls use the current accent colour, and search fits
  the round screen better.
- Debug Mode's description now says SSH over Wi-Fi; the USB serial shell is for developer builds.
- The installer accepts only the unmodified official V2.09, V2.28, and V2.40 firmware for install
  and build. The untested-firmware override no longer applies to them. V2.57 is refused.
- Release packaging requires the boot selector, its probe and helper, and the delete guard, and the
  image check verifies all of them and the patched boot script.

### Fixed

- Stopping card access now cancels a running scan and all artwork work. A cancelled scan keeps the
  previous library.
- Artwork cache files are flushed to the card before they are used (where the card supports it);
  artwork and playlist exports also try to flush their folder.
- Player start-up and the hand-off to stock have firmer time limits, clean up their helper
  processes, and reset signals correctly.
- Default UI's boot flag is written by a time-limited helper that reports failure.
- Bluetooth status and audio-route checks are time-limited: the check is killed along with anything
  it started, and cleanup never waits without a limit. Bluetooth shutdown no longer waits on the
  Bluetooth service. This closes one way the screen could freeze, not every possible one.
- Scan messages now tell apart refused, running, failed, and finished scans, with a count of skipped
  files. Help hints no longer replace an active message.
- Failed playback, seek, and playlist-export requests now report failure.
- The mode picker keeps showing "switching" until a mode change finishes, and clears the checkmark
  if it fails.
- The boot log keeps earlier start-up messages until it reaches its size limit.

## [1.1.2] - 2026-09-20

### Fixed

- Install now works from a plain source checkout that does not include the bundled squashfs tools:
  the installer uses a system `mksquashfs`/`unsquashfs` from `PATH` (for example from the
  `squashfs-tools` package) when the vendored copies are absent. `usbboot` and the device files
  remain bundle-only. Previously a source checkout failed with "bundled tool 'unsquashfs' not found"
  even when squashfs-tools was installed.
- Added a squashfs LZO capability check that runs before the build and in `doctor`, so a squashfs
  tool built without LZO support fails with a clear, actionable message instead of failing partway
  through the build.

### Changed

- `doctor` now labels each native tool as bundled or system, flags broken entries, and reports the
  squashfs LZO check result. Tool-not-found guidance points at the correct per-platform build step
  instead of advising a re-download.

## [1.1.1] - 2026-09-20

### Fixed

- Fixed rootfs extraction on the default case-insensitive macOS filesystem, resolving E230
  `unsquashfs ... already exists` failures from filenames that differ only in case.
  The installer automatically creates, mounts, and verifies a case-sensitive APFS scratch image
  using `hdiutil`, uses it for extraction and validation, then detaches and removes it.
  Cleanup failures are reported.
- Kept the existing build path unchanged on Linux with a case-sensitive filesystem and on
  case-sensitive macOS volumes. Validated on real macOS with `back_home.png` and `BACK_HOME.png`
  coexisting after extraction, and on Linux. macOS device flashing remains unverified end to end.

## [1.1.0] - 2026-09-20

### Added

- V2.40 firmware support, flash-tested alongside V2.09 and V2.28 on Linux x86-64.
- Album cover flow with horizontal swipe navigation, pre-baked cover sprites, and reflections.
  Dynamic album lists and a bounded sprite cache accommodate libraries with thousands of albums.
- Dedicated Books view for single-file `.m4b` audiobooks with saved listening positions, plus
  chapter navigation from Now Playing (jump to a chapter, current chapter highlighted, with titles
  and durations).
- File/folder browsing on the microSD card, with playback of selected library tracks.
  Playback uses the all-songs queue, not a folder-only queue.
- Bundled Noto Sans glyphs for Latin-extended, Greek, and Cyrillic library text.
- On-device UI source under GPL-3.0-or-later, with vendored LVGL and build instructions.
  Installer, build tooling, and documentation remain MIT licensed.

### Changed

- Increased image and writer capacity from 580 to 768 NAND blocks (96 MiB) to fit V2.40's
  roughly 88 MB stock root filesystem, without changing the partition layout.
- Shortened the conservative flash wait; flashing and verification now take about 15 minutes.
- Separated `.m4b` entries from music views and queues, including migration of existing entries.
- Refreshed the circular listening interface and browsing views.

### Fixed

- Refuse undersized or unrecognized writers before any NAND write, and check reported capacity
  after flashing to detect writer/image mismatches.
- Repad older short saved stock images into a separate copy, then validate them before restore.
- Preserve USER EQ presets changed on the stock player when reselected in diskOS.

## [1.0.0-beta]

### Added

- Initial public release for the FiiO Snowsky Disc, flash-tested with V2.09 and V2.28 on Linux x86-64.
- Circular player interface with local playback, bezel and alphabet navigation, M3U playlist import,
  and album-art dynamic colors.
- Experimental weather and Last.fm integrations.
- Graphical and command-line installer using a local Python environment.
- Local image builds from user-supplied official FiiO firmware archives.
- Mask-ROM USB flashing with a bad-block-aware writer and block verification.
- Saved-stock restore, persistent stock UI selection, one-time stock boot, and stock UI fallback.
- Opt-in SSH Debug Mode and separate public and development installation variants.
- macOS build-from-source path, unverified for release artifacts and device flashing.
- MIT-licensed installer tooling, recovery documentation, hardware notes, and third-party notices.
  The initial release distributed the UI as a binary; UI source publication followed in v1.1.0.

[1.1.3]: https://github.com/b0hemia/diskos/compare/v1.1.2...v1.1.3
[1.1.2]: https://github.com/b0hemia/diskos/compare/v1.1.1...v1.1.2
[1.1.1]: https://github.com/b0hemia/diskos/compare/v1.1.0...v1.1.1
[1.1.0]: https://github.com/b0hemia/diskos/compare/v1.0.0-beta...v1.1.0
[1.0.0-beta]: https://github.com/b0hemia/diskos/releases/tag/v1.0.0-beta
