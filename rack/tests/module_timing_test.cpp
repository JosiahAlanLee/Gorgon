// Real Module::process() timing and zero-offset regression; no JACK or display.
// --benchmark [version [offset-count [repeats]]] reports thread CPU ns/sample.
#include <engine/Engine.hpp>
#include <context.hpp>
#include <cassert>
#include <ctime>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>
#ifndef GORGON_MODULE_SOURCE
#define GORGON_MODULE_SOURCE "../src/Gorgon.cpp"
#endif
#include GORGON_MODULE_SOURCE
Plugin* pluginInstance = nullptr;

struct TimingRig {
	std::unique_ptr<Gorgon> module = std::make_unique<Gorgon>();
	plugin::Plugin plugin;
	plugin::Model model;
	Module receiver;
	gorgon_sound::Packet packet;
	Module::ProcessArgs args;

	TimingRig(bool v3) {
		auto& m = *module;
		for (auto& out : m.outputs) out.channels = 1;
		plugin.slug = "MachinemoduleRack"; plugin.version = "2.2.0";
		model.slug = "Machinemodule"; model.plugin = &plugin;
		receiver.model = &model;
		receiver.leftExpander.producerMessage = &packet;
		m.rightExpander.module = &receiver;
		auto score = std::make_shared<gorgon::v3::ScoreV3>();
		score->bpm = m.snapshot.score.bpm = 120.;
		score->loop = true;
		gorgon::v3::PatternV3 p;
		p.geometry.w = 480; p.geometry.rows = p.geometry.bar = 16; p.geometry.bars = 1;
		for (int l = 0; l < 6; ++l) {
			p.lanes[l].len = 16;
			p.lanes[l].cells.resize(16);
			for (int r = 0; r < 16; r += 2) {
				auto& cell = p.lanes[l].cells[r];
				cell.present = true; cell.vel = 63.5 + (r % 4 ? 0 : 63.5);
				cell.midi = r == 4 ? -1 : 69 + l + r;
				std::memcpy(cell.note, r == 4 ? "AIR" : "A-4", 4);
				cell.hasLocks = true; cell.locks.machine = r + l;
				cell.locks.mask = 1; cell.locks.values[0] = float(2 * r + l);
				if (l < 4) {
					auto& legacy = m.snapshot.score.patterns[0];
					legacy.hit[l][r] = true; legacy.velocity[l][r] = cell.vel;
					legacy.pitched[l][r] = cell.midi >= 0; legacy.midi[l][r] = cell.midi;
					legacy.locks[l][r] = cell.locks;
				}
			}
		}
		score->store.push_back(p); score->index[0] = 0; score->buildTimeline();
		m.snapshot.score3 = score;
		m.snapshot.layer = std::make_shared<gorgon::v3::Layer>();
		m.haveSnapshot = true;
		auto& h = m.snapshot.playhead;
		h.valid = h.enabled = h.playing = true; h.v3 = v3; h.transportRevision = 1;
		m.follower.locked = m.tickFollower.locked = true;
		args.sampleRate = 48000.f; args.sampleTime = 1.f / args.sampleRate; args.frame = 0;
	}
	~TimingRig() { module->rightExpander.module = nullptr; model.plugin = nullptr; }
	void step() { module->process(args); ++args.frame; }
};

static void sameFloat(float actual, float expected) {
	assert(std::memcmp(&actual, &expected, sizeof(float)) == 0);
}

