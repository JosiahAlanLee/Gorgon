// Score format 3 (hydrapy-tracker-3): the score, its timeline and evaluate().
//
// The contract is spec/score-v3.md, spec/evaluator.md, spec/transforms.md and
// spec/bus-io.md; the reference is hydrapy/tracker_v3.py, and the version-3
// fixture (tests/fixture3.txt) checks this against it candidate by candidate.
//
// No Rack or jansson dependency: BridgeJson.hpp parses the document into a
// ScoreV3 on the bridge thread. evaluate() allocates nothing, so the audio
// thread can call it; the strings a trace needs are built by the helpers at the
// end, off the audio path. Onsets are integer ticks (1920 per quarter note);
// explicit durations may be fractional.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "GorgonEngine.hpp"
#include "SoundLink.hpp"

namespace gorgon {
namespace v3 {

constexpr int64_t TICKS = 1920;
constexpr int64_t HOLD_TICKS = 16 * 480;
constexpr int LANES = 6, PATTERNS = 128, MAX_ROWS = 128;
constexpr uint32_t PROB = 0x70726f62, DEGRADE = 0x64656772;   // "prob" and "degr", chance's salts

inline uint32_t lowbias32(uint32_t x) {
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

// evaluator.md's hash of the seed and an occurrence; its value mod 100 is the draw.
inline uint32_t chance(uint32_t seed, uint32_t salt, uint32_t pattern, uint32_t lane, uint32_t row, int64_t bar, uint32_t sub) {
	uint32_t h = lowbias32(seed ^ salt);
	for (uint32_t v : {pattern, lane, row, uint32_t(uint64_t(bar)), sub})
		h = lowbias32(h ^ v);
	return h;
}

enum CondKind : uint8_t { COND_NONE, COND_EVERY, COND_PROB, COND_FILL, COND_FIRST };

struct Cond {
	CondKind kind = COND_NONE;
	int a = 0, b = 0, p = 0;
	bool negate = false;
};

struct Cell {
	bool present = false;          // a note with vel > 0: null and vel 0 never play
	double vel = 0.;
	int midi = -1;                 // -1 for AIR
	char note[4] = {};             // as written: "C-4", "D#4", "AIR"
	bool hasLength = false;
	double length = 0.;
	bool hasLocks = false;
	gorgon_sound::Sound locks;
	Cond cond;
};

enum NodeKind : uint8_t { NODE_ROT, NODE_REV, NODE_PLY, NODE_DEGRADE, NODE_EVERY };

// One transform node. A lane keeps its nodes in pre-order, so a node's index is
// its number (transforms.md) and an every's inner is the node after it.
struct Node {
	NodeKind kind = NODE_REV;
	int value = 0;                 // rot's k, ply's n, degrade's p
	int a = 0, b = 0;              // every a:b
	int top = 0;                   // the index in the lane's list it belongs to
	int depth = 0;                 // 0 for a list item, 1 for its inner, 2 for that one's inner
};

struct LaneV3 {
	int len = 64;
	std::vector<Cell> cells = std::vector<Cell>(64);
	std::vector<Node> nodes;       // pre-order; see Node
};

struct Geometry {
	int64_t w = 480;               // ticks per row
	int rows = 64, bar = 16, bars = 4;
};

// An automation lane (z, roll or feedback): keys on rows below the pattern's, sorted by row.
struct AutoLaneV3 {
	Ease ease = EASE_HOLD;
	std::vector<Key> keys;

	double value(int laneIndex, double row) const {
		return laneValue(keys.data(), int(keys.size()), ease, laneIndex, row);
	}
};

struct PatternV3 {
	Geometry geometry;
	LaneV3 lanes[LANES];
	AutoLaneV3 autos[AUTO_LANES];
};

struct Transition {
	int64_t at = 0;
	bool on = false;
};

// An [{at, on}…] list: on at u if the last transition at or before u is on; of two
// at the same tick the later counts, and before the anchor (u < 0) it has the anchor's state.
struct Transitions {
	const Transition* data = nullptr;
	size_t size = 0;

	Transitions() = default;
	Transitions(const std::vector<Transition>& list) : data(list.data()), size(list.size()) {}
	Transitions(const Transition* d, size_t n) : data(d), size(n) {}

	bool operator()(int64_t u) const {
		if (!size)
			return false;
		u = std::max<int64_t>(u, 0);
		const Transition* end = std::upper_bound(data, data + size, u,
		                                         [](int64_t value, const Transition& t) { return value < t.at; });
		return (end == data ? data : end - 1)->on;
	}
};

// evaluate()'s transport: the anchor's cycle tick, Studio's FILL and the bus layer.
struct TransportV3 {
	int64_t tick = 0;
	Transitions fill, busFill, busMutes[LANES];
};

struct ScoreV3 {
	double bpm = 120.;
	int key = 2;
	bool loop = false;
	int pattern = 0;
	std::vector<int> chain{0};
	uint32_t seed = 0;
	bool mutes[LANES] = {};
	gorgon_sound::Sound sounds[LANES];
	std::vector<PatternV3> store;  // the patterns that aren't null
	int16_t index[PATTERNS];       // into store, or -1 for an empty pattern

	// The timeline (score-v3.md), built by buildTimeline().
	std::vector<int> entries;
	std::vector<int64_t> starts;
	int64_t cycle = 0;
	bool oneVisit = true;          // every entry plays the same pattern, so a visit never ends
	std::vector<int> runs;         // entries just before each one that play its pattern too

	ScoreV3() { std::fill(index, index + PATTERNS, int16_t(-1)); }

	const PatternV3* patternAt(int p) const { return index[p] < 0 ? nullptr : &store[size_t(index[p])]; }
	Geometry geometry(int p) const { return index[p] < 0 ? Geometry() : store[size_t(index[p])].geometry; }

	void buildTimeline() {
		entries = loop ? std::vector<int>{pattern} : chain;
		starts.assign(1, 0);
		for (int p : entries) {
			Geometry g = geometry(p);
			starts.push_back(starts.back() + g.w * g.rows);
		}
		cycle = starts.back();
		int n = int(entries.size());
		oneVisit = std::all_of(entries.begin(), entries.end(), [&](int p) { return p == entries[0]; });
		runs.assign(size_t(n), 0);
		if (!oneVisit)
			for (int i = 0; i < n; i++)
				while (entries[size_t(((i - runs[size_t(i)] - 1) % n + n) % n)] == entries[size_t(i)])
					runs[size_t(i)]++;
	}

	// The entry containing cycle tick `tick`.
	int entryAt(int64_t tick) const {
		return int(std::upper_bound(starts.begin(), starts.end(), tick) - starts.begin()) - 1;
	}
};

// Why a candidate does or doesn't fire: the first reason that fails, else its condition's value.
enum WhyKind : uint8_t { WHY_NONE, WHY_MUTED_SCORE, WHY_MUTED_BUS, WHY_EVERY, WHY_PROB, WHY_FILL, WHY_FIRST, WHY_DEGRADE };

struct Why {
	WhyKind kind = WHY_NONE;
	int64_t bar = 0;               // every and first
	int b = 0;                     // every
	uint32_t draw = 0;             // prob and degrade
	bool fill = false;
	int node = -1;                 // degrade: the node's index in its lane
};

struct Candidate {
	int64_t at = 0;
	double duration = 0.;
	int lane = 0;
	const Cell* cell = nullptr;    // note, velocity, locks and condition
	bool fires = false;
	Why why;
	// origin
	int64_t play = 0, bar = 0;
	int entry = -1;                // -1 when looping
	int pattern = 0, row = 0, laneRow = 0, cellIndex = 0, sub = 0, of = 1;
	int via[8] = {};               // the active leaves, as node indices in the lane
	int viaCount = 0;
};

namespace detail {

inline int64_t floorMod(int64_t x, int64_t m) { return ((x % m) + m) % m; }

inline bool everyHolds(int64_t bar, int a, int b) { return bar % b == a - 1; }

// The active leaves for `bar`, in order (transforms.md, step 1).
inline int activeLeaves(const LaneV3& lane, int64_t bar, int* out) {
	int count = 0;
	for (size_t k = 0; k < lane.nodes.size(); k++) {
		const Node& node = lane.nodes[k];
		if (node.depth != 0)
			continue;
		size_t i = k;
		bool active = true;
		while (lane.nodes[i].kind == NODE_EVERY) {
			active = active && everyHolds(bar, lane.nodes[i].a, lane.nodes[i].b);
			i++;
		}
		if (active)
			out[count++] = int(i);
	}
	return count;
}

struct Event {
	int sub = 0, of = 1;
	int dropNode = -1;
	uint32_t dropDraw = 0;
};

// The candidates of one lane on one row, in sub-step order.
inline int laneRow(const ScoreV3& score, const TransportV3& transport, int64_t u0, int64_t u1, int lane, const LaneV3& l,
                   int p, int row, int64_t rowAt, int64_t w, int64_t play, int entry, int64_t bar, bool fill, Candidate* out, bool history) {
	int leaves[8];
	int leafCount = activeLeaves(l, bar, leaves);
	int laneRowIndex = row % l.len;
	int cell = laneRowIndex;
	for (int k = leafCount - 1; k >= 0; k--) {
		const Node& node = l.nodes[size_t(leaves[k])];
		if (node.kind == NODE_ROT)
			cell = int(floorMod(cell - node.value, l.len));
		else if (node.kind == NODE_REV)
			cell = l.len - 1 - cell;
	}
	const Cell& hit = l.cells[size_t(cell)];
	if (!hit.present)
		return 0;
	Event events[4];
	int eventCount = 1;
	for (int k = 0; k < leafCount; k++) {
		const Node& node = l.nodes[size_t(leaves[k])];
		if (node.kind == NODE_PLY) {
			Event next[4];
			int count = 0;
			for (int e = 0; e < eventCount; e++)
				for (int q = 0; q < node.value && count < 4; q++) {
					next[count] = events[e];
					next[count].sub = events[e].sub * node.value + q;
					next[count].of = events[e].of * node.value;
					count++;
				}
			std::copy(next, next + count, events);
			eventCount = count;
		}
		else if (node.kind == NODE_DEGRADE) {
			for (int e = 0; e < eventCount; e++) {
				if (events[e].dropNode >= 0)
					continue;
				uint32_t h = chance(score.seed, DEGRADE + uint32_t(leaves[k]), uint32_t(p), uint32_t(lane), uint32_t(row), bar,
				                    uint32_t(events[e].sub));
				if (int(h % 100) >= node.value) {
					events[e].dropNode = leaves[k];
					events[e].dropDraw = h;
				}
			}
		}
	}
	// The condition is judged once per row, at its start (transforms.md).
	bool holds = true;
	Why condWhy;
	if (hit.cond.kind != COND_NONE) {
		const Cond& c = hit.cond;
		bool value = false;
		if (c.kind == COND_EVERY) {
			value = everyHolds(bar, c.a, c.b);
			condWhy.kind = WHY_EVERY;
			condWhy.bar = bar;
			condWhy.b = c.b;
		}
		else if (c.kind == COND_PROB) {
			uint32_t h = chance(score.seed, PROB, uint32_t(p), uint32_t(lane), uint32_t(row), bar, 0);
			value = int(h % 100) < c.p;
			condWhy.kind = WHY_PROB;
			condWhy.draw = h;
		}
		else if (c.kind == COND_FILL) {
			value = fill;
			condWhy.kind = WHY_FILL;
			condWhy.fill = fill;
		}
		else {
			value = bar == 0;
			condWhy.kind = WHY_FIRST;
			condWhy.bar = bar;
		}
		holds = value != c.negate;
	}
	int count = 0;
	for (int e = 0; e < eventCount; e++) {
		int64_t step = w / events[e].of;
		int64_t at = rowAt + events[e].sub * step;
		double duration = hit.hasLength ? std::min(hit.length * 480., double(rowAt + (score.geometry(p).rows - row) * w - at)) : double(step);
		if (at < u0 || at >= u1 || (!history && at + duration <= 0))
			continue;
		Candidate& c = out[count++];
		c = Candidate();
		c.at = at;
		c.duration = duration;
		c.lane = lane;
		c.cell = &hit;
		if (score.mutes[lane]) {
			c.why.kind = WHY_MUTED_SCORE;
		}
		else if (transport.busMutes[lane](at)) {
			c.why.kind = WHY_MUTED_BUS;
		}
		else if (!holds) {
			c.why = condWhy;
		}
		else if (events[e].dropNode >= 0) {
			c.why.kind = WHY_DEGRADE;
			c.why.node = events[e].dropNode;
			c.why.draw = events[e].dropDraw;
		}
		else {
			c.fires = true;
			c.why = condWhy;
		}
		c.play = play;
		c.entry = entry;
		c.pattern = p;
		c.row = row;
		c.laneRow = laneRowIndex;
		c.cellIndex = cell;
		c.sub = events[e].sub;
		c.of = events[e].of;
		c.bar = bar;
		std::copy(leaves, leaves + leafCount, c.via);
		c.viaCount = leafCount;
	}
	return count;
}

} // namespace detail

// Every candidate with u0 <= at < u1 that ends after the anchor, in order of at, then lane
// (evaluator.md#evaluate), passed to `emit(const Candidate&)`. The anchor's row comes back
// with a negative at when the anchor is inside it; held explicit lengths may
// also come from earlier rows in the anchor entry. It jumps straight
// to the row it needs, so its cost doesn't grow with the session.
template <class Emit>
void evaluate(const ScoreV3& score, const TransportV3& transport, int64_t u0, int64_t u1, Emit&& emit, bool history = false) {
	const int64_t cycle = score.cycle;
	const int64_t a = transport.tick;
	const int n = int(score.entries.size());
	if (cycle <= 0 || a < 0 || a >= cycle || n == 0)
		return;
	const int first = score.entryAt(a);
	// Locate the anchor entry for negative queries, then scan at most HOLD_TICKS
	// backward within it. Never recover notes across entry boundaries.
	const int64_t from = std::max<int64_t>(u0, 0);
	int64_t cycles = (a + from) / cycle;
	int i = score.entryAt((a + from) % cycle);
	int64_t play = cycles * n + i - first;
	int64_t playStart = cycles * cycle + score.starts[size_t(i)] - a;
	int row = std::max(0, int(std::floor(double(std::max(u0, -HOLD_TICKS) - playStart) / double(score.geometry(score.entries[size_t(i)]).w))));
	Candidate buffer[LANES * 4];
	while (playStart < u1) {
		const int p = score.entries[size_t(i)];
		const Geometry g = score.geometry(p);
		const PatternV3* pattern = score.patternAt(p);
		const int64_t visit = score.oneVisit ? play : std::min<int64_t>(play, score.runs[size_t(i)]);
		for (; row < g.rows; row++) {
			const int64_t rowAt = playStart + row * g.w;
			if (rowAt >= u1)
				return;
			if (!pattern)
				continue;
			const int64_t bar = visit * g.bars + row / g.bar;
			const bool fill = transport.fill(rowAt) || transport.busFill(rowAt);
			int count = 0;
			for (int lane = 0; lane < LANES; lane++)
				count += detail::laneRow(score, transport, u0, u1, lane, pattern->lanes[lane], p, row, rowAt, g.w, play,
				                         score.loop ? -1 : i, bar, fill, buffer + count, history);
			// A row's events all fall inside it, so ordering each row orders the whole interval.
			std::stable_sort(buffer, buffer + count,
			                 [](const Candidate& x, const Candidate& y) { return x.at != y.at ? x.at < y.at : x.lane < y.lane; });
			for (int k = 0; k < count; k++)
				emit(buffer[k]);
		}
		play++;
		if (++i == n) {
			i = 0;
			cycles++;
		}
		playStart = cycles * cycle + score.starts[size_t(i)] - a;
		row = 0;
	}
}

// -- trace strings (evaluator.md#trace), off the audio path ---------------------

inline std::string pointer(const LaneV3& lane, int node) {
	std::string out = "/" + std::to_string(lane.nodes[size_t(node)].top);
	for (int d = 0; d < lane.nodes[size_t(node)].depth; d++)
		out += "/inner";
	return out;
}

inline std::string candidateId(const Candidate& c) {
	std::string id = std::to_string(c.play) + "." + std::to_string(c.lane) + "." + std::to_string(c.row);
	return c.of > 1 ? id + "." + std::to_string(c.sub) : id;
}

inline std::string condCode(const Cond& c) {
	std::string code = c.kind == COND_EVERY ? std::to_string(c.a) + ":" + std::to_string(c.b)
	                 : c.kind == COND_PROB ? "%" + std::to_string(c.p)
	                 : c.kind == COND_FILL ? "F" : c.kind == COND_FIRST ? "1ST" : "";
	return c.negate ? "!" + code : code;
}

// The `why` string, or "" for null.
inline std::string whyText(const Candidate& c, const LaneV3& lane) {
	const Why& w = c.why;
	auto drawText = [](uint32_t h) { return "chance " + std::to_string(h) + " mod 100 = " + std::to_string(h % 100); };
	switch (w.kind) {
		case WHY_MUTED_SCORE: return "muted (score)";
		case WHY_MUTED_BUS: return "muted (bus)";
		case WHY_EVERY:
			return "bar " + std::to_string(w.bar) + " mod " + std::to_string(w.b) + " = " + std::to_string(w.bar % w.b);
		case WHY_PROB: return drawText(w.draw);
		case WHY_FILL: return w.fill ? "fill on" : "fill off";
		case WHY_FIRST: return "bar " + std::to_string(w.bar);
		case WHY_DEGRADE: return "degrade " + pointer(lane, w.node) + ": " + drawText(w.draw);
		default: return "";
	}
}

} // namespace v3
} // namespace gorgon
