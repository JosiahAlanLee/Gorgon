// Gorgon: a bridge between VCV Rack and HydraPy Studio's Gorgon tracker.
//
// Gorgon is the master clock. Studio's playhead arrives about 30 times a second;
// between updates the module runs its own beat at the score tempo on Rack's
// sample clock, nudged toward Studio's, so clock, gates and held automation
// steps come out sample-exact. CV inputs go back to Studio as values for the
// patch's named controls, and PLAY/RESET drive Studio's transport.
#include <atomic>

#include "plugin.hpp"
#include "Layout.hpp"
#include "engine/Bridge.hpp"

namespace L = gorgon_layout;

struct Gorgon : Module {
	enum ParamIds { PLAY_PARAM, RESET_PARAM, OFFSET_PARAM, NUM_PARAMS };
	enum InputIds { PLAY_INPUT, RESET_INPUT, CV1_INPUT, CV2_INPUT, CV3_INPUT, CV4_INPUT, NUM_INPUTS };
	enum OutputIds {
		CLOCK_OUTPUT, RESET_OUTPUT, RUN_OUTPUT, VEL_OUTPUT,
		GATE1_OUTPUT, GATE2_OUTPUT, GATE3_OUTPUT, GATE4_OUTPUT,
		Z_OUTPUT, ROLL_OUTPUT, FEEDBACK_OUTPUT, GATES_OUTPUT, PITCH_OUTPUT, NUM_OUTPUTS
	};
	enum LightIds { PLAY_LIGHT, LINK_LIGHT, CLOCK_LIGHT, NUM_LIGHTS };

	gorgon::Bridge bridge{[] { return system::getTime(); }};
	gorgon::Snapshot snapshot;
	uint64_t seen = 0;
	bool haveSnapshot = false;
	gorgon::Follower follower;
	gorgon::Options options;
	gorgon::OutputState outputState;
	dsp::SchmittTrigger playTrigger, resetTrigger;
	dsp::BooleanTrigger playButton, resetButton;

	// Set from the context menu (UI thread), read by process().
	std::atomic<float> ppqn{4.f}, gateLength{.5f};
	std::atomic<int> pitchZero{60};
	// For the display (written by process()).
	std::atomic<float> shownBeat{0.f};
	std::atomic<bool> shownMoving{false};

