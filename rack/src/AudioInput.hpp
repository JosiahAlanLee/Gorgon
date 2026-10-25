// Uses Rack's audio::Port API, so JACK and PipeWire-JACK are ordinary selectable
// Rack drivers. The same API also permits other supported audio drivers.
#pragma once
#include <audio.hpp>
#include <array>
#include <atomic>
#include <cmath>
#include "engine/PcmQueue.hpp"

struct GorgonAudioInput : rack::audio::Port {
	rack::engine::Module* owner;
	gorgon::PcmQueue queue;
	rack::dsp::SampleRateConverter<4> converter;
	std::array<rack::dsp::Frame<4>, 4096> converted{};
	std::atomic<bool> active{false};
	std::atomic<float> deviceRate{0.f};
	std::atomic<int> period{0}, channels{0}, maxBacklog{512};
	std::atomic<uint64_t> underruns{0}, discarded{0}, generation{0};
	uint64_t seenGeneration = 0;
	int requestedFrames = 0;
	bool hadAudio = false;

	explicit GorgonAudioInput(rack::engine::Module* module) : owner(module) {
		maxInputs = 4;
		maxOutputs = 0;
		converter.setQuality(6);
	}
	bool master() const { return APP->engine->getMasterModule() == owner; }
	void processInput(const float* input, int stride, int frames) override {
		requestedFrames = 0;
		int count = std::min(getNumInputs(), 4);
		float rate = getSampleRate();
		if (!input || count <= 0 || rate <= 0.f || frames <= 0 || frames > 65536) return;
		if (!APP->engine->getMasterModule()) APP->engine->setMasterModule(owner);
		if (master()) APP->engine->setSuggestedSampleRate(rate);
		float engineRate = APP->engine->getSampleRate();
		deviceRate.store(rate, std::memory_order_relaxed);
		period.store(frames, std::memory_order_relaxed);
		channels.store(count, std::memory_order_relaxed);
		maxBacklog.store(std::min<int>(gorgon::PcmQueue::CAPACITY, std::max(1, int(std::ceil(2. * frames * engineRate / rate)))), std::memory_order_relaxed);
		converter.setRates(rate, engineRate);
		converter.setChannels(count);
		int consumed = 0;
		while (consumed < frames) {
			int in = frames - consumed, out = int(converted.size());
			converter.process(input + consumed * stride, stride, &in, reinterpret_cast<float*>(converted.data()), 4, &out);
			if (!in && !out) break;
			consumed += in;
			for (int i = 0; i < out; ++i) {
				gorgon::AudioFrame sample;
				sample.generation = generation.load(std::memory_order_relaxed);
				for (int c = 0; c < count; ++c) {
					float v = converted[i].samples[c];
					sample.samples[c] = std::isfinite(v) ? v : 0.f;
				}
				if (queue.push(sample)) requestedFrames++;
			}
		}
	}
	void processBuffer(const float*, int, float*, int, int) override {
		if (master() && requestedFrames > 0) APP->engine->stepBlock(requestedFrames);
	}
	void onStartStream() override {
		converter.refreshState();
		generation.fetch_add(1, std::memory_order_release);
		active.store(true, std::memory_order_release);
	}
	void onStopStream() override {
		active.store(false, std::memory_order_release);
		generation.fetch_add(1, std::memory_order_release);
		channels.store(0, std::memory_order_relaxed);
		deviceRate.store(0.f, std::memory_order_relaxed);
		if (master()) APP->engine->setMasterModule_NoLock(nullptr);
	}
	bool read(gorgon::AudioFrame& frame) {
		uint64_t next = generation.load(std::memory_order_acquire);
		if (next != seenGeneration) {
			seenGeneration = next;
			hadAudio = false;
		}
		if (!active.load(std::memory_order_acquire)) { queue.discard(); return false; }
		while (queue.pop(frame)) {
			// Retire old stream data without throwing away the new stream's first
			// block (and potentially its first drum attack).
			if (frame.generation != next) continue;
			if (queue.available() >= uint64_t(maxBacklog.load(std::memory_order_relaxed))) {
				queue.discard();
				discarded.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			hadAudio = true;
			return true;
		}
		if (hadAudio) underruns.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
};
