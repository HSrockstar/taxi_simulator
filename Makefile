CXX := C:/msys64/ucrt64/bin/g++.exe
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -pedantic -Isrc
NPM := npm
FRONTEND_DIR := frontend
APP_SOURCES := src/main.cpp src/manual_structures.cpp src/simulator.cpp src/http_server.cpp
TEST_SOURCES := tests/test_main.cpp src/manual_structures.cpp src/simulator.cpp src/http_server.cpp
WINDOWS_LIBS := -lws2_32 -lshell32

.PHONY: all frontend test

all: frontend taxi_simulator.exe

frontend:
	$(NPM) --prefix $(FRONTEND_DIR) run build

taxi_simulator.exe: $(APP_SOURCES)
	$(CXX) $(CXXFLAGS) $(APP_SOURCES) -o $@ $(WINDOWS_LIBS)

taxi_tests.exe: $(TEST_SOURCES)
	$(CXX) $(CXXFLAGS) -DTAXI_TESTING $(TEST_SOURCES) -o $@ $(WINDOWS_LIBS)

test: frontend taxi_tests.exe
	./taxi_tests.exe