	Gorgon() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configButton(PLAY_PARAM, "Play / pause Gorgon");
		configButton(RESET_PARAM, "Back to the start of the score or looped pattern");
		configParam(OFFSET_PARAM, -100.f, 100.f, 0.f, "Timing offset", " ms");
		paramQuantities[OFFSET_PARAM]->description = "Positive sends Rack's outputs earlier than Gorgon's picture";
		configInput(PLAY_INPUT, "Play / pause trigger");
		configInput(RESET_INPUT, "Reset trigger");
		for (int i = 0; i < gorgon::INPUTS; i++)
			configInput(CV1_INPUT + i, string::f("CV %d to a Gorgon control (0–10 V spans its range)", i + 1));
		configOutput(CLOCK_OUTPUT, "Clock");
		configOutput(RESET_OUTPUT, "Reset (5 ms trigger at the start of the score or loop)");
		configOutput(RUN_OUTPUT, "Run (high while Gorgon plays)");
		configOutput(VEL_OUTPUT, "Velocity, lanes 1–4 (polyphonic, held)");
		for (int i = 0; i < gorgon::AUDIO_LANES; i++)
			configOutput(GATE1_OUTPUT + i, string::f("Gate %d", i + 1));
		configOutput(Z_OUTPUT, "Dolly Z lane (0–10 V)");
		configOutput(ROLL_OUTPUT, "Roll lane (±5 V for ±45°)");
		configOutput(FEEDBACK_OUTPUT, "Feedback lane (0–10 V for 0–0.9)");
		configOutput(GATES_OUTPUT, "Gates 1–4 (polyphonic)");
		configOutput(PITCH_OUTPUT, "Pitch, lanes 1–4 (polyphonic, held, 1 V/oct)");
		configLight(LINK_LIGHT, "Linked to Studio");
		configLight(CLOCK_LIGHT, "Clock");
		bridge.start();
	}

	~Gorgon() override { bridge.stop(); }

	void process(const ProcessArgs& args) override {
		if (args.frame % 32 == 0) {
			if (bridge.poll(seen, snapshot))
				haveSnapshot = true;
			if (haveSnapshot && snapshot.clock.ready()) {
				// Studio's beat for this sample, on Studio's clock.
				double local = APP->engine->getBlockTime() + double(args.frame - APP->engine->getBlockFrame()) * args.sampleTime;
				double offset = params[OFFSET_PARAM].getValue() / 1000.;
				double target = snapshot.playhead.beatAt(snapshot.score, snapshot.clock.toServer(local + offset));
				follower.correct(snapshot.score, snapshot.playhead.moving(), target);
			}
			for (int i = 0; i < gorgon::INPUTS; i++)
				bridge.setInput(i, inputs[CV1_INPUT + i].isConnected(), inputs[CV1_INPUT + i].getVoltage());
			options.ppqn = ppqn.load(std::memory_order_relaxed);
			options.gate = gateLength.load(std::memory_order_relaxed);
				options.pitchZero = pitchZero.load(std::memory_order_relaxed);
		}

		bool moving = haveSnapshot && snapshot.playhead.moving() && follower.locked;
			gorgon::Outputs o = outputState.render(snapshot.score, moving, follower.beat,
			                                      snapshot.playhead.transportRevision, options, args.sampleTime);
		follower.advance(snapshot.score, moving, args.sampleTime);

		outputs[CLOCK_OUTPUT].setVoltage(o.clock);
		outputs[RESET_OUTPUT].setVoltage(o.reset);
		outputs[RUN_OUTPUT].setVoltage(o.run);
		outputs[VEL_OUTPUT].setChannels(gorgon::AUDIO_LANES);
		outputs[GATES_OUTPUT].setChannels(gorgon::AUDIO_LANES);
			outputs[PITCH_OUTPUT].setChannels(gorgon::AUDIO_LANES);
		for (int l = 0; l < gorgon::AUDIO_LANES; l++) {
			outputs[GATE1_OUTPUT + l].setVoltage(o.gate[l]);
			outputs[GATES_OUTPUT].setVoltage(o.gate[l], l);
				outputs[VEL_OUTPUT].setVoltage(o.velocity[l] * 10.f, l);
				outputs[PITCH_OUTPUT].setVoltage(o.pitch[l], l);
		}
		outputs[Z_OUTPUT].setVoltage(gorgon::laneVolts(gorgon::LANE_Z, o.lane[gorgon::LANE_Z]));
		outputs[ROLL_OUTPUT].setVoltage(gorgon::laneVolts(gorgon::LANE_ROLL, o.lane[gorgon::LANE_ROLL]));
		outputs[FEEDBACK_OUTPUT].setVoltage(gorgon::laneVolts(gorgon::LANE_FEEDBACK, o.lane[gorgon::LANE_FEEDBACK]));

		if (playButton.process(params[PLAY_PARAM].getValue() > 0.f) |
		    playTrigger.process(inputs[PLAY_INPUT].getVoltage(), .1f, 1.f))
			bridge.requestPlayToggle();
		if (resetButton.process(params[RESET_PARAM].getValue() > 0.f) |
		    resetTrigger.process(inputs[RESET_INPUT].getVoltage(), .1f, 1.f))
			bridge.requestRestart();

		lights[PLAY_LIGHT].setBrightness(moving ? 1.f : 0.f);
		lights[LINK_LIGHT].setBrightness(bridge.linked() ? 1.f : 0.f);
		lights[CLOCK_LIGHT].setBrightnessSmooth(o.clock > 0.f ? 1.f : 0.f, args.sampleTime);
		shownBeat.store(float(follower.beat), std::memory_order_relaxed);
		shownMoving.store(moving, std::memory_order_relaxed);
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_t* b = json_array();
		for (int i = 0; i < gorgon::INPUTS; i++)
			json_array_append_new(b, json_string(bridge.binding(i).c_str()));
		json_object_set_new(root, "bindings", b);
		json_object_set_new(root, "ppqn", json_real(ppqn.load()));
		json_object_set_new(root, "gate", json_real(gateLength.load()));
			json_object_set_new(root, "pitchZero", json_integer(pitchZero.load()));
		return root;
	}

	void dataFromJson(json_t* root) override {
		json_t* b = json_object_get(root, "bindings");
		for (int i = 0; i < gorgon::INPUTS; i++) {
			const char* name = json_string_value(json_array_get(b, size_t(i)));
			bridge.bind(i, name ? name : "");
		}
		if (json_t* p = json_object_get(root, "ppqn"))
			ppqn = clamp(float(json_number_value(p)), 1.f, 96.f);
		if (json_t* g = json_object_get(root, "gate"))
			gateLength = clamp(float(json_number_value(g)), .01f, 1.f);
			if (json_t* p = json_object_get(root, "pitchZero"))
				pitchZero = clamp(int(json_integer_value(p)), 0, 127);
	}
};

