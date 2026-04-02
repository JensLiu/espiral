include ../common.mk

DESTDIR ?= $(CURDIR)/..

SRC_DIR := $(VORTEX_HOME)/runtime/espiral

CXXFLAGS += -std=c++17 -Wall -Wextra -Wfatal-errors
CXXFLAGS += -Wno-maybe-uninitialized
# Need generated headers like VX_config.h and VX_types.h from the sibling build tree's hw directory.
CXXFLAGS += -I$(INC_DIR) -I../common -I$(DESTDIR)/hw -I$(dir $(DESTDIR))hw -I$(SIM_DIR) -I$(SW_COMMON_DIR) -I$(RT_COMMON_DIR)
CXXFLAGS += -I$(SRC_DIR)/includes -I$(SRC_DIR)/mm -I$(SRC_DIR)/accelerator -I$(SRC_DIR)
CXXFLAGS += -DXLEN_$(XLEN) -DVM_ENABLE
CXXFLAGS += $(CONFIGS)
CXXFLAGS += -fPIC

ifdef DEBUG
	CXXFLAGS += -g -O0
else
	CXXFLAGS += -O2 -DNDEBUG
endif

LIB_SRCS := $(SRC_DIR)/espiral.cpp

LIB        := $(DESTDIR)/libespiral.so
LIB_SIMX   := $(DESTDIR)/libespiral-simx.so
LIB_RTLSIM := $(DESTDIR)/libespiral-rtlsim.so
HW_CONFIG_HEADERS := $(dir $(DESTDIR))hw/VX_config.h $(dir $(DESTDIR))hw/VX_types.h
HW_BUILD_DIR := $(dir $(DESTDIR))hw

CXXFLAGS_ALL := $(CXXFLAGS)
CXXFLAGS_SIMX   := $(CXXFLAGS) -DESPIRAL_BACKEND_SIMX
CXXFLAGS_RTLSIM := $(CXXFLAGS) -DESPIRAL_BACKEND_RTLSIM

LDFLAGS_ALL    := -pthread -L$(DESTDIR) -Wl,--no-as-needed -lsimx -lrtlsim -Wl,--as-needed -Wl,-rpath,$(DESTDIR)
LDFLAGS_SIMX   := -pthread -L$(DESTDIR) -lsimx   -Wl,-rpath,$(DESTDIR)
LDFLAGS_RTLSIM := -pthread -L$(DESTDIR) -lrtlsim -Wl,-rpath,$(DESTDIR)

TESTS_DIR := $(SRC_DIR)/tests
TEST_SRCS := $(wildcard $(TESTS_DIR)/*.cpp)
TEST_BINS := $(patsubst $(TESTS_DIR)/%.cpp, $(TESTS_DIR)/%, $(TEST_SRCS))

.PHONY: all lib lib-all lib-simx lib-rtlsim tests clean

all: lib tests

lib: lib-all lib-simx lib-rtlsim

lib-all: $(LIB)

lib-simx: $(LIB_SIMX)

lib-rtlsim: $(LIB_RTLSIM)

$(LIB): $(LIB_SRCS) $(DESTDIR)/libsimx.so $(DESTDIR)/librtlsim.so $(HW_CONFIG_HEADERS)
	$(CXX) $(CXXFLAGS_ALL) -shared $< $(LDFLAGS_ALL) -o $@

$(LIB_SIMX): $(LIB_SRCS) $(DESTDIR)/libsimx.so $(HW_CONFIG_HEADERS)
	$(CXX) $(CXXFLAGS_SIMX) -shared $< $(LDFLAGS_SIMX) -o $@

$(LIB_RTLSIM): $(LIB_SRCS) $(DESTDIR)/librtlsim.so $(HW_CONFIG_HEADERS)
	$(CXX) $(CXXFLAGS_RTLSIM) -shared $< $(LDFLAGS_RTLSIM) -o $@

$(HW_CONFIG_HEADERS): | $(HW_BUILD_DIR)
	$(MAKE) -C $(HW_BUILD_DIR) config

tests: $(LIB_SIMX) $(TEST_BINS)

$(DESTDIR)/libsimx.so:
	DESTDIR=$(DESTDIR) $(MAKE) -C $(ROOT_DIR)/sim/simx $(DESTDIR)/libsimx.so

$(DESTDIR)/librtlsim.so:
	DESTDIR=$(DESTDIR) $(MAKE) -C $(ROOT_DIR)/sim/rtlsim $(DESTDIR)/librtlsim.so

$(TESTS_DIR)/%: $(TESTS_DIR)/%.cpp $(LIB_SIMX)
	$(CXX) $(CXXFLAGS_SIMX) $< -L$(DESTDIR) -lespiral-simx -Wl,-rpath,$(DESTDIR) -o $@

clean:
	rm -f $(LIB) $(LIB_SIMX) $(LIB_RTLSIM) $(TEST_BINS)
