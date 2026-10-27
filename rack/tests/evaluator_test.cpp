// The version-3 evaluator (ScoreV3.hpp) against HydraPy's own candidates (fixture3.txt,
// from tools/make_fixture.py --v3): every score read by the module's JSON reader, every
// candidate compared field by field, and each interval split to check invariant 2.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "../src/engine/BridgeJson.hpp"

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
	long compared = 0;
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
			// Invariant 2: the same candidates whatever the interval is split into.
			std::vector<v3::Candidate> split;
			for (int64_t a = u0, step = (u1 - u0) / 7 + 1; a < u1; a += step)
				v3::evaluate(score, transport, a, std::min(a + step, u1), [&](const v3::Candidate& c) { split.push_back(c); });
			bool same = split.size() == got.size();
			for (size_t k = 0; same && k < got.size(); k++)
				same = split[k].at == got[k].at && split[k].lane == got[k].lane && split[k].sub == got[k].sub && split[k].fires == got[k].fires;
			CHECK(same, "score %d segment %d: a split interval differs", s, g);
		}
	}
	std::printf("fixture3: %ld candidates compared\n", compared);
}

int main(int argc, char** argv) {
	CHECK(v3::lowbias32(1) == 1753845952u && v3::lowbias32(0xffffffffu) == 1734902346u, "lowbias32");
	const uint32_t want[] = {1936593078u, 1284988505u, 1732901356u, 3891962147u, 2080199420u, 1655901407u};
	for (int bar = 0; bar < 6; bar++)
		CHECK(v3::chance(0x1234, v3::PROB, 0, 1, 12, bar, 0) == want[bar], "chance bar %d", bar);
	fixture(argc > 1 ? argv[1] : "fixture3.txt");
	if (failures)
		std::printf("%d failures\n", failures);
	else
		std::printf("evaluator_test: all passed\n");
	return failures ? 1 : 0;
}
