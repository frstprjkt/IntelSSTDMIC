# Build IntelSSTDMIC.kext on macOS (Xcode CLT).
#   make SDK=/path/to/MacKernelSDK LILU=/path/to/Lilu.kext/Contents/Resources
SDK      ?= $(CURDIR)/../MacKernelSDK
LILU     ?= $(CURDIR)/../Lilu.kext/Contents/Resources
# Intel production-signed SOF firmwares (the CSE rejects community-signed builds: ROM error 44).
# Both images are compiled into the binary: an OpenCore-injected kext has no readable directory.
FW_TGL   = Firmware/TGL/sof-tgl.ri
FW_CML   = Firmware/CML/sof-cml.ri
FW_ADL   = Firmware/ADL/sof-adl.ri
FW_RPL   = Firmware/RPL/sof-rpl.ri
LAYOUTS  = ../Layouts/Layouts.plist
LAYOUTPY = ../tools/embed-layouts.py
VERSION  = 1.0.4
SRC      = IntelSSTDMIC
OBJS     = build/IntelSSTDMIC.o build/LayoutConfig.o build/HdaAudioEngine.o build/IntelDspAudioEngine.o \
           build/DspEngines.o build/DspAudioEngineFactory.o build/DspAudioEngineManager.o \
           build/HookManager.o build/LiluGlue.o build/plugin_start.o build/Firmware.o \
           build/LayoutsData.o build/kmod_info.o
OUT      = build/IntelSSTDMIC.kext
ARCH     = -arch x86_64 -mmacosx-version-min=10.15
CXXFLAGS = $(ARCH) -std=gnu++17 -O2 -Wall -Wno-unused-private-field -Wno-unused-parameter \
           -nostdinc -isystem $(SDK)/Headers -mkernel -fapple-kext -fno-rtti -fno-exceptions \
           -fno-builtin -fno-common -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
           -I$(SRC) -I$(LILU) -DPRODUCT_NAME=IntelSSTDMIC -DMODULE_VERSION=$(VERSION) \
           -DLILU_CUSTOM_IOKIT_INIT=1 -DLILU_CUSTOM_KMOD_INIT=1 -Wno-deprecated-declarations
CFLAGS   = $(ARCH) -std=gnu11 -O2 -nostdinc -isystem $(SDK)/Headers -mkernel -fno-builtin -fno-common -DKERNEL
CCKEXT   = $(shell clang -print-resource-dir)/lib/darwin/libclang_rt.cc_kext.a
LDFLAGS  = $(ARCH) -nostdlib -Xlinker -kext -Xlinker -export_dynamic -L$(SDK)/Library/x86_64 -lkmod \
           $(wildcard $(CCKEXT))

all: $(OUT)

build/%.o: $(SRC)/%.cpp $(SRC)/*.hpp
	@mkdir -p build
	clang++ $(CXXFLAGS) -c $< -o $@

build/Firmware.c: $(FW_TGL) $(FW_CML) $(FW_ADL) $(FW_RPL) ../tools/gen-fw.py
	@mkdir -p build
	python3 ../tools/gen-fw.py --tgl $(FW_TGL) --cml $(FW_CML) --adl $(FW_ADL) --rpl $(FW_RPL) -o $@

build/Firmware.o: build/Firmware.c
	clang $(CFLAGS) -c $< -o $@

build/kmod_info.o: $(SRC)/kmod_info.c
	@mkdir -p build
	clang $(CFLAGS) -c $< -o $@

build/plugin_start.o: $(LILU)/Library/plugin_start.cpp
	@mkdir -p build
	clang++ $(CXXFLAGS) -c $< -o $@

build/Info.plist: $(SRC)/Info.plist $(LAYOUTS) $(LAYOUTPY)
	@mkdir -p build
	python3 $(LAYOUTPY) --layouts $(LAYOUTS) --info $(SRC)/Info.plist \
		--info-out build/Info.plist --cpp-out build/LayoutsData.cpp

build/LayoutsData.cpp: build/Info.plist
	@mkdir -p build
	@test -f $@ || python3 $(LAYOUTPY) --layouts $(LAYOUTS) --info $(SRC)/Info.plist \
		--info-out build/Info.plist --cpp-out $@

build/LayoutsData.o: build/LayoutsData.cpp
	clang++ $(CXXFLAGS) -c $< -o $@

$(OUT): $(OBJS) $(SRC)/Info.plist build/Info.plist
	rm -rf $(OUT) && mkdir -p $(OUT)/Contents/MacOS
	clang++ $(LDFLAGS) $(OBJS) -o $(OUT)/Contents/MacOS/IntelSSTDMIC
	cp build/Info.plist $(OUT)/Contents/Info.plist
	codesign -f -s - $(OUT) || true
	mkdir build/dist
	cd build && ditto -c -k --keepParent IntelSSTDMIC.kext dist/IntelSSTDMIC-$(VERSION).zip
	@echo "built $(OUT) (firmware embedded from kext/Firmware)"
	@nm -gU $(OUT)/Contents/MacOS/IntelSSTDMIC | grep -E 'kmod_info|_start' || true

clean:
	rm -rf build
