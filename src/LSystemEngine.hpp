// LSystemEngine.hpp
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <deque>
#include <random>
#include <regex>
#include <algorithm>
#include <climits>
#include <cmath>

namespace lsys {

constexpr int PPQN = 48; // ticks por "ciclo"

inline int floorDiv(int a, int b) {
    int d = a / b;
    int r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) d--;
    return d;
}
inline int floorMod(int a, int b) {
    int r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}

// ---------------------------------------------------------------------
// Tipos básicos
// ---------------------------------------------------------------------

struct GradeValue {
    bool isRest = false;
    int value = 0;

    GradeValue() = default;
    GradeValue(bool rest, int val) : isRest(rest), value(val) {}

    bool operator==(const GradeValue& o) const {
        if (isRest != o.isRest) return false;
        return isRest || value == o.value;
    }
};

struct RuleKey {
    GradeValue grade;
    int durationTicks = 0;

    RuleKey() = default;
    RuleKey(GradeValue g, int ticks) : grade(g), durationTicks(ticks) {}

    bool operator==(const RuleKey& o) const {
        return grade == o.grade && durationTicks == o.durationTicks;
    }
};

// A dequeued, playable event. Carries the RuleKey (grade+duration, used for
// audio output and for chaining to the next rule) plus an optional pitch-glide
// target: a 'fake slide' realized as a fixed per-clock-pulse V/oct step,
// assuming a 48 PPQN clock (see LSystemModule::onClockTick).
struct ResolvedEvent {
    RuleKey key;
    bool hasGlide = false;
    GradeValue glideTarget;
    // True for a step written with a literal '0' duration that had to be kept
    // (as the very last step of a production) purely so its grade can drive
    // rule-routing. The module must skip V/Oct + Gate updates for it.
    bool silent = false;

    ResolvedEvent() = default;
    ResolvedEvent(RuleKey k) : key(k) {}
    ResolvedEvent(GradeValue g, int ticks) : key(g, ticks) {}
};

struct RuleKeyHash {
    size_t operator()(const RuleKey& k) const {
        size_t h1 = std::hash<int>()(k.grade.isRest ? INT_MIN : k.grade.value);
        size_t h2 = std::hash<int>()(k.durationTicks);
        return h1 ^ (h2 << 1);
    }
};

enum class SpecKind { FIXED, RANDOM_ANY, RANDOM_LIST, LAST_RANDOM, LAST_LIST, FILL };

struct WeightedGradeItem {
    bool isRandom = false;
    bool isLastRandom = false; // 'k' inside a <...> list
    bool isLastList = false;   // 'l' inside a <...> list
    GradeValue value;
    double weight = 1.0;
};

struct WeightedDurationItem {
    bool isRandom = false;
    bool isLastRandom = false; // 'k' inside a <...> list
    bool isLastList = false;   // 'l' inside a <...> list
    int ticks = 0;
    double weight = 1.0;
};

// A single entry in the 'r' candidate pools (LSystemModule's r-Degrees / r-Durations
// fields): a fixed value (grade or duration ticks) with an optional weight.
struct WeightedPoolItem {
    int value = 0;
    double weight = 1.0;

    WeightedPoolItem() = default;
    WeightedPoolItem(int v, double w) : value(v), weight(w) {}
};

struct GradeSpec {
    SpecKind kind = SpecKind::FIXED;
    GradeValue fixedValue;
    int chromaStep = 0;
    std::vector<WeightedGradeItem> items;
};

struct DurationSpec {
    SpecKind kind = SpecKind::FIXED;
    int fixedTicks = 0;
    bool zeroMarker = false; // true if this FIXED duration was literally '0' (skip marker)
    int fillTargetTicks = 0; // used by FILL (=T)
    std::vector<WeightedDurationItem> items;
};

struct Symbol {
    GradeSpec grade;
    DurationSpec duration;
    // True if this symbol was written as 'sym^nextSym' (glide) rather than
    // 'sym nextSym' (space): it should pitch-glide toward the following
    // symbol's resolved grade over its own duration.
    bool glideToNext = false;
};

struct Production {
    std::vector<Symbol> symbols;
    int repeatCount = 1;
};

using RuleTable = std::unordered_map<RuleKey, std::vector<Production>, RuleKeyHash>;

