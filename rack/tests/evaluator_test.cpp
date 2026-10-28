// The version-3 evaluator (ScoreV3.hpp) against HydraPy's own candidates (fixture3.txt,
// from tools/make_fixture.py --v3): every score read by the module's JSON reader, every
// candidate compared field by field, and each interval split to check invariant 2. Then
// the version-3 engine (EngineV3.hpp) against TrackerCV's signals, sample by sample, from
// the same playheads: gates, clock, reset, run, velocity and pitch on every sample, the
// automation lanes every 50 ms.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "../src/engine/BridgeJson.hpp"
#include "../src/engine/ByteBusOutput.hpp"

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

static std::vector<v3::Transition> transitions(json_t* list) {
	std::vector<v3::Transition> out;
	size_t i;
	json_t* t;
	json_array_foreach(list, i, t)
		out.push_back({json_integer_value(json_object_get(t, "at")), json_is_true(json_object_get(t, "on"))});
	return out;
}

struct Row {
	long n = 0;
	int bits = 0;
	double vel[6] = {}, pitch[6] = {};
};

// The SIGNALS section: replay the playheads TrackerCV was given, block by block, through
// EngineV3, with u computed as TrackerCV computes it, and compare.
static long signals(v3::ScoreV3& score, std::istream& in, int s) {
	const int SR = 48000, BLOCK = 480;
	std::string keyword, rest;
	int first = 0, blocks = 0, count = 0;
	in >> keyword >> first >> blocks >> count;
	CHECK(keyword == "SIGNALS", "score %d: no SIGNALS section", s);
	struct Head {
		int block = 0;
		Playhead h;
		v3::Layer layer;
		double bpm = 120.;
	};
	std::vector<Head> heads(static_cast<size_t>(count));
	for (Head& head : heads) {
		in >> keyword >> head.block;
		std::getline(in, rest);
		json_error_t e;
		json_t* j = json_loads(rest.c_str(), 0, &e);
		head.h.valid = true;
		head.h.transportRevision = json_integer_value(json_object_get(j, "transport_revision"));
		head.h.enabled = json_is_true(json_object_get(j, "enabled"));
		head.h.playing = json_is_true(json_object_get(j, "playing"));
		head.h.anchorTime = json_number_value(json_object_get(j, "anchor_time"));
		head.h.clockTime = json_number_value(json_object_get(j, "clock_time"));
		head.h.wall = json_number_value(json_object_get(j, "wall"));
		head.bpm = json_number_value(json_object_get(j, "bpm"));
		CHECK(parsePlayheadV3(j, head.h, head.layer), "score %d: playhead at block %d doesn't parse", s, head.block);
		json_decref(j);
	}
	std::vector<Row> rows;
	in >> keyword >> count;
	for (int k = 0; k < count; k++) {
		Row r;
		in >> r.n >> r.bits;
		for (double& v : r.vel)
			in >> v;
		for (double& v : r.pitch)
			in >> v;
		rows.push_back(r);
	}
	struct Auto {
		long n = 0;
		double lane[3] = {};
	};
	std::vector<Auto> autos;
	in >> keyword >> count;
	for (int k = 0; k < count; k++) {
		Auto a;
		in >> a.n >> a.lane[0] >> a.lane[1] >> a.lane[2];
		autos.push_back(a);
	}
	CHECK(!heads.empty() && !rows.empty() && heads[0].block == first && rows[0].n == long(first) * BLOCK,
	      "score %d: the signal section starts at its first block", s);
	if (heads.empty() || rows.empty())
		return 0;
	v3::EngineV3 engine;
	Options o;   // TrackerCV's defaults: 4 PPQN, half-row gates, 5 ms reset, C4 = 0 V
	size_t head = 0, row = 0, next = 0;
	int failed = 0;
	for (int k = first; k < blocks; k++) {
		while (head + 1 < heads.size() && heads[head + 1].block <= k)
			head++;
		const Head& h = heads[head];
		score.bpm = h.bpm;
		v3::TransportV3 transport = h.layer.transport(h.h.startTick);
		bool moving = h.h.playing && h.h.enabled;
		for (int i = 0; i < BLOCK; i++) {
			long n = long(k) * BLOCK + i;
			double time = double(long(k) * BLOCK) / SR + 0. + double(i) / SR;
			double u = v3::ticksAt(h.h, time, score.bpm);
			Outputs out = engine.render(score, transport, moving, u, h.h.transportRevision, o, SR);
			while (row + 1 < rows.size() && rows[row + 1].n <= n)
				row++;
			const Row& want = rows[row];
			int bits = int(out.clock > 0.f) | int(out.reset > 0.f) << 1 | int(out.run > 0.f) << 2;
			for (int l = 0; l < 6; l++)
				bits |= int(out.gate[l] > 0.f) << (3 + l);
			bool same = bits == want.bits;
			for (int l = 0; l < 6; l++)
				same = same && std::fabs(out.velocity[l] - want.vel[l]) < 1e-6 && std::fabs(out.pitch[l] - want.pitch[l]) < 1e-5;
			if (!same && failed++ < 3)
				CHECK(same, "score %d sample %ld (block %d, u %.6f): bits %x vs %x, vel1 %g vs %g, pitch1 %g vs %g", s, n, k, u,
				      bits, want.bits, out.velocity[0], want.vel[0], out.pitch[0], want.pitch[0]);
			if (next < autos.size() && autos[next].n == n) {
				for (int a = 0; a < 3; a++)
					CHECK(std::fabs(out.lane[a] - autos[next].lane[a]) < 1e-4 * (LANE_MAX[a] - LANE_MIN[a]),
					      "score %d sample %ld: lane %d %g vs %g", s, n, a, out.lane[a], autos[next].lane[a]);
				next++;
			}
		}
	}
	CHECK(next == autos.size() && row + 1 == rows.size(), "score %d: %zu of %zu automation rows and %zu of %zu signal rows reached",
	      s, next, autos.size(), row + 1, rows.size());
	CHECK(engine.dropped == 0, "score %d: %ld candidates dropped", s, engine.dropped);
	std::getline(in, rest);
	return long(blocks - first) * BLOCK;
}

