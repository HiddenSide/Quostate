#pragma once
#include <vector>
#include <string>
#include "LSystemEngine.hpp"

namespace lsys {

// scale degree -> MIDI note, with octaves for degrees <=0 or > scale size
inline int degreeToNote(int degree, const std::vector<int>& scale, int rootNote) {
    int n = (int)scale.size();
    int zeroBased = degree - 1;
    int idx = floorMod(zeroBased, n);
    int octave = floorDiv(zeroBased, n);
    return rootNote + scale[idx] + octave * 12;
}

struct ScalePreset {
    const char* name;
    std::vector<int> semitones;
};


inline const std::vector<ScalePreset>& getScalePresets() {
    static const std::vector<ScalePreset> presets = {
        {"Major (Ionian)",       {0, 2, 4, 5, 7, 9, 11}},
        {"Natural minor (Aeolian)", {0, 2, 3, 5, 7, 8, 10}},
        {"Harmonic minor",       {0, 2, 3, 5, 7, 8, 11}},
        {"Melodic minor",        {0, 2, 3, 5, 7, 9, 11}},
        {"Dorian",                {0, 2, 3, 5, 7, 9, 10}},
        {"Phrygian",              {0, 1, 3, 5, 7, 8, 10}},
        {"Lydian",                {0, 2, 4, 6, 7, 9, 11}},
        {"Mixolydian",            {0, 2, 4, 5, 7, 9, 10}},
        {"Locrian",               {0, 1, 3, 5, 6, 8, 10}},
        {"Major pentatonic",      {0, 2, 4, 7, 9}},
        {"Minor pentatonic",      {0, 3, 5, 7, 10}},
        {"Chromatic",             {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
        {"Harmonic major",        {0, 2, 4, 5, 7, 8, 11}},
        {"Phrygian dominant",     {0, 1, 4, 5, 7, 8, 10}},
        {"Blues",                 {0, 3, 5, 6, 7, 10}},
        {"Whole tone",            {0, 2, 4, 6, 8, 10}},
        {"Octatonic (Half-whole)",{0, 2, 3, 5, 6, 8, 9, 11}},
        {"Hirajoshi (Japan)",     {0, 2, 3, 7, 8}},
        {"In Sen (Japan)",        {0, 1, 5, 7, 10}},
        {"Balinese (Pelog)",      {0, 1, 3, 7, 8}},
        {"Hungarian minor",       {0, 2, 3, 6, 7, 8, 11}},
        {"Double harmonic (Byzantine)", {0, 1, 4, 5, 7, 8, 11}},
    };
    return presets;
}

inline const std::vector<const char*>& getNoteNames() {
    static const std::vector<const char*> names = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    return names;
}

} // namespace lsys
