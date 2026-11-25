CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Werror
TARGET := light-ai-fanout
PREFIX ?= /usr/local/bin

.PHONY: all clean install

all: $(TARGET)

$(TARGET): ts_fanout.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

install: $(TARGET)
	install -d $(PREFIX)
	install -m 0755 $(TARGET) $(PREFIX)/$(TARGET)

clean:
	rm -f $(TARGET)
