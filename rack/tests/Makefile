CXX ?= g++
# No fused multiply-adds: the fixtures compare HydraPy's doubles to the sample.
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -ffp-contract=off
RACK_DIR ?= ../../../deps/Rack-SDK
RACK_TEST_LDFLAGS = -Wl,--allow-shlib-undefined
JANSSON ?= -ljansson
ENGINE = ../src/engine/*.hpp

all: engine_test evaluator_test bridge_test pcm_test bytebus_test sound_test ping_test
test: all
	cmp ../src/engine/SoundLink.hpp ../../spec/sound-link.hpp
	./engine_test fixture.txt
	./evaluator_test fixture3.txt
	./evaluator_test fixture-length3.txt
	./bridge_test
	./pcm_test
	./bytebus_test
	./sound_test
	./ping_test
ping_test: ping_test.cpp $(ENGINE)
	$(CXX) $(CXXFLAGS) -pthread $< -o $@
sound_test: sound_test.cpp $(ENGINE)
	$(CXX) $(CXXFLAGS) $< $(JANSSON) -o $@
bytebus_test: bytebus_test.cpp $(ENGINE)
	$(CXX) $(CXXFLAGS) $< -o $@
pcm_test: pcm_test.cpp $(ENGINE)
	$(CXX) $(CXXFLAGS) -pthread $< -o $@
engine_test: engine_test.cpp $(ENGINE)
	$(CXX) $(CXXFLAGS) $< -o $@
evaluator_test: evaluator_test.cpp $(ENGINE)
	$(CXX) $(CXXFLAGS) $< $(JANSSON) -o $@
bridge_test: bridge_test.cpp $(ENGINE)
	$(CXX) $(CXXFLAGS) -pthread $< $(JANSSON) -o $@
sanitize: bridge_test.cpp engine_test.cpp evaluator_test.cpp $(ENGINE)
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=thread -pthread bridge_test.cpp $(JANSSON) -o bridge_tsan
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -pthread bridge_test.cpp $(JANSSON) -o bridge_asan
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined engine_test.cpp -o engine_asan
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined evaluator_test.cpp $(JANSSON) -o evaluator_asan
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=thread -pthread pcm_test.cpp -o pcm_tsan
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined bytebus_test.cpp -o bytebus_asan
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=thread -pthread ping_test.cpp -o ping_tsan
	./bridge_tsan && ./bridge_asan && ./engine_asan fixture.txt && ./evaluator_asan fixture3.txt && ./evaluator_asan fixture-length3.txt && ./pcm_tsan && ./bytebus_asan && ./ping_tsan
studio_probe: studio_probe.cpp $(ENGINE)
	$(CXX) $(CXXFLAGS) -pthread $< $(JANSSON) -o $@
# Against a real HydraPy Studio; needs HydraPy importable by python3.
e2e: studio_probe
	python3 studio_e2e.py ./studio_probe
module_test: module_test.cpp ../src/Gorgon.cpp ../src/AudioInput.hpp ../src/Layout.hpp $(ENGINE)
	$(CXX) $(CXXFLAGS) -pthread -I$(RACK_DIR)/include -I$(RACK_DIR)/dep/include -I../src $< -L$(RACK_DIR) -Wl,-rpath,$(abspath $(RACK_DIR)) -lRack $(RACK_TEST_LDFLAGS) -o $@
module_timing_test: module_timing_test.cpp ../src/Gorgon.cpp ../src/AudioInput.hpp ../src/Layout.hpp $(ENGINE)
	$(CXX) $(CXXFLAGS) -pthread -I$(RACK_DIR)/include -I$(RACK_DIR)/dep/include -I../src $< -L$(RACK_DIR) -Wl,-rpath,$(abspath $(RACK_DIR)) -lRack $(RACK_TEST_LDFLAGS) -o $@
ping_module_test: ping_module_test.cpp ../src/Ping.cpp ../src/PingLayout.hpp $(ENGINE)
	$(CXX) $(CXXFLAGS) -pthread -I$(RACK_DIR)/include -I$(RACK_DIR)/dep/include -I../src $< -L$(RACK_DIR) -Wl,-rpath,$(abspath $(RACK_DIR)) -lRack $(RACK_TEST_LDFLAGS) -o $@
clean:
	rm -f ping_test ping_tsan ping_module_test sound_test engine_test evaluator_test evaluator_asan bridge_test bridge_tsan bridge_asan engine_asan studio_probe module_test module_timing_test pcm_test pcm_tsan bytebus_test bytebus_asan
.PHONY: all test sanitize e2e clean
