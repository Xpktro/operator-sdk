#pragma once
// The store a mode reads its files out of, mocked.
//
// It carries the read side the harness needs, mount and open and read and seek and
// close, over a pool of host file handles rooted at exchange_root(). The harness
// resolves and checks the path first, so this maps a relative path straight onto
// that root and opens it.

#include <cstdint>
#include <span>
#include <string>

namespace op::sim {

// Absolute path to the exchange tree the mock roots every file under. Each process
// gets its own, a `storage/exchange` subtree inside a scratch directory named for
// the process id under the system temp dir, so the whole sample-test suite can run
// under `ctest -jN` with every process on a tree of its own. Each consumer seeds
// and reads its own fixtures, so a private tree per process is transparent.
// Computed once on first call. The scratch directory is created fresh and removed
// when the process exits.
const std::string& exchange_root();

// File handle: a non-negative index into the mock's slot pool, or kInvalidFile.
using FileHandle                         = std::int32_t;
inline constexpr FileHandle kInvalidFile = -1;

// Minimal result code. Ok is the success value the harness compares against.
// Every other condition collapses to a single generic error.
enum class StoreError : std::uint8_t {
    Ok    = 0,
    Error = 1,
};

// 16 files open at once, which is what the device allows.
inline constexpr std::int32_t kMaxOpenFiles = 16;

class MockExchangeStore {
public:
    MockExchangeStore() = default;
    ~MockExchangeStore();

    MockExchangeStore(const MockExchangeStore&)            = delete;
    MockExchangeStore& operator=(const MockExchangeStore&) = delete;

    // Ensure the exchange_root() directory exists. Idempotent. Returns Ok on
    // success or when the root already exists, Error otherwise.
    StoreError mount();

    // Open <root>/<path> for reading. Returns a valid FileHandle on success or
    // kInvalidFile if the file is missing or no slot is free.
    FileHandle open(const char* path);

    // Read up to buffer.size() bytes into buffer. Returns the number of bytes read
    // (0 at EOF), or -1 on an invalid handle or a read error.
    std::int32_t read(FileHandle handle, std::span<std::uint8_t> buffer);

    // Absolute seek (SEEK_SET) to offset. Returns 0 on success, -1 on an invalid
    // handle or a seek error. Seeking past EOF is permitted.
    std::int32_t seek(FileHandle handle, std::uint32_t offset);

    // Close an open handle and free its slot. Returns Ok on success, Error on
    // an invalid handle.
    StoreError close(FileHandle handle);

private:
    // std::FILE* pool. A null entry marks a free slot. Stored as void* to keep
    // the header free of <cstdio>. The .cpp casts back to std::FILE*.
    void* handles_[kMaxOpenFiles] = {};
};

}  // namespace op::sim
