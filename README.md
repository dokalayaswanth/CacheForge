# CacheForge

CacheForge is a C++20 storage library (`cfdb`). Task 1 provides an in-memory
engine; Task 2 adds a binary write-ahead log (WAL); Task 3 connects the WAL
to the engine for automatic recovery when reopening a database. Task 4 adds
CRC-32 corruption detection and versioned WAL headers.
The engine owns its keys and values, permits empty strings, and treats keys
as case-sensitive. It is single-threaded, with in-memory and WAL-backed modes.

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
- `tests/wal_test.cpp`: fifteen test groups covering record ordering, reopen,
  binary data, exact bytes, malformed/truncated records, limits, and I/O errors.
- `tests/recovery_test.cpp`: eleven test groups covering engine recovery,
  read-only replay, continued appends, invalid inputs, and failure handling.
- `include/cfdb/checksum.hpp` and `src/checksum.cpp`: CRC-32/ISO-HDLC.
- `tests/checksum_test.cpp`: three independent CRC test vectors.

## WAL module (Tasks 2 and 4)

```cpp
#include "cfdb/wal.hpp"

cfdb::Wal wal("operations.wal"); // Stores the path; does not create a file yet.
wal.appendPut("city", "Seattle"); // Creates the file if necessary.
wal.appendPut("city", "Boston");
wal.appendDelete("city");
auto records = wal.readAll(); // Three operations, in their original order.
```

Every initialized WAL begins with an eight-byte header: ASCII `CFWL`, then
a four-byte little-endian version number (`1`). A header-only file is a valid
empty log. Nonempty headerless logs from Tasks 2-3 are **incompatible**: they
are rejected unchanged, not automatically upgraded. Preserve old logs; use a
new path for new-format databases. There is no migration tool in this milestone.

Each record follows this layout, with no padding or text separators:

| Field | Encoding |
| --- | --- |
| CRC32 | Four-byte unsigned little-endian checksum of the following record body |
| Operation | One byte: 1 = Put, 2 = Delete |
| Key length | Four-byte unsigned little-endian integer |
| Value length | Four-byte unsigned little-endian integer |
| Key | Exactly key-length bytes |
| Value | Exactly value-length bytes |

CRC-32/ISO-HDLC (IEEE) covers the exact encoded operation, lengths, key, and
value bytes, excluding the checksum itself and the file header. It uses the
reflected polynomial `0xEDB88320`, initial accumulator `0xFFFFFFFF`, and final
XOR `0xFFFFFFFF`. The standard ASCII `123456789` check value is `0xCBF43926`.
Per-record overhead is 13 bytes, in addition to the file's eight-byte header.

Delete records require a zero value length. Empty keys/values and arbitrary
string bytes are supported. Maximum key size is 65,536 bytes; maximum value
size is 1,048,576 bytes. Both writer and reader check lengths before writing
or allocating payloads. Oversized caller input throws `std::invalid_argument`.
I/O failures and malformed files throw `std::runtime_error`.

Every append validates input before touching the file, initializes a missing
or zero-byte file, validates an existing header, then appends a checksummed
record. Writes are flushed and explicitly closed with error checks.
`ensureExists()` initializes missing/zero-byte files or validates nonempty
headers; neither it nor append scans all existing records. A standalone caller
must ensure that existing records are valid.

`readAll()` never initializes a file: missing files, zero-byte files, partial
headers, incorrect magic, and unsupported versions are errors. It validates
record structure and checksums before accepting records. Header-only logs
return an empty vector. EOF is accepted only between records. A partial or
corrupt record anywhere rejects the entire read, without returning a partial
result or changing the file. Record errors include the starting byte offset.

This is a single-caller prototype and assumes any existing file is a valid
log before appending. A failed append can leave a partial record: stop using
that log until it has been inspected or recovered. There is no rollback or
automatic repair. Stream flushing is not `fsync` and does not promise
power-loss durability. CRC is not cryptographic authentication and cannot
detect every possible alteration. Removing complete trailing records leaves
a valid shorter log; record reordering is also not prevented by per-record CRCs.
`readAll()` loads the whole log into memory; per-record limits do not bound
total memory consumption.

## Engine recovery (Task 3)

```cpp
#include "cfdb/engine.hpp"

{
    cfdb::Engine engine("database.wal");
    engine.put("city", "Seattle");
    engine.put("city", "Boston");
    engine.put("language", "CPP");
    engine.erase("language");
}
{
    cfdb::Engine recovered("database.wal");
    auto city = recovered.get("city"); // "Boston"
    auto language = recovered.get("language"); // std::nullopt
}
```

`Engine()` still creates an in-memory-only store. The path constructor pins
the path to an absolute location, initializes a header-only log if missing or zero-byte (without
creating parent directories), validates all records, and replays them directly
into the memtable in order. Existing logs are never truncated. Recovery does
not call public mutation methods and therefore never appends duplicate records.
`Wal::ensureExists()` provides the non-truncating open/create helper.

In WAL-backed mode, `put` logs before updating memory. `erase` logs before
removing an existing key; deleting a missing key does not append anything.
`get` and `size` never write. WAL size limits apply to mutations in WAL-backed
mode; in-memory mode retains its original behavior. Oversized requests throw
`std::invalid_argument` before changing state, leaving the engine usable.

If WAL append fails, or memory update fails after logging, the engine enters
a failed state and rethrows the original exception. Every subsequent operation
throws `std::runtime_error`, including reads and no-op deletes. Close the engine
and inspect the log before reopening. A call that threw may still appear after
recovery if its record was written; an incomplete record instead prevents
reopening. This milestone does not attempt rollback or repair. Allocation failure
after logging is handled by the failure-state guard; the current fault tests
exercise append-open failures, not injected allocator or mid-write failures.

Engine copying and moving are disabled. Only one engine may use a given WAL
path at a time, and callers must not modify its file while the engine is live.
This ownership rule is a caller obligation, not a filesystem lock. Do not use
a separate `Wal` writer against an active engine's log.

Tests establish recovery across engine lifetimes, not power-loss safety.
Durable synchronization, torn-tail repair, process locking, and
concurrent access remain later milestones.
