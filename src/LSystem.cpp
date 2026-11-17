// LSystem.cpp
#include "plugin.hpp"
#include "LSystemEngine.hpp"
#include "MusicUtils.hpp"
#include "components.hpp"
#include "RuleGenerator.hpp"
#include "LSystemExpander.hpp"
#include <mutex>
#include <atomic>
#include <cmath>
#include <cstring>
#include <random>
#include <functional>
#include <algorithm>


using namespace lsys;

// Definicion (C++11) de miembros static constexpr odr-used por el motor.
constexpr int LSystemEngine::BUILTIN_POOL_NUM[4];
constexpr int LSystemEngine::BUILTIN_POOL_DEN[4];

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

    // Configuration
    FallbackMode fallback = FallbackMode::LOOP_TO_INITIATOR;
    int gradeMin = -8, gradeMax = 16;
    int scaleIndex = 0;
    int rootNote = 60;
    int numChannels = 1;
    bool resetOnRun = true;
    // Gate width as a fraction of the current step duration (0.05..1.0, default 0.5).
    float gateWidth = 0.5f;
    // 0 = disabled: EOS fires only when the sequence "completes" (Rule 1, the
    // initiator, fires again). >0: measured in PULSES -- every (autoResetSteps)
    // incoming clock pulses this channel autoresets internally
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

    // Polyphonic engine state
    LSystemEngine engines[MAX_CHANNELS];
    std::mutex engineMutex;

    dsp::SchmittTrigger clockTrigger;
    dsp::SchmittTrigger resetTrigger;
    dsp::SchmittTrigger resetButtonTrigger;
    dsp::SchmittTrigger runTrigger;

    // ---- Clock front-end -------------------------------------------------
    // The external clock delivers ONE pulse per quarter note. The engine runs in
    // SUBPULSES: each incoming pulse is divided into 'pulseSubdivision' equal parts
    // (dynamic LCM of the denominators present in the rules and
    // pools). A "1" step lasts exactly one pulse, "1/2" half a pulse, and "2"
    // two pulses. Downbeats are anchored to the actual pulse edge, and the
    // fractional remainder is carried over between pulses (never discarded), so
    // the average rate always matches the clock: no cumulative drift.
    static constexpr int64_t CLOCK_MIN_PULSE_SAMPLES = 64; // rejects glitches
    std::atomic<int> pulseSubdivision{GATE_MIN_SUBDIVISION}; // subpulses per pulse; atomic for audio-thread reads
    std::atomic<bool> pendingRecompile{false}; // deferred recompile: set by UI, applied at next downbeat
    std::atomic<bool> pendingReset{false};     // deferred engine reset (used by randomizeRules)

    // Precomputed recompile data: heavy parsing done on UI thread, applied
    // lightweight on the audio thread at the next downbeat.
    struct PrecomputedRecompile {
        RuleTable table;
        std::unordered_map<RuleKey, int, RuleKeyHash> keyOrder;
        std::unordered_map<RuleKey, std::vector<int>, RuleKeyHash> keyFieldIndices;
        RuleKey initKey{GradeValue{false, 1}, GATE_MIN_SUBDIVISION};
        std::vector<WeightedPoolItem> durPool;
        int newSub = GATE_MIN_SUBDIVISION;
        int oldSub = GATE_MIN_SUBDIVISION;
        bool valid = false;
    };
    PrecomputedRecompile precomputed;
    double samplesPerPulse = 0.0;    // measured interval between edges, in samples
    double lastAppliedSpp = 0.0;     // last tempo that was re-anchored (Part 3 / tempo change)
    int64_t lastEdgeSamplePos = -1;  // sampleCounter of the last edge
    double fracPos = 0.0;            // position [0..1+) within the current pulse
    int nextBoundary = 1;            // next inner limit of the pulse (1..D-1)
    bool haveClockTempo = false;     // false until the first complete interval is measured
    bool clockFrozen = false;        // delayed edge: hold until it arrives
    bool awaitingClockAfterReset = false;
    int aasPulseCounter = 0;         // Pulses since the last autoress (AAS)
    int64_t sampleCounter = 0;       // Incremented once per process() frame
    int ticksRemaining[MAX_CHANNELS] = {};
    bool alignHold[MAX_CHANNELS] = {}; // Holds starts until downbeat after subdivision change
    bool gateHigh[MAX_CHANNELS] = {};
    float currentPitch[MAX_CHANNELS] = {};
    int retrigSamplesLeft[MAX_CHANNELS] = {};
    bool gliding[MAX_CHANNELS] = {};
    float glideStepV[MAX_CHANNELS] = {}; // V/oct added to currentPitch on every clock pulse while gliding
    // 0-10V, linearly mapped across the 7 rule rows (Rule 1 = 0V ... Rule 7 = 10V),
    // held per-channel between rule changes. Replaces the old EOS trigger output.
    float ruleVoltage[MAX_CHANNELS] = {};

    dsp::PulseGenerator eorPulse[MAX_CHANNELS];

    // ---- Expander (LS-Exp) ------------------------------------------------
    // Persistent message buffers (never allocate per frame).
    lsxp::ExpanderToLSystem fromExp;
    lsxp::LSystemToExpander toExp;
    // Decoded external scale, rebuilt when the expander payload changes.
    // extIntervals: ascending offsets in volts from the external root, spanning
    // one cycle; extLen = number of tones per cycle; extRootVoct = root pitch.
    std::vector<float> extIntervals;
    float extRootVoct = 0.f;
    int extLen = 0;
    bool useExternalScale = false;
    // Root-only transposition: a root input with no corresponding scale input
    // transposes the internal scale by (externalRoot - internalRoot) in volts.
    float extRootOffset = 0.f;
    // Absolute root override: when ROOT_IN is connected, the incoming root
    // becomes the scale's tonic, replacing the module's internal root note.
    bool absRootFromInput = false;
    float absRootVoct = 0.f;
    int extScaleMode = lsxp::MODE_STD_7CH;
    // Event-driven expander I/O: the heavy decode/publish only runs when a
    // rule step fires or the module resets, never at audio sample rate.
    bool expanderDirty = true;
    // Per-channel live state published to the expander.
    int xpAbsDegree[MAX_CHANNELS] = {1, 1, 1, 1, 1, 1};
    bool xpIsRest[MAX_CHANNELS] = {};
    bool xpIsSilent[MAX_CHANNELS] = {};
    int xpRuleIdx[MAX_CHANNELS] = {};
    int xpStepRep[MAX_CHANNELS] = {};
    int xpStepRepTotal[MAX_CHANNELS] = {1, 1, 1, 1, 1, 1};
    int xpStepWhole[MAX_CHANNELS] = {};
    int xpStepWholeTotal[MAX_CHANNELS] = {1, 1, 1, 1, 1, 1};
    // Gate-width tracking: total ticks of the current step and elapsed ticks.
    int stepTicksTotal[MAX_CHANNELS] = {1, 1, 1, 1, 1, 1};
    int stepTickPos[MAX_CHANNELS] = {};
    // Internal root as V/oct (C4 = 0V) for the expander payload.
    float internalRootVoct() const { return (float)(rootNote - 60) / 12.f; }

    bool wasRunning = false;

    LSystemModule() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(RUN_PARAM, 0.f, 1.f, 0.f, "Run");
        configParam(RESET_PARAM, 0.f, 1.f, 0.f, "Reset");
        configInput(CLOCK_INPUT, "Clock (1 pulse per quarter note)");
        configInput(RESET_INPUT, "Reset");
        configInput(RUN_INPUT, "Run Toggle (trigger)");
        configInput(EVAL_INPUT, "Eval");
        configOutput(PITCH_OUTPUT, "V/Oct");
        configOutput(GATE_OUTPUT, "Gate");
        configOutput(EOR_OUTPUT, "End of Rule Trigger");
        configOutput(RULE_OUTPUT, "Rule Number (0-10V)");

