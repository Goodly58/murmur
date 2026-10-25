# Murmur native build (MinGW-w64, no other dependencies).
#   mingw32-make            -> bin/Murmur.exe (windowed release) and bin/murmur_dev.exe (console, self-tests)
#   mingw32-make release    -> only the release exe
#   mingw32-make dev        -> only the dev exe
#   mingw32-make APP_VERSION=1.2.3 -> stamps that version into the exe (the release workflow does this for a tag);
#                              without it a build is 1.0.0, see src/version.h
CXX      := g++
SRC      := $(wildcard src/*.cpp)
WARN     := -Wall -Wextra -Wno-unused-parameter
ifneq ($(APP_VERSION),)
VER_PARTS := $(subst ., ,$(APP_VERSION))
VERDEFS   := -DMURMUR_VER_MAJOR=$(word 1,$(VER_PARTS)) -DMURMUR_VER_MINOR=$(or $(word 2,$(VER_PARTS)),0) -DMURMUR_VER_PATCH=$(or $(word 3,$(VER_PARTS)),0)
endif
COMMON   := -std=c++17 -municode -D_WIN32_WINNT=0x0A00 $(WARN) $(VERDEFS) -MMD -MP
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
	$(CXX) $(COMMON) -O2 -DNDEBUG $(DEBUG_INFO) -c $< -o $@
obj/dev/%.o: src/%.cpp | obj/dev
	$(CXX) $(COMMON) -O1 -c $< -o $@
obj obj/rel obj/dev bin:
	mkdir -p $@

ifneq ($(HAS_ICON),)
$(RES_OBJ): res/murmur.rc res/murmur.manifest res/murmur.ico src/version.h | obj
	windres res/murmur.rc -O coff -o $@ $(VERDEFS)
EXTRA := $(RES_OBJ)
endif

bin/Murmur.exe: $(REL_OBJ) $(EXTRA) | bin
	$(CXX) -municode -mwindows -s $(REL_OBJ) $(EXTRA) -o $@ $(LIBS)
bin/murmur_dev.exe: $(DEV_OBJ) $(EXTRA) | bin
	$(CXX) -municode -mconsole $(DEV_OBJ) $(EXTRA) -o $@ $(LIBS)
# the release code with its symbols kept, for a debugger (mingw32-make DEBUG_INFO=-g sym adds line numbers)
sym: bin/Murmur_sym.exe
bin/Murmur_sym.exe: $(REL_OBJ) $(EXTRA) | bin
	$(CXX) -municode -mwindows $(REL_OBJ) $(EXTRA) -o $@ $(LIBS)

clean:
	rm -rf obj bin/Murmur.exe bin/murmur_dev.exe bin/Murmur_sym.exe

-include $(REL_OBJ:.o=.d) $(DEV_OBJ:.o=.d)
.PHONY: all release dev sym clean
