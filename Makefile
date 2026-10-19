# Copyright 2026 Austin Lehman
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Aussom C interpreter build
#
# Targets:
#   make           - build the binary at $(BIN) and the library at $(LIB)
#   make lib       - build only the library at $(LIB)
#   make targets   - list the cross-build targets
#   make version   - print the version, read from include/aussom/version.h
#   make <target>  - cross build, e.g. `make android-arm64`. One command
#                    per file in scripts/targets/; each produces only the
#                    archive, at build/<target>/libaussom.a
#
# Variables:
#   GC_PROFILE     - auto (default), mobile, or desktop. See below.
#   make test      - build and run the C unit tests under tests/
#   make clean     - remove the host build only
#   make clean-<target> - remove one cross build, e.g. clean-android-arm64
#   make clean-all - remove the host build and every cross build
#   make rebuild   - clean then build
#
# Layout:
#   include/       - public headers
#   src/           - implementation .c files
#   src/parser/    - flex (.l) and bison (.y) sources
#   stdlib/aus/    - standard-library Aussom source (embedded)
#   third_party/   - vendored code and data; licenses in THIRD-PARTY.md
#   scripts/targets/ - one file per cross-build target
#   build/gen/     - generated scanner/parser/embedded sources
#   build/obj/     - compiled object files
#   build/<target>/  - a cross build's objects and archive
#   build/         - final binary

CC      := gcc
AR      := ar
LEX     := flex
YACC    := bison
AWK     := awk

# --- version -------------------------------------------------------
#
# include/aussom/version.h is where a host reads the version from, so
# that is where it lives. Everything in this file that needs it takes it
# from there, so a release is one edit and cannot end up saying two
# things.
#
# The dot in each pattern is the hash of "#define", matched rather than
# written. A hash is make's comment character, and make strips comments
# before it parses a function call, so a literal one here ends the line
# in the middle of $(shell and the closing bracket is never seen:
#
#   Makefile:NN: *** unterminated call to function `shell': missing `)'.
#
# GNU make 4 lets it pass; the make Apple ships is 3.81 and does not.
VERSION_H            := include/aussom/version.h
AUSSOM_VERSION_MAJOR := $(shell sed -n 's/^.define AUSSOM_VERSION_MAJOR *//p' $(VERSION_H))
AUSSOM_VERSION_MINOR := $(shell sed -n 's/^.define AUSSOM_VERSION_MINOR *//p' $(VERSION_H))
AUSSOM_VERSION_PATCH := $(shell sed -n 's/^.define AUSSOM_VERSION_PATCH *//p' $(VERSION_H))
AUSSOM_VERSION_LABEL := $(shell sed -n 's/^.define AUSSOM_VERSION_LABEL *"\(.*\)"/\1/p' $(VERSION_H))
AUSSOM_VERSION       := $(AUSSOM_VERSION_MAJOR).$(AUSSOM_VERSION_MINOR).$(AUSSOM_VERSION_PATCH)

# What a shared library's loader would match on, which is not the release
# number: it changes only when a host compiled against an older aussom-c
# would stop working. While the major number is zero there is nothing to
# promise, so the two move together; from 1.0 they part.
AUSSOM_ABI           := $(AUSSOM_VERSION_MAJOR)

