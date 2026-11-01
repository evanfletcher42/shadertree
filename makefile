# Builds runshader. On a Steam Deck, install.sh installs the compiler and headers this needs.

CXX = g++
CXXFLAGS = -Wall -O2 -std=c++17 -g

# Include paths (adjust if necessary)
INCLUDES =

# Libraries to link against
LIBS = -lEGL -lOpenGL -lusb-1.0

# Source files
SRCS = shaderrunner/runshader.cpp

# Header files
HDRS = shaderrunner/x32breakoutdriver.h shaderrunner/os_generic.h

# Output executable
TARGET = shaderrunner/runshader
COPY_DIR = stweb/runshader

.PHONY: all copy clean

all: $(TARGET) copy

$(TARGET): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SRCS) -o $(TARGET) $(LIBS)

copy: $(TARGET)
	cp $(TARGET) $(COPY_DIR)

clean:
	rm -f $(TARGET)
