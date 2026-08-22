// LSystem.cpp
#include "plugin.hpp"
#include "LSystemEngine.hpp"
#include "MusicUtils.hpp"
#include "components.hpp"
#include "RuleGenerator.hpp"
#include <mutex>
#include <cmath>
#include <random>
#include <functional>
#include <algorithm>


using namespace lsys;

// =======================================================================
// MODULE
// =======================================================================

struct LSystemModule : Module {
    enum InputIds { CLOCK_INPUT, RESET_INPUT, RUN_INPUT, EVAL_INPUT, NUM_INPUTS };
    enum OutputIds { PITCH_OUTPUT, GATE_OUTPUT, EOR_OUTPUT, RULE_OUTPUT, NUM_OUTPUTS };
    enum ParamIds { RUN_PARAM, RESET_PARAM, NUM_PARAMS };
    enum LightIds { RUN_LIGHT, NUM_LIGHTS };

    static constexpr int NUM_FIELDS = 7;
    static constexpr int MAX_CHANNELS = 6;
    // Global tunables for the two text-field character caps (per user testing).
    static constexpr int RULE_FIELD_MAX_CHARS = 50;
    static constexpr int LIST_FIELD_MAX_CHARS = 24;

    using GenStyle = lgen::GenStyle;
    static constexpr GenStyle STYLE_MELODIC = lgen::STYLE_MELODIC;
    static constexpr GenStyle STYLE_ACID_TECHNO = lgen::STYLE_ACID_TECHNO;
    static constexpr GenStyle STYLE_AMBIENT = lgen::STYLE_AMBIENT;
    static constexpr GenStyle STYLE_COMPLEX_CHAOS = lgen::STYLE_COMPLEX_CHAOS;
    static constexpr int NUM_GEN_STYLES = (int)lgen::NUM_GEN_STYLES;

    int genStyle = STYLE_MELODIC;

    enum EvalMode {
        EVAL_RULE_SELECT = 0, // CV 0-10V: selects target rule (queued at EOR, instant with Reset)
        EVAL_HOLD_GATE,       // Gate (>2V): loops/holds current rule while high
        EVAL_STEP_TRIGGER,    // Trigger: advances/evaluates next rule only when trigger arrives at EOR
        NUM_EVAL_MODES
    };
    EvalMode evalMode = EVAL_RULE_SELECT;
    dsp::SchmittTrigger evalInputTrigger[MAX_CHANNELS];
    bool evalTriggered[MAX_CHANNELS] = {};

    std::string fieldText[NUM_FIELDS];
    bool fieldError[NUM_FIELDS] = {};
    std::string fieldErrorMsg[NUM_FIELDS];
    RuleKey fieldKey[NUM_FIELDS];
    bool fieldKeyValid[NUM_FIELDS] = {};

    // "Committed" = last version of each field that compiled without errors.
    // The engine's rule table is always built from these, never from the raw
    // (possibly currently-invalid) fieldText, so a bad edit only turns that one
    // field red instead of silently deleting a working rule and stalling playback.
    std::string fieldTextCommitted[NUM_FIELDS];
    RuleKey fieldKeyCommitted[NUM_FIELDS];
    bool fieldKeyCommittedValid[NUM_FIELDS] = {};

    int activeField = -1;

    // Toggled by the momentary RUN_PARAM button (see process()); this is the
    // actual persistent run/stop state, since the CKD6 button itself springs
    // back to 0 as soon as it's released.
    bool running = true;
    dsp::SchmittTrigger runButtonTrigger;
    dsp::SchmittTrigger runInputTrigger;

    // Configuración
    FallbackMode fallback = FallbackMode::LOOP_TO_INITIATOR;
    int gradeMin = -8, gradeMax = 16;
    int scaleIndex = 0;
    int rootNote = 60;
    int numChannels = 1;
    bool resetOnRun = true;
    // 0 = disabled: EOS fires only when the sequence "completes" (Rule 1, the
    // initiator, fires again). >0: measured in beats -- every (autoResetSteps *
    // PPQN) internal ticks (i.e. autoResetSteps quarter notes), this channel
    // autoresets internally
    // (equivalent to a Reset pulse, but with no cable-latency artifact) and
    // EOS fires only at that instant, decoupled from the rule engine entirely
    // so it can be used as a precise clock division for other modules.
    int autoResetSteps = 0;
    std::vector<int> scale = getScalePresets()[0].semitones;

    // Optional restricted candidate lists for 'r' (see ListTextField below).
    // Empty text = current behavior (full range / built-in duration pool).
    std::string rGradeListText;
    std::string rDurationListText;
    bool rGradeListError = false;
    bool rDurationListError = false;

    // Estado del motor polifónico
    LSystemEngine engines[MAX_CHANNELS];
    std::mutex engineMutex;

    dsp::SchmittTrigger clockTrigger;
    dsp::SchmittTrigger resetTrigger;
    dsp::SchmittTrigger resetButtonTrigger;
    dsp::SchmittTrigger runTrigger;

    // ---- Clock front-end (1 PPQN input) --------------------------------
    // The external clock delivers ONE pulse per quarter note. The engine,
    // however, runs at its native resolution of PPQN (48) internal ticks per
    // beat, so this front-end does the multiplying: it measures the interval
    // between consecutive pulses (= 1 beat), divides it by PPQN and generates
    // the internal ticks with a fractional phase accumulator, re-anchoring the
    // beat boundary on every incoming pulse. Rule parsing, durations, glide,
    // EOR and autoreset therefore behave exactly as before -- they only ever
    // see internal ticks.
    static constexpr int64_t CLOCK_MIN_BEAT_SAMPLES = 64; // rejects glitchy ultra-fast pulses
    static constexpr double CLOCK_GAP_ADOPT_RATIO = 1.5;  // gaps up to 1.5x the current beat adopt instantly
    static constexpr double CLOCK_GAP_CONFIRM_TOL = 0.25; // tolerance when confirming an oversized gap
    double samplesPerTick = 0;       // beat interval / PPQN, in samples
    double tickPhase = 0.0;          // [0..1) progress toward the next internal tick
    int64_t lastPulseSamplePos = -1; // sampleCounter value at the previous pulse
    int64_t sampleCounter = 0;       // incremented once per process() frame
    bool haveClockTempo = false;     // false until the first full beat has been measured
    bool clockFrozen = false;        // true while holding output (next pulse overdue)
    double pendingBeatSamples = 0.0; // oversized gap awaiting confirmation (see process())
    // Armed by every reset (input, button, AAS autoreset, patch load): the
    // sequence holds frozen until the NEXT incoming clock pulse, so a restarted
    // sequence lands exactly ON that pulse -- aligned with anything else
    // clocked from the same LFO (kick, etc.). Cleared by that first pulse.
    bool awaitingClockAfterReset = false;

    int ticksRemaining[MAX_CHANNELS] = {};
    bool gateHigh[MAX_CHANNELS] = {};
    float currentPitch[MAX_CHANNELS] = {};
    int retrigSamplesLeft[MAX_CHANNELS] = {};
    int autoResetPulseCount[MAX_CHANNELS] = {}; // internal ticks since last autoreset
    bool gliding[MAX_CHANNELS] = {};
    float glideStepV[MAX_CHANNELS] = {}; // V/oct added to currentPitch on every clock pulse while gliding
    // 0-10V, linearly mapped across the 7 rule rows (Rule 1 = 0V ... Rule 7 = 10V),
    // held per-channel between rule changes. Replaces the old EOS trigger output.
    float ruleVoltage[MAX_CHANNELS] = {};

    dsp::PulseGenerator eorPulse[MAX_CHANNELS];

    bool wasRunning = false;

