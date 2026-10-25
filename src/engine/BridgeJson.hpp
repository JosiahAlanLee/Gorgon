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

// The tracker document (hydrapy-tracker-1). Returns false if it isn't one.
inline bool parseScore(json_t* d, Score& score) {
	const char* format = json_string_value(json_object_get(d, "format"));
	if (!format || std::string(format) != "hydrapy-tracker-1")
		return false;
	score = Score();
	score.bpm = number(json_object_get(d, "bpm"), 120.);
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
					pattern.hit[l][r] = true;
					pattern.velocity[l][r] = float(number(json_object_get(hit, "vel"), 100.));
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
