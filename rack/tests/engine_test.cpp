// Portable tests for the Gorgon engine: HydraPy's own outputs (fixture.txt,
// from tools/make_fixture.py), and the follower under clock drift and jitter.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "../src/engine/ByteBusOutput.hpp"
#include "../src/engine/EngineV3.hpp"
#include "../src/engine/GorgonEngine.hpp"

using namespace gorgon;

static int failures = 0;
#define CHECK(cond, ...)                                                                \
	do {                                                                                \
		if (!(cond)) {                                                                  \
			if (failures++ < 20) {                                                      \
				std::printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);             \
				std::printf(__VA_ARGS__);                                               \
				std::printf("\n");                                                      \
			}                                                                           \
		}                                                                               \
	} while (0)

static void fixture(const char* path) {
	std::ifstream in(path);
	CHECK(bool(in), "cannot open %s", path);
	int scores = 0;
	std::string format;
	in >> format >> scores;
	CHECK(format == "GORGON_FIXTURE_2", "regenerate fixture with current HydraPy");
	if (format != "GORGON_FIXTURE_2")
		return;
	long compared = 0;
	for (int s = 0; s < scores; s++) {
		Score score;
		int loop;
		in >> score.bpm >> score.key >> loop >> score.pattern;
		score.loop = loop;
		for (int l = 0; l < AUDIO_LANES; l++) {
			int m;
			in >> m;
			score.mutes[l] = m;
		}
		for (int p = 0; p < PATTERNS; p++) {
			int hits;
			in >> hits;
			for (int h = 0; h < hits; h++) {
				int lane, row, vel, midi;
				in >> lane >> row >> vel >> midi;
				score.patterns[p].hit[lane][row] = true;
				score.patterns[p].velocity[lane][row] = float(vel);
				score.patterns[p].pitched[lane][row] = midi >= 0;
				score.patterns[p].midi[lane][row] = midi;
			}
			for (int k = 0; k < AUTO_LANES; k++) {
				int ease, count;
				in >> ease >> count;
				Lane& lane = score.patterns[p].lanes[k];
				lane.ease = Ease(ease);
				lane.count = count;
				for (int i = 0; i < count; i++)
					in >> lane.keys[i].row >> lane.keys[i].value;
			}
		}
		int beats;
		in >> beats;
		Options o;
		for (int b = 0; b < beats; b++) {
			double beat;
			int moving, clock, reset, run, gate[4];
			int64_t transport;
			double vel[4], lane[3], pitch[4];
			in >> beat >> moving >> transport >> clock >> reset >> run;
			for (int& g : gate)
				in >> g;
			for (double& v : vel)
				in >> v;
			for (double& v : lane)
				in >> v;
			for (double& v : pitch)
				in >> v;
			// Stateless rows, as the generator wrote them: same transport, same
			// moving state, nothing held (latches are tested separately below).
			OutputState memory;
			memory.transportRevision = transport;
			memory.wasMoving = moving;
			Outputs out = memory.render(score, moving, beat, transport, o, 1. / 48000.);
			CHECK((out.clock > 0) == bool(clock), "score %d beat %.17g clock", s, beat);
			CHECK((out.reset > 0) == bool(reset), "score %d beat %.17g reset", s, beat);
			CHECK((out.run > 0) == bool(run), "score %d beat %.17g run", s, beat);
			for (int l = 0; l < AUDIO_LANES; l++) {
				CHECK((out.gate[l] > 0) == bool(gate[l]), "score %d beat %.17g gate %d", s, beat, l);
				double expected = vel[l];
				double got = out.velocity[l];
				CHECK(std::fabs(got - expected) < 1e-6, "score %d beat %.17g vel %d: %g vs %g", s, beat, l, got, expected);
				CHECK(std::fabs(out.pitch[l] - pitch[l]) < 1e-6, "score %d beat %.17g pitch %d: %g vs %g",
				      s, beat, l, out.pitch[l], pitch[l]);
			}
			for (int k = 0; k < AUTO_LANES; k++)
				CHECK(std::fabs(out.lane[k] - lane[k]) < 1e-4 * (LANE_MAX[k] - LANE_MIN[k]),
				      "score %d beat %.17g lane %d: %g vs %g", s, beat, k, out.lane[k], lane[k]);
			compared++;
		}
	}
	std::printf("fixture: %ld beats across %d scores compared with HydraPy\n", compared, scores);
	CHECK(compared > 1000, "fixture too small");
	CHECK(bool(in), "truncated fixture");
}

