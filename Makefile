CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Werror
SO_FLAGS = -shared -fPIC -g -O0 -ldl -pthread
SAMPLE_FLAGS = -g -O0 -no-pie -pthread

TARGET_SO = libleakscope.so
SAMPLES = samples/leaky samples/threads samples/double_free samples/clean

.PHONY: all test demo clean

all: $(TARGET_SO) $(SAMPLES)

$(TARGET_SO): src/leakscope.cpp
	$(CXX) $(CXXFLAGS) -shared -fPIC -g -O0 src/leakscope.cpp -o $(TARGET_SO) -ldl -pthread

samples/%: samples/%.cpp $(TARGET_SO)
	$(CXX) $(CXXFLAGS) $(SAMPLE_FLAGS) $< -o $@

test: all
	@chmod +x scripts/test.sh scripts/symbolize.sh
	./scripts/test.sh

demo: all
	@chmod +x scripts/symbolize.sh
	@echo "--- Running leaky demo through LeakScope & symbolize.sh ---"
	@LD_PRELOAD=./$(TARGET_SO) ./samples/leaky 2>&1 | ./scripts/symbolize.sh ./samples/leaky

clean:
	rm -f $(TARGET_SO) $(SAMPLES)
