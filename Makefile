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

TESTER_SRC := tester.cpp
TESTER_TARGET := tester
TESTER_DEPFILE := $(OBJDIR)/$(TESTER_TARGET).d

DEMO_SRC := demo_shared_ptr.cpp
DEMO_TARGET := demo_shared_ptr
DEMO_DEPFILE := $(OBJDIR)/$(DEMO_TARGET).d

all: $(OBJDIR)/$(TESTER_TARGET) $(OBJDIR)/$(DEMO_TARGET)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJDIR)/$(TESTER_TARGET): $(TESTER_SRC) | $(OBJDIR)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

$(OBJDIR)/$(DEMO_TARGET): $(DEMO_SRC) | $(OBJDIR)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# Include the dependency files if they exist
-include $(TESTER_DEPFILE)
-include $(DEMO_DEPFILE)

clean:
	rm -f $(OBJDIR)/$(TESTER_TARGET) $(OBJDIR)/$(DEMO_TARGET) $(OBJDIR)/*.o $(OBJDIR)/*.d
.PHONY: all clean
