
LIB_NAME = skippyhls
INCLUDE_DIR = include
SRC_DIR = src
TESTS_DIR = tests

CXX_FLAGS	  = -std=c++11 -Wall
GCC_FLAGS         = -Wall
GCC_INCLUDE_FLAGS = -I$(INCLUDE_DIR)
GCC_LIBRARY_FLAGS = -lglib-2.0 -lgio-2.0 -lgobject-2.0 -lgnutls -lcurl -lgstreamer-1.0

C_FILES = $(shell find $(SRC_DIR) -type f -name "*.c") $(shell find $(SRC_DIR) -type f -name "*.cpp")
H_FILES = $(shell find $(SRC_DIR) -type f -name "*.h") $(shell find $(INCLUDE_DIR) -type f -name "*.h")  $(shell find $(INCLUDE_DIR) -type f -name "*.hpp")

C_FILES_TESTS = $(shell find $(TESTS_DIR) -type f -name "*.cpp")

ARCHIVE_TARGET = build/lib$(LIB_NAME).a

ifeq ($(OSX_GSTREAMER),yes)
	GCC_INCLUDE_FLAGS += -I/Library/Frameworks/GStreamer.framework/Headers/
	GCC_LIBRARY_FLAGS += -L/Library/Frameworks/GStreamer.framework/Libraries/
endif

.PHONY: all build lib clean objects

all: build

build: lib

lib: $(ARCHIVE_TARGET) $(HEADERS_TARGET)

$(ARCHIVE_TARGET): objects
	mkdir -p build
	ar rcs $(ARCHIVE_TARGET) $(shell find build -type f -name "*.o")

objects: $(C_FILES) $(H_FILES)
	mkdir -p build
	gcc $(GCC_FLAGS) $(GCC_INCLUDE_FLAGS) -o build/skippy_fragment.o -c src/skippy_fragment.c
	gcc $(GCC_FLAGS) $(GCC_INCLUDE_FLAGS) -o build/skippy_hlsdemux.o -c src/skippy_hlsdemux.c
	gcc $(GCC_FLAGS) $(GCC_INCLUDE_FLAGS) -o build/skippy_uridownloader.o -c src/skippy_uridownloader.c
	g++ $(CXX_FLAGS) $(GCC_INCLUDE_FLAGS) -o build/skippy_m3u8.o -c src/skippy_m3u8.cpp
	g++ $(CXX_FLAGS) $(GCC_INCLUDE_FLAGS) -o build/SkippyM3UParser.o -c src/skippy_m3u8_parser.cpp

tests: $(C_FILES_TESTS) lib
	mkdir -p build
	g++ $(CXX_FLAGS) $(GCC_INCLUDE_FLAGS) $(GCC_LIBRARY_FLAGS) -L./build -lskippyhls -o build/SkippyM3UParserTest tests/SkippyM3UParserTest.cpp

clean:
	rm -f $(ARCHIVE_TARGET)
	rm -f ./build/*.o
