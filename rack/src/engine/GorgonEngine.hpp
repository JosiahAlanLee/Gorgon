// Gorgon bridge engine: the HydraPy Studio tracker score, its playhead, and a
// sample-rate follower that keeps Rack locked to Gorgon's clock.
//
// No Rack dependency. Times are seconds on one monotonic clock; the module maps
// Studio's clock onto Rack's with BridgeClock below.
//
// The score semantics mirror hydrapy/studio_tracker.py and hydrapy/tracker_cv.py:
// four patterns of 64 sixteenth rows (16 beats each), four audio lanes of hits,
// and three automation lanes (z, roll, feedback) with hold/linear/smooth keys.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "SoundLink.hpp"

namespace gorgon {

constexpr int PATTERNS = 4, ROWS = 64, AUDIO_LANES = 4, AUTO_LANES = 3, BEATS_PER_PATTERN = 16;
enum AutoLane { LANE_Z, LANE_ROLL, LANE_FEEDBACK };
constexpr double LANE_MIN[AUTO_LANES] = {0., -45., 0.};
constexpr double LANE_MAX[AUTO_LANES] = {1., 45., .9};
constexpr double LANE_DEFAULT[AUTO_LANES] = {.35, 0., .25};
enum Ease { EASE_HOLD, EASE_LINEAR, EASE_SMOOTH };

struct Key {
	int row = 0;
	double value = 0.;
};

struct Lane {
	Ease ease = EASE_HOLD;
	int count = 0;
	Key keys[ROWS];
};

struct Pattern {
	gorgon_sound::Sound locks[AUDIO_LANES][ROWS];
	bool hit[AUDIO_LANES][ROWS] = {};
	float velocity[AUDIO_LANES][ROWS] = {};
	bool pitched[AUDIO_LANES][ROWS] = {};
	int midi[AUDIO_LANES][ROWS] = {};
	Lane lanes[AUTO_LANES];
};

// Trivially copyable on purpose: the audio thread copies it without allocating.
struct Score {
	double bpm = 120.;
	int key = 2;  // the score's D-based notes transpose by key - 2
	bool loop = false;
	int pattern = 0;
	bool mutes[AUDIO_LANES] = {};
	gorgon_sound::Sound sounds[AUDIO_LANES];
	Pattern patterns[PATTERNS];

	double cycleStart() const { return loop ? pattern * BEATS_PER_PATTERN : 0.; }
	double cycleLength() const { return loop ? BEATS_PER_PATTERN : PATTERNS * BEATS_PER_PATTERN; }
	// Wrap a beat into the playing cycle, as Tracker.normalize does.
	double wrap(double beat) const {
		double start = cycleStart(), length = cycleLength();
		double b = start + std::fmod(beat - start, length);
		if (b < start)
			b += length;
		return std::min(b, std::nextafter(start + length, 0.));
	}
};

// Resolve from the kit on every hit. Unspecified values remain unspecified so
// the receiving instrument can apply the resolved machine's native defaults.
inline gorgon_sound::Sound soundAt(const Score& score, int pattern, int lane, int row) {
    auto sound = score.sounds[lane];
    const auto& locks = score.patterns[pattern].locks[lane][row];
    if (locks.machine >= 0) sound.machine = locks.machine;
    for (int i = 0; i < gorgon_sound::PARAMS; ++i)
        if (locks.mask & (1u << i)) sound.values[i] = locks.values[i];
    sound.mask |= locks.mask;
    return sound;
}

// value_at() from studio_tracker.py, for a fractional row, over `count` keys sorted by row.
inline double laneValue(const Key* keys, int count, Ease ease, int laneIndex, double row) {
	if (count == 0)
		return LANE_DEFAULT[laneIndex];
	if (row <= keys[0].row)
		return keys[0].value;
	for (int i = 0; i + 1 < count; i++) {
		const Key& a = keys[i];
		const Key& b = keys[i + 1];
		if (row < b.row) {
			double t = (row - a.row) / double(b.row - a.row);
			if (ease == EASE_HOLD)
				t = 0.;
			else if (ease == EASE_SMOOTH)
				t = t * t * (3. - 2. * t);
			return a.value + (b.value - a.value) * t;
		}
	}
	return keys[count - 1].value;
}

inline double laneValue(const Lane& lane, int laneIndex, double row) {
	return laneValue(lane.keys, lane.count, lane.ease, laneIndex, row);
}

// Studio's playhead as published by GET /api/bridge.
struct Playhead {
	bool valid = false;
	bool enabled = false;
	bool playing = false;
	double anchorBeat = 0., anchorTime = 0., clockTime = 0., wall = 0.;
	int64_t revision = -1;
	int64_t transportRevision = -1;
	// Version 3 (spec/evaluator.md#playhead-fields): the segment's anchor and elapsed ticks.
	bool v3 = false;
	int64_t startTick = 0, elapsed = 0;
	int startEntry = -1;   // -1 when looping

