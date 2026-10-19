# $Id $

# Copyright (c) 2007-2015, Trustees of The Leland Stanford Junior University
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# Redistributions of source code must retain the above copyright notice, this
# list of conditions and the following disclaimer.
# Redistributions in binary form must reproduce the above copyright notice,
# this list of conditions and the following disclaimer in the documentation
# and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.

LEX = flex
YACC   = bison -y
DEFINE = 
INCPATH = -I. -Iarbiters -Iallocators -Irouters -Inetworks -Ipower -Iengine
CPPFLAGS += -Wall $(INCPATH) $(DEFINE)
CPPFLAGS += -O3
CPPFLAGS += -g
# -fPIC so the archive can be linked into a shared object or a PIE executable
# (the SimAI frontend links libbooksim.a into its own binary).
CPPFLAGS += -fPIC
# Applied to C++ translation units only: the generated lex/yacc sources below
# are compiled with $(CC) and would reject a C++ standard flag.
CXXSTD ?= -std=c++17
LFLAGS +=

PROG := booksim
LIB  := libbooksim.a

# Standalone tools: each tools/*.cpp defines its own main() and links against
# the library, so they must be kept out of both CPP_SRCS and LIB_OBJS.
TOOL_SRCS = $(wildcard tools/*.cpp)
TOOL_OBJS = $(TOOL_SRCS:.cpp=.o)
TOOL_DEPS = $(TOOL_SRCS:.cpp=.d)
TOOLS     = $(TOOL_SRCS:.cpp=)

# Unit tests: built like the tools, and run by `make test` from the booksim2
# root (their configs name runfiles/ relative to it).
TEST_SRCS = $(wildcard tests/*.cpp)
TEST_OBJS = $(TEST_SRCS:.cpp=.o)
TEST_DEPS = $(TEST_SRCS:.cpp=.d)
TESTS     = $(TEST_SRCS:.cpp=)

# simulator source files
CPP_SRCS = $(filter-out $(TOOL_SRCS) $(TEST_SRCS),$(wildcard *.cpp) $(wildcard */*.cpp))
CPP_HDRS = $(wildcard *.hpp) $(wildcard */*.hpp)
CPP_DEPS = $(CPP_SRCS:.cpp=.d)
CPP_OBJS = $(CPP_SRCS:.cpp=.o)

LEX_SRCS = lex.yy.c
LEX_OBJS = lex.yy.o

YACC_SRCS = y.tab.c
YACC_HDRS = y.tab.h
YACC_OBJS = y.tab.o

OBJS :=  $(CPP_OBJS) $(LEX_OBJS) $(YACC_OBJS)

# Everything except main(), so the simulator can be embedded in another program.
# The globals upstream defines in main.cpp live in booksim_globals.cpp.
LIB_OBJS := $(filter-out main.o,$(CPP_OBJS)) $(LEX_OBJS) $(YACC_OBJS)

.PHONY: clean all test
.SECONDARY: $(TEST_OBJS)

all: $(PROG) $(LIB) $(TOOLS)

$(PROG): $(OBJS)
	 $(CXX) $(LFLAGS) $^ -o $@

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

# Link order matters: the archive must follow the object that references it.
tools/%: tools/%.o $(LIB)
	$(CXX) $(LFLAGS) $< $(LIB) -o $@

tests/%: tests/%.o $(LIB)
	$(CXX) $(LFLAGS) $< $(LIB) -o $@

test: $(TESTS)
	@cd .. && for t in $(TESTS); do echo "== $$t"; src/$$t || exit 1; done

$(LEX_SRCS): config.l
	$(LEX) $<

$(YACC_SRCS) $(YACC_HDRS): config.y
	$(YACC) -d $<

$(LEX_OBJS): $(LEX_SRCS) $(YACC_HDRS)
	$(CC) $(CPPFLAGS) -c $< -o $@

$(YACC_OBJS): $(YACC_SRCS)
	$(CC) $(CPPFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXSTD) -MMD -c $< -o $@

clean:
	rm -f $(YACC_SRCS) $(YACC_HDRS)
	rm -f $(LEX_SRCS)
	rm -f $(CPP_DEPS) $(TOOL_DEPS)
	rm -f $(OBJS) $(TOOL_OBJS)
	rm -f $(PROG)
	rm -f $(LIB)
	rm -f $(TOOLS)
	rm -f $(TEST_DEPS) $(TEST_OBJS) $(TESTS)

distclean: clean
	rm -f *~ */*~
	rm -f *.o */*.o
	rm -f *.d */*.d

-include $(CPP_DEPS)
-include $(TOOL_DEPS)
-include $(TEST_DEPS)
