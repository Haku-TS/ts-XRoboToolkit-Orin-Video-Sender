###############################################################################
#
# Makefile for OrinVideoSender (WebCam only)
# By lyu20@gmu.edu
#
###############################################################################

CXX = g++
APP := OrinVideoSender

SRCS := main_web_gst.cpp
OBJS := $(SRCS:.cpp=.o)

# Build flags
USE_NV_HW_ENCODER ?= 0

# Include paths
CPPFLAGS := -std=c++11 \
	$(shell pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 glib-2.0)

# Add hardware encoder flag if enabled
ifeq ($(USE_NV_HW_ENCODER),1)
	CPPFLAGS += -DUSE_NV_HW_ENCODER
endif

# Compiler flags
CXXFLAGS := -Wall -Wextra -O2 -g

# Libraries
LDFLAGS := \
	$(shell pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 glib-2.0) \
	-lpthread

all: $(APP)

orin:
	$(MAKE) all USE_NV_HW_ENCODER=1

debug: CXXFLAGS += -DDEBUG -g3 -O0
debug: $(APP)

%.o: %.cpp
	@echo "Compiling: $<"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(APP): $(OBJS)
	@echo "Linking: $@"
	$(CXX) -o $@ $(OBJS) $(LDFLAGS)

clean:
	rm -rf $(APP) $(OBJS)

install: $(APP)
	@echo "Installing $(APP)..."
	install -D $(APP) /usr/local/bin/$(APP)

.PHONY: all orin debug clean install