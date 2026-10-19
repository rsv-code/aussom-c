# aussom-c

A C implementation of the Aussom programming language. It is a port of
the reference Java interpreter, `aussom-base`, that aims to match that
interpreter's behavior as closely as possible while running as a native
program built with the GNU toolchain (GCC and Make).

Three things define the project:

- **A faithful port.** It runs the same Aussom source, the same standard
  library, and the same language features as `aussom-base`. The full
  language regression suite (`tests/interpreter.aus`, 842 tests) passes.
- **Garbage collected.** Memory is managed automatically by the Boehm
  garbage collector. Aussom code never frees memory, and neither does the
  interpreter's own C code.
- **Extensible from C.** A host program that embeds the interpreter can
  add its own classes and functions written in C. The interpreter itself
  never loads code at run time.

This is version 0.2.0-alpha, which matches aussom-base 1.4.10.

## What Aussom is

Aussom is an object-oriented, dynamically typed scripting language. A
program is a set of classes; execution begins in the `main` method of the
first class that declares one. For the language itself (syntax, standard
library, style), see https://aussom-lang.com.

This repository is the *interpreter*, not the language definition. It is a
tree-walking interpreter: it parses source into an abstract syntax tree
(AST) and evaluates the tree directly.

## Building

You need a Linux system with:

- `gcc` and GNU `make`
- `flex` and `bison` (the lexer and parser generators)
- `xxd` (used to embed the standard-library sources and the time zone
  database into the binary)
- POSIX threads (`-pthread`, standard on Linux)

On Debian or Ubuntu:

```
sudo apt install build-essential flex bison xxd
```

All library dependencies (PCRE2, argparse, and the Boehm-Demers-Weiser
collector) and the IANA time zone database are vendored under
`third_party/` and built from source, so there is nothing else to
fetch. The collector in particular is built here
rather than taken from the system, because iOS and Android have no system
`libgc` and because building it ourselves pins its configuration instead
of inheriting whatever a distribution shipped.

Build and run:

```
make            # builds the interpreter at build/aussomc
                # and the embeddable library at build/libaussom.a
make lib        # builds only build/libaussom.a
make clean      # removes the host build only
make rebuild    # clean, then build
make targets    # lists the cross-build targets
```

## Cross builds

One command per target, named after the target:

```
make                    # the machine you are on
make android-arm64      # Android devices
make android-x86_64     # the Android emulator
```

The list comes from `scripts/targets/`, so every file there is a command
with nothing else to write. Re-running is cheap: nothing already built is
rebuilt.

### Output

```
build/                  # the machine you are on
build/android-arm64/    # one directory per target
build/android-x86_64/
```

Objects for one machine are not objects for another, so each target gets
its own directory; one directory holding both would produce an archive
that links nowhere. The plain build stays in `build/`, where it has
always been.

Each build is cleaned on its own:

```
make clean                    # the host build only
make clean-android-arm64      # one cross build
make clean-android-x86_64
make clean-all                # all of them together
```

A cross target builds the archive only -- `libaussom.a`. The CLI has a
`main()`, and on a phone the interpreter links into an app rather than
an executable.

### Prerequisites

Android needs the NDK:

```
export ANDROID_NDK_HOME=$HOME/Android/Sdk/ndk/<version>
```

API level 24 by default; override with `TARGET_ANDROID_API`.

Adding a target is adding a file to `scripts/targets/`. Nothing in the
Makefile changes and the new name becomes a command automatically.

Everything is compiled `-fPIC`, so the library objects can go into a
shared library as well as a static one. The vendored dependencies are
compiled `-fvisibility=hidden` so their symbols do not reach the dynamic
table of whatever links them; the interpreter's own `av_*`, `engine_*`
and `aussom_*` stay visible, because a library that a host loads at run
time resolves them against the running host.

### Cross-compiling for Android

The build takes its compiler, archiver, and output directory from the
command line, so an Android library needs no separate build system. Point
`CC` and `AR` at the NDK toolchain and give each ABI its own `BUILD`
directory:

```
NDK=$HOME/Android/Sdk/ndk/<version>
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64

make BUILD=build/android/arm64-v8a \
     CC=$TC/bin/aarch64-linux-android24-clang \
     AR=$TC/bin/llvm-ar \
     lib

make BUILD=build/android/x86_64 \
     CC=$TC/bin/x86_64-linux-android24-clang \
     AR=$TC/bin/llvm-ar \
     lib
```

`arm64-v8a` is what ships; `x86_64` is for the emulator on an x86
development machine. API level 24 is the minimum this project targets.
`lib` rather than the default goal, because the command-line interpreter
is not what an application embeds and cannot run on the target anyway.