	bool moving() const { return valid && enabled && playing; }
	// Tracker.beat() at Studio monotonic time `serverTime`.
	double beatAt(const Score& score, double serverTime) const {
		double clock = clockTime + (playing ? serverTime - wall : 0.);
		double elapsed = enabled ? clock - anchorTime : 0.;
		return score.wrap(anchorBeat + elapsed * score.bpm / 60.);
	}
};

// Rack's clock → Studio's clock, estimated from request round trips the way NTP
// does: the sample with the shortest round trip carries the least queuing error.
struct BridgeClock {
	static constexpr int WINDOW = 32;
	double offsets[WINDOW] = {}, trips[WINDOW] = {};
	int count = 0, next = 0;
	double offset = 0.;

	// `sent` and `received` on the local clock around a response stamped `serverNow`.
	void observe(double sent, double received, double serverNow) {
		offsets[next] = serverNow - (sent + received) / 2.;
		trips[next] = received - sent;
		next = (next + 1) % WINDOW;
		count = std::min(count + 1, WINDOW);
		int best = 0;
		for (int i = 1; i < count; i++)
			if (trips[i] < trips[best])
				best = i;
		offset = offsets[best];
	}
	bool ready() const { return count > 0; }
	double toServer(double local) const { return local + offset; }
};

// The beat Rack plays. It advances at the score tempo on Rack's sample clock and
// is nudged toward Studio's playhead, so clock edges stay evenly spaced while
// the two clocks' small drift is taken up. Seeks, tempo jumps, starting and
// stopping are followed at once.
struct Follower {
	// Corrections larger than this (in beats) jump instead of slewing.
	double jumpBeats = .1;
	// At most ±0.5% tempo trim, reached for 5 ms of filtered error.
	double maxTrim = .005, gainPerSecond = 1.;
	double beat = 0., trim = 0., filtered = 0.;
	bool locked = false, wasMoving = false;
	long jumps = 0;   // corrections taken as jumps while moving (seeks, edits, lost lock)
	int64_t transportRevision = -1;

	void advance(const Score& score, bool moving, double seconds) {
		if (!moving || !locked)
			return;
		beat = score.wrap(beat + seconds * score.bpm / 60. * (1. + trim));
	}

