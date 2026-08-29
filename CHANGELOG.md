# Changelog

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