static std::string text(json_t* j) {
	return json_is_string(j) ? json_string_value(j) : std::string();
}

// One candidate against its trace line.
static void compare(const v3::ScoreV3& score, const v3::Candidate& c, json_t* line, const char* where) {
	const v3::LaneV3& lane = score.patternAt(c.pattern)->lanes[c.lane];
	json_t* origin = json_object_get(line, "origin");
	std::string id = v3::candidateId(c);
	CHECK(id == text(json_object_get(line, "id")), "%s: id %s vs %s", where, id.c_str(), text(json_object_get(line, "id")).c_str());
	CHECK(c.at == json_integer_value(json_object_get(line, "at")), "%s %s: at", where, id.c_str());
	CHECK(c.lane == json_integer_value(json_object_get(line, "lane")), "%s %s: lane", where, id.c_str());
	CHECK(c.duration == json_integer_value(json_object_get(line, "duration")), "%s %s: duration", where, id.c_str());
	CHECK(std::string(c.cell->note) == text(json_object_get(line, "note")), "%s %s: note", where, id.c_str());
	CHECK(c.cell->vel == json_number_value(json_object_get(line, "vel")), "%s %s: vel", where, id.c_str());
	json_t* locks = json_object_get(line, "locks");
	CHECK(c.cell->hasLocks == !json_is_null(locks), "%s %s: locks present", where, id.c_str());
	if (c.cell->hasLocks) {
		gorgon_sound::Sound want;
		parseSoundParams(locks, want, true);
		bool same = want.machine == c.cell->locks.machine && want.mask == c.cell->locks.mask;
		for (int k = 0; k < gorgon_sound::PARAMS; k++)
			same = same && want.values[k] == c.cell->locks.values[k];
		CHECK(same, "%s %s: locks", where, id.c_str());
	}
	CHECK(c.fires == json_is_true(json_object_get(line, "fires")), "%s %s: fires", where, id.c_str());
	std::string why = v3::whyText(c, lane);
	CHECK(why == text(json_object_get(line, "why")), "%s %s: why '%s' vs '%s'", where, id.c_str(), why.c_str(),
	      text(json_object_get(line, "why")).c_str());
	json_t* entry = json_object_get(origin, "entry");
	CHECK(c.play == json_integer_value(json_object_get(origin, "play")), "%s %s: play", where, id.c_str());
	CHECK(c.entry == (json_is_null(entry) ? -1 : json_integer_value(entry)), "%s %s: entry", where, id.c_str());
	CHECK(c.pattern == json_integer_value(json_object_get(origin, "pattern")), "%s %s: pattern", where, id.c_str());
	CHECK(c.row == json_integer_value(json_object_get(origin, "row")), "%s %s: row", where, id.c_str());
	CHECK(c.laneRow == json_integer_value(json_object_get(origin, "lane_row")), "%s %s: lane_row", where, id.c_str());
	CHECK(c.cellIndex == json_integer_value(json_object_get(origin, "cell")), "%s %s: cell", where, id.c_str());
	CHECK(c.sub == json_integer_value(json_object_get(origin, "sub")), "%s %s: sub", where, id.c_str());
	CHECK(c.of == json_integer_value(json_object_get(origin, "of")), "%s %s: of", where, id.c_str());
	CHECK(c.bar == json_integer_value(json_object_get(origin, "bar")), "%s %s: bar", where, id.c_str());
	std::string cond = c.cell->cond.kind == v3::COND_NONE ? "" : v3::condCode(c.cell->cond);
	CHECK(cond == text(json_object_get(origin, "cond")), "%s %s: cond", where, id.c_str());
	json_t* via = json_object_get(origin, "via");
	bool same = int(json_array_size(via)) == c.viaCount;
	for (int k = 0; same && k < c.viaCount; k++)
		same = v3::pointer(lane, c.via[k]) == text(json_array_get(via, size_t(k)));
	CHECK(same, "%s %s: via", where, id.c_str());
	// The Rack module's trace line (Trace.hpp) is HydraPy's, plus x_frame.
	json_t* start = json_object_get(line, "start");
	json_t* entryJson = json_object_get(start, "entry");
	std::string traced = v3::traceLine(v3::candidateRecord(c, lane, json_integer_value(json_object_get(line, "transport_revision")),
	                                                       json_integer_value(json_object_get(start, "tick")),
	                                                       json_is_null(entryJson) ? -1 : int(json_integer_value(entryJson)), 7));
	json_error_t e;
	json_t* mine = json_loads(traced.c_str(), 0, &e);
	CHECK(mine && json_integer_value(json_object_get(mine, "x_frame")) == 7, "%s %s: trace line doesn't parse: %s", where, id.c_str(),
	      traced.c_str());
	json_object_del(mine, "x_frame");
	CHECK(json_equal(mine, line), "%s %s: trace line\n  rack:   %s\n  hydrapy: %s", where, id.c_str(), traced.c_str(),
	      json_dumps(line, JSON_COMPACT));
	json_decref(mine);
}

