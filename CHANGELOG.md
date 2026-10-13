# Changelog

## 1.0.2 (firmware `M1-1.0.2`, app 1.0.2)

### Fixed
- The PRESET knob scrolls the MANUAL page and moves between rows in the settings menu; before, it loaded the next patch even with the menu open (edits on that voice were lost).
- Holding OCT- and OCT+ right as the loading screen ends no longer drops the FM-1 into the update loader at once: the 5 s hold now counts from the end of the splash.
- During the 5 s loading screen the FM-1 answers the editor, the flasher and the UBOOT request, and plays incoming MIDI notes, instead of ignoring them until the screen is gone.
- Fewer redundant redraws at the end of boot.
- Host preview tool: the menu tab and manual line arguments are clamped.

### Not verified
- Not yet seen on the display.

## 1.0.1 (firmware `M1-1.0.1`, app 1.0.1)

### Added
- **Loading screen:** the name, the version, a `LOADING INPUT / AUDIO / USB / MIDI / STATE / VOICE n` line and a bar, shown while the FM-1 starts and the patches load, and for at least 5 seconds. The pages stay hidden until it is done; the restore note (STATE RESTORED or RESTORE FAILED) follows in the top bar.
- **MANUAL tab in the settings menu:** one scrollable page (SELECT scrolls) with the controls, the MIDI mapping, the editor address and the memory limits. Control names are in the accent colour.

### Changed
- Wording: a stored sound is called a **patch** on the device and in the web app (tab "Patches", "Patch bank", "Factory patches", "Load patch…"). Only the hardware PRESET knob keeps its name.

### Not verified
- The loading screen and the manual page were checked in a host preview and flashed, but not reviewed on the display by me.

## 1.0 (firmware `M1-1.0`, app 1.0.0)

Four voices, a mixer, a settings menu and a complete saved state. First version that is no longer a bring-up.

### Voices
- Four independent patches play at once (`ENG_VOICES` 4). SEL cycles the voice that the keys, knobs, LCD and editor act on; each voice has its own accent colour (orange, purple, cyan, green), arpeggiator and MIDI channel (defaults 1 to 4).
- Voices 2 to 4 use voice 1's arpeggiator scale.
- Limits per voice: 24 modules, 128 cables, one delay (up to 521 ms) and one reverb. The browser engine uses the same delay length, so the web app sounds like the device.

### Panel
- **Mixer (GLO):** K1 to K4 set the voice levels; the black keys F#3, G#3, A#3 and C#4 (OP1 to OP4) mute voices 1 to 4 and light their LEDs while muted.
- **Settings menu (hold HOME):** tabs DISPLAY, CONTROL, MIDI and SYSTEM in Felucca's layout. Rows: LEDS (off, dim low, dim high), CPU in bar, knob acceleration, octave, V1 to V4 MIDI channel, MIDI clock in, program change, version, CPU peak, update mode and DEFAULTS (restores everything and erases the saved state).
- The top bar shows the preset name and the CPU load.
- LED glow: DIM LOW is the former glow, DIM HIGH is brighter.

### Saved state
- SAVE stores the whole state in one record: all four patches including knob edits, all arpeggiators, mixer levels and mutes, menu settings, MIDI channels, octave, and each voice's module and preset. Older records still load.
- The bar shows STATE RESTORED after boot (RESTORE FAILED if a saved record did not load).

### Main module
- New gate output `clock` (24 ppqn): from the arpeggiator while it runs, else from MIDI clock; silent without a source.
- The delay has tempo sync (it follows that clock), note-length division and ping-pong.

### Changed
- The Drone preset's delay is 500 ms (was 700 ms) because of the shorter delay line.
- The editor's HELLO reply reports the new limits (24 modules, 128 cables, 1 delay); patch size limit is 2048 bytes.

### Not verified
- Four voices on the device: CPU load and audio quality under load.
- That the OP3 and OP4 keys are A#3 and C#4.
- TRS MIDI input with a real source.
