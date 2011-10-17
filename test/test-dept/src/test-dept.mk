# Copyright 2008--2010 Mattias Norrby
#
# This file is part of Test Dept..
#
# Test Dept. is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Test Dept. is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with Test Dept..  If not, see <http://www.gnu.org/licenses/>.
#
# As a special exception, you may use this file as part of a free
# testing framework without restriction.  Specifically, if other files
# include this makefile for contructing test-cases this file does not
# by itself cause the resulting executable to be covered by the GNU
# General Public License.  This exception does not however invalidate
# any other reasons why the executable file might be covered by the
# GNU General Public License.

TEST_DEPT_EXEC_ARCH?=$(shell uname -m)
VPATH+=$(dir $(TEST_SRCS))
VPATH+=$(TEST_DEPT_SRC_DIR)

TEST_MAIN_SRCS=$(notdir $(patsubst %.c,%_main.c,$(TEST_SRCS)))
TEST_MAIN_OBJS=$(patsubst %.c,%.o,$(TEST_MAIN_SRCS))
TEST_MAINS=$(patsubst %_main.c,%,$(TEST_MAIN_SRCS))

%_replacement_symbols.txt:	%_undef_syms.txt %_tmpmain_undef_syms.txt
	grep -f $^ | $(TEST_DEPT_RUNTIME_PREFIX)sym2repl >$@ || true

%_undef_syms.txt:        %.o
	$(NM) -p $< | awk '/ U / {print $$(NF-1),$$(NF)}' |\
                      sed 's/[^A-Za-z_0-9 ].*$$//' >$@ || true

# Constructed in order to see which symbols are resolved by the loader.
# Such symbols that we do not want to stub could be stdout, stderr, etc
%_tmpmain.o:	%.o
	$(CC) $(LDFLAGS) $(LDFLAGS_UNRESOLVED) $(TARGET_ARCH)	$^ -o $@

%_proxies.s:	%.o %_test_main.o
	$(TEST_DEPT_RUNTIME_PREFIX)sym2asm $^ $(SYMBOLS_TO_ASM) $(NM) >$@

# Add LDFLAGS here for ignoring unresolved references
# LDFLAGS_UNRESOLVED?=
SYMBOLS_TO_ASM=$(TEST_DEPT_RUNTIME_PREFIX)sym2asm_$(TEST_DEPT_EXEC_ARCH).awk
GNU_LD_IGNORE_UNRESOLVED=-Wl,--unresolved-symbols=ignore-in-object-files
CCS_LD_IGNORE_UNRESOLVED=-Wl,-znodefs
ifeq (,$(LDFLAGS_UNRESOLVED))
  ifeq (,$(LDFLAGS_TYPE))
    LDFLAGS_TYPE:=$(shell $(CC) $(GNU_LD_IGNORE_UNRESOLVED) >/dev/null 2>&1 && echo "GNU" || echo $(LDFLAGS_TYPE))
    LDFLAGS_TYPE:=$(shell $(CC) $(CCS_LD_IGNORE_UNRESOLVED) >/dev/null 2>&1 && echo "CCS" || echo $(LDFLAGS_TYPE))
  endif
  ifeq (GNU,$(LDFLAGS_TYPE))
    LDFLAGS_UNRESOLVED=$(GNU_LD_IGNORE_UNRESOLVED)
  else
    ifeq (CCS,$(LDFLAGS_TYPE))
      LDFLAGS_UNRESOLVED=$(CCS_LD_IGNORE_UNRESOLVED)
    endif
  endif
endif
ifeq (,$(LDFLAGS_UNRESOLVED))
  $(error LDFLAGS_UNRESOLVED must have "-Wl,"-flags that ignore missing symbols)
endif

ifneq (,$(TEST_DEPT_INCLUDE_PATH))
TEST_DEPT_MAKEFILE_INCLUDE_PATH=$(TEST_DEPT_INCLUDE_PATH)/
endif
include $(TEST_DEPT_MAKEFILE_INCLUDE_PATH)test-dept-proxies.mk