static void heldOutputsAndTransport() {
	Score score;
	score.bpm = 120.;
	Options options;
	OutputState memory;
	// First packet arrives after the phase reset window, with 20 ms A/V lead.
	int resetSamples = 0;
	for (int i = 0; i < 960; i++) {
		auto out = memory.render(score, true, .04 + 2. * i / 48000., 1, options, 1. / 48000.);
		resetSamples += out.reset > 0;
	}
	CHECK(resetSamples == 240, "full delayed-start reset: %d samples", resetSamples);
	// A fresh seek at the same moving state must produce another complete pulse.
	resetSamples = 0;
	for (int i = 0; i < 960; i++) {
		auto out = memory.render(score, true, 8. + 2. * i / 48000., 2, options, 1. / 48000.);
		resetSamples += out.reset > 0;
	}
	CHECK(resetSamples == 240, "seek reset: %d samples", resetSamples);
	CHECK(memory.render(score, false, 8., 2, options, 1. / 48000.).reset == 0.f, "pause silences reset");
	CHECK(memory.render(score, true, 8., 2, options, 1. / 48000.).reset == 10.f, "resume reset");
	// Loop reset remains phase-based even without a new transport revision.
	CHECK(memory.render(score, true, 63.9, 3, options, 1. / 48000.).reset == 10.f, "seek pulse");
	for (int i = 0; i < 300; i++) memory.render(score, true, 63.9, 3, options, 1. / 48000.);
	CHECK(memory.render(score, true, 0., 3, options, 1. / 48000.).reset == 10.f, "natural loop reset");

	Pattern& p = score.patterns[0];
	p.hit[1][0] = p.hit[1][1] = p.hit[1][2] = p.hit[1][3] = true;
	p.pitched[1][0] = p.pitched[1][1] = p.pitched[1][3] = true;
	p.midi[1][0] = 60; p.midi[1][1] = 72; p.midi[1][3] = 74;
	p.velocity[1][0] = p.velocity[1][1] = p.velocity[1][2] = 100;
	p.velocity[1][3] = 0;
	score.key = 7; options.pitchZero = 48;
	auto first = memory.render(score, true, 0., 4, options, 1. / 48000.);
	auto octave = memory.render(score, true, .25, 4, options, 1. / 48000.);
	auto air = memory.render(score, true, .5, 4, options, 1. / 48000.);
	auto silent = memory.render(score, true, .75, 4, options, 1. / 48000.);
	CHECK(std::fabs(first.pitch[1] - 17.f / 12) < 1e-6, "key and reference pitch");
	CHECK(std::fabs(octave.pitch[1] - first.pitch[1] - 1.f) < 1e-6, "one volt per octave");
	CHECK(air.pitch[1] == octave.pitch[1] && air.gate[1] == 10.f, "AIR holds pitch but triggers");
	CHECK(silent.pitch[1] == octave.pitch[1] && silent.gate[1] == 0.f, "zero velocity holds pitch without gate");
	CHECK(silent.velocity[1] == octave.velocity[1], "zero velocity holds previous velocity");
	score.mutes[1] = true;
	auto muted = memory.render(score, true, 0., 4, options, 1. / 48000.);
	CHECK(muted.pitch[1] == octave.pitch[1] && muted.gate[1] == 0.f, "mute holds pitch without gate");
	auto paused = memory.render(score, false, 0., 4, options, 1. / 48000.);
	CHECK(paused.pitch[1] == octave.pitch[1] && paused.clock == 0.f, "pause holds pitch");
	auto seek = memory.render(score, true, 1., 5, options, 1. / 48000.);
	CHECK(seek.pitch[1] == 0.f && seek.velocity[1] == 0.f, "seek clears held notes");
}

