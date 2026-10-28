// Gorgon: a bridge between VCV Rack and HydraPy Studio's Gorgon tracker.
//
// Gorgon is the master clock. Studio's playhead arrives about 30 times a second;
// between updates the module runs its own beat at the score tempo on Rack's
// sample clock, nudged toward Studio's, so clock, gates and held automation
// steps come out sample-exact. CV inputs go back to Studio as values for the
// patch's named controls, and PLAY/RESET drive Studio's transport.
//
// Versions 1 and 2 of the score play on four lanes through GorgonEngine.hpp; version 3
// plays six through EngineV3.hpp, on elapsed ticks rather than beats.
#include <atomic>
#include <cstring>
#include <fstream>

#include <osdialog.h>

#include "plugin.hpp"
#include "Layout.hpp"
#include "engine/Bridge.hpp"
#include "engine/ByteBusInput.hpp"
#include "engine/ByteBusOutput.hpp"
#include "AudioInput.hpp"

namespace L = gorgon_layout;

struct Gorgon : Module {
	enum ParamIds { PLAY_PARAM, RESET_PARAM, OFFSET_PARAM, NUM_PARAMS };
	// IDs are saved in patches: new ports go at the end of each list.
	enum InputIds { PLAY_INPUT, RESET_INPUT, CV1_INPUT, CV2_INPUT, CV3_INPUT, CV4_INPUT, BUS_INPUT, NUM_INPUTS };
	enum OutputIds {
		CLOCK_OUTPUT, RESET_OUTPUT, RUN_OUTPUT, VEL_OUTPUT,
		GATE1_OUTPUT, GATE2_OUTPUT, GATE3_OUTPUT, GATE4_OUTPUT,
			Z_OUTPUT, ROLL_OUTPUT, FEEDBACK_OUTPUT, GATES_OUTPUT, PITCH_OUTPUT,
		OPTICAL_L_OUTPUT, OPTICAL_R_OUTPUT, BREAKS_L_OUTPUT, BREAKS_R_OUTPUT, BUS_OUTPUT,
		GATE5_OUTPUT, GATE6_OUTPUT, NUM_OUTPUTS
	};
	enum LightIds { PLAY_LIGHT, LINK_LIGHT, CLOCK_LIGHT, NUM_LIGHTS };

	gorgon::Bridge bridge{[] { return system::getTime(); }};
	gorgon::Snapshot snapshot;
	uint64_t seen = 0;
	bool haveSnapshot = false;
	gorgon::Follower follower;
	gorgon::Options options;
	gorgon::OutputState outputState;
	// Version 3: elapsed ticks instead of a cycle beat, and the candidates' engine.
	gorgon::v3::TickFollower tickFollower;
	gorgon::v3::EngineV3 engine3;
	gorgon::v3::TraceRing traceRing{4096};   // evaluator.md#trace: the menu saves it as JSONL
	bool wasV3 = false, rendered = false;
	// BUS IN (bus-io.md): the reader, this module's own layer, and the bit map.
	gorgon::ByteBusReader busReader;
	gorgon::v3::BusInput busIn;
	gorgon::v3::BusMap busMap;                              // the audio thread's copy
	std::atomic<int> busBits[gorgon::v3::BUS_FUNCTIONS];    // set from the menu
	std::atomic<int> shownBusIn{-1};
	gorgon::ByteBusOutput busOutput;
	std::atomic<int> busSource{int(gorgon::BusSource::Z)};
	std::atomic<unsigned> shownBusWord{0};
	GorgonAudioInput audioInput{this};
	dsp::SchmittTrigger playTrigger, resetTrigger;
	dsp::BooleanTrigger playButton, resetButton;