enum class FallbackMode { LOOP_TO_INITIATOR, RANDOM_KEY };

// ---------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------

inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

inline std::vector<std::string> splitTopLevel(const std::string& s, char sep) {
    std::vector<std::string> parts;
    int depth = 0;
    std::string buf;
    for (char c : s) {
        if (c == '<') depth++;
        if (c == '>') depth--;
        if (c == sep && depth == 0) {
            parts.push_back(buf);
            buf.clear();
        } else {
            buf.push_back(c);
        }
    }
    parts.push_back(buf);
    return parts;
}

// Sane ceilings for anything the user can type as a plain literal (grade
// values, chroma +N/-N offsets, *N repeat counts, and resolved tick counts).
// These exist purely to keep parsing exception-free and to keep later
// arithmetic (offsets * repeats, etc.) safely within int range -- a musically
// reasonable module never needs numbers anywhere near these limits.
constexpr int MAX_USER_INT = 100000;
constexpr int MAX_REPEAT_COUNT = 256;
constexpr int MAX_TICKS = 1000000; // ~20,833 cycles at 48 PPQN

// Parses a plain (optionally signed) integer literal with a hard magnitude
// limit, and NEVER throws: malformed or too-long input is rejected up front,
// before it ever reaches std::stol, so a huge pasted number is just an
// "invalid rule" instead of an uncaught exception crashing the module.
inline bool parseBoundedInt(const std::string& tokRaw, int& out, int limit = MAX_USER_INT) {
    std::string tok = trim(tokRaw);
    if (tok.empty()) return false;
    size_t start = (tok[0] == '+' || tok[0] == '-') ? 1 : 0;
    if (start >= tok.size()) return false;
    for (size_t i = start; i < tok.size(); i++) {
        if (!isdigit((unsigned char)tok[i])) return false;
    }
    if (tok.size() - start > 9) return false; // far more digits than any valid (in-limit) value could have
    try {
        long v = std::stol(tok);
        if (v < -(long)limit || v > (long)limit) return false;
        out = (int)v;
        return true;
    } catch (...) {
        return false;
    }
}

inline bool parseDurationTicks(const std::string& tokRaw, int& outTicks, bool* wasZero = nullptr) {
    std::string tok = trim(tokRaw);
    static const std::regex fracRe("^(\\d+)/(\\d+)$");
    std::smatch m;
    double cycles;
    try {
        if (std::regex_match(tok, m, fracRe)) {
            double num = std::stod(m[1].str());
            double den = std::stod(m[2].str());
            if (den == 0.0) return false;
            cycles = num / den;
        } else {
            cycles = std::stod(tok);
        }
    } catch (...) {
        return false;
    }
    if (!std::isfinite(cycles)) return false;
    if (wasZero) *wasZero = (cycles == 0.0);
    // Clamp before rounding: extreme values here would otherwise overflow the
    // long->int conversion below (implementation-defined/UB on such values).
    double ticksD = cycles * PPQN;
    if (ticksD > (double)MAX_TICKS) { outTicks = MAX_TICKS; return true; }
    if (ticksD < 1.0) { outTicks = 1; return true; }
    outTicks = (int)std::lround(ticksD);
    return true;
}

inline bool parseGradeValue(const std::string& tokRaw, GradeValue& out) {
    std::string tok = trim(tokRaw);
    if (tok == "s") {
        out.isRest = true;
        out.value = 0;
        return true;
    }
    int v;
    if (!parseBoundedInt(tok, v)) return false;
    out.isRest = false;
    out.value = v;
    return true;
}

