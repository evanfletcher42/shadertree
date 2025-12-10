# Makefile for compiling shader_runner on Arch Linux (Steam Deck)

CXX = g++
CXXFLAGS = -Wall -O2 -std=c++11 -g

# Include paths (adjust if necessary)
INCLUDES =

# Libraries to link against
LIBS = -lOSMesa -lGL -ldl -ludev -lusb-1.0

# Source files
SRCS = shaderrunner/runshader.cpp

# Header files
HDRS = shaderrunner/treeinterface.h

# Output executable
TARGET = shaderrunner/runshader
COPY_DIR = stweb/runshader

all: $(TARGET) copy

$(TARGET): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SRCS) -o $(TARGET) $(LIBS)

copy: $(TARGET)
	cp $(TARGET) $(COPY_DIR)

install_udev_rules : 
	sudo cp shaderrunner/99-x32-ws2812-usbhs-breakout.rules /etc/udev/rules.d/
	sudo udevadm control --reload
	sudo udevadm trigger
	
clean:
	rm -f $(TARGET)