// A transport change heard late still plays the hit it landed on (spec/cv-semantics.md,
// landing gate). 120 BPM: a row is 125 ms (6000 samples), its gate 62.5 ms.
static void landingGate() {
	const double dt = 1. / 48000.;
	const int row = 6000, ms = 48;
	Options options;
	auto score = [](bool nextRow) {
		Score s;
		s.bpm = 120.;
		Pattern& p = s.patterns[0];
		p.hit[0][4] = p.hit[2][4] = true;
		p.hit[0][5] = nextRow;
		p.velocity[0][4] = p.velocity[2][4] = p.velocity[0][5] = 127;
		s.mutes[2] = true;
		return s;
	};
	// `frames` of lane 1's gate, from `late` seconds after the playhead landed at `landing`.
	auto heard = [&](OutputState& memory, const Score& s, double landing, double late, int frames,
	                 int64_t transport, bool moving = true, double anchor = NAN) {
		std::vector<float> gate(frames);
		for (int i = 0; i < frames; i++) {
			double beat = landing + (moving ? (late + i * dt) * s.bpm / 60. : 0.);
			Outputs out = memory.render(s, moving, beat, transport, options, dt, std::isnan(anchor) ? landing : anchor);
			gate[i] = out.gate[0];
			CHECK(out.gate[2] == 0.f, "muted lane stays closed");
		}
		return gate;
	};
	auto high = [](const std::vector<float>& g) { int n = 0; for (float v : g) n += v > 0.f; return n; };
	auto rises = [](const std::vector<float>& g) {
		std::vector<int> at;
		for (size_t i = 1; i < g.size(); i++)
			if (g[i] > 0.f && g[i - 1] == 0.f)
				at.push_back(int(i));
		return at;
	};
	Score one = score(false), two = score(true);
	// At once: the row's own gate. 40 ms late: all of it, from now. 70 ms late (the window
	// had closed): until 1 ms before the row ends. In the next row: nothing.
	const double lates[] = {0., .04, .07, .13};
	const int lengths[] = {row / 2, row / 2 - ms, row - 3360 - ms, 0};
	for (int k = 0; k < 4; k++) {
		OutputState memory;
		auto g = heard(memory, one, 1., lates[k], row, 1);
		CHECK(high(g) == lengths[k] && (!lengths[k] || (g[0] > 0.f && g[lengths[k] - 1] > 0.f)),
		      "seek heard %.0f ms late: %d samples high, expected %d", lates[k] * 1000, high(g), lengths[k]);
	}
	{
		OutputState memory;
		auto g = heard(memory, two, 1., .07, row, 1);
		CHECK(g[0] > 0.f && rises(g) == std::vector<int>{row - 3360}, "next row's hit still rises");
	}
	{
		OutputState memory;
		CHECK(high(heard(memory, one, 1., .07, 64, 1)) == 64, "landing gate");
		CHECK(high(heard(memory, one, 1., .07, 64, 1, false)) == 0, "pause ends it");
		OutputState seek;
		CHECK(high(heard(seek, one, 1., .07, 64, 1)) == 64, "landing gate");
		CHECK(high(heard(seek, one, 1.5, 0., 64, 2)) == 0, "another seek ends it");
	}
	// A seek that interrupts an open gate retriggers it: 1 ms low, then the landing hit.
	{
		OutputState memory;
		Score s = score(false);
		s.patterns[0].hit[0][0] = true;
		s.patterns[0].velocity[0][0] = 127;
		CHECK(high(heard(memory, s, 0., 0., 64, 1)) == 64, "row 0 gate open");
		auto g = heard(memory, s, 1., 0., row, 2);
		CHECK(g[0] == 0.f && g[ms - 1] == 0.f && g[ms] > 0.f && high(g) == row / 2 - ms,
		      "seek retriggers an open gate: %d samples high", high(g));
		CHECK(high(heard(memory, s, 1.05, 0., 64, 2)) == 64, "a repeated snapshot does not retrigger");
	}
	// Resume: the landing is where the playhead stood, not the anchor of the last seek.
	for (double stood : {1., 1.15}) {
		OutputState memory;
		heard(memory, one, stood, 0., 64, 1, false, 3.);
		int n = high(heard(memory, one, stood, .07, row, 1, true, 3.));
		CHECK(n == (stood == 1. ? row - 3360 - ms : 0), "resume at beat %g heard 70 ms late: %d samples", stood, n);
	}
}

// Studio at `bpm` on its clock; Rack's sample clock runs `ppm` fast, blocks are
// processed with timing jitter, and the bridge updates 30 times a second.
// Returns the largest phase error after settling, in milliseconds.
struct Run {
	double maxErrorMs = 0., maxPeriodErrorPct = 0.;
	int edges = 0;
	long jumps = 0;
};

