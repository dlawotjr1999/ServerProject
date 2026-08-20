CC := gcc
CFLAGS := -Wall -Wextra -O2 -pthread
LDFLAGS := -pthread

SRCS := $(wildcard *.c)
OBJS := $(SRCS:.c=.o)
TARGET := server

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
