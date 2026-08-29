#include "plugin.hpp"

#include <cmath>


static const int NUM_CURVES = 4;
static const int NUM_DELAYS = 7;
static const int DELAY_VALUES[NUM_DELAYS] = {0, 1, 2, 4, 8, 16, 32};
static const char* DELAY_LABELS[NUM_DELAYS] = {"OFF", "01", "02", "04", "08", "16", "32"};


struct MorphfastMini : Module {
	enum ParamIds {
		INIT_PARAM,
		TARGET_PARAM,
		DURATION_PARAM,
		FIRE_PARAM,
		SWAP_PARAM,
		CURVE_PARAM,
		DELAY_PARAM,
		EASE_IN_PARAM,
		EASE_OUT_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		CLOCK_INPUT,
		RESET_INPUT,
		FIRE_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		EOT_OUTPUT,
		MAIN_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		ACTIVE_LIGHT,
		NUM_LIGHTS
	};

	enum Curves {
		RAMP,
		RAMP_BACK,
		STEP,
		TRIANGLE
	};
	enum State {
		ST_IDLE,
		ST_ARMED,
		ST_DELAYING,
		ST_TRANSITION
	};

	State state = ST_IDLE;

	dsp::SchmittTrigger clockTrigger;
	dsp::SchmittTrigger fireInputTrigger;
	dsp::SchmittTrigger fireButtonTrigger;
	dsp::SchmittTrigger resetTrigger;
	dsp::SchmittTrigger swapTrigger;
	dsp::SchmittTrigger curveTrigger;
	dsp::SchmittTrigger delayTrigger;

	int curveIndex = 0;
	int delayIndex = 0;

	bool haveLastEdge = false;
	float samplesSinceEdge = 0.f;
	float lastPeriod = 0.5f;
	float snapPeriod = 0.5f;

	int completedPeriods = 0;
	int transSteps = 8;
	int delayRemaining = 0;

	float snapInit = 0.f;
	float snapTarget = 0.f;
	float easeC1 = 1.f;
	float easeC2 = 0.f;

	float outVoltage = 0.f;
	float eotRemaining = 0.f;
	static constexpr float EOT_DURATION = 10e-3f;

	int frameDivider = 0;

	// Same-step chaining: a fire arriving shortly after a clock edge is
	// treated as coincident with that edge (tolerates cable latency).
	bool sameStepChain = true;

	float catchupWindow() const {
		return std::min(0.0005f, 0.25f * lastPeriod);
	}

	MorphfastMini() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configParam(INIT_PARAM, -10.f, 10.f, 0.f, "Init", " V");
		configParam(TARGET_PARAM, -10.f, 10.f, 0.f, "Target", " V");
		configParam(DURATION_PARAM, 1.f, 64.f, 8.f, "Duration", " pulses");
		paramQuantities[DURATION_PARAM]->snapEnabled = true;
		configButton(FIRE_PARAM, "Fire");
		configButton(SWAP_PARAM, "Swap Init/Target");
		configButton(CURVE_PARAM, "Curve");
		configButton(DELAY_PARAM, "Delay");
		configParam(EASE_IN_PARAM, 0.f, 1.f, 0.f, "Ease in", " %", 0.f, 100.f);
		configParam(EASE_OUT_PARAM, 0.f, 1.f, 0.f, "Ease out", " %", 0.f, 100.f);
		configInput(CLOCK_INPUT, "Clock");
		configInput(RESET_INPUT, "Reset");
		configInput(FIRE_INPUT, "Fire");
		configOutput(EOT_OUTPUT, "End of transition");
		configOutput(MAIN_OUTPUT, "Transition voltage");
		configLight(ACTIVE_LIGHT, "Transition active");