// Compare the real ports and the whole sound packet byte-for-byte with an independent
// main renderer, including holds, AIR, pause, resume, seeks and offset deactivation.
static void identity(bool v3) {
	TimingRig rig(v3);
	auto& m = *rig.module;
	gorgon::v3::EngineV3 main3;
	gorgon::OutputState legacy;
	gorgon::ByteBusOutput bus;
	for (int i = 0; i < 120000; ++i) {
		if (i == 30000) m.snapshot.playhead.playing = false;
		if (i == 32000) m.snapshot.playhead.playing = true;
		if (i == 45000) ++m.snapshot.playhead.transportRevision;
		// Only lane 1 is ever offset. Its return to zero must use the independent main,
		// never carry shifted state into the other lanes or into the main renderer.
		if (i == 60000 || i == 80000) m.laneOffsetMs[0] = 5.f;
		if (i == 64000 || i == 84000) m.laneOffsetMs[0] = 0.f;
		double u = m.tickFollower.u, beat = m.follower.beat;
		int64_t frame = rig.args.frame;
		bool moving = m.snapshot.playhead.moving();
		rig.step();
		gorgon::Outputs expected;
		if (v3) expected = main3.render(*m.snapshot.score3, m.snapshot.layer->transport(0), moving, u,
		                                m.snapshot.playhead.transportRevision, m.options, rig.args.sampleRate);
		else expected = legacy.render(m.snapshot.score, moving, beat, m.snapshot.playhead.transportRevision,
		                              m.options, rig.args.sampleTime, m.snapshot.playhead.anchorBeat);
		auto word = bus.render(expected, moving, m.snapshot.playhead.transportRevision, gorgon::BusSource::Z);
		if (m.laneOffsetMs[0].load() != 0.f) {
			if (v3) assert(m.laneEngine[0].trace == nullptr);
			continue;
		}
		const int lanes = v3 ? 6 : 4;
		assert(m.outputs[Gorgon::GATES_OUTPUT].getChannels() == lanes);
		assert(m.outputs[Gorgon::VEL_OUTPUT].getChannels() == lanes);
		assert(m.outputs[Gorgon::PITCH_OUTPUT].getChannels() == lanes);
		assert(m.outputs[Gorgon::BUS_OUTPUT].getChannels() == 9);
		for (int c = 0; c < 4; ++c) sameFloat(m.outputs[Gorgon::OPTICAL_L_OUTPUT + c].getVoltage(), 0.f);
		if (!v3) {
			sameFloat(m.outputs[Gorgon::GATE5_OUTPUT].getVoltage(), 0.f);
			sameFloat(m.outputs[Gorgon::GATE6_OUTPUT].getVoltage(), 0.f);
		}
		float gates[6]{}, velocities[6]{}, pitches[6]{};
		gorgon_sound::Packet packet;
		packet.frame = frame; packet.transport = m.snapshot.playhead.transportRevision;
		packet.moving = moving; packet.reset = expected.reset;
		int pattern = std::clamp(int(beat / gorgon::BEATS_PER_PATTERN), 0, gorgon::PATTERNS - 1);
		int row = std::clamp(int(std::fmod(std::max(0., beat), double(gorgon::BEATS_PER_PATTERN)) * 4.), 0, gorgon::ROWS - 1);
		for (int l = 0; l < lanes; ++l) {
			gates[l] = expected.gate[l]; velocities[l] = expected.velocity[l] * m.options.high; pitches[l] = expected.pitch[l];
			packet.lanes[l].sound = v3 ? main3.sound(*m.snapshot.score3, l) : gorgon::soundAt(m.snapshot.score, pattern, l, row);
			packet.lanes[l].gate = expected.gate[l]; packet.lanes[l].velocity = expected.velocity[l];
			packet.lanes[l].pitch = expected.linkPitch[l];
			int port = l < 4 ? Gorgon::GATE1_OUTPUT + l : Gorgon::GATE5_OUTPUT + l - 4;
			sameFloat(m.outputs[port].getVoltage(), gates[l]);
		}
		assert(std::memcmp(m.outputs[Gorgon::GATES_OUTPUT].getVoltages(), gates, lanes * sizeof(float)) == 0);
		assert(std::memcmp(m.outputs[Gorgon::VEL_OUTPUT].getVoltages(), velocities, lanes * sizeof(float)) == 0);
		assert(std::memcmp(m.outputs[Gorgon::PITCH_OUTPUT].getVoltages(), pitches, lanes * sizeof(float)) == 0);
		assert(std::memcmp(&rig.packet, &packet, sizeof packet) == 0);
		sameFloat(m.outputs[Gorgon::CLOCK_OUTPUT].getVoltage(), expected.clock);
		sameFloat(m.outputs[Gorgon::RESET_OUTPUT].getVoltage(), expected.reset);
		sameFloat(m.outputs[Gorgon::RUN_OUTPUT].getVoltage(), expected.run);
		for (int bit = 0; bit < 8; ++bit)
			sameFloat(m.outputs[Gorgon::BUS_OUTPUT].getVoltage(bit), (word.word & (1u << bit)) ? 10.f : 0.f);
		sameFloat(m.outputs[Gorgon::BUS_OUTPUT].getVoltage(8), word.strobe ? 10.f : 0.f);
		sameFloat(m.outputs[Gorgon::Z_OUTPUT].getVoltage(), gorgon::laneVolts(gorgon::LANE_Z, expected.lane[gorgon::LANE_Z]) * m.options.high / 10.f);
		sameFloat(m.outputs[Gorgon::ROLL_OUTPUT].getVoltage(), gorgon::laneVolts(gorgon::LANE_ROLL, expected.lane[gorgon::LANE_ROLL]));
		sameFloat(m.outputs[Gorgon::FEEDBACK_OUTPUT].getVoltage(), gorgon::laneVolts(gorgon::LANE_FEEDBACK, expected.lane[gorgon::LANE_FEEDBACK]) * m.options.high / 10.f);
		for (int l = i < 60000 ? 0 : 1; l < lanes; ++l) {
			assert(!m.laneEngine[l].heard);
			assert(m.laneLegacy[l].transportRevision == -1);
		}
	}
}

