# Builds against the installed Hyprland's headers (pacman: hyprland). Rebuild after every Hyprland update.
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++26 -shared -fPIC -Wall --no-gnu-unique $(shell pkg-config --cflags pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon) -I/usr/include

all: minimo-desks.so

minimo-desks.so: src/main.cpp
	$(CXX) $(CXXFLAGS) src/main.cpp -o $@

clean:
	rm -f minimo-desks.so

.PHONY: all clean
