RACK_DIR ?= ../../../deps/Rack-SDK
SOURCES += src/plugin.cpp src/Gorgon.cpp
DISTRIBUTABLES += res README.md LICENSE docs
EXTRA_CXXFLAGS += -std=c++17
include $(RACK_DIR)/arch.mk
ifdef ARCH_WIN
	# Winsock for the loopback client; MinGW runtimes linked statically, as Rack does not ship them.
	EXTRA_LDFLAGS += -lws2_32 -static-libgcc -Wl,-Bstatic -lstdc++ -lwinpthread -Wl,-Bdynamic
endif
include $(RACK_DIR)/plugin.mk
