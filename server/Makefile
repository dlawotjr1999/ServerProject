CC := gcc
CXX := g++
CFLAGS := -Wall -Wextra -O2 -pthread
CXXFLAGS := -Wall -Wextra -O2 -pthread -std=c++17
LDFLAGS := -pthread

# net.c/protocol.c/main.c/logic.c는 C로, state.cpp/job_queue.cpp/log.cpp는 C++로 유지(REDESIGN.md 참고)
C_SRCS := $(wildcard *.c)
CXX_SRCS := $(wildcard *.cpp)
OBJS := $(C_SRCS:.c=.o) $(CXX_SRCS:.cpp=.o)
TARGET := server

.PHONY: all clean

all: $(TARGET)

# C/C++ 오브젝트가 섞여 있으므로 최종 링크는 g++로 수행(C++ 런타임을 자동으로 링크해줌)
$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
