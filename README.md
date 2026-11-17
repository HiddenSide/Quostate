# Quostate

Plugin for VCV Rack 2 with generative, rule-based sequencing.

## Modules

- **LSystem** — degree/duration sequencer driven by L-system-style rules that route into each other, with random degrees and durations, weighted lists, glides, repetitions, and up to 6 polyphonic voices. [Manual](LSystem_manual_EN.md)
- **LS-Exp** — expander for LSystem: external scale/root control, degree/step/scale/root outputs (Meander-compatible `degree.octava` and circle-of-fifths Root), and a live display. [Manual](LS-Exp_manual_EN.md)
- **MorphfastMini** — clock-synced voltage transition generator with Ramp, RampBack and Step curves, delay and easing. [Manual](MorphFastMini_manual_EN.md)

## Build

Requirements: the Rack 2 SDK (`make` expects `RACK_DIR` to point to it), `make`, and `zstd` (used to package the plugin).

```sh
export RACK_DIR=/path/to/Rack-SDK
make          # builds plugin.so
make dist     # packages dist/<name>-<version>-<platform>.vcvplugin
```

## License

GPL-3.0-or-later — see [LICENSE](LICENSE).