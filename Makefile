# hypr-shaders: Hyprland plugin that renders tagged windows (or what is behind them)
# through GPU shaders. Builds against the installed Hyprland headers (pkg-config hyprland).

CXX      ?= g++
CXXFLAGS ?= -O2
CXXFLAGS += -fPIC -std=c++2b -Wall -Wno-unused-parameter -Wno-c++11-narrowing -fvisibility=default
ifeq ($(CXX),g++)
    CXXFLAGS += --no-gnu-unique
endif

PKGS     = hyprland pixman-1 libdrm cairo libinput libudev wayland-server xkbcommon
INCLUDES = $(shell pkg-config --cflags $(PKGS)) -Isrc
LIBS     = $(shell pkg-config --libs cairo glesv2)

SRC    = $(wildcard src/*.cpp src/effects/*.cpp)
OBJ    = $(patsubst src/%.cpp,build/%.o,$(SRC))
DEP    = $(OBJ:.o=.d)
TARGET = hypr-shaders.so

all: $(TARGET)

$(TARGET): $(OBJ)
	# link to a temp file and rename: overwriting a .so that Hyprland has mapped in place
	# would crash the compositor; a rename gives the new build a fresh inode
	$(CXX) $(CXXFLAGS) -shared $(LDFLAGS) $^ -o $@.tmp $(LIBS)
	mv -f $@.tmp $@

build/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

clean:
	rm -rf build $(TARGET)

-include $(DEP)

.PHONY: all clean