// Initial rules: a long, mostly stable sequence with occasional wild
// excursions. Rules 1..6 form a deterministic, anchored progression
// (I -> III -> V -> II -> IV -> VI in the default Major scale) with short
// motifs and heavy *N repetition, so the engine stays coherent and
// predictable most of the time. Rule 7 is intentionally the unstable one:
// short 1/8 notes, heavy use of r / lists containing r, and a small chance
// to loop onto itself -- producing unpredictable bursts. Rule 6 branches
// into rule 7 only ~20% of the time (weighted exit), so stability dominates
// but the wild region is reached regularly.
        fieldText[0] = "1,1 -> 1,1^3,1 5,1 3,1 1,1 3,1 *4";
        fieldText[1] = "3,1 -> 3,1 5,1 <2,4,3>,1 5,1 5,1 *4";
        fieldText[2] = "5,1 -> 5,1 7,1 4,1 5,1 <2:4,6>,1 *4";
        fieldText[3] = "2,1 -> 2,1 4,1 1,1 6,1 <4,6>,1 *4";
        fieldText[4] = "4,1 -> 4,1 6,1 2,1 3,1 6,1 *4";
        fieldText[5] = "6,1 -> 6,1 1,1 3,1 5,1 <1:3,7:1>,1";
        fieldText[6] = "7,1 -> r,1/2 <r,8>,1/4 k+2,r <1:3,8,r,7>,=2 *8";

        for (int i = 0; i < NUM_FIELDS; i++) fieldTextCommitted[i] = fieldText[i];

