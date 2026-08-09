// components.hpp
// Reusable "skin" components (jacks, buttons, lights) built from the custom
// SVG assets in res/. Keep this file plugin-wide (not per-module) so future
// modules can reuse the same look by just including it.
#pragma once
#include <rack.hpp>

using namespace rack;

extern Plugin* pluginInstance;

// ---------------------------------------------------------------------
// Jack
// ---------------------------------------------------------------------
struct QuoJack : app::SvgPort {
    QuoJack() {
        setSvg(APP->window->loadSvg(asset::plugin(pluginInstance, "res/QuoJack.svg")));
    }
};

// ---------------------------------------------------------------------
// Button (momentary, 2 frames: up / pressed)
// ---------------------------------------------------------------------
struct QuoButton : app::SvgSwitch {
    QuoButton() {
        momentary = true;
        addFrame(APP->window->loadSvg(asset::plugin(pluginInstance, "res/QuoButton_0.svg")));
        addFrame(APP->window->loadSvg(asset::plugin(pluginInstance, "res/QuoButton_1.svg")));
    }
};

// A light matching the diameter of the button's own "press_light" circle
// (extracted from quoSmallButton.svg: r=1.281769mm -> d=2.5636mm), drawn with
// Rack's normal procedural round-light shape. Visually identical to reusing
// the SVG shape directly (it IS the same circle), without depending on any
// SVG-internals API.
template <typename BASE>
struct QuoButtonLight : BASE {
    QuoButtonLight() {
        this->box.size = mm2px(Vec(2.5636f, 2.5636f));
        this->bgColor = nvgRGBA(0, 0, 0, 0.2);
        this->borderColor = nvgRGBA(0, 0, 0, 0);
    }
};

// ---------------------------------------------------------------------
// StateOverlaySvg: swaps in one of several pre-colored copies of a panel
// shape (all sharing the panel's full coordinate space) to fake a
// recolorable "light" out of arbitrary custom artwork, without touching any
// nanosvg/NSVG internals. Each variant file contains just the one shape,
// already positioned correctly, painted a single flat color; showing one on
// top of the panel exactly replaces the panel's own baked-in (default/off)
// version of that same shape, pixel for pixel.
// ---------------------------------------------------------------------
struct StateOverlaySvg : widget::SvgWidget {
    enum class State { NONE, FOCUS, ERROR, ACTIVE };

    std::shared_ptr<window::Svg> focusSvg, errorSvg, activeSvg;
    State state = State::NONE;

    StateOverlaySvg() {
        box.pos = Vec(0.f, 0.f);
    }

    // Any path may be empty ("") if that state doesn't apply to this row
    // (e.g. the r-value list rows have no ACTIVE/green state).
    void loadVariants(const std::string& focusPath, const std::string& errorPath, const std::string& activePath = "") {
        if (!focusPath.empty()) focusSvg = APP->window->loadSvg(asset::plugin(pluginInstance, focusPath));
        if (!errorPath.empty()) errorSvg = APP->window->loadSvg(asset::plugin(pluginInstance, errorPath));
        if (!activePath.empty()) activeSvg = APP->window->loadSvg(asset::plugin(pluginInstance, activePath));
        // All variants share the same full-panel canvas size; load any one
        // just to establish box.size, then hide until a state is set.
        auto any = focusSvg ? focusSvg : (errorSvg ? errorSvg : activeSvg);
        if (any) setSvg(any);
        this->visible = false;
    }

    void setState(State s) {
        if (s == state) return;
        state = s;
        switch (state) {
            case State::FOCUS:
                if (focusSvg) { setSvg(focusSvg); this->visible = true; }
                else this->visible = false;
                break;
            case State::ERROR:
                if (errorSvg) { setSvg(errorSvg); this->visible = true; }
                else this->visible = false;
                break;
            case State::ACTIVE:
                if (activeSvg) { setSvg(activeSvg); this->visible = true; }
                else this->visible = false;
                break;
            case State::NONE:
            default:
                this->visible = false;
                break;
        }
    }
};