static void fixture(const char* path) {
	std::ifstream in(path);
	CHECK(bool(in), "cannot open %s", path);
	std::string format;
	int scores = 0;
	in >> format >> scores;
	CHECK(format == "GORGON_FIXTURE_3", "regenerate fixture3 with current HydraPy");
	if (format != "GORGON_FIXTURE_3")
		return;
	long compared = 0, samples = 0;
	std::string keyword, rest;
	for (int s = 0; s < scores; s++) {
		in >> keyword;
		std::getline(in, rest);
		json_error_t e;
		json_t* document = json_loads(rest.c_str(), 0, &e);
		v3::ScoreV3 score;
		CHECK(keyword == "SCORE" && parseScoreV3(document, score), "score %d doesn't parse", s);
		json_decref(document);
		int segments = 0;
		in >> keyword >> segments;
		for (int g = 0; g < segments; g++) {
			in >> keyword;
			std::getline(in, rest);
			json_t* segment = json_loads(rest.c_str(), 0, &e);
			std::vector<v3::Transition> fill = transitions(json_object_get(segment, "fill"));
			json_t* bus = json_object_get(segment, "bus");
			std::vector<v3::Transition> busFill = transitions(json_object_get(bus, "fill")), busMutes[v3::LANES];
			v3::TransportV3 transport;
			transport.tick = json_integer_value(json_object_get(json_object_get(segment, "start"), "tick"));
			transport.fill = fill;
			transport.busFill = busFill;
			for (int l = 0; l < v3::LANES; l++) {
				busMutes[l] = transitions(json_array_get(json_object_get(bus, "mutes"), size_t(l)));
				transport.busMutes[l] = busMutes[l];
			}
			int64_t u0 = json_integer_value(json_object_get(segment, "u0")), u1 = json_integer_value(json_object_get(segment, "u1"));
			json_decref(segment);
			int count = 0;
			in >> keyword >> count;
			std::getline(in, rest);
			std::vector<v3::Candidate> got;
			v3::evaluate(score, transport, u0, u1, [&](const v3::Candidate& c) { got.push_back(c); });
			CHECK(int(got.size()) == count, "score %d segment %d: %zu candidates, HydraPy %d", s, g, got.size(), count);
			char where[48];
			std::snprintf(where, sizeof where, "score %d segment %d", s, g);
			for (int k = 0; k < count; k++) {
				std::getline(in, rest);
				json_t* line = json_loads(rest.c_str(), 0, &e);
				if (k < int(got.size()))
					compare(score, got[size_t(k)], line, where);
				json_decref(line);
				compared++;
			}
			// Invariant 2: partition once at all occurrence boundaries and their neighbors.
			// Sorting/deduplicating keeps work proportional to the interval, rather than
			// evaluating both halves of the whole interval once per candidate.
			std::vector<int64_t> cuts{u0, u1};
			for (int64_t a = u0, step = (u1 - u0) / 7 + 1; a < u1; a += step)
				cuts.push_back(a);
			for (const auto& c : got)
				for (int delta : {-1, 0, 1})
					if (c.at + delta > u0 && c.at + delta < u1)
						cuts.push_back(c.at + delta);
			std::sort(cuts.begin(), cuts.end());
			cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
			std::vector<v3::Candidate> split;
			for (size_t k = 1; k < cuts.size(); k++)
				v3::evaluate(score, transport, cuts[k - 1], cuts[k], [&](const v3::Candidate& c) { split.push_back(c); });
			bool same = split.size() == got.size();
			for (size_t k = 0; same && k < got.size(); k++)
				same = v3::traceLine(v3::candidateRecord(split[k], score.patternAt(split[k].pattern)->lanes[split[k].lane], 1, transport.tick, -1, 0)) ==
				       v3::traceLine(v3::candidateRecord(got[k], score.patternAt(got[k].pattern)->lanes[got[k].lane], 1, transport.tick, -1, 0));
			CHECK(same, "score %d segment %d: a split interval differs", s, g);
		}
		samples += signals(score, in, s);
	}
	std::printf("fixture3: %ld candidates and their trace lines compared, %ld samples of signals\n", compared, samples);
}

