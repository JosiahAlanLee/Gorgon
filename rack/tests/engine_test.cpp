// Portable tests for the Gorgon engine: HydraPy's own outputs (fixture.txt,
// from tools/make_fixture.py), and the follower under clock drift and jitter.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <vector>

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
				seeked = true;
			}
			published = trueTime;
		}
		if (frame % 32 == 0) {
			// The module stamps blocks with Rack's monotonic time, which tracks
			// Studio's (same machine) with scheduling jitter.
			double blockStart = std::floor(double(frame) / block) * block;
			double local = blockStart / sr / (1. + ppm * 1e-6) + jitter(rng) + (frame - blockStart) / sr;
			f.correct(score, true, head.beatAt(score, local));
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
	follower();
	fixture(argc > 1 ? argv[1] : "tests/fixture.txt");
	if (failures) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("engine_test: all passed\n");
	return 0;
}
