// The link to HydraPy Studio: a worker thread polls GET /api/bridge, keeps the
// clock estimate, and sends CV-driven control values and transport commands.
//
// Threads: the audio thread calls poll(), setInput() and the request*()
// functions, which never block. The UI thread reads status(), controls() and
// the bindings. Everything else happens on the worker.
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "BridgeJson.hpp"
#include "BusIn.hpp"
#include "LoopbackHttp.hpp"

namespace gorgon {

constexpr int INPUTS = 4;

// What the audio thread follows. A version-3 score and its playhead's lists are too big to
// copy on the audio thread, so they come by shared pointer; the bridge frees them (see bury).
struct Snapshot {
	Playhead playhead;
	Score score;                                  // versions 1 and 2
	std::shared_ptr<const v3::ScoreV3> score3;    // version 3, else null
	std::shared_ptr<const v3::Layer> layer;       // version 3's FILL and bus layer
	BridgeClock clock;

	bool v3() const { return score3 && layer && playhead.v3; }
	double bpm() const { return v3() ? score3->bpm : score.bpm; }
};

class Bridge {
public:
	typedef std::function<double()> Now;

	// `now` is the local monotonic clock in seconds (Rack's system::getTime in
	// the module). `period` is the poll interval.
	explicit Bridge(Now now, std::string advertisement = advertisementPath(), double period = 1. / 30.)
	    : now(std::move(now)), advertisement(std::move(advertisement)), period(period), bus(new BusIn) {
		for (int i = 0; i < INPUTS; i++) {
			volts[i] = 0.f;
			connected[i] = false;
		}
		// The bus layer's sender (bus-io.md): random per module instance, never saved.
		std::random_device random;
		char id[17];
		std::snprintf(id, sizeof id, "%08x%08x", unsigned(random()), unsigned(random()));
		sender = id;
	}
	~Bridge() { stop(); }
	Bridge(const Bridge&) = delete;
	Bridge& operator=(const Bridge&) = delete;

	void start() {
		if (worker.joinable())
			return;
		running = true;
		worker = std::thread([this] { loop(); });
	}
	void stop() {
		running = false;
		if (worker.joinable())
			worker.join();
		clearBus();
	}

	// -- audio thread ---------------------------------------------------------
	// Copies the latest snapshot if it changed since `seen`. Never blocks: if
	// the worker is mid-publish, the caller keeps its copy until next time.
	bool poll(uint64_t& seen, Snapshot& into) {
		if (version.load(std::memory_order_acquire) == seen)
			return false;
		std::unique_lock<std::mutex> lock(snapshotMutex, std::try_to_lock);
		if (!lock.owns_lock())
			return false;
		into = latest;
		seen = version.load(std::memory_order_relaxed);
		return true;
	}
	void setInput(int i, bool isConnected, float v) {
		connected[i].store(isConnected, std::memory_order_relaxed);
		volts[i].store(v, std::memory_order_relaxed);
	}
	// Plain atomic stores: the worker notices within a couple of milliseconds,
	// and the audio thread never makes a system call here.
	void requestPlayToggle() { playToggle = true; }
	void requestRestart() { restart = true; }
	// BUS IN: one sample's changes for the worker to send, with BUS IN's state after them,
	// and the first tick not yet evaluated.
	void pushBus(const v3::BusChange* changes, int n, const v3::BusState& state) { bus->send(changes, n, state); }
	void pushBus(const v3::BusChange& change) { bus->push(change); }   // a change alone, for tests
	// Changes the ring had no room for (the worker resends the state after them).
	uint64_t busLost() const { return bus->lost(); }
	void setBusFrontier(int64_t tick) { busFrontier.store(tick, std::memory_order_relaxed); }
	// Whether this module plays its own bus layer: a window is outstanding or held, and
	// Studio hasn't given the layer to another module.
	bool busOutstanding() const { return busActive.load(std::memory_order_relaxed); }
	bool busRefused() const { return refused.load(std::memory_order_relaxed); }

	// -- UI thread ------------------------------------------------------------
	std::string status() {
		std::lock_guard<std::mutex> lock(uiMutex);
		return statusText;
	}
	bool linked() { return linkedFlag.load(); }
	std::vector<Control> controls() {
		std::lock_guard<std::mutex> lock(uiMutex);
		return controlList;
	}
	// "" = automatic (the Nth control not driven by a tracker lane), "-" = none.
	std::string binding(int i) {
		std::lock_guard<std::mutex> lock(uiMutex);
		return bindings[i];
	}
	void bind(int i, const std::string& name) {
		std::lock_guard<std::mutex> lock(uiMutex);
		bindings[i] = name;
		lastSent[i] = NAN;
	}
	// The control input `i` drives right now ("" if none).
	std::string target(int i) {
		std::lock_guard<std::mutex> lock(uiMutex);
		return resolve(i);
	}
	const std::string& advertisementFile() const { return advertisement; }
	const std::string& busSender() const { return sender; }

private:
	Now now;
	std::string advertisement;
	double period;
	LoopbackHttp http;
	std::thread worker;
	std::atomic<bool> running{false}, playToggle{false}, restart{false}, linkedFlag{false};