static void continuity(bool v3) {
	for (int lane = 0; lane < (v3 ? 6 : 4); ++lane) for (float ms : {-5.f, 5.f}) {
		TimingRig rig(v3);
		auto& m = *rig.module;
		// Toggle twice inside a sustained gate: no fabricated 1 ms retrigger/reset.
		for (int i = 0; i < 2600; ++i) {
			if (i == 960 || i == 1920) m.laneOffsetMs[lane] = ms;
			if (i == 1440 || i == 2400) m.laneOffsetMs[lane] = 0.f;
			rig.step();
			assert(m.outputs[(lane < 4 ? Gorgon::GATE1_OUTPUT + lane : Gorgon::GATE5_OUTPUT + lane - 4)].getVoltage() == 10.f);
			sameFloat(m.outputs[Gorgon::PITCH_OUTPUT].getVoltage(lane), float(9 + lane) / 12.f);
		}
		// An empty row and an AIR hit must retain the pitch already heard on main.
		for (int target : {8000, 26000}) {
			while (rig.args.frame < target) rig.step();
			float pitch = m.outputs[Gorgon::PITCH_OUTPUT].getVoltage(lane);
			float velocity = m.outputs[Gorgon::VEL_OUTPUT].getVoltage(lane);
			for (int k = 0; k < 4; ++k) {
				m.laneOffsetMs[lane] = k % 2 ? 0.f : ms;
				rig.step();
				sameFloat(m.outputs[Gorgon::PITCH_OUTPUT].getVoltage(lane), pitch);
				sameFloat(m.outputs[Gorgon::VEL_OUTPUT].getVoltage(lane), velocity);
			}
		}
	}
}

