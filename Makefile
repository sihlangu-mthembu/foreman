# Native build (Linux, for development):   make
# QNX 8.0 x86_64 (after sourcing qnxsdp-env.sh):   make QNX=1
ifeq ($(QNX),1)
  CC     = qcc -Vgcc_ntox86_64
  CXX    = q++ -Vgcc_ntox86_64_cxx
  LDLIBS_NET = -lsocket
  OUT   ?= build-qnx
else
  CC    ?= cc
  CXX   ?= c++
  LDLIBS_NET =
  OUT   ?= build
endif
CFLAGS  += -O2 -Wall -Wextra -std=c11
LDLIBS  += -lm

COMMON = src/common/fm_json.c

all: $(OUT)/fm-plant $(OUT)/fmctl

$(OUT):
	mkdir -p $(OUT)

$(OUT)/fm-plant: src/plant/plant.c src/plant/profiles.c src/common/fm_shm.h src/plant/profiles.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ src/plant/plant.c src/plant/profiles.c $(LDLIBS) -lpthread

$(OUT)/fmctl: src/plant/fmctl.c $(COMMON) src/common/fm_json.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ src/plant/fmctl.c $(COMMON) $(LDLIBS)

clean:
	rm -rf build build-qnx

.PHONY: all clean