	std::mutex snapshotMutex;
	Snapshot latest;
	std::atomic<uint64_t> version{0};
	// Every version-3 score and layer published, kept until no one else holds it. The audio
	// thread only ever lets go of a reference this still holds, so a score is never freed
	// there; the worker frees it on its next pass.
	std::vector<std::shared_ptr<const void>> graveyard;

	std::atomic<float> volts[INPUTS];
	std::atomic<bool> connected[INPUTS];

	// BUS IN: changes from the audio thread, Rack's record and the lease, on the worker.
	typedef v3::BusChannel<8192> BusIn;
	std::unique_ptr<BusIn> bus;
	std::atomic<int64_t> busFrontier{0};
	std::atomic<bool> busActive{false}, refused{false};
	v3::BusRecord record;
	std::string sender;
	double busSentAt = -1e9;
	std::shared_ptr<const v3::ScoreV3> busScore;   // the score the record's read ticks come from

	// UI-visible, guarded by uiMutex.
	std::mutex uiMutex;
	std::string statusText = "Looking for Studio";
	std::vector<Control> controlList;
	std::vector<std::string> laneBound;   // controls the tracker's lanes are bound to
	std::string bindings[INPUTS];
	double lastSent[INPUTS] = {NAN, NAN, NAN, NAN};
	double lastSentAt[INPUTS] = {};

	// Worker-only.
	int port = 0;
	std::string token;
	int64_t scoreRevision = -1;
	double lastOk = -1e9;

	std::string resolve(int i) const {
		if (bindings[i] == "-")
			return "";
		if (!bindings[i].empty())
			return bindings[i];
		int n = 0;
		for (const Control& c : controlList) {
			bool lane = false;
			for (const std::string& b : laneBound)
				lane = lane || b == c.name;
			if (lane)
				continue;
			if (n++ == i)
				return c.name;
		}
		return "";
	}

	void setStatus(const std::string& s) {
		std::lock_guard<std::mutex> lock(uiMutex);
		statusText = s;
	}

	// Encodes and releases `j`. jansson writes numbers with '.', whatever the locale.
	static std::string dump(json_t* j) {
		char* d = json_dumps(j, JSON_COMPACT);
		std::string out = d ? d : "{}";
		std::free(d);
		json_decref(j);
		return out;
	}

	void bury(const std::shared_ptr<const void>& p) {
		if (p)
			graveyard.push_back(p);
	}
	void sweep() {
		graveyard.erase(std::remove_if(graveyard.begin(), graveyard.end(),
		                               [](const std::shared_ptr<const void>& p) { return p.use_count() == 1; }),
		                graveyard.end());
	}

