# LS-Exp
LS-Exp is an **expander** for [LSystem](LSystem_manual_EN.md). It reads the LSystem's live sequence and exposes it as normal VCV signals, adds external scale/root control, and mirrors the running sequence on its own display. It connects to the side of the LSystem module; place it physically adjacent (no gap) and it is recognized automatically. It needs LSystem present to work; without it the display shows **"not connected"** and the outputs send 0V.

If an LSystem has expanders on both sides, only the right one is active — the left one is disabled.

## Outputs

- **DEGREE**: polyphonic. The current scale degree, packed in Meander-style `degree.octava` (see below). **0.0V = silence/rest** (no note).
- **STEP**: polyphonic, 0-10V. The current step's position inside the current repetition of the active rule. A rule with a single step returns 0V.
- **SCALE**: monophonic/polyphonic copy of the active scale (mode-dependent, see *Scale formats*). Missing tones read 0.0V.
- **ROOT**: monophonic. The current root note. By default as raw V/oct; optionally in Meander's circle-of-fifths "Root CV" format (see *Root and Degree output options*).

### The `degree.octava` format (Meander-compatible)

The **Degree** output reports the scale degree currently playing in the same packed decimal format Meander uses, so the two can talk to each other.

- **Integer part** = scale degree, **1-based** (1 = I, 2 = II, ...; octave = first decimal digit).
- **0.0V** = silence (rest / no active note).
- Example: `3.4V` reads as degree III, octave 4.

How the value is built from the LSystem's absolute degree:

```
octave        = floor((absoluteDegree - 1) / scaleLength)
degreeInScale = ((absoluteDegree - 1) mod scaleLength) + 1
voltage       = degreeInScale + (octave * 0.1)
```

The octave comes from the note's true register: every full traversal of the scale adds one octave. It is centered so the tonic (degree 1) sits at octave 4, and is clamped to the 0-7 range.

Compatibility notes with Meander:

- Degree numbering (`1.0=I ... 7.0=VII`) and the packing match Meander.
- The octave is **absolute** (derived from the real register, centered at 4), not relative to a module-set target octave. This makes the output directly usable as a pitch reference.
- `0.0V` = silence is the same "no note / skip" sentinel Meander uses.
- Scales with **more than 7 notes** can emit degrees above 7, which goes beyond Meander's heptatonic domain but keeps the same encoding.
- Octaves clamp to 0-7, matching Meander's octal octave range.

## Inputs

- **SCALE**: polyphonic external scale. The LSystem uses it as its active scale. Its format is set with *Scale format IN* in the context menu (see below).
- **ROOT**: monophonic root. When connected it becomes the tonic, overriding any root embedded in the scale input. When disconnected, the root comes from the scale input (embedded 10V root on 12-channel modes) or, failing that, the lowest note of the scale; in Free/Microtonal the LSystem's internal key is ignored.

## Scale formats

Both the *Scale format IN* (how the SCALE input is read) and *Scale format OUT* (how the SCALE output is written) are chosen independently from the expander's context menu. These are the available formats:

- **Heptatonic Chromatic-12ch**: 12 channels, categorical. 0V = off, 8V = tone in the scale, 10V = root.
- **Heptatonic Diatonic STD-7ch** *(default)*: 7 channels, raw V/oct.
- **Pentatonic-5ch**: 5 channels, raw V/oct (a heptatonic scale with the 4th and 7th degrees removed; a non-heptatonic source simply uses its first 5 tones).
- **Pentatonic Chromatic-12ch**: 12 channels, categorical, with 5 active tones.
- **Free / Microtonal**: 1-16 channels, raw V/oct. The tones are deduplicated and sorted ascending; the lowest tone is the default root.
- **LSystem Index**: 0-10V. On the **IN** side, SCALE's first channel selects one of the LSystem's internal scale presets by index. On the **OUT** side, SCALE reports the index of the currently active LSystem scale (regardless of any external scale that may be sounding).

For modes with categorical output (the two 12-channel ones), the tones are derived from the active scale: each active semitone marks 8V, the root marks 10V. For raw V/oct modes, each channel copies the corresponding scale tone.

## Display and the channel button

The expander mirrors the running sequence for one channel at a time. The **CHN** button (next to the display) cycles the display between the module's active channels (up to 6).

For the selected channel the display shows:

- **SCALE**: the name of the active scale (*External* if an external scale is fed).
- **ROOT**: the current root note name.
- **DEGREE**: the current degree, as a number or, optionally, as Roman numerals (I-VII).
- **RULE**: the current rule number.
- **NOTE**: the current note name. In STD-7ch, Pentatonic and Free/Microtonal modes, microtonal deviations are shown as an offset in cents (e.g. `C# +23`).

## Root and Degree output options

### Root format OUT

The ROOT output can be switched between two encodings:

- **V/oct (raw)** *(default)*: the root as V/oct, e.g. 0.0V = C4.
- **Circle of fifths (Meander)**: the root in the "Root CV" format Meander's
  ROOT input expects. Each pitch class is a voltage, ordered along the circle
  of fifths so a CV sweep moves through the cycle of fifths for harmonic
  modulation:

  `C 0.5V, G 1.0V, D 2.0V, A 3.0V, E 4.0V, B 5.0V, F# 6.0V, Db 7.0V, Ab 8.0V, Eb 9.0V, Bb 9.5V, F 10.0V`

  This format carries no octave information, so the ROOT output in this mode
  reports the pitch class only.

### Degree octave offset

The **Degree octave offset** option shifts the octave field of the
`degree.octava` output by **-2 / -1 / 0 (default) / +1 / +2** octaves while
keeping the same encoding. A positive offset transposes everything up by that
number of scale octaves when the Degree output drives a Meander-style quantizer;
a negative offset transposes down. The packed octave stays clamped to Meander's
0-7 range.

## Context menu

- **Scale format IN** / **Scale format OUT**: choose the scale format for the input and the output (see *Scale formats*).
- **Step includes repetitions**: when on, the STEP output spans the whole rule including all `*N` repetitions instead of resetting at each repetition.
- **Roman numerals**: show DEGREE as Roman numerals (I-VII). Automatically disabled for scales with more than 7 notes; limited to the scale length for smaller scales.
- **Root format OUT**: raw V/oct (default) or circle-of-fifths "Root CV" for Meander (see *Root and Degree output options*).
- **Degree octave offset**: -2 / -1 / 0 (default) / +1 / +2 octaves for the DEGREE output's octave field.

Settings are saved with the patch and restored on load.