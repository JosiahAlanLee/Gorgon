// The event trace (spec/evaluator.md#trace): a bounded ring of what the version-3 engine
// evaluated and of BUS IN's changes, which the menu dumps as JSONL in the format HydraPy
// writes. The audio thread writes records without allocating; the UI thread reads them.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ScoreV3.hpp"

namespace gorgon {
namespace v3 {

// One line, with everything it prints copied in, so a later score can't change it.
struct TraceRecord {
	enum Kind : uint8_t { CANDIDATE, BUS_FILL, BUS_MUTE } kind = CANDIDATE;
	int64_t revision = -1, tick = 0;   // the segment
	int16_t entry = -1;                // its start.entry, -1 for null
	int64_t frame = 0;                 // x_frame: Rack's engine frame when it was written
	// CANDIDATE
	int64_t at = 0, duration = 0, play = 0, bar = 0;
	int8_t lane = 0;
	bool fires = false, hasLocks = false;
	char note[4] = {};
	double vel = 0.;
	gorgon_sound::Sound locks;
	Why why;
	int8_t whyTop = 0, whyDepth = 0;   // a degrade's node, as its pointer
	int16_t originEntry = -1;
	int16_t pattern = 0, row = 0, laneRow = 0, cell = 0;
	int8_t sub = 0, of = 1, viaCount = 0;
	int8_t viaTop[8] = {}, viaDepth[8] = {};
	Cond cond;
	// BUS_FILL and BUS_MUTE
	bool on = false;
	int64_t received = 0;
};

inline TraceRecord candidateRecord(const Candidate& c, const LaneV3& lane, int64_t revision, int64_t tick, int entry, int64_t frame) {
	TraceRecord r;
	r.kind = TraceRecord::CANDIDATE;
	r.revision = revision;
	r.tick = tick;
	r.entry = int16_t(entry);
	r.frame = frame;
	r.at = c.at;
	r.duration = c.duration;
	r.play = c.play;
	r.bar = c.bar;
	r.lane = int8_t(c.lane);
	r.fires = c.fires;
	r.hasLocks = c.cell->hasLocks;
	std::memcpy(r.note, c.cell->note, sizeof r.note);
	r.vel = c.cell->vel;
	r.locks = c.cell->locks;
	r.why = c.why;
	if (c.why.kind == WHY_DEGRADE) {
		r.whyTop = int8_t(lane.nodes[size_t(c.why.node)].top);
		r.whyDepth = int8_t(lane.nodes[size_t(c.why.node)].depth);
	}
	r.originEntry = int16_t(c.entry);
	r.pattern = int16_t(c.pattern);
	r.row = int16_t(c.row);
	r.laneRow = int16_t(c.laneRow);
	r.cell = int16_t(c.cellIndex);
	r.sub = int8_t(c.sub);
	r.of = int8_t(c.of);
	r.viaCount = int8_t(c.viaCount);
	for (int k = 0; k < c.viaCount; k++) {
		r.viaTop[k] = int8_t(lane.nodes[size_t(c.via[k])].top);
		r.viaDepth[k] = int8_t(lane.nodes[size_t(c.via[k])].depth);
	}
	r.cond = c.cell->cond;
	return r;
}

// A change to this module's record of the bus layer: `function` 0 is FILL, 1–6 the mutes.
inline TraceRecord busRecord(int function, int64_t at, bool on, int64_t received, int64_t revision, int64_t tick, int entry,
                             int64_t frame) {
	TraceRecord r;
	r.kind = function == 0 ? TraceRecord::BUS_FILL : TraceRecord::BUS_MUTE;
	r.lane = int8_t(function - 1);
	r.at = at;
	r.on = on;
	r.received = received;
	r.revision = revision;
	r.tick = tick;
	r.entry = int16_t(entry);
	r.frame = frame;
	return r;
}

namespace detail {

inline std::string number(double v) {
	char text[32];
	if (v == double(int64_t(v)))
		std::snprintf(text, sizeof text, "%lld", (long long)v);
	else
		std::snprintf(text, sizeof text, "%.17g", v);
	return text;
}

inline std::string pointer(int top, int depth) {
	std::string out = "/" + std::to_string(top);
	for (int d = 0; d < depth; d++)
		out += "/inner";
	return out;
}

} // namespace detail

// The JSONL line: evaluator.md's fields, then x_frame.
inline std::string traceLine(const TraceRecord& r) {
	std::string line = "{\"transport_revision\": " + std::to_string(r.revision) + ", \"start\": {\"tick\": " + std::to_string(r.tick) +
	                   ", \"entry\": " + (r.entry < 0 ? std::string("null") : std::to_string(r.entry)) + "}, ";
	if (r.kind == TraceRecord::BUS_FILL || r.kind == TraceRecord::BUS_MUTE) {
		std::string change = "{" + (r.kind == TraceRecord::BUS_MUTE ? "\"lane\": " + std::to_string(r.lane) + ", " : std::string()) +
		                     "\"at\": " + std::to_string(r.at) + ", \"on\": " + (r.on ? "true" : "false") + "}";
		return line + "\"bus\": {\"" + (r.kind == TraceRecord::BUS_FILL ? "fill" : "mute") + "\": " + change + "}, \"received\": " +
		       std::to_string(r.received) + ", \"x_frame\": " + std::to_string(r.frame) + "}";
	}
	std::string id = std::to_string(r.play) + "." + std::to_string(r.lane) + "." + std::to_string(r.row);
	if (r.of > 1)
		id += "." + std::to_string(r.sub);
	std::string locks = "null";
	if (r.hasLocks) {
		locks = "{";
		if (r.locks.machine >= 0)
			locks += "\"machine\": " + std::to_string(r.locks.machine);
		for (int i = 0; i < gorgon_sound::PARAMS; i++)
			if (r.locks.mask & (1u << i))
				locks += std::string(locks.size() > 1 ? ", " : "") + "\"p" + std::to_string(i + 1) + "\": " + detail::number(r.locks.values[i]);
		locks += "}";
	}
	std::string why = "null";
	auto draw = [](uint32_t h) { return "chance " + std::to_string(h) + " mod 100 = " + std::to_string(h % 100); };
	switch (r.why.kind) {
		case WHY_MUTED_SCORE: why = "\"muted (score)\""; break;
		case WHY_MUTED_BUS: why = "\"muted (bus)\""; break;
		case WHY_EVERY:
			why = "\"bar " + std::to_string(r.why.bar) + " mod " + std::to_string(r.why.b) + " = " + std::to_string(r.why.bar % r.why.b) + "\"";
			break;
		case WHY_PROB: why = "\"" + draw(r.why.draw) + "\""; break;
		case WHY_FILL: why = r.why.fill ? "\"fill on\"" : "\"fill off\""; break;
		case WHY_FIRST: why = "\"bar " + std::to_string(r.why.bar) + "\""; break;
		case WHY_DEGRADE: why = "\"degrade " + detail::pointer(r.whyTop, r.whyDepth) + ": " + draw(r.why.draw) + "\""; break;
		default: break;
	}
	std::string via = "[";
	for (int k = 0; k < r.viaCount; k++)
		via += std::string(k ? ", " : "") + "\"" + detail::pointer(r.viaTop[k], r.viaDepth[k]) + "\"";
	via += "]";
	std::string cond = r.cond.kind == COND_NONE ? "null" : "\"" + condCode(r.cond) + "\"";
	return line + "\"id\": \"" + id + "\", \"at\": " + std::to_string(r.at) + ", \"lane\": " + std::to_string(r.lane) +
	       ", \"duration\": " + std::to_string(r.duration) + ", \"note\": \"" + std::string(r.note, strnlen(r.note, sizeof r.note)) +
	       "\", \"vel\": " + detail::number(r.vel) + ", \"locks\": " + locks + ", \"fires\": " + (r.fires ? "true" : "false") +
	       ", \"why\": " + why + ", \"origin\": {\"play\": " + std::to_string(r.play) + ", \"entry\": " +
	       (r.originEntry < 0 ? std::string("null") : std::to_string(r.originEntry)) + ", \"pattern\": " + std::to_string(r.pattern) +
	       ", \"row\": " + std::to_string(r.row) + ", \"lane_row\": " + std::to_string(r.laneRow) + ", \"cell\": " + std::to_string(r.cell) +
	       ", \"sub\": " + std::to_string(r.sub) + ", \"of\": " + std::to_string(r.of) + ", \"bar\": " + std::to_string(r.bar) +
	       ", \"cond\": " + cond + ", \"via\": " + via + "}, \"x_frame\": " + std::to_string(r.frame) + "}";
}

// A bounded ring of records: the audio thread writes, the UI thread copies out. Each slot is
// a seqlock over atomic words, so a copy taken while a record is rewritten is discarded,
// not torn.
class TraceRing {
public:
	explicit TraceRing(size_t capacity = 4096) : slots(capacity) {}