    LSystemModule() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(RUN_PARAM, 0.f, 1.f, 0.f, "Run");
        configParam(RESET_PARAM, 0.f, 1.f, 0.f, "Reset");
        configInput(CLOCK_INPUT, "Clock (1 PPQN, 1 pulse per beat)");
        configInput(RESET_INPUT, "Reset");
        configInput(RUN_INPUT, "Run Toggle (trigger)");
        configInput(EVAL_INPUT, "Eval");
        configOutput(PITCH_OUTPUT, "V/Oct");
        configOutput(GATE_OUTPUT, "Gate");
        configOutput(EOR_OUTPUT, "End of Rule Trigger");
        configOutput(RULE_OUTPUT, "Rule Number (0-10V)");

        // Reglas de inicio: set validado (compila sin errores) que muestra todas
        // las capacidades: iniciador, grados fijos, silencio, aleatorio simple,
        // listas aleatorias ponderadas (grado y duración), k (recordar el último
        // aleatorio), desplazamientos cromáticos +N/-N y repetición *N.
        fieldText[0] = "1,1 -> r,1/2 <3,5,7>,1/2";
        fieldText[1] = "3,1/2 -> k+2,1/4 k,1/4 1,1/2 *2";
        fieldText[2] = "5,1/2 -> <1:2,r:1>,1/2 k,k";
        fieldText[3] = "7,1/2 -> 8,1/4 7,1/4 5,1/2";
        fieldText[4] = "8,1/4 -> k+1,1/4 k-1,1/4 s,1/4";
        fieldText[5] = "s,1/2 -> 1,1/2";
        fieldText[6] = "1,1/4 -> <2,4,6:2>,<1/4:2,1/8:1> k,k *2";

        for (int i = 0; i < NUM_FIELDS; i++) fieldTextCommitted[i] = fieldText[i];

