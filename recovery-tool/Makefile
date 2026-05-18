CXX = g++
COMMON_FLAGS = -std=c++17 -Wall -Wextra -Wpedantic -Isrc/include
LDFLAGS = -lm -lpthread

RELEASE_FLAGS = $(COMMON_FLAGS) -O2 -g
DEBUG_FLAGS = $(COMMON_FLAGS) -g -O0 -DDEBUG -fsanitize=address

# Default to release; use BUILD=debug or 'make debug'
BUILD ?= release
ifeq ($(BUILD),debug)
  CXXFLAGS = $(DEBUG_FLAGS)
  LDFLAGS += -fsanitize=address
else
  CXXFLAGS = $(RELEASE_FLAGS)
endif

BUILD_DIR = build/$(BUILD)
TARGET = $(BUILD_DIR)/sdrecov

SRCS = src/main.cpp \
       src/core/partition.cpp \
       src/core/fat32.cpp \
       src/core/fat32_autodetect.cpp \
       src/core/fat_merge.cpp \
       src/core/cluster_map.cpp \
       src/core/jpeg_parse.cpp \
       src/core/huffman.cpp \
       src/core/template_lib.cpp \
       src/core/standard_tables.cpp \
       src/search/seed.cpp \
       src/search/candidates.cpp \
       src/search/scoring.cpp \
       src/search/engine.cpp \
       src/search/boundary_check.cpp \
       src/search/chain_validate.cpp \
       src/search/dfs_explore.cpp \
       src/search/fast_path.cpp \
       src/search/file_writer.cpp \
       src/repair/header_graft.cpp \
       src/repair/thumbnail_validate.cpp \
       src/repair/rst_recovery.cpp \
       src/repair/gap_detect.cpp \
       src/util/log.cpp \
       src/util/disk_io.cpp \
       src/util/bitstream.cpp

OBJS = $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))

all: $(TARGET)
	@ln -sf $(TARGET) sdrecov

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

debug:
	$(MAKE) BUILD=debug

TEST_SRCS = $(filter-out src/main.cpp, $(SRCS))
TEST_OBJS = $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(TEST_SRCS))

test_huffman: tests/test_huffman.cpp $(TEST_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $< $(TEST_OBJS) $(LDFLAGS)

clean:
	rm -rf build sdrecov test_huffman

.PHONY: all clean debug
