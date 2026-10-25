// Drives the real Gorgon module through Rack's Module API (no GUI, no Studio):
// a Studio snapshot is injected, then clock, gates, velocity, lanes, polyphony,
// stopping and JSON state are checked on the module's own outputs.
// Host internals first (before rack.hpp marks them private) to provide a Rack context.
#include <engine/Engine.hpp>
#include <context.hpp>
#include <cstdlib>
#include "../src/Gorgon.cpp"
#include <cassert>
#include <iostream>
Plugin* pluginInstance = nullptr;

// Exercise Rack's actual Port/Device dispatch and sample-rate conversion.
struct CaptureDevice : audio::Device {
	float rate = 48000.f;
	int getNumInputs() override { return 4; }
	float getSampleRate() override { return rate; }
	int getBlockSize() override { return 128; }
};
struct CaptureDriver : audio::Driver {
	CaptureDevice device;
	std::vector<int> getDeviceIds() override { return {0}; }
	int getDefaultDeviceId() override { return 0; }
	int getDeviceNumInputs(int) override { return 4; }
	std::string getName() override { return "Test PCM capture"; }
	std::string getDeviceName(int) override { return "Four channels"; }
	audio::Device* subscribe(int, audio::Port* port) override { device.subscribe(port); return &device; }
	void unsubscribe(int, audio::Port* port) override { device.unsubscribe(port); }
};

static void run(Gorgon& m, int n, Module::ProcessArgs& a) {
	for (int i = 0; i < n; i++) {
		m.process(a);
		a.frame++;
	}
}

