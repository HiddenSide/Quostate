// LSystemExpander.cpp
// LS-Exp: expander for LSystem (scale/root I/O, degree/step outputs, display).
#include "plugin.hpp"
#include "LSystemExpander.hpp"
#include "MusicUtils.hpp"
#include "components.hpp"
#include <mutex>
#include <cmath>
#include <cstring>
#include <algorithm>

using namespace lsys;

struct LSExpanderModule : Module {
    enum InputIds { SCALE_INPUT, ROOT_INPUT, NUM_INPUTS };
    enum OutputIds { STEP_OUTPUT, DEGREE_OUTPUT, SCALE_OUTPUT, ROOT_OUTPUT, NUM_OUTPUTS };
    enum ParamIds { CHANNEL_PARAM, NUM_PARAMS };
    enum LightIds { NUM_LIGHTS };

    static constexpr int MAX_LIBRE_CHANS = 16;

    int scaleModeIn = lsxp::MODE_STD_7CH;
    int scaleModeOut = lsxp::MODE_STD_7CH;
    int displayChannel = 0; // 0..5, shown as 1..6
    bool stepWholeOpt = false; // false: per-repetition (default); true: include *N repetitions
    bool romanOpt = false;
    int rootFormatOut = 0;   // 0: raw V/oct (default); 1: circle-of-fifths (Meander "Root")
    int degreeOctOffset = 0; // -2..+2 octaves added to the DEGREE output's octave field
    dsp::SchmittTrigger channelTrigger;

    // Meander "Root" mapping: pitch class (semitone 0=C) -> circle-of-fifths
    // voltage (the values Meander's Root CV input expects).
    static constexpr float fifthsRootVoltage[12] = {0.5f, 7.0f, 2.0f, 9.0f, 4.0f, 10.0f,
                                                    6.0f, 1.0f, 8.0f, 3.0f, 9.5f, 5.0f};

    // CPU optimization: cache the host pointer and only re-detect it at a low
    // rate, and throttle the display-string rebuilding (outputs run per-frame).
    Module* cachedHost = nullptr;
    int frameCount = 0;

    // Owned message buffers for LSystem -> Expander traffic (push pattern).
    // Allocated here, written by the LSystem into producer, read from consumer.
    // Expander -> LSystem traffic goes into the LSystem's own buffers.
    lsxp::LSystemToExpander cached; // last payload read (for display/outputs)
    bool linked = false;
    std::string scaleName = "--";
    std::string rootName = "--";
    std::string degreeText = "--";
    std::string noteText = "--";
    std::string ruleText = "--";

    LSExpanderModule() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(CHANNEL_PARAM, 0.f, 1.f, 0.f, "Display channel");
        configInput(SCALE_INPUT, "Scale");
        configInput(ROOT_INPUT, "Root");
        configOutput(SCALE_OUTPUT, "Scale");
        configOutput(ROOT_OUTPUT, "Root");
        configOutput(DEGREE_OUTPUT, "Degree (degree.octave)");
        configOutput(STEP_OUTPUT, "Step (0-10V)");