		outVoltage = params[INIT_PARAM].getValue();
	}

	float shape(float t) const {
		return t * (easeC1 + easeC2 * t);
	}

	// Triangle: reaches Target at half the transition, then returns to Init.
	// Ease in softens departures (from Init and from Target), ease out softens
	// arrivals (into Target and into Init), applied symmetrically per half.
	float shapedProgress(float t) const {
		if (curveIndex != TRIANGLE)
			return shape(t);
		return (t < 0.5f) ? shape(2.f * t) : 1.f - shape(2.f * t - 1.f);
	}

	void startTransition() {
		state = ST_TRANSITION;
		completedPeriods = 0;
		snapPeriod = lastPeriod;
		if (snapPeriod <= 0.f)
			snapPeriod = 0.5f;
		int steps = (int) std::round(params[DURATION_PARAM].getValue());
		steps = math::clamp(steps, 1, 64);
		steps = std::min(steps, 64 - DELAY_VALUES[delayIndex]);
		transSteps = steps;
		snapInit = params[INIT_PARAM].getValue();
		snapTarget = params[TARGET_PARAM].getValue();
		float a = params[EASE_IN_PARAM].getValue();
		float b = params[EASE_OUT_PARAM].getValue();
		easeC1 = 1.f - a + b;
		easeC2 = a - b;
		if (curveIndex == STEP)
			outVoltage = snapInit + (snapTarget - snapInit) * shape(0.f);
	}

	void finishTransition() {
		state = ST_IDLE;
		outVoltage = (curveIndex == RAMP_BACK || curveIndex == TRIANGLE) ? snapInit : snapTarget;
		eotRemaining = EOT_DURATION;
	}

	void resetAll() {
		state = ST_IDLE;
		delayRemaining = 0;
		eotRemaining = 0.f;
		outVoltage = params[INIT_PARAM].getValue();
	}

	void process(const ProcessArgs& args) override {
		const float sampleTime = args.sampleTime;

		// Cyclic buttons and swap: act on press only
		if (swapTrigger.process(params[SWAP_PARAM].getValue())) {
			float v = params[INIT_PARAM].getValue();
			params[INIT_PARAM].setValue(params[TARGET_PARAM].getValue());
			params[TARGET_PARAM].setValue(v);
		}
		if (curveTrigger.process(params[CURVE_PARAM].getValue()))
			curveIndex = (curveIndex + 1) % NUM_CURVES;
		if (delayTrigger.process(params[DELAY_PARAM].getValue()))
			delayIndex = (delayIndex + 1) % NUM_DELAYS;

		// Evaluate input triggers once per sample
		bool clockRise = clockTrigger.process(inputs[CLOCK_INPUT].getVoltage());
		bool resetRise = resetTrigger.process(inputs[RESET_INPUT].getVoltage());
		bool fireInputRise = fireInputTrigger.process(inputs[FIRE_INPUT].getVoltage());
		bool fireButtonRise = fireButtonTrigger.process(params[FIRE_PARAM].getValue());
		bool fireRise = fireInputRise || fireButtonRise;

		// Reset cancels everything and returns output to Init voltage
		if (resetRise)
			resetAll();

		// Clock edge handling first, so a transition can end on this edge
		if (clockRise) {
			if (haveLastEdge) {
				float p = samplesSinceEdge * sampleTime;
				lastPeriod = math::clamp(p, 5e-4f, 15.f);
			}
			haveLastEdge = true;
			samplesSinceEdge = 0.f;

			switch (state) {
				case ST_ARMED:
					if (delayRemaining > 0)
						state = ST_DELAYING;
					else
						startTransition();
					break;
				case ST_DELAYING:
					delayRemaining--;
					if (delayRemaining <= 0)
						startTransition();
					break;
case ST_TRANSITION: {
                    completedPeriods++;
                    if (completedPeriods >= transSteps) {
                        finishTransition();
                    }
                    else if (curveIndex == STEP) {
                        float t = (float) completedPeriods / transSteps;
                        outVoltage = snapInit + (snapTarget - snapInit) * shape(t);
                    }
                } break;
				default:
					break;
			}
		}
		else {
			samplesSinceEdge += 1.f;
		}

		// Fire input and/or button.
		// Ignored while a transition/delay is already active.
		if (fireRise && state == ST_IDLE) {
			float sinceEdge = samplesSinceEdge * sampleTime;
			bool edgeAvailable = clockRise
				|| (sameStepChain && haveLastEdge && sinceEdge <= catchupWindow());
			int dly = DELAY_VALUES[delayIndex];
			delayRemaining = dly;
			if (edgeAvailable) {
				// The current/recent clock edge acts as the arming edge:
				// the delay is always respected, counted from that edge.
				if (dly > 0)
					state = ST_DELAYING;
				else
					startTransition();
			}
			else {
				state = ST_ARMED;
			}
		}

		// Continuous progress for Ramp / RampBack: O(1) incremental update.
		// Progress follows the clock: advances only within the granted clock
		// period; if the clock stops, progress freezes until the next edge.
		if (state == ST_TRANSITION && curveIndex != STEP) {
			float e = samplesSinceEdge * sampleTime;
			if (e <= snapPeriod) {
				float t = (completedPeriods + e / snapPeriod) / transSteps;
				t = math::clamp(t, 0.f, 1.f);
				outVoltage = snapInit + (snapTarget - snapInit) * shapedProgress(t);
			}
		}

		outputs[MAIN_OUTPUT].setVoltage(outVoltage);
		if (eotRemaining > 0.f) {
			eotRemaining -= sampleTime;
			outputs[EOT_OUTPUT].setVoltage(10.f);
		}
		else {
			outputs[EOT_OUTPUT].setVoltage(0.f);
		}

		// Housekeeping every 64 samples: light smoothing only
		if (++frameDivider >= 64) {
			frameDivider = 0;
			bool active = (state == ST_DELAYING || state == ST_TRANSITION);
			lights[ACTIVE_LIGHT].setSmoothBrightness(active ? 1.f : 0.f, sampleTime * 64.f);
		}
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "curve", json_integer(curveIndex));
		json_object_set_new(rootJ, "delay", json_integer(delayIndex));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* j = json_object_get(rootJ, "curve");
		if (j)
			curveIndex = math::clamp((int) json_integer_value(j), 0, NUM_CURVES - 1);
		j = json_object_get(rootJ, "delay");
		if (j)
			delayIndex = math::clamp((int) json_integer_value(j), 0, NUM_DELAYS - 1);
		j = json_object_get(rootJ, "sameStepChain");
		if (j)
			sameStepChain = json_is_true(j);
	}
};


