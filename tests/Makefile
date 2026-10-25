CXX ?= g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
RACK_DIR ?= ../../../../deps/Rack-SDK
RACK_TEST_LDFLAGS = -Wl,--allow-shlib-undefined
JANSSON ?= -ljansson
ENGINE = ../src/engine/*.hpp

all: engine_test bridge_test
test: all
	./engine_test fixture.txt
	./bridge_test
engine_test: engine_test.cpp $(ENGINE)
	$(CXX) $(CXXFLAGS) $< -o $@
bridge_test: bridge_test.cpp $(ENGINE)
	$(CXX) $(CXXFLAGS) -pthread $< $(JANSSON) -o $@
sanitize: bridge_test.cpp engine_test.cpp $(ENGINE)
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=thread -pthread bridge_test.cpp $(JANSSON) -o bridge_tsan
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -pthread bridge_test.cpp $(JANSSON) -o bridge_asan
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined engine_test.cpp -o engine_asan
	./bridge_tsan && ./bridge_asan && ./engine_asan fixture.txt
studio_probe: studio_probe.cpp $(ENGINE)
	$(CXX) $(CXXFLAGS) -pthread $< $(JANSSON) -o $@
# Against a real HydraPy Studio; needs HydraPy importable by python3.
e2e: studio_probe
	python3 studio_e2e.py ./studio_probe
module_test: module_test.cpp ../src/Gorgon.cpp ../src/Layout.hpp $(ENGINE)
	$(CXX) $(CXXFLAGS) -pthread -I$(RACK_DIR)/include -I$(RACK_DIR)/dep/include -I../src $< -L$(RACK_DIR) -Wl,-rpath,$(abspath $(RACK_DIR)) -lRack $(RACK_TEST_LDFLAGS) -o $@
clean:
	rm -f engine_test bridge_test bridge_tsan bridge_asan engine_asan studio_probe module_test
.PHONY: all test sanitize e2e clean
