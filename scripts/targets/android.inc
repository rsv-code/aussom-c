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

# Shared Android toolchain resolution.
#
# Named .inc rather than .mk on purpose: the Makefile turns every *.mk
# in this directory into a command, and this file is not a target.

# The NDK supplies the compiler. ANDROID_NDK_ROOT is accepted too,
# because both spellings are in circulation.
ANDROID_NDK_HOME ?= $(ANDROID_NDK_ROOT)
ifeq ($(strip $(ANDROID_NDK_HOME)),)
  $(error Android builds need the NDK. Set ANDROID_NDK_HOME, for example:\
    export ANDROID_NDK_HOME=$$HOME/Android/Sdk/ndk/<version>)
endif

# Oldest API level supported. Override with TARGET_ANDROID_API. The NDK
# encodes it in the compiler name rather than in a flag, so each level
# is a different driver binary.
TARGET_ANDROID_API ?= 24

# The NDK ships one prebuilt toolchain per build host. Apple silicon
# runs the darwin-x86_64 one under Rosetta; there is no arm64 tag.
ifeq ($(shell uname -s),Darwin)
  NDK_HOST_TAG := darwin-x86_64
else
  NDK_HOST_TAG := linux-x86_64
endif

NDK_BIN := $(ANDROID_NDK_HOME)/toolchains/llvm/prebuilt/$(NDK_HOST_TAG)/bin

ifeq ($(wildcard $(NDK_BIN)),)
  $(error No NDK toolchain at $(NDK_BIN). Check ANDROID_NDK_HOME.)
endif

AR := $(NDK_BIN)/llvm-ar

# Android is Linux for every flag this Makefile chooses on: ELF, dlopen,
# pthreads, -fPIC and -fvisibility=hidden all behave as on Linux.
HOST_OS := linux
