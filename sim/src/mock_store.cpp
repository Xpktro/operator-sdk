// The store a mode reads its files out of, mocked. See mock_store.h.
//
// Maps the read side onto host primitives: <filesystem> for the root directory
// and <cstdio> for the handle pool.

#include "mock_store.h"

#include <cstdio>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <windows.h>   // GetCurrentProcessId
#else
#include <unistd.h>    // getpid
#endif

namespace op::sim {

namespace fs = std::filesystem;

namespace {

long process_id() {
#if defined(_WIN32)
    return static_cast<long>(::GetCurrentProcessId());
#else
    return static_cast<long>(::getpid());
#endif
}

// Owns the per-process scratch directory. Constructed on first exchange_root()
// call: the scratch tree is wiped fresh (defends against a reused PID leaving a
// tree behind after an abnormal exit) and its `storage/exchange` subtree
// created. The destructor removes the whole scratch tree at process exit so a
// normal run leaves nothing behind under the system temp dir.
struct ScratchRoot {
    std::string base;        // <temp>/operator-sdk-sim-<pid>
    std::string exchange;    // <base>/storage/exchange

    ScratchRoot() {
        std::error_code ec;
        fs::path root = fs::temp_directory_path(ec);
        if (ec) root = fs::path(".");
        root /= ("operator-sdk-sim-" + std::to_string(process_id()));
        base       = root.string();
        exchange   = (root / "storage" / "exchange").string();
        fs::remove_all(root, ec);
        fs::create_directories(exchange, ec);
    }

    ~ScratchRoot() {
        std::error_code ec;
        fs::remove_all(base, ec);
    }
};

}  // namespace

const std::string& exchange_root() {
    static const ScratchRoot root;
    return root.exchange;
}

MockExchangeStore::~MockExchangeStore() {
    for (auto*& slot : handles_) {
        if (slot != nullptr) {
            std::fclose(static_cast<std::FILE*>(slot));
            slot = nullptr;
        }
    }
}

StoreError MockExchangeStore::mount() {
    std::error_code ec;
    fs::create_directories(exchange_root(), ec);
    if (ec) {
        // create_directories reports an error only if the path could not be
        // created. An already-existing root is not an error.
        return fs::exists(exchange_root()) ? StoreError::Ok : StoreError::Error;
    }
    return StoreError::Ok;
}

FileHandle MockExchangeStore::open(const char* path) {
    if (path == nullptr || path[0] == '\0') return kInvalidFile;

    // Find a free slot.
    FileHandle slot = kInvalidFile;
    for (std::int32_t i = 0; i < kMaxOpenFiles; ++i) {
        if (handles_[i] == nullptr) {
            slot = i;
            break;
        }
    }
    if (slot == kInvalidFile) return kInvalidFile;

    // Resolve <root>/<path>. The harness has already validated/scoped the path,
    // so a leading '/' is treated as root-relative (stripped) rather than
    // absolute-host.
    fs::path full_path(exchange_root());
    full_path /= (path[0] == '/') ? (path + 1) : path;

    std::FILE* file = std::fopen(full_path.string().c_str(), "rb");
    if (file == nullptr) return kInvalidFile;

    handles_[slot] = file;
    return slot;
}

std::int32_t MockExchangeStore::read(FileHandle handle, std::span<std::uint8_t> buffer) {
    if (handle < 0 || handle >= kMaxOpenFiles) return -1;
    std::FILE* file = static_cast<std::FILE*>(handles_[handle]);
    if (file == nullptr) return -1;
    if (buffer.empty()) return 0;

    std::size_t bytes_read = std::fread(buffer.data(), 1, buffer.size(), file);
    if (bytes_read == 0 && std::ferror(file)) return -1;
    return static_cast<std::int32_t>(bytes_read);
}

std::int32_t MockExchangeStore::seek(FileHandle handle, std::uint32_t offset) {
    if (handle < 0 || handle >= kMaxOpenFiles) return -1;
    std::FILE* file = static_cast<std::FILE*>(handles_[handle]);
    if (file == nullptr) return -1;
    // Absolute seek from the start. Seeking past EOF is allowed, and a subsequent
    // read returns 0 bytes.
    if (std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0) return -1;
    return 0;
}

StoreError MockExchangeStore::close(FileHandle handle) {
    if (handle < 0 || handle >= kMaxOpenFiles) return StoreError::Error;
    std::FILE* file = static_cast<std::FILE*>(handles_[handle]);
    if (file == nullptr) return StoreError::Error;
    std::fclose(file);
    handles_[handle] = nullptr;
    return StoreError::Ok;
}

}  // namespace op::sim
