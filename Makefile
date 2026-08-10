CXX ?= clang++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror
CPPFLAGS ?=
CPPFLAGS += -Iinclude

ifeq ($(shell uname -s),Darwin)
MACOS_SDK := $(shell xcrun --show-sdk-path)
CPPFLAGS += -isysroot $(MACOS_SDK) -isystem $(MACOS_SDK)/usr/include/c++/v1
LDFLAGS += -isysroot $(MACOS_SDK)
endif

BUILD_DIR := build
OBJECTS := $(BUILD_DIR)/raft.o $(BUILD_DIR)/mvcc.o $(BUILD_DIR)/linearizability.o
TOOL := $(BUILD_DIR)/raft_mvcc
TEST := $(BUILD_DIR)/test_raft_mvcc
BENCH := $(BUILD_DIR)/benchmark

.PHONY: all test benchmark sanitize wasm clean

all: $(TOOL)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/raft.o: src/raft.cpp include/raftmvcc/raft.h include/raftmvcc/mvcc.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/mvcc.o: src/mvcc.cpp include/raftmvcc/mvcc.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/linearizability.o: src/linearizability.cpp include/raftmvcc/linearizability.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(TOOL): src/main.cpp $(OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $^ -o $@

$(TEST): tests/test_raft_mvcc.cpp $(OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $^ -o $@

$(BENCH): bench/benchmark.cpp $(OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $^ -o $@

test: $(TEST)
	./$(TEST)

benchmark: $(BENCH)
	./$(BENCH)

wasm:
	@test -n "$(shell command -v em++ 2>/dev/null)" || (echo "em++ is required: https://emscripten.org" && exit 1)
	mkdir -p wasm/dist
	em++ -Iinclude -std=c++17 -O3 \
		-sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web,worker,node \
		-sALLOW_MEMORY_GROWTH=1 -sFILESYSTEM=0 \
		-sEXPORTED_FUNCTIONS='["_faultline_reset","_faultline_snapshot","_faultline_campaign","_faultline_tick","_faultline_isolate","_faultline_heal","_faultline_propose","_faultline_check_history"]' \
		-sEXPORTED_RUNTIME_METHODS='["ccall","UTF8ToString"]' \
		wasm/faultline.cpp src/raft.cpp src/mvcc.cpp src/linearizability.cpp \
		-o wasm/dist/faultline-engine.mjs

sanitize: CXXFLAGS := -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined
sanitize: LDFLAGS += -fsanitize=address,undefined
sanitize: clean test

clean:
	rm -rf $(BUILD_DIR)