	void write(const TraceRecord& record) {
		uint64_t n = head.load(std::memory_order_relaxed);
		Slot& slot = slots[n % slots.size()];
		uint64_t words[WORDS] = {};
		std::memcpy(words, &record, sizeof record);
		uint64_t seq = slot.seq.load(std::memory_order_relaxed);
		slot.seq.store(seq + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		for (int w = 0; w < WORDS; w++)
			slot.words[w].store(words[w], std::memory_order_relaxed);
		slot.index.store(n, std::memory_order_relaxed);
		slot.seq.store(seq + 2, std::memory_order_release);
		head.store(n + 1, std::memory_order_release);
	}

	// The records still in the ring, oldest first.
	std::vector<TraceRecord> copy() const {
		std::vector<TraceRecord> out;
		uint64_t end = head.load(std::memory_order_acquire);
		uint64_t begin = end > slots.size() ? end - slots.size() : 0;
		for (uint64_t n = begin; n < end; n++) {
			const Slot& slot = slots[n % slots.size()];
			uint64_t before = slot.seq.load(std::memory_order_acquire), words[WORDS];
			for (int w = 0; w < WORDS; w++)
				words[w] = slot.words[w].load(std::memory_order_relaxed);
			uint64_t index = slot.index.load(std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_acquire);
			if ((before & 1) || slot.seq.load(std::memory_order_relaxed) != before || index != n)
				continue;   // being rewritten: the ring has moved past it
			TraceRecord record;
			std::memcpy(static_cast<void*>(&record), words, sizeof record);
			out.push_back(record);
		}
		return out;
	}

	uint64_t written() const { return head.load(std::memory_order_relaxed); }

private:
	static constexpr int WORDS = int((sizeof(TraceRecord) + 7) / 8);
	struct Slot {
		std::atomic<uint64_t> seq{0}, index{~uint64_t(0)};
		std::atomic<uint64_t> words[WORDS];
		Slot() {
			for (auto& w : words)
				w.store(0, std::memory_order_relaxed);
		}
	};
	std::vector<Slot> slots;
	std::atomic<uint64_t> head{0};
};

} // namespace v3
} // namespace gorgon
