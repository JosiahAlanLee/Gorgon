// Parse HydraPy Studio's GET /api/bridge reply (jansson) into engine structs,
// and find Studio's advertised port and token.
#pragma once
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <jansson.h>

#include "GorgonEngine.hpp"
#include "ScoreV3.hpp"

namespace gorgon {

struct Control {
	std::string name;
	double min = 0., max = 1., value = 0.;
};

struct BridgeState {
	Playhead playhead;
	bool hasScore = false;   // false when Studio left the score out (unchanged revision)
	Score score;
	std::vector<Control> controls;
	double serverNow = 0.;
};

inline double number(json_t* j, double fallback = 0.) {
	return json_is_number(j) ? json_number_value(j) : fallback;
}

// Same spelling as studio_tracker.note_midi: C-4 is C4; AIR is unpitched.
inline int noteMidi(const char* note) {
	if (!note)
		return -1;
	std::string s(note);
	if (s.size() != 2 && s.size() != 3)
		return -1;
	int semitone;
	switch (s[0]) {
		case 'C': semitone = 0; break;
		case 'D': semitone = 2; break;
		case 'E': semitone = 4; break;
		case 'F': semitone = 5; break;
		case 'G': semitone = 7; break;
		case 'A': semitone = 9; break;
		case 'B': semitone = 11; break;
		default: return -1;
	}
	if (s.size() == 3) {
		if (s[1] == '#' && s[0] != 'B' && s[0] != 'E')
			semitone++;
		else if (s[1] != '-')
			return -1;
	}
	char octave = s.back();
	if (octave < '0' || octave > '9')
		return -1;
	int midi = (octave - '0' + 1) * 12 + semitone;
	return midi <= 127 ? midi : -1;
}

// Strict version-2 raw controls. JSON booleans are never numbers here.
inline bool rawControl(json_t* j, int high, int& value) {
    if (!json_is_number(j)) return false;
    double n = json_number_value(j);
    if (!std::isfinite(n) || n < 0 || n > high || n != std::floor(n)) return false;
    value = int(n);
    return true;
}
inline bool parseSoundParams(json_t* j, gorgon_sound::Sound& sound, bool machine) {
    if (!json_is_object(j)) return false;
    const char* key;
    json_t* raw;
    json_object_foreach(j, key, raw) {
        int value;
        if (machine && std::string(key) == "machine") {
            if (!rawControl(raw, 191, value)) return false;
            sound.machine = int16_t(value);
        } else {
            if (key[0] != 'p' || key[1] < '1' || key[1] > '8' || key[2] != '\0' || !rawControl(raw, 127, value)) return false;
            int i = key[1] - '1';
            sound.mask |= uint16_t(1u << i);
            sound.values[i] = float(value);
        }
    }
    return true;
}

// The tracker document (hydrapy-tracker-1 or -2). Returns false if it isn't one.
inline bool parseScore(json_t* d, Score& score) {
	const char* format = json_string_value(json_object_get(d, "format"));
	if (!format || (std::string(format) != "hydrapy-tracker-1" && std::string(format) != "hydrapy-tracker-2"))
		return false;
    bool v2 = std::string(format) == "hydrapy-tracker-2";
    score = Score();
    json_t* sounds = json_object_get(d, "sounds");
    if (!v2 && sounds) return false;
    if (v2) {
        if (!json_is_array(sounds) || json_array_size(sounds) != AUDIO_LANES) return false;
        for (int l = 0; l < AUDIO_LANES; ++l) {
            json_t* s = json_array_get(sounds, size_t(l));
            if (!json_is_object(s) || json_object_size(s) != 2) return false;
            json_t* m = json_object_get(s, "machine");
            int value;
            if (!json_is_null(m)) {
                if (!rawControl(m, 191, value)) return false;
                score.sounds[l].machine = int16_t(value);
            }
            if (!parseSoundParams(json_object_get(s, "params"), score.sounds[l], false)) return false;
        }
    }
	score.bpm = number(json_object_get(d, "bpm"), 120.);
	score.key = int(number(json_object_get(d, "key"), 2.));
	if (!(score.bpm >= 1.))
		return false;
	score.loop = json_is_true(json_object_get(d, "loop"));
	score.pattern = std::min(std::max(int(number(json_object_get(d, "pattern"))), 0), PATTERNS - 1);
	json_t* mutes = json_object_get(d, "mutes");
	for (int l = 0; l < AUDIO_LANES; l++)
		score.mutes[l] = json_is_true(json_array_get(mutes, size_t(l)));
	static const char* laneNames[AUTO_LANES] = {"z", "roll", "feedback"};
	json_t* patterns = json_object_get(d, "patterns");
	if (json_array_size(patterns) != PATTERNS)
		return false;
	for (int p = 0; p < PATTERNS; p++) {
		json_t* pj = json_array_get(patterns, size_t(p));
		Pattern& pattern = score.patterns[p];
		json_t* audio = json_object_get(pj, "audio");
		for (int l = 0; l < AUDIO_LANES; l++) {
			json_t* lane = json_array_get(audio, size_t(l));
			for (int r = 0; r < ROWS; r++) {
				json_t* hit = json_array_get(lane, size_t(r));
                if (json_is_object(hit)) {
                    json_t* locks = json_object_get(hit, "locks");
                    if (locks && (!v2 || !parseSoundParams(locks, pattern.locks[l][r], true))) return false;
					pattern.hit[l][r] = true;
					pattern.velocity[l][r] = float(number(json_object_get(hit, "vel"), 100.));
					int midi = noteMidi(json_string_value(json_object_get(hit, "note")));
					pattern.pitched[l][r] = midi >= 0;
					pattern.midi[l][r] = midi;
				}
			}
		}
		json_t* keys = json_object_get(pj, "keys");
		json_t* easing = json_object_get(pj, "easing");
		for (int k = 0; k < AUTO_LANES; k++) {
			Lane& lane = pattern.lanes[k];
			const char* ease = json_string_value(json_object_get(easing, laneNames[k]));
			std::string e = ease ? ease : "hold";
			lane.ease = e == "linear" ? EASE_LINEAR : e == "smooth" ? EASE_SMOOTH : EASE_HOLD;
			json_t* list = json_object_get(keys, laneNames[k]);
			size_t n = std::min(json_array_size(list), size_t(ROWS));
			lane.count = 0;
			for (size_t i = 0; i < n; i++) {
				json_t* key = json_array_get(list, i);
				lane.keys[lane.count].row = int(number(json_object_get(key, "r")));
				lane.keys[lane.count].value = number(json_object_get(key, "v"));
				lane.count++;
			}
		}
	}
	return true;
}

// -- score format 3 (spec/score-v3.md, evaluator.md, transforms.md) -------------------

inline bool integerIn(json_t* j, long long low, long long high, long long& value) {
	if (!json_is_integer(j))
		return false;
	value = json_integer_value(j);
	return value >= low && value <= high;
}

// One transform node and, for every, its inner, appended to the lane in pre-order.
inline bool parseTransform(json_t* j, v3::LaneV3& lane, int top, int depth, int& product) {
	const char* kind = json_string_value(json_object_get(j, "kind"));
	if (!json_is_object(j) || !kind)
		return false;
	std::string k(kind);
	v3::Node node;
	node.top = top;
	node.depth = depth;
	long long value = 0, a = 0, b = 0;
	size_t fields = json_object_size(j);
	if (k == "every") {
		if (depth >= 2 || fields != 4 || !integerIn(json_object_get(j, "a"), 1, 8, a) || !integerIn(json_object_get(j, "b"), 1, 8, b) || a > b)
			return false;
		node.kind = v3::NODE_EVERY;
		node.a = int(a);
		node.b = int(b);
		lane.nodes.push_back(node);
		return parseTransform(json_object_get(j, "inner"), lane, top, depth + 1, product);
	}
	if (k == "rev") {
		if (fields != 1)
			return false;
		node.kind = v3::NODE_REV;
	}
	else if (k == "rot" && fields == 2 && integerIn(json_object_get(j, "k"), -127, 127, value))
		node.kind = v3::NODE_ROT;
	else if (k == "ply" && fields == 2 && integerIn(json_object_get(j, "n"), 2, 4, value)) {
		node.kind = v3::NODE_PLY;
		product *= int(value);
	}
	else if (k == "degrade" && fields == 2 && integerIn(json_object_get(j, "p"), 1, 99, value))
		node.kind = v3::NODE_DEGRADE;
	else
		return false;
	node.value = int(value);
	lane.nodes.push_back(node);
	return true;
}

inline bool parseCond(json_t* j, v3::Cond& cond) {
	const char* kind = json_string_value(json_object_get(j, "kind"));
	if (!json_is_object(j) || !kind)
		return false;
	json_t* negate = json_object_get(j, "not");
	if (negate && !json_is_boolean(negate))
		return false;
	cond.negate = json_is_true(negate);
	size_t fields = json_object_size(j) - (negate ? 1 : 0);
	std::string k(kind);
	long long a = 0, b = 0, p = 0;
	if (k == "every" && fields == 3 && integerIn(json_object_get(j, "a"), 1, 8, a) && integerIn(json_object_get(j, "b"), 1, 8, b) && a <= b) {
		cond.kind = v3::COND_EVERY;
		cond.a = int(a);
		cond.b = int(b);
	}
	else if (k == "prob" && fields == 2 && integerIn(json_object_get(j, "p"), 1, 99, p)) {
		cond.kind = v3::COND_PROB;
		cond.p = int(p);
	}
	else if (k == "fill" && fields == 1)
		cond.kind = v3::COND_FILL;
	else if (k == "first" && fields == 1)
		cond.kind = v3::COND_FIRST;
	else
		return false;
	return true;
}

inline bool parsePatternV3(json_t* j, v3::PatternV3& pattern) {
	double rate = number(json_object_get(j, "rate"), -1.);
	long long bar = 0, bars = 0;
	int perQuarter = int(std::lround(rate * 4.));
	if (perQuarter < 1 || perQuarter > 32 || (perQuarter & (perQuarter - 1)) || perQuarter != rate * 4.)
		return false;
	if (!integerIn(json_object_get(j, "bar"), 1, 32, bar) || !integerIn(json_object_get(j, "bars"), 1, 8, bars) || bar * bars > v3::MAX_ROWS)
		return false;
	pattern.geometry.w = v3::TICKS / perQuarter;
	pattern.geometry.bar = int(bar);
	pattern.geometry.bars = int(bars);
	pattern.geometry.rows = int(bar * bars);
	json_t* lanes = json_object_get(j, "lanes");
	if (json_array_size(lanes) != v3::LANES)
		return false;
	for (int l = 0; l < v3::LANES; l++) {
		json_t* lj = json_array_get(lanes, size_t(l));
		v3::LaneV3& lane = pattern.lanes[l];
		long long len = 0;
		json_t* cells = json_object_get(lj, "cells");
		if (!integerIn(json_object_get(lj, "len"), 1, pattern.geometry.rows, len) || json_array_size(cells) != size_t(len))
			return false;
		lane.len = int(len);
		lane.cells.assign(size_t(len), v3::Cell());
		for (long long r = 0; r < len; r++) {
			json_t* hit = json_array_get(cells, size_t(r));
			if (json_is_null(hit))
				continue;
			const char* note = json_string_value(json_object_get(hit, "note"));
			json_t* vel = json_object_get(hit, "vel");
			if (!json_is_object(hit) || !note || !json_is_number(vel))
				return false;
			v3::Cell& cell = lane.cells[size_t(r)];
			int midi = noteMidi(note);
			if (midi < 0 && std::string(note) != "AIR")
				return false;
			cell.midi = midi;
			std::snprintf(cell.note, sizeof cell.note, "%s", note);
			cell.vel = json_number_value(vel);
			if (!(cell.vel >= 0. && cell.vel <= 127.))
				return false;
			cell.present = cell.vel > 0.;
			json_t* locks = json_object_get(hit, "locks");
			if (locks && !parseSoundParams(locks, cell.locks, true))
				return false;
			cell.hasLocks = locks != nullptr;
			json_t* cond = json_object_get(hit, "cond");
			if (cond && !parseCond(cond, cell.cond))
				return false;
		}
		json_t* transforms = json_object_get(lj, "transforms");
		if (!json_is_array(transforms) || json_array_size(transforms) > 8)
			return false;
		int product = 1;
		for (size_t t = 0; t < json_array_size(transforms); t++)
			if (!parseTransform(json_array_get(transforms, t), lane, int(t), 0, product))
				return false;
		if (product > 4)
			return false;
	}
	return true;
}

// A hydrapy-tracker-3 document into a ScoreV3, its timeline built. Built off the audio
// thread: it allocates. Returns false for anything that isn't a valid version-3 score.
inline bool parseScoreV3(json_t* d, v3::ScoreV3& score) {
	const char* format = json_string_value(json_object_get(d, "format"));
	if (!format || std::string(format) != "hydrapy-tracker-3")
		return false;
	score = v3::ScoreV3();
	score.bpm = number(json_object_get(d, "bpm"), 120.);
	score.key = int(number(json_object_get(d, "key"), 2.));
	if (!(score.bpm >= 1.) || !json_is_boolean(json_object_get(d, "loop")))
		return false;
	score.loop = json_is_true(json_object_get(d, "loop"));
	long long value = 0;
	if (!integerIn(json_object_get(d, "pattern"), 0, v3::PATTERNS - 1, value))
		return false;
	score.pattern = int(value);
	if (!integerIn(json_object_get(d, "seed"), 0, 0xffffffffLL, value))
		return false;
	score.seed = uint32_t(value);
	json_t* chain = json_object_get(d, "chain");
	size_t entries = json_array_size(chain);
	if (entries < 1 || entries > 256)
		return false;
	score.chain.clear();
	for (size_t i = 0; i < entries; i++) {
		if (!integerIn(json_array_get(chain, i), 0, v3::PATTERNS - 1, value))
			return false;
		score.chain.push_back(int(value));
	}
	json_t* mutes = json_object_get(d, "mutes");
	json_t* sounds = json_object_get(d, "sounds");
	if (json_array_size(mutes) != v3::LANES || json_array_size(sounds) != v3::LANES)
		return false;
	for (int l = 0; l < v3::LANES; l++) {
		score.mutes[l] = json_is_true(json_array_get(mutes, size_t(l)));
		json_t* s = json_array_get(sounds, size_t(l));
		json_t* m = json_object_get(s, "machine");
		int machine;
		if (!json_is_object(s) || json_object_size(s) != 2)
			return false;
		if (!json_is_null(m)) {
			if (!rawControl(m, 191, machine))
				return false;
			score.sounds[l].machine = int16_t(machine);
		}
		if (!parseSoundParams(json_object_get(s, "params"), score.sounds[l], false))
			return false;
	}
	json_t* patterns = json_object_get(d, "patterns");
	if (json_array_size(patterns) != v3::PATTERNS)
		return false;
	for (int p = 0; p < v3::PATTERNS; p++) {
		json_t* pj = json_array_get(patterns, size_t(p));
		if (json_is_null(pj))
			continue;
		v3::PatternV3 pattern;
		if (!json_is_object(pj) || !parsePatternV3(pj, pattern))
			return false;
		score.index[p] = int16_t(score.store.size());
		score.store.push_back(std::move(pattern));
	}
	score.buildTimeline();
	return true;
}

inline bool parseBridge(const std::string& body, BridgeState& state, std::string* error = nullptr) {
	json_error_t e;
	json_t* root = json_loads(body.c_str(), 0, &e);
	if (!json_is_object(root)) {
		if (error)
			*error = "bad JSON from Studio";
		json_decref(root);
		return false;
	}
	bool ok = json_is_integer(json_object_get(root, "revision"));
	if (ok) {
		Playhead& h = state.playhead;
		h.valid = true;
		h.revision = json_integer_value(json_object_get(root, "revision"));
		json_t* transport = json_object_get(root, "transport_revision");
		h.transportRevision = json_is_integer(transport) ? json_integer_value(transport) : -1;
		h.enabled = json_is_true(json_object_get(root, "enabled"));
		h.playing = json_is_true(json_object_get(root, "playing"));
		h.anchorBeat = number(json_object_get(root, "anchor_beat"));
		h.anchorTime = number(json_object_get(root, "anchor_time"));
		h.clockTime = number(json_object_get(root, "clock_time"));
		h.wall = number(json_object_get(root, "wall"));
		state.serverNow = number(json_object_get(root, "server_now"));
		json_t* doc = json_object_get(root, "document");
		state.hasScore = doc != nullptr;
		if (doc && !parseScore(doc, state.score)) {
			ok = false;
			if (error)
				*error = "Studio sent an unknown score format";
		}
		state.controls.clear();
		size_t i;
		json_t* c;
		json_array_foreach(json_object_get(root, "controls"), i, c) {
			const char* name = json_string_value(json_object_get(c, "name"));
			if (!name)
				continue;
			state.controls.push_back({name, number(json_object_get(c, "min")), number(json_object_get(c, "max"), 1.),
			                          number(json_object_get(c, "value"))});
		}
	}
	else if (error) {
		*error = "Studio is starting";
	}
	json_decref(root);
	return ok;
}

// Studio's advertisement: HYDRAPY_STUDIO_FILE, else ~/.hydrapy/studio.json.
inline std::string advertisementPath() {
	if (const char* override = std::getenv("HYDRAPY_STUDIO_FILE"))
		if (*override)
			return override;
#ifdef _WIN32
	const char* home = std::getenv("USERPROFILE");
	return std::string(home ? home : ".") + "\\.hydrapy\\studio.json";
#else
	const char* home = std::getenv("HOME");
	return std::string(home ? home : ".") + "/.hydrapy/studio.json";
#endif
}

inline bool readAdvertisement(const std::string& path, int& port, std::string& token) {
	std::ifstream file(path);
	if (!file)
		return false;
	std::stringstream text;
	text << file.rdbuf();
	json_error_t e;
	json_t* root = json_loads(text.str().c_str(), 0, &e);
	bool ok = json_is_object(root) && json_is_integer(json_object_get(root, "port")) &&
	          json_is_string(json_object_get(root, "token"));
	if (ok) {
		port = int(json_integer_value(json_object_get(root, "port")));
		token = json_string_value(json_object_get(root, "token"));
	}
	json_decref(root);
	return ok;
}

} // namespace gorgon
