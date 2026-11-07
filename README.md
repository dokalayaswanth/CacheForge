# CacheForge

CacheForge is a C++20 storage library (`cfdb`). Task 1 provides an in-memory
engine; Task 2 adds a standalone binary write-ahead log (WAL) module.
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

- `include/cfdb/wal.hpp`: operation types, log API, and record size limits.
- `src/wal.cpp`: append writer and strict binary reader.
- `tests/wal_test.cpp`: twelve test groups covering record ordering, reopen,
  binary data, exact bytes, malformed/truncated records, limits, and I/O errors.

## WAL module (Task 2)

```cpp
#include "cfdb/wal.hpp"

cfdb::Wal wal("operations.wal"); // Stores the path; does not create a file yet.
wal.appendPut("city", "Seattle"); // Creates the file if necessary.
wal.appendPut("city", "Boston");
wal.appendDelete("city");
auto records = wal.readAll(); // Three operations, in their original order.
```

Each record contains these fields, with no padding or text separators:

| Field | Encoding |
| --- | --- |
| Operation | One byte: 1 = Put, 2 = Delete |
| Key length | Four-byte unsigned little-endian integer |
| Value length | Four-byte unsigned little-endian integer |
| Key | Exactly key-length bytes |
| Value | Exactly value-length bytes |

Delete records require a zero value length. Empty keys/values and arbitrary
string bytes are supported. Maximum key size is 65,536 bytes; maximum value
size is 1,048,576 bytes. Both writer and reader check lengths before writing
or allocating payloads. Oversized caller input throws `std::invalid_argument`.
I/O failures and malformed files throw `std::runtime_error`.

Every append opens the file in binary append mode, writes one record, flushes,
and explicitly closes it with error checks. Reading an existing empty file
returns an empty vector; a missing file is an error. EOF is accepted only
between records. A partial record rejects the entire read without changing
the file or returning a partial result.

This is a single-caller prototype and assumes any existing file is a valid
log before appending. A failed append can leave a partial record: stop using
that log until it has been inspected or recovered. There is no rollback or
automatic repair. Stream flushing is not `fsync` and does not promise
power-loss durability. This format has no checksum or version header yet,
so it cannot detect every corruption, including arbitrary payload changes.
`readAll()` loads the whole log into memory; per-record limits do not bound
total memory consumption.

The engine still uses `std::map` and does not automatically write or replay
the WAL. Engine recovery and durable synchronization are later milestones.