        recompileAll();
        resetAllEngines();
        

    }

    void recompileField(int idx) {
        fieldError[idx] = false;
        fieldErrorMsg[idx].clear();
        fieldKeyValid[idx] = false;

        std::string t = trim(fieldText[idx]);
        if (t.empty()) {
            // Field was intentionally cleared and committed (Enter/blur reached
            // recompileField with empty text): deactivate this slot instead of
            // leaving the previous rule as a "ghost" still active in the engine.
            fieldTextCommitted[idx].clear();
            fieldKeyCommittedValid[idx] = false;
            return;
        }

        RuleTable tmp;
        std::string err;
        if (parseRuleLine(t, tmp, err)) {
            auto arrow = t.find("->");
            auto leftParts = splitTopLevel(trim(t.substr(0, arrow)), ',');
            GradeValue g;
            int ticks;
            if (leftParts.size() == 2 &&
                parseGradeValue(trim(leftParts[0]), g) &&
                parseDurationTicks(trim(leftParts[1]), ticks)) {
                fieldKey[idx] = RuleKey{g, ticks};
                fieldKeyValid[idx] = true;
            }
            // Valid: promote to committed, so the engine keeps using it even if
            // a later edit to this same field temporarily breaks the syntax.
            fieldTextCommitted[idx] = fieldText[idx];
            fieldKeyCommitted[idx] = fieldKey[idx];
            fieldKeyCommittedValid[idx] = fieldKeyValid[idx];
        } else {
            fieldError[idx] = true;
            fieldErrorMsg[idx] = err;
        }
    }

    void recompileAll() {
        for (int i = 0; i < NUM_FIELDS; i++) recompileField(i);

        RuleTable table;
        std::unordered_map<RuleKey, int, RuleKeyHash> keyOrder;
        std::unordered_map<RuleKey, std::vector<int>, RuleKeyHash> keyFieldIndices;
        for (int i = 0; i < NUM_FIELDS; i++) {
            std::string t = trim(fieldTextCommitted[i]);
            if (t.empty()) continue;
            std::string err;
            if (parseRuleLine(t, table, err) && fieldKeyCommittedValid[i]) {
                keyOrder.emplace(fieldKeyCommitted[i], i);
                keyFieldIndices[fieldKeyCommitted[i]].push_back(i);
            }
        }

        RuleKey initKey = fieldKeyCommittedValid[0] ? fieldKeyCommitted[0] : RuleKey{GradeValue{false, 1}, PPQN};

        std::lock_guard<std::mutex> lock(engineMutex);
        for (int ch = 0; ch < MAX_CHANNELS; ch++) {
            engines[ch].setRules(table);
            engines[ch].setKeyOrder(keyOrder);
            engines[ch].setKeyFieldIndices(keyFieldIndices);
            engines[ch].setInitiator(initKey);
            engines[ch].setFallback(fallback);
            engines[ch].setGradeRange(gradeMin, gradeMax);
        }
    }


        // ---- Rule randomization -------------------------------------------
    // Deterministic given the same seed text. The generation logic lives in
    // RuleGenerator.hpp (namespace lgen); here we only feed it the module's
    // config and apply the results back to the module state.
    std::string seedText; // shown/edited in the context menu; numeric or any string (hashed)

    void randomizeRules() {
        lgen::GeneratorConfig cfg;
        cfg.numFields = NUM_FIELDS;
        cfg.ruleFieldMaxChars = RULE_FIELD_MAX_CHARS;
        cfg.listFieldMaxChars = LIST_FIELD_MAX_CHARS;
        cfg.gradeMin = gradeMin;
        cfg.gradeMax = gradeMax;
        cfg.ppqn = PPQN;

        lgen::GeneratedRuleSet result =
            lgen::generateAll((lgen::GenStyle)genStyle, seedText, cfg);

        setRandomGradeListText(result.gradePool);
        setRandomDurationListText(result.durationPool);

        for (int i = 0; i < NUM_FIELDS && i < (int)result.rules.size(); i++) {
            fieldText[i] = result.rules[i];
        }

        recompileAll();
        resetAllEngines();
    }

    
    // ---- Optional 'r' candidate lists -------------------------------------

    // Splits an optional trailing ':weight' off a pool entry. Returns the
    // value part (trimmed) and sets weight (default 1.0 if no ':' present).
    static bool splitPoolWeight(const std::string& partRaw, std::string& valStr, double& weight) {
        std::string part = trim(partRaw);
        size_t colon = part.find(':');
        weight = 1.0;
        if (colon == std::string::npos) {
            valStr = part;
            return true;
        }
        valStr = trim(part.substr(0, colon));
        try { weight = std::stod(part.substr(colon + 1)); }
        catch (...) { return false; }
        return weight > 0.0 && std::isfinite(weight);
    }

    static bool parseIntCsv(const std::string& raw, std::vector<WeightedPoolItem>& out) {
        out.clear();
        std::string t = trim(raw);
        if (t.empty()) return true; // empty = unrestricted, not an error
        for (auto& partRaw : splitTopLevel(t, ',')) {
            std::string part = trim(partRaw);
            if (part.empty()) continue;
            std::string valStr; double weight;
            if (!splitPoolWeight(part, valStr, weight)) return false;
            GradeValue g;
            if (!parseGradeValue(valStr, g) || g.isRest) return false;
            out.push_back({g.value, weight});
        }
        return !out.empty();
    }

    static bool parseDurationCsv(const std::string& raw, std::vector<WeightedPoolItem>& out) {
        out.clear();
        std::string t = trim(raw);
        if (t.empty()) return true; // empty = unrestricted, not an error
        for (auto& partRaw : splitTopLevel(t, ',')) {
            std::string part = trim(partRaw);
            if (part.empty()) continue;
            std::string valStr; double weight;
            if (!splitPoolWeight(part, valStr, weight)) return false;
            int ticks;
            if (!parseDurationTicks(valStr, ticks)) return false;
            out.push_back({ticks, weight});
        }
        return !out.empty();
    }

    void setRandomGradeListText(const std::string& text) {
        rGradeListText = text;
        std::vector<WeightedPoolItem> parsed;
        rGradeListError = !parseIntCsv(text, parsed);
        if (!rGradeListError) {
            std::lock_guard<std::mutex> lock(engineMutex);
            for (int ch = 0; ch < MAX_CHANNELS; ch++) engines[ch].setRandomGradeList(parsed);
        }
    }

    void setRandomDurationListText(const std::string& text) {
        rDurationListText = text;
        std::vector<WeightedPoolItem> parsed;
        rDurationListError = !parseDurationCsv(text, parsed);
        if (!rDurationListError) {
            std::lock_guard<std::mutex> lock(engineMutex);
            for (int ch = 0; ch < MAX_CHANNELS; ch++) engines[ch].setRandomDurationList(parsed);
        }
    }

    void setFieldText(int idx, const std::string& text) {
        fieldText[idx] = text;
        recompileAll();
    }

    void setFallback(FallbackMode m) {
        fallback = m;
        std::lock_guard<std::mutex> lock(engineMutex);
        for (int ch = 0; ch < MAX_CHANNELS; ch++) engines[ch].setFallback(m);
    }

    void setGradeRange(int mn, int mx) {
        gradeMin = mn; gradeMax = mx;
        std::lock_guard<std::mutex> lock(engineMutex);
        for (int ch = 0; ch < MAX_CHANNELS; ch++) engines[ch].setGradeRange(mn, mx);
    }

    void setAutoResetSteps(int steps) {
        autoResetSteps = steps;
        for (int ch = 0; ch < MAX_CHANNELS; ch++) autoResetPulseCount[ch] = 0;
    }

    void setScale(int idx) {
        auto& presets = getScalePresets();
        if (idx < 0 || idx >= (int)presets.size()) return;
        scaleIndex = idx;
        scale = presets[idx].semitones;
    }

    void setRootNoteClass(int pitchClass) {
        int octave = floorDiv(rootNote, 12);
        rootNote = octave * 12 + pitchClass;
    }

    void resetAllEngines() {
        std::lock_guard<std::mutex> lock(engineMutex);
        for (int ch = 0; ch < MAX_CHANNELS; ch++) {
            engines[ch].reset();
            ticksRemaining[ch] = 0;
            gateHigh[ch] = false;
            retrigSamplesLeft[ch] = 0;
            autoResetPulseCount[ch] = 0;
            gliding[ch] = false;
            evalTriggered[ch] = false;
        }
        activeField = -1;
        // Restart aligned to the next clock edge (see awaitingClockAfterReset),
        // and drop any stale oversized-gap candidate from before the reset.
        awaitingClockAfterReset = true;
        pendingBeatSamples = 0.0;
    }

    void onInternalTick(const ProcessArgs& args, int ch, bool atPulse) {
        // AAS (Autoreset After Steps): counts internal ticks (PPQN per beat),
        // independent of the rule engine. 1 "step" = 1 beat = PPQN internal
        // ticks. When the threshold is reached, this channel is reset exactly
        // like a Reset pulse.
        if (autoResetSteps > 0) {
            autoResetPulseCount[ch]++;
            if (autoResetPulseCount[ch] >= autoResetSteps * PPQN) {
                autoResetPulseCount[ch] = 0;
                {
                    std::lock_guard<std::mutex> lock(engineMutex);
                    engines[ch].reset();
                }
                ticksRemaining[ch] = 0;
                gateHigh[ch] = false;
                retrigSamplesLeft[ch] = 0;
                gliding[ch] = false;
                if (ch == 0) activeField = -1;
                // Same contract as a Reset pulse: hold until the next clock
                // edge so the restarted cycle lands exactly ON the beat --
                // unless the threshold happened to land on the downbeat tick
                // itself (the common case once starts are pulse-aligned):
                // there no hold is needed and forcing one would mute a whole
                // beat every cycle.
                if (!atPulse) awaitingClockAfterReset = true;
            }
        }

        if (ticksRemaining[ch] > 0) {
            if (gliding[ch]) currentPitch[ch] += glideStepV[ch];
            ticksRemaining[ch]--;
            return;
        }

        ResolvedEvent ev;
        bool got;
        {
            std::lock_guard<std::mutex> lock(engineMutex);
            if (engines[ch].isQueueEmpty() && inputs[EVAL_INPUT].isConnected()) {
                int evalChans = inputs[EVAL_INPUT].getChannels();
                float evalV = inputs[EVAL_INPUT].getVoltage(ch < evalChans ? ch : 0);
                switch (evalMode) {
                    case EVAL_RULE_SELECT: {
                        // Voltajes < -1V se tratan como "ignorar Eval": el L-System
                        // continua su evaluacion natural, util con fuentes CV bipolares.
                        if (evalV >= -1.0f) {
                            int targetRow = std::max(0, std::min(NUM_FIELDS - 1, (int)std::round(evalV * (float)(NUM_FIELDS - 1) / 10.f)));
                            if (fieldKeyCommittedValid[targetRow]) {
                                engines[ch].setCurrentKey(fieldKeyCommitted[targetRow]);
                                engines[ch].setForcedFieldIndex(targetRow);
                            }
                        }
                        break;
                    }
                    case EVAL_HOLD_GATE: {
                        if (evalV > 2.0f) {
                            engines[ch].setCurrentKey(engines[ch].getLastFiredKey());
                            engines[ch].setForcedFieldIndex(engines[ch].getLastFiredFieldIndex());
                        }
                        break;
                    }
                    case EVAL_STEP_TRIGGER: {
                        if (evalTriggered[ch]) {
                            evalTriggered[ch] = false;
                        } else {
                            engines[ch].setCurrentKey(engines[ch].getLastFiredKey());
                            engines[ch].setForcedFieldIndex(engines[ch].getLastFiredFieldIndex());
                        }
                        break;
                    }
                    default:
                        break;
                }
            }

            got = engines[ch].nextEvent(ev);
            if (got && engines[ch].firedThisStep) {
                if (ch == 0) activeField = engines[ch].lastFiredFieldIndex;
                int fi = engines[ch].lastFiredFieldIndex;
                if (fi >= 0 && NUM_FIELDS > 1) {
                    ruleVoltage[ch] = fi * (10.f / (float)(NUM_FIELDS - 1));
                }
            }
            if (engines[ch].eorFired) eorPulse[ch].trigger(1e-3f);
        }

        if (!got) {
            gateHigh[ch] = false;
            gliding[ch] = false;
            return;
        }

        ticksRemaining[ch] = std::max(1, ev.key.durationTicks) - 1;

        if (ev.silent) {
            // Duration-0 step kept only to drive rule-routing (see engine
            // expandOnce()): consumes its floored tick, but must not appear
            // as a note -- leave V/Oct and Gate untouched, and don't carry
            // any glide-in-progress through it.
            gliding[ch] = false;
        } else if (ev.key.grade.isRest) {
            gateHigh[ch] = false;
            gliding[ch] = false;
        } else {
            int note = degreeToNote(ev.key.grade.value, scale, rootNote);
            float originPitch = (note - 60) / 12.f;

            // 'Fake slide': instead of sample-accurate interpolation, step the
            // pitch by a fixed amount on every clock pulse across this note's
            // duration, assuming the standard 48-PPQN tick resolution already
            // used throughout the engine. Coarser on wide intervals or slow
            // clocks, but needs no clock-speed detection or per-sample DSP.
            if (ev.hasGlide && !ev.glideTarget.isRest) {
                int targetNote = degreeToNote(ev.glideTarget.value, scale, rootNote);
                float targetPitch = (targetNote - 60) / 12.f;
                int ticks = std::max(1, ev.key.durationTicks);
                glideStepV[ch] = (targetPitch - originPitch) / (float)ticks;
                gliding[ch] = true;
                originPitch += glideStepV[ch]; // pitch starts moving on this very pulse
            } else {
                gliding[ch] = false;
            }

            currentPitch[ch] = originPitch;
            if (gateHigh[ch]) {
                retrigSamplesLeft[ch] = (int)(args.sampleRate * 0.001f);
            }
            gateHigh[ch] = true;
        }
    }

    // Fires one internal engine tick on every active channel. atPulse marks
    // the tick generated by the incoming clock pulse itself (the downbeat).
    void fireInternalTick(const ProcessArgs& args, bool atPulse) {
        for (int ch = 0; ch < numChannels; ch++) {
            onInternalTick(args, ch, atPulse);
        }
    }

    void process(const ProcessArgs& args) override {
        if (runButtonTrigger.process(params[RUN_PARAM].getValue())) {
            running = !running;
        }
        if (runInputTrigger.process(inputs[RUN_INPUT].getVoltage())) {
            running = !running;
        }
        lights[RUN_LIGHT].setBrightness(running ? 1.f : 0.f);

        bool isRunning = running;

        if (isRunning && !wasRunning && resetOnRun) {
            resetAllEngines();
        }
        wasRunning = isRunning;

        if (inputs[EVAL_INPUT].isConnected()) {
            int evalChans = inputs[EVAL_INPUT].getChannels();
            for (int ch = 0; ch < numChannels; ch++) {
                float v = inputs[EVAL_INPUT].getVoltage(ch < evalChans ? ch : 0);
                if (evalInputTrigger[ch].process(v)) {
                    evalTriggered[ch] = true;
                }
            }
        }

        bool resetBtn = resetButtonTrigger.process(params[RESET_PARAM].getValue());
        bool resetIn = resetTrigger.process(inputs[RESET_INPUT].getVoltage());
        if (resetBtn || resetIn) {
            if (resetIn && !resetBtn && inputs[EVAL_INPUT].isConnected() && evalMode == EVAL_RULE_SELECT) {
                // If ch0 voltage is below -1V, treat Reset+Eval as a plain reset.
                int evalChans = inputs[EVAL_INPUT].getChannels();
                float evalV0 = inputs[EVAL_INPUT].getVoltage(0 < evalChans ? 0 : 0);
                if (evalV0 >= -1.0f) {
                    std::lock_guard<std::mutex> lock(engineMutex);
                    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
                        float evalV = inputs[EVAL_INPUT].getVoltage(ch < evalChans ? ch : 0);
                        // Per-channel: if a polyphonic channel is negative, reset it normally.
                        if (evalV < -1.0f) {
                            engines[ch].resetTo(fieldKeyCommittedValid[0] ? fieldKeyCommitted[0] : RuleKey{GradeValue{false, 1}, PPQN});
                            ticksRemaining[ch] = 0; gateHigh[ch] = false;
                            retrigSamplesLeft[ch] = 0; autoResetPulseCount[ch] = 0;
                            gliding[ch] = false; evalTriggered[ch] = false;
                            continue;
                        }
                        int targetRow = std::max(0, std::min(NUM_FIELDS - 1, (int)std::round(evalV * (float)(NUM_FIELDS - 1) / 10.f)));
                        RuleKey targetKey = fieldKeyCommittedValid[targetRow] ? fieldKeyCommitted[targetRow] : (fieldKeyCommittedValid[0] ? fieldKeyCommitted[0] : RuleKey{GradeValue{false, 1}, PPQN});
                        engines[ch].resetToField(targetRow, targetKey);
                        ticksRemaining[ch] = 0;
                        gateHigh[ch] = false;
                        retrigSamplesLeft[ch] = 0;
                        autoResetPulseCount[ch] = 0;
                        gliding[ch] = false;
                        evalTriggered[ch] = false;
                        if (ch == 0) activeField = targetRow;
                    }
                } else {
                    resetAllEngines();
                }
            } else {
                resetAllEngines();
            }
        }

        // ---- Clock front-end: 1 PPQN in -> PPQN internal ticks out -------
        sampleCounter++;

        if (isRunning && inputs[CLOCK_INPUT].isConnected()) {
            if (clockTrigger.process(inputs[CLOCK_INPUT].getVoltage())) {
                // Incoming beat pulse: re-anchor the internal beat boundary
                // here and fire its downbeat tick immediately, so the sequence
                // still starts on the very first clock pulse (as it always
                // did). Sub-beat interpolation becomes available from the
                // second pulse on, once a full beat has been measured.
                double gap = (lastPulseSamplePos >= 0)
                    ? double(sampleCounter - lastPulseSamplePos) : 0.0;
                double prevBeat = samplesPerTick * (double)PPQN;

                if (!haveClockTempo) {
                    if (gap >= (double)CLOCK_MIN_BEAT_SAMPLES) {
                        samplesPerTick = gap / (double)PPQN;
                        haveClockTempo = true;
                        pendingBeatSamples = 0.0;
                    }
                } else if (gap >= (double)CLOCK_MIN_BEAT_SAMPLES &&
                           gap <= prevBeat * CLOCK_GAP_ADOPT_RATIO) {
                    // Normal case (stable clock, moderate tempo moves):
                    // adopt the fresh measurement directly.
                    samplesPerTick = gap / (double)PPQN;
                    pendingBeatSamples = 0.0;
                } else if (gap > prevBeat * CLOCK_GAP_ADOPT_RATIO) {
                    // Oversized gap: either a transport pause/resume or an
                    // abrupt big slowdown. A single such gap is not trusted
                    // (adopting it would drag one whole beat after every
                    // pause); it is adopted only when the next gap confirms
                    // a similar spacing.
                    if (pendingBeatSamples > 0.0 &&
                        std::abs(gap - pendingBeatSamples) <= pendingBeatSamples * CLOCK_GAP_CONFIRM_TOL) {
                        samplesPerTick = gap / (double)PPQN;
                        pendingBeatSamples = 0.0;
                    } else {
                        pendingBeatSamples = gap;
                    }
                }
                // (gaps below CLOCK_MIN_BEAT_SAMPLES are glitches: keep tempo)

                lastPulseSamplePos = sampleCounter;
                clockFrozen = false;
                tickPhase = 0.0;
                awaitingClockAfterReset = false;
                fireInternalTick(args, true);
            } else if (haveClockTempo && !awaitingClockAfterReset) {
                // Between pulses: interpolate the remaining internal ticks of
                // the current beat at the measured rate. Once the next pulse
                // is overdue, freeze instead of free-running -- matching how
                // a stopped clock used to suspend the sequence mid-note.
                // While awaitingClockAfterReset is armed (just after a Reset)
                // this whole branch is skipped: the sequence stays silent
                // until the next real pulse re-anchors it exactly on the beat.
                double dueAt = double(lastPulseSamplePos) + samplesPerTick * (double)PPQN;
                if (double(sampleCounter) <= dueAt) {
                    tickPhase += 1.0 / samplesPerTick;
                    while (tickPhase >= 1.0) {
                        tickPhase -= 1.0;
                        fireInternalTick(args, false);
                    }
                    clockFrozen = false;
                } else {
                    clockFrozen = true;
                }
            }
        } else {
            // Clock unplugged or module stopped: never stay armed forever,
            // otherwise a later resume without resetOnRun would hold silently.
            awaitingClockAfterReset = false;
        }

        outputs[PITCH_OUTPUT].setChannels(numChannels);
        outputs[GATE_OUTPUT].setChannels(numChannels);
        outputs[EOR_OUTPUT].setChannels(numChannels);
        outputs[RULE_OUTPUT].setChannels(numChannels);

        for (int ch = 0; ch < numChannels; ch++) {
            float gateV = (isRunning && gateHigh[ch]) ? 10.f : 0.f;
            if (retrigSamplesLeft[ch] > 0) {
                retrigSamplesLeft[ch]--;
                gateV = 0.f;
            }

            outputs[PITCH_OUTPUT].setVoltage(currentPitch[ch], ch);
            outputs[GATE_OUTPUT].setVoltage(gateV, ch);
            outputs[EOR_OUTPUT].setVoltage(eorPulse[ch].process(args.sampleTime) ? 10.f : 0.f, ch);
            outputs[RULE_OUTPUT].setVoltage(ruleVoltage[ch], ch);
        }
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_t* fieldsJ = json_array();
        for (int i = 0; i < NUM_FIELDS; i++)
            json_array_append_new(fieldsJ, json_string(fieldText[i].c_str()));
        json_object_set_new(rootJ, "fields", fieldsJ);
        json_object_set_new(rootJ, "fallback", json_integer((int)fallback));
        json_object_set_new(rootJ, "gradeMin", json_integer(gradeMin));
        json_object_set_new(rootJ, "gradeMax", json_integer(gradeMax));
        json_object_set_new(rootJ, "scaleIndex", json_integer(scaleIndex));
        json_object_set_new(rootJ, "rootNote", json_integer(rootNote));
        json_object_set_new(rootJ, "numChannels", json_integer(numChannels));
        json_object_set_new(rootJ, "resetOnRun", json_boolean(resetOnRun));
        json_object_set_new(rootJ, "autoResetSteps", json_integer(autoResetSteps));
        json_object_set_new(rootJ, "running", json_boolean(running));
        json_object_set_new(rootJ, "rGradeList", json_string(rGradeListText.c_str()));
        json_object_set_new(rootJ, "rDurationList", json_string(rDurationListText.c_str()));
        json_object_set_new(rootJ, "seedText", json_string(seedText.c_str()));
        json_object_set_new(rootJ, "genStyle", json_integer(genStyle));
        json_object_set_new(rootJ, "evalMode", json_integer((int)evalMode));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* fieldsJ = json_object_get(rootJ, "fields");
        if (fieldsJ) {
            for (int i = 0; i < NUM_FIELDS && i < (int)json_array_size(fieldsJ); i++) {
                json_t* s = json_array_get(fieldsJ, i);
                if (s) fieldText[i] = json_string_value(s);
            }
        }
        json_t* fbJ = json_object_get(rootJ, "fallback");
        if (fbJ) fallback = (FallbackMode)json_integer_value(fbJ);
        json_t* gminJ = json_object_get(rootJ, "gradeMin");
        if (gminJ) gradeMin = (int)json_integer_value(gminJ);
        json_t* gmaxJ = json_object_get(rootJ, "gradeMax");
        if (gmaxJ) gradeMax = (int)json_integer_value(gmaxJ);
        json_t* siJ = json_object_get(rootJ, "scaleIndex");
        if (siJ) setScale((int)json_integer_value(siJ));
        json_t* rnJ = json_object_get(rootJ, "rootNote");
        if (rnJ) rootNote = (int)json_integer_value(rnJ);
        json_t* chJ = json_object_get(rootJ, "numChannels");
        if (chJ) numChannels = (int)json_integer_value(chJ);
        json_t* rorJ = json_object_get(rootJ, "resetOnRun");
        if (rorJ) resetOnRun = json_is_true(rorJ);
        json_t* eosJ = json_object_get(rootJ, "autoResetSteps");
        if (!eosJ) eosJ = json_object_get(rootJ, "eosMaxSteps"); // old patches
        if (eosJ) autoResetSteps = (int)json_integer_value(eosJ);
        json_t* runJ = json_object_get(rootJ, "running");
        if (runJ) running = json_is_true(runJ);
        json_t* rgJ = json_object_get(rootJ, "rGradeList");
        if (rgJ) setRandomGradeListText(json_string_value(rgJ));
        json_t* rdJ = json_object_get(rootJ, "rDurationList");
        if (rdJ) setRandomDurationListText(json_string_value(rdJ));
        json_t* seedJ = json_object_get(rootJ, "seedText");
        if (seedJ) seedText = json_string_value(seedJ);
        json_t* gsJ = json_object_get(rootJ, "genStyle");
        if (gsJ) genStyle = std::max(0, std::min((int)NUM_GEN_STYLES - 1, (int)json_integer_value(gsJ)));
        json_t* emJ = json_object_get(rootJ, "evalMode");
        if (emJ) evalMode = (EvalMode)std::max(0, std::min((int)NUM_EVAL_MODES - 1, (int)json_integer_value(emJ)));
        recompileAll();
        resetAllEngines();
    }
    void onReset(const ResetEvent& e) override {
        Module::onReset(e);
        for (int i = 0; i < NUM_FIELDS; i++) {
            fieldText[i].clear();
            fieldTextCommitted[i].clear();
            fieldKeyCommittedValid[i] = false;
            fieldError[i] = false;
            fieldErrorMsg[i].clear();
        }
        for (int ch = 0; ch < MAX_CHANNELS; ch++) {
            evalTriggered[ch] = false;
        }
        // Clock front-end back to pristine: the tempo must be relearned from
        // the next two incoming pulses.
        haveClockTempo = false;
        clockFrozen = false;
        pendingBeatSamples = 0.0;
        tickPhase = 0.0;
        lastPulseSamplePos = -1;
        samplesPerTick = 0;
        sampleCounter = 0;
        rGradeListText.clear();
        rDurationListText.clear();
        rGradeListError = false;
        rDurationListError = false;
        seedText.clear();
        recompileAll();
        resetAllEngines();
    }


};

