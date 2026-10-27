// Ping: a latency meter. It sends a pulse to a destination (or takes the trigger that plays it) and times the
// sound that comes back, so each destination's latency is a number stored with the patch.
#include "plugin.hpp"
#include "PingLayout.hpp"
#include "engine/PingMeter.hpp"

namespace P = ping_layout;

struct Ping : Module {
	enum ParamIds { PING_PARAM, AUTO_PARAM, THRESH_PARAM, NUM_PARAMS };
	enum InputIds { TRIG_INPUT, RETURN_INPUT, NUM_INPUTS };
	enum OutputIds { PULSE_OUTPUT, NUM_OUTPUTS };
	enum LightIds { PING_LIGHT, NUM_LIGHTS };

	gorgon::PingMeter meter;
	dsp::SchmittTrigger button, trigger;
	dsp::PulseGenerator pulse;
	float rate = 0.f, sinceAuto = 0.f;
	std::atomic<float> interval{.5f}, pulseLength{.01f};
	std::atomic<float> resultRate{0.f};      // the sample rate the results were measured at
	std::atomic<bool> clearRequested{false};

	Ping() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configButton(PING_PARAM, "Ping: send a pulse and time its return");
		configSwitch(AUTO_PARAM, 0.f, 1.f, 0.f, "Ping repeatedly", {"Off", "On"});
		// In dB, shown in volts: 1 mV to 1 V.
		configParam(THRESH_PARAM, -60.f, 0.f, -40.f, "Return threshold", " V", std::pow(10.f, 1.f / 20.f));
		configInput(TRIG_INPUT, "Trigger: start a measurement (patch the gate that also plays the destination)");
		configInput(RETURN_INPUT, "Return: the destination's output (polyphonic: the loudest channel)");
		configOutput(PULSE_OUTPUT, "Pulse: to the destination's trigger or gate input");
	}

	void process(const ProcessArgs& args) override {
		if (args.sampleRate != rate) {
			rate = args.sampleRate;
			meter.setRate(rate);
		}
		if (clearRequested.exchange(false))
			meter.clear();
		meter.threshold = std::pow(10.f, params[THRESH_PARAM].getValue() / 20.f);

		bool start = button.process(params[PING_PARAM].getValue());
		if (params[AUTO_PARAM].getValue() > .5f) {
			sinceAuto += args.sampleTime;
			if (sinceAuto >= interval.load(std::memory_order_relaxed)) {
				sinceAuto = 0.f;
				start = true;
			}
		}
		else
			sinceAuto = 0.f;
		// A trigger also reaches the destination itself, so its count drops one cable step, not two.
		int steps = gorgon::PingMeter::PULSE_STEPS;
		if (trigger.process(inputs[TRIG_INPUT].getVoltage(), .1f, 1.f)) {
			start = true;
			steps = gorgon::PingMeter::TRIGGER_STEPS;
		}
		if (start) {
			// Results in samples belong to one sample rate.
			if (resultRate.load(std::memory_order_relaxed) != rate) {
				meter.clear();
				resultRate.store(rate, std::memory_order_relaxed);
			}
			meter.start(steps);
			pulse.trigger(pulseLength.load(std::memory_order_relaxed));
		}
		float level = 0.f;
		for (int c = 0; c < inputs[RETURN_INPUT].getChannels(); c++)
			level = std::max(level, std::fabs(inputs[RETURN_INPUT].getVoltage(c)));
		meter.step(level);
		outputs[PULSE_OUTPUT].setVoltage(pulse.process(args.sampleTime) ? 10.f : 0.f);
		lights[PING_LIGHT].setBrightness(meter.state() == gorgon::PingMeter::Status::Waiting ? 1.f : 0.f);
	}

	gorgon::PingStats stats() const {
		std::array<int64_t, gorgon::PingMeter::HISTORY> values;
		int n = meter.copy(values);
		return gorgon::pingStats(values, n);
	}

	std::string summary() const {
		gorgon::PingStats s = stats();
		float r = resultRate.load();
		if (!s.n || r <= 0.f)
			return "Ping: no results";
		auto ms = [r](int64_t samples) { return 1000.0 * double(samples) / r; };
		return string::f("Ping: %d results at %.0f Hz. Last %lld samples (%.2f ms), median %lld (%.2f ms), min %lld, max %lld.",
		                 s.n, r, (long long)s.last, ms(s.last), (long long)s.median, ms(s.median), (long long)s.min, (long long)s.max);
	}

	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		interval = .5f;
		pulseLength = .01f;
		clearRequested = true;
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "interval", json_real(interval.load()));
		json_object_set_new(root, "pulse", json_real(pulseLength.load()));
		std::array<int64_t, gorgon::PingMeter::HISTORY> values;
		int n = meter.copy(values);
		json_t* results = json_array();
		for (int i = 0; i < n; i++)
			json_array_append_new(results, json_integer(values[size_t(i)]));
		json_object_set_new(root, "results", results);
		json_object_set_new(root, "rate", json_real(resultRate.load()));
		return root;
	}

	void dataFromJson(json_t* root) override {
		auto number = [root](const char* key, float lo, float hi, float fallback) {
			json_t* v = json_object_get(root, key);
			double x = json_is_number(v) ? json_number_value(v) : fallback;
			return std::isfinite(x) ? clamp(float(x), lo, hi) : fallback;
		};
		interval = number("interval", .05f, 10.f, .5f);
		pulseLength = number("pulse", .0005f, 1.f, .01f);
		std::array<int64_t, gorgon::PingMeter::HISTORY> values{};
		int n = 0;
		json_t* results = json_object_get(root, "results");
		for (size_t i = 0; i < json_array_size(results) && n < gorgon::PingMeter::HISTORY; i++) {
			json_t* v = json_array_get(results, i);
			if (json_is_integer(v))
				values[size_t(n++)] = json_integer_value(v);
		}
		float r = number("rate", 0.f, 1e6f, 0.f);
		meter.restore(values.data(), r > 0.f ? n : 0);
		resultRate = r;
	}
};