static void formatContinuity() {
	for (int lane = 0; lane < 6; ++lane) {
		TimingRig rig(true);
		auto& m = *rig.module;
		m.laneOffsetMs[lane] = 5.f;
		for (int i = 0; i < 2600; ++i) {
			if (i == 960) {
				m.follower.beat = m.tickFollower.u / gorgon::v3::TICKS;
				m.snapshot.playhead.v3 = false;
			}
			if (i == 1920) {
				m.tickFollower.u = m.follower.beat * gorgon::v3::TICKS;
				m.snapshot.playhead.v3 = true;
			}
			if (i == 2400) m.laneOffsetMs[lane] = 0.f;
			rig.step();
			if (i >= 300 && (lane < 4 || m.snapshot.playhead.v3)) {
				int port = lane < 4 ? Gorgon::GATE1_OUTPUT + lane : Gorgon::GATE5_OUTPUT + lane - 4;
				assert(m.outputs[port].getVoltage() == 10.f);
				sameFloat(m.outputs[Gorgon::PITCH_OUTPUT].getVoltage(lane), float(9 + lane) / 12.f);
			}
		}
	}
}

// At a new note, a positive offset must seed the PRE-render held pitch, not leak
// main's just-heard note early into a delayed lane while the preceding row is empty.
static void activationAtOnset(bool v3) {
	TimingRig rig(v3);
	auto& m = *rig.module;
	while ((v3 ? m.tickFollower.u / gorgon::v3::TICKS : m.follower.beat) < .5) rig.step();
	float held = m.outputs[Gorgon::PITCH_OUTPUT].getVoltage(0);
	m.laneOffsetMs[0] = 5.f;
	for (int i = 0; i < 200; ++i) {
		rig.step();
		sameFloat(m.outputs[Gorgon::PITCH_OUTPUT].getVoltage(0), held);
		assert(m.outputs[Gorgon::GATE1_OUTPUT].getVoltage() == 0.f);
	}
	for (int i = 0; i < 100; ++i) rig.step();
	sameFloat(m.outputs[Gorgon::PITCH_OUTPUT].getVoltage(0), 11.f / 12.f);
	assert(m.outputs[Gorgon::GATE1_OUTPUT].getVoltage() == 10.f);
}

// All six private renderers use the same explicit spans and retrigger gap as
// main, evaluated at each lane's signed timing offset. Check real ports and
// the sound packet against the score's analytic gate/held-value windows.
static void explicitLengthOffsets() {
	TimingRig rig(true);
	auto& m = *rig.module;
	auto score = std::make_shared<gorgon::v3::ScoreV3>(*m.snapshot.score3);
	for (auto& lane : score->store[0].lanes) {
		lane.cells.assign(16, gorgon::v3::Cell());
		for (int row : {0, 3}) {
			auto& cell = lane.cells[row];
			cell.present = cell.hasLength = true;
			cell.length = row == 0 ? 3. : 1.;
			cell.midi = row == 0 ? 60 : 62;
			cell.vel = row == 0 ? 100 : 64;
		}
	}
	m.snapshot.score3 = score;
	m.gateLength = .1f; // explicit spans must bypass this fraction in every lane
	const float offsets[] = {-25.f, 0.f, 25.f, 100.f, -100.f, 12.5f};
	for (int l = 0; l < 6; ++l) m.laneOffsetMs[l] = offsets[l];
	for (int i = 0; i < 30000; ++i) {
		double mainU = m.tickFollower.u;
		rig.step();
		for (int l = 0; l < 6; ++l) {
			double u = gorgon::lanePosition(mainU, 120., gorgon::v3::TICKS, offsets[l]);
			bool gate = (u >= 0. && u < 1440.) || (u >= 1440. + 3.84 && u < 1920.);
			float high = gate ? m.options.high : 0.f;
			float vel = u < 0. ? 0.f : float((u < 1440. ? 100. : 64.) / 127.);
			float pitch = u < 1440. ? 0.f : 2.f / 12.f;
			sameFloat(m.outputs[Gorgon::GATES_OUTPUT].getVoltage(l), high);
			sameFloat(m.outputs[Gorgon::VEL_OUTPUT].getVoltage(l), vel * m.options.high);
			sameFloat(m.outputs[Gorgon::PITCH_OUTPUT].getVoltage(l), pitch);
			sameFloat(rig.packet.lanes[l].gate, high);
			if (l < 4) sameFloat(m.outputs[Gorgon::GATE1_OUTPUT + l].getVoltage(), high);
			else sameFloat(m.outputs[l == 4 ? Gorgon::GATE5_OUTPUT : Gorgon::GATE6_OUTPUT].getVoltage(), high);
		}
	}
}