# --- cross targets -------------------------------------------------
#
# One command per file in scripts/targets/, named after the file:
#
#   make                    the machine you are on
#   make android-arm64      an Android device
#
# A target file sets the compiler and archiver that target needs, and
# HOST_OS when it differs. It is included here, before everything that
# depends on those, so the rest of this file picks the right flags with
# no further conditionals: the assignments below are all `?=` or derived
# from what a target file may set.
#
# Adding a target is adding a file. Nothing else in this Makefile
# changes, and the new name becomes a command automatically.
TARGET_DIR    := scripts/targets
CROSS_TARGETS := $(sort $(patsubst $(TARGET_DIR)/%.mk,%,$(wildcard $(TARGET_DIR)/*.mk)))

ifdef TARGET
  ifeq (,$(filter $(TARGET),$(CROSS_TARGETS)))
    $(error unknown target '$(TARGET)'. Available: $(CROSS_TARGETS))
  endif
  include $(TARGET_DIR)/$(TARGET).mk
endif
STD     := -std=c11
WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
           -Wmissing-prototypes -Wpointer-arith -Wcast-align \
           -Wwrite-strings -Wno-unused-parameter
OPT     := -O2 -g
# Host platform, used only to pick the few flags that genuinely differ.
# Override HOST_OS on a cross build (HOST_OS=darwin, mingw, linux).
HOST_OS ?= $(shell uname -s | tr 'A-Z' 'a-z')

# GC_THREADS makes <gc.h> declare the thread API and redirect
# pthread_create to the GC's auto-registering wrapper. -pthread is
# both a compile flag (sets _REENTRANT) and a link flag (pulls in
# libpthread), so it goes in both CFLAGS and LDFLAGS.
# PCRE2_STATIC belongs in the project-wide defines, not only in
# PCRE2's own: without it pcre2.h declares every entry point
# __declspec(dllimport) on Windows, and the statically linked library
# then fails to resolve against the __imp_ symbols that produces.
DEFS    := -D_POSIX_C_SOURCE=200809L -DGC_THREADS -DPCRE2_STATIC

# Which heap growth policy the collector uses. See aussom_gc_init.
#
#   auto     (default) the target decides: Android and iOS take the
#            small-heap policy, everything else pre-grows.
#   mobile   force the small-heap policy.
#   desktop  force the pre-grown heap.
#
# The switch exists because of the Raspberry Pi, and the Pi is why it is
# a switch rather than a detection. A Pi is ordinary aarch64 Linux and
# nothing in the toolchain distinguishes it from an aarch64 server,
# which has memory to spare and wants the pre-grown heap. So aussom-c
# owns the mechanism and the default, and the caller owns the one call
# a compiler cannot make:
#
#   make GC_PROFILE=mobile
GC_PROFILE ?= auto
ifeq ($(GC_PROFILE),mobile)
  GC_PROFILE_DEF := -DAUSSOM_GC_MOBILE_PROFILE=1
else ifeq ($(GC_PROFILE),desktop)
  GC_PROFILE_DEF := -DAUSSOM_GC_MOBILE_PROFILE=0
else ifeq ($(GC_PROFILE),auto)
  GC_PROFILE_DEF :=
else
  $(error GC_PROFILE must be auto, mobile or desktop, not '$(GC_PROFILE)')
endif

# Extra defines for the project's own sources, for build-time choices a
# compiler predefine cannot make.
CFLAGS_EXTRA ?=
DEFS += $(GC_PROFILE_DEF) $(CFLAGS_EXTRA)
THREADS := -pthread

# Position independent code, for every object in the library. A consumer
# that links these objects into a shared library needs it; in the static
# products it costs nothing worth measuring on the targets here. Windows has no such flag -- PE code is
# position independent already and GCC warns when asked for it -- so it
# is left off there.
ifneq (,$(filter mingw% msys% cygwin% windows%,$(HOST_OS)))
  PIC :=
else
  PIC := -fPIC
endif

# Hidden visibility for the vendored dependencies, so their symbols stay
# out of the dynamic table of whatever these objects end up inside. That
# is the point of the exercise: a host that links this library and also
# carries its own PCRE2 must not have the two meet.
#
# It is deliberately NOT applied to this project's own sources. A host
# that loads its own libraries at run time has them resolve av_* against
# the host, and a hidden symbol never reaches the dynamic table. It would
# also defeat a version script downstream, which can restrict exports but
# cannot un-hide what the compiler already hid. Restricting this
# project's own API is the linker's job, at the point the shared product
# is made.
#
# bdwgc is left visible too, so such a library can register a Boehm
# finalizer to release a foreign resource.
ifneq (,$(filter mingw% msys% cygwin% windows%,$(HOST_OS)))
  DEP_VIS :=
else
  DEP_VIS := -fvisibility=hidden
endif

# Apple's headers hide non-POSIX declarations when _POSIX_C_SOURCE is
# set strictly, and the collector and sys stdlib need some of them, so
# ask for the Darwin additions alongside it.
ifneq (,$(findstring darwin,$(HOST_OS)))
DEFS += -D_DARWIN_C_SOURCE
endif

# Executable suffix. Windows linkers append .exe when the -o name has no
# extension, so a target spelled without it is a file that never appears
# and make relinks it on every run. Naming the target what the linker
# actually writes is what makes the check work.
ifneq (,$(filter mingw% msys% cygwin% windows%,$(HOST_OS)))
  EXE := .exe
else
  EXE :=
endif

# Objects for one machine are not objects for another, so each target
# gets its own directory; one directory holding both would produce an
# archive that links nowhere. The host build stays in build/, where it
# has always been.
BUILD_ROOT := build
ifdef TARGET
  BUILD   := $(BUILD_ROOT)/$(TARGET)
else
  BUILD   := $(BUILD_ROOT)
endif
OBJDIR    := $(BUILD)/obj
GENDIR    := $(BUILD)/gen
BIN       := $(BUILD)/aussomc$(EXE)
LIB       := $(BUILD)/libaussom.a
PCRE2_DIR := third_party/pcre2
ARGPARSE_DIR := third_party/argparse
BDWGC_DIR := third_party/bdwgc
GREATEST_DIR := third_party/greatest

INCDIRS := -Iinclude -I$(GENDIR) -I$(PCRE2_DIR) -I$(ARGPARSE_DIR) \
           -I$(BDWGC_DIR)/include

CFLAGS  := $(STD) $(WARN) $(OPT) $(PIC) $(DEFS) $(THREADS) $(INCDIRS) -MMD -MP
# The interpreter loads no code at run time, but the vendored collector
# references dlopen (its GC_dlopen wrapper), and before glibc 2.34 that
# lives in a separate libdl. macOS and Windows have no libdl. The
# collector is vendored under third_party/bdwgc, so there is no -lgc
# anywhere.
LDFLAGS := $(THREADS)
ifneq (,$(findstring darwin,$(HOST_OS)))
  LDLIBS  := -lm
else ifneq (,$(filter mingw% msys% cygwin% windows%,$(HOST_OS)))
  LDLIBS  := -lm
else
  LDLIBS  := -lm -ldl
endif

# Relaxed warning set for generated / vendored C we don't own.
#
# -Wno-unused-but-set-variable is for bison's `yynerrs`. With the pure
# parser it is a local inside yyparse rather than a global, and bison
# increments it without reading it, so clang reports it set-but-unused.
# gcc does not warn on the same code, which is why this only shows up on
# a clang build. There is nothing to fix in the grammar -- the variable
# is bison's, and the count is already tracked separately in
# parser_ctx.syntax_errors.
GEN_WARN := -Wall -Wno-unused-function -Wno-sign-compare \
            -Wno-unused-but-set-variable

# --- generated lexer / parser ------------------------------------
PARSER_Y  := src/parser/parser.y
SCANNER_L := src/parser/scanner.l
PARSER_C  := $(GENDIR)/parser.c
PARSER_H  := $(GENDIR)/parser.h
SCANNER_C := $(GENDIR)/scanner.c

# --- embedded stdlib .aus sources --------------------------------
# Each stdlib/aus/NAME.aus becomes build/gen/NAME_aus.c exposing
# `const char aussom_NAME_aus[]` (a null-terminated string).
AUS_SRCS := $(wildcard stdlib/aus/*.aus)
AUS_GEN  := $(AUS_SRCS:stdlib/aus/%.aus=$(GENDIR)/%_aus.c)
AUS_OBJS := $(AUS_SRCS:stdlib/aus/%.aus=$(OBJDIR)/gen/%_aus.o)

# --- embedded time zone database ---------------------------------
# third_party/tzdata/tzdata.zi becomes build/gen/tzdata_zi.c exposing
# `const char aussom_tzdata_zi[]`, read by src/stdlib/tzdb.c.
TZDATA_DIR := third_party/tzdata
TZDATA_ZI  := $(TZDATA_DIR)/tzdata.zi
TZDATA_OBJ := $(OBJDIR)/gen/tzdata_zi.o

# --- vendored argparse (MIT) -------------------------------------
ARGPARSE_OBJ := $(OBJDIR)/third_party/argparse/argparse.o

# --- vendored PCRE2 (32-bit, no JIT) -----------------------------
# Built from the upstream non-autotools templates (config.h / pcre2.h /
# pcre2_chartables.c). All .c in the dir are library sources.
PCRE2_SRCS := $(wildcard $(PCRE2_DIR)/*.c)
PCRE2_OBJS := $(PCRE2_SRCS:$(PCRE2_DIR)/%.c=$(OBJDIR)/third_party/pcre2/%.o)
PCRE2_DEFS := -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32 -DPCRE2_STATIC

# --- vendored Boehm-Demers-Weiser collector -----------------------
# Built from the distribution sources with no configure step: without
# HAVE_CONFIG_H the collector configures itself from compiler predefines
# in include/private/gcconfig.h, the same way PCRE2 is vendored here.
#
# The define set is upstream's own no-configure default from
# Makefile.direct, plus two of ours. Taking upstream's set rather than
# trimming it keeps the build to a combination upstream tests.
#
#   USE_MMAP, USE_MUNMAP     Return reclaimed memory to the operating
#                            system instead of holding the high water
#                            mark for the life of the process. This is
#                            what iOS needs, where footprint decides
#                            what gets killed.
#   NO_EXECUTE_PERMISSION    Never map collector memory executable.
#   ALL_INTERIOR_POINTERS    Upstream and distribution default; a
#                            pointer into the middle of an object keeps
#                            it alive.
#   ENABLE_DISCLAIM,         Upstream default feature set.
#   GC_ATOMIC_UNCOLLECTABLE,
#   GC_GCJ_SUPPORT,
#   JAVA_FINALIZATION
#   GC_THREADS               Ours. Required by the project; must agree
#                            with $(DEFS) so every unit sees the same
#                            <gc.h>.
#   GC_BUILTIN_ATOMIC        Ours. Use compiler atomic builtins so the
#                            collector needs no libatomic_ops.
BDWGC_SRCS := $(wildcard $(BDWGC_DIR)/*.c)
BDWGC_OBJS := $(BDWGC_SRCS:$(BDWGC_DIR)/%.c=$(OBJDIR)/third_party/bdwgc/%.o)
BDWGC_DEFS := -DALL_INTERIOR_POINTERS -DENABLE_DISCLAIM \
              -DGC_ATOMIC_UNCOLLECTABLE -DGC_GCJ_SUPPORT \
              -DJAVA_FINALIZATION -DNO_EXECUTE_PERMISSION \
              -DUSE_MMAP -DUSE_MUNMAP -DGC_THREADS -DGC_BUILTIN_ATOMIC
# On Windows the project builds against MinGW's winpthreads, so the
# collector has to be told to use the pthreads API rather than the Win32
# native one, and that it is being linked statically rather than as a
# DLL. Both names are upstream's, from bdwgc docs/macros.md.
ifneq (,$(filter mingw% msys% cygwin% windows%,$(HOST_OS)))
BDWGC_DEFS += -DGC_WIN32_PTHREADS -DGC_NOT_DLL
endif

GEN_OBJS := $(OBJDIR)/gen/parser.o $(OBJDIR)/gen/scanner.o $(AUS_OBJS) $(TZDATA_OBJ) $(PCRE2_OBJS) $(ARGPARSE_OBJ) $(BDWGC_OBJS)

# Hand-written sources under src/ (the .l/.y are handled separately).
SRCS := $(shell find src -type f -name '*.c' 2>/dev/null)
OBJS := $(SRCS:src/%.c=$(OBJDIR)/%.o)
# Deliberately recursive (`=`, not `:=`). TEST_BINS is defined further
# down with the test rules, so an immediate assignment here would expand
# it while it is still empty and silently drop its .d files -- which is what it did, leaving the test
# binaries with no header-dependency tracking at all. The only reference
# is the `-include $(DEPS)` on the last line of this file, by which point
# everything is defined. Keep it recursive.
#
# The .d names are stripped of $(EXE), not appended to: gcc -MMD names
# the file after the -o target with its suffix replaced, so
# `-o test_foo.exe` writes test_foo.d. With $(EXE) empty the same pattern
# is a plain append.
DEPS = $(OBJS:.o=.d) $(GEN_OBJS:.o=.d) $(TEST_BINS:%$(EXE)=%.d)

.PHONY: all lib clean rebuild
.DEFAULT_GOAL := all

all: $(BIN) $(LIB)

# The archive on its own, for a consumer that does not want the command
# line interpreter. A cross build wants exactly this: the executable
# cannot run on the target, and asking for the default goal builds it
# anyway.
lib: $(LIB)

# Everything except the CLI front end. This is what a host embeds:
# engine_run drives a program, so main.c is one host among others
# rather than the only place a program can be started from.
LIB_OBJS := $(filter-out $(OBJDIR)/main.o,$(OBJS)) $(GEN_OBJS)

# Rebuilt from scratch, because `ar rcs` adds and replaces members but
# never removes one. A deleted source would otherwise leave its object
# in the archive, still linked into everything.
$(LIB): $(LIB_OBJS)
	@mkdir -p $(@D)
	rm -f $@
	$(AR) rcs $@ $^

$(BIN): $(OBJDIR)/main.o $(LIB)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $(OBJDIR)/main.o $(LIB) $(LDLIBS)

# --- generator rules ---------------------------------------------

$(PARSER_C) $(PARSER_H): $(PARSER_Y)
	@mkdir -p $(GENDIR)
	$(YACC) -Wall -d -o $(PARSER_C) $(PARSER_Y)

$(SCANNER_C): $(SCANNER_L) $(PARSER_H)
	@mkdir -p $(GENDIR)
	$(LEX) -o $(SCANNER_C) $(SCANNER_L)

# Embed each stdlib .aus as a null-terminated C string via xxd.
$(GENDIR)/%_aus.c: stdlib/aus/%.aus
	@mkdir -p $(GENDIR)
	printf 'const char aussom_%s_aus[] = {\n' '$*' > $@
	xxd -i < $< >> $@
	printf ', 0x00\n};\n' >> $@

# Embed tzdata.zi the same way.
$(GENDIR)/tzdata_zi.c: $(TZDATA_ZI)
	@mkdir -p $(GENDIR)
	printf 'const char aussom_tzdata_zi[] = {\n' > $@
	xxd -i < $< >> $@
	printf ', 0x00\n};\n' >> $@

# tzdata.zi is built from the IANA source files beside it with IANA's
# own awk scripts, the way IANA's Makefile builds it, less the "factory"
# file. Run by hand after replacing the files with a new release; the
# normal build only reads the result.
TZDATA_SRCS := africa antarctica asia australasia europe northamerica \
               southamerica etcetera backward
.PHONY: tzdata-zi
tzdata-zi:
	cd $(TZDATA_DIR) && \
	  $(AWK) -v DATAFORM=main -v PACKRATDATA= -v PACKRATLIST= \
	    -f ziguard.awk $(TZDATA_SRCS) > main.zi.out && \
	  LC_ALL=C $(AWK) -v dataform=main \
	    -v deps='ziguard.awk $(TZDATA_SRCS) zishrink.awk' \
	    -v redo=posix_only -v version="$$(cat version)" \
	    -f zishrink.awk main.zi.out > tzdata.zi && \
	  rm -f main.zi.out

# --- compile rules -----------------------------------------------

# Hand-written sources: src/foo/bar.c -> build/obj/foo/bar.o
$(OBJDIR)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

# Generated parser/scanner: relaxed warnings, but same $(DEFS)/$(THREADS)
# so their view of <gc.h> matches the hand-written units.
$(OBJDIR)/gen/parser.o: $(PARSER_C) $(PARSER_H)
	@mkdir -p $(@D)
	$(CC) $(STD) $(GEN_WARN) $(OPT) $(PIC) $(DEFS) $(THREADS) $(INCDIRS) -MMD -MP -c -o $@ $(PARSER_C)

$(OBJDIR)/gen/scanner.o: $(SCANNER_C) $(PARSER_H)
	@mkdir -p $(@D)
	$(CC) $(STD) $(GEN_WARN) $(OPT) $(PIC) $(DEFS) $(THREADS) $(INCDIRS) -MMD -MP -c -o $@ $(SCANNER_C)

# Embedded .aus strings: pure data. UTF-8 bytes >127 overflow signed
# char (harmless for a byte buffer), so warnings are off.
$(OBJDIR)/gen/%_aus.o: $(GENDIR)/%_aus.c
	@mkdir -p $(@D)
	$(CC) $(STD) $(OPT) $(PIC) $(DEFS) $(THREADS) $(INCDIRS) -w -MMD -MP -c -o $@ $<

$(TZDATA_OBJ): $(GENDIR)/tzdata_zi.c
	@mkdir -p $(@D)
	$(CC) $(STD) $(OPT) $(PIC) $(DEFS) $(THREADS) $(INCDIRS) -w -MMD -MP -c -o $@ $<

# Vendored argparse: third-party, relaxed warnings.
$(ARGPARSE_OBJ): $(ARGPARSE_DIR)/argparse.c
	@mkdir -p $(@D)
	$(CC) $(STD) $(GEN_WARN) $(OPT) $(PIC) $(DEP_VIS) $(DEFS) -I$(ARGPARSE_DIR) -MMD -MP -c -o $@ $<

# Vendored PCRE2: third-party, warnings off. Built 8-bit, no JIT, with
# the upstream config.h via -DHAVE_CONFIG_H. No project DEFS/THREADS.
$(OBJDIR)/third_party/pcre2/%.o: $(PCRE2_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(STD) -w $(OPT) $(PIC) $(DEP_VIS) $(PCRE2_DEFS) -I$(PCRE2_DIR) -c -o $@ $<

# Vendored bdwgc: third-party, warnings off. -pthread is needed for the
# collector's own thread support, matching $(THREADS) elsewhere.
$(OBJDIR)/third_party/bdwgc/%.o: $(BDWGC_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(STD) -w $(OPT) $(PIC) $(BDWGC_DEFS) $(THREADS) -I$(BDWGC_DIR)/include -c -o $@ $<

# --- C unit tests -------------------------------------------------
# One binary per tests/test_*.c, so a crash loses one file's results
# rather than the whole run. Each links libaussom.a, never main.o, which
# is the same separation that lets a host embed the library.
#
# These are C tests. The Aussom language regression suite is a different
# thing and runs through the interpreter:
#     ./build/aussomc -t tests/interpreter.aus
# and the aussom-c Aussom tests, such as
#     ./build/aussomc -t tests/c-interpreter.aus
#     ./build/aussomc -t tests/bufferCharsetTest.aus
#     ./build/aussomc -t tests/stringUnicodeTest.aus
#     ./build/aussomc -t tests/dateFormatTest.aus
TEST_SRCS := $(wildcard tests/test_*.c)
TEST_BINS := $(TEST_SRCS:tests/%.c=$(BUILD)/tests/%$(EXE))

$(BUILD)/tests:
	@mkdir -p $(BUILD)/tests

$(BUILD)/tests/%$(EXE): tests/%.c $(LIB) | $(BUILD)/tests
	$(CC) $(CFLAGS) -I$(GREATEST_DIR) -MMD -MP -o $@ $< $(LIB) $(LDLIBS)

.PHONY: test
test: $(TEST_BINS)
	@fail=0; 	for t in $(TEST_BINS); do 		echo "== $$t"; 		$$t || fail=1; 	done; 	exit $$fail

# `make clean` clears the host build ONLY. Cross builds live in
# subdirectories of the same tree, so this removes everything directly
# under build/ except those, rather than the tree itself -- a plain
# `rm -rf build` would take the cross builds with it.
#
# Computed by subtraction rather than by listing what the host build
# produces, so artifacts nothing here knows about (the test binaries,
# for one) are still cleared.
HOST_CLEAN := $(filter-out $(addprefix $(BUILD_ROOT)/,$(CROSS_TARGETS)), \
                $(wildcard $(BUILD_ROOT)/*))

.PHONY: clean
clean:
	rm -rf $(HOST_CLEAN)

# One clean command per cross target, named after it:
#   make clean-android-arm64
.PHONY: $(addprefix clean-,$(CROSS_TARGETS))
$(addprefix clean-,$(CROSS_TARGETS)):
	rm -rf $(BUILD_ROOT)/$(patsubst clean-%,%,$@)

# Host build and every cross build together.
.PHONY: clean-all
clean-all:
	rm -rf $(BUILD_ROOT)

# --- one command per cross target ----------------------------------
#
# `make <target>` re-enters this Makefile with TARGET set, which selects
# the toolchain file and the output directory. A cross build produces
# the archive only: the CLI has a main(), and on a phone the interpreter
# links into an app rather than an executable.
.PHONY: $(CROSS_TARGETS)
$(CROSS_TARGETS):
	@$(MAKE) --no-print-directory TARGET=$@ $(BUILD_ROOT)/$@/libaussom.a
	@echo "built $(BUILD_ROOT)/$@/libaussom.a"

# Names the targets available, so the list never has to be maintained
# in two places.
.PHONY: targets
targets:
	@for t in $(CROSS_TARGETS); do echo "  make $$t"; done

# The version this tree builds, as read from include/aussom/version.h.
.PHONY: version
version:
	@echo "$(AUSSOM_VERSION)$(AUSSOM_VERSION_LABEL)"

rebuild: clean all

-include $(DEPS)