// A cycle that isn't a whole number of clock periods (cv-semantics.md#signals): rate 8, bar 3,
// bars 1 is 180 ticks, under half of a 4 PPQN period (480 ticks). CLOCK follows elapsed
// ticks, not the cycle beat, so it keeps its period through every wrap and BUS OUT, which
// publishes on CLOCK's rising edges, keeps publishing.
static void shortCycle() {
	std::string text = "{\"format\":\"hydrapy-tracker-3\",\"bpm\":120,\"key\":2,\"loop\":false,\"pattern\":0,\"seed\":0,"
	                   "\"chain\":[0],\"mutes\":[false,false,false,false,false,false],\"sounds\":[";
	for (int l = 0; l < 6; l++)
		text += std::string(l ? "," : "") + "{\"machine\":null,\"params\":{}}";
	text += "],\"patterns\":[{\"name\":\"A01\",\"rate\":8,\"bar\":3,\"bars\":1,\"lanes\":[";
	for (int l = 0; l < 6; l++)
		text += std::string(l ? "," : "") + "{\"len\":3,\"cells\":[null,null,null],\"transforms\":[]}";
	text += "],\"keys\":{\"z\":[],\"roll\":[],\"feedback\":[]},\"easing\":{\"z\":\"smooth\",\"roll\":\"smooth\",\"feedback\":\"smooth\"},"
	        "\"scenes\":[]}";
	for (int p = 1; p < v3::PATTERNS; p++)
		text += ",null";
	text += "]}";
	json_error_t e;
	json_t* document = json_loads(text.c_str(), 0, &e);
	v3::ScoreV3 score;
	CHECK(document && parseScoreV3(document, score), "the short-cycle score doesn't parse: %s", e.text);
	json_decref(document);
	if (!document)
		return;
	CHECK(score.cycle == 180, "short cycle: %lld ticks", (long long)score.cycle);
	const double SR = 48000.;
	const int64_t start = 60;   // the anchor inside the cycle, so the first wrap comes early
	v3::EngineV3 engine;
	v3::TransportV3 transport;
	transport.tick = start;   // FILL and the bus layer empty: off
	Options o;
	ByteBusOutput bus;
	long edges = 0, strobes = 0, mismatches = 0;
	bool was = false;
	// 2 s at 120 BPM is 7680 ticks, ticks 60 to 7740: CLOCK starts high (60 is in the first half
	// of a period) and rises again at every multiple of 480, 16 times, across 42 cycle wraps.
	const long samples = long(2. * SR);
	for (long n = 0; n < samples; n++) {
		double u = double(n) / SR * score.bpm / 60. * double(v3::TICKS);
		Outputs out = engine.render(score, transport, true, u, 1, o, SR);
		bool high = out.clock > 0.f;
		double b = (double(start) + u) / double(v3::TICKS);
		double phase = b * o.ppqn;
		mismatches += high != (phase - std::floor(phase) < .5);
		edges += high && !was;
		was = high;
		strobes += bus.render(out, true, 1, BusSource::Z).strobe;
	}
	CHECK(mismatches == 0, "short cycle: CLOCK differs from frac(b * ppqn) < 0.5 on %ld samples", mismatches);
	CHECK(edges == 17, "short cycle: %ld CLOCK rising edges in 2 s, want 17", edges);
	// The transport's start publishes on the first sample, with the first edge; then one per edge.
	CHECK(strobes == 17, "short cycle: BUS OUT strobed %ld times in 2 s, want 17", strobes);
	std::printf("short cycle: %ld clock edges, %ld BUS OUT strobes\n", edges, strobes);
}

