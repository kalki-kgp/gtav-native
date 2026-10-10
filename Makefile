# Builds the translated engine (gen/, from scripts/gen.sh) plus the native host.
# make            macOS arm64 build -> build/mac/gta
# make PLATFORM=ios   object files for iPhone -> build/ios/
PLATFORM ?= mac
OPT ?= -O2
B := build/$(PLATFORM)

ifeq ($(PLATFORM),ios)
CC := xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=17.0
else
CC := clang -arch arm64
endif

WABT := tools/wabt
INC := -I$(WABT)/wasm2c -I$(WABT)/third_party/simde -Igen -Ihost
# The module's memory is 64-bit and shared: wasm2c checks bounds explicitly; GTA_NO_BOUNDS_CHECK turns the checks off (scripts/gen.sh).
DEFS := -DWASM_RT_MEMCHECK_BOUNDS_CHECK=1 -DGTA_NO_BOUNDS_CHECK
# ORDERED=1: x86-style ordering for the engine's memory accesses (scripts/gen.sh, GTA_ORDERED_MEMORY). The CPU setting lets the
# acquire loads use the cheaper LDAPR instruction.
ORDERED ?= 1
ifeq ($(ORDERED),1)
DEFS += -DGTA_ORDERED_MEMORY
ifeq ($(PLATFORM),ios)
DEFS += -mcpu=apple-a14
else
DEFS += -mcpu=apple-m1
endif
endif
ifneq ($(PLATFORM),ios)
# On the Mac the engine heap is mapped at a fixed address (host_memory_init), so generated code addresses it with a constant
# base. macOS on Apple silicon keeps 0x1000000000-0x7000000000 for itself, so the heap sits above that. An iPhone app has too
# little address space to count on a fixed spot: there the base is a variable.
HEAP_BASE ?= 0x8000000000ull
DEFS += -DGTA_HEAP_BASE=$(HEAP_BASE)
endif
CFLAGS := $(OPT) -g0 -w $(INC) $(DEFS)

GEN_SRC := $(wildcard gen/gta_*.c)
GEN_OBJ := $(patsubst gen/%.c,$(B)/gen/%.o,$(GEN_SRC))
HOST_C := $(filter-out host/window_ios.c,$(wildcard host/*.c))
ifeq ($(PLATFORM),ios)
HOST_C += host/window_ios.c
endif
HOST_OBJ := $(patsubst host/%.c,$(B)/host/%.o,$(HOST_C)) $(patsubst host/%.cc,$(B)/host/%.o,$(wildcard host/*.cc))
DAWN := tools/dawn
DAWN_INC := -I$(DAWN)/headers/dawn-headers/include
ifeq ($(PLATFORM),ios)
# built from source with BC texture support (tools/build-dawn-ios.sh); the prebuilt iOS library has none
DAWN_LIB := tools/dawn-src/out/ios_arm64/src/dawn/native/libwebgpu_dawn.a
FRAMEWORKS := -framework Metal -framework QuartzCore -framework IOSurface -framework Foundation -framework CoreGraphics -framework UIKit -framework AudioToolbox
else
HOST_OBJ += $(B)/host/window_mac.o
DAWN_LIB := $(wildcard $(DAWN)/dawn-apple-*.xcframework)/macos-arm64_x86_64/libwebgpu_dawn.a
FRAMEWORKS := -framework Metal -framework QuartzCore -framework IOKit -framework IOSurface -framework Foundation -framework CoreGraphics -framework Cocoa -framework AudioToolbox -framework CoreAudio
endif
RT_OBJ := $(B)/rt/wasm-rt-impl.o

.PHONY: all engine clean
ifeq ($(PLATFORM),ios)
# the iPhone app (ios/app) links this library and Dawn
all: $(B)/libgta.a
else
all: $(B)/gta
endif
engine: $(GEN_OBJ)

$(B)/libgta.a: $(GEN_OBJ) $(HOST_OBJ) $(RT_OBJ)
	rm -f $@
	xcrun libtool -static -o $@ $^

$(B)/gta: $(GEN_OBJ) $(HOST_OBJ) $(RT_OBJ)
	$(CC) -o $@ $^ $(DAWN_LIB) $(FRAMEWORKS) -lc++ -lm -lpthread

# gta_0.c holds the data segments and function table: huge and not hot.
$(B)/gen/gta_0.o: gen/gta_0.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -O0 -c $< -o $@

$(B)/gen/%.o: gen/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -O0 -c $< -o $@

$(B)/gen/%.o: gen/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/host/%.o: host/%.c $(wildcard host/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -O2 -g -Wall -c $< -o $@

$(B)/host/%.o: host/%.cc $(wildcard host/*.h)
	@mkdir -p $(dir $@)
	$(CC) -x c++ -std=c++20 -O2 -g -Wall -Ihost $(DAWN_INC) -c $< -o $@

$(B)/host/%.o: host/%.m $(wildcard host/*.h)
	@mkdir -p $(dir $@)
	$(CC) -fobjc-arc -O2 -g -Wall -Ihost -c $< -o $@

$(B)/rt/%.o: $(WABT)/wasm2c/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -O2 -DWASM_RT_TRAP_HANDLER=host_trap -c $< -o $@

clean:
	rm -rf build