        leftExpander.producerMessage = new lsxp::LSystemToExpander;
        leftExpander.consumerMessage = new lsxp::LSystemToExpander;
        rightExpander.producerMessage = new lsxp::LSystemToExpander;
        rightExpander.consumerMessage = new lsxp::LSystemToExpander;
    }

    ~LSExpanderModule() {
        delete (lsxp::LSystemToExpander*)leftExpander.producerMessage;
        delete (lsxp::LSystemToExpander*)leftExpander.consumerMessage;
        delete (lsxp::LSystemToExpander*)rightExpander.producerMessage;
        delete (lsxp::LSystemToExpander*)rightExpander.consumerMessage;
        leftExpander.producerMessage = leftExpander.consumerMessage = nullptr;
        rightExpander.producerMessage = rightExpander.consumerMessage = nullptr;
    }

    // Self-detection: connect to the left LSystem first (we are its
    // rightExpander, which has priority), else to the right one.
    Module* findHost() {
        // Only re-scan periodically instead of every audio frame.
        if (frameCount % 64 != 0)
            return cachedHost;
        if (leftExpander.module && leftExpander.module->model &&
            leftExpander.module->model->slug == "LSystem")
            return cachedHost = leftExpander.module;
        if (rightExpander.module && rightExpander.module->model &&
            rightExpander.module->model->slug == "LSystem")
            return cachedHost = rightExpander.module;
        return cachedHost = nullptr;
    }

    static std::string noteName(float voct) {
        auto& names = getNoteNames();
        int semi = (int)std::round(voct * 12.f);
        return names[floorMod(semi, 12)];
    }

    static std::string noteNameCents(float voct) {
        auto& names = getNoteNames();
        float semiF = voct * 12.f;
        int semi = (int)std::round(semiF);
        int cents = (int)std::round((semiF - (float)semi) * 100.f);
        std::string s = names[floorMod(semi, 12)];
        if (cents != 0) s += (cents > 0 ? " +" : " ") + std::to_string(cents);
        return s;
    }

    void process(const ProcessArgs& args) override {
        frameCount++;

        Module* host = findHost();
        // Read host payload from OUR consumer buffer on the side facing it.
        lsxp::LSystemToExpander* msg = nullptr;
        if (host) {
            if (host == leftExpander.module) msg = (lsxp::LSystemToExpander*)leftExpander.consumerMessage;
            else if (host == rightExpander.module) msg = (lsxp::LSystemToExpander*)rightExpander.consumerMessage;
        }
        linked = host && msg && msg->active;
        if (linked) cached = *msg;

        // ---- Push our inputs to the host (Expander -> LSystem) ----
        if (host) {
            // Zero-init so padding bytes are stable: the host compares the
            // struct with memcmp to detect real changes.
            lsxp::ExpanderToLSystem out = {};
            out.active = true;
            out.scaleMode = scaleModeIn;
            if (scaleModeIn == lsxp::MODE_LSYSTEM_INDEX) {
                // LSystem Index: SCALE_IN is mono; ch0 carries the scale index
                // (0-10V).
                if (inputs[SCALE_INPUT].isConnected()) {
                    out.scaleConnected = true;
                    out.scaleChans = 1;
                    out.scaleVoct[0] = inputs[SCALE_INPUT].getVoltage(0);
                }
            } else if (inputs[SCALE_INPUT].isConnected()) {
                int nc = inputs[SCALE_INPUT].getChannels();
                if (nc > MAX_LIBRE_CHANS) nc = MAX_LIBRE_CHANS;
                out.scaleConnected = true;
                out.scaleChans = nc;
                for (int i = 0; i < nc; i++) out.scaleVoct[i] = inputs[SCALE_INPUT].getVoltage(i);
            }
            if (inputs[ROOT_INPUT].isConnected()) {
                out.rootConnected = true;
                out.rootVoct = inputs[ROOT_INPUT].getVoltage(0);
            }
            // Write into the host buffer facing us, then request flip.
            if (host == leftExpander.module && host->rightExpander.producerMessage) {
                std::memcpy(host->rightExpander.producerMessage, &out, sizeof(out));
                host->rightExpander.messageFlipRequested = true;
            } else if (host == rightExpander.module && host->leftExpander.producerMessage) {
                std::memcpy(host->leftExpander.producerMessage, &out, sizeof(out));
                host->leftExpander.messageFlipRequested = true;
            }
        }

        if (!linked) {
            outputs[SCALE_OUTPUT].setChannels(1);
            outputs[SCALE_OUTPUT].setVoltage(0.f);
            outputs[ROOT_OUTPUT].setChannels(1);
            outputs[ROOT_OUTPUT].setVoltage(0.f);
            outputs[DEGREE_OUTPUT].setChannels(1);
            outputs[DEGREE_OUTPUT].setVoltage(0.f);
            outputs[STEP_OUTPUT].setChannels(1);
            outputs[STEP_OUTPUT].setVoltage(0.f);
            scaleName = "not connected";
            rootName = degreeText = noteText = ruleText = "--";
            return;
        }

        int nCh = std::max(1, std::min(6, cached.numChannels));
        int sLen = std::max(1, cached.scaleLen);

        // Display channel button cycles only among the active channels.
        if (channelTrigger.process(params[CHANNEL_PARAM].getValue())) {
            displayChannel = (displayChannel + 1) % nCh;
        }
        if (displayChannel >= nCh) displayChannel = nCh - 1;

        // ---- DEGREE_OUT (poly, Meander-style degree.octave, silence = 0V) ----
        outputs[DEGREE_OUTPUT].setChannels(nCh);
        for (int ch = 0; ch < nCh; ch++) {
            float v = 0.f;
            if (!cached.isRest[ch] && !cached.isSilent[ch]) {
                lsxp::DegreeResult r = lsxp::encodeDegree(cached.absDegree[ch], sLen);
                // Optional octave offset: shift the packed octave field while
                // keeping the degree.octave encoding (clamped to Meander's 0-7).
                int oct = std::max(0, std::min(7, r.relativeOctave + degreeOctOffset));
                v = (float)r.degreeInScale + (float)oct * 0.1f;
            }
            outputs[DEGREE_OUTPUT].setVoltage(v, ch);
        }

        // ---- STEP_OUT (poly) ----
        outputs[STEP_OUTPUT].setChannels(nCh);
        for (int ch = 0; ch < nCh; ch++) {
            float v = stepWholeOpt
                ? lsxp::stepVoltage(cached.stepIdxWhole[ch], cached.stepTotalWhole[ch])
                : lsxp::stepVoltage(cached.stepIdx[ch], cached.stepTotal[ch]);
            outputs[STEP_OUTPUT].setVoltage(v, ch);
        }

        // ---- SCALE_OUT (mode-encoded) + ROOT_OUT ----
        // Nominal channel counts per mode; missing tones read 0.0V.
        int nominal = 7;
        switch (scaleModeOut) {
            case lsxp::MODE_CHROMATIC_12CH: nominal = 12; break;
            case lsxp::MODE_STD_7CH: nominal = 7; break;
            case lsxp::MODE_PENTA_5CH: nominal = 5; break;
            case lsxp::MODE_PENTA_CHROM_12CH: nominal = 12; break;
            case lsxp::MODE_LIBRE: nominal = std::max(1, cached.scaleVoctChans); break;
            default: nominal = 7; break;
        }
        if (scaleModeOut == lsxp::MODE_LSYSTEM_INDEX) {
            // LSystem Index: emit the LSystem's internal scale index mapped to
            // 0-10V, regardless of any external scale currently sounding.
            outputs[SCALE_OUTPUT].setChannels(1);
            int nPresets = (int)getScalePresets().size();
            int idx = std::max(0, std::min(nPresets - 1, cached.scaleIndex));
            float v = nPresets > 1 ? (float)idx * 10.f / (float)(nPresets - 1) : 0.f;
            outputs[SCALE_OUTPUT].setVoltage(v, 0);
        } else if (scaleModeOut == lsxp::MODE_CHROMATIC_12CH || scaleModeOut == lsxp::MODE_PENTA_CHROM_12CH) {
            outputs[SCALE_OUTPUT].setChannels(nominal);
            // Categorical 12ch: mark active semitones (8V) and root (10V).
            bool activeSemi[12] = {};
            int want = (scaleModeOut == lsxp::MODE_CHROMATIC_12CH) ? 12 : 5;
            // Derive the active set from the payload tones.
            int count = 0;
            for (int i = 0; i < cached.scaleVoctChans && count < want; i++) {
                int s = floorMod((int)std::round((cached.scaleVoct[i] - cached.rootVoct) * 12.f), 12);
                if (!activeSemi[s]) { activeSemi[s] = true; count++; }
            }
            for (int i = 0; i < nominal; i++) {
                float v = 0.f;
                if (activeSemi[i]) v = (i == 0) ? 10.f : 8.f;
                outputs[SCALE_OUTPUT].setVoltage(v, i);
            }
        } else if (scaleModeOut == lsxp::MODE_PENTA_5CH) {
            outputs[SCALE_OUTPUT].setChannels(nominal);
            // Pentatonic subset: drop 4th/7th (indices 3,6) of a heptatonic
            // scale; otherwise take the first 5 tones. Fixed stack arrays to
            // avoid per-frame heap allocations.
            float penta[5];
            int pentaN = 0;
            int tonesN = cached.scaleVoctChans;
            const float* tones = cached.scaleVoct;
            if (tonesN == 7) {
                for (int i = 0; i < 7; i++)
                    if (i != 3 && i != 6) penta[pentaN++] = tones[i];
            } else {
                for (int i = 0; i < tonesN && pentaN < 5; i++)
                    penta[pentaN++] = tones[i];
            }
            for (int i = 0; i < nominal; i++)
                outputs[SCALE_OUTPUT].setVoltage(i < pentaN ? penta[i] : 0.f, i);
        } else {
            outputs[SCALE_OUTPUT].setChannels(nominal);
            for (int i = 0; i < nominal; i++)
                outputs[SCALE_OUTPUT].setVoltage(i < cached.scaleVoctChans ? cached.scaleVoct[i] : 0.f, i);
        }
        outputs[ROOT_OUTPUT].setChannels(1);
        if (rootFormatOut == 1) {
            // Circle-of-fifths (Meander Root): map the current root's pitch
            // class to the fifths voltage, ignoring register (the format has
            // no octave information).
            int semi = floorMod((int)std::lround(cached.rootVoct * 12.f), 12);
            outputs[ROOT_OUTPUT].setVoltage(fifthsRootVoltage[semi], 0);
        } else {
            outputs[ROOT_OUTPUT].setVoltage(cached.rootVoct, 0);
        }

        // ---- Display state for the selected channel ----
        // Rebuild these display strings at a low rate; they're only read by the
        // NVG widgets on the UI thread and don't need per-frame updates.
        if (frameCount % 64 == 0) {
            int ch = std::min(displayChannel, nCh - 1);
            scaleName = cached.externalScale ? "External"
                : getScalePresets()[std::max(0, std::min((int)getScalePresets().size() - 1, cached.scaleIndex))].name;
            rootName = noteName(cached.rootVoct);
            // DEGREE and NOTE are separate readouts (own boxes on the panel).
            if (cached.isRest[ch] || cached.isSilent[ch]) {
                degreeText = "s";
                noteText = "--";
            } else {
                int zb = cached.absDegree[ch] - 1;
                int degIn = floorMod(zb, sLen) + 1;
                std::string deg;
                if (romanOpt && sLen <= 7) {
                    static const char* roman[] = {"I", "II", "III", "IV", "V", "VI", "VII"};
                    deg = roman[std::max(0, std::min(sLen - 1, degIn - 1))];
                } else {
                    deg = std::to_string(cached.absDegree[ch]);
                }
                // Pitch from payload tones (root + ascending intervals, 1V cycle).
                float pitch = cached.rootVoct;
                if (cached.scaleVoctChans > 0) {
                    int d = floorDiv(zb, sLen);
                    int m = floorMod(zb, sLen);
                    int ti = std::min(m, cached.scaleVoctChans - 1);
                    pitch = cached.scaleVoct[ti] + (float)d * 1.f;
                }
                bool showCents = (scaleModeIn == lsxp::MODE_STD_7CH || scaleModeIn == lsxp::MODE_PENTA_5CH ||
                                  scaleModeIn == lsxp::MODE_LIBRE || cached.externalScale);
                noteText = showCents ? noteNameCents(pitch) : noteName(pitch);
                degreeText = deg;
            }
            ruleText = std::to_string(std::max(0, cached.ruleIdx[ch]) + 1);
        }
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "scaleModeIn", json_integer(scaleModeIn));
        json_object_set_new(rootJ, "scaleModeOut", json_integer(scaleModeOut));
        json_object_set_new(rootJ, "displayChannel", json_integer(displayChannel));
        json_object_set_new(rootJ, "stepWhole", json_boolean(stepWholeOpt));
        json_object_set_new(rootJ, "roman", json_boolean(romanOpt));
        json_object_set_new(rootJ, "rootFormatOut", json_integer(rootFormatOut));
        json_object_set_new(rootJ, "degreeOctOffset", json_integer(degreeOctOffset));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        auto clampMode = [](int v) { return std::max(0, std::min(lsxp::NUM_SCALE_MODES - 1, v)); };
        json_t* mInJ = json_object_get(rootJ, "scaleModeIn");
        json_t* mOutJ = json_object_get(rootJ, "scaleModeOut");
        if (mInJ && mOutJ) {
            scaleModeIn = clampMode((int)json_integer_value(mInJ));
            scaleModeOut = clampMode((int)json_integer_value(mOutJ));
        } else {
            // Migration from older patches that had a single "scaleMode".
            json_t* oldJ = json_object_get(rootJ, "scaleMode");
            if (oldJ) {
                int m = clampMode((int)json_integer_value(oldJ));
                scaleModeIn = scaleModeOut = m;
            }
        }
        json_t* cJ = json_object_get(rootJ, "displayChannel");
        if (cJ) displayChannel = std::max(0, std::min(5, (int)json_integer_value(cJ)));
        json_t* sJ = json_object_get(rootJ, "stepWhole");
        if (sJ) stepWholeOpt = json_is_true(sJ);
        json_t* rJ = json_object_get(rootJ, "roman");
        if (rJ) romanOpt = json_is_true(rJ);
        json_t* rfJ = json_object_get(rootJ, "rootFormatOut");
        if (rfJ) rootFormatOut = std::max(0, std::min(1, (int)json_integer_value(rfJ)));
        json_t* doJ = json_object_get(rootJ, "degreeOctOffset");
        if (doJ) degreeOctOffset = std::max(-2, std::min(2, (int)json_integer_value(doJ)));
    }
};