// =======================================================================
// WIDGET
// =======================================================================

// Fully custom text-field rendering: draws our own background (or none, for
// a transparent field over the panel's own artwork), our own text color, and
// a deterministically vertically-centered baseline (nvgTextAlign MIDDLE),
// instead of relying on ui::TextField::draw()'s internal padding/positioning
// -- which is what was causing text to clip out the bottom on short rows.
// TUNE THESE to taste:
static const NVGcolor QUO_FIELD_BG = nvgRGB(0x2a, 0x2a, 0x2a);   // dark gray fill; set alpha 0 (see below) for transparent
static const NVGcolor QUO_FIELD_TEXT = nvgRGB(0xe8, 0xe8, 0xe8); // light/white text
static const NVGcolor QUO_FIELD_CURSOR = nvgRGB(0xff, 0xcc, 0x30);
static const NVGcolor QUO_FIELD_SELECTION = nvgRGBA(0xff, 0xcc, 0x30, 70); // translucent selection highlight
static const float QUO_FIELD_RADIUS = 2.f;   // corner radius; 0 = square corners
static const bool QUO_FIELD_DRAW_BG = true;  // set false for fully transparent (panel art shows through)
static const float QUO_FIELD_FONT_SIZE = 11.f;
static const float QUO_FIELD_TEXT_X = 4.f;   // left padding text starts at

