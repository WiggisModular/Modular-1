# Changelog

## Unreleased

## 1.1.0 (firmware `M1-1.1.0`, app 1.1.0)

### Added
- **Drum machine** as the third voice (SEL: voice 1, voice 2, drums): 16 steps with a trigger type per step (ON, ratchet 2-4, flam), a condition (probability or A:B) and a volume (ghost, normal, hard); 8 factory patterns with ghost notes and ratchets (PRESET knob or MIDI program change); PLAY starts and stops it, it follows MIDI clock; MIDI channel 10 (GM notes 36/38/42/46/39/56); its own mixer row.
- Six synthesized drum parts: kick, snare, closed hat, open hat, **clap** and **cowbell**. Every part has four controls on the SOUND page: tune, drive, decay and a fourth (kick: frequency delay, snare and hats: noise amount, clap: spread, cowbell: tone). The MIX page (ARP button again) has the levels, the tempo and the pattern length.
- The 16 steps are the 16 white keys; the six parts are the black keys C#4 D#4 F#4 G#4 A#4 C#5. A switched-on step lights its key, the beats (steps 1, 5, 9, 13) glow dimly.
- **Simulator tab**: the firmware runs on an emulated FM-1 (fm1-emu, MIT) in the web app.
- **Chip oscillator** module (NES and Game Boy sounds with the fast chip arpeggio) and two factory patches; switches show OFF/ON on the LCD. The oscillator morphs continuously between sine, triangle, saw and square.
- A new set of 32 factory patches.
- MELODY page: the scale shows its number of notes, e.g. `Maj Pentatonic (6)`.
- Arpeggiator modes (K1 on the MELODY page, appended after RANDOM so stored states keep their mode): **DOUBLE** (up and down, every note twice), **WAVE** (overlapping triples 1 2 1, 2 3 2, 3 4 3 ... up, then the mirror image down), **JUMPS** (1 n 2 n-1 3 n-2 ... to the middle and back, the turning notes once).

### Changed
- Two patch voices instead of four, to make room for the drums.
- The drums are much louder (every part peaks near 0.95 at level 100; the hats were far too quiet), the default drum level is 90.
- The saved state is version 5 (six drum parts and their controls); states saved by 1.0.x and by the earlier drum versions still load (clap and cowbell at their defaults).

### Fixed
- The cursor frame of a drum matrix cell was left on the display when the cursor moved on.
- Long drum decays end instead of ringing on; the Game Boy short noise cannot lock at zero; the drum keys' edge memory follows every page.
- MIDI clock check: only two arpeggiators exist.

### Not verified
- Host-tested (`tools/arp_test.sh`, `tools/golden.sh`) and previewed on the host only. The two voices, the drums, their LEDs and key layout, and the simulator tab have not been run on a real FM-1, and the CPU use of two voices plus drums has not been measured.

## 1.0.3 (firmware `M1-1.0.3`, app 1.0.3)

### Fixed
- The reverb module was invisible in the patch diagram on the display (no colour was defined for it, so its box and label were black on black). It is now sea green with the label RE.

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
