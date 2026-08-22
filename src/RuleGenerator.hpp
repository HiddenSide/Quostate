// RuleGenerator.hpp
// Generador de reglas aleatorias para el módulo LSystem.
// Extraído de LSystem.cpp para mantener el módulo principal enfocado
// en runtime y UI. No depende del Module: recibe un GeneratorConfig
// y devuelve un GeneratedRuleSet.
#pragma once

#include <string>
#include <vector>
#include <random>
#include <algorithm>
#include <cstdint>
#include <functional>
#include "LSystemEngine.hpp"

namespace lgen {

// =======================================================================
// Tipos y configuración
// =======================================================================

enum GenStyle {
    STYLE_MELODIC = 0,
    STYLE_ACID_TECHNO,
    STYLE_AMBIENT,
    STYLE_COMPLEX_CHAOS,
    NUM_GEN_STYLES
};

struct GenProfile {
    int minSteps = 2;
    int maxSteps = 3;
    float rProb = 0.35f;
    float kProb = 0.25f;
    float lProb = 0.15f;
    float listProb = 0.25f;
    float restProb = 0.12f;
    float glideProb = 0.20f;
    float repeatProb = 0.30f;
    float branchExitProb = 0.35f;
    // Duraciones candidatas como texto, en múltiplos/fracciones de UN PULSO
    // de clock ("1" = un pulso completo, "1/4" = un cuarto de pulso).
    std::vector<const char*> durations;
};

// Parámetros del entorno que el generador necesita conocer.
struct GeneratorConfig {
    int numFields = 7;
    int ruleFieldMaxChars = 50;
    int listFieldMaxChars = 24;
    int gradeMin = -8;
    int gradeMax = 16;
};

// Resultado completo de una generación.
struct GeneratedRuleSet {
    std::vector<std::string> rules;   // una entrada por campo de regla
    std::string gradePool;            // texto para el pool de grados 'r'
    std::string durationPool;         // texto para el pool de duraciones 'r'
};

// =======================================================================
// Perfil por estilo
// =======================================================================

inline GenProfile getProfileForStyle(GenStyle style) {
    GenProfile p;
    switch (style) {
        case STYLE_ACID_TECHNO:
            p.minSteps = 2;
            p.maxSteps = 3;
            p.rProb = 0.35f;
            p.kProb = 0.30f;
            p.lProb = 0.15f;
            p.listProb = 0.20f;
            p.restProb = 0.18f;
            p.glideProb = 0.35f;
            p.repeatProb = 0.45f;
            p.branchExitProb = 0.30f;
            p.durations = {"1/4", "1/8", "1/2", "1/3"};
            break;
        case STYLE_AMBIENT:
            p.minSteps = 1;
            p.maxSteps = 2;
            p.rProb = 0.40f;
            p.kProb = 0.25f;
            p.lProb = 0.20f;
            p.listProb = 0.35f;
            p.restProb = 0.08f;
            p.glideProb = 0.30f;
            p.repeatProb = 0.35f;
            p.branchExitProb = 0.40f;
            p.durations = {"1", "2", "1/2", "3/4"};
            break;
        case STYLE_COMPLEX_CHAOS:
            p.minSteps = 2;
            p.maxSteps = 3;
            p.rProb = 0.40f;
            p.kProb = 0.25f;
            p.lProb = 0.20f;
            p.listProb = 0.40f;
            p.restProb = 0.15f;
            p.glideProb = 0.25f;
            p.repeatProb = 0.35f;
            p.branchExitProb = 0.50f;
            p.durations = {"1/4", "1/2", "1", "3/4", "1/3"};
            break;
        case STYLE_MELODIC:
        default:
            p.minSteps = 2;
            p.maxSteps = 3;
            p.rProb = 0.35f;
            p.kProb = 0.25f;
            p.lProb = 0.15f;
            p.listProb = 0.25f;
            p.restProb = 0.12f;
            p.glideProb = 0.20f;
            p.repeatProb = 0.30f;
            p.branchExitProb = 0.35f;
            p.durations = {"1/4", "1/2", "1", "3/4"};
            break;
    }
    return p;
}

// =======================================================================
// Seed
// =======================================================================

// Determinista para un mismo texto. Si seedText está vacío, genera uno
// aleatorio y lo escribe de vuelta en seedText.
inline uint32_t resolveSeed(std::string& seedText) {
    if (seedText.empty()) {
        std::random_device rd;
        uint32_t s = rd();
        seedText = std::to_string(s);
        return s;
    }
    try {
        unsigned long v = std::stoul(seedText);
        return (uint32_t)(v & 0xFFFFFFFFu);
    } catch (...) {
        return (uint32_t)(std::hash<std::string>{}(seedText) & 0xFFFFFFFFu);
    }
}

// =======================================================================
// Generación de pools para 'r'
// =======================================================================

inline void generatePools(std::mt19937& rng, GenStyle style,
                          const GeneratorConfig& cfg,
                          std::string& gradePoolOut,
                          std::string& durationPoolOut) {
    // ---- 1. Pool de grados ----------------------------------------
    std::vector<std::pair<int, int>> degCandidates;
    switch (style) {
        case STYLE_ACID_TECHNO:
            degCandidates = {{1, 5}, {-3, 2}, {8, 3}, {3, 2}, {5, 2}, {-1, 1}};
            break;
        case STYLE_AMBIENT:
            degCandidates = {{1, 4}, {3, 3}, {5, 3}, {7, 2}, {8, 2}, {10, 1}};
            break;
        case STYLE_COMPLEX_CHAOS:
            degCandidates = {{1, 4}, {2, 2}, {3, 3}, {5, 3}, {7, 2}, {-2, 1}};
            break;
        case STYLE_MELODIC:
        default:
            degCandidates = {{1, 4}, {3, 3}, {5, 3}, {7, 2}, {8, 2}, {-2, 1}};
            break;
    }
    std::shuffle(degCandidates.begin(), degCandidates.end(), rng);

    std::string dStr;
    for (size_t i = 0; i < degCandidates.size(); i++) {
        int g = std::max(cfg.gradeMin, std::min(cfg.gradeMax, degCandidates[i].first));
        int w = degCandidates[i].second;
        std::string item = std::to_string(g) + (w > 1 ? ":" + std::to_string(w) : "");
        if (!dStr.empty() &&
            dStr.size() + 1 + item.size() > (size_t)cfg.listFieldMaxChars) break;
        if (!dStr.empty()) dStr += ",";
        dStr += item;
    }
    gradePoolOut = dStr;

    // ---- 2. Pool de duraciones ------------------------------------
    std::vector<std::pair<std::string, int>> durCandidates;
    switch (style) {
        case STYLE_ACID_TECHNO:
            durCandidates = {{"1/4", 5}, {"1/8", 3}, {"1/2", 2}, {"1/3", 1}};
            break;
        case STYLE_AMBIENT:
            durCandidates = {{"1", 4}, {"2", 2}, {"1/2", 3}, {"3/4", 1}};
            break;
        case STYLE_COMPLEX_CHAOS:
            durCandidates = {{"1/4", 4}, {"1/2", 3}, {"1/3", 2}, {"1", 1}};
            break;
        case STYLE_MELODIC:
        default:
            durCandidates = {{"1/4", 4}, {"1/2", 3}, {"1", 2}, {"3/4", 1}};
            break;
    }
    std::shuffle(durCandidates.begin(), durCandidates.end(), rng);

    std::string tStr;
    for (size_t i = 0; i < durCandidates.size(); i++) {
        std::string item = durCandidates[i].first +
            (durCandidates[i].second > 1 ? ":" + std::to_string(durCandidates[i].second) : "");
        if (!tStr.empty() &&
            tStr.size() + 1 + item.size() > (size_t)cfg.listFieldMaxChars) break;
        if (!tStr.empty()) tStr += ",";
        tStr += item;
    }
    durationPoolOut = tStr;
}

// =======================================================================
// Generador genérico (Melodic / Ambient / Complex)
// =======================================================================
// Topología: ciclo hamiltoniano + atajos probabilísticos (grafo ergódico).
inline void generateRules(std::mt19937& rng, GenStyle style,
                          const GeneratorConfig& cfg,
                          std::vector<std::string>& rulesOut) {
    rulesOut.assign(cfg.numFields, "");

    GenProfile prof = getProfileForStyle(style);

    // ---- Iniciadores distintos arraigados en la escala -------------
    std::vector<int> degreeBase = {1, 3, 5, 2, 4, 7, 8, -2, 6, -1};
    std::vector<int> pickedGrades;
    pickedGrades.push_back(1); // Rule 1 siempre es la tónica (grado 1)

    std::vector<int> poolForRest(degreeBase.begin() + 1, degreeBase.end());
    std::shuffle(poolForRest.begin(), poolForRest.end(), rng);
    for (int i = 0; i < cfg.numFields - 1; i++) {
        int g = std::max(cfg.gradeMin, std::min(cfg.gradeMax, poolForRest[i]));
        pickedGrades.push_back(g);
    }

    std::uniform_int_distribution<int> durDist(0, (int)prof.durations.size() - 1);

    struct InitiatorDef { int grade; std::string durText; };
    std::vector<InitiatorDef> initiators(cfg.numFields);
    for (int i = 0; i < cfg.numFields; i++) {
        int di = (i == 0) ? (prof.durations.size() > 1 ? 1 : 0) : durDist(rng);
        initiators[i] = { pickedGrades[i], prof.durations[di] };
    }

    // ---- Grafo dirigido: ciclo hamiltoniano ------------------------
    std::vector<int> order(cfg.numFields);
    for (int i = 0; i < cfg.numFields; i++) order[i] = i;
    std::shuffle(order.begin() + 1, order.end(), rng);

    std::vector<int> cycleNext(cfg.numFields);
    for (int i = 0; i < cfg.numFields; i++) {
        int cur = order[i];
        int nxt = order[(i + 1) % cfg.numFields];
        cycleNext[cur] = nxt;
    }

    std::uniform_real_distribution<float> prob(0.f, 1.f);
    std::uniform_int_distribution<int> stepsDist(prof.minSteps, prof.maxSteps);
    std::uniform_int_distribution<int> offsetDist(1, 2);

    // ---- Contorno melódico -----------------------------------------
    // Caminante de grados con pesos por intervalo: movimiento conjunto
    // (-1/+1) favorecido sobre saltos, persistencia de direccion y
    // recuperacion obligada tras un salto grande. El primer grado fijo del
    // cuerpo se ajusta al tono de acorde (1/3/5/8) mas cercano, para que
    // cada frase arranque consonante con la tonica.
    const int CHORD_TONES[4] = {1, 3, 5, 8};
    auto nearestChordTone = [&](int g) {
        int best = CHORD_TONES[0], bd = INT_MAX;
        for (int c : CHORD_TONES) {
            int d = std::abs(c - g);
            if (d < bd || (d == bd && c < best)) { bd = d; best = c; }
        }
        return best;
    };

    struct IntervalW { int iv; float w; };
    auto pickInterval = [&](int& dir, int prevInt) -> int {
        static const IntervalW base[] = {
            {-1, 6.f}, {1, 6.f}, {-2, 3.f}, {2, 3.f}, {-3, 1.f}, {3, 1.f},
            {-4, 0.6f}, {4, 0.6f}, {-7, 0.4f}, {7, 0.4f}};
        const size_t N = sizeof(base) / sizeof(base[0]);
        float ws[N];
        float tot = 0;
        for (size_t k = 0; k < N; k++) {
            float w = base[k].w;
            if (std::abs(prevInt) >= 4 && std::abs(base[k].iv) > 2)
                w = 0.f; // tras un salto, solo grados conjuntos (recuperacion)
            if (dir != 0 && ((base[k].iv > 0) == (dir > 0)))
                w *= 1.6f; // persistencia de direccion
            ws[k] = w;
            tot += w;
        }
        std::uniform_real_distribution<float> dist(0.f, tot);
        float r = dist(rng);
        float acc = 0;
        size_t sel = 0;
        for (; sel + 1 < N; sel++) {
            acc += ws[sel];
            if (r <= acc) break;
        }
        int iv = base[sel].iv;
        dir = (iv > 0) ? 1 : -1;
        return iv;
    };

    for (int i = 0; i < cfg.numFields; i++) {
        bool valid = false;
        int attempts = 0;
        std::string finalRule;

        while (!valid && attempts < 20) {
            attempts++;
            // Estado del contorno y celula ritmica: frescos por intento para
            // que cada linea candidata sea autosuficiente.
            int contourCur = initiators[i].grade;
            int contourDir = 0;
            int prevIntv = 0;
            bool snappedToChord = false;
            // Celula ritmica del cuerpo: 45% unica duracion repetida (motivo
            // hipnotico), 55% sorteos independientes (variacion).
            bool uniRhythm = prob(rng) < 0.45f;
            std::string uniDur = prof.durations[durDist(rng)];

            std::string line = std::to_string(initiators[i].grade) + "," +
                               initiators[i].durText + " -> ";

            int numSteps = stepsDist(rng);
            bool rUsed = false;
            bool listUsed = false;
            bool lastWasRest = false;
            std::vector<std::string> stepTokens;
            std::vector<bool> glideFlags;

            for (int s = 0; s < numSteps; s++) {
                float pG = prob(rng);
                float pD = prob(rng);
                std::string gStr;
                std::string dStr;

                // Grado
                if (pG < prof.restProb && !lastWasRest && s > 0) {
                    gStr = "s";
                    lastWasRest = true;
                } else if (pG < prof.restProb + prof.rProb) {
                    gStr = "r";
                    rUsed = true;
                    lastWasRest = false;
                } else if (rUsed && pG < prof.restProb + prof.rProb + prof.kProb) {
                    if (prob(rng) < 0.4f) {
                        int off = offsetDist(rng);
                        gStr = "k+" + std::to_string(off);
                    } else if (prob(rng) < 0.2f) {
                        int off = offsetDist(rng);
                        gStr = "k-" + std::to_string(off);
                    } else {
                        gStr = "k";
                    }
                    lastWasRest = false;
                } else if (listUsed &&
                           pG < prof.restProb + prof.rProb + prof.kProb + prof.lProb) {
                    gStr = "l";
                    lastWasRest = false;
                } else if (pG < prof.restProb + prof.rProb + prof.kProb +
                                    prof.lProb + prof.listProb) {
                    int c1 = pickedGrades[rng() % pickedGrades.size()];
                    int c2 = pickedGrades[rng() % pickedGrades.size()];
                    if (c1 == c2) c2 = (c1 == 1 ? 5 : 1);
                    gStr = "<" + std::to_string(c1) + "," + std::to_string(c2) + ">";
                    listUsed = true;
                    lastWasRest = false;
                } else {
                    // Grado fijo: caminante de contorno. El primer grado fijo
                    // de la frase se ajusta al tono de acorde mas cercano.
                    if (!snappedToChord) {
                        contourCur = nearestChordTone(contourCur);
                        snappedToChord = true;
                    }
                    int iv = pickInterval(contourDir, prevIntv);
                    prevIntv = iv;
                    contourCur = std::max(cfg.gradeMin,
                              std::min(cfg.gradeMax, contourCur + iv));
                    gStr = std::to_string(contourCur);
                    lastWasRest = false;
                }

                // Duración
                if (lastWasRest) {
                    dStr = uniRhythm ? uniDur : prof.durations[durDist(rng)];
                } else if (pD < prof.rProb * 0.5f) {
                    dStr = "r";
                } else if (rUsed && pD < (prof.rProb * 0.5f) + prof.kProb * 0.5f) {
                    dStr = "k";
                } else if (pD < 0.25f && prof.durations.size() >= 2) {
                    int di1 = rng() % prof.durations.size();
                    int di2 = (di1 + 1) % prof.durations.size();
                    dStr = std::string("<") + prof.durations[di1] + "," +
                           prof.durations[di2] + ">";
                } else {
                    dStr = uniRhythm ? uniDur : prof.durations[durDist(rng)];
                }

                stepTokens.push_back(gStr + "," + dStr);
                bool canGlide = (!lastWasRest && s < numSteps - 1 &&
                                 prob(rng) < prof.glideProb);
                glideFlags.push_back(canGlide);
            }

            // Ensamblar pasos
            for (size_t s = 0; s < stepTokens.size(); s++) {
                line += stepTokens[s];
                if (s + 1 < stepTokens.size()) {
                    line += (glideFlags[s] ? "^" : " ");
                }
            }

            // Paso de enrutamiento de salida
            int targetMain = cycleNext[i];
            std::string exitStep;
            if (prob(rng) < prof.branchExitProb && i != targetMain) {
                int targetBranch = 0; // Atajo de vuelta a la tónica u otro tema
                if (targetBranch == targetMain)
                    targetBranch = order[(i + 3) % cfg.numFields];
                int gA = initiators[targetMain].grade;
                int gB = initiators[targetBranch].grade;
                if (gA != gB) {
                    exitStep = "<" + std::to_string(gA) + ":3," +
                               std::to_string(gB) + ":1>," +
                               initiators[targetMain].durText;
                } else {
                    exitStep = std::to_string(gA) + "," +
                               initiators[targetMain].durText;
                }
            } else {
                exitStep = std::to_string(initiators[targetMain].grade) + "," +
                           initiators[targetMain].durText;
            }

            if (!stepTokens.empty()) {
                line += (glideFlags.empty() || !glideFlags.back() ? " " : "^");
            }
            line += exitStep;

            // Repetición *N
            if (prob(rng) < prof.repeatProb) {
                int reps = (style == STYLE_AMBIENT || style == STYLE_ACID_TECHNO)
                    ? (prob(rng) < 0.5f ? 4 : 2) : 2;
                line += " *" + std::to_string(reps);
            }

            // Validar longitud y sintaxis
            if (line.size() <= (size_t)cfg.ruleFieldMaxChars) {
                lsys::RuleTable testTable;
                std::string err;
                if (lsys::parseRuleLine(line, testTable, err)) {
                    finalRule = line;
                    valid = true;
                }
            }
        }

        if (!valid) {
            int targetMain = cycleNext[i];
            finalRule = std::to_string(initiators[i].grade) + "," +
                        initiators[i].durText + " -> " +
                        std::to_string(initiators[targetMain].grade) + "," +
                        initiators[targetMain].durText;
        }

        rulesOut[i] = finalRule;
    }
}

// =======================================================================
// Generador Acid / Techno
// =======================================================================
// Groove, repetición, loops hipnóticos y finalización de loop con '=T'.
// Las reglas se encadenan entre sí mediante salidas ponderadas
// hogar/destino: pegajosas pero con escape garantizado.
inline void generateAcidRules(std::mt19937& rng,
                              const GeneratorConfig& cfg,
                              std::vector<std::string>& rulesOut,
                              std::string& gradePoolOut,
                              std::string& durationPoolOut) {
    rulesOut.assign(cfg.numFields, "");

    // Pools fijos, afinados para acid.
    gradePoolOut = "1:6,5:3,3:2,8:2";
    durationPoolOut = "1/4:8,1/2:2";

    // Detectar si la sintaxis de relleno '=T' está disponible.
    // Si no, usar duraciones fijas de salida.
    bool canUseFill = false;
    {
        lsys::RuleTable testTable;
        std::string err;
        canUseFill = lsys::parseRuleLine("1,1 -> 1,=1", testTable, err);
    }

    auto clampGrade = [&](int g) {
        return std::max(cfg.gradeMin, std::min(cfg.gradeMax, g));
    };

    // ---- Elegir grados acid, preferentemente distintos -------------
    std::vector<int> grades;
    std::vector<int> preferred = {
        1, 5, 3, 8, 2, 7, -1, 4, 6, -2, 9, 11, 10, 12
    };
    for (int g : preferred) {
        int cg = clampGrade(g);
        if (std::find(grades.begin(), grades.end(), cg) == grades.end()) {
            grades.push_back(cg);
        }
        if ((int)grades.size() == cfg.numFields) break;
    }
    // Si el rango de grados es muy pequeño, rellenar huecos.
    for (int g = cfg.gradeMin;
         g <= cfg.gradeMax && (int)grades.size() < cfg.numFields; g++) {
        if (std::find(grades.begin(), grades.end(), g) == grades.end()) {
            grades.push_back(g);
        }
    }
    while ((int)grades.size() < cfg.numFields) {
        grades.push_back(grades.empty() ? 1 : grades.back());
    }

    // ---- Columna vertebral de transiciones -------------------------
    std::vector<int> order(cfg.numFields);
    for (int i = 0; i < cfg.numFields; i++) order[i] = i;
    std::shuffle(order.begin() + 1, order.end(), rng);

    std::vector<int> cycleNext(cfg.numFields);
    for (int i = 0; i < cfg.numFields; i++) {
        cycleNext[order[i]] = order[(i + 1) % cfg.numFields];
    }

    // ---- Patrones hipnóticos cortos --------------------------------
    // H = grado "home" de esta regla. Cada patrón usa como mucho un 'r'.
    static const std::vector<std::vector<std::string>> patterns1 = {
        {"H", "H"},
        {"H", "H", "H"},
        {"H", "r"},
        {"H", "r", "k"},
        {"H", "r", "H"},
        {"H", "s", "r"}
    };

    static const std::vector<std::vector<std::string>> patterns2 = {
        {"H", "H", "H", "H"},
        {"H", "H", "r", "k"},
        {"H", "r", "k", "H"},
        {"H", "r", "k+1", "k"},
        {"H", "s", "r", "k"}
    };

    std::uniform_real_distribution<float> prob(0.f, 1.f);
    std::uniform_int_distribution<size_t> pick1(0, patterns1.size() - 1);
    std::uniform_int_distribution<size_t> pick2(0, patterns2.size() - 1);

    for (int i = 0; i < cfg.numFields; i++) {
        int home = grades[i];
        std::string homeStr = std::to_string(home);

        // La mayoría de las reglas acid quedan mejor en 1 beat.
        // Algunas a 2 beats añaden variación de frase.
        std::string fillTarget = (prob(rng) < 0.70f) ? "1" : "2";

        // Salida "pegajosa con escape": lista ponderada entre el propio hogar
        // y el siguiente del ciclo. Los grados acid son únicos por campo, así
        // que el grado identifica la regla destino sin ambigüedad aunque la
        // duración de relleno no coincida con el LHS (rescate por grado del
        // motor). Tónica 50/50; resto ~33% quedarse / ~67% avanzar.
        int fwdRule = cycleNext[i];
        int targetGrade = grades[fwdRule];
        int gSelf = home;
        bool twoWay = (gSelf != targetGrade);
        int wSelf = 1;
        int wFwd = (i == 0) ? 1 : 2;

        std::string finalRule;
        bool valid = false;

        for (int attempt = 0; attempt < 20 && !valid; attempt++) {
            std::vector<std::string> pattern =
                (fillTarget == "1") ? patterns1[pick1(rng)] : patterns2[pick2(rng)];

            std::string line = homeStr + ",1/4 -> ";

            for (size_t s = 0; s < pattern.size(); s++) {
                std::string tok = pattern[s];
                if (tok == "H") tok = homeStr;

                bool curIsRest = (pattern[s] == "s");
                bool nextIsRest = (s + 1 < pattern.size() && pattern[s + 1] == "s");

                // Casi siempre 1/4. Para loops de 2 beats, a veces 1/2.
                std::string dur = "1/4";
                if (fillTarget == "2" && !curIsRest && prob(rng) < 0.18f) {
                    dur = "1/2";
                }

                line += tok + "," + dur;

                if (s + 1 < pattern.size()) {
                    bool glide = !curIsRest && !nextIsRest && prob(rng) < 0.35f;
                    line += glide ? "^" : " ";
                } else {
                    // Glide opcional hacia el paso de salida/relleno.
                    bool glideToExit = !curIsRest && prob(rng) < 0.25f;
                    line += glideToExit ? "^" : " ";
                }
            }

            // Paso de salida.
            // Si '=T' está disponible, completa el loop y enruta por grado
            // (lista ponderada hogar/destino); si no, duración fija 1/4.
            std::string exitGrades = twoWay
                ? "<" + homeStr + ":" + std::to_string(wSelf) + "," +
                  std::to_string(targetGrade) + ":" + std::to_string(wFwd) + ">"
                : homeStr;
            std::string exitStep;
            if (canUseFill) {
                exitStep = exitGrades + ",=" + fillTarget;
            } else {
                exitStep = exitGrades + ",1/4";
            }
            line += exitStep;

            // Repeticiones: largas para el groove, pero sin enterrar la
            // progresión del ciclo.
            int reps;
            float pr = prob(rng);
            if (fillTarget == "1") reps = (pr < 0.30f) ? 4 : 8;
            else reps = (pr < 0.50f) ? 4 : 8;
            line += " *" + std::to_string(reps);

            if (line.size() <= (size_t)cfg.ruleFieldMaxChars) {
                lsys::RuleTable testTable;
                std::string err;
                if (lsys::parseRuleLine(line, testTable, err)) {
                    finalRule = line;
                    valid = true;
                }
            }
        }

        // Respaldo simple.
        if (!valid) {
            std::string line = homeStr + ",1/4 -> " + homeStr + ",1/4 r,1/4 ";
            if (canUseFill) {
                line += std::to_string(targetGrade) + ",=" + fillTarget;
            } else {
                line += std::to_string(targetGrade) + ",1/4";
            }
            line += " *8";
            finalRule = line;
        }

        rulesOut[i] = finalRule;
    }
}

// =======================================================================
// Orquestador principal
// =======================================================================
// Punto de entrada único para el módulo: resuelve el seed, genera pools
// y reglas según el estilo, y devuelve todo en un GeneratedRuleSet.
inline GeneratedRuleSet generateAll(GenStyle style,
                                    std::string& seedText,
                                    const GeneratorConfig& cfg) {
    GeneratedRuleSet result;
    uint32_t seed = resolveSeed(seedText);
    std::mt19937 rng(seed);

    if (style == STYLE_ACID_TECHNO) {
        generateAcidRules(rng, cfg, result.rules,
                          result.gradePool, result.durationPool);
    } else {
        generatePools(rng, style, cfg, result.gradePool, result.durationPool);
        generateRules(rng, style, cfg, result.rules);
    }

    return result;
}

} // namespace lgen