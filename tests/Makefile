# =============================================================================================== #
# kutaQ3 hook tests
#
# The DLL itself is a Win32 build (Visual Studio, MSVC, x86) and cannot be produced here. These
# tests exercise the parts of the NAME ESP that do not need Windows:
#
#   mirror  SDK/code/client/cl_sdkmirror.cpp - q3sdk.h / vmFind.h vs the real 1.32b engine headers.
#           Every mirrored size, offset and syscall number is a static_assert against the GPL
#           headers, so this target fails to *compile* when the mirror drifts.
#   core    nameEspCore.cpp (the real file the DLL is built from) driven by a fake engine syscall
#           trampoline, with the projection cross-checked against the engine's own AngleVectors()
#           compiled out of SDK/code/game/q_math.c.
#   vm      vmFind.cpp (again the real file) - the scanners that find the cgame VM record and the
#           cgame's gameState copy inside quake3.exe, driven with records built the way VM_Create()
#           and CL_ParseGamestate() build them.
#   gl      nameEsp.cpp + glText.cpp + glDraw.cpp (again, the real files) against a stub
#           <windows.h>/<gl/GL.h> that records every call, so the tags actually issued to
#           GL::Font can be asserted on.
#   vmhook.o  vmHook.cpp, compiled only. It is the Win32 half of the VM hook (PE headers,
#           VirtualQuery, Detours) and cannot run off Windows; tests/stub_win/ declares the Win32
#           surface it touches so it still gets type-checked here.
#
#   make -C tests check
# =============================================================================================== #

CC       ?= gcc
CXX      ?= g++
CFLAGS   := -std=gnu99 -O1 -g
CXXFLAGS := -std=c++11 -Wall -Wextra -Wno-unused-parameter -O1 -g

ROOT     := ..
SDK      := $(ROOT)/SDK
BIN      := build

CORE_SRCS := test_nameesp.cpp fake_engine.cpp $(ROOT)/nameEspCore.cpp
GL_SRCS   := test_gl.cpp fake_engine.cpp stub_glStateGuard.cpp \
             $(ROOT)/nameEsp.cpp $(ROOT)/nameEspCore.cpp $(ROOT)/glText.cpp $(ROOT)/glDraw.cpp
VM_SRCS   := test_vmfind.cpp $(ROOT)/vmFind.cpp

.PHONY: all check clean

all: $(BIN)/mirror $(BIN)/core $(BIN)/gl $(BIN)/vm $(BIN)/vmhook.o

check: all
	@echo "=== mirror (q3sdk.h + vmFind.h vs the SDK headers) ==="
	@./$(BIN)/mirror | tail -n 4
	@echo
	@echo "=== core (nameEspCore.cpp) ==="
	@./$(BIN)/core | tail -n 4
	@echo
	@echo "=== vm (vmFind.cpp) ==="
	@./$(BIN)/vm | tail -n 4
	@echo
	@echo "=== gl (nameEsp.cpp + glText.cpp + glDraw.cpp) ==="
	@./$(BIN)/gl | tail -n 4
	@echo
	@echo "vmhook.o compiled: vmHook.cpp type-checks against stub_win/ (Win32-only file, not run here)"

$(BIN):
	@mkdir -p $(BIN)

$(BIN)/mirror: $(SDK)/code/client/cl_sdkmirror.cpp | $(BIN)
	$(CXX) $(CXXFLAGS) -o $@ $<

$(BIN)/q_math.o: $(SDK)/code/game/q_math.c | $(BIN)
	$(CC) $(CFLAGS) -I$(SDK) -c -o $@ $<

$(BIN)/core: $(CORE_SRCS) $(BIN)/q_math.o
	$(CXX) $(CXXFLAGS) -I$(ROOT) -I. -o $@ $(CORE_SRCS) $(BIN)/q_math.o -lm

$(BIN)/vm: $(VM_SRCS) | $(BIN)
	$(CXX) $(CXXFLAGS) -I$(ROOT) -I. -o $@ $(VM_SRCS) -lm

# vmHook.cpp is Win32-only (PE headers, VirtualQuery, Detours) and cannot be *run* here, but it can
# be compiled: stub_win/ declares just the Win32 surface it touches so a typo, a wrong member name
# or a type mismatch fails here instead of in Visual Studio. -Istub_win has to win over -Istub,
# which holds the <windows.h> the GL tests use.
$(BIN)/vmhook.o: $(ROOT)/vmHook.cpp $(ROOT)/vmHook.h $(ROOT)/vmFind.h stub_win/windows.h | $(BIN)
	$(CXX) $(CXXFLAGS) -Wno-unknown-pragmas -Istub_win -Istub -I$(ROOT) -I. -c -o $@ $(ROOT)/vmHook.cpp

# -Istub has to win over the system include path: it provides <windows.h> and <gl/GL.h>
$(BIN)/gl: $(GL_SRCS) stub/windows.h stub/gl/GL.h
	$(CXX) $(CXXFLAGS) -I$(ROOT) -I. -Istub -o $@ $(GL_SRCS) -lm

clean:
	rm -rf $(BIN)
