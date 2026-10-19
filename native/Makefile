# Murmur native build (MinGW-w64, no other dependencies).
#   mingw32-make            -> bin/Murmur.exe (windowed release) and bin/murmur_dev.exe (console, self-tests)
#   mingw32-make release    -> only the release exe
#   mingw32-make dev        -> only the dev exe
CXX      := g++
SRC      := $(wildcard src/*.cpp)
WARN     := -Wall -Wextra -Wno-unused-parameter
COMMON   := -std=c++17 -municode -D_WIN32_WINNT=0x0A00 $(WARN) -MMD -MP
LIBS     := -static -static-libgcc -static-libstdc++ -lpthread -lole32 -loleaut32 -luuid -lavrt -lpsapi -lwinhttp \
            -lshell32 -ld2d1 -ldwrite -lwindowscodecs -lshlwapi -lgdi32 -lcomdlg32 -ldwmapi -lcomctl32 -luser32 -ladvapi32
RES_OBJ  := obj/murmur_res.o
HAS_ICON := $(wildcard res/murmur.ico)

REL_OBJ  := $(patsubst src/%.cpp,obj/rel/%.o,$(SRC))
DEV_OBJ  := $(patsubst src/%.cpp,obj/dev/%.o,$(SRC))

all: release dev
release: bin/Murmur.exe
dev: bin/murmur_dev.exe

obj/rel/%.o: src/%.cpp | obj/rel
	$(CXX) $(COMMON) -O2 -DNDEBUG -c $< -o $@
obj/dev/%.o: src/%.cpp | obj/dev
	$(CXX) $(COMMON) -O1 -c $< -o $@
obj obj/rel obj/dev bin:
	mkdir -p $@

ifneq ($(HAS_ICON),)
$(RES_OBJ): res/murmur.rc res/murmur.manifest res/murmur.ico | obj
	windres res/murmur.rc -O coff -o $@
EXTRA := $(RES_OBJ)
endif

bin/Murmur.exe: $(REL_OBJ) $(EXTRA) | bin
	$(CXX) -municode -mwindows -s $(REL_OBJ) $(EXTRA) -o $@ $(LIBS)
bin/murmur_dev.exe: $(DEV_OBJ) $(EXTRA) | bin
	$(CXX) -municode -mconsole $(DEV_OBJ) $(EXTRA) -o $@ $(LIBS)

clean:
	rm -rf obj bin/Murmur.exe bin/murmur_dev.exe

-include $(REL_OBJ:.o=.d) $(DEV_OBJ:.o=.d)
.PHONY: all release dev clean