	// `target` is Studio's beat for the sample about to be played. `transport` is the
	// playhead's transport_revision: a change is an explicit seek (or automation turned on
	// or off), followed at once however short; only unmarked differences are drift.
	void correct(const Score& score, bool moving, double target, int64_t transport = -1) {
		double length = score.cycleLength();
		double error = target - beat;
		error -= length * std::floor(error / length + .5);   // shortest way round the loop
		bool seeked = transport != transportRevision;
		transportRevision = transport;
		bool jump = !locked || seeked || moving != wasMoving || !moving || std::fabs(error) > jumpBeats;
		wasMoving = moving;
		if (jump) {
			if (moving)
				jumps++;
			beat = score.wrap(target);
			trim = filtered = 0.;
			locked = true;
			return;
		}
		filtered += .05 * (error - filtered);
		double seconds = filtered * 60. / score.bpm;
		trim = std::max(-maxTrim, std::min(maxTrim, seconds * gainPerSecond));
	}
};

struct Options {
	double ppqn = 4.;          // clock pulses per quarter note
	double gate = .5;          // gate length as a fraction of a row
	double high = 10.;         // gate and clock voltage (Rack convention)
	double resetSeconds = .005;
	int pitchZero = 60;       // C4 = 0 V (1 V/oct)
};

// Version 3 has six audio lanes; versions 1 and 2 use the first four and leave 5 and 6 at 0.
constexpr int OUTPUT_LANES = 6;

struct Outputs {
	float clock = 0.f, reset = 0.f, run = 0.f;
	float gate[OUTPUT_LANES] = {};
	bool present[OUTPUT_LANES] = {};    // the current row has a hit (moving, lane not muted)
	float velocity[OUTPUT_LANES] = {};  // 0..1 from that hit; read only where present is set
	bool pitched[OUTPUT_LANES] = {};
	float pitch[OUTPUT_LANES] = {};     // volts, read only where pitched is set
	float lane[AUTO_LANES] = {};        // in the lane's own units (z 0..1, roll degrees, feedback 0..0.9)
	bool fill = false;                  // version 3: FILL in force on the current row (BUS OUT bit 6)
};

// Everything the tracker would send at `beat`. Matches TrackerCV.render.
inline Outputs evaluate(const Score& score, bool moving, double beat, const Options& o) {
	Outputs out;
	int p = std::min(std::max(int(beat / BEATS_PER_PATTERN), 0), PATTERNS - 1);
	double rowPos = std::fmod(beat, double(BEATS_PER_PATTERN)) * 4.;
	if (rowPos < 0.)
		rowPos = 0.;
	int row = std::min(int(rowPos), ROWS - 1);
	double rowPhase = rowPos - row;
	const Pattern& pattern = score.patterns[p];
	if (moving) {
		double phase = beat * o.ppqn;
		out.clock = (phase - std::floor(phase) < .5) ? o.high : 0.f;
		double resetBeats = o.resetSeconds * score.bpm / 60.;
		out.reset = (beat - score.cycleStart() < resetBeats) ? o.high : 0.f;
		out.run = o.high;
		for (int l = 0; l < AUDIO_LANES; l++) {
			if (score.mutes[l] || !pattern.hit[l][row] || pattern.velocity[l][row] <= 0.f)
				continue;
			out.present[l] = true;
			out.velocity[l] = pattern.velocity[l][row] / 127.f;
			out.pitched[l] = pattern.pitched[l][row];
			if (out.pitched[l])
				out.pitch[l] = float(pattern.midi[l][row] + score.key - 2 - o.pitchZero) / 12.f;
			if (rowPhase < o.gate)
				out.gate[l] = o.high;
		}
	}
	for (int k = 0; k < AUTO_LANES; k++)
		out.lane[k] = float(laneValue(pattern.lanes[k], k, rowPos));
	return out;
}

// Pattern and row as one index (0..255), and the phase within the row, as evaluate() finds them.
inline int absoluteRow(double beat, double* phase = nullptr) {
	int p = std::min(std::max(int(beat / BEATS_PER_PATTERN), 0), PATTERNS - 1);
	double rowPos = std::max(0., std::fmod(beat, double(BEATS_PER_PATTERN)) * 4.);
	int row = std::min(int(rowPos), ROWS - 1);
	if (phase)
		*phase = rowPos - row;
	return p * ROWS + row;
}

// Stateful part of TrackerCV.render. A transport update can arrive after the
// phase-based 5 ms reset window, so deliver a full pulse on the first available
// samples. Repeated snapshots and ordinary score edits must not retrigger it.
// Likewise the gates of the row it landed on (spec/cv-semantics.md, landing gate):
// heard after the row's gate window closed, they would never open. And a gate the
// change interrupts closes for 1 ms (retrigger), or a hit it lands on never rises.
struct OutputState {
	int64_t transportRevision = -1;
	bool wasMoving = false;
	int resetRemaining = 0;
	int landingRemaining = 0, landingRow = -1;
	double stood = -1.;   // the beat while not moving: where a resume lands
	bool gateWas[AUDIO_LANES] = {};
	int retrigger[AUDIO_LANES] = {};
	float velocity[AUDIO_LANES] = {}, pitch[AUDIO_LANES] = {};