// Activating a delayed lane after main's note ended must recover the length
// under FILL as it was at onset, even though BUS IN went low long ago.
static void delayedHeldFill() {
	TimingRig rig(true);
	auto& m = *rig.module;
	auto score = std::make_shared<gorgon::v3::ScoreV3>(*m.snapshot.score3);
	for (auto& lane : score->store[0].lanes) lane.cells.assign(16, gorgon::v3::Cell());
	auto& hit = score->store[0].lanes[0].cells[0];
	hit.present = hit.hasLength = true;
	hit.length = 16.; hit.midi = 60; hit.vel = 100;
	hit.cond.kind = gorgon::v3::COND_FILL;
	m.snapshot.score3 = score;
	m.inputs[Gorgon::BUS_INPUT].channels = 8;
	m.inputs[Gorgon::BUS_INPUT].setVoltage(10.f, 6);
	for (int i = 0; i < 98000; ++i) {
		if (i == 1) m.inputs[Gorgon::BUS_INPUT].setVoltage(0.f, 6);
		rig.step();
	}
	assert(m.outputs[Gorgon::GATE1_OUTPUT].getVoltage() == 0.f);
	m.laneOffsetMs[0] = 100.f;
	rig.step();
	assert(m.outputs[Gorgon::GATE1_OUTPUT].getVoltage() == m.options.high);
	assert(m.laneEngine[0].trace == nullptr);
}

static uint64_t cpuNs() {
	timespec now;
	int result = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
	assert(result == 0);
	return uint64_t(now.tv_sec) * 1000000000 + uint64_t(now.tv_nsec);
}

static void benchmark(int version, int offsets, int repeats) {
	constexpr int samples = 250000;
	for (bool v3 : {false, true}) for (int active : {0, 1, 6}) {
		if ((version && version != (v3 ? 3 : 1)) || (offsets >= 0 && offsets != active)) continue;
		std::vector<double> timings;
		for (int repeat = 0; repeat < repeats; ++repeat) {
			TimingRig rig(v3);
			for (int l = 0; l < active; ++l) rig.module->laneOffsetMs[l] = 5.f * (l + 1);
			for (int i = 0; i < 48000; ++i) rig.step();
			auto start = cpuNs();
			for (int i = 0; i < samples; ++i) rig.step();
			double ns = double(cpuNs() - start) / samples;
			timings.push_back(ns);
		}
		std::sort(timings.begin(), timings.end());
		std::cout << "{\"version\": " << (v3 ? 3 : 1) << ", \"offsets\": " << active
		          << ", \"median_ns\": " << timings[timings.size() / 2] << ", \"min_ns\": " << timings.front()
		          << ", \"max_ns\": " << timings.back() << "}\n";
	}
}

int main(int argc, char** argv) {
	setenv("HYDRAPY_STUDIO_FILE", "/nonexistent/gorgon-timing-test.json", 1);
	Context context; context.engine = new engine::Engine; contextSet(&context);
	if (argc > 1 && std::string(argv[1]) == "--benchmark") benchmark(argc > 2 ? std::stoi(argv[2]) : 0, argc > 3 ? std::stoi(argv[3]) : -1, argc > 4 ? std::max(1, std::stoi(argv[4])) : 7);
	else {
		identity(false); identity(true); continuity(false); continuity(true);
		formatContinuity(); activationAtOnset(false); activationAtOnset(true);
		explicitLengthOffsets(); delayedHeldFill();
		std::cout << "module_timing_test: byte-identical main outputs and offset transitions passed\n";
	}
}