Note that flex, bison, and xxd run on the build host, not the target, so
a cross build still needs them locally.

### Other platforms

The build picks its platform-specific link flags from `uname -s`. On a
cross build, say which target you mean with `HOST_OS`.

**macOS and iOS.** Nothing extra: `uname -s` reports Darwin, which
drops `-ldl` (dlopen lives in libSystem) and adds `-D_DARWIN_C_SOURCE`
so Apple's headers do not hide declarations behind the strict POSIX define. An iOS
build additionally wants the library target rather than the interpreter,
built with the Xcode toolchain for the device and simulator slices.

**Windows**, built natively under MSYS2 with the MinGW-w64 toolchain.

MSYS2 is one installation containing several environments, each with its
own shell and its own compiler. Use the **MINGW64** shell (or UCRT64),
never the MSYS shell. The MSYS compiler links `msys-2.0.dll`, a
Cygwin-derived POSIX layer, which makes the result both dependent on that
DLL at runtime and subject to its copyleft terms. `gcc -dumpmachine`
should say `x86_64-w64-mingw32`; if it says `x86_64-pc-msys`, you are in
the wrong shell.

```
pacman -S mingw-w64-x86_64-gcc make bison flex m4 vim
make
```

`bison` must be 3.x, for `%code requires` and `%define parse.error
verbose` in the grammar. `vim` is what supplies `xxd`. Nothing else goes
on the command line: `uname -s` reports `MINGW64_NT-...`, and the build
picks the Windows flags from that on its own.

Confirm the result is a native binary before shipping it:

```
objdump -p build/aussomc.exe | grep -i "DLL Name"
```

`msys-2.0.dll` must not be listed. UCRT64 works equally well, but links
the Universal C Runtime rather than the `msvcrt` named in
`THIRD-PARTY.md`, so that table needs updating if you ship a UCRT64
build.

This produces `aussomc.exe` and `libaussom.a`.

To cross-compile a Windows binary from Linux instead, name the target
and the toolchain explicitly:

```
make HOST_OS=mingw32 CC=x86_64-w64-mingw32-gcc AR=x86_64-w64-mingw32-ar
```

**Raspberry Pi** is an ordinary 64-bit Linux build. Nothing distinguishes
it from an aarch64 server at compile time, and a server wants the
pre-grown heap, so the smaller memory profile is a choice you make:

```
make GC_PROFILE=mobile
```

`GC_PROFILE` takes `auto` (the default; Android and iOS pick the small
heap, everything else pre-grows), `mobile`, or `desktop`.

## Running

```
aussomc [options] <aussom-file>
```

With no options, the interpreter loads the file, finds the first class
that declares a `main` method, creates an instance of it, and calls
`main`. The integer that `main` returns becomes the process exit code.

A minimal program (`hello.aus`):

```
class Main {
    public main() {
        c.println("Hello from Aussom.");
        return 0;
    }
}
```

```
./build/aussomc hello.aus
```

Options:

- `-h`, `--help` -- print usage and exit.
- `-t`, `--test` -- run the `@Test` methods defined in the file.
- `-a`, `--test-all` -- run the `@Test` methods in every class loaded by
  the file, including included modules.
- `-d`, `--doc` -- reserved for aussomdoc generation; not yet implemented
  in this port.

## Tests

There are two suites, and they test different layers.

**The Aussom language regression suite** runs through the interpreter's
own test runner, and is what proves the port matches `aussom-base`:

```
./build/aussomc --test tests/interpreter.aus
```

`tests/interpreter.aus` is an exact copy of the one in `aussom-base`,
which is the reference. Do not add tests to it or change it here; copy
it over again when `aussom-base` changes it.

Tests written for aussom-c go in their own Aussom files. Interpreter
tests that only aussom-c has are in `tests/c-interpreter.aus`. Some of
them check C-only behavior (the native stack guard, the call depth
limit, a map changed during a foreach) and fail under Java. The other
files should pass under both interpreters:

```
./build/aussomc --test tests/c-interpreter.aus
./build/aussomc --test tests/bufferCharsetTest.aus
./build/aussomc --test tests/stringUnicodeTest.aus
./build/aussomc --test tests/dateFormatTest.aus
```

