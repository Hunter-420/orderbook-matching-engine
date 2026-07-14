CXX      = g++
CXXFLAGS = -std=c++17 -O3 -march=native -Wall -Wextra -I include

TARGET = matching_engine_bin
SRCS   = src/main.cpp src/engine.cpp src/memory_pool.cpp

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRCS) -lpthread

clean:
	rm -f $(TARGET)
