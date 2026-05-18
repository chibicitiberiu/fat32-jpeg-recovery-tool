CXX = g++
COMMON_FLAGS = -std=c++17 -Wall -Wextra -Wpedantic -Isrc/include
LDFLAGS = -lm -lpthread

RELEASE_FLAGS = $(COMMON_FLAGS) -O2 -g
DEBUG_FLAGS = $(COMMON_FLAGS) -g -O0 -DDEBUG -fsanitize=address

BUILD ?= release
ifeq ($(BUILD),debug)
  CXXFLAGS = $(DEBUG_FLAGS)
  LDFLAGS += -fsanitize=address
else
  CXXFLAGS = $(RELEASE_FLAGS)
endif

BUILD_DIR = build/$(BUILD)
TARGET = $(BUILD_DIR)/corrsim

SRCS = src/main.cpp \
       src/core/image_create.cpp \
       src/core/file_populate.cpp \
       src/core/fragment.cpp \
       src/core/usage_sim.cpp \
       src/core/ground_truth.cpp \
       src/corrupt/bitflip.cpp \
       src/corrupt/ftl_block.cpp \
       src/corrupt/metadata.cpp \
       src/corrupt/jpeg_targeted.cpp \
       src/util/log.cpp \
       src/util/rng.cpp \
       src/util/json_writer.cpp \
       src/util/subprocess.cpp \
       src/data/folder_names.cpp \
       src/data/profiles.cpp

OBJS = $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))

all: $(TARGET)
	@ln -sf $(TARGET) corrsim

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

debug:
	$(MAKE) BUILD=debug

clean:
	rm -rf build corrsim

.PHONY: all clean debug
