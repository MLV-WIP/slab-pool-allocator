GTESTDIR := /opt/homebrew/Cellar/googletest/1.17.0
CXXFLAGS := -std=c++20 -I./spallocator -I$(GTESTDIR)/include -I/usr/local/include
LDFLAGS  := -L$(GTESTDIR)/lib -L/usr/local/lib -lgtest

OBJDIR := ./obj
SRC := tester.cpp
TARGET := tester

all: $(OBJDIR)/$(TARGET)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJDIR)/$(TARGET): $(SRC) | $(OBJDIR)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f $(OBJDIR)/$(TARGET) $(OBJDIR)/*.o
.PHONY: all clean