int main() {
	std::cout << std::unitbuf;
	// No Studio: the bridge looks at a file that doesn't exist.
	setenv("HYDRAPY_STUDIO_FILE", "/nonexistent/gorgon-module-test.json", 1);
	Context context;
	context.engine = new engine::Engine;
	contextSet(&context);
	auto* capture = new CaptureDriver;
	audio::addDriver(10000, capture);
	const float sr = 48000.f;
	Module::ProcessArgs args;
	args.sampleRate = sr;
	args.sampleTime = 1.f / sr;
	args.frame = 0;
	{
		Gorgon m;
		for (auto& o : m.outputs)
			o.channels = 1;
		// Unlinked: silent, no crash.
		run(m, 4800, args);
		assert(m.outputs[Gorgon::CLOCK_OUTPUT].getVoltage() == 0.f && m.outputs[Gorgon::RUN_OUTPUT].getVoltage() == 0.f);
		assert(m.outputs[Gorgon::GATES_OUTPUT].getChannels() == 4 && m.outputs[Gorgon::VEL_OUTPUT].getChannels() == 4);
		assert(m.outputs[Gorgon::PITCH_OUTPUT].getChannels() == 4);

		// Inject what Studio would publish: playing from beat 0 at 120 BPM, with
		// Studio's clock equal to the engine's block clock (which is 0 here).
		gorgon::Snapshot s;
		s.score.bpm = 120.;
		s.score.patterns[0].hit[1][2] = true;           // lane 2, row 2 (beat 0.5)
		s.score.patterns[0].velocity[1][2] = 63.5f;
		s.score.patterns[0].pitched[1][2] = true;
		s.score.patterns[0].midi[1][2] = 69;          // A4 = +0.75 V
		s.score.patterns[0].lanes[gorgon::LANE_ROLL].count = 1;
		s.score.patterns[0].lanes[gorgon::LANE_ROLL].keys[0] = {0, 22.5};
		s.playhead.valid = s.playhead.enabled = s.playhead.playing = true;
		s.playhead.transportRevision = 1;
		s.playhead.anchorBeat = 0.;
		s.playhead.anchorTime = 0.;
		s.playhead.clockTime = 0.;
		s.playhead.wall = 0.;
		s.clock.observe(0., 0., 0.);
		args.frame = 0;
		m.snapshot = s;
		m.haveSnapshot = true;
		m.follower = gorgon::Follower();

		// Sixteenths at 120 BPM: a rising clock edge every 6000 samples.
		std::vector<long> edges;
		float last = 0.f;
		bool gateSeen = false;
		long gateStart = -1;
		for (long i = 0; i < 48000; i++) {
			run(m, 1, args);
			float c = m.outputs[Gorgon::CLOCK_OUTPUT].getVoltage();
			if (c > 1.f && last <= 1.f)
				edges.push_back(i);
			last = c;
			if (!gateSeen && m.outputs[Gorgon::GATE2_OUTPUT].getVoltage() > 1.f) {
				gateSeen = true;
				gateStart = i;
				assert(m.outputs[Gorgon::GATES_OUTPUT].getVoltage(1) == 10.f);
				assert(m.outputs[Gorgon::GATES_OUTPUT].getVoltage(0) == 0.f);
				assert(std::abs(m.outputs[Gorgon::VEL_OUTPUT].getVoltage(1) - 5.f) < 1e-4f);
				assert(std::abs(m.outputs[Gorgon::PITCH_OUTPUT].getVoltage(1) - .75f) < 1e-4f);
			}
		}
		std::cout << "clock edges " << edges.size() << ", first " << edges[0] << ", second " << edges[1]
		          << "; gate 2 opened at " << gateStart << "\n";
		assert(edges.size() == 8);
		for (size_t k = 1; k < edges.size(); k++)
			assert(std::abs((edges[k] - edges[k - 1]) - 6000) <= 1);
		assert(std::abs(gateStart - 12000) <= 32);       // row 2 = beat 0.5 = 0.25 s
		assert(m.outputs[Gorgon::RUN_OUTPUT].getVoltage() == 10.f);
		assert(std::abs(m.outputs[Gorgon::ROLL_OUTPUT].getVoltage() - 2.5f) < 1e-4f);   // 22.5° → +2.5 V
		assert(std::abs(m.outputs[Gorgon::Z_OUTPUT].getVoltage() - 3.5f) < 1e-4f);      // default 0.35 → 3.5 V
		// Velocity holds after the gate closes.
		assert(std::abs(m.outputs[Gorgon::VEL_OUTPUT].getVoltage(1) - 5.f) < 1e-4f);
		assert(std::abs(m.outputs[Gorgon::PITCH_OUTPUT].getVoltage(1) - .75f) < 1e-4f);

		// The 5 V profile changes gate/velocity/unipolar lanes, preserving pitch and roll.
		m.outputHigh = 5.f;
		run(m, 64, args);
		assert(m.outputs[Gorgon::RUN_OUTPUT].getVoltage() == 5.f);
		assert(std::abs(m.outputs[Gorgon::VEL_OUTPUT].getVoltage(1) - 2.5f) < 1e-4f);
		assert(std::abs(m.outputs[Gorgon::Z_OUTPUT].getVoltage() - 1.75f) < 1e-4f);
		assert(std::abs(m.outputs[Gorgon::ROLL_OUTPUT].getVoltage() - 2.5f) < 1e-4f);
		assert(std::abs(m.outputs[Gorgon::PITCH_OUTPUT].getVoltage(1) - .75f) < 1e-4f);

		// Studio stops: outputs fall at the next correction, lanes keep their value.
		m.snapshot.playhead.playing = false;
		run(m, 64, args);
		assert(m.outputs[Gorgon::CLOCK_OUTPUT].getVoltage() == 0.f && m.outputs[Gorgon::RUN_OUTPUT].getVoltage() == 0.f);
		assert(std::abs(m.outputs[Gorgon::ROLL_OUTPUT].getVoltage() - 2.5f) < 1e-4f);
		assert(std::abs(m.outputs[Gorgon::PITCH_OUTPUT].getVoltage(1) - .75f) < 1e-4f);

		// Restart with 20 ms lookahead: don't miss the 5 ms reset window.
		m.params[Gorgon::OFFSET_PARAM].setValue(20.f);
		m.snapshot.playhead.playing = true;
		m.snapshot.playhead.anchorTime = double(args.frame) * args.sampleTime;
		m.snapshot.playhead.anchorBeat = 0.;
		m.snapshot.playhead.transportRevision++;
		int resets = 0;
		for (int i = 0; i < 960; i++) {
			run(m, 1, args);
			resets += m.outputs[Gorgon::RESET_OUTPUT].getVoltage() > 1.f;
		}
		assert(resets == 240);
		assert(m.outputs[Gorgon::PITCH_OUTPUT].getVoltage(1) == 0.f); // old note cleared on restart

		// JSON: bindings and options survive a round trip.
		m.bridge.bind(0, "Warp");
		m.bridge.bind(2, "-");
		m.ppqn = 24.f;
		m.gateLength = .25f;
		m.pitchZero = 48;
		json_t* j = m.dataToJson();
		Gorgon n;
		n.dataFromJson(j);
		json_decref(j);
		assert(n.bridge.binding(0) == "Warp" && n.bridge.binding(1).empty() && n.bridge.binding(2) == "-");
		assert(n.ppqn.load() == 24.f && n.gateLength.load() == .25f);
		assert(n.pitchZero.load() == 48);
		assert(n.outputHigh.load() == 5.f);
		// The appended pitch port must not change existing patch cable IDs.
		assert(Gorgon::GATES_OUTPUT == 11 && Gorgon::PITCH_OUTPUT == 12);
		assert(Gorgon::OPTICAL_L_OUTPUT == 13 && Gorgon::NUM_OUTPUTS == 17);

		// Device PCM -1..1 maps to -10..10 V, independent of the CV level profile.
		settings::sampleRate = sr;
		context.engine->setSampleRate(sr);
		context.engine->addModule(&m);
		m.audioInput.setDriverId(10000);
		m.audioInput.setDeviceId(0);
		capture->device.onStartStream();
		std::array<float, 128 * 4> pcm;
		for (size_t i = 0; i < pcm.size(); i += 4) {
			pcm[i] = .1f; pcm[i + 1] = -.2f; pcm[i + 2] = .3f; pcm[i + 3] = -.4f;
		}
		capture->device.processBuffer(pcm.data(), 4, nullptr, 0, 128);
		for (int c = 0; c < 4; c++)
			assert(std::abs(m.outputs[Gorgon::OPTICAL_L_OUTPUT + c].getVoltage() - pcm[c] * 10.f) < 1e-5f);
		for (float deviceRate : {48000.f, 44100.f, 96000.f}) {
			capture->device.onStopStream();
			capture->device.rate = deviceRate;
			capture->device.onStartStream();
			for (int i = 0; i < 40; i++) capture->device.processBuffer(pcm.data(), 4, nullptr, 0, 128);
			for (int c = 0; c < 4; c++)
				assert(std::abs(m.outputs[Gorgon::OPTICAL_L_OUTPUT + c].getVoltage() - pcm[c] * 10.f) < .01f);
		}
		// A disconnected stream must not repeat the last sample or play a stale queue.
		capture->device.onStopStream();
		run(m, 64, args);
		for (int c = 0; c < 4; c++) assert(m.outputs[Gorgon::OPTICAL_L_OUTPUT + c].getVoltage() == 0.f);
		m.audioInput.setDriverId(-1);
		context.engine->removeModule(&m);
	}
	std::cout << "module_test: all passed\n";
	return 0;
}
