// LSystemExpander.hpp
// Shared message structs and helpers between LSystem (host) and LS-Exp (expander).
// To be used to pass state through VCV Rack's native expander system.
#pragma once
#include <rack.hpp>
#include <string>
#include <vector>
#include <cmath>

using namespace rack;

namespace lsxp {

// Scale modes, selected explicitly via expander context menu (no autodetection).
enum ScaleMode {
    MODE_CHROMATIC_12CH = 0, // Heptatonic Chromatic-12ch: 12ch categorical (0=off, 8=on, 10=root)
    MODE_STD_7CH = 1,        // Heptatonic Diatonic STD-7ch (default): 7ch raw V/oct
    MODE_PENTA_5CH = 2,      // Pentatonic-5ch: 5ch raw V/oct
    MODE_PENTA_CHROM_12CH = 3, // Pentatonic Chromatic-12ch: 12ch categorical, 5 active
    MODE_LIBRE = 4,          // Free / Microtonal: 1-16ch raw V/oct, sorted, lowest = root
    MODE_LSYSTEM_INDEX = 5,  // LSystem Index: 0-10V indexes the LSystem's internal scale presets
    NUM_SCALE_MODES
};

inline const char* scaleModeName(int m) {
    switch (m) {
        case MODE_CHROMATIC_12CH: return "Chromatic-12ch";
        case MODE_STD_7CH: return "STD-7ch";
        case MODE_PENTA_5CH: return "Pentatonic-5ch";
        case MODE_PENTA_CHROM_12CH: return "Pent. Chrom-12ch";
        case MODE_LIBRE: return "Free / Microtonal";
        case MODE_LSYSTEM_INDEX: return "LSystem Index";
        default: return "?";
    }
}

// Expander -> LSystem. Written by the expander each process() frame.
struct ExpanderToLSystem {
    bool active = false;
    int scaleMode = MODE_STD_7CH;
    // Raw scale input voltages as read from the expander SCALE_IN port,
    // before interpretation (up to 16 channels for Libre).
    float scaleVoct[16] = {};
    int scaleChans = 0;
    bool scaleConnected = false;
    float rootVoct = 0.f;
    bool rootConnected = false;
};

// LSystem -> Expander. Written by LSystem each process() frame.
struct LSystemToExpander {
    bool active = false;
    int numChannels = 1;
    int scaleIndex = 0;
    int scaleLen = 7;
    float rootVoct = 0.f; // internal root as V/oct (C4 = 0V)
    // Per-channel live state (up to 6 voices).
    int absDegree[6] = {1, 1, 1, 1, 1, 1};
    bool isRest[6] = {};
    bool isSilent[6] = {};
    int ruleIdx[6] = {};
    // Step position within current repetition (per-rep, default Step behavior).
    int stepIdx[6] = {};
    int stepTotal[6] = {1, 1, 1, 1, 1, 1};
    // Step position across the whole rule including *N repetitions (optional mode).
    int stepIdxWhole[6] = {};
    int stepTotalWhole[6] = {1, 1, 1, 1, 1, 1};
    // Active scale as voltages, for SCALE_OUT thru (mode-dependent encoding
    // is applied by the expander when writing outputs).
    float scaleVoct[16] = {};
    int scaleVoctChans = 0;
    bool externalScale = false;
};

// Model pointers (defined in the .cpp files, declared here for expander checks).
// modelLSystem and modelLSExp live in plugin.hpp; this header only carries payloads.

// Degree encoding: LSystem absolute degree -> Meander-style packed degree.octave.
struct DegreeResult {
    float voltage = 0.f;
    bool clipped = false;
    int degreeInScale = 1;
    int relativeOctave = 4;
};

inline DegreeResult encodeDegree(int absoluteDegree, int scaleLength) {
    DegreeResult r;
    if (scaleLength < 1) scaleLength = 1;
    int zb = absoluteDegree - 1;
    // floorDiv/floorMod handle negative degrees correctly (C++ % truncates).
    int q = zb >= 0 ? zb / scaleLength : -((scaleLength - 1 - zb % scaleLength) % scaleLength == 0 && false ? 0 : 0);
    // Use portable floor division:
    int d = zb / scaleLength;
    int m = zb % scaleLength;
    if (m != 0 && ((m < 0) != (scaleLength < 0))) { d--; m += scaleLength; }
    q = d;
    int degIn = m + 1;
    r.degreeInScale = degIn;
    int meanderOct = q + 4; // center octave offset 4 (range 0-7)
    if (meanderOct < 0) { meanderOct = 0; r.clipped = true; }
    if (meanderOct > 7) { meanderOct = 7; r.clipped = true; }
    r.relativeOctave = meanderOct;
    r.voltage = (float)degIn + (float)meanderOct * 0.1f;
    return r;
}

inline float stepVoltage(int idx, int total) {
    if (total <= 1) return 0.f;
    if (idx < 0) idx = 0;
    if (idx > total - 1) idx = total - 1;
    return (float)idx * 10.f / (float)(total - 1);
}

} // namespace lsxp
