// BUS IN: lane mutes and FILL from a byte bus, for version-3 scores (spec/bus-io.md).
//
// The audio thread reads the word before it evaluates each sample, stamps each change
// with the first tick it hasn't evaluated, and plays its own layer at once (BusInput).
// It hands the changes to the bridge worker through a fixed ring; the worker keeps
// Rack's record, coalesced between the ticks evaluate() reads, and sends it to Studio
// as windows (BusRecord). No Rack dependency; only BusRecord allocates.
#pragma once
#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "ScoreV3.hpp"

namespace gorgon {
namespace v3 {

// What BUS IN controls: FILL, then the six lane mutes.
constexpr int BUS_FUNCTIONS = 1 + LANES;
enum BusFunction { BUS_FILL = 0, BUS_MUTE1 = 1 };

// Which bit drives each function, or -1 for none. The default is bus-io.md's one word
// layout: mutes on bits 0–5, FILL on bit 6, bit 7 unused.
struct BusMap {
	int8_t bit[BUS_FUNCTIONS] = {6, 0, 1, 2, 3, 4, 5};

	uint8_t states(int word) const {
		uint8_t s = 0;
		if (word < 0)
			return 0;   // disconnected: every function off
		for (int f = 0; f < BUS_FUNCTIONS; f++)
			if (bit[f] >= 0 && (word >> bit[f] & 1))
				s |= uint8_t(1u << f);
		return s;
	}
};

// One message from the audio thread to the bridge worker.
struct BusChange {
	enum Kind : uint8_t { SEGMENT, CHANGE, DISCONNECT } kind = CHANGE;
	int64_t revision = -1, tick = 0;   // the segment: transport_revision and anchor tick
	int entry = -1;                    // its start.entry, -1 for null
	int64_t at = 0;                    // CHANGE: the stamp
	uint8_t function = 0;
	bool on = false;
	uint8_t states = 0;                // SEGMENT: every function's state at the anchor
};

// A single-producer, single-consumer ring of fixed capacity: push() on the audio thread,
// pop() on the worker. Neither allocates or blocks.
template <class T, size_t N> struct SpscRing {
	T items[N];
	std::atomic<size_t> head{0}, tail{0};
	std::atomic<uint64_t> lost{0};

	bool push(const T& item) {
		size_t h = head.load(std::memory_order_relaxed);
		if (h - tail.load(std::memory_order_acquire) == N) {
			lost.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		items[h % N] = item;
		head.store(h + 1, std::memory_order_release);
		return true;
	}
	bool pop(T& item) {
		size_t t = tail.load(std::memory_order_relaxed);
		if (t == head.load(std::memory_order_acquire))
			return false;
		item = items[t % N];
		tail.store(t + 1, std::memory_order_release);
		return true;
	}
};

// The first tick at or after elapsed tick `at` at which evaluate() reads `function`: the
// next row start for FILL, the next W/12 step from a row start for a mute (bus-io.md,
// Rack's record). Changes with the same read tick are seen together.
inline int64_t readTick(const ScoreV3& score, int64_t anchor, int64_t at, int function) {
	if (score.cycle <= 0)
		return at;
	int64_t tick = detail::floorMod(anchor + at, score.cycle);
	int entry = score.entryAt(tick);
	int64_t w = score.geometry(score.entries[size_t(entry)]).w;
	int64_t step = function == BUS_FILL ? w : w / 12;
	int64_t into = (tick - score.starts[size_t(entry)]) % step;
	return into ? at + step - into : at;
}

// Add a change at `at` to a list, keeping only the last change between two read ticks and
// none that ends where its interval began. A stamp of 0 sets the state at the anchor.
template <class List, class ReadTick> void coalesce(List& list, int& count, int capacity, int64_t at, bool on, ReadTick&& read) {
	if (at <= 0) {
		list[0] = {0, on};
		count = 1;
		return;
	}
	int64_t key = read(at);
	while (count > 1 && read(list[count - 1].at) == key)
		count--;
	if (list[count - 1].on == on)
		return;
	if (count == capacity) {
		// Full (only an audio-rate signal on a long row): fold the oldest change into the base.
		for (int k = 1; k < count; k++)
			list[k - 1] = list[k];
		list[0].at = 0;
		count--;
	}
	list[count++] = {at, on};
}

// The audio thread's side: the word, the stamps and the layer this module plays at once.
struct BusInput {
	static constexpr int FILL_CAPACITY = 192;   // a 7680-tick row's worth of 60-tick rows, and room

	bool connected = false, begun = false;
	uint8_t states = 0;
	int64_t revision = -1, tick = 0;
	Transition fill[FILL_CAPACITY];
	int fillCount = 1;
	Transition mutes[LANES][1];

	BusInput() {
		fill[0] = {0, false};
		for (auto& m : mutes)
			m[0] = {0, false};
	}

	// One sample, before the engine evaluates it. `word` is BUS IN's word, or -1 while
	// disconnected; `frontier` the engine's first unevaluated tick of this segment. Writes
	// the messages for the worker to `out` (at most 9) and returns how many.
	int process(int word, const BusMap& map, const ScoreV3& score, int64_t segmentRevision, int64_t anchor, int entry,
	            int64_t frontier, BusChange* out) {
		int n = 0;
		uint8_t next = map.states(word);
		bool nowConnected = word >= 0;
		if (!begun || segmentRevision != revision || anchor != tick) {
			// A new segment: the record begins at 0 with the state at this moment.
			begun = true;
			revision = segmentRevision;
			tick = anchor;
			fill[0] = {0, bool(states & 1)};
			fillCount = 1;
			for (int l = 0; l < LANES; l++)
				mutes[l][0] = {0, bool(states >> (1 + l) & 1)};
			if (connected) {
				BusChange& c = out[n++];
				c = BusChange();
				c.kind = BusChange::SEGMENT;
				c.revision = revision;
				c.tick = tick;
				c.entry = entry;
				c.states = states;
			}
		}
		if (nowConnected && !connected) {
			// Connecting is read as a change of each high bit: the segment starts from all off.
			connected = true;
			BusChange& c = out[n++];
			c = BusChange();
			c.kind = BusChange::SEGMENT;
			c.revision = revision;
			c.tick = tick;
			c.entry = entry;
			c.states = states;
		}
		int64_t at = std::max<int64_t>(frontier, 0);
		for (int f = 0; f < BUS_FUNCTIONS; f++) {
			bool on = next >> f & 1;
			if (on == bool(states >> f & 1))
				continue;
			if (f == BUS_FILL)
				coalesce(fill, fillCount, FILL_CAPACITY, at, on, [&](int64_t t) { return readTick(score, tick, t, BUS_FILL); });
			else
				mutes[f - 1][0].on = on;   // read at each candidate's own tick, none of them before `at`
			if (connected) {
				BusChange& c = out[n++];
				c = BusChange();
				c.kind = BusChange::CHANGE;
				c.revision = revision;
				c.tick = tick;
				c.entry = entry;
				c.at = at;
				c.function = uint8_t(f);
				c.on = on;
			}
		}
		states = next;
		if (!nowConnected && connected) {
			connected = false;
			BusChange& c = out[n++];
			c = BusChange();
			c.kind = BusChange::DISCONNECT;
			c.revision = revision;
			c.tick = tick;
		}
		return n;
	}

	// `transport` with this module's bus layer in place of the snapshot's.
	TransportV3 overlay(TransportV3 transport) const {
		transport.busFill = Transitions(fill, size_t(fillCount));
		for (int l = 0; l < LANES; l++)
			transport.busMutes[l] = Transitions(mutes[l], 1);
		return transport;
	}

	// Forget FILL changes no candidate can read again: rows start at most LONGEST_ROW
	// before the frontier.
	void prune(int64_t frontier, int64_t longestRow) {
		int64_t horizon = frontier - longestRow;
		int drop = 0;
		while (drop + 1 < fillCount && fill[drop + 1].at <= horizon)
			drop++;
		if (!drop)
			return;
		bool base = fill[drop].on;
		for (int k = drop; k < fillCount; k++)
			fill[k - drop] = fill[k];
		fill[0] = {0, base};
		fillCount -= drop;
	}
};

// The bridge worker's side: Rack's record of the current segment, and the windows that
// send it (bus-io.md). Allocates; never on the audio thread.
struct BusRecord {
	bool active = false;        // a cable is in, or its all-clear isn't acknowledged yet
	bool connected = false;
	int64_t revision = -1, tick = 0;
	int entry = -1;
	std::vector<Transition> lists[BUS_FUNCTIONS];
	uint64_t sequence = 0, acknowledged = 0;
	std::deque<std::pair<uint64_t, int64_t>> unacknowledged;   // (sequence, stamp)

	bool pending() const { return !unacknowledged.empty(); }

	void apply(const BusChange& c, const ScoreV3* score) {
		if (c.kind == BusChange::SEGMENT || c.revision != revision || c.tick != tick) {
			revision = c.revision;
			tick = c.tick;
			entry = c.entry;
			uint8_t states = c.kind == BusChange::SEGMENT ? c.states : 0;
			for (int f = 0; f < BUS_FUNCTIONS; f++)
				lists[f].assign(1, Transition{0, bool(states >> f & 1)});
			unacknowledged.clear();
			unacknowledged.push_back({++sequence, 0});   // the first window of a segment starts at 0
			active = connected = true;
			if (c.kind == BusChange::SEGMENT)
				return;
		}
		if (c.kind == BusChange::DISCONNECT) {
			connected = false;
			return;
		}
		std::vector<Transition>& list = lists[c.function];
		int count = int(list.size());
		list.resize(list.size() + 1);
		coalesce(list, count, INT_MAX, c.at, c.on, [&](int64_t t) {
			return score ? readTick(*score, tick, t, c.function) : t;
		});
		list.resize(size_t(count));
		unacknowledged.push_back({++sequence, c.at});
	}

	// The window from `from`, the oldest unacknowledged stamp or the first unevaluated tick,
	// whichever is earlier, as the JSON body of POST /api/tracker_transport. `upTo` is
	// the last change it carries, for acknowledge().
	std::string window(int64_t firstUnevaluated, const std::string& sender, uint64_t& upTo) const {
		int64_t from = std::max<int64_t>(0, firstUnevaluated);
		if (!unacknowledged.empty())
			from = std::min(from, unacknowledged.front().second);
		upTo = sequence;
		auto list = [&](const std::vector<Transition>& l) {
			bool state = l.front().on;
			for (const Transition& t : l)
				if (t.at <= from)
					state = t.on;
			std::string out = "[{\"at\":" + std::to_string(from) + ",\"on\":" + (state ? "true" : "false") + "}";
			for (const Transition& t : l)
				if (t.at > from)
					out += ",{\"at\":" + std::to_string(t.at) + ",\"on\":" + (t.on ? "true" : "false") + "}";
			return out + "]";
		};
		std::string mutes;
		for (int l = 0; l < LANES; l++)
			mutes += (l ? "," : "") + list(lists[1 + l]);
		return "{\"segment\":{\"transport_revision\":" + std::to_string(revision) + ",\"start\":{\"tick\":" + std::to_string(tick) +
		       ",\"entry\":" + (entry < 0 ? std::string("null") : std::to_string(entry)) + "}},\"sender\":\"" + sender +
		       "\",\"bus\":{\"from\":" + std::to_string(from) + ",\"fill\":" + list(lists[BUS_FILL]) + ",\"mutes\":[" + mutes + "]}}";
	}

	// A reply acknowledges every change in the window it answers.
	void acknowledge(uint64_t upTo) {
		while (!unacknowledged.empty() && unacknowledged.front().first <= upTo)
			unacknowledged.pop_front();
		acknowledged = std::max(acknowledged, upTo);
		if (!connected && unacknowledged.empty())
			active = false;   // the all-clear is through: nothing more to send
	}
};

} // namespace v3
} // namespace gorgon
