// Score format 3's signals: the candidates evaluate() returns, played one sample at a time.
//
// The contract is spec/cv-semantics.md with evaluator.md's changes for version 3; the
// reference is TrackerCV._render_v3 in hydrapy/tracker_cv.py, and the version-3 fixture
// (tests/fixture3.txt) checks this against it sample by sample. No Rack dependency and
// no allocation: the module calls render() on the audio thread.
#pragma once
#include <cmath>
#include <vector>

#include "GorgonEngine.hpp"
#include "ScoreV3.hpp"

namespace gorgon {
namespace v3 {

// The longest row, at rate ¼. A candidate that starts this long before a tick has ended by it.
constexpr int64_t LONGEST_ROW = 4 * TICKS;

// The playhead's FILL and bus layer (evaluator.md#playhead-fields, bus-io.md). The bridge
// thread builds one and never changes it after, so the audio thread reads it without a lock.
struct Layer {
	std::vector<Transition> fill{Transition()}, busFill{Transition()}, busMutes[LANES];

	Layer() {
		for (auto& mutes : busMutes)
			mutes.assign(1, Transition());
	}
	bool operator==(const Layer& o) const {
		auto same = [](const std::vector<Transition>& a, const std::vector<Transition>& b) {
			return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const Transition& x, const Transition& y) {
				return x.at == y.at && x.on == y.on;
			});
		};
		bool equal = same(fill, o.fill) && same(busFill, o.busFill);
		for (int l = 0; l < LANES; l++)
			equal = equal && same(busMutes[l], o.busMutes[l]);
		return equal;
	}
	TransportV3 transport(int64_t tick) const {
		TransportV3 t;
		t.tick = tick;
		t.fill = fill;
		t.busFill = busFill;
		for (int l = 0; l < LANES; l++)
			t.busMutes[l] = busMutes[l];
		return t;
	}
};

// Elapsed ticks u at Studio time `serverTime` (evaluator.md#playhead-fields): it moves while
// the tracker is playing and enabled, as the beat does.
inline double ticksAt(const Playhead& h, double serverTime, double bpm) {
	if (!h.enabled)
		return double(h.elapsed);
	double clock = h.clockTime + (h.playing ? serverTime - h.wall : 0.);
	return double(h.elapsed) + (clock - h.anchorTime) * bpm / 60. * double(TICKS);
}

// Where elapsed ticks u fall in the cycle: tracker_v3.position() for one sample.
struct Position {
	double tick = 0.;     // the cycle tick, real
	int entry = 0;
	double rowPos = 0.;   // the fractional row in that entry's pattern
};

inline Position position(const ScoreV3& score, int64_t start, double u) {
	Position p;
	const double cycle = double(score.cycle);
	double t = std::fmod(double(start) + u, cycle);
	if (t < 0.)
		t += cycle;
	p.tick = std::min(t, std::nextafter(cycle, 0.));   // x mod C can round to C
	p.entry = int(std::upper_bound(score.starts.begin(), score.starts.end(), p.tick,
	                               [](double value, int64_t s) { return value < double(s); }) - score.starts.begin()) - 1;
	p.rowPos = (p.tick - double(score.starts[size_t(p.entry)])) / double(score.geometry(score.entries[size_t(p.entry)]).w);
	return p;
}

// The module's elapsed ticks: Follower's rules for version 3. It runs at the score tempo on
// Rack's sample clock, trimmed toward Studio's u, and jumps at a new segment or transport
// change; u doesn't wrap, so there is no shortest way round.
struct TickFollower {
	double jumpTicks = .1 * double(TICKS);   // as Follower's 0.1 beat
	double maxTrim = .005, gainPerSecond = 1.;
	double u = 0., trim = 0., filtered = 0.;
	bool locked = false, wasMoving = false;
	long jumps = 0;
	int64_t transportRevision = -1, segmentTick = -1;

	void advance(double bpm, bool moving, double seconds) {
		if (moving && locked)
			u += seconds * bpm / 60. * double(TICKS) * (1. + trim);
	}

	// `target` is Studio's u for the sample about to be played; `transport` and `tick` name
	// the segment (its transport_revision and anchor).
	void correct(double bpm, bool moving, double target, int64_t transport, int64_t tick) {
		double error = target - u;
		bool seeked = transport != transportRevision || tick != segmentTick;
		transportRevision = transport;
		segmentTick = tick;
		bool jump = !locked || seeked || moving != wasMoving || !moving || std::fabs(error) > jumpTicks;
		wasMoving = moving;
		if (jump) {
			if (moving)
				jumps++;
			u = target;
			trim = filtered = 0.;
			locked = true;
			return;
		}
		filtered += .05 * (error - filtered);
		double seconds = filtered / double(TICKS) * 60. / bpm;
		trim = std::max(-maxTrim, std::min(maxTrim, seconds * gainPerSecond));
	}
};

// A candidate the engine keeps until it ends, with what it plays copied out of the score,
// so a score edit while it sounds can't change or free it.
struct Hit {
	int64_t at = 0, duration = 0;
	int lane = 0;
	bool fires = false;
	double vel = 0.;
	int midi = -1;                 // -1 for AIR
	gorgon_sound::Sound sound;     // the lane's kit sound with the cell's locks (sound-locks.md)
};