// Every supported rate and ply product, including two ply-2 nodes. Geometry is
// W = TICKS / R, with R = 4 * rate (evaluator.md); no v3 transform stretches rows.
static void candidateBound() {
	int64_t longest = 0;
	for (int64_t w : {1920, 960, 480, 240, 120, 60}) {
		for (int mode = 1; mode <= 5; mode++) {
			v3::ScoreV3 score;
			score.store.emplace_back();
			score.index[0] = 0;
			auto& pattern = score.store[0];
			pattern.geometry = {w, 3, 3, 1};
			for (auto& lane : pattern.lanes) {
				lane.len = 3;
				lane.cells.resize(3);
				for (auto& cell : lane.cells) {
					cell.present = true;
					cell.vel = 100;
					cell.midi = 60;
				}
				if (mode > 1) {
					v3::Node node;
					node.kind = v3::NODE_PLY;
					node.value = mode == 5 ? 2 : mode;
					lane.nodes.push_back(node);
					if (mode == 5) lane.nodes.push_back(node);
				}
			}
			score.buildTimeline();
			v3::TransportV3 transport;
			transport.tick = w - 1; // anchor just before the longest possible landing span ends
			std::vector<v3::Candidate> all;
			v3::evaluate(score, transport, -w, 6 * w, [&](const v3::Candidate& c) {
				all.push_back(c);
				longest = std::max(longest, c.duration);
				CHECK(c.duration <= v3::LONGEST_ROW, "candidate exceeds lookback");
				CHECK(c.duration == w / (mode == 5 ? 4 : mode), "candidate duration");
			});
			CHECK(!all.empty(), "bound case has candidates");
			// Bound the work/trace after a large forward correction, independently of
			// LONGEST_ROW, so a safe but wastefully wide lookback also fails this test.
			v3::TraceRing trace(4096);
			v3::EngineV3 jumped;
			jumped.trace = &trace;
			jumped.segmentTick = transport.tick;
			const int64_t far = 20 * v3::TICKS;
			uint64_t expected = 0;
			v3::evaluate(score, transport, far - v3::TICKS, far + 1, [&](const v3::Candidate&) { expected++; });
			jumped.advance(score, transport, double(far));
			CHECK(trace.written() == expected, "jump trace is limited to one beat: %llu vs %llu",
			      (unsigned long long)trace.written(), (unsigned long long)expected);
			// A frontier jump must retain every covering candidate, even near the end
			// of the slowest row, and through cycle wraps. Compare against full history.
			for (int64_t u : {int64_t(0), w, 4 * w}) {
				v3::EngineV3 engine;
				engine.segmentTick = transport.tick;
				engine.frontier = -w;
				engine.advance(score, transport, double(u));
				int covering = 0;
				for (const auto& c : all)
					covering += c.at <= u && u < c.at + c.duration;
				CHECK(engine.count == covering && engine.dropped == 0, "jump retains covering candidates");
			}
		}
	}
	CHECK(longest == v3::LONGEST_ROW, "lookback is tight: max duration %lld, bound %lld",
	      (long long)longest, (long long)v3::LONGEST_ROW);
}

int main(int argc, char** argv) {
	candidateBound();
	CHECK(v3::lowbias32(1) == 1753845952u && v3::lowbias32(0xffffffffu) == 1734902346u, "lowbias32");
	const uint32_t want[] = {1936593078u, 1284988505u, 1732901356u, 3891962147u, 2080199420u, 1655901407u};
	for (int bar = 0; bar < 6; bar++)
		CHECK(v3::chance(0x1234, v3::PROB, 0, 1, 12, bar, 0) == want[bar], "chance bar %d", bar);
	shortCycle();
	fixture(argc > 1 ? argv[1] : "fixture3.txt");
	if (failures)
		std::printf("%d failures\n", failures);
	else
		std::printf("evaluator_test: all passed\n");
	return failures ? 1 : 0;
}