inline bool parseGradeSpecBase(const std::string& strRaw, GradeSpec& out) {
    std::string str = trim(strRaw);
    if (str == "r") {
        out.kind = SpecKind::RANDOM_ANY;
        return true;
    }
    if (str == "k") {
        out.kind = SpecKind::LAST_RANDOM;
        return true;
    }
    if (str == "l") {
        out.kind = SpecKind::LAST_LIST;
        return true;
    }
    if (!str.empty() && str.front() == '<' && str.back() == '>') {
        std::string inner = str.substr(1, str.size() - 2);
        out.kind = SpecKind::RANDOM_LIST;
        for (auto& itemStr : splitTopLevel(inner, ',')) {
            std::string it = trim(itemStr);
            size_t colon = it.find(':');
            std::string valStr = it;
            double weight = 1.0;
            if (colon != std::string::npos) {
                valStr = trim(it.substr(0, colon));
                try { weight = std::stod(it.substr(colon + 1)); }
                catch (...) { return false; }
            }
            WeightedGradeItem item;
            item.weight = weight;
            if (valStr == "r") {
                item.isRandom = true;
            } else if (valStr == "k") {
                item.isLastRandom = true;
            } else if (valStr == "l") {
                item.isLastList = true;
            } else {
                if (!parseGradeValue(valStr, item.value)) return false;
            }
            out.items.push_back(item);
        }
        return !out.items.empty();
    }
    out.kind = SpecKind::FIXED;
    return parseGradeValue(str, out.fixedValue);
}

inline bool parseGradeSpec(const std::string& strRaw, GradeSpec& out) {
    std::string str = trim(strRaw);
    int chromaStep = 0;
    std::string base = str;
    if (str.size() >= 2) {
        size_t i = str.size();
        while (i > 0 && isdigit((unsigned char)str[i - 1])) i--;
        if (i < str.size() && i > 0 && (str[i - 1] == '+' || str[i - 1] == '-')) {
            std::string prefix = str.substr(0, i - 1);
            std::string digits = str.substr(i);
            if (!prefix.empty() && !digits.empty()) {
                base = prefix;
                if (!parseBoundedInt(str.substr(i - 1), chromaStep)) return false;
            }
        }
    }
    if (!parseGradeSpecBase(base, out)) return false;
    out.chromaStep = chromaStep;
    return true;
}

inline bool parseDurationSpec(const std::string& strRaw, DurationSpec& out) {
    std::string str = trim(strRaw);

    // Fill duration: =T
    // Completes elapsed time inside the current repetition toward the
    // smallest multiple of T that is >= elapsed.
    if (!str.empty() && str.front() == '=') {
        std::string target = trim(str.substr(1));
        if (target.empty()) return false;

        // Accept plain decimal numbers and simple unsigned fractions.
        // Examples: =1, =0.5, =1/2, =3/4
        static const std::regex plainRe("^([0-9]+(\\.[0-9]*)?|\\.[0-9]+)$");
        static const std::regex fracRe("^[0-9]+/[0-9]+$");

        if (!std::regex_match(target, plainRe) &&
            !std::regex_match(target, fracRe)) {
            return false;
        }

        int ticks = 0;
        bool wasZero = false;
        if (!parseDurationTicks(target, ticks, &wasZero)) return false;

        // A fill target of zero makes no sense.
        if (wasZero || ticks <= 0) return false;

        out.kind = SpecKind::FILL;
        out.fixedTicks = 0;
        out.zeroMarker = false;
        out.fillTargetTicks = ticks;
        out.items.clear();
        return true;
    }

    if (str == "r") {
        out.kind = SpecKind::RANDOM_ANY;
        return true;
    }

    if (str == "k") {
        out.kind = SpecKind::LAST_RANDOM;
        return true;
    }

    if (str == "l") {
        out.kind = SpecKind::LAST_LIST;
        return true;
    }

    if (!str.empty() && str.front() == '<' && str.back() == '>') {
        std::string inner = str.substr(1, str.size() - 2);
        out.kind = SpecKind::RANDOM_LIST;

        for (auto& itemStr : splitTopLevel(inner, ',')) {
            std::string it = trim(itemStr);

            size_t colon = it.find(':');
            std::string valStr = it;
            double weight = 1.0;

            if (colon != std::string::npos) {
                valStr = trim(it.substr(0, colon));
                try {
                    weight = std::stod(it.substr(colon + 1));
                } catch (...) {
                    return false;
                }
            }

            WeightedDurationItem item;
            item.weight = weight;

            // Fill is not allowed inside duration lists.
            if (!valStr.empty() && valStr.front() == '=') return false;

            if (valStr == "r") {
                item.isRandom = true;
            } else if (valStr == "k") {
                item.isLastRandom = true;
            } else if (valStr == "l") {
                item.isLastList = true;
            } else {
                if (!parseDurationTicks(valStr, item.ticks)) return false;
            }

            out.items.push_back(item);
        }

        return !out.items.empty();
    }

    out.kind = SpecKind::FIXED;
    return parseDurationTicks(str, out.fixedTicks, &out.zeroMarker);
}