// The kit sound for `lane` with a cell's locks over it.
inline gorgon_sound::Sound resolve(const ScoreV3& score, int lane, const Cell& cell) {
	gorgon_sound::Sound sound = score.sounds[lane];
	if (!cell.hasLocks)
		return sound;
	if (cell.locks.machine >= 0)
		sound.machine = cell.locks.machine;
	for (int i = 0; i < gorgon_sound::PARAMS; ++i)
		if (cell.locks.mask & (1u << i))
			sound.values[i] = cell.locks.values[i];
	sound.mask |= cell.locks.mask;
	return sound;
}

// TrackerCV's version-3 state, one sample at a time: the segment's candidates, the reset
// pulse, landing gates, retriggers and held pitch and velocity (cv-semantics.md).
struct EngineV3 {
	static constexpr int CAPACITY = 64;

	bool heard = false;            // a playhead seen before; the first segment is joined late
	bool begun = false;
	int64_t transportRevision = -1, segmentRevision = -1, segmentTick = -1;
	bool wasMoving = false;
	int64_t frontier = 0;          // the first tick of the segment not yet evaluated
	Hit hits[CAPACITY];
	int count = 0;
	long dropped = 0;              // candidates lost to a full buffer (never, in practice)
	int resetRemaining = 0;
	int landing[LANES] = {};
	int64_t landingLow[LANES] = {}, landingHigh[LANES] = {};
	double stood = NAN;            // u while not moving: where a resume lands
	bool gateWas[LANES] = {};
	int retrigger[LANES] = {};
	float velocity[LANES] = {}, pitch[LANES] = {};
	int covering[LANES] = {-1, -1, -1, -1, -1, -1};   // the firing hit under u, per lane

	// Carry the transport state across from the version-1/2 engine when the score changes
	// format, as TrackerCV keeps one state for both: no reset, held values kept.
	void continueFrom(const OutputState& old) {
		heard = old.transportRevision >= 0 || old.wasMoving;
		transportRevision = old.transportRevision;
		wasMoving = old.wasMoving;
		for (int l = 0; l < AUDIO_LANES; l++) {
			gateWas[l] = old.gateWas[l];
			velocity[l] = old.velocity[l];
			pitch[l] = old.pitch[l];
		}
		stood = NAN;
	}
	void continueInto(OutputState& next) const {
		next.transportRevision = transportRevision;
		next.wasMoving = wasMoving;
		for (int l = 0; l < AUDIO_LANES; l++) {
			next.gateWas[l] = gateWas[l];
			next.velocity[l] = velocity[l];
			next.pitch[l] = pitch[l];
		}
		next.stood = -1.;
	}

	// Evaluate through tick ⌊u⌋, keeping the candidates that haven't ended by u. Earlier ones
	// are skipped unevaluated: nothing would hear them, and evaluate() partitions exactly.
	void advance(const ScoreV3& score, const TransportV3& transport, double u) {
		int64_t floorU = int64_t(std::floor(u));
		int64_t top = floorU + 1;
		if (top <= frontier)
			return;
		int64_t first = std::max(frontier, floorU - LONGEST_ROW);
		frontier = top;
		evaluate(score, transport, first, top, [&](const Candidate& c) {
			if (double(c.at + c.duration) <= u)
				return;
			if (count == CAPACITY) {
				dropped++;
				return;
			}
			Hit& h = hits[count++];
			h.at = c.at;
			h.duration = c.duration;
			h.lane = c.lane;
			h.fires = c.fires;
			h.vel = c.cell->vel;
			h.midi = c.cell->midi;
			h.sound = resolve(score, c.lane, *c.cell);
		});
	}

	// The landing gate (cv-semantics.md#landing-gate): per lane, the firing candidate whose
	// span holds the landing starts a gate if the landing is inside its gate fraction and `u`
	// is still inside its span. Counted in samples, as TrackerCV counts it.
	void startLanding(double at, double u, double bpm, const Options& o, double sampleRate) {
		for (int k = 0; k < count; k++) {
			const Hit& h = hits[k];
			int64_t end = h.at + h.duration;
			if (!h.fires || !(double(h.at) <= at && at < double(end)) || !(double(h.at) <= u && u < double(end)))
				continue;
			double phase = (at - double(h.at)) / double(h.duration);
			if (phase >= o.gate)
				continue;
			double ticks = std::min(o.gate - phase, 1. - (u - double(h.at)) / double(h.duration)) * double(h.duration);
			double seconds = ticks * 60. / bpm / double(TICKS) - .001;
			landing[h.lane] = std::max(0, int(std::nearbyint(seconds * sampleRate)));
			landingLow[h.lane] = h.at;
			landingHigh[h.lane] = end;
		}
	}

