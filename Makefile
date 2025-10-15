GTESTDIR := /opt/homebrew/Cellar/googletest/1.17.0
CXXFLAGS := -std=c++20 -I./spallocator -I$(GTESTDIR)/include -I/usr/local/include -MMD -MP
LDFLAGS  := -L$(GTESTDIR)/lib -L/usr/local/lib -lgtest

OBJDIR := ./obj
SRC := tester.cpp
TARGET := tester
DEPFILE := $(OBJDIR)/$(TARGET).d

all: $(OBJDIR)/$(TARGET)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJDIR)/$(TARGET): $(SRC) | $(OBJDIR)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# Include the dependency file if it exists
-include $(DEPFILE)

clean:
	rm -f $(OBJDIR)/$(TARGET) $(OBJDIR)/*.o $(OBJDIR)/*.d
.PHONY: all clean
