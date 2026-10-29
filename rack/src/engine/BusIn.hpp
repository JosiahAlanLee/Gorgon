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
// pop() on the worker. Neither allocates or blocks. head and tail count every item ever
// pushed and popped, so an item's index is the value of tail before its pop().
template <class T, size_t N> struct SpscRing {
	T items[N];
	std::atomic<size_t> head{0}, tail{0};
	std::atomic<uint64_t> lost{0};

	// Refuses (and counts) the item when fewer than `reserve` + 1 slots are free, so items
	// pushed with a smaller reserve still find room.
	bool push(const T& item, size_t reserve = 0) {
		size_t h = head.load(std::memory_order_relaxed);
		if (h - tail.load(std::memory_order_acquire) + reserve >= N) {
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

// The audio thread's BUS IN state after the messages of one sample: what the worker
// resynchronizes its record from when the ring has dropped messages.
struct BusState {
	int64_t revision = -1, tick = 0;   // the segment
	int entry = -1;
	int64_t at = 0;                    // the sample's stamp
	uint8_t states = 0;                // every function's state, 0 while disconnected
	bool connected = false;
	uint64_t pushed = 0, lost = 0;     // the ring's head and lost count after this sample
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
	static constexpr int FILL_CAPACITY = 192;   // room for HOLD_TICKS plus 100 ms lane delay at 60 ticks/row, with margin

	bool connected = false, begun = false;
	uint8_t states = 0;
	int64_t revision = -1, tick = 0;
	int entry = -1;
	int64_t at = 0;   // the last sample's stamp
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
	int process(int word, const BusMap& map, const ScoreV3& score, int64_t segmentRevision, int64_t anchor, int startEntry,
	            int64_t frontier, BusChange* out) {
		int n = 0;
		uint8_t next = map.states(word);
		bool nowConnected = word >= 0;
		if (!begun || segmentRevision != revision || anchor != tick) {
			// A new segment: the record begins at 0 with the state at this moment.
			begun = true;
			revision = segmentRevision;
			tick = anchor;
			entry = startEntry;
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
		at = std::max<int64_t>(frontier, 0);
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

	// The state after the last process(), less the ring's counts, which BusChannel fills in.
	BusState state() const {
		BusState s;
		s.revision = revision;
		s.tick = tick;
		s.entry = entry;
		s.at = at;
		s.states = states;
		s.connected = connected;
		return s;
	}

	// `transport` with this module's bus layer in place of the snapshot's.
	TransportV3 overlay(TransportV3 transport) const {
		transport.busFill = Transitions(fill, size_t(fillCount));
		for (int l = 0; l < LANES; l++)
			transport.busMutes[l] = Transitions(mutes[l], 1);
		return transport;
	}

	// Forget FILL changes no candidate can read again. The caller supplies
	// the held-note lookback plus any delayed lane history.
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

// Bring the record to the audio thread's state `s` after the ring dropped messages: the
// lost SEGMENT, DISCONNECT and changes are restated from what the audio thread holds now.
// What a lost change did between its stamp and `s.at` can't be recovered; from `s.at` on,
// the record is right.
inline void resync(BusRecord& record, const BusState& s, const ScoreV3* score) {
	auto change = [&](int f, int64_t at, bool on) {
		BusChange c;
		c.revision = record.revision;
		c.tick = record.tick;
		c.entry = record.entry;
		c.at = at;
		c.function = uint8_t(f);
		c.on = on;
		record.apply(c, score);
	};
	auto segment = [&](uint8_t states) {
		BusChange c;
		c.kind = BusChange::SEGMENT;
		c.revision = s.revision;
		c.tick = s.tick;
		c.entry = s.entry;
		c.states = states;
		record.apply(c, score);
	};
	bool same = !record.lists[0].empty() && record.revision == s.revision && record.tick == s.tick;
	if (!s.connected) {
		if (!record.active)
			return;   // nothing held, nothing to clear
		if (record.connected && !same)
			segment(0);   // the lost SEGMENT, which the all-clear then goes in
		same = record.revision == s.revision && record.tick == s.tick;
		int64_t at = 0;
		if (same)
			at = s.at;
		else   // the record's own segment, already disconnected: clear at its latest stamp
			for (const std::vector<Transition>& l : record.lists)
				at = std::max(at, l.back().at);
		for (int f = 0; f < BUS_FUNCTIONS; f++)
			if (record.lists[f].back().on)
				change(f, at, false);
		if (record.connected) {
			BusChange d;
			d.kind = BusChange::DISCONNECT;
			d.revision = record.revision;
			d.tick = record.tick;
			record.apply(d, score);
		}
		return;
	}
	if (!same)
		segment(s.states);   // a lost new segment: the state is taken as held from its anchor
	else if (!record.connected)
		segment(0);          // a lost reconnection: from all off, as connecting reads
	for (int f = 0; f < BUS_FUNCTIONS; f++) {
		bool on = s.states >> f & 1;
		if (record.lists[f].back().on != on)
			change(f, s.at, on);
	}
}

// The audio thread's messages to the worker: the ring, and the state after the last
// sample that sent any, published through a seqlock. A full ring drops changes but
// keeps RESERVE slots for SEGMENT and DISCONNECT; and whatever it drops, the worker
// notices the count and resynchronizes from the published state, so a lost message can
// leave the record wrong only until the worker's next drain. send() neither allocates nor
// blocks.
template <size_t N> struct BusChannel {
	static constexpr size_t RESERVE = 16;   // a sample sends at most 2 SEGMENTs and a DISCONNECT
	static_assert(N > 2 * RESERVE, "room for changes");
	SpscRing<BusChange, N> ring;

	// -- audio thread
	void push(const BusChange& c) { ring.push(c, c.kind == BusChange::CHANGE ? RESERVE : 0); }
	void send(const BusChange* changes, int n, BusState state) {
		if (n <= 0)
			return;
		for (int k = 0; k < n; k++)
			push(changes[k]);
		state.pushed = ring.head.load(std::memory_order_relaxed);
		state.lost = ring.lost.load(std::memory_order_relaxed);
		uint64_t s = seq.load(std::memory_order_relaxed);
		seq.store(s + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		revision.store(state.revision, std::memory_order_relaxed);
		tick.store(state.tick, std::memory_order_relaxed);
		entry.store(state.entry, std::memory_order_relaxed);
		at.store(state.at, std::memory_order_relaxed);
		states.store(state.states, std::memory_order_relaxed);
		connected.store(state.connected, std::memory_order_relaxed);
		pushed.store(state.pushed, std::memory_order_relaxed);
		lostAt.store(state.lost, std::memory_order_relaxed);
		seq.store(s + 2, std::memory_order_release);
	}

	// -- any thread: messages dropped so far, and resynchronizations done
	uint64_t lost() const { return ring.lost.load(std::memory_order_relaxed); }
	uint64_t resyncs() const { return resynced.load(std::memory_order_relaxed); }

	// -- worker: everything in the ring into `record`, then a resync if anything was lost.
	void drain(BusRecord& record, const ScoreV3* score) {
		take(record, score);
		uint64_t now = lost();
		if (now != lostSeen) {
			lostSeen = now;
			pending = true;
		}
		if (!pending)
			return;
		BusState s;
		// The published state must come after the loss and after every message drained; the
		// audio thread publishes in the same sample, so a miss is retried on the next drain.
		for (int attempt = 0; attempt < 4; attempt++) {
			if (!read(s))
				continue;
			if (s.lost < lostSeen || s.pushed < ring.tail.load(std::memory_order_relaxed))
				return;
			resync(record, s, score);
			skipBelow = s.pushed;   // the messages the state already includes
			pending = false;
			resynced.fetch_add(1, std::memory_order_relaxed);
			take(record, score);
			return;
		}
	}

private:
	std::atomic<uint64_t> seq{0};
	std::atomic<int64_t> revision{-1}, tick{0}, at{0};
	std::atomic<int> entry{-1};
	std::atomic<uint8_t> states{0};
	std::atomic<bool> connected{false};
	std::atomic<uint64_t> pushed{0}, lostAt{0};
	std::atomic<uint64_t> resynced{0};
	// The worker's own.
	uint64_t lostSeen = 0, skipBelow = 0;
	bool pending = false;

	void take(BusRecord& record, const ScoreV3* score) {
		BusChange c;
		for (;;) {
			size_t index = ring.tail.load(std::memory_order_relaxed);
			if (!ring.pop(c))
				return;
			if (index >= skipBelow)
				record.apply(c, score);
		}
	}
	bool read(BusState& s) const {
		uint64_t before = seq.load(std::memory_order_acquire);
		s.revision = revision.load(std::memory_order_relaxed);
		s.tick = tick.load(std::memory_order_relaxed);
		s.entry = entry.load(std::memory_order_relaxed);
		s.at = at.load(std::memory_order_relaxed);
		s.states = states.load(std::memory_order_relaxed);
		s.connected = connected.load(std::memory_order_relaxed);
		s.pushed = pushed.load(std::memory_order_relaxed);
		s.lost = lostAt.load(std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_acquire);
		return !(before & 1) && seq.load(std::memory_order_relaxed) == before;
	}
};

} // namespace v3
} // namespace gorgon
