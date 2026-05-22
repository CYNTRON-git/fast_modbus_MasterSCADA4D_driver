# Build only mplc_protocol_fast_modbus for this repository.
# Requires local API/ SDK tree (MasterSCADA 4D), see docs/BUILD_INSTRUCTIONS.md.

ROOT     = ../../../API
SRCDIR   = ../../../mplc_protocol_fast_modbus
TARGET   = mplc_protocol_fast_modbus
OBJDIR   = .obj/$(TARGET)

SRC = $(filter-out $(SRCDIR)/dllmain.cpp,\
      $(wildcard $(SRCDIR)/*.cpp) $(wildcard $(SRCDIR)/*.c))
OBJ = $(addprefix $(OBJDIR)/,$(notdir $(SRC:.cpp=.o)))
OBJ := $(OBJ:.c=.o)

CXXFLAGS += -Wno-narrowing -fpermissive -O2 -fPIC -D_DEFAULT_SOURCE
LDFLAGS  += -shared -fPIC -lm -ldl -lrt -lpthread -Wl,-rpath,./ -Wl,-rpath-link,$(MPLCLIBS)
CXX ?= g++
CC  ?= gcc

INCLUDES = -I$(SRCDIR) \
           -I$(ROOT)/include \
           -I$(ROOT)/include/core \
           -I$(ROOT)/lib \
           -I$(ROOT)/lib/opcua

DEFINES = -DLINUX \
          -D_GNUC_ \
          -DTARGET_PLATFORM_NAME=\"$(TARGET)\" \
          -D'BOOST_USER_CONFIG=<mplc/detail/mplc_boost_config.hpp>'

.PHONY: all clear

all: $(TARGET).so
	@echo Done

clear:
	@rm -rf $(OBJDIR) $(TARGET).so $(TARGET).map

$(TARGET).so: $(OBJ)
	@echo "Linking $@"
	$(CXX) $(CXXFLAGS) $(OBJ) -Wl,-Map,$(TARGET).map $(LDFLAGS) $(DEPs) -o $@

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(OBJDIR)
	$(CXX) $(DEFINES) $(INCLUDES) $(CXXFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(OBJDIR)
	$(CC) $(DEFINES) $(INCLUDES) $(CFLAGS) -c -o $@ $<
