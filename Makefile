###############################################################################
#
# Makefile for OrinVideoSender (WebCam only)
# By liuchuan.yu@bytedance.com
#
###############################################################################

CXX = g++
APP := OrinVideoSender

SRCS := main_web_gst.cpp
OBJS := $(SRCS:.cpp=.o)

# Include paths
CPPFLAGS := -std=c++11 \
	$(shell pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 glib-2.0)

# Compiler flags
CXXFLAGS := -Wall -Wextra -O2 -g

# Libraries
LDFLAGS := \
	$(shell pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 glib-2.0) \
	-lpthread

all: $(APP)

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

.PHONY: all debug clean install