// Out-of-class definition of the static constexpr member (C++11 requires it
// for odr-use such as array indexing).
constexpr float LSExpanderModule::fifthsRootVoltage[12];

// ---------------------------------------------------------------------
// Display text widgets (hd44780 font, sizes/boxes from the spec).
// ---------------------------------------------------------------------
inline std::shared_ptr<window::Font> getExpFont() {
    static std::shared_ptr<window::Font> font;
    if (!font) font = APP->window->loadFont(asset::plugin(pluginInstance, "res/hd44780.otf"));
    return font;
}

struct ExpText : TransparentWidget {
    LSExpanderModule* module = nullptr;
    std::string LSExpanderModule::* field = nullptr;
    bool center = false;
    bool twoLine = false;
    float fontMm = 2.1167f; // ~6pt
    void draw(const DrawArgs& args) override {
        std::string t = (module && field) ? (module->*field) : "--";
        auto font = getExpFont();
        if (!font || font->handle < 0) return;
        nvgFontFaceId(args.vg, font->handle);
        nvgFontSize(args.vg, mm2px(fontMm));
        nvgFillColor(args.vg, nvgRGB(0xe8, 0xcc, 0x40));
        if (!twoLine) {
            nvgTextAlign(args.vg, (center ? NVG_ALIGN_CENTER : NVG_ALIGN_LEFT) | NVG_ALIGN_MIDDLE);
            float x = center ? box.size.x * 0.5f : 0.f;
            nvgText(args.vg, x, box.size.y * 0.5f, t.c_str(), NULL);
        } else {
            // Scale name: up to 2 centered lines, first two words on top.
            std::string l1 = t, l2;
            {
                std::vector<std::string> w;
                std::string cur;
                for (char c : t) {
                    if (c == ' ') { if (!cur.empty()) { w.push_back(cur); cur.clear(); } }
                    else cur.push_back(c);
                }
                if (!cur.empty()) w.push_back(cur);
                // "Octatonic (Half-whole)" overflows a single line; force it
                // onto two lines.
                if (w.size() >= 2 && w[0] == "Octatonic" && w[1].find("(Half-whole)") != std::string::npos) {
                    l1 = w[0];
                    l2.assign(t, l1.size(), std::string::npos);
                    if (!l2.empty() && l2[0] == ' ') l2.erase(0, 1);
                } else if (w.size() > 2) {
                    l1 = w[0] + " " + w[1];
                    l2.clear();
                    for (size_t i = 2; i < w.size(); i++) {
                        if (i > 2) l2 += " ";
                        l2 += w[i];
                    }
                }
            }
            nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            if (l2.empty()) {
                nvgText(args.vg, box.size.x * 0.5f, box.size.y * 0.5f, l1.c_str(), NULL);
            } else {
                nvgText(args.vg, box.size.x * 0.5f, box.size.y * 0.25f, l1.c_str(), NULL);
                nvgText(args.vg, box.size.x * 0.5f, box.size.y * 0.75f, l2.c_str(), NULL);
            }
        }
    }
};

