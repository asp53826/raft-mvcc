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

.PHONY: all test benchmark sanitize clean

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

sanitize: CXXFLAGS := -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined
sanitize: LDFLAGS += -fsanitize=address,undefined
sanitize: clean test

clean:
	rm -rf $(BUILD_DIR)