static Run follow(double bpm, double ppm, double jitterMs, double seekAt = -1., double seekTo = 0., bool loop = false) {
	const double sr = 48000.;
	const int block = 256;
	Score score;
	score.bpm = bpm;
	score.loop = loop;
	score.pattern = loop ? 2 : 0;
	Playhead head;
	head.anchorBeat = score.cycleStart();
	head.valid = head.enabled = head.playing = true;
	Follower f;
	Options o;
	std::mt19937 rng(7);
	std::normal_distribution<double> jitter(0., jitterMs / 1000.);
	double rackSeconds = 0.;            // Rack's idea of time (sample clock)
	double published = -1.;
	Run run;
	bool last = false;
	double lastEdge = -1.;
	double period = 60. / bpm / o.ppqn;
	bool seeked = false;
	for (long frame = 0; frame < long(sr * 20); frame++) {
		double trueTime = rackSeconds / (1. + ppm * 1e-6);    // Studio's clock
		if (trueTime - published >= 1. / 30.) {                // a bridge update
			head.wall = head.clockTime = trueTime;
			if (!seeked && seekAt >= 0. && trueTime >= seekAt) {
				head.anchorBeat = seekTo;
				head.anchorTime = trueTime;
				head.transportRevision++;   // Studio marks every explicit seek
				seeked = true;
			}
			published = trueTime;
		}
		if (frame % 32 == 0) {
			// The module stamps blocks with Rack's monotonic time, which tracks
			// Studio's (same machine) with scheduling jitter.
			double blockStart = std::floor(double(frame) / block) * block;
			double local = blockStart / sr / (1. + ppm * 1e-6) + jitter(rng) + (frame - blockStart) / sr;
			f.correct(score, true, head.beatAt(score, local), head.transportRevision);
		}
		Outputs out = evaluate(score, true, f.beat, o);
		bool high = out.clock > 0;
		if (high && !last) {
			double t = trueTime;
			if (lastEdge >= 0. && t > 2. && !(seeked && std::fabs(t - seekAt) < .5)) {
				double err = std::fabs((t - lastEdge) - period) / period * 100.;
				run.maxPeriodErrorPct = std::max(run.maxPeriodErrorPct, err);
			}
			lastEdge = t;
			run.edges++;
		}
		last = high;
		if (trueTime > 2. && !(seeked && trueTime - seekAt < .5)) {
			double truth = head.beatAt(score, trueTime);
			double e = truth - f.beat;
			e -= score.cycleLength() * std::floor(e / score.cycleLength() + .5);
			run.maxErrorMs = std::max(run.maxErrorMs, std::fabs(e) * 60. / bpm * 1000.);
		}
		f.advance(score, true, 1. / sr);
		rackSeconds += 1. / sr;
	}
	run.jumps = f.jumps;
	return run;
}

static void follower() {
	Run steady = follow(138., 0., 0.);
	CHECK(steady.maxErrorMs < .05, "no drift: %.3f ms", steady.maxErrorMs);
	CHECK(steady.maxPeriodErrorPct < .2, "no drift: period error %.3f%%", steady.maxPeriodErrorPct);
	CHECK(steady.jumps == 1, "no drift: %ld jumps", steady.jumps);
	// 300 ppm is far worse than real audio clocks (uncorrected, 6 ms over the
	// run); 2 ms of block jitter is heavy. Looping crosses the wrap every 7 s.
	Run drift = follow(138., 300., 2., -1., 0., true);
	std::printf("follower at 138 BPM, 300 ppm drift, 2 ms jitter, looping: phase error %.2f ms, period error %.2f%%, "
	            "%d edges, %ld jump(s)\n", drift.maxErrorMs, drift.maxPeriodErrorPct, drift.edges, drift.jumps);
	CHECK(drift.maxErrorMs < 1., "drift phase error %.3f ms", drift.maxErrorMs);
	CHECK(drift.maxPeriodErrorPct < 1.5, "drift period error %.3f%%", drift.maxPeriodErrorPct);
	CHECK(std::abs(drift.edges - int(20. * 138. / 60. * 4.)) <= 2, "edge count %d", drift.edges);
	CHECK(drift.jumps == 1, "drift: locked once, then slewed (%ld jumps)", drift.jumps);
	Run seek = follow(120., 50., 1., 8., 40.);
	CHECK(seek.maxErrorMs < 1., "after a seek: %.3f ms", seek.maxErrorMs);
	CHECK(seek.jumps == 2, "a seek is one jump (%ld)", seek.jumps);
	// A seek shorter than the drift threshold (a fifth of a row back from beat 16 at 8 s) is still a
	// seek: transport_revision changes, so the follower jumps instead of slewing for seconds.
	Run shortSeek = follow(120., 50., 1., 8., 16. - .05);
	CHECK(shortSeek.maxErrorMs < 1., "after a short seek: %.3f ms", shortSeek.maxErrorMs);
	CHECK(shortSeek.jumps == 2, "a short seek is one jump (%ld)", shortSeek.jumps);
}

