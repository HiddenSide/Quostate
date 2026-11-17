# Changelog

## Version: "2.4.0"

**LS-Exp: Circle-of-fifths Root output**: a new *Root format OUT* option emits
  the root in the circle-of-fifths "Root CV" format Meander expects (C 0.5V,
  G 1.0V, D 2.0V, A 3.0V, E 4.0V, B 5.0V, F# 6.0V, Db 7.0V, Ab 8.0V,
  Eb 9.0V, Bb 9.5V, F 10.0V). Default remains raw V/oct.

**LS-Exp: Degree octave offset**: a new *Degree octave offset* option shifts the
  octave field of the `degree.octava` Degree output by -2 / -1 / 0 (default) /
  +1 / +2 octaves, keeping the same encoding (clamped to Meander's 0-7 octave
  range).

## Version: "2.3.3"

**Duration-0 last step no longer consumes a tick**: a duration-0 final step
  (used for routing) was previously enqueued as a 1-subpulse event, which
  consumed one tick and could cause phase drift. The engine now loops over
  the event queue until a non-silent event or a silent event with ticks
  remaining is found, so duration-0 routing steps are consumed instantly
  without wasting a tick.

**Rule-confirm desync fixed**: `applyRecompile()` and `resetAllEngines()`
  now run before `fireInternalTick()` at the downbeat, so the new rules are
  in place when the downbeat fires. Previously the downbeat fired with the
  old rules, causing a persistent ~8% phase offset on every confirm.

## Version: "2.3.2"

**LSystem clock phase stability**: fixed the race condition between the UI
  thread (`recompileAll`) and the audio thread (clock interpolation loop) on
  `pulseSubdivision` and grid state (`fracPos`, `nextBoundary`). Two-level fix:
- `pulseSubdivision` is now `std::atomic<int>` with release/acquire
  synchronization, initialized to `GATE_MIN_SUBDIVISION` (20) to eliminate the
  D=1→20 jump on first recompile.
- **Deferred recompile**: `recompileAll()` is no longer called directly from
  the UI thread (rule edits, pool changes, scale changes). Instead, an atomic
  flag (`pendingRecompile`) is set, and the actual recompile runs on the audio
  thread at the next clock downbeat. This guarantees the grid reset
  (`fracPos=0, nextBoundary=1`) always happens at a known safe point
  synchronized with the clock edge — the audio thread reads and writes
  `fracPos`/`nextBoundary` on the same thread, eliminating the data race.
- The inner-subpulse loop uses `while` (restored) to correctly fire all needed
  ticks per frame at fast clock rates.
- Grid reset and `alignHold` on subdivision change are retained.

**LSystem Gate width honored on whole-pulse steps**: the module now forces a
  minimum per-pulse subdivision (20) so the "Gate width" menu option (10/25/50/
  75/90/100%) takes effect even when every rule uses whole-pulse durations
  (previously these steps always sounded at 100% width). 20 is the LCM of the
  gate-width denominators, so every menu percentage maps to an exact number of
  internal sub-steps. The downbeat remains anchored to the incoming clock edge;
  the extra subdivision only adds resolution inside each pulse.

**LS-Exp: degree 0 is a valid note**: a degree of `0` is now treated as the last
  scale degree one octave below, instead of being shown as silence. The Degree
  output and the display no longer collapse degree 0 to `--`/0V; only explicit
  rests (`s`) and routing-silent steps do.

## Version: "2.2.2"

**MorphfastMini LCD font load fixed**: the display font is now loaded inside the
  draw callback instead of the widget constructor, so it stays valid when the
  plugin editor window is reopened in a DAW host (new OpenGL context). Resolves
  the VCV library pattern-check warning.


## Version: "2.2.1"

**MorphfastMini STEP curve fixed**: the Step curve now interpolates from Init to
  Target one step per clock pulse across the Duration, instead of jumping
  straight to Target on the first clock edge. Easing shapes the height of each
  step.


## Version: "2.2.0"

**Added new module**: MorphfastMini.

**LSystem**:

**Clock and timing model rewritten**: from "48 PPQN / fixed ticks" to
  "1 PPQN with automatic internal resolution" (subpulses = least common multiple
  of the denominators used, capped at 96); timing is anchored to the incoming
  clock pulses, the module holds its state if a pulse arrives late, and two
  consecutive pulses are required before sub-beat timing is available.


**New "Rule matching order" section**: exact match (degree+duration) →
  initiator with the closest duration → topmost rule in row order → fallback.


**Field limits**: each rule field accepts up to 50 characters, and
  a rule beginning with `--` is ignored.


**`l` operator expanded**: now returns the last list value for the degree or
  the duration, can be used inside `<...>` lists, and defaults to degree 1 /
  duration 1 if no list has been evaluated yet.
 

**New `=` (completing duration) operator**: `=T` completes the
  repetition up to the nearest multiple of T; applied per repetition with `*N`;
  if already a multiple it stays as a one-subpulse routing step; not allowed in
  the initiator or inside `<...>` lists.


**Glide `^`**: implementation details added (stepped pitch slide, at least 16
  steps per pulse, up to 96, roughly 25 cents per step, not sample-accurate).


**Silence `s`**: can now be used as an initiator, allowing a rule to be
  triggered after a rest.


**Duration `0`**: a routing/skip marker (omitted mid-rule; kept
  as a one-subpulse routing step at the end; becomes 1 subpulse in an initiator).


**`r` pool**: 24-character limit per field.


**Weighted lists**: clarified that weights must be positive numbers.


**Repetitions**: maximum repeat count of 256 (`*N`).


**Eval input**: polyphonic per-channel Eval voltage documented (a monophonic
  signal uses channel 1 for all voices).


**Autoreset after steps**: clarified as "beats" (the restart always lands on a
  beat) instead of "steps".


**Randomize rules style**: the 4 profiles described (Melodic / Acid-Techno /
  Ambient / Complex Chaos), and "Randomize Rules" is noted to also generate the
  `r` pools according to the chosen style.


**Durations section rewritten**: subpulses, LCM capped at 96, maximum of
  1,000,000 subpulses, and rounding up to 1 subpulse as the minimum duration.