struct QuoJackPort : SvgPort {
	QuoJackPort() {
		setSvg(Svg::load(asset::plugin(pluginInstance, "res/QuoJack.svg")));
	}
};

struct MorphKnob : SvgKnob {

	MorphKnob() {
        minAngle = -2.11185f;
        maxAngle = 2.11185f;
		setSvg(Svg::load(asset::plugin(pluginInstance, "res/MorphfastMiniKnob.svg")));
	}
};

struct MorphButton : SvgSwitch {
	MorphButton() {
		momentary = true;
		addFrame(Svg::load(asset::plugin(pluginInstance, "res/MorphfastButton.svg")));
	}
};

struct MorphButtonFire : SvgSwitch {
	MorphButtonFire() {
		momentary = true;
		addFrame(Svg::load(asset::plugin(pluginInstance, "res/MorphfastButtonFire.svg")));
	}
};

// LCD-style text display: white text, transparent background,
// font height fitted exactly to the widget box height.
struct LcdTextWidget : widget::Widget {
	std::string text;
	std::shared_ptr<window::Font> font;
	MorphfastMini* module = NULL;

	LcdTextWidget() {
		font = APP->window->loadFont(asset::plugin(pluginInstance, "res/hd44780.otf"));
	}

	void draw(const DrawArgs& args) override {
		if (text.empty() || !font)
			return;
		nvgFontSize(args.vg, box.size.y);
		nvgFontFaceId(args.vg, font->handle);
		nvgFillColor(args.vg, nvgRGBf(1.f, 1.f, 1.f));
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgText(args.vg, box.size.x / 2.f, box.size.y / 2.f, text.c_str(), NULL);
	}
};

struct DurationDisplay : LcdTextWidget {
	void step() override {
		if (module)
			text = string::f("%02d", (int) std::round(module->params[MorphfastMini::DURATION_PARAM].getValue()));
	}
};

struct DelayDisplay : LcdTextWidget {
	void step() override {
		MorphfastMini* m = dynamic_cast<MorphfastMini*>(module);
		text = DELAY_LABELS[m ? m->delayIndex : 0];
	}
};

struct CurveDisplay : widget::Widget {
	MorphfastMini* module = NULL;
	widget::SvgWidget* svgs[NUM_CURVES];

	CurveDisplay() {
		static const char* files[NUM_CURVES] = {"CurveRamp.svg", "CurveRampBack.svg", "CurveStep.svg", "CurveTriangle.svg"};
		for (int i = 0; i < NUM_CURVES; i++) {
			svgs[i] = new widget::SvgWidget;
			svgs[i]->setSvg(Svg::load(asset::plugin(pluginInstance, std::string("res/") + files[i])));
			addChild(svgs[i]);
		}
		box.size = svgs[0]->box.size;
	}

	void step() override {
		MorphfastMini* m = dynamic_cast<MorphfastMini*>(module);
		int idx = m ? m->curveIndex : 0;
		for (int i = 0; i < NUM_CURVES; i++)
			svgs[i]->visible = (i == idx);
	}
};