	void loop() {
		Snapshot working;
		while (running) {
			double started = now();
			sweep();
			pollStudio(working);
			sendTransport(working);
			sendInputs();
			sendBus(working);
			// Sleep in short slices so stop() and transport presses are prompt
			// without a condition variable (see the note on requestPlayToggle).
			while (running && !playToggle && !restart && now() - started < period)
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	}

	void pollStudio(Snapshot& working) {
		if (port == 0 && !readAdvertisement(advertisement, port, token)) {
			linkedFlag = false;
			setStatus("No Studio running");
			return;
		}
		std::string path = "/api/bridge" + (scoreRevision >= 0 ? "?revision=" + std::to_string(scoreRevision) : std::string());
		double sent = now();
		HttpResponse r = http.request(port, path, token);
		double received = now();
		BridgeState state;
		std::string error;
		if (r.status == 200 && parseBridge(r.body, state, &error)) {
			if (state.hasScore) {
				if (state.score3) {
					working.score3 = state.score3;
					bury(working.score3);
				}
				else {
					working.score = state.score;
					working.score3.reset();
				}
				scoreRevision = state.playhead.revision;
				std::vector<std::string> lanes = laneBindings(r.body);
				std::lock_guard<std::mutex> lock(uiMutex);
				laneBound = lanes;
			}
			else if (scoreRevision != state.playhead.revision) {
				scoreRevision = -1;   // ask for the score again
				return;
			}
			if (state.playhead.v3 != bool(working.score3)) {
				scoreRevision = -1;   // a playhead for the other format: the score changed under us
				return;
			}
			if (!state.layer)
				working.layer.reset();
			else if (!working.layer || !(*working.layer == *state.layer)) {
				working.layer = state.layer;
				bury(working.layer);
			}
			working.playhead = state.playhead;
			working.clock.observe(sent, received, state.serverNow);
			{
				std::lock_guard<std::mutex> lock(snapshotMutex);
				latest = working;
				version.fetch_add(1, std::memory_order_release);
			}
			{
				std::lock_guard<std::mutex> lock(uiMutex);
				controlList = state.controls;
				char text[96];
				std::snprintf(text, sizeof text, "Linked  %.0f BPM%s", working.bpm(),
				              state.playhead.moving() ? "" : state.playhead.enabled ? "  paused" : "  auto off");
				statusText = text;
			}
			lastOk = received;
			linkedFlag = true;
			return;
		}
		// Studio restarted (new port or token) or went away: look again.
		if (r.status == 403 || !r.error.empty()) {
			port = 0;
			scoreRevision = -1;
		}
		if (received - lastOk > .5)
			linkedFlag = false;
		setStatus(r.status == 403 ? "Studio refused the token" : !error.empty() ? error : !r.error.empty() ? r.error : "Studio error");
	}

	static std::vector<std::string> laneBindings(const std::string& body) {
		std::vector<std::string> out;
		json_error_t e;
		json_t* root = json_loads(body.c_str(), 0, &e);
		json_t* b = json_object_get(json_object_get(root, "document"), "bindings");
		const char* key;
		json_t* value;
		json_object_foreach(b, key, value) {
			if (json_is_string(value) && *json_string_value(value))
				out.push_back(json_string_value(value));
		}
		json_decref(root);
		return out;
	}

	void sendTransport(const Snapshot& s) {
		if (port == 0)
			return;
		if (playToggle.exchange(false)) {
			json_t* body = json_object();
			if (s.playhead.moving())
				json_object_set_new(body, "playing", json_false());
			else {
				json_object_set_new(body, "enabled", json_true());
				json_object_set_new(body, "playing", json_true());
			}
			http.request(port, "/api/tracker_transport", token, dump(body));
		}
		if (restart.exchange(false)) {
			json_t* body = json_object();
			// A version-3 beat counts from the cycle's start (score-v3.md#transport).
			json_object_set_new(body, "beat", json_real(s.v3() ? 0. : s.score.cycleStart()));
			http.request(port, "/api/tracker_transport", token, dump(body));
		}
	}

	// One window per poll while there are changes to send, and at least one a second while
	// BUS IN is patched, which keeps the lease (bus-io.md#bus-in).
	void sendBus(const Snapshot& s) {
		if (s.v3())
			busScore = s.score3;
		bus->drain(record, busScore.get());
		busActive = record.active;
		if (port == 0 || !record.active || !s.v3())
			return;
		double t = now();
		if (!record.pending() && t - busSentAt < 1.)
			return;
		uint64_t upTo = 0;
		std::string body = record.window(busFrontier.load(std::memory_order_relaxed), sender, upTo);
		HttpResponse r = http.request(port, "/api/tracker_transport", token, body);
		busSentAt = t;
		if (r.status == 200) {
			record.acknowledge(upTo);   // applied, or dropped as a stale segment: answered either way
			refused = false;
		}
		else if (r.status == 400 && r.body.find("bus held by another module") != std::string::npos)
			refused = true;
		busActive = record.active;
	}

	// Removing the module or closing the patch clears every bit, sent as an all-clear.
	void clearBus() {
		bus->drain(record, busScore.get());
		if (!record.active || port == 0)
			return;
		int64_t at = std::max<int64_t>(0, busFrontier.load(std::memory_order_relaxed));
		for (int f = 0; f < v3::BUS_FUNCTIONS; f++) {
			v3::BusChange off;
			off.revision = record.revision;
			off.tick = record.tick;
			off.entry = record.entry;
			off.at = at;
			off.function = uint8_t(f);
			off.on = false;
			if (!record.lists[f].empty() && record.lists[f].back().on)
				record.apply(off, busScore.get());
		}
		uint64_t upTo = 0;
		http.request(port, "/api/tracker_transport", token, record.window(at, sender, upTo));
		record.active = false;
		busActive = false;
	}

	void sendInputs() {
		if (port == 0 || !linkedFlag)
			return;
		json_t* values = json_object();
		double t = now();
		{
			std::lock_guard<std::mutex> lock(uiMutex);
			for (int i = 0; i < INPUTS; i++) {
				if (!connected[i].load(std::memory_order_relaxed))
					continue;
				std::string name = resolve(i);
				const Control* c = nullptr;
				for (const Control& candidate : controlList)
					if (candidate.name == name)
						c = &candidate;
				if (!c)
					continue;
				double value = voltsToControl(volts[i].load(std::memory_order_relaxed), c->min, c->max);
				// Send changes beyond 1/2000 of the range, and repeat once a second.
				bool changed = !(std::fabs(value - lastSent[i]) <= (c->max - c->min) / 2000.);
				if (!changed && t - lastSentAt[i] < 1.)
					continue;
				lastSent[i] = value;
				lastSentAt[i] = t;
				json_object_set_new(values, name.c_str(), json_real(value));
			}
		}
		if (json_object_size(values) == 0) {
			json_decref(values);
			return;
		}
		json_t* body = json_object();
		json_object_set_new(body, "values", values);
		http.request(port, "/api/bridge_params", token, dump(body));
	}
};

} // namespace gorgon