inline bool parseRuleLine(const std::string& lineRaw, RuleTable& table, std::string& error) {
    std::string line = trim(lineRaw);
    if (line.empty() || line.rfind("--", 0) == 0) return true;

    size_t arrow = line.find("->");
    if (arrow == std::string::npos) {
        error = "falta '->'";
        return false;
    }
    std::string left = trim(line.substr(0, arrow));
    std::string right = trim(line.substr(arrow + 2));

    auto leftParts = splitTopLevel(left, ',');
    if (leftParts.size() != 2) {
        error = "clave invalida (se esperaba 'grado,duracion')";
        return false;
    }
    GradeValue keyGrade;
    int keyTicks;
    if (!parseGradeValue(trim(leftParts[0]), keyGrade)) {
        error = "grado de clave invalido";
        return false;
    }
    if (!parseDurationTicks(trim(leftParts[1]), keyTicks)) {
        error = "duracion de clave invalida";
        return false;
    }
    RuleKey key{keyGrade, keyTicks};

    Production prod;
    prod.repeatCount = 1;
    bool repeatCountSet = false;

    std::vector<std::string> tokens;
    std::vector<bool> tokenGlideNext; // tokenGlideNext[i]: token i was written as 'i^i+1' (glide toward next)
    {
        std::string cur;
        int depth = 0;
        for (char c : right) {
            if (c == '<') depth++;
            if (c == '>') depth--;
            bool isSep = depth == 0 && (isspace((unsigned char)c) || c == '^');
            if (isSep) {
                if (!cur.empty()) {
                    tokens.push_back(cur);
                    tokenGlideNext.push_back(c == '^');
                    cur.clear();
                }
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) { tokens.push_back(cur); tokenGlideNext.push_back(false); }
    }
    if (tokens.empty()) {
        error = "produccion vacia";
        return false;
    }

    for (size_t ti = 0; ti < tokens.size(); ti++) {
        const std::string& tok = tokens[ti];
        if (tok.size() >= 2 && tok[0] == '*' &&
            std::all_of(tok.begin() + 1, tok.end(), [](char c) { return isdigit((unsigned char)c); })) {
            if (repeatCountSet) {
                error = "solo se permite un operador *N por regla (ya se especifico uno antes de '" + tok + "')";
                return false;
            }
            int rc;
            if (!parseBoundedInt(tok.substr(1), rc, MAX_REPEAT_COUNT)) {
                error = "cantidad de repeticion invalida: '" + tok + "'";
                return false;
            }
            prod.repeatCount = std::max(1, rc);
            repeatCountSet = true;
            continue;
        }
        auto pair = splitTopLevel(tok, ',');
        if (pair.size() != 2) {
            error = "simbolo invalido: '" + tok + "'";
            return false;
        }
        Symbol sym;
        if (!parseGradeSpec(pair[0], sym.grade)) {
            error = "grado invalido: '" + pair[0] + "'";
            return false;
        }
        if (!parseDurationSpec(pair[1], sym.duration)) {
            error = "duracion invalida: '" + pair[1] + "'";
            return false;
        }
        sym.glideToNext = tokenGlideNext[ti];
        prod.symbols.push_back(sym);
    }

    table[key].push_back(prod);
    return true;
}

// ---------------------------------------------------------------------
// Motor L-system
// ---------------------------------------------------------------------

class LSystemEngine {
public:
    void setRules(RuleTable table) { rules = std::move(table); }
    void setKeyOrder(std::unordered_map<RuleKey, int, RuleKeyHash> order) { keyOrder = std::move(order); }
    // Parallel to the RuleTable: for each key, the field row indices (top to bottom)
    // that contributed a production, in the same order as the productions vector.
    // Lets the UI know exactly which row fired, even when several rows share a key.
    void setKeyFieldIndices(std::unordered_map<RuleKey, std::vector<int>, RuleKeyHash> indices) {
        keyFieldIndices = std::move(indices);
    }
    void setInitiator(RuleKey init) { initiator = init; }
    void setFallback(FallbackMode m) { fallback = m; }
    void setGradeRange(int mn, int mx) { gradeMin = mn; gradeMax = mx; }
    // Optional restricted value sets for 'r' (RANDOM_ANY). Empty = unrestricted
    // (grade: uniform over [gradeMin,gradeMax]; duration: the built-in pool).
    void setRandomGradeList(std::vector<WeightedPoolItem> list) { randomGradeList = std::move(list); }
    void setRandomDurationList(std::vector<WeightedPoolItem> list) { randomDurationList = std::move(list); }

    void reset() {
        current = initiator;
        queue.clear();
        firedThisStep = false;
        eorFired = false;
        eosFired = false;
        lastRandomGrade = 1;
        lastRandomDuration = PPQN;
        lastListGrade = 1;
        lastListDuration = PPQN;
        productionCycleIndex.clear();
        // Starting the sequence from the top isn't a completion event: suppress
        // EOR/EOS on the very first rule that fires after this reset (it's
        // always Rule 1), so a Reset pulse doesn't masquerade as an "end of".
        suppressEndSignalsOnce = true;
    }

    bool nextEvent(ResolvedEvent& outEvent) {
        firedThisStep = false;
        eorFired = false;
        eosFired = false;

        int guard = 0;
        while (queue.empty()) {
            if (++guard > maxExpansions) return false;
            ExpandResult r = expandOnce();
            if (r == ExpandResult::STOPPED) return false;
        }
        outEvent = queue.front();
        queue.pop_front();
        current = outEvent.key;
        return true;
    }

    bool isQueueEmpty() const { return queue.empty(); }
    RuleKey getLastFiredKey() const { return lastFiredKey; }
    int getLastFiredFieldIndex() const { return lastFiredFieldIndex; }
    void setCurrentKey(RuleKey k) { current = k; }
    void setForcedFieldIndex(int idx) { forcedFieldIndex = idx; }
    void resetTo(RuleKey k) {
        current = k;
        forcedFieldIndex = -1;
        queue.clear();
        firedThisStep = false;
        eorFired = false;
        eosFired = false;
        lastRandomGrade = 1;
        lastRandomDuration = PPQN;
        lastListGrade = 1;
        lastListDuration = PPQN;
        productionCycleIndex.clear();
        suppressEndSignalsOnce = true;
    }
    void resetToField(int fieldIdx, RuleKey k) {
        current = k;
        forcedFieldIndex = fieldIdx;
        queue.clear();
        firedThisStep = false;
        eorFired = false;
        eosFired = false;
        lastRandomGrade = 1;
        lastRandomDuration = PPQN;
        lastListGrade = 1;
        lastListDuration = PPQN;
        productionCycleIndex.clear();
        suppressEndSignalsOnce = true;
    }

    RuleKey getCurrentKey() const { return current; }

    bool firedThisStep = false;
    bool eorFired = false;
    bool eosFired = false;
    RuleKey lastFiredKey;
    int lastFiredFieldIndex = -1; // the actual field row that fired, or -1 if unknown
    int forcedFieldIndex = -1;    // if >= 0, forces the next expansion to evaluate this exact row
    int maxExpansions = 200;
    bool suppressEndSignalsOnce = false; // true right after reset(), until the first firing

private:
    enum class ExpandResult { PRODUCED, RETRY, STOPPED };

    static int wrapGrade(int g, int mn, int mx) {
        int range = mx - mn + 1;
        if (range <= 0) return g;
        return floorMod(g - mn, range) + mn;
    }

    // Priority: (1) exact grade+duration match is handled by the caller via
    // rules.find(). This function only covers (2) same grade, nearest duration.
    // Ties (same distance) are broken by field order (top row wins), never at random.
    const std::vector<Production>* findByGrade(const RuleKey& target, RuleKey& matchedKeyOut) {
        int bestDiff = INT_MAX;
        int bestOrder = INT_MAX;
        const std::pair<const RuleKey, std::vector<Production>>* best = nullptr;
        for (auto& kv : rules) {
            if (kv.first.grade == target.grade) {
                int diff = std::abs(kv.first.durationTicks - target.durationTicks);
                int order = keyOrderOf(kv.first);
                if (diff < bestDiff || (diff == bestDiff && order < bestOrder)) {
                    bestDiff = diff;
                    bestOrder = order;
                    best = &kv;
                }
            }
        }
        if (!best) return nullptr;
        matchedKeyOut = best->first;
        return &best->second;
    }

    int keyOrderOf(const RuleKey& k) const {
        auto it = keyOrder.find(k);
        return it != keyOrder.end() ? it->second : INT_MAX;
    }

    GradeValue resolveGrade(const GradeSpec& spec) {
        switch (spec.kind) {
            case SpecKind::FIXED:
                return spec.fixedValue;
            case SpecKind::LAST_RANDOM: {
                GradeValue v; v.isRest = false; v.value = lastRandomGrade;
                return v;
            }
            case SpecKind::LAST_LIST: {
                GradeValue v; v.isRest = false; v.value = lastListGrade;
                return v;
            }
            case SpecKind::RANDOM_ANY: {
                GradeValue v; v.isRest = false; v.value = randomGradeAny();
                return v;
            }
            case SpecKind::RANDOM_LIST:
                return pickWeightedGrade(spec.items);
            case SpecKind::FILL:
                return GradeValue(true, 0); // FILL solo aplica a duraciones
        }
        return {};
    }

    int resolveDuration(const DurationSpec& spec) {
        switch (spec.kind) {
            case SpecKind::FIXED: return spec.fixedTicks;
            case SpecKind::LAST_RANDOM: return lastRandomDuration;
            case SpecKind::LAST_LIST: return lastListDuration;
            case SpecKind::RANDOM_ANY: return randomDurationAny();
            case SpecKind::RANDOM_LIST: return pickWeightedDuration(spec.items);
            case SpecKind::FILL: return spec.fillTargetTicks; // normally resolved in expandOnce()
        }
        return PPQN;
    }

    int randomGradeAny() {
        if (!randomGradeList.empty()) {
            lastRandomGrade = pickWeightedPoolItem(randomGradeList);
        } else {
            std::uniform_int_distribution<int> dist(gradeMin, gradeMax);
            lastRandomGrade = dist(rng);
        }
        return lastRandomGrade;
    }

    int randomDurationAny() {
        if (!randomDurationList.empty()) {
            lastRandomDuration = pickWeightedPoolItem(randomDurationList);
        } else if (!durationPool.empty()) {
            std::uniform_int_distribution<size_t> dist(0, durationPool.size() - 1);
            lastRandomDuration = durationPool[dist(rng)];
        } else {
            lastRandomDuration = PPQN;
        }
        return lastRandomDuration;
    }

    // Weighted pick for the r-pools (LSystemModule's r-Degrees / r-Durations
    // fields): "1/2, 1/3:0.1, 1/4:0.5, 1" -- entries without ':weight' default to 1.0.
    int pickWeightedPoolItem(const std::vector<WeightedPoolItem>& items) {
        double total = 0;
        for (auto& it : items) total += it.weight;
        if (total <= 0.0) total = (double)items.size(); // safety: fall back to uniform-ish
        std::uniform_real_distribution<double> dist(0.0, total);
        double r = dist(rng), acc = 0;
        for (auto& it : items) {
            acc += it.weight;
            if (r <= acc) return it.value;
        }
        return items.back().value;
    }

    GradeValue pickWeightedGrade(const std::vector<WeightedGradeItem>& items) {
        double total = 0;
        for (auto& it : items) total += it.weight;
        std::uniform_real_distribution<double> dist(0.0, total);
        double r = dist(rng), acc = 0;
        GradeValue result;
        bool found = false;
        for (auto& it : items) {
            acc += it.weight;
            if (r <= acc) {
                if (it.isRandom) { result.isRest = false; result.value = randomGradeAny(); }
                else if (it.isLastRandom) { result.isRest = false; result.value = lastRandomGrade; }
                else if (it.isLastList) { result.isRest = false; result.value = lastListGrade; }
                else { result = it.value; }
                found = true;
                break;
            }
        }
        if (!found) {
            const auto& last = items.back();
            if (last.isRandom) { result.isRest = false; result.value = randomGradeAny(); }
            else if (last.isLastRandom) { result.isRest = false; result.value = lastRandomGrade; }
            else if (last.isLastList) { result.isRest = false; result.value = lastListGrade; }
            else { result = last.value; }
        }
        lastListGrade = result.value; // 'l' recalls this, no matter which branch produced it
        return result;
    }

    int pickWeightedDuration(const std::vector<WeightedDurationItem>& items) {
        double total = 0;
        for (auto& it : items) total += it.weight;
        std::uniform_real_distribution<double> dist(0.0, total);
        double r = dist(rng), acc = 0;
        int result = 0;
        bool found = false;
        for (auto& it : items) {
            acc += it.weight;
            if (r <= acc) {
                if (it.isRandom) result = randomDurationAny();
                else if (it.isLastRandom) result = lastRandomDuration;
                else if (it.isLastList) result = lastListDuration;
                else result = it.ticks;
                found = true;
                break;
            }
        }
        if (!found) {
            const auto& last = items.back();
            if (last.isRandom) result = randomDurationAny();
            else if (last.isLastRandom) result = lastRandomDuration;
            else if (last.isLastList) result = lastListDuration;
            else result = last.ticks;
        }
        lastListDuration = result; // 'l' recalls this, no matter which branch produced it
        return result;
    }

    const Production& pickProduction(const RuleKey& key, const std::vector<Production>& list, size_t& usedIndexOut) {
        if (list.size() == 1) { usedIndexOut = 0; return list[0]; }
        size_t& idx = productionCycleIndex[key];
        usedIndexOut = idx % list.size();
        const Production& p = list[usedIndexOut];
        idx = (idx + 1) % list.size();
        return p;
    }

    RuleKey pickRandomRuleKeyFromTable() {
        std::uniform_int_distribution<size_t> dist(0, rules.size() - 1);
        size_t target = dist(rng);
        size_t i = 0;
        for (auto& kv : rules) {
            if (i == target) return kv.first;
            i++;
        }
        return current;
    }

    ExpandResult expandOnce() {
        const std::vector<Production>* prodList = nullptr;
        RuleKey matchedKey = current;
        size_t usedIndex = 0;
        bool forceMatched = false;

        if (forcedFieldIndex >= 0) {
            for (auto& kv : keyFieldIndices) {
                for (size_t p = 0; p < kv.second.size(); p++) {
                    if (kv.second[p] == forcedFieldIndex) {
                        matchedKey = kv.first;
                        auto rIt = rules.find(matchedKey);
                        if (rIt != rules.end() && p < rIt->second.size()) {
                            prodList = &rIt->second;
                            usedIndex = p;
                            forceMatched = true;
                            break;
                        }
                    }
                }
                if (forceMatched) break;
            }
            forcedFieldIndex = -1; // consume forced row
        }

        if (!forceMatched) {
            auto it = rules.find(current);
            if (it != rules.end()) {
                prodList = &it->second;
            } else {
                prodList = findByGrade(current, matchedKey);
            }
        }

        if (!prodList) {
            switch (fallback) {
                case FallbackMode::LOOP_TO_INITIATOR:
                    current = initiator;
                    return ExpandResult::RETRY;
                case FallbackMode::RANDOM_KEY:
                    if (rules.empty()) return ExpandResult::STOPPED;
                    current = pickRandomRuleKeyFromTable();
                    return ExpandResult::RETRY;
            }
        }

        lastFiredKey = matchedKey;
        firedThisStep = true;
        if (!suppressEndSignalsOnce) eorFired = true;

        const Production& prod = forceMatched ? (*prodList)[usedIndex] : pickProduction(matchedKey, *prodList, usedIndex);
        auto fieldsIt = keyFieldIndices.find(matchedKey);
        lastFiredFieldIndex = (fieldsIt != keyFieldIndices.end() && usedIndex < fieldsIt->second.size())
            ? fieldsIt->second[usedIndex] : -1;

        // "Sequence complete": specifically row 0 (Rule 1, the initiator) firing
        // again -- not just any row that happens to share its key/duplicate rows
        // of the same key don't count, since they're distinct productions.
        if (lastFiredFieldIndex == 0 && !suppressEndSignalsOnce) eosFired = true;
        suppressEndSignalsOnce = false;

        for (int rep = 0; rep < prod.repeatCount; rep++) {
            size_t n = prod.symbols.size();
            std::vector<GradeValue> resolvedGrades(n);
            std::vector<int> resolvedTicks(n);
            std::vector<char> silentFill(n, 0);

            // Elapsed ticks inside this repetition, used by '=T' fill durations.
            long long elapsed = 0;

            for (size_t i = 0; i < n; i++) {
                const Symbol& sym = prod.symbols[i];

                GradeValue base = resolveGrade(sym.grade);
                GradeValue finalGrade = base;

                if (!base.isRest && sym.grade.chromaStep != 0) {
                    // (rep + 1) so the offset already applies on the very first (and possibly only)
                    // repetition, and grows cumulatively on further repeats when *N is used.
                    finalGrade.value = wrapGrade(base.value + sym.grade.chromaStep * (rep + 1), gradeMin, gradeMax);
                }

                resolvedGrades[i] = finalGrade;

                bool isVeryLast = (rep == prod.repeatCount - 1) && (i == n - 1);

                if (sym.duration.kind == SpecKind::FILL) {
                    int target = sym.duration.fillTargetTicks;

                    // Smallest multiple of target that is >= elapsed.
                    long long q = (elapsed + target - 1) / target;
                    if (q < 1) q = 1;

                    long long boundary = q * (long long)target;
                    long long fill = boundary - elapsed;

                    if (fill < 0) fill = 0;
                    if (fill > MAX_TICKS) fill = MAX_TICKS;

                    if (fill == 0) {
                        // Exact multiple: keep a 1-tick routing step.
                        // If it is a note degree, mark it silent so it does not
                        // trigger a new note; if it is already a rest, let it
                        // behave as a normal rest.
                        resolvedTicks[i] = 1;
                        silentFill[i] = resolvedGrades[i].isRest ? 0 : 1;
                        elapsed += 1;
                    } else {
                        resolvedTicks[i] = (int)fill;
                        elapsed += fill;
                    }
                } else {
                    resolvedTicks[i] = resolveDuration(sym.duration);

                    bool isZeroStep = (sym.duration.kind == SpecKind::FIXED && sym.duration.zeroMarker);
                    bool omittedZero = (isZeroStep && !isVeryLast);

                    if (!omittedZero) {
                        elapsed += resolvedTicks[i];
                    }
                }
            }

    for (size_t i = 0; i < n; i++) {
        const Symbol& sym = prod.symbols[i];

        bool isZeroStep = (sym.duration.kind == SpecKind::FIXED && sym.duration.zeroMarker);
        bool isVeryLast = (rep == prod.repeatCount - 1) && (i == n - 1);

        if (isZeroStep && !isVeryLast) {
            // Fully omitted: no audible/timed footprint at all, as if this
            // step were never written. Its grade is still resolved above,
            // so any glide targeting it still works correctly.
            continue;
        }

        ResolvedEvent ev(resolvedGrades[i], resolvedTicks[i]);

        if (isZeroStep || silentFill[i]) {
            // Zero-duration routing step, or exact fill (=0 calculated).
            // The module must not treat it as a normal audible note.
            ev.silent = true;
        }

        if (sym.glideToNext && i + 1 < n) {
            ev.hasGlide = true;
            ev.glideTarget = resolvedGrades[i + 1];
        }

        queue.push_back(ev);
    }
}
        return ExpandResult::PRODUCED;
    }

    RuleTable rules;
    std::unordered_map<RuleKey, int, RuleKeyHash> keyOrder;
    std::unordered_map<RuleKey, std::vector<int>, RuleKeyHash> keyFieldIndices;
    std::unordered_map<RuleKey, size_t, RuleKeyHash> productionCycleIndex;
    RuleKey initiator;
    FallbackMode fallback = FallbackMode::LOOP_TO_INITIATOR;
    int gradeMin = -8, gradeMax = 16;
    std::vector<int> durationPool = {PPQN, PPQN / 2, PPQN * 2, PPQN / 4};
    std::vector<WeightedPoolItem> randomGradeList;    // optional restricted candidates for 'r' (grade)
    std::vector<WeightedPoolItem> randomDurationList; // optional restricted candidates for 'r' (duration), in ticks

    RuleKey current;
    std::deque<ResolvedEvent> queue;

    int lastRandomGrade = 1;
    int lastRandomDuration = PPQN;
    int lastListGrade = 1;      // last value produced by ANY <...> list, for 'l'
    int lastListDuration = PPQN;

    std::mt19937 rng{std::random_device{}()};
};

} // namespace lsys