	// `anchor` is the playhead's anchor_beat, where a transport_revision change landed.
	// Without one, a change is taken to have landed at `beat`.
	Outputs render(const Score& score, bool moving, double beat, int64_t transport,
	               const Options& options, double sampleTime, double anchor = NAN) {
		bool changed = transport != transportRevision;
		if (changed || (moving && !wasMoving)) {
			resetRemaining = moving ? int(std::round(options.resetSeconds / sampleTime)) : 0;
			std::fill(velocity, velocity + AUDIO_LANES, 0.f);
			std::fill(pitch, pitch + AUDIO_LANES, 0.f);
			landingRemaining = 0;
			// A gate the change interrupts closes for 1 ms, so a hit it lands on rises again.
			for (int l = 0; l < AUDIO_LANES; l++)
				retrigger[l] = (moving && gateWas[l]) ? int(std::lround(.001 / sampleTime)) : 0;
			if (moving) {
				double landing = (changed || stood < 0.) ? anchor : stood;
				startLanding(score, std::isfinite(landing) ? landing : beat, beat, options, sampleTime);
			}
		}
		if (!moving) {
			resetRemaining = landingRemaining = 0;
			std::fill(retrigger, retrigger + AUDIO_LANES, 0);
			stood = beat;
		}
		transportRevision = transport;
		wasMoving = moving;
		Outputs out = evaluate(score, moving, beat, options);
		if (resetRemaining > 0) {
			out.reset = float(options.high);
			resetRemaining--;
		}
		if (landingRemaining > 0) {
			// Leaving the landing row ends it for good.
			if (absoluteRow(beat) != landingRow)
				landingRemaining = 0;
			else {
				for (int l = 0; l < AUDIO_LANES; l++)
					if (out.present[l])
						out.gate[l] = float(options.high);
				landingRemaining--;
			}
		}
		for (int l = 0; l < AUDIO_LANES; l++) {
			if (retrigger[l] > 0) {
				out.gate[l] = 0.f;
				retrigger[l]--;
			}
			gateWas[l] = out.gate[l] > 0.f;
		}
		for (int l = 0; l < AUDIO_LANES; l++) {
			if (out.present[l])
				velocity[l] = out.velocity[l];
			if (out.pitched[l])
				pitch[l] = out.pitch[l];
			out.velocity[l] = velocity[l];
			out.pitch[l] = pitch[l];
		}
		return out;
	}

	// The landing row's gates last what they had left at the landing, from this sample,
	// and fall 1 ms before the row ends so the next hit still rises. A landing at or after
	// the gate length in its row, or one the playhead has already left, adds nothing.
	void startLanding(const Score& score, double landing, double beat, const Options& options, double sampleTime) {
		double landingPhase, phase;
		int row = absoluteRow(landing, &landingPhase);
		if (landingPhase >= options.gate || absoluteRow(beat, &phase) != row)
			return;
		double seconds = std::min(options.gate - landingPhase, 1. - phase) * 15. / score.bpm - .001;
		landingRemaining = std::max(0, int(std::lround(seconds / sampleTime)));
		landingRow = row;
	}
};

// Lane units to Rack volts: z and feedback 0..10 V, roll ±5 V.
inline float laneVolts(int lane, double value) {
	double unit = (value - LANE_MIN[lane]) / (LANE_MAX[lane] - LANE_MIN[lane]);
	return float(lane == LANE_ROLL ? unit * 10. - 5. : unit * 10.);
}

// 0..10 V into a control's range, clamped.
inline double voltsToControl(double volts, double lo, double hi) {
	double unit = std::max(0., std::min(1., volts / 10.));
	return lo + unit * (hi - lo);
}

} // namespace gorgon