// A version-3 score looping one empty pattern: `rows` rows at `perQuarter` rows a quarter.
static v3::ScoreV3 loopV3(double bpm, int rows = 64, int perQuarter = 4) {
	v3::ScoreV3 score;
	score.bpm = bpm;
	score.loop = true;
	v3::PatternV3 p;
	p.geometry.w = v3::TICKS / perQuarter;
	p.geometry.rows = rows;
	p.geometry.bar = std::min(rows, 16);
	p.geometry.bars = rows / p.geometry.bar;
	for (v3::LaneV3& lane : p.lanes) {
		lane.len = rows;
		lane.cells.assign(size_t(rows), v3::Cell());
	}
	score.store.push_back(p);
	score.index[0] = 0;
	score.buildTimeline();
	return score;
}

// The version-3 follower: Studio's u at `bpm` on its clock, Rack's sample clock `ppm` fast with
// block jitter, bridge updates 30 times a second. A tempo change re-anchors Studio on a tick
// in the same segment (no jump); a seek starts a new one (one jump).
static Run followV3(double bpm, double ppm, double jitterMs, double tempoAt = -1., double tempo = 0., double seekAt = -1.) {
	const double sr = 48000.;
	const int block = 256;
	v3::ScoreV3 score = loopV3(bpm);
	Playhead head;
	head.valid = head.enabled = head.playing = head.v3 = true;
	head.transportRevision = 0;
	v3::TickFollower f;
	v3::EngineV3 engine;
	Options o;
	v3::Layer layer;
	std::mt19937 rng(7);
	std::normal_distribution<double> jitter(0., jitterMs / 1000.);
	double rackSeconds = 0., published = -1., lastEdge = -1., period = 60. / bpm / o.ppqn;
	bool last = false, changed = false;
	Run run;
	for (long frame = 0; frame < long(sr * 20); frame++) {
		double trueTime = rackSeconds / (1. + ppm * 1e-6);
		if (trueTime - published >= 1. / 30.) {
			head.wall = head.clockTime = trueTime;
			if (!changed && tempoAt >= 0. && trueTime >= tempoAt) {
				// Tracker._reanchor: elapsed becomes the whole tick last reached, anchor_time when.
				double u = v3::ticksAt(head, trueTime, score.bpm);
				head.elapsed = int64_t(std::floor(u));
				head.anchorTime = trueTime - (u - double(head.elapsed)) * 60. / score.bpm / double(v3::TICKS);
				score.bpm = tempo;
				period = 60. / tempo / o.ppqn;
				changed = true;
			}
			if (!changed && seekAt >= 0. && trueTime >= seekAt) {
				head.transportRevision++;
				head.startTick = 3 * v3::TICKS + 100;
				head.elapsed = 0;
				head.anchorTime = trueTime;
				changed = true;
			}
			published = trueTime;
		}
		if (frame % 32 == 0) {
			double blockStart = std::floor(double(frame) / block) * block;
			double local = blockStart / sr / (1. + ppm * 1e-6) + jitter(rng) + (frame - blockStart) / sr;
			f.correct(score.bpm, true, v3::ticksAt(head, local, score.bpm), head.transportRevision, head.startTick);
		}
		Outputs out = engine.render(score, layer.transport(head.startTick), true, f.u, head.transportRevision, o, sr);
		bool high = out.clock > 0;
		bool settling = changed && std::fabs(trueTime - std::max(tempoAt, seekAt)) < .5;
		if (high && !last) {
			if (lastEdge >= 0. && trueTime > 2. && !settling)
				run.maxPeriodErrorPct = std::max(run.maxPeriodErrorPct, std::fabs((trueTime - lastEdge) - period) / period * 100.);
			lastEdge = trueTime;
			run.edges++;
		}
		last = high;
		if (trueTime > 2. && !settling)
			run.maxErrorMs = std::max(run.maxErrorMs, std::fabs(v3::ticksAt(head, trueTime, score.bpm) - f.u) / double(v3::TICKS) * 60. / score.bpm * 1000.);
		f.advance(score.bpm, true, 1. / sr);
		rackSeconds += 1. / sr;
	}
	run.jumps = f.jumps;
	return run;
}

