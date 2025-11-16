# Makefile for compiling shader_runner on Arch Linux (Steam Deck)

CXX = g++
CXXFLAGS = -Wall -O2 -std=c++11

# Include paths (adjust if necessary)
INCLUDES =

# Libraries to link against
LIBS = -lGLEW -lglfw -lGL

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
	
clean:
	rm -f $(TARGET)
