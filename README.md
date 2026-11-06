# CacheForge

Task 1 implements a C++20 in-memory key-value storage library (`cfdb`).
The engine owns its keys and values, permits empty strings, and treats keys
as case-sensitive. It is single-threaded and does not persist data yet.

## Build and test

Prerequisites: a C++20 compiler and CMake 3.20 or newer. Ninja is optional.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

For a multi-configuration generator, add `--config Debug` to the build
command and `-C Debug` to the CTest command. To build only the library,
configure with `-DBUILD_TESTING=OFF`.

### This Windows workspace

The verified local build uses MSYS2's UCRT64 GCC and project-local CMake/Ninja
in `tmp/build-tools`. From `E:\CacheForge` in PowerShell:

```powershell
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
$cmake = '.\tmp\build-tools\cmake\data\bin\cmake.exe'
& $cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe -DCMAKE_MAKE_PROGRAM=E:/CacheForge/tmp/build-tools/bin/ninja.exe
& $cmake --build build
& '.\tmp\build-tools\cmake\data\bin\ctest.exe' --test-dir build --output-on-failure
```

These temporary tools and build outputs are ignored by Git. Other machines
should install the prerequisites and use the portable commands above.

## API

```cpp
#include "cfdb/engine.hpp"

cfdb::Engine engine;
engine.put("city", "Seattle");
auto city = engine.get("city"); // optional containing a copy of "Seattle"
bool removed = engine.erase("city"); // true
auto missing = engine.get("city"); // std::nullopt
```

- `put` inserts or replaces a value with owned copies of the input strings.
- `get` returns a value copy or `std::nullopt`, without changing storage.
- `erase` returns whether a key was removed.
- `size` reports the number of distinct stored keys.

The engine performs no console I/O. Callers can link the `cfdb` CMake target
to use its public headers and C++20 requirement.

## Source guide

- `include/cfdb/engine.hpp`: public contract and private memtable.
- `src/engine.cpp`: map-backed implementation.
- `tests/engine_test.cpp`: seven test groups covering insertion, replacement,
  missing lookups, deletion, empty strings, case sensitivity, ownership, and
  independent instances. Checks remain enabled in Release builds.

The memtable currently uses `std::map`. WAL persistence and recovery belong
to the next milestone.