// Default 'r' pools: keep the random degrees diatonic (so even the wild
// rule 7 stays musical) and offer a spread of durations down to 1/8 for
// the unstable bursts.
        setRandomGradeListText("1,3,5,8,2,7");
        setRandomDurationListText("1,1/2,1/4");

        recompileAll();
        resetAllEngines();

        // Expander message buffers (push pattern): the LS-Exp writes
        // ExpanderToLSystem into OUR producer buffer; we read consumer.
        leftExpander.producerMessage = new lsxp::ExpanderToLSystem;
        leftExpander.consumerMessage = new lsxp::ExpanderToLSystem;
        rightExpander.producerMessage = new lsxp::ExpanderToLSystem;
        rightExpander.consumerMessage = new lsxp::ExpanderToLSystem;
    }

    ~LSystemModule() {
        delete (lsxp::ExpanderToLSystem*)leftExpander.producerMessage;
        delete (lsxp::ExpanderToLSystem*)leftExpander.consumerMessage;
        delete (lsxp::ExpanderToLSystem*)rightExpander.producerMessage;
        delete (lsxp::ExpanderToLSystem*)rightExpander.consumerMessage;
        leftExpander.producerMessage = leftExpander.consumerMessage = nullptr;
        rightExpander.producerMessage = rightExpander.consumerMessage = nullptr;
    }

    void recompileField(int idx, int subdiv) {
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
        if (parseRuleLine(t, tmp, err, subdiv)) {
            auto arrow = t.find("->");
            auto leftParts = splitTopLevel(trim(t.substr(0, arrow)), ',');
            GradeValue g;
            long long num = 0, den = 1;
            if (leftParts.size() == 2 &&
                parseGradeValue(trim(leftParts[0]), g) &&
                parseDurationPulses(trim(leftParts[1]), num, den)) {
                fieldKey[idx] = RuleKey{g, toSubpulses(num, den, subdiv)};
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

    // Heavy parsing — runs on the UI thread so the audio thread is never
    // blocked by regex / LCM / table building.  Stores results in the
    // `precomputed` struct; the audio thread picks them up via the
    // pendingRecompile flag and calls the lightweight applyRecompile().
    void precomputeRecompile() {
        int oldSub = pulseSubdivision.load(std::memory_order_relaxed);
        for (int i = 0; i < NUM_FIELDS; i++) recompileField(i, oldSub);

        // Collect denominators (rules + pool 'r') for the LCM.
        std::vector<long long> dens;
        RuleTable tmpTable;
        std::string tmpErr;
        for (int i = 0; i < NUM_FIELDS; i++) {
            std::string t = trim(fieldTextCommitted[i]);
            if (t.empty()) continue;
            parseRuleLine(t, tmpTable, tmpErr, 1, &dens);
        }
        for (auto& e : rDurationPoolRational) dens.push_back(e.den);
        long long D = 1;
        for (long long d : dens) {
            if (d <= 1) continue;
            long long g = gcdLL(D, d);
            long long next = D / g * d;
            if (next > MAX_SUBDIVISION) continue;
            D = next;
        }

        // Glide resolution requirement.
        bool anyGlide = false;
        long long glideReq = 0;
        for (auto& kv : tmpTable) {
            for (auto& prod : kv.second) {
                size_t n = prod.symbols.size();
                for (size_t i = 0; i + 1 < n; i++) {
                    const Symbol& s = prod.symbols[i];
                    if (!s.glideToNext) continue;
                    anyGlide = true;
                    if (s.duration.kind != SpecKind::FIXED || s.duration.num <= 0) continue;
                    if (i + 1 >= prod.symbols.size()) continue;
                    const Symbol& nxt = prod.symbols[i + 1];
                    if (s.grade.kind != SpecKind::FIXED || nxt.grade.kind != SpecKind::FIXED) continue;
                    const GradeValue& a = s.grade.fixedValue;
                    const GradeValue& b = nxt.grade.fixedValue;
                    if (a.isRest || b.isRest) continue;
                    double semi = std::abs(degreeToNote(b.value, scale, rootNote) -
                                           degreeToNote(a.value, scale, rootNote));
                    if (semi <= 0.0) continue;
                    long long req = (long long)std::ceil(semi * GLIDE_STEPS_PER_SEMITONE *
                                                         (double)s.duration.den / (double)s.duration.num);
                    if (req > glideReq) glideReq = req;
                }
            }
        }
        if (anyGlide) {
            D = std::max(D, std::min(glideReq, (long long)MAX_SUBDIVISION));
            D = std::max(D, (long long)GLIDE_MIN_SUBDIVISION);
        }
        D = std::max(D, (long long)GATE_MIN_SUBDIVISION);
        int newSub = (int)D;

        for (int i = 0; i < NUM_FIELDS; i++) recompileField(i, newSub);

        RuleTable table;
        std::unordered_map<RuleKey, int, RuleKeyHash> keyOrder;
        std::unordered_map<RuleKey, std::vector<int>, RuleKeyHash> keyFieldIndices;
        for (int i = 0; i < NUM_FIELDS; i++) {
            std::string t = trim(fieldTextCommitted[i]);
            if (t.empty()) continue;
            std::string err;
            if (parseRuleLine(t, table, err, newSub) && fieldKeyCommittedValid[i]) {
                keyOrder.emplace(fieldKeyCommitted[i], i);
                keyFieldIndices[fieldKeyCommitted[i]].push_back(i);
            }
        }
        RuleKey initKey = fieldKeyCommittedValid[0] ? fieldKeyCommitted[0] : RuleKey{GradeValue{false, 1}, newSub};

        std::vector<WeightedPoolItem> durPool;
        for (auto& e : rDurationPoolRational)
            durPool.push_back(WeightedPoolItem(toSubpulses(e.num, e.den, newSub), e.weight));

        // Store into precomputed (written by UI thread, read by audio thread
        // only after the pendingRecompile acquire barrier).
        precomputed.table = std::move(table);
        precomputed.keyOrder = std::move(keyOrder);
        precomputed.keyFieldIndices = std::move(keyFieldIndices);
        precomputed.initKey = initKey;
        precomputed.durPool = std::move(durPool);
        precomputed.newSub = newSub;
        precomputed.oldSub = oldSub;
        precomputed.valid = true;
    }

    // Lightweight apply — runs on the audio thread at the downbeat under the
    // engineMutex.  All heavy work was already done by precomputeRecompile().
    void applyRecompile() {
        if (!precomputed.valid) return;
        int oldSub = precomputed.oldSub;
        int newSub = precomputed.newSub;
        bool subChanged = (oldSub != newSub);

        std::lock_guard<std::mutex> lock(engineMutex);
        for (int ch = 0; ch < MAX_CHANNELS; ch++) {
            engines[ch].setSubdivision(newSub);
            engines[ch].migrateSubdivision(oldSub, newSub);
            engines[ch].setRules(precomputed.table);
            engines[ch].setKeyOrder(precomputed.keyOrder);
            engines[ch].setKeyFieldIndices(precomputed.keyFieldIndices);
            engines[ch].setInitiator(precomputed.initKey);
            engines[ch].setFallback(fallback);
            engines[ch].setGradeRange(gradeMin, gradeMax);
            engines[ch].setRandomDurationList(precomputed.durPool);
        }
        if (subChanged) {
            double r = (double)newSub / (double)oldSub;
            for (int ch = 0; ch < MAX_CHANNELS; ch++) {
                if (ticksRemaining[ch] > 0)
                    ticksRemaining[ch] = std::max(1,
                        (int)llround((double)ticksRemaining[ch] * r));
                glideStepV[ch] *= (float)(1.0 / r);
                alignHold[ch] = true;
            }
            fracPos = 0.0;
            nextBoundary = 1;
        }
        pulseSubdivision.store(newSub, std::memory_order_release);
        precomputed.valid = false;
    }

    // Combined path for lifecycle events (constructor, dataFromJson, onReset)
    // where both phases run on the UI thread before / during audio activity.
    void recompileAll() {
        precomputeRecompile();
        applyRecompile();
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

        lgen::GeneratedRuleSet result =
            lgen::generateAll((lgen::GenStyle)genStyle, seedText, cfg);

        setRandomGradeListText(result.gradePool);
        setRandomDurationListText(result.durationPool);

        for (int i = 0; i < NUM_FIELDS && i < (int)result.rules.size(); i++) {
            fieldText[i] = result.rules[i];
        }

        // Defer recompile + reset to next downbeat.
        precomputeRecompile();
        pendingRecompile.store(true, std::memory_order_release);
        pendingReset.store(true, std::memory_order_release);
    }

    
    // ---- Optional 'r' candidate lists -------------------------------------

    // Pool 'r' of durations in rational form (pulses): num/den + weight. The
    // conversion to subpulses occurs in recompileAll(), once the
    // dynamic subdivision of the rule set is known.
    struct RationalPoolEntry { long long num; long long den; double weight; };
    std::vector<RationalPoolEntry> rDurationPoolRational;

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

    static bool parseDurationCsv(const std::string& raw, std::vector<RationalPoolEntry>& out) {
        out.clear();
        std::string t = trim(raw);
        if (t.empty()) return true; // empty = unrestricted, not an error
        for (auto& partRaw : splitTopLevel(t, ',')) {
            std::string part = trim(partRaw);
            if (part.empty()) continue;
            std::string valStr; double weight;
            if (!splitPoolWeight(part, valStr, weight)) return false;
            long long num = 0, den = 1;
            if (!parseDurationPulses(valStr, num, den)) return false;
            if (num <= 0) return false;
            out.push_back({num, den, weight});
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
        std::vector<RationalPoolEntry> parsed;
        rDurationListError = !parseDurationCsv(text, parsed);
        if (!rDurationListError) {
            rDurationPoolRational = parsed;
            precomputeRecompile();
            pendingRecompile.store(true, std::memory_order_release);
        }
    }

    void setFieldText(int idx, const std::string& text) {
        fieldText[idx] = text;
        // Precompute on the UI thread (heavy parsing), then defer only the
        // lightweight engine-state swap to the next downbeat on the audio thread.
        precomputeRecompile();
        pendingRecompile.store(true, std::memory_order_release);
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
        aasPulseCounter = 0;
    }

    void setScale(int idx) {
        auto& presets = getScalePresets();
        if (idx < 0 || idx >= (int)presets.size()) return;
        scaleIndex = idx;
        scale = presets[idx].semitones;
        precomputeRecompile();
        pendingRecompile.store(true, std::memory_order_release);
    }

    void setRootNoteClass(int pitchClass) {
        int octave = floorDiv(rootNote, 12);
        rootNote = octave * 12 + pitchClass;
    }

    // ---- Expander helpers -------------------------------------------------
    // Priority per LSystem: right expander wins; otherwise left; otherwise none.
    // The expander must be physically adjacent (Rack guarantees left/right
    // pointers only for direct neighbors).
    Module* findActiveExpander() {
        if (rightExpander.module && rightExpander.module->model &&
            rightExpander.module->model->slug == "LS-Exp")
            return rightExpander.module;
        if (leftExpander.module && leftExpander.module->model &&
            leftExpander.module->model->slug == "LS-Exp")
            return leftExpander.module;
        return nullptr;
    }

    static float quantizeRootToScale(float rootV, const std::vector<float>& intervals) {
        if (intervals.empty()) return rootV;
        // Search across the real voltage range (octave replicas), not by modulo,
        // so non-octave-repeating scales behave correctly.
        float best = intervals[0];
        float bestD = 1e9f;
        for (int oct = -10; oct <= 10; oct++) {
            for (float iv : intervals) {
                float cand = iv + (float)oct;
                float d = std::fabs(cand - rootV);
                if (d < bestD) { bestD = d; best = cand; }
            }
        }
        return best;
    }

    // Decode the raw expander payload into extIntervals/extRootVoct/extLen.
    // Called when a new payload arrives; the result takes effect on the NEXT
    // event per channel (the sounding step keeps its pitch until then).
    void decodeExternalScale(const lsxp::ExpanderToLSystem& p) {
        extScaleMode = p.scaleMode;
        extIntervals.clear();
        extLen = 0;
        useExternalScale = false;
        extRootOffset = 0.f;
        absRootFromInput = false;
        if (!p.active) return;

        // LSystem Index: SCALE_IN ch0 carries a 0-10V index into the internal
        // scale presets. Applies the internal scale switch; no external scale.
        if (p.scaleMode == lsxp::MODE_LSYSTEM_INDEX) {
            if (p.scaleConnected) {
                auto& presets = getScalePresets();
                int n = (int)presets.size();
                int idx = (int)std::lround(p.scaleVoct[0] * (float)(n - 1) / 10.f);
                if (idx >= 0 && idx < n && idx != scaleIndex) setScale(idx);
            }
            // Root input is the unique root for the selected internal scale.
            absRootFromInput = p.rootConnected;
            absRootVoct = p.rootVoct;
            return;
        }

        std::vector<float> raw;
        for (int i = 0; i < p.scaleChans && i < 16; i++) raw.push_back(p.scaleVoct[i]);

        // Root-only case: no external scale, but a root input is connected.
        // Transpose the internal scale by the requested root offset instead of
        // switching to an external scale.
        if (raw.empty()) {
            // No external scale: the root input (if any) becomes the unique root
            // of the internal scale; otherwise the module root is used.
            absRootFromInput = p.rootConnected;
            absRootVoct = p.rootVoct;
            extRootOffset = 0.f;
            useExternalScale = false;
            return;
        }

        if (p.scaleMode == lsxp::MODE_CHROMATIC_12CH || p.scaleMode == lsxp::MODE_PENTA_CHROM_12CH) {
            // Categorical 12ch: 0V=off, 8V=on, 10V=root. Channel i = semitone i.
            int rootSemi = -1;
            for (int i = 0; i < (int)raw.size() && i < 12; i++)
                if (raw[i] >= 9.5f) rootSemi = i;
            std::vector<int> semis;
            for (int i = 0; i < (int)raw.size() && i < 12; i++)
                if (raw[i] > 4.f) semis.push_back(i);
            if (semis.empty()) return;
            if (rootSemi < 0) {
                // Embedded root missing: fall back to internal root class.
                rootSemi = floorMod(rootNote, 12);
            }
            // Sort by distance from root, ascending pitch.
            std::sort(semis.begin(), semis.end(), [&](int a, int b) {
                int da = (a - rootSemi + 12) % 12, db = (b - rootSemi + 12) % 12;
                return da < db;
            });
            for (int s : semis) {
                int d = (s - rootSemi + 12) % 12;
                extIntervals.push_back((float)d / 12.f);
            }
            float embRoot = (float)(rootSemi - floorMod(rootNote, 12)) / 12.f + internalRootVoct();
            // Snap embedded root to exact semitone grid relative to C.
            embRoot = std::round(embRoot * 12.f) / 12.f;
            if (p.rootConnected) extRootVoct = quantizeRootToScale(p.rootVoct, extIntervals);
            else extRootVoct = embRoot;
            extLen = (int)extIntervals.size();
            useExternalScale = extLen > 0;
            return;
        }
        // Raw V/oct modes: sort ascending, deduplicate.
        std::sort(raw.begin(), raw.end());
        std::vector<float> uniq;
        for (float v : raw) {
            if (uniq.empty() || std::fabs(v - uniq.back()) > 0.0005f) uniq.push_back(v);
        }
        if (uniq.empty()) return;
        if (p.scaleMode == lsxp::MODE_STD_7CH || p.scaleMode == lsxp::MODE_PENTA_5CH) {
            // Lowest channel = default root; intervals relative to it.
            float base = uniq[0];
            for (float v : uniq) extIntervals.push_back(v - base);
            float defRoot = base;
            if (p.rootConnected) extRootVoct = quantizeRootToScale(p.rootVoct, extIntervals);
            else extRootVoct = defRoot;
            extLen = (int)extIntervals.size();
            useExternalScale = extLen > 0;
            return;
        }
        // Libre / microtonal: same, but explicitly ignore the internal 12-TET root.
        {
            float base = uniq[0];
            for (float v : uniq) extIntervals.push_back(v - base);
            float defRoot = base;
            if (p.rootConnected) extRootVoct = quantizeRootToScale(p.rootVoct, extIntervals);
            else extRootVoct = defRoot;
            extLen = (int)extIntervals.size();
            useExternalScale = extLen > 0;
            return;
        }
    }

    float extDegreeToPitch(int absDegree) const {
        if (extLen < 1) return internalRootVoct();
        int zb = absDegree - 1;
        int d = floorDiv(zb, extLen);
        int m = floorMod(zb, extLen);
        return extRootVoct + extIntervals[m] + (float)d * 1.f;
    }

    // Pitch of a degree within the internal scale, measured from an arbitrary
    // float root base (in V/oct). Octave advance is counted relative to that
    // base, so a root override maps correctly regardless of the internal note.
    float internalDegreeToPitch(int absDegree, float rootVoct) const {
        int n = (int)scale.size();
        if (n < 1) return rootVoct;
        int zb = absDegree - 1;
        int d = floorDiv(zb, n);
        int m = floorMod(zb, n);
        return rootVoct + (float)scale[m] / 12.f + (float)d * 1.f;
    }

    // The effective tonic of the internal scale: the incoming ROOT_IN value
    // when connected (unique root), otherwise the module root plus offset.
    float effectiveInternalRoot() const {
        if (absRootFromInput) return absRootVoct;
        return internalRootVoct() + extRootOffset;
    }

    void resetAllEngines() {
        std::lock_guard<std::mutex> lock(engineMutex);
        for (int ch = 0; ch < MAX_CHANNELS; ch++) {
            engines[ch].reset();
            ticksRemaining[ch] = 0;
            alignHold[ch] = false;
            gateHigh[ch] = false;
            retrigSamplesLeft[ch] = 0;
            gliding[ch] = false;
            evalTriggered[ch] = false;
            stepTicksTotal[ch] = 1;
            stepTickPos[ch] = 0;
            xpAbsDegree[ch] = 1;
            xpIsRest[ch] = false;
            xpIsSilent[ch] = false;
            xpRuleIdx[ch] = 0;
            xpStepRep[ch] = 0;
            xpStepRepTotal[ch] = 1;
            xpStepWhole[ch] = 0;
            xpStepWholeTotal[ch] = 1;
        }
        // Reset is an update event for the expander's published state.
        expanderDirty = true;
        activeField = -1;
        // Restart aligned to the NEXT clock edge: the sequence stays silent
        // until the incoming pulse arrives, so it lands exactly ON the beat
        // (same alignment as anything else clocked from the same source).
        awaitingClockAfterReset = true;
        aasPulseCounter = 0;
        fracPos = 0.0;
        nextBoundary = 1;
        clockFrozen = false;
        // Forget the detector's stale level: after this, only a genuine
        // low->high transition of the clock line counts as a pulse.
        clockTrigger.reset();
    }

    void onInternalTick(const ProcessArgs& args, int ch, bool atPulse) {
        if (ticksRemaining[ch] > 0) {
            if (gliding[ch]) currentPitch[ch] += glideStepV[ch];
            ticksRemaining[ch]--;
            stepTickPos[ch]++;
            // Gate width: drop the gate once the width fraction elapses,
            // pitch keeps gliding/held until the step ends.
            int gw = (int)std::round((float)stepTicksTotal[ch] * gateWidth);
            if (gw < 1) gw = 1;
            if (stepTickPos[ch] >= gw) gateHigh[ch] = false;
            return;
        }


        // Quantized subdivision transition to the beat: if D changes while
        // this note was playing, hold the START of the next event until the
        // next downbeat. Without this, an inner tick of the new grid
        // would trigger the off-beat event and push the entire sequence
        // out of phase with the clock until the next Reset.
        if (!atPulse && alignHold[ch]) return;
        alignHold[ch] = false;

        ResolvedEvent ev;
        bool got;
        do {
        {
            std::lock_guard<std::mutex> lock(engineMutex);
            if (engines[ch].isQueueEmpty() && inputs[EVAL_INPUT].isConnected()) {
                int evalChans = inputs[EVAL_INPUT].getChannels();
                float evalV = inputs[EVAL_INPUT].getVoltage(ch < evalChans ? ch : 0);
                switch (evalMode) {
                    case EVAL_RULE_SELECT: {
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
        stepTicksTotal[ch] = std::max(1, ev.key.durationTicks);
        stepTickPos[ch] = 0;

        if (!ev.silent) {
            xpAbsDegree[ch] = ev.key.grade.isRest ? 0 : ev.key.grade.value;
            xpIsRest[ch] = ev.key.grade.isRest;
            xpIsSilent[ch] = false;
            xpRuleIdx[ch] = engines[ch].lastFiredFieldIndex;
            xpStepRep[ch] = ev.stepIdxRep;
            xpStepRepTotal[ch] = ev.stepTotalRep;
            xpStepWhole[ch] = ev.stepIdxWhole;
            xpStepWholeTotal[ch] = ev.stepTotalWhole;
            expanderDirty = true;
        } else {
            xpIsSilent[ch] = true;
        }

        if (ev.silent) {
            gliding[ch] = false;
            // Zero-duration routing step: the engine already advanced
            // current to this step's grade (via nextEvent), so rule
            // routing is correct.  Consume no tick — loop immediately
            // to dequeue the actual next event in the same subpulse.
            if (ticksRemaining[ch] == 0) continue;
        } else if (ev.key.grade.isRest) {
            gateHigh[ch] = false;
            gliding[ch] = false;
        } else {
            float originPitch;
            if (useExternalScale) {
                originPitch = extDegreeToPitch(ev.key.grade.value);
            } else {
                originPitch = internalDegreeToPitch(ev.key.grade.value, effectiveInternalRoot());
            }

            // 'Fake slide': instead of sample-accurate interpolation, step the
            // pitch by a fixed amount on every internal subpulse across this
            // note's duration. Coarser on wide intervals or slow clocks, but
            // needs no clock-speed detection or per-sample DSP.
            if (ev.hasGlide && !ev.glideTarget.isRest) {
                float targetPitch;
                if (useExternalScale) {
                    targetPitch = extDegreeToPitch(ev.glideTarget.value);
                } else {
                    targetPitch = internalDegreeToPitch(ev.glideTarget.value, effectiveInternalRoot());
                }
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
        break;
        } while (true);
    }

    // Fires one internal engine tick on every active channel. atPulse marks
    // the downbeat tick generated by the incoming clock edge itself.
    void fireInternalTick(const ProcessArgs& args, bool atPulse) {
        for (int ch = 0; ch < numChannels; ch++) {
            onInternalTick(args, ch, atPulse);
        }
    }

    void process(const ProcessArgs& args) override {
        // Capture whether an expander update was requested (a rule step fired
        // or a reset in a recent frame). Both the decode (top) and the publish
        // (bottom) run only when set, keeping all heavy expander work at
        // musical-event rate instead of audio sample rate.
        bool doExpander = expanderDirty;
        expanderDirty = false;

        // ---- Expander input: read the active LS-Exp payload (right wins) ----
        // Event-driven: only runs when a rule step fired or a reset happened
        // in a recent frame (doExpander). Never decodes at audio sample rate.
        if (doExpander) {
            Module* xp = findActiveExpander();
            bool got = false;
            if (xp) {
                lsxp::ExpanderToLSystem* p = nullptr;
                if (xp == rightExpander.module)
                    p = (lsxp::ExpanderToLSystem*)rightExpander.consumerMessage;
                else if (xp == leftExpander.module)
                    p = (lsxp::ExpanderToLSystem*)leftExpander.consumerMessage;
                if (p && p->active) {
                    // Decode the latest payload; takes effect on the NEXT
                    // dequeued event per channel, never mid-step.
                    if (std::memcmp(p, &fromExp, sizeof(lsxp::ExpanderToLSystem)) != 0) {
                        fromExp = *p;
                        decodeExternalScale(fromExp);
                    }
                    got = true;
                }
            }
            if (!got) {
                if (fromExp.active || useExternalScale || extRootOffset != 0.f || absRootFromInput) {
                    fromExp.active = false;
                    useExternalScale = false;
                    extRootOffset = 0.f;
                    absRootFromInput = false;
                    extIntervals.clear();
                    extLen = 0;
                }
            }
        }
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
                            engines[ch].resetTo(fieldKeyCommittedValid[0] ? fieldKeyCommitted[0] : RuleKey{GradeValue{false, 1}, pulseSubdivision});
                            ticksRemaining[ch] = 0; gateHigh[ch] = false;
                            retrigSamplesLeft[ch] = 0;
                            gliding[ch] = false; evalTriggered[ch] = false;
                            continue;
                        }
                        int targetRow = std::max(0, std::min(NUM_FIELDS - 1, (int)std::round(evalV * (float)(NUM_FIELDS - 1) / 10.f)));
                        RuleKey targetKey = fieldKeyCommittedValid[targetRow] ? fieldKeyCommitted[targetRow] : (fieldKeyCommittedValid[0] ? fieldKeyCommitted[0] : RuleKey{GradeValue{false, 1}, pulseSubdivision});
                        engines[ch].resetToField(targetRow, targetKey);
                        ticksRemaining[ch] = 0;
                        gateHigh[ch] = false;
                        retrigSamplesLeft[ch] = 0;
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

        // ---- Clock front-end: 1 incoming pulse -> D internal subpulses ----
        sampleCounter++;

        if (isRunning && inputs[CLOCK_INPUT].isConnected()) {
            if (clockTrigger.process(inputs[CLOCK_INPUT].getVoltage())) {
                // Real flank of the clock: anchor the downbeat here. The measured interval
                // is adopted directly (without heuristics); the gaps
                // are glitches and are ignored.
                if (lastEdgeSamplePos >= 0) {
                    double gap = double(sampleCounter - lastEdgeSamplePos);
                    if (!haveClockTempo) {
                        if (gap >= (double)CLOCK_MIN_PULSE_SAMPLES) {
                            samplesPerPulse = gap;
                            haveClockTempo = true;
                        }
                    } else if (gap >= (double)CLOCK_MIN_PULSE_SAMPLES) {
                        samplesPerPulse = gap;
                    }
                }
                lastEdgeSamplePos = sampleCounter;

                // Tempo change (Part 3): if the measured pulse interval swung
                // significantly, defer any in-flight event to the downbeat so a
                // tempo change can't leave a note starting off-beat. The
                // downbeat branch just below re-anchors the grid (fracPos/
                // nextBoundary); alignHold only delays the NEXT event start.
                // A >10% interval change marks a deliberate speed change and
                // ignores cycle-to-cycle jitter.
                {
                    double spp = samplesPerPulse;
                    if (haveClockTempo && spp > 0.0 && lastAppliedSpp > 0.0) {
                        double d = spp / lastAppliedSpp;
                        if (d < 0.9 || d > 1.1) {
                            for (int ch = 0; ch < MAX_CHANNELS; ch++) alignHold[ch] = true;
                        }
                    }
                    if (spp > 0.0) lastAppliedSpp = spp;
                }

                // AAS: counts complete pulses; the threshold falls right on a
                // edge, so the restarted cycle starts aligned without
                // needing to wait for another pulse.
                if (autoResetSteps > 0 && ++aasPulseCounter >= autoResetSteps) {
                    aasPulseCounter = 0;
                    std::lock_guard<std::mutex> lock(engineMutex);
                    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
                        engines[ch].reset();
                        ticksRemaining[ch] = 0;
                        gateHigh[ch] = false;
                        retrigSamplesLeft[ch] = 0;
                        gliding[ch] = false;
                        evalTriggered[ch] = false;
                    }
                    activeField = -1;
                }

                awaitingClockAfterReset = false;
                clockFrozen = false;
                // Real edge re-anchors phase: any remainder of pulse 
                //previous is obsolete (if preserved, an early edge 
                // would trigger a spurious internal tick right after the downbeat 
                // and would freeze the rest of the pulse, stretching the current note).
                fracPos = 0.0;
                nextBoundary = 1;
                // Deferred recompile: apply BEFORE fireInternalTick so the new
                // rules are in place when the downbeat event dequeues.
                if (pendingRecompile.exchange(false)) {
                    applyRecompile();
                }
                if (pendingReset.exchange(false)) {
                    resetAllEngines();
                }
                fireInternalTick(args, true);
            } else if (!awaitingClockAfterReset && !clockFrozen && haveClockTempo &&
                       samplesPerPulse > 0.0) {
                // Between edges: interpolate the inner limits of the pulse
                // (subpulses 1..D-1). Upon reaching 1.0 WITHOUT a new edge, freeze
                // and wait for the actual edge -- the module never runs faster
                // than its clock and remains silent if the clock stops.
                fracPos += 1.0 / samplesPerPulse;
                // Acquire ensures we see the complete grid state (fracPos=0,
                // nextBoundary=1) that was written under the mutex before the
                // release store of pulseSubdivision in applyRecompile().
                int D = pulseSubdivision.load(std::memory_order_acquire);
                while (nextBoundary < D && fracPos * (double)D >= (double)nextBoundary) {
                    fireInternalTick(args, false);
                    nextBoundary++;
                }
                if (fracPos >= 1.0) clockFrozen = true;
            }
        } else {
            // Clock unplugged or module stopped: never stay armed forever,
            // otherwise a later resume without resetOnRun would hold silently.
            awaitingClockAfterReset = false;
            // Apply deferred recompile even when stopped, so the user sees the
            // new rules take effect. The grid is not mid-pulse, so there's no
            // risk of desync.
            if (pendingRecompile.exchange(false)) {
                applyRecompile();
            }
            if (pendingReset.exchange(false)) {
                resetAllEngines();
            }
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

        // ---- Expander output: publish live state (LSystem -> LS-Exp) ----
        // Event-driven (see doExpander above): publishes whenever a rule step
        // fired or a reset happened, never at audio sample rate.
        if (doExpander) {
            Module* xp = findActiveExpander();
            if (xp) {
                toExp.active = true;
                toExp.numChannels = numChannels;
                toExp.scaleIndex = scaleIndex;
                toExp.scaleLen = useExternalScale ? extLen : (int)scale.size();
                toExp.rootVoct = useExternalScale ? extRootVoct : effectiveInternalRoot();
                toExp.externalScale = useExternalScale;
                for (int ch = 0; ch < MAX_CHANNELS; ch++) {
                    toExp.absDegree[ch] = xpAbsDegree[ch];
                    toExp.isRest[ch] = xpIsRest[ch];
                    toExp.isSilent[ch] = xpIsSilent[ch];
                    toExp.ruleIdx[ch] = xpRuleIdx[ch];
                    toExp.stepIdx[ch] = xpStepRep[ch];
                    toExp.stepTotal[ch] = xpStepRepTotal[ch];
                    toExp.stepIdxWhole[ch] = xpStepWhole[ch];
                    toExp.stepTotalWhole[ch] = xpStepWholeTotal[ch];
                }
                // Active scale as voltages for SCALE_OUT thru.
                toExp.scaleVoctChans = 0;
                if (useExternalScale) {
                    for (int i = 0; i < extLen && i < 16; i++)
                        toExp.scaleVoct[i] = extRootVoct + extIntervals[i];
                    toExp.scaleVoctChans = extLen > 16 ? 16 : extLen;
                } else {
                    float rootV = effectiveInternalRoot();
                    // Quantize root to semitone grid for clean thru.
                    float rq = std::round(rootV * 12.f) / 12.f;
                    for (int i = 0; i < (int)scale.size() && i < 16; i++)
                        toExp.scaleVoct[i] = rq + (float)scale[i] / 12.f;
                    toExp.scaleVoctChans = (int)scale.size() > 16 ? 16 : (int)scale.size();
                }
                // Push into the expander's buffer facing us, then request flip
                // (1-sample latency, handled by the engine at end of step).
                if (xp == rightExpander.module && xp->leftExpander.producerMessage) {
                    std::memcpy(xp->leftExpander.producerMessage, &toExp, sizeof(toExp));
                    xp->leftExpander.messageFlipRequested = true;
                } else if (xp == leftExpander.module && xp->rightExpander.producerMessage) {
                    std::memcpy(xp->rightExpander.producerMessage, &toExp, sizeof(toExp));
                    xp->rightExpander.messageFlipRequested = true;
                }
            } else {
                toExp.active = false;
            }
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
        json_object_set_new(rootJ, "gateWidth", json_real(gateWidth));
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
        json_t* gwJ = json_object_get(rootJ, "gateWidth");
        if (gwJ) gateWidth = std::max(0.05f, std::min(1.f, (float)json_real_value(gwJ)));
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
        awaitingClockAfterReset = false;
        fracPos = 0.0;
        nextBoundary = 1;
        lastEdgeSamplePos = -1;
        samplesPerPulse = 0.0;
        pulseSubdivision.store(GATE_MIN_SUBDIVISION, std::memory_order_relaxed);
        aasPulseCounter = 0;
        sampleCounter = 0;
        rGradeListText.clear();
        rDurationListText.clear();
        rDurationPoolRational.clear();
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

    LSystemModuleWidget(LSystemModule* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/LSystem.svg")));

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

        // Row 8: optional restricted values for 'r' (empty = current behavior)
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

        //Separate title text in your svg to add them to the last ones and the overlays do not cover it. 
        //Wrapped in a FramebufferWidget: same as the panel, it renders to 
        //2x and it is rescaled, so the text paths are minified smooth to the 
        //zoom out instead of aliasing.
        widget::FramebufferWidget* titleFb = new widget::FramebufferWidget;
        titleFb->oversample = 2.0;
        titleFb->box.pos = mm2px(Vec(0.0f, 0.0f));
        SvgWidget* titleText = new SvgWidget();
        titleText->setSvg(APP->window->loadSvg(asset::plugin(pluginInstance, "res/RulesTitleText.svg")));
        titleFb->addChild(titleText);
        titleFb->box.size = titleText->box.size;
        addChild(titleFb);


        // Buttons
        addParam(createParamCentered<QuoButton>(mm2px(Vec(33.582f, 101.469f)), module, LSystemModule::RUN_PARAM));
        addChild(createLightCentered<QuoButtonLight<GreenLight>>(mm2px(Vec(33.500f, 101.400f)), module, LSystemModule::RUN_LIGHT));
        addParam(createParamCentered<QuoButton>(mm2px(Vec(21.378f, 101.469f)), module, LSystemModule::RESET_PARAM));

        // Inputs / Outputs
        addInput(createInputCentered<QuoJack>(mm2px(Vec(9.173f, 110.839f)), module, LSystemModule::CLOCK_INPUT));
        addInput(createInputCentered<QuoJack>(mm2px(Vec(21.387f, 110.839f)), module, LSystemModule::RESET_INPUT));
        addInput(createInputCentered<QuoJack>(mm2px(Vec(33.576f, 110.839f)), module, LSystemModule::RUN_INPUT));
        addInput(createInputCentered<QuoJack>(mm2px(Vec(45.681f, 110.839f)), module, LSystemModule::EVAL_INPUT));
        addOutput(createOutputCentered<QuoJack>(mm2px(Vec(70.006f, 110.839f)), module, LSystemModule::EOR_OUTPUT));
        addOutput(createOutputCentered<QuoJack>(mm2px(Vec(57.913f, 110.839f)), module, LSystemModule::RULE_OUTPUT));
        addOutput(createOutputCentered<QuoJack>(mm2px(Vec(85.283f, 110.815f)), module, LSystemModule::GATE_OUTPUT));
        addOutput(createOutputCentered<QuoJack>(mm2px(Vec(97.512f, 110.815f)), module, LSystemModule::PITCH_OUTPUT));
        
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

        menu->addChild(createSubmenuItem("Gate width", string::f("%.0f%%", m->gateWidth * 100.f), [=](Menu* menu) {
            static const int pcts[] = {10, 25, 50, 75, 90, 100};
            for (int p : pcts) {
                menu->addChild(createCheckMenuItem(string::f("%d%%", p), "",
                    [=]() { return std::round(m->gateWidth * 100.f) == p; },
                    [=]() { m->gateWidth = (float)p / 100.f; }));
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
                {"Melodic", LSystemModule::STYLE_MELODIC},
                {"Acid / Techno", LSystemModule::STYLE_ACID_TECHNO},
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