	// Set from the context menu (UI thread), read by process().
	std::atomic<float> ppqn{4.f}, gateLength{.5f};
	// The Clock menu's rates. 32 clocks the fastest row grid (rate 8) once per row, 16 rate 4
	// (score-v3.md#signals-automation-and-scenes).
	static constexpr float PPQN_CHOICES[] = {1.f, 2.f, 4.f, 8.f, 16.f, 24.f, 32.f};
	std::atomic<int> pitchZero{60};
	std::atomic<float> outputHigh{10.f};
	// For the display (written by process()).
	std::atomic<float> shownBeat{0.f};
	std::atomic<bool> shownMoving{false}, shownV3{false};
	std::atomic<int> shownPattern{0}, shownRow{0}, shownBar{16};   // version 3: pattern, row and rows per bar

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
		configInput(BUS_INPUT, "BUS IN: lane mutes on bits 0–5 and FILL on bit 6, high for on (byte bus; version-3 scores)");
		for (int f = 0; f < gorgon::v3::BUS_FUNCTIONS; f++)
			busBits[f] = gorgon::v3::BusMap().bit[f];
		configOutput(CLOCK_OUTPUT, "Clock");
		configOutput(RESET_OUTPUT, "Reset (5 ms trigger at the start of the score or loop)");
		configOutput(RUN_OUTPUT, "Run (high while Gorgon plays)");
		configOutput(VEL_OUTPUT, "Velocity, lanes 1–6 (polyphonic, held; 4 channels for a version-1/2 score)");
		for (int i = 0; i < gorgon::AUDIO_LANES; i++)
			configOutput(GATE1_OUTPUT + i, string::f("Gate %d", i + 1));
		configOutput(GATE5_OUTPUT, "Gate 5 (version-3 scores)");
		configOutput(GATE6_OUTPUT, "Gate 6 (version-3 scores)");
		configOutput(Z_OUTPUT, "Dolly Z lane (0 to selected CV level)");
		configOutput(ROLL_OUTPUT, "Roll lane (±5 V for ±45°)");
		configOutput(FEEDBACK_OUTPUT, "Feedback lane (0 to selected CV level for 0–0.9)");
		configOutput(GATES_OUTPUT, "Gates 1–6 (polyphonic; 4 channels for a version-1/2 score)");
		configOutput(PITCH_OUTPUT, "Pitch, lanes 1–6 (polyphonic, held, 1 V/oct; 4 channels for a version-1/2 score)");
		configOutput(OPTICAL_L_OUTPUT, "Optical audio left (audio device input 1)");
		configOutput(OPTICAL_R_OUTPUT, "Optical audio right (audio device input 2)");
		configOutput(BREAKS_L_OUTPUT, "Breaks audio left (audio device input 3)");
		configOutput(BREAKS_R_OUTPUT, "Breaks audio right (audio device input 4)");
		configOutput(BUS_OUTPUT, "BUS OUT: bits 0–7, channel 9 word clock (0/10 V)");
		configLight(LINK_LIGHT, "Linked to Studio");
		engine3.trace = &traceRing;
		configLight(CLOCK_LIGHT, "Clock");
		bridge.start();
	}

	~Gorgon() override { audioInput.setDriverId(-1); bridge.stop(); }

	// The sound-link receivers this module writes to (spec/sound-locks.md): Machinemodule (the
	// Machinedrum) and Monomodule (the Monomachine), version 2.x.
	static bool soundReceiver(const Module* m) {
		if (!m || !m->model || !m->model->plugin || m->model->plugin->version.rfind("2.", 0) != 0)
			return false;
		const std::string& plugin = m->model->plugin->slug;
		const std::string& model = m->model->slug;
		return (plugin == "MachinemoduleRack" && model == "Machinemodule") || (plugin == "MonomoduleRack" && model == "Monomodule");
	}

	// The sound link to an adjacent receiver (spec/sound-locks.md): one packet per sample, in
	// whichever version the receiver's buffer declares.
	template <class P>
	void sendSound(P& packet, int64_t frame, bool moving, const gorgon::Outputs& o, bool v3, int pattern, int row) {
		packet = P();
		packet.frame = frame;
		packet.transport = snapshot.playhead.transportRevision;
		packet.moving = moving;
		packet.reset = o.reset;
		constexpr int lanes = int(sizeof(packet.lanes) / sizeof(packet.lanes[0]));
		for (int lane = 0; lane < lanes; ++lane) {
			if (v3)
				packet.lanes[lane].sound = engine3.sound(*snapshot.score3, lane);
			else if (lane < gorgon::AUDIO_LANES)
				packet.lanes[lane].sound = gorgon::soundAt(snapshot.score, pattern, lane, row);
			packet.lanes[lane].gate = o.gate[lane];
			packet.lanes[lane].velocity = o.velocity[lane];
			packet.lanes[lane].pitch = o.linkPitch[lane];
		}
	}

	void process(const ProcessArgs& args) override {
		bool tick = args.frame % 32 == 0;
		if (tick && bridge.poll(seen, snapshot))
			haveSnapshot = true;
		bool v3 = haveSnapshot && snapshot.v3();
		// Correct at once on a change of format, so the new follower never plays a stale place.
		if ((tick || v3 != wasV3) && haveSnapshot && snapshot.clock.ready()) {
			// Studio's beat or ticks for this sample, on Studio's clock.
			double local = APP->engine->getBlockTime() + double(args.frame - APP->engine->getBlockFrame()) * args.sampleTime;
			double offset = params[OFFSET_PARAM].getValue() / 1000.;
			double server = snapshot.clock.toServer(local + offset);
			const gorgon::Playhead& h = snapshot.playhead;
			if (v3)
				tickFollower.correct(snapshot.score3->bpm, h.moving(), gorgon::v3::ticksAt(h, server, snapshot.score3->bpm),
				                     h.transportRevision, h.startTick);
			else
				follower.correct(snapshot.score, h.moving(), h.beatAt(snapshot.score, server), h.transportRevision);
		}
		if (tick) {
			for (int i = 0; i < gorgon::INPUTS; i++)
				bridge.setInput(i, inputs[CV1_INPUT + i].isConnected(), inputs[CV1_INPUT + i].getVoltage());
			options.ppqn = ppqn.load(std::memory_order_relaxed);
			options.gate = gateLength.load(std::memory_order_relaxed);
			options.high = outputHigh.load(std::memory_order_relaxed);
			options.pitchZero = pitchZero.load(std::memory_order_relaxed);
			for (int f = 0; f < gorgon::v3::BUS_FUNCTIONS; f++)
				busMap.bit[f] = int8_t(busBits[f].load(std::memory_order_relaxed));
		}
		Input& busInput = inputs[BUS_INPUT];
		int busWord = busReader.process(busInput.isConnected() ? busInput.getChannels() : 0,
		                                [&](int c) { return busInput.getVoltage(c); });
		shownBusIn.store(busWord, std::memory_order_relaxed);

		bool moving = haveSnapshot && snapshot.playhead.moving() && (v3 ? tickFollower.locked : follower.locked);
		gorgon::Outputs o;
		int pattern = 0, row = 0;
		if (v3) {
			const gorgon::v3::ScoreV3& score = *snapshot.score3;
			const gorgon::Playhead& h = snapshot.playhead;
			if (!wasV3 && rendered)
				engine3.continueFrom(outputState);   // the score changed format: one transport state
			// BUS IN is read before the sample is evaluated, and a change is stamped with the
			// first tick not yet evaluated, so it applies to this sample at once (V11).
			int64_t first = engine3.firstUnevaluated(h.transportRevision, h.startTick, tickFollower.u);
			gorgon::v3::BusChange changes[gorgon::v3::BUS_FUNCTIONS + 2];
			int n = busIn.process(busWord, busMap, score, h.transportRevision, h.startTick, h.startEntry, first, changes);
			bridge.pushBus(changes, n, busIn.state());
			for (int k = 0; k < n; k++) {
				const gorgon::v3::BusChange& c = changes[k];
				if (c.kind == gorgon::v3::BusChange::CHANGE)
					traceRing.write(gorgon::v3::busRecord(c.function, c.at, c.on, std::max<int64_t>(first, 0), c.revision, c.tick, c.entry,
					                                      args.frame));
			}
			busIn.prune(std::max<int64_t>(first, 0), gorgon::v3::LONGEST_ROW);
			bridge.setBusFrontier(std::max<int64_t>(first, 0));
			gorgon::v3::TransportV3 transport = snapshot.layer->transport(h.startTick);
			// The module that holds the layer plays its own record; others play Studio's.
			if ((busIn.connected || bridge.busOutstanding()) && !bridge.busRefused())
				transport = busIn.overlay(transport);
			engine3.frame = args.frame;
			o = engine3.render(score, transport, moving, tickFollower.u, h.transportRevision, options, args.sampleRate);
			if (tick) {
				gorgon::v3::Position p = gorgon::v3::position(score, snapshot.playhead.startTick, tickFollower.u);
				int shown = score.entries[size_t(p.entry)];
				gorgon::v3::Geometry g = score.geometry(shown);
				shownPattern.store(shown, std::memory_order_relaxed);
				shownRow.store(std::min(int(p.rowPos), g.rows - 1), std::memory_order_relaxed);
				shownBar.store(g.bar, std::memory_order_relaxed);
				shownBeat.store(float(p.tick / double(gorgon::v3::TICKS)), std::memory_order_relaxed);
			}
		}
		else {
			if (wasV3 && rendered)
				engine3.continueInto(outputState);
			o = outputState.render(snapshot.score, moving, follower.beat, snapshot.playhead.transportRevision, options,
			                       args.sampleTime, snapshot.playhead.anchorBeat);
			pattern = std::clamp(int(follower.beat / gorgon::BEATS_PER_PATTERN), 0, gorgon::PATTERNS - 1);
			row = std::clamp(int(std::fmod(std::max(0., follower.beat), double(gorgon::BEATS_PER_PATTERN)) * 4.), 0, gorgon::ROWS - 1);
			shownBeat.store(float(follower.beat), std::memory_order_relaxed);
		}
		wasV3 = v3;
		rendered = true;
		shownV3.store(v3, std::memory_order_relaxed);
		// Receiver owns both buffers. Gates and resolved controls share this
		// flip: patch order cannot pair a new trigger with the previous lock.
		Module* receiver = rightExpander.module;
		if (soundReceiver(receiver) && receiver->leftExpander.producerMessage) {
			void* message = receiver->leftExpander.producerMessage;
			gorgon_sound::Header header;
			std::memcpy(static_cast<void*>(&header), message, sizeof header);
			bool sent = true;
			if (header.magic == gorgon_sound::MAGIC && header.version == gorgon_sound::VERSION && header.bytes == sizeof(gorgon_sound::Packet))
				sendSound(*static_cast<gorgon_sound::Packet*>(message), args.frame, moving, o, v3, pattern, row);
			else if (header.magic == gorgon_sound::MAGIC && header.version == gorgon_sound::v1::VERSION &&
			         header.bytes == sizeof(gorgon_sound::v1::Packet))
				sendSound(*static_cast<gorgon_sound::v1::Packet*>(message), args.frame, moving, o, v3, pattern, row);
			else
				sent = false;
			if (sent)
				receiver->leftExpander.messageFlipRequested = true;
		}
		if (v3)
			tickFollower.advance(snapshot.score3->bpm, moving, args.sampleTime);
		else
			follower.advance(snapshot.score, moving, args.sampleTime);

		outputs[CLOCK_OUTPUT].setVoltage(o.clock);
		outputs[RESET_OUTPUT].setVoltage(o.reset);
		outputs[RUN_OUTPUT].setVoltage(o.run);
		int lanes = v3 ? gorgon::OUTPUT_LANES : gorgon::AUDIO_LANES;
		outputs[VEL_OUTPUT].setChannels(lanes);
		outputs[GATES_OUTPUT].setChannels(lanes);
		outputs[PITCH_OUTPUT].setChannels(lanes);
		for (int l = 0; l < lanes; l++) {
			outputs[GATES_OUTPUT].setVoltage(o.gate[l], l);
			outputs[VEL_OUTPUT].setVoltage(o.velocity[l] * options.high, l);
			outputs[PITCH_OUTPUT].setVoltage(o.pitch[l], l);
		}
		for (int l = 0; l < gorgon::AUDIO_LANES; l++)
			outputs[GATE1_OUTPUT + l].setVoltage(o.gate[l]);
		outputs[GATE5_OUTPUT].setVoltage(o.gate[4]);
		outputs[GATE6_OUTPUT].setVoltage(o.gate[5]);
		outputs[Z_OUTPUT].setVoltage(gorgon::laneVolts(gorgon::LANE_Z, o.lane[gorgon::LANE_Z]) * options.high / 10.f);
		outputs[ROLL_OUTPUT].setVoltage(gorgon::laneVolts(gorgon::LANE_ROLL, o.lane[gorgon::LANE_ROLL]));
		outputs[FEEDBACK_OUTPUT].setVoltage(gorgon::laneVolts(gorgon::LANE_FEEDBACK, o.lane[gorgon::LANE_FEEDBACK]) * options.high / 10.f);
		auto bus = busOutput.render(o, moving, snapshot.playhead.transportRevision,
		                            gorgon::busSourceFromInt(busSource.load(std::memory_order_relaxed)));
		bus.write(outputs[BUS_OUTPUT]);
		shownBusWord.store(bus.word, std::memory_order_relaxed);
		gorgon::AudioFrame audio;
		bool received = audioInput.read(audio);
		for (int c = 0; c < 4; ++c) outputs[OPTICAL_L_OUTPUT + c].setVoltage(received ? audio.samples[c] * 10.f : 0.f);

		if (playButton.process(params[PLAY_PARAM].getValue() > 0.f) |
		    playTrigger.process(inputs[PLAY_INPUT].getVoltage(), .1f, 1.f))
			bridge.requestPlayToggle();
		if (resetButton.process(params[RESET_PARAM].getValue() > 0.f) |
		    resetTrigger.process(inputs[RESET_INPUT].getVoltage(), .1f, 1.f))
			bridge.requestRestart();

		lights[PLAY_LIGHT].setBrightness(moving ? 1.f : 0.f);
		lights[LINK_LIGHT].setBrightness(bridge.linked() ? 1.f : 0.f);
		lights[CLOCK_LIGHT].setBrightnessSmooth(o.clock > 0.f ? 1.f : 0.f, args.sampleTime);
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
		json_object_set_new(root, "outputHigh", json_real(outputHigh.load()));
		json_object_set_new(root, "audio", audioInput.toJson());
		json_object_set_new(root, "pitchZero", json_integer(pitchZero.load()));
		json_object_set_new(root, "busSource", json_integer(busSource.load()));
		json_t* bits = json_array();
		for (int f = 0; f < gorgon::v3::BUS_FUNCTIONS; f++)
			json_array_append_new(bits, json_integer(busBits[f].load()));
		json_object_set_new(root, "busIn", bits);   // FILL, then mutes 1–6: a bit 0–7, or -1
		return root;
	}

	void dataFromJson(json_t* root) override {
		if (json_t* a = json_object_get(root, "audio")) audioInput.fromJson(a);
		if (json_t* h = json_object_get(root, "outputHigh")) outputHigh = json_number_value(h) == 5. ? 5.f : 10.f;
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
		json_t* source = json_object_get(root, "busSource");
		json_int_t selected = json_is_integer(source) ? json_integer_value(source) : -1;
		busSource = selected >= 0 && selected <= int(gorgon::BusSource::HitMask) ? int(selected) : int(gorgon::BusSource::Z);
		busOutput = gorgon::ByteBusOutput();
		shownBusWord = 0;
		json_t* bits = json_object_get(root, "busIn");
		for (int f = 0; f < gorgon::v3::BUS_FUNCTIONS; f++) {
			json_t* b = json_array_get(bits, size_t(f));
			json_int_t bit = json_is_integer(b) ? json_integer_value(b) : -2;
			busBits[f] = bit >= -1 && bit <= 7 ? int(bit) : gorgon::v3::BusMap().bit[f];
		}
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
			std::string place;
			if (module->shownV3.load()) {
				// As the editor shows it: the pattern's code, then bar:row within the pattern.
				int pattern = module->shownPattern.load(), row = module->shownRow.load(), bar = std::max(1, module->shownBar.load());
				place = string::f("%c%02d %03d:%02d", 'A' + pattern / 16, pattern % 16 + 1, row / bar + 1, row % bar + 1);
			}
			else {
				int pattern = int(beat / gorgon::BEATS_PER_PATTERN) + 1;
				int row = int(std::fmod(beat, float(gorgon::BEATS_PER_PATTERN)) * 4.f);
				place = string::f("P%d row %02d", pattern, row);
			}
			position = module->shownMoving.load() ? string::f("%s  beat %.1f", place.c_str(), beat) : place + "  stopped";
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
		addInput(createInputCentered<PJ301MPort>(Vec(L::JACK_BUS_IN_X, L::JACK_BUS_IN_Y), module, Gorgon::BUS_INPUT));

		struct Jack { float x, y; int id; };
		const Jack outs[] = {
			{L::JACK_CLOCK_X, L::JACK_CLOCK_Y, Gorgon::CLOCK_OUTPUT}, {L::JACK_RESET_OUT_X, L::JACK_RESET_OUT_Y, Gorgon::RESET_OUTPUT},
			{L::JACK_RUN_X, L::JACK_RUN_Y, Gorgon::RUN_OUTPUT}, {L::JACK_VEL_X, L::JACK_VEL_Y, Gorgon::VEL_OUTPUT},
			{L::JACK_PITCH_X, L::JACK_PITCH_Y, Gorgon::PITCH_OUTPUT},
			{L::JACK_GATE1_X, L::JACK_GATE1_Y, Gorgon::GATE1_OUTPUT}, {L::JACK_GATE2_X, L::JACK_GATE2_Y, Gorgon::GATE2_OUTPUT},
			{L::JACK_GATE3_X, L::JACK_GATE3_Y, Gorgon::GATE3_OUTPUT}, {L::JACK_GATE4_X, L::JACK_GATE4_Y, Gorgon::GATE4_OUTPUT},
			{L::JACK_GATE5_X, L::JACK_GATE5_Y, Gorgon::GATE5_OUTPUT}, {L::JACK_GATE6_X, L::JACK_GATE6_Y, Gorgon::GATE6_OUTPUT},
			{L::JACK_Z_X, L::JACK_Z_Y, Gorgon::Z_OUTPUT}, {L::JACK_ROLL_X, L::JACK_ROLL_Y, Gorgon::ROLL_OUTPUT},
			{L::JACK_FEEDBACK_X, L::JACK_FEEDBACK_Y, Gorgon::FEEDBACK_OUTPUT}, {L::JACK_GATES_X, L::JACK_GATES_Y, Gorgon::GATES_OUTPUT},
			{L::JACK_OPTICAL_L_X, L::JACK_OPTICAL_L_Y, Gorgon::OPTICAL_L_OUTPUT}, {L::JACK_OPTICAL_R_X, L::JACK_OPTICAL_R_Y, Gorgon::OPTICAL_R_OUTPUT},
			{L::JACK_BREAKS_L_X, L::JACK_BREAKS_L_Y, Gorgon::BREAKS_L_OUTPUT}, {L::JACK_BREAKS_R_X, L::JACK_BREAKS_R_Y, Gorgon::BREAKS_R_OUTPUT},
			{L::JACK_BUS_X, L::JACK_BUS_Y, Gorgon::BUS_OUTPUT},
		};
		for (const Jack& j : outs)
			addOutput(createOutputCentered<DarkPJ301MPort>(Vec(j.x, j.y), module, j.id));
	}

	void appendContextMenu(Menu* menu) override {
		Gorgon* module = getModule<Gorgon>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Studio: " + module->bridge.status()));
		menu->addChild(createMenuLabel("Reads " + module->bridge.advertisementFile()));
		menu->addChild(createMenuLabel("Audio: inputs 1/2 optical, 3/4 breaks"));
		// Rack's driver and device list goes in a submenu: with JACK it lists every client in 4-channel
		// groups, which inline would push the rest of this menu past the bottom of a 1080p screen.
		rack::audio::Port* port = &module->audioInput;
		int device = port->getDeviceId();
		menu->addChild(createSubmenuItem("Audio device",
			port->getDriverName() + ": " + (device >= 0 ? port->getDeviceName(device) : "no device"),
			[=](Menu* sub) { app::appendAudioMenu(sub, port); }));
		menu->addChild(createMenuLabel(string::f("Audio underrun samples: %llu; backlog drops: %llu; overruns: %llu",
			(unsigned long long)module->audioInput.underruns.load(), (unsigned long long)module->audioInput.discarded.load(),
			(unsigned long long)module->audioInput.queue.overruns.load())));
		menu->addChild(createSubmenuItem("CV output levels", string::f("%.0f V", module->outputHigh.load()), [=](Menu* sub) {
			for (float level : {5.f, 10.f}) sub->addChild(createCheckMenuItem(level == 5.f ? "5 V (match direct CV)" : "10 V (Rack default)", "",
				[=]{return module->outputHigh.load() == level;}, [=]{module->outputHigh = level;}));
		}));
		bool v3 = module->shownV3.load();
		menu->addChild(createSubmenuItem("BUS OUT source",
			string::f("%02X · %s", module->shownBusWord.load(),
				gorgon::busSourceName(gorgon::busSourceFromInt(module->busSource.load()), v3)), [=](Menu* sub) {
			for (int source = 0; source <= int(gorgon::BusSource::HitMask); ++source)
				sub->addChild(createCheckMenuItem(gorgon::busSourceName(gorgon::BusSource(source), v3), "",
					[=] { return module->busSource.load() == source; }, [=] { module->busSource = source; }));
		}));

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

		int busIn = module->shownBusIn.load();
		std::string busState = busIn < 0 ? "not patched" : string::f("%02X", busIn);
		if (module->bridge.busRefused())
			busState += " · another module holds the bus";
		uint64_t lost = module->bridge.busLost();
		if (lost)   // the worker fell behind an audio-rate bus; its record was resent from the current state
			busState += string::f(" · %llu changes dropped", (unsigned long long)lost);
		menu->addChild(createSubmenuItem("BUS IN bits", busState, [=](Menu* sub) {
			sub->addChild(createMenuLabel("High is on; a version-3 score only"));
			if (lost)
				sub->addChild(createMenuLabel(string::f("%llu changes dropped while Studio was slow; resent as the current state",
				                                        (unsigned long long)lost)));
			for (int f = 0; f < gorgon::v3::BUS_FUNCTIONS; f++) {
				std::string name = f == gorgon::v3::BUS_FILL ? "FILL" : string::f("Mute lane %d", f);
				int bit = module->busBits[f].load();
				sub->addChild(createSubmenuItem(name, bit < 0 ? "none" : string::f("bit %d", bit), [=](Menu* bits) {
					for (int b = -1; b < 8; b++)
						bits->addChild(createCheckMenuItem(b < 0 ? "None" : string::f("Bit %d (channel %d)", b, b + 1), "",
							[=] { return module->busBits[f].load() == b; }, [=] { module->busBits[f] = b; }));
				}));
			}
			sub->addChild(createMenuItem("Default (mutes on bits 0–5, FILL on 6)", "", [=] {
				for (int f = 0; f < gorgon::v3::BUS_FUNCTIONS; f++)
					module->busBits[f] = gorgon::v3::BusMap().bit[f];
			}));
		}));

		uint64_t traced = module->traceRing.written();
		menu->addChild(createMenuItem("Save event trace…", string::f("%llu events", (unsigned long long)std::min<uint64_t>(traced, 4096)), [=] {
			// The last 4096 candidates and BUS IN changes, as evaluator.md#trace's JSONL.
			std::vector<gorgon::v3::TraceRecord> records = module->traceRing.copy();
			osdialog_filters* filters = osdialog_filters_parse("Event trace:jsonl");
			char* path = osdialog_file(OSDIALOG_SAVE, nullptr, "gorgon-trace.jsonl", filters);
			osdialog_filters_free(filters);
			if (!path)
				return;
			std::ofstream out(path);
			for (const gorgon::v3::TraceRecord& r : records)
				out << gorgon::v3::traceLine(r) << "\n";
			std::free(path);
		}, traced == 0));

		menu->addChild(createSubmenuItem("Clock", string::f("%g PPQN", module->ppqn.load()), [=](Menu* sub) {
			for (float p : Gorgon::PPQN_CHOICES)
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