static void followerV3() {
	Run drift = followV3(138., 300., 2.);
	std::printf("version-3 follower at 138 BPM, 300 ppm drift, 2 ms jitter: phase error %.2f ms, period error %.2f%%, "
	            "%d edges, %ld jump(s)\n", drift.maxErrorMs, drift.maxPeriodErrorPct, drift.edges, drift.jumps);
	CHECK(drift.maxErrorMs < 1., "version-3 drift phase error %.3f ms", drift.maxErrorMs);
	CHECK(drift.maxPeriodErrorPct < 1.5, "version-3 drift period error %.3f%%", drift.maxPeriodErrorPct);
	CHECK(std::abs(drift.edges - int(20. * 138. / 60. * 4.)) <= 2, "version-3 edge count %d", drift.edges);
	CHECK(drift.jumps == 1, "version-3 drift: locked once, then slewed (%ld jumps)", drift.jumps);
	Run tempo = followV3(138., 50., 1., 8., 120.);
	CHECK(tempo.maxErrorMs < 1. && tempo.jumps == 1, "a tempo change keeps the segment: %.3f ms, %ld jumps", tempo.maxErrorMs, tempo.jumps);
	Run seek = followV3(120., 50., 1., -1., 0., 8.);
	CHECK(seek.maxErrorMs < 1. && seek.jumps == 2, "a seek is one jump: %.3f ms, %ld jumps", seek.maxErrorMs, seek.jumps);
}

// Version 3's outputs on six lanes, and BUS OUT's word: lanes on bits 0–5 from the candidates
// that fire, FILL on bit 6 from the row's start (bytebus-output.md#version-3).
static void outputsV3() {
	v3::ScoreV3 score = loopV3(120., 4, 4);   // four rows of 480 ticks
	auto cell = [&](int lane, int row, int midi) {
		v3::Cell& c = score.store[0].lanes[lane].cells[size_t(row)];
		c.present = true;
		c.vel = 100.;
		c.midi = midi;
		return &c;
	};
	cell(0, 0, 60);
	cell(5, 1, 72);
	cell(2, 2, -1)->cond.kind = v3::COND_FILL;   // AIR, on FILL
	cell(3, 3, 60);
	score.store[0].lanes[5].cells[1].hasLocks = true;
	score.store[0].lanes[5].cells[1].locks.mask = 1;
	score.store[0].lanes[5].cells[1].locks.values[0] = 99.f;
	v3::Layer layer;
	layer.fill = {{0, false}, {960, true}};
	layer.busMutes[3] = {{0, false}, {1440, true}};
	v3::TransportV3 t = layer.transport(0);
	v3::EngineV3 engine;
	Options o;
	const double sr = 48000.;
	auto at = [&](double u) { return engine.render(score, t, true, u, 1, o, sr); };
	at(0.);   // the first segment an engine hears is joined at its own tick
	Outputs a = at(10.);
	CHECK(busWord(a, BusSource::HitMask) == 0x01 && !a.fill, "row 0: lane 1 (%02x)", busWord(a, BusSource::HitMask));
	CHECK(a.gate[0] > 0.f && std::fabs(a.velocity[0] - 100.f / 127.f) < 1e-6f && a.pitch[0] == 0.f, "lane 1 plays C4 at 0 V");
	CHECK(engine.sound(score, 0).mask == 0, "lane 1 has the kit sound");
	Outputs b = at(500.);
	CHECK(busWord(b, BusSource::HitMask) == 0x20 && b.gate[5] > 0.f && std::fabs(b.pitch[5] - 1.f) < 1e-6f, "row 1: lane 6, C5 at 1 V");
	CHECK(engine.sound(score, 5).mask == 1 && engine.sound(score, 5).values[0] == 99.f, "lane 6's lock reaches the sound link");
	Outputs c = at(1000.);
	CHECK(busWord(c, BusSource::HitMask) == 0x44 && c.fill && c.gate[2] > 0.f, "row 2: FILL and lane 3's FILL hit (%02x)",
	      busWord(c, BusSource::HitMask));
	CHECK(c.pitch[2] == 0.f && c.pitch[5] == b.pitch[5], "AIR leaves pitch; other lanes hold theirs");
	Outputs d = at(1500.);
	CHECK(busWord(d, BusSource::HitMask) == 0x40 && d.gate[3] == 0.f, "row 3: lane 4 muted by the bus (%02x)", busWord(d, BusSource::HitMask));
	Outputs e = at(1919.);
	CHECK(busWord(e, BusSource::HitMask) == 0x40, "FILL holds to the end of row 3");
	Outputs f = at(1920. + 10.);
	CHECK(busWord(f, BusSource::HitMask) == 0x41 && f.fill, "the next cycle: lane 1 and FILL");
	CHECK(engine.count <= 2, "%d hits kept", engine.count);
	CHECK(std::string(busSourceName(BusSource::HitMask, true)) == "Hit mask (lanes 1–6, FILL)", "menu name");
	// A version-1/2 engine's state carries across a change of format: no reset, held values kept.
	Score old;
	old.bpm = 120.;
	old.patterns[0].hit[1][0] = true;
	old.patterns[0].velocity[1][0] = 127.f;
	OutputState state;
	state.render(old, true, .01, 3, o, 1. / sr);
	v3::EngineV3 next;
	next.continueFrom(state);
	Outputs g = next.render(score, t, true, 2000., 3, o, sr);
	CHECK(g.reset == 0.f && g.velocity[1] == 1.f, "format change: no reset, velocity held");
	CHECK(next.frontier == 2001, "a segment heard as it starts is evaluated from at or before its anchor row");
}