struct PingDisplay : TransparentWidget {
	Ping* module = nullptr;

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1)
			return;
		std::shared_ptr<Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		if (!font)
			return;
		std::string big = "22.96 ms", samples = "1102 samples", median = "med 1102  n 20", range = "1101..1104", status = "48000 Hz";
		if (module) {
			gorgon::PingStats s = module->stats();
			float r = module->resultRate.load();
			if (s.n && r > 0.f) {
				big = string::f("%.2f ms", 1000.0 * double(s.last) / r);
				samples = string::f("%lld samples", (long long)s.last);
				median = string::f("med %lld  n %d", (long long)s.median, s.n);
				range = string::f("%lld..%lld", (long long)s.min, (long long)s.max);
			}
			else {
				big = "-";
				samples = median = range = "";
			}
			switch (module->meter.state()) {
				case gorgon::PingMeter::Status::Waiting: status = "WAITING"; break;
				case gorgon::PingMeter::Status::NoReturn: status = "NO RETURN"; break;
				case gorgon::PingMeter::Status::NotQuiet: status = "NOT QUIET"; break;
				default: status = s.n ? string::f("%.0f Hz", r) : "PING TO MEASURE";
			}
		}
		nvgFontFaceId(args.vg, font->handle);
		nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
		nvgFontSize(args.vg, 14.f);
		nvgFillColor(args.vg, nvgRGB(0x8f, 0xdc, 0xc4));
		nvgText(args.vg, 5.f, 16.f, big.c_str(), nullptr);
		nvgFontSize(args.vg, 10.f);
		nvgFillColor(args.vg, nvgRGB(0xd9, 0xe2, 0xe5));
		nvgText(args.vg, 5.f, 29.f, samples.c_str(), nullptr);
		nvgFontSize(args.vg, 9.f);
		nvgFillColor(args.vg, nvgRGB(0x7f, 0x94, 0x9c));
		nvgText(args.vg, 5.f, 41.f, median.c_str(), nullptr);
		nvgText(args.vg, 5.f, 52.f, range.c_str(), nullptr);
		nvgFillColor(args.vg, nvgRGB(0xf0, 0xb4, 0x6a));
		nvgText(args.vg, 5.f, 63.f, status.c_str(), nullptr);
	}
};

struct PingWidget : ModuleWidget {
	PingWidget(Ping* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Ping.svg")));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		PingDisplay* display = createWidget<PingDisplay>(Vec(P::DISPLAY_X, P::DISPLAY_Y));
		display->box.size = Vec(P::DISPLAY_W, P::DISPLAY_H);
		display->module = module;
		addChild(display);

		addParam(createLightParamCentered<VCVLightBezel<YellowLight>>(Vec(P::BUTTON_PING_X, P::BUTTON_PING_Y), module,
		                                                             Ping::PING_PARAM, Ping::PING_LIGHT));
		addParam(createParamCentered<CKSS>(Vec(P::SWITCH_AUTO_X, P::SWITCH_AUTO_Y), module, Ping::AUTO_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(Vec(P::KNOB_THRESH_X, P::KNOB_THRESH_Y), module, Ping::THRESH_PARAM));
		addInput(createInputCentered<PJ301MPort>(Vec(P::JACK_TRIG_X, P::JACK_TRIG_Y), module, Ping::TRIG_INPUT));
		addInput(createInputCentered<PJ301MPort>(Vec(P::JACK_RETURN_X, P::JACK_RETURN_Y), module, Ping::RETURN_INPUT));
		addOutput(createOutputCentered<DarkPJ301MPort>(Vec(P::JACK_PULSE_X, P::JACK_PULSE_Y), module, Ping::PULSE_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Ping* module = getModule<Ping>();
		if (!module)
			return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel(module->summary()));
		menu->addChild(createMenuItem("Copy results", "", [=] { glfwSetClipboardString(APP->window->win, module->summary().c_str()); }));
		menu->addChild(createMenuItem("Clear results", "", [=] { module->clearRequested = true; }));
		menu->addChild(createSubmenuItem("Repeat every (AUTO)", "", [=](Menu* sub) {
			for (float s : {.25f, .5f, 1.f, 2.f, 4.f})
				sub->addChild(createCheckMenuItem(string::f("%g s", s), "", [=] { return module->interval.load() == s; },
				                                  [=] { module->interval = s; }));
		}));
		menu->addChild(createSubmenuItem("Pulse length", "", [=](Menu* sub) {
			for (float s : {.001f, .01f, .05f, .2f})
				sub->addChild(createCheckMenuItem(string::f("%g ms", s * 1000.f), "", [=] { return module->pulseLength.load() == s; },
				                                  [=] { module->pulseLength = s; }));
		}));
		menu->addChild(createMenuLabel("Counts from the destination's input to its output crossing THRESH;"));
		menu->addChild(createMenuLabel("Rack's one-sample cable steps are taken off."));
	}
};

Model* modelPing = createModel<Ping, PingWidget>("Ping");