inline std::shared_ptr<window::Font> getQuoFieldFont() {
    static std::shared_ptr<window::Font> font;
    if (!font) font = APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
    return font;
}

// Sets up vg with the exact font/size/alignment used both for drawing AND for
// hit-testing (see quoFindCursorIndex below) -- keeping the two in lockstep is
// what makes mouse clicks/drags land on the character actually under the
// cursor, instead of drifting the further right you go.
inline bool setQuoFieldFont(NVGcontext* vg) {
    auto font = getQuoFieldFont();
    if (!font || font->handle < 0) return false;
    nvgFontFaceId(vg, font->handle);
    nvgFontSize(vg, QUO_FIELD_FONT_SIZE);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
    return true;
}

// Character index whose glyph boundary is closest to mouseX, using the same
// font/size/offset as drawStyledTextField. O(n^2) in text length, but fields
// here are capped at a few dozen characters, so this is negligible and only
// runs on mouse events (not every frame).
inline int quoFindCursorIndex(NVGcontext* vg, const std::string& text, float mouseX) {
    if (!setQuoFieldFont(vg)) return (int)text.size();
    if (mouseX <= QUO_FIELD_TEXT_X) return 0;
    float bounds[4];
    float prevW = 0.f;
    for (int i = 1; i <= (int)text.size(); i++) {
        float w = nvgTextBounds(vg, 0.f, 0.f, text.substr(0, i).c_str(), NULL, bounds);
        if (QUO_FIELD_TEXT_X + w >= mouseX) {
            bool closerToPrev = (mouseX - (QUO_FIELD_TEXT_X + prevW)) < ((QUO_FIELD_TEXT_X + w) - mouseX);
            return closerToPrev ? i - 1 : i;
        }
        prevW = w;
    }
    return (int)text.size();
}