static void details() {
	Score score;
	score.loop = true;
	score.pattern = 3;
	CHECK(std::fabs(score.wrap(47.9) - 63.9) < 1e-9, "loop wrap %g", score.wrap(47.9));
	CHECK(score.wrap(64.) == 48., "wrap at end %g", score.wrap(64.));
	CHECK(score.wrap(-1e-17) < 64., "wrap rounding %g", score.wrap(-1e-17));
	// Stopped: hold position, outputs off, lanes still sent.
	Playhead head;
	head.valid = head.enabled = true;
	head.anchorBeat = 50.;
	CHECK(head.beatAt(score, 10.) == 50., "paused beat %g", head.beatAt(score, 10.));
	Outputs o = evaluate(score, false, 50., Options());
	CHECK(o.clock == 0.f && o.run == 0.f && o.gate[0] == 0.f, "stopped outputs");
	CHECK(std::fabs(o.lane[LANE_Z] - .35) < 1e-6, "default z %g", o.lane[LANE_Z]);
	CHECK(laneVolts(LANE_ROLL, -45.) == -5.f && laneVolts(LANE_ROLL, 45.) == 5.f, "roll volts");
	CHECK(laneVolts(LANE_FEEDBACK, .9) == 10.f, "feedback volts");
	CHECK(voltsToControl(5., -45., 45.) == 0. && voltsToControl(12., 0., 2.) == 2., "control mapping");
	// Clock: the smallest-trip sample wins.
	BridgeClock c;
	c.observe(10., 10.010, 110.003);   // slow trip, offset 99.998
	c.observe(20., 20.001, 120.0005);  // fast trip, offset 100.0
	c.observe(30., 30.020, 130.015);   // slow
	CHECK(std::fabs(c.offset - 100.) < 1e-9, "offset %g", c.offset);
	// A follower that was stopped jumps when playback starts.
	Follower f;
	f.correct(score, false, 50.);
	f.correct(score, true, 52.);
	CHECK(f.beat == 52., "start jump %g", f.beat);
}

int main(int argc, char** argv) {
	details();
	heldOutputsAndTransport();
	landingGate();
	follower();
	followerV3();
	outputsV3();
	fixture(argc > 1 ? argv[1] : "tests/fixture.txt");
	if (failures) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("engine_test: all passed\n");
	return 0;
}