	// Everything the tracker sends at elapsed tick `u` of the segment `revision`/`transport.tick`.
	Outputs render(const ScoreV3& score, const TransportV3& transport, bool moving, double u, int64_t revision,
	               const Options& o, double sampleRate) {
		bool changed = !heard || revision != transportRevision;
		bool newSegment = !begun || revision != segmentRevision || transport.tick != segmentTick;
		if (newSegment) {
			// Heard as it starts, from at or before the anchor's row; the first playhead this
			// engine ever sees is joined late, from its own tick (evaluator.md#evaluate).
			frontier = heard ? -TICKS : int64_t(std::floor(u));
			count = 0;
			begun = true;
			segmentRevision = revision;
			segmentTick = transport.tick;
		}
		if (changed || (moving && !wasMoving)) {
			resetRemaining = moving ? int(std::nearbyint(o.resetSeconds * sampleRate)) : 0;
			std::fill(velocity, velocity + LANES, 0.f);
			std::fill(pitch, pitch + LANES, 0.f);
			std::fill(landing, landing + LANES, 0);
			// A gate the change interrupts closes for 1 ms, so a hit it lands on rises again.
			for (int l = 0; l < LANES; l++)
				retrigger[l] = (gateWas[l] && moving) ? int(std::nearbyint(.001 * sampleRate)) : 0;
			if (moving) {
				advance(score, transport, u);
				startLanding((changed || newSegment || std::isnan(stood)) ? 0. : stood, u, score.bpm, o, sampleRate);
			}
		}
		if (!moving) {
			resetRemaining = 0;
			std::fill(landing, landing + LANES, 0);
			std::fill(retrigger, retrigger + LANES, 0);
			stood = u;
		}
		heard = true;
		transportRevision = revision;
		wasMoving = moving;
		if (moving)
			advance(score, transport, u);
		bool reset = resetRemaining > 0;
		resetRemaining = std::max(0, resetRemaining - 1);

		Outputs out;
		for (int l = 0; l < LANES; l++) {
			bool gate = false;
			covering[l] = -1;
			for (int k = 0; k < count; k++) {
				const Hit& h = hits[k];
				if (h.lane != l || !h.fires || !(double(h.at) <= u && u < double(h.at + h.duration)))
					continue;
				gate = gate || u < double(h.at) + o.gate * double(h.duration);
				covering[l] = k;
			}
			if (landing[l] > 0) {
				// Leaving the landing candidate's span ends it for good.
				bool held = u >= double(landingLow[l]) && u < double(landingHigh[l]);
				landing[l] = held ? landing[l] - 1 : 0;
				gate = gate || held;
			}
			bool on = gate && moving && retrigger[l] <= 0;
			gateWas[l] = on;
			out.gate[l] = on ? float(o.high) : 0.f;
			if (moving && covering[l] >= 0) {
				const Hit& h = hits[covering[l]];
				velocity[l] = float(h.vel / 127.);
				if (h.midi >= 0)
					pitch[l] = float(h.midi + score.key - 2 - o.pitchZero) / 12.f;
			}
			out.present[l] = moving && covering[l] >= 0;
			out.pitched[l] = out.present[l] && hits[covering[l]].midi >= 0;
			out.velocity[l] = velocity[l];
			out.pitch[l] = pitch[l];
		}
		for (int l = 0; l < LANES; l++)
			retrigger[l] = std::max(0, retrigger[l] - 1);

		Position p = position(score, transport.tick, u);
		double beat = p.tick / double(TICKS);
		if (moving) {
			double phase = beat * o.ppqn;
			out.clock = (phase - std::floor(phase) < .5) ? float(o.high) : 0.f;
			out.reset = (reset || beat < o.resetSeconds * score.bpm / 60.) ? float(o.high) : 0.f;
			out.run = float(o.high);
			// FILL as conditions see it on this row: at the row's start (BUS OUT bit 6).
			int64_t floorU = int64_t(std::floor(u));
			int64_t tick = detail::floorMod(transport.tick + floorU, score.cycle);
			int entry = score.entryAt(tick);
			int64_t w = score.geometry(score.entries[size_t(entry)]).w;
			int64_t rowAt = floorU - (tick - score.starts[size_t(entry)]) % w;
			out.fill = transport.fill(rowAt) || transport.busFill(rowAt);
		}
		const PatternV3* pattern = score.patternAt(score.entries[size_t(p.entry)]);
		for (int k = 0; k < AUTO_LANES; k++)
			out.lane[k] = float(pattern ? pattern->autos[k].value(k, p.rowPos) : LANE_DEFAULT[k]);

		// Drop what has ended. A covering hit hasn't, so it moves with its index.
		int kept = 0;
		for (int k = 0; k < count; k++) {
			if (double(hits[k].at + hits[k].duration) <= u)
				continue;
			for (int l = 0; l < LANES; l++)
				if (covering[l] == k)
					covering[l] = kept;
			hits[kept++] = hits[k];
		}
		count = kept;
		return out;
	}

	// The sound lane `l` plays: its covering candidate's, else the lane's kit sound.
	gorgon_sound::Sound sound(const ScoreV3& score, int l) const {
		return covering[l] >= 0 ? hits[covering[l]].sound : score.sounds[l];
	}
};

} // namespace v3
} // namespace gorgon