// Rendered pixel width of the whole string, same font/size as drawing.
inline float quoTextWidth(NVGcontext* vg, const std::string& text) {
    if (!setQuoFieldFont(vg)) return 0.f;
    float bounds[4];
    return nvgTextBounds(vg, 0.f, 0.f, text.c_str(), NULL, bounds);
}

// Trims characters off the end of `text` (in place) until it both fits within
// `maxWidthPx` (measured in real pixels, so narrow chars like ',' don't waste
// space and wide ones don't overflow) AND is at or under `maxChars` (a hard
// backstop in case font measurement is unavailable). Used only when the text
// just grew (new typing/paste), never to retroactively shrink pre-existing content.
inline void quoTrimToFit(std::string& text, float maxWidthPx, size_t maxChars) {
    NVGcontext* vg = APP->window ? APP->window->vg : nullptr;
    bool haveFont = vg && setQuoFieldFont(vg);
    while (!text.empty()) {
        bool overCount = text.size() > maxChars;
        bool overWidth = haveFont && (QUO_FIELD_TEXT_X + quoTextWidth(vg, text)) > maxWidthPx;
        if (!overCount && !overWidth) break;
        text.pop_back();
    }
}

inline void drawStyledTextField(const Widget::DrawArgs& args, const Vec& size,
                                 const std::string& text, int cursor, int selection, bool focused) {
    if (QUO_FIELD_DRAW_BG) {
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, 0.f, 0.f, size.x, size.y, QUO_FIELD_RADIUS);
        nvgFillColor(args.vg, QUO_FIELD_BG);
        nvgFill(args.vg);
    }

    if (!setQuoFieldFont(args.vg)) return;
    nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

    float ty = size.y * 0.5f;

    // Selection highlight (drawn before the text so glyphs stay legible on top)
    if (focused && cursor != selection) {
        int begin = std::min(cursor, selection);
        int end = std::max(cursor, selection);
        float bounds[4];
        float x0 = QUO_FIELD_TEXT_X + nvgTextBounds(args.vg, 0.f, 0.f, text.substr(0, begin).c_str(), NULL, bounds);
        float x1 = QUO_FIELD_TEXT_X + nvgTextBounds(args.vg, 0.f, 0.f, text.substr(0, end).c_str(), NULL, bounds);
        nvgBeginPath(args.vg);
        nvgRect(args.vg, x0, 2.f, x1 - x0, size.y - 4.f);
        nvgFillColor(args.vg, QUO_FIELD_SELECTION);
        nvgFill(args.vg);
    }

    nvgFillColor(args.vg, QUO_FIELD_TEXT);
    nvgText(args.vg, QUO_FIELD_TEXT_X, ty, text.c_str(), NULL);

    if (focused) {
        float bounds[4];
        std::string before = text.substr(0, std::min((size_t)cursor, text.size()));
        float cw = nvgTextBounds(args.vg, 0.f, 0.f, before.c_str(), NULL, bounds);
        float cx = QUO_FIELD_TEXT_X + cw;
        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, cx, 3.f);
        nvgLineTo(args.vg, cx, size.y - 3.f);
        nvgStrokeColor(args.vg, QUO_FIELD_CURSOR);
        nvgStrokeWidth(args.vg, 1.f);
        nvgStroke(args.vg);
    }
}

struct RuleTextField : ui::TextField {
    LSystemModule* module = nullptr;
    int fieldIndex = -1;
    // Assigned right after construction by the ModuleWidget: the SVG "chip"
    // shape (behind this row's title text) that shows this field's status.
    StateOverlaySvg* stateLight = nullptr;

    RuleTextField() { box.size.y = 20.f; }

    // Global char cap (LSystemModule::RULE_FIELD_MAX_CHARS), enforced in step()
    // (every frame) rather than only onChange, so it catches every insertion
    // path -- typed keys, IME, paste -- regardless of whether that path fires
    // a Change event.
    size_t maxChars() const {
        return (size_t)LSystemModule::RULE_FIELD_MAX_CHARS;
    }

    // -1 = not yet initialized. Tracks length across frames so we only ever
    // block NEW growth past the limit (typing/pasting further), and never
    // retroactively truncate text that was already here (e.g. loaded from a
    // patch, or simply longer than a limit that got lowered later) -- that
    // silent retroactive truncation is what was eating characters on click.
    int lastTextLen = -1;

    void step() override {
        TextField::step();

        if (lastTextLen < 0) {
            lastTextLen = (int)text.size();
        } else if ((int)text.size() > lastTextLen) {
            // Cap by actual rendered pixel width (not just character count),
            // so narrow characters like ',' don't stop short of the field's
            // right edge, and wide ones can't overflow it either.
            float maxWidthPx = box.size.x - QUO_FIELD_TEXT_X - 3.f;
            size_t before = text.size();
            quoTrimToFit(text, maxWidthPx, maxChars());
            if (text.size() != before) {
                cursor = std::min(cursor, (int)text.size());
                selection = std::min(selection, (int)text.size());
            }
        }
        lastTextLen = (int)text.size();

        bool focused = (APP->event && APP->event->selectedWidget == this);
        if (module && !focused && text != module->fieldText[fieldIndex]) {
            text = module->fieldText[fieldIndex];
            cursor = std::min(cursor, (int)text.size());
            selection = std::min(selection, (int)text.size());
            lastTextLen = (int)text.size();
        }

        if (stateLight) {
            bool hasError = module && module->fieldError[fieldIndex];
            bool isActive = module && module->activeField == fieldIndex;
            StateOverlaySvg::State s = StateOverlaySvg::State::NONE;
            if (hasError) s = StateOverlaySvg::State::ERROR;
            else if (isActive) s = StateOverlaySvg::State::ACTIVE;
            else if (focused) s = StateOverlaySvg::State::FOCUS;
            stateLight->setState(s);
        }
    }

    // Best-effort: some Rack SDK versions define a virtual
    // TextField::getTextPosition(Vec) used internally to translate a mouse
    // position into a character index for click/drag selection. Deliberately
    // NOT marked 'override' -- if the name/signature doesn't match your SDK,
    // this just becomes an unused function (compiles fine, does nothing) and
    // the mouse/cursor drift on drag-select persists. If so, check
    // include/ui/TextField.hpp in your Rack-SDK for the real hook name and
    // I'll fix it, or tell me the compiler warning about an unused method.
    int getTextPosition(Vec mousePos) override {
        return quoFindCursorIndex(APP->window->vg, text, mousePos.x);
    }

    void onAction(const event::Action& e) override {
        TextField::onAction(e);
        if (module) module->setFieldText(fieldIndex, text);
    }

    void onDeselect(const event::Deselect& e) override {
        TextField::onDeselect(e);
        if (module) module->setFieldText(fieldIndex, text);
    }

    void draw(const DrawArgs& args) override {
        bool focused = (APP->event && APP->event->selectedWidget == this);
        drawStyledTextField(args, box.size, text, cursor, selection, focused);
    }
};

// Lightweight text field for the two 'r' candidate-list inputs (grade / duration).
// Same commit-on-Enter/blur behavior as RuleTextField, but without rule-key/
// highlight logic since these aren't rule rows (no ACTIVE/green state).
struct ListTextField : ui::TextField {
    LSystemModule* module = nullptr;
    bool isGradeList = true; // true: grade list, false: duration list