**The C unit tests** cover the interpreter's own C API, the part a host
embeds. They use [greatest](https://github.com/silentbicycle/greatest),
a single-header framework vendored under `third_party/greatest`:

```
make test
```

Each `tests/test_*.c` builds into its own binary, so a crash costs one
file's results rather than the whole run, and each links `libaussom.a`
rather than the interpreter's `main`. To add a suite, drop a new
`tests/test_*.c` in place; the Makefile picks it up with no further
wiring. `tests/test_harness.c` is the template.

## Memory model

The interpreter uses the **Boehm-Demers-Weiser garbage collector**
(`libgc`). All interpreter values and most internal structures are
allocated from the GC heap and are never explicitly freed. The collector
reclaims them automatically once they are no longer reachable.

A few points worth knowing:

- **Allocation.** Internally, memory comes from `AUSSOM_ALLOC`, which is
  Boehm's `GC_MALLOC`. There are no matching free calls anywhere in the
  code base.
- **Conservative collection.** Boehm is a conservative mark-sweep
  collector. It finds live objects by scanning the stack, registers,
  global data, and its own heap for values that look like pointers into
  the heap. It also scans the data segments of loaded shared libraries,
  so global variables in a library a host loaded can safely hold
  references to Aussom values.
- **Startup heap.** The growth policy depends on the platform (see
  `aussom_gc_init`). On desktop the interpreter pre-grows the heap so
  allocation-heavy programs do not thrash through many small collections
  early on; the reservation is committed lazily, so small scripts pay
  almost nothing for it. On iOS and Android it does not pre-grow, because
  those systems kill processes on footprint and a small live heap
  collects quickly enough to make the trade worthwhile. Either way you
  can raise the starting heap with the standard `GC_INITIAL_HEAP_SIZE`
  environment variable.

The practical rule for anyone writing C that runs inside the interpreter
(including a host's own extern classes): allocate Aussom values with the provided
`av_*` constructors so they live in the one GC heap, and do not call
`free` on them.

## Threading model

The collector is built with thread support (`GC_THREADS`), and the
runtime's shared value collections are individually thread-safe:

- **Per-object locking.** Each shared value container -- a map, a list,
  or an object's member table -- carries its own lock. Concurrent reads
  and writes to the same container stay correct. This is coarser than
  fine-grained striping but simple and correct, which fits the runtime's
  design goal of letting an embedding program share Aussom objects across
  its own threads.
- **Frame-private state is lock-free.** A call frame's local variables,
  its environment, and its call stack are private to the thread running
  that frame and are never shared, so they skip locking entirely. This is
  why local-variable access is fast.
- **GC and threads.** Any thread that allocates must be known to the
  collector. Threads created through the standard pthreads path are
  registered automatically; a thread created by other means must call the
  registration helper before it allocates.

Note that the Aussom language does not yet expose a way for a script to
spawn threads. The thread-safety above exists so the interpreter can be
embedded in a multithreaded host program and so a host's C code behaves
correctly; today Aussom scripts themselves run on a single thread.

## Project layout

```
include/       public C headers
src/           interpreter implementation
src/parser/    flex (.l) and bison (.y) grammar sources
stdlib/aus/    standard-library Aussom source, embedded into the binary
third_party/   vendored libraries (PCRE2, argparse, bdwgc) and tzdata
tests/         test scripts, including the language regression suite
build/         build output (the binary and object files)
```

## Versioning

aussom-c is alpha. Its version is its own, not aussom-base's: the two are
released separately, and the C embedding API has no counterpart in
aussom-base. Each release says which aussom-base release it matches.

The version is written in one place, `include/aussom/version.h`, as
`AUSSOM_VERSION_MAJOR`, `_MINOR`, `_PATCH` and a label such as
`-alpha`. Everything else reads it from there:

- `aussom_version()` returns it at run time, and `sys.getAussomVersion()`
  returns the same string to a script (when `aussom.info.view` allows).
- `aussomc --help` shows it.
- The Makefile reads it; `make version` prints it.

The macros say what a host was **compiled** against, and work in `#if`.
`aussom_version()` says what is **running**.

While the major number is zero, nothing is promised: any release may
change or remove anything, so pin an exact version. From 1.0 the numbers
follow the usual rules for a C library:

| What changed | Old source still compiles | Old binaries still run | Bump |
|---|---|---|---|
| Only the implementation | yes | yes | patch |
| Something **added** to a header | yes | yes | minor |
| Something existing **changed or removed** | no | no | major |

Adding a field to a public struct is a major change, even though it looks
like an addition. A program compiled against the old header allocates the
old, smaller struct, and the new library then reads past its end.

## License

Copyright 2026 Austin Lehman

Licensed under the Apache License, Version 2.0. You may not use this
project except in compliance with the License. A copy of the License is
in the `LICENSE` file at the root of this repository, and is also
available at http://www.apache.org/licenses/LICENSE-2.0.

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.

Third-party components and their licenses are listed in
`THIRD-PARTY.md`, along with the notices that must accompany a binary
distribution.