struct ExpChannelText : TransparentWidget {
    LSExpanderModule* module = nullptr;
    void draw(const DrawArgs& args) override {
        auto font = getExpFont();
        if (!font || font->handle < 0) return;
        nvgFontFaceId(args.vg, font->handle);
        nvgFontSize(args.vg, mm2px(2.1167f));
        nvgFillColor(args.vg, nvgRGB(0xe8, 0xcc, 0x40));
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        std::string t = module ? std::to_string(module->displayChannel + 1) : "1";
        nvgText(args.vg, 0.f, box.size.y * 0.5f, t.c_str(), NULL);
    }
};

// Static display title labels (SCALE/ROOT/DEGREE/RULE/CHN). Constant strings,
// rendered with the same hd44780 font as the dynamic values.
struct ExpLabel : TransparentWidget {
    std::string text;
    float fontMm = 2.1167f; // 6pt
    void draw(const DrawArgs& args) override {
        auto font = getExpFont();
        if (!font || font->handle < 0) return;
        nvgFontFaceId(args.vg, font->handle);
        nvgFontSize(args.vg, mm2px(fontMm));
        nvgFillColor(args.vg, nvgRGB(0xd0, 0xd0, 0xd0));
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgText(args.vg, 0.f, box.size.y * 0.5f, text.c_str(), NULL);
    }
};