    ListTextField() { box.size.y = 20.f; }

    size_t maxChars() const { return (size_t)LSystemModule::LIST_FIELD_MAX_CHARS; }

    const std::string& committedText() const {
        return isGradeList ? module->rGradeListText : module->rDurationListText;
    }
    bool hasError() const {
        return isGradeList ? module->rGradeListError : module->rDurationListError;
    }
    void commit() {
        if (!module) return;
        if (isGradeList) module->setRandomGradeListText(text);
        else module->setRandomDurationListText(text);
    }

    // Assigned right after construction by the ModuleWidget.
    StateOverlaySvg* stateLight = nullptr;

    int lastTextLen = -1; // see RuleTextField for why this exists

    void step() override {
        TextField::step();
        if (lastTextLen < 0) {
            lastTextLen = (int)text.size();
        } else if ((int)text.size() > lastTextLen) {
            float maxWidthPx = box.size.x - QUO_FIELD_TEXT_X - 3.f;
            size_t before = text.size();
            quoTrimToFit(text, maxWidthPx, maxChars());
            if (text.size() != before) {
                cursor = std::min(cursor, (int)text.size());
                selection = std::min(selection, (int)text.size());
            }
        }
        lastTextLen = (int)text.size();

        bool focused = (APP->event && APP->event->selectedWidget == this);
        if (module && !focused && text != committedText()) {
            text = committedText();
            cursor = std::min(cursor, (int)text.size());
            selection = std::min(selection, (int)text.size());
            lastTextLen = (int)text.size();
        }

        if (stateLight) {
            bool err = module && hasError();
            StateOverlaySvg::State s = StateOverlaySvg::State::NONE;
            if (err) s = StateOverlaySvg::State::ERROR;
            else if (focused) s = StateOverlaySvg::State::FOCUS;
            stateLight->setState(s);
        }
    }

    
    int getTextPosition(Vec mousePos) override {
        return quoFindCursorIndex(APP->window->vg, text, mousePos.x);
    }

    void onAction(const event::Action& e) override { TextField::onAction(e); commit(); }
    void onDeselect(const event::Deselect& e) override { TextField::onDeselect(e); commit(); }

    void draw(const DrawArgs& args) override {
        bool focused = (APP->event && APP->event->selectedWidget == this);
        drawStyledTextField(args, box.size, text, cursor, selection, focused);
    }
};

// Seed entry field for "Randomize Rules", embedded directly in the context
// menu (no panel space needed). Accepts any text: a plain number is used
// directly as the RNG seed; anything else is hashed into one, so a
// memorable word works too. Left empty, Randomize Rules generates a fresh
// random seed and writes it back here, so the result can be reproduced later.
struct SeedTextField : ui::TextField {
    LSystemModule* module = nullptr;
    int lastTextLen = -1;

    SeedTextField() { box.size = Vec(140.f, 20.f); }

    size_t maxChars() const { return 24; }

    void step() override {
        TextField::step();
        if (lastTextLen < 0) {
            lastTextLen = (int)text.size();
        } else if ((int)text.size() > lastTextLen) {
            float maxWidthPx = box.size.x - QUO_FIELD_TEXT_X - 3.f;
            size_t before = text.size();
            quoTrimToFit(text, maxWidthPx, maxChars());
            if (text.size() != before) {
                cursor = std::min(cursor, (int)text.size());
                selection = std::min(selection, (int)text.size());
            }
        }
        lastTextLen = (int)text.size();

        bool focused = (APP->event && APP->event->selectedWidget == this);
        if (module && !focused && text != module->seedText) {
            text = module->seedText;
            cursor = std::min(cursor, (int)text.size());
            selection = std::min(selection, (int)text.size());
            lastTextLen = (int)text.size();
        }
    }

    int getTextPosition(Vec mousePos) override {
        return quoFindCursorIndex(APP->window->vg, text, mousePos.x);
    }

    void onAction(const event::Action& e) override {
        TextField::onAction(e);
        if (module) module->seedText = text;
    }
    void onDeselect(const event::Deselect& e) override {
        TextField::onDeselect(e);
        if (module) module->seedText = text;
    }

    void draw(const DrawArgs& args) override {
        bool focused = (APP->event && APP->event->selectedWidget == this);
        drawStyledTextField(args, box.size, text, cursor, selection, focused);
    }
};


// =======================================================================
// MODULE WIDGET
// =======================================================================

struct LSystemModuleWidget : ModuleWidget {




    // All positions below come straight from the panel SVG's hidden
    // "Reference" layer (jacks/buttons) and the rulesBack/rndDegressBack/
    // rndDurationsBack rectangles (text fields), read directly out of the
    // artwork so the code never has to eyeball pixel offsets against it.
    LSystemModuleWidget(LSystemModule* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/LSystem.svg")));


        // Screws
        addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // x, y, width, height (mm) of each rule row's background rect.
        static const float ruleRows[LSystemModule::NUM_FIELDS][4] = {
            {4.572f, 22.195f, 97.293f, 5.841f},
            {4.572f, 31.540f, 97.293f, 5.842f},
            {4.572f, 40.886f, 97.293f, 5.841f},
            {4.572f, 50.232f, 97.293f, 5.840f},
            {4.572f, 59.577f, 97.293f, 5.841f},
            {4.572f, 68.923f, 97.293f, 5.840f},
            {4.572f, 78.268f, 97.293f, 5.841f},
        };
        // res/overlays/ruleN_<state>.svg for each row's color chip.
        static const char* ruleOverlayPrefix[LSystemModule::NUM_FIELDS] = {
            "rule1", "rule2", "rule3", "rule4", "rule5", "rule6", "rule7"
        };
        const float fieldInset = 0.4f; // mm of breathing room inside each row rect

        for (int i = 0; i < LSystemModule::NUM_FIELDS; i++) {
            float rx = ruleRows[i][0], ry = ruleRows[i][1], rw = ruleRows[i][2], rh = ruleRows[i][3];

            StateOverlaySvg* light = new StateOverlaySvg;
            std::string prefix = std::string("res/overlays/") + ruleOverlayPrefix[i] + "_";
            light->loadVariants(prefix + "focus.svg", prefix + "error.svg", prefix + "active.svg");
            addChild(light);

            RuleTextField* tf = new RuleTextField;
            tf->box.pos = mm2px(Vec(rx + fieldInset, ry + fieldInset));
            tf->box.size = mm2px(Vec(rw - 2.f * fieldInset, rh - 2.f * fieldInset));
            tf->module = module;
            tf->fieldIndex = i;
            tf->stateLight = light;
            if (module) tf->text = module->fieldText[i];
            tf->multiline = false;
            addChild(tf);
        }

        // Fila 8: valores restringidos opcionales para 'r' (vacío = comportamiento actual)
        static const float poolRows[2][4] = {
            {4.572f, 87.614f, 47.022f, 5.841f},  // degrees
            {52.917f, 87.614f, 48.948f, 5.841f}, // durations
        };
        static const char* poolOverlayPrefix[2] = {"poolDegree", "poolDurations"};

        for (int i = 0; i < 2; i++) {
            float rx = poolRows[i][0], ry = poolRows[i][1], rw = poolRows[i][2], rh = poolRows[i][3];

            StateOverlaySvg* light = new StateOverlaySvg;
            std::string prefix = std::string("res/overlays/") + poolOverlayPrefix[i] + "_";
            light->loadVariants(prefix + "focus.svg", prefix + "error.svg"); // no ACTIVE state here
            addChild(light);

            ListTextField* lf = new ListTextField;
            lf->box.pos = mm2px(Vec(rx + fieldInset, ry + fieldInset));
            lf->box.size = mm2px(Vec(rw - 2.f * fieldInset, rh - 2.f * fieldInset));
            lf->module = module;
            lf->isGradeList = (i == 0);
            lf->stateLight = light;
            if (module) lf->text = lf->isGradeList ? module->rGradeListText : module->rDurationListText;
            lf->multiline = false;
            addChild(lf);
        }

        //Texto de titulos separados en su svg para agregarlos a los ultimo y los overlay no lo tapen:
        SvgWidget* titleText = new SvgWidget(); 
        titleText->box.pos = mm2px(Vec(0.0f, 0.0f)); // Ajusta las coordenadas x e y según necesites
        titleText->setSvg(APP->window->loadSvg(asset::plugin(pluginInstance, "res/RulesTitleText.svg")));
        addChild(titleText);


        // Botones (momentáneos, con luz propia solo en Run)
        addParam(createParamCentered<QuoButton>(mm2px(Vec(33.582f, 101.469f)), module, LSystemModule::RUN_PARAM));
        addChild(createLightCentered<QuoButtonLight<GreenLight>>(mm2px(Vec(33.500f, 101.400f)), module, LSystemModule::RUN_LIGHT));
        addParam(createParamCentered<QuoButton>(mm2px(Vec(21.378f, 101.469f)), module, LSystemModule::RESET_PARAM));

        // Entradas / Salidas (posiciones exactas leídas de la capa Reference)
        addInput(createInputCentered<QuoJack>(mm2px(Vec(9.173f, 110.839f)), module, LSystemModule::CLOCK_INPUT));
        addInput(createInputCentered<QuoJack>(mm2px(Vec(21.387f, 110.839f)), module, LSystemModule::RESET_INPUT));
        addInput(createInputCentered<QuoJack>(mm2px(Vec(33.576f, 110.839f)), module, LSystemModule::RUN_INPUT));
        addInput(createInputCentered<QuoJack>(mm2px(Vec(45.681f, 110.839f)), module, LSystemModule::EVAL_INPUT));
        addOutput(createOutputCentered<QuoJack>(mm2px(Vec(70.006f, 110.839f)), module, LSystemModule::EOR_OUTPUT));
        addOutput(createOutputCentered<QuoJack>(mm2px(Vec(57.913f, 110.839f)), module, LSystemModule::RULE_OUTPUT));
        addOutput(createOutputCentered<QuoJack>(mm2px(Vec(85.283f, 110.815f)), module, LSystemModule::GATE_OUTPUT));
        addOutput(createOutputCentered<QuoJack>(mm2px(Vec(97.512f, 110.815f)), module, LSystemModule::PITCH_OUTPUT));
        // Port/button labels ("Clk", "Rst", "Run", "V/Oct", "Gate", "EOR", "EOS")
        // are baked into the panel art (insOutsText group) -- nothing to add here.
    }

