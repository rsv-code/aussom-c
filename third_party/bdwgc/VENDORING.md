# Vendoring notes: bdwgc 8.2.12

Source: https://github.com/bdwgc/bdwgc/releases/download/v8.2.12/gc-8.2.12.tar.gz
SHA-256: 42e5194ad06ab6ffb806c83eb99c03462b495d979cda782f3c72c08af833cd4e

## Modifications

None. Every file is byte-for-byte upstream. The collector's licence asks
that modified code carry a notice saying so; there is nothing to declare
because nothing was changed.

## What was taken

- the 31 library sources at the distribution root
- `include/*.h` and `include/private/*.h`

## What was left out, and why

- **The GNU-style build files** (`configure`, `config.guess`, `config.sub`,
  `ltmain.sh`, `aclocal.m4`, `m4/`, `Makefile.am`, `Makefile.in`, `compile`,
  `depcomp`, `missing`, `install-sh`, `test-driver`, `autogen.sh`). These are
  the files the upstream licence singles out as carrying different terms,
  some of them GPL with an exception. They are build scaffolding, not part
  of the collector, and this project builds the sources from its own
  Makefile, so they never enter the build. See `THIRD-PARTY.md`.
- `CMakeLists.txt`, `Config.cmake.in`, `bdw-gc.pc.in`, and the per-platform
  makefiles (`NT_MAKEFILE`, `OS2_MAKEFILE`, `digimars.mak`, `SMakefile.amiga`,
  `WCC_MAKEFILE`, `PCR-Makefile`, `Makefile.direct`): other build systems.
- `tests/`, `doc/`, `cord/`, `tools/`, `extra/`, `ChangeLog`: not built.
- The C++ support (`gc_cpp.cc`, `gc_badalc.cc`): the interpreter is C.
- `include/config.h.in`: only used by the autotools path, which is not used.
  Without `HAVE_CONFIG_H` the collector configures itself from compiler
  predefines in `include/private/gcconfig.h`, which is how PCRE2 is vendored
  here too.
- The architecture assembly stubs (`ia64_save_regs_in_stack.s`,
  `sparc_mach_dep.S`, `sparc_netbsd_mach_dep.s`): for architectures this
  project does not target.

## Build configuration

The define set is in the Makefile, under the bdwgc section, with the
reasoning for each.