struct LSExpanderWidget : ModuleWidget {
    LSExpanderWidget(LSExpanderModule* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/LSystemExpander.svg")));

        addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Jacks (center positions in mm, from the spec).
        addInput(createInputCentered<QuoJack>(mm2px(Vec(14.2052f, 80.0552f)), module, LSExpanderModule::SCALE_INPUT));
        addInput(createInputCentered<QuoJack>(mm2px(Vec(26.4347f, 80.0553f)), module, LSExpanderModule::ROOT_INPUT));
        addOutput(createOutputCentered<QuoJack>(mm2px(Vec(14.2052f, 95.5140f)), module, LSExpanderModule::STEP_OUTPUT));
        addOutput(createOutputCentered<QuoJack>(mm2px(Vec(26.4347f, 95.5140f)), module, LSExpanderModule::DEGREE_OUTPUT));
        addOutput(createOutputCentered<QuoJack>(mm2px(Vec(14.2053f, 110.8395f)), module, LSExpanderModule::SCALE_OUTPUT));
        addOutput(createOutputCentered<QuoJack>(mm2px(Vec(26.4348f, 110.8395f)), module, LSExpanderModule::ROOT_OUTPUT));

        // Channel button.
        addParam(createParamCentered<QuoButton>(mm2px(Vec(30.0739f, 68.0071f)), module, LSExpanderModule::CHANNEL_PARAM));

        auto addText = [&](float x, float y, float w, float h,
                           std::string LSExpanderModule::* f, bool center, bool twoLine, float fmm) {
            ExpText* t = new ExpText;
            t->box.pos = mm2px(Vec(x, y));
            t->box.size = mm2px(Vec(w, h));
            t->module = module;
            t->field = f;
            t->center = center;
            t->twoLine = twoLine;
            t->fontMm = fmm;
            addChild(t);
        };
        // Dynamic value boxes (origin + size in mm, from the spec).
        addText(9.2260f, 30.4298f, 22.6748f, 3.9688f, &LSExpanderModule::scaleName, true, true, 1.7639f);
        addText(23.0482f, 37.2714f, 8.8032f, 2.1167f, &LSExpanderModule::rootName, false, false, 2.1167f);
        addText(23.0482f, 41.7657f, 8.8032f, 2.1167f, &LSExpanderModule::degreeText, false, false, 2.1167f);
        addText(23.0482f, 46.2600f, 8.8032f, 2.1167f, &LSExpanderModule::ruleText, false, false, 2.1167f);
        addText(23.0482f, 50.7543f, 8.8032f, 2.1167f, &LSExpanderModule::noteText, false, false, 2.1167f);

        // Static display title labels (positions/sizes from the spec).
        auto addLabel = [&](float x, float y, float w, float h, const char* txt) {
            ExpLabel* l = new ExpLabel;
            l->box.pos = mm2px(Vec(x, y));
            l->box.size = mm2px(Vec(w, h));
            l->text = txt;
            addChild(l);
        };
        addLabel(8.5106f, 26.3109f, 12.7468f, 2.1167f, "SCALE");
        addLabel(8.5106f, 37.2714f, 12.7468f, 2.1167f, "ROOT");
        addLabel(8.5106f, 41.7657f, 12.7468f, 2.1167f, "DEGREE");
        addLabel(8.5106f, 46.2600f, 12.7468f, 2.1167f, "RULE");
        addLabel(8.5106f, 50.7543f, 12.7468f, 2.1167f, "NOTE");
        addLabel(20.6110f, 57.0726f, 7.4160f, 2.1167f, "CHN");

        ExpChannelText* ct = new ExpChannelText;
        ct->box.pos = mm2px(Vec(28.3750f, 57.0726f));
        ct->box.size = mm2px(Vec(3.4764f, 2.1167f));
        ct->module = module;
        addChild(ct);
    }

