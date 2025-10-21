GTESTDIR := /opt/homebrew/Cellar/googletest/1.17.0
OBJDIR := ./obj

CXXFLAGS := -std=c++20 -I./spallocator -I$(GTESTDIR)/include -I/usr/local/include -MMD -MP
LDFLAGS  := -L$(GTESTDIR)/lib -L/usr/local/lib -lgtest

CXXDEBUG := -DDEBUG -O0 -ggdb -fno-omit-frame-pointer
LDDEBUG := -ggdb

CXXRELEASE := -O3 -DNDEBUG
LDRELEASE := -g

#CXXMEMSAN := -fsanitize=address -fsanitize=hwaddress -fsanitize=leak -fsanitize=memory -fsanitize=safe-stack -fsanitize=undefined -fno-omit-frame-pointer
#LDMEMSAN := -fsanitize=address -fsanitize=hwaddress -fsanitize=leak -fsanitize=memory -fsanitize=safe-stack -fsanitize=undefined -fno-omit-frame-pointer
CXXMEMSAN := -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer
LDMEMSAN := -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer

CXXTHREADSAN := -fsanitize=thread -fno-omit-frame-pointer
LDTHREADSAN := -fsanitize=thread

ifeq ($(BUILD),debug)
	CXXFLAGS += $(CXXDEBUG)
	LDFLAGS += $(LDDEBUG)
	OBJDIR := ./obj/debug
else ifeq ($(BUILD),memsan)
	CXXFLAGS += $(CXXDEBUG) $(CXXMEMSAN)
	LDFLAGS += $(LDDEBUG) $(LDMEMSAN)
	OBJDIR := ./obj/memsan
else ifeq ($(BUILD),threadsan)
	CXXFLAGS += $(CXXDEBUG) $(CXXTHREADSAN)
	LDFLAGS += $(LDDEBUG) $(LDTHREADSAN)
	OBJDIR := ./obj/threadsan
else
	CXXFLAGS += $(CXXRELEASE)
	LDFLAGS += $(LDRELEASE)
	OBJDIR := ./obj/release
endif

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