    void appendContextMenu(Menu* menu) override {
        LSystemModule* m = dynamic_cast<LSystemModule*>(this->module);
        if (!m) return;

        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("L-system of notes"));

        menu->addChild(createSubmenuItem("Polyphony channels", "", [=](Menu* menu) {
            for (int ch = 1; ch <= LSystemModule::MAX_CHANNELS; ch++) {
                std::string label = string::f("%d Channel%s", ch, ch > 1 ? "s" : "");
                menu->addChild(createCheckMenuItem(label, "",
                    [=]() { return m->numChannels == ch; },
                    [=]() { m->numChannels = ch; }));
            }
        }));

        menu->addChild(createBoolPtrMenuItem("Reset when starting playback", "", &m->resetOnRun));

        menu->addChild(createSubmenuItem("If a note has no rule...", "", [=](Menu* menu) {
            struct Opt { const char* name; FallbackMode mode; };
            static const Opt opts[] = {
                {"Loop back to first rule", FallbackMode::LOOP_TO_INITIATOR},
                {"Jump to a random rule", FallbackMode::RANDOM_KEY},
            };
            for (auto& o : opts) {
                menu->addChild(createCheckMenuItem(o.name, "",
                    [=]() { return m->fallback == o.mode; },
                    [=]() { m->setFallback(o.mode); }));
            }
        }));

        menu->addChild(createSubmenuItem("Eval input mode", "", [=](Menu* menu) {
            static const struct { const char* name; LSystemModule::EvalMode mode; } modes[] = {
                {"Rule select (0-10V CV)", LSystemModule::EVAL_RULE_SELECT},
                {"Loop / Hold rule (Gate)", LSystemModule::EVAL_HOLD_GATE},
                {"Advance on trigger (Trigger)", LSystemModule::EVAL_STEP_TRIGGER},
            };
            for (auto& mde : modes) {
                menu->addChild(createCheckMenuItem(mde.name, "",
                    [=]() { return m->evalMode == mde.mode; },
                    [=]() { m->evalMode = mde.mode; }));
            }
        }));

        menu->addChild(createSubmenuItem("Autoreset after steps (each step = 1 beat)", "", [=](Menu* menu) {
            static const struct { const char* name; int steps; } opts[] = {
                {"Off", 0},
                {"8 beats", 8},
                {"16 beats", 16},
                {"32 beats", 32},
                {"64 beats", 64}
            };
            for (auto& o : opts) {
                menu->addChild(createCheckMenuItem(o.name, "",
                    [=]() { return m->autoResetSteps == o.steps; },
                    [=]() { m->setAutoResetSteps(o.steps); }));
            }
        }));

        menu->addChild(createSubmenuItem("Scale", "", [=](Menu* menu) {
            auto& presets = getScalePresets();
            for (int i = 0; i < (int)presets.size(); i++) {
                menu->addChild(createCheckMenuItem(presets[i].name, "",
                    [=]() { return m->scaleIndex == i; },
                    [=]() { m->setScale(i); }));
            }
        }));

        menu->addChild(createSubmenuItem("Root note", "", [=](Menu* menu) {
            auto& names = getNoteNames();
            for (int i = 0; i < 12; i++) {
                menu->addChild(createCheckMenuItem(names[i], "",
                    [=]() { return floorMod(m->rootNote, 12) == i; },
                    [=]() { m->setRootNoteClass(i); }));
            }
        }));

        menu->addChild(createSubmenuItem("Random degree range (r)", "", [=](Menu* menu) {
            static const int presets[][2] = {{-8, 16}, {-4, 11}, {-16, 32}, {-1, 7}};
            for (auto& p : presets) {
                std::string label = string::f("%d to %d", p[0], p[1]);
                menu->addChild(createCheckMenuItem(label, "",
                    [=]() { return m->gradeMin == p[0] && m->gradeMax == p[1]; },
                    [=]() { m->setGradeRange(p[0], p[1]); }));
            }
        }));

        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Randomize rules"));

        menu->addChild(createSubmenuItem("Randomize rules style", "", [=](Menu* menu) {
            static const struct { const char* name; LSystemModule::GenStyle style; } styles[] = {
                {"Melodic (Song Form)", LSystemModule::STYLE_MELODIC},
                {"Acid / Techno / Polyrhythmic", LSystemModule::STYLE_ACID_TECHNO},
                {"Ambient / Evolving", LSystemModule::STYLE_AMBIENT},
                {"Complex L-System", LSystemModule::STYLE_COMPLEX_CHAOS},
            };
            for (auto& s : styles) {
                menu->addChild(createCheckMenuItem(s.name, "",
                    [=]() { return m->genStyle == (int)s.style; },
                    [=]() { m->genStyle = (int)s.style; }));
            }
        }));

        SeedTextField* seedField = new SeedTextField;
        seedField->module = m;
        seedField->text = m->seedText;
        menu->addChild(seedField);

        menu->addChild(createMenuItem("Randomize Rules", "", [=]() {
            m->randomizeRules();
        }));
    }
};

Model* modelLSystem = createModel<LSystemModule, LSystemModuleWidget>("LSystem");
