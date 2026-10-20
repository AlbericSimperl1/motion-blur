CXXFLAGS ?= -O2
CXXFLAGS += -shared -fPIC -std=c++2b -Wall -fno-gnu-unique  # unique symbols make the .so impossible to unload, so a reload would keep the old code
FLAGS     = $(shell pkg-config --cflags pixman-1 libdrm hyprland cairo)

SRC = main.cpp TrailPassElement.cpp

all: pointer-trail.so

pointer-trail.so: $(SRC) TrailPassElement.hpp
	$(CXX) $(CXXFLAGS) $(FLAGS) $(SRC) -o $@

clean:
	rm -f pointer-trail.so

.PHONY: all clean