struct GorgonDisplay : TransparentWidget {
	Gorgon* module = nullptr;

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1)
			return;
		std::shared_ptr<Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		if (!font)
			return;
		std::string status = "Linked  138 BPM", position = "P1 row 00  beat 0.0", targets = "1 Warp 2 Hue 3 - 4 -";
		bool linked = true;
		if (module) {
			status = module->bridge.status();
			linked = module->bridge.linked();
			float beat = module->shownBeat.load();
			int pattern = int(beat / gorgon::BEATS_PER_PATTERN) + 1;
			int row = int(std::fmod(beat, float(gorgon::BEATS_PER_PATTERN)) * 4.f);
			position = module->shownMoving.load() ? string::f("P%d row %02d  beat %.1f", pattern, row, beat)
			                                      : string::f("P%d row %02d  stopped", pattern, row);
			targets.clear();
			for (int i = 0; i < gorgon::INPUTS; i++) {
				std::string t = module->bridge.target(i);
				if (t.size() > 7)
					t = t.substr(0, 6) + ".";
				targets += string::f("%s%d %s", i ? " " : "", i + 1, t.empty() ? "-" : t.c_str());
			}
		}
		nvgFontFaceId(args.vg, font->handle);
		nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
		nvgFontSize(args.vg, 11.f);
		nvgFillColor(args.vg, linked ? nvgRGB(0x8f, 0xdc, 0xc4) : nvgRGB(0xf0, 0xb4, 0x6a));
		nvgText(args.vg, 6.f, 15.f, status.c_str(), nullptr);
		nvgFillColor(args.vg, nvgRGB(0xd9, 0xe2, 0xe5));
		nvgText(args.vg, 6.f, 30.f, position.c_str(), nullptr);
		nvgFontSize(args.vg, 9.5f);
		nvgFillColor(args.vg, nvgRGB(0x7f, 0x94, 0x9c));
		nvgText(args.vg, 6.f, 45.f, targets.c_str(), nullptr);
	}
};

struct GorgonWidget : ModuleWidget {
	GorgonWidget(Gorgon* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Gorgon.svg")));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		GorgonDisplay* display = createWidget<GorgonDisplay>(Vec(L::DISPLAY_X, L::DISPLAY_Y));
		display->box.size = Vec(L::DISPLAY_W, L::DISPLAY_H);
		display->module = module;
		addChild(display);

		addParam(createLightParamCentered<VCVLightBezel<GreenLight>>(Vec(L::BUTTON_PLAY_X, L::BUTTON_PLAY_Y), module,
		                                                            Gorgon::PLAY_PARAM, Gorgon::PLAY_LIGHT));
		addParam(createParamCentered<VCVButton>(Vec(L::BUTTON_RESET_X, L::BUTTON_RESET_Y), module, Gorgon::RESET_PARAM));
		addParam(createParamCentered<Trimpot>(Vec(L::TRIMPOT_OFFSET_X, L::TRIMPOT_OFFSET_Y), module, Gorgon::OFFSET_PARAM));
		addChild(createLightCentered<SmallLight<GreenLight>>(Vec(L::LIGHT_LINK_LIGHT_X, L::LIGHT_LINK_LIGHT_Y), module, Gorgon::LINK_LIGHT));
		addChild(createLightCentered<SmallLight<YellowLight>>(Vec(L::LIGHT_CLOCK_LIGHT_X, L::LIGHT_CLOCK_LIGHT_Y), module, Gorgon::CLOCK_LIGHT));

		addInput(createInputCentered<PJ301MPort>(Vec(L::JACK_PLAY_IN_X, L::JACK_PLAY_IN_Y), module, Gorgon::PLAY_INPUT));
		addInput(createInputCentered<PJ301MPort>(Vec(L::JACK_RESET_IN_X, L::JACK_RESET_IN_Y), module, Gorgon::RESET_INPUT));
		const Vec cv[] = {{L::JACK_CV1_X, L::JACK_CV1_Y}, {L::JACK_CV2_X, L::JACK_CV2_Y},
		                  {L::JACK_CV3_X, L::JACK_CV3_Y}, {L::JACK_CV4_X, L::JACK_CV4_Y}};
		for (int i = 0; i < gorgon::INPUTS; i++)
			addInput(createInputCentered<PJ301MPort>(cv[i], module, Gorgon::CV1_INPUT + i));

		struct Jack { float x, y; int id; };
		const Jack outs[] = {
			{L::JACK_CLOCK_X, L::JACK_CLOCK_Y, Gorgon::CLOCK_OUTPUT}, {L::JACK_RESET_OUT_X, L::JACK_RESET_OUT_Y, Gorgon::RESET_OUTPUT},
			{L::JACK_RUN_X, L::JACK_RUN_Y, Gorgon::RUN_OUTPUT}, {L::JACK_VEL_X, L::JACK_VEL_Y, Gorgon::VEL_OUTPUT},
			{L::JACK_PITCH_X, L::JACK_PITCH_Y, Gorgon::PITCH_OUTPUT},
			{L::JACK_GATE1_X, L::JACK_GATE1_Y, Gorgon::GATE1_OUTPUT}, {L::JACK_GATE2_X, L::JACK_GATE2_Y, Gorgon::GATE2_OUTPUT},
			{L::JACK_GATE3_X, L::JACK_GATE3_Y, Gorgon::GATE3_OUTPUT}, {L::JACK_GATE4_X, L::JACK_GATE4_Y, Gorgon::GATE4_OUTPUT},
			{L::JACK_Z_X, L::JACK_Z_Y, Gorgon::Z_OUTPUT}, {L::JACK_ROLL_X, L::JACK_ROLL_Y, Gorgon::ROLL_OUTPUT},
			{L::JACK_FEEDBACK_X, L::JACK_FEEDBACK_Y, Gorgon::FEEDBACK_OUTPUT}, {L::JACK_GATES_X, L::JACK_GATES_Y, Gorgon::GATES_OUTPUT},
		};
		for (const Jack& j : outs)
			addOutput(createOutputCentered<DarkPJ301MPort>(Vec(j.x, j.y), module, j.id));
	}

