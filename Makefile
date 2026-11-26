# Native build (Linux, for development):   make
# QNX 8.0 (after sourcing qnxsdp-env.sh):   make QNX=1 [QNXARCH=x86_64|aarch64le]
ifeq ($(QNX),1)
  QNXARCH ?= x86_64
  CC     = qcc -Vgcc_nto$(QNXARCH)
  CXX    = q++ -Vgcc_nto$(QNXARCH)_cxx
  LDLIBS_NET = -lsocket
  LDLIBS_THR =
  OUT   ?= build-qnx
else
  CC    ?= cc
  CXX   ?= c++
  LDLIBS_NET =
  LDLIBS_THR = -lpthread
  OUT   ?= build
endif
CFLAGS  += -O2 -Wall -Wextra -std=gnu11
LDLIBS  += -lm

COMMON = src/common/fm_json.c

ifeq ($(QNX),1)
all: $(OUT)/fm-plant $(OUT)/fmctl $(OUT)/fm-telemetryd $(OUT)/fm-agent
AGENT_DEFS =
AGENT_SRC  =
else
all: $(OUT)/fm-plant $(OUT)/fmctl $(OUT)/fm-agent
# On a development host there is no resource manager: the agent reads shared memory directly (still read-only).
AGENT_DEFS = -DFM_TELEMETRY_DIRECT
AGENT_SRC  = $(OUT)/fm_json.o
endif
CXXFLAGS += -O2 -Wall -std=gnu++17 -Ithird_party/cpp-httplib -Ithird_party

$(OUT):
	mkdir -p $(OUT)

$(OUT)/fm-plant: src/plant/plant.c src/plant/profiles.c src/common/fm_shm.h src/plant/profiles.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ src/plant/plant.c src/plant/profiles.c $(LDLIBS) $(LDLIBS_THR)

$(OUT)/fmctl: src/plant/fmctl.c $(COMMON) src/common/fm_json.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ src/plant/fmctl.c $(COMMON) $(LDLIBS)

$(OUT)/fm-telemetryd: src/telemetryd/telemetryd.c $(COMMON) | $(OUT)
	$(CC) $(CFLAGS) -o $@ src/telemetryd/telemetryd.c $(COMMON) $(LDLIBS)

$(OUT)/fm_json.o: $(COMMON) src/common/fm_json.h | $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $(COMMON)

$(OUT)/httplib.o: third_party/cpp-httplib/httplib.cpp third_party/cpp-httplib/httplib.h | $(OUT)
	$(CXX) $(CXXFLAGS) -w -c -o $@ third_party/cpp-httplib/httplib.cpp

$(OUT)/fm-agent: src/agent/agent.cpp $(OUT)/httplib.o $(AGENT_SRC) | $(OUT)
	$(CXX) $(CXXFLAGS) $(AGENT_DEFS) -o $@ src/agent/agent.cpp $(OUT)/httplib.o $(AGENT_SRC) $(LDLIBS_NET) $(LDLIBS_THR)

clean:
	rm -rf build build-qnx

.PHONY: all clean