struct MorphfastMiniWidget : ModuleWidget {
	MorphfastMiniWidget(MorphfastMini* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/MorphfastMini.svg")));

		// Knob backgrounds are added first so knobs draw on top of them
		auto addKnobBg = [&](math::Vec centerMM) {
			auto bg = new widget::SvgWidget;
			bg->setSvg(Svg::load(asset::plugin(pluginInstance, "res/MorphfastMiniKnob_bg.svg")));
			bg->box.pos = mm2px(centerMM) - bg->box.size / 2.f;
			addChild(bg);
		};
		addKnobBg(math::Vec(7.62f, 45.8814f));
		addKnobBg(math::Vec(7.62f, 64.4447f));
		addKnobBg(math::Vec(7.62f, 96.9137f));

		addInput(createInputCentered<QuoJackPort>(mm2px(math::Vec(7.6200f, 9.5250f)), module, MorphfastMini::CLOCK_INPUT));
		addInput(createInputCentered<QuoJackPort>(mm2px(math::Vec(7.6200f, 19.1823f)), module, MorphfastMini::RESET_INPUT));
		addInput(createInputCentered<QuoJackPort>(mm2px(math::Vec(7.6200f, 28.8396f)), module, MorphfastMini::FIRE_INPUT));

		addParam(createParamCentered<MorphButtonFire>(mm2px(math::Vec(7.6200f, 36.8637f)), module, MorphfastMini::FIRE_PARAM));
		addParam(createParamCentered<MorphKnob>(mm2px(math::Vec(7.6200f, 45.8814f)), module, MorphfastMini::INIT_PARAM));
		addParam(createParamCentered<MorphButton>(mm2px(math::Vec(7.6200f, 55.9897f)), module, MorphfastMini::SWAP_PARAM));
		addParam(createParamCentered<MorphKnob>(mm2px(math::Vec(7.6200f, 64.4447f)), module, MorphfastMini::TARGET_PARAM));

		auto durationDisplay = createWidget<DurationDisplay>(mm2px(math::Vec(4.0359f, 74.5772f)));
		durationDisplay->box.size = mm2px(math::Vec(7.2326f, 4.6087f));
		durationDisplay->module = module;
		addChild(durationDisplay);

		auto curveDisplay = createWidgetCentered<CurveDisplay>(mm2px(math::Vec(5.4754f, 82.1841f)));
		curveDisplay->module = module;
		addChild(curveDisplay);

		auto dlyLabel = createWidget<LcdTextWidget>(mm2px(math::Vec(8.8395f, 80.2130f)));
		dlyLabel->box.size = mm2px(math::Vec(3.4269f, 1.4897f));
		dlyLabel->text = "DLY";
		addChild(dlyLabel);

		auto dlyValue = createWidget<DelayDisplay>(mm2px(math::Vec(8.8395f, 81.9220f)));
		dlyValue->box.size = mm2px(math::Vec(3.4269f, 1.4898f));
		dlyValue->module = module;
		addChild(dlyValue);

		addParam(createParamCentered<MorphButton>(mm2px(math::Vec(4.2776f, 88.4551f)), module, MorphfastMini::CURVE_PARAM));
		addParam(createParamCentered<MorphButton>(mm2px(math::Vec(11.0103f, 88.4551f)), module, MorphfastMini::DELAY_PARAM));

		addParam(createParamCentered<MorphKnob>(mm2px(math::Vec(7.6200f, 96.9137f)), module, MorphfastMini::DURATION_PARAM));

		addOutput(createOutputCentered<QuoJackPort>(mm2px(math::Vec(7.6200f, 109.7980f)), module, MorphfastMini::EOT_OUTPUT));
		addOutput(createOutputCentered<QuoJackPort>(mm2px(math::Vec(7.6200f, 119.3230f)), module, MorphfastMini::MAIN_OUTPUT));

		addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(math::Vec(11.4407f, 116.9840f)), module, MorphfastMini::ACTIVE_LIGHT));
	}

	void appendContextMenu(Menu* menu) override {
		MorphfastMini* m = dynamic_cast<MorphfastMini*>(this->module);
		if (!m)
			return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Ease"));
		auto easeInSlider = new ui::Slider;
		easeInSlider->quantity = m->paramQuantities[MorphfastMini::EASE_IN_PARAM];
		easeInSlider->box.size = math::Vec(220.f, 20.f);
		menu->addChild(easeInSlider);
		auto easeOutSlider = new ui::Slider;
		easeOutSlider->quantity = m->paramQuantities[MorphfastMini::EASE_OUT_PARAM];
		easeOutSlider->box.size = math::Vec(220.f, 20.f);
		menu->addChild(easeOutSlider);
		menu->addChild(new MenuSeparator);
		menu->addChild(createCheckMenuItem("Same-step chain", "",
			[=]() { return m->sameStepChain; },
			[=]() { m->sameStepChain ^= true; }
		));
	}
};


Model* modelMorphfastMini = createModel<MorphfastMini, MorphfastMiniWidget>("MorphfastMini");