	void appendContextMenu(Menu* menu) override {
		Gorgon* module = getModule<Gorgon>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Studio: " + module->bridge.status()));
		menu->addChild(createMenuLabel("Reads " + module->bridge.advertisementFile()));

		for (int i = 0; i < gorgon::INPUTS; i++) {
			std::string current = module->bridge.target(i);
			menu->addChild(createSubmenuItem(string::f("CV %d drives", i + 1), current.empty() ? "nothing" : current,
				[=](Menu* sub) {
					std::string binding = module->bridge.binding(i);
					sub->addChild(createCheckMenuItem("Automatic (next control not on a tracker lane)", "",
						[=] { return binding.empty(); }, [=] { module->bridge.bind(i, ""); }));
					sub->addChild(createCheckMenuItem("Nothing", "",
						[=] { return binding == "-"; }, [=] { module->bridge.bind(i, "-"); }));
					std::vector<gorgon::Control> controls = module->bridge.controls();
					if (!controls.empty())
						sub->addChild(new MenuSeparator);
					for (const gorgon::Control& c : controls) {
						std::string name = c.name;
						sub->addChild(createCheckMenuItem(name, string::f("%g–%g", c.min, c.max),
							[=] { return binding == name; }, [=] { module->bridge.bind(i, name); }));
					}
				}));
		}

		menu->addChild(createSubmenuItem("Clock", string::f("%g PPQN", module->ppqn.load()), [=](Menu* sub) {
			for (float p : {1.f, 2.f, 4.f, 8.f, 24.f})
				sub->addChild(createCheckMenuItem(string::f("%g per quarter note%s", p, p == 4.f ? " (sixteenths)" : ""), "",
					[=] { return module->ppqn.load() == p; }, [=] { module->ppqn = p; }));
		}));
		menu->addChild(createSubmenuItem("Gate length", string::f("%.0f%% of a row", module->gateLength.load() * 100.f), [=](Menu* sub) {
			for (float g : {.25f, .5f, .75f, 1.f})
				sub->addChild(createCheckMenuItem(string::f("%.0f%%", g * 100.f), "",
					[=] { return module->gateLength.load() == g; }, [=] { module->gateLength = g; }));
		}));
		menu->addChild(createSubmenuItem("Pitch zero", string::f("MIDI %d", module->pitchZero.load()), [=](Menu* sub) {
			for (int note : {48, 60})
				sub->addChild(createCheckMenuItem(note == 60 ? "C4 = 0 V" : "C3 = 0 V", "",
					[=] { return module->pitchZero.load() == note; }, [=] { module->pitchZero = note; }));
		}));
	}
};

Model* modelGorgon = createModel<Gorgon, GorgonWidget>("Gorgon");