    void appendContextMenu(Menu* menu) override {
        LSExpanderModule* m = dynamic_cast<LSExpanderModule*>(this->module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("LS-Exp"));
        // Build a scale-format submenu (IN or OUT). Binds to the given module field.
        auto addFormat = [&](const char* title, int LSExpanderModule::* field) {
            menu->addChild(createSubmenuItem(title, lsxp::scaleModeName(m->*field), [=](Menu* menu) {
                static const struct { const char* name; int mode; } modes[] = {
                    {"Heptatonic Chromatic-12ch", lsxp::MODE_CHROMATIC_12CH},
                    {"Heptatonic Diatonic STD-7ch", lsxp::MODE_STD_7CH},
                    {"Pentatonic-5ch", lsxp::MODE_PENTA_5CH},
                    {"Pentatonic Chromatic-12ch", lsxp::MODE_PENTA_CHROM_12CH},
                    {"Free / Microtonal", lsxp::MODE_LIBRE},
                    {"LSystem Index", lsxp::MODE_LSYSTEM_INDEX},
                };
                for (auto& md : modes) {
                    menu->addChild(createCheckMenuItem(md.name, "",
                        [=]() { return m->*field == md.mode; },
                        [=]() { m->*field = md.mode; }));
                }
            }));
        };
        addFormat("Scale format IN", &LSExpanderModule::scaleModeIn);
        addFormat("Scale format OUT", &LSExpanderModule::scaleModeOut);
        menu->addChild(createBoolPtrMenuItem("Step includes repetitions", "", &m->stepWholeOpt));
        menu->addChild(createBoolPtrMenuItem("Roman numerals", "", &m->romanOpt));

        // Root format OUT: raw V/oct or circle-of-fifths (Meander Root).
        menu->addChild(createSubmenuItem("Root format OUT", m->rootFormatOut == 1 ? "Circle of fifths (Meander)" : "V/oct (raw)", [=](Menu* menu) {
            menu->addChild(createCheckMenuItem("V/oct (raw)", "",
                [=]() { return m->rootFormatOut == 0; },
                [=]() { m->rootFormatOut = 0; }));
            menu->addChild(createCheckMenuItem("Circle of fifths (Meander)", "",
                [=]() { return m->rootFormatOut == 1; },
                [=]() { m->rootFormatOut = 1; }));
        }));

        // Degree octave offset (-2..+2).
        const char* octLabels[] = {"-2", "-1", "0 (default)", "+1", "+2"};
        menu->addChild(createSubmenuItem("Degree octave offset", octLabels[m->degreeOctOffset + 2], [=](Menu* menu) {
            for (int o = -2; o <= 2; o++) {
                menu->addChild(createCheckMenuItem(octLabels[o + 2], "",
                    [=]() { return m->degreeOctOffset == o; },
                    [=]() { m->degreeOctOffset = o; }));
            }
        }));
    }
};

Model* modelLSExp = createModel<LSExpanderModule, LSExpanderWidget>("LS-Exp");
