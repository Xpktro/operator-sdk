#pragma once
/// @file storage.h
/// @brief Saving a whole file in one call with `op::write_file(...)`.
///
/// Opt-in, header-only. Include on demand:
///
///     #include <operator_sdk/storage.h>
///     const uint8_t bytes[] = {0x01, 0x02, 0x03};
///     int32_t written = op::write_file("clip.bin", bytes, sizeof(bytes));
///
/// `op::write_file` opens the file, writes the whole buffer, and closes it
/// again. The path is a relative name under the mode's own storage area. A
/// large recording that arrives in pieces is better written through @ref
/// OperatorApi::storage_file_open_write "storage_file_open_write", @ref
/// OperatorApi::storage_file_write "storage_file_write" and @ref
/// OperatorApi::storage_file_close "storage_file_close", holding the handle
/// across process() calls.

#include <cstdint>

#include <operator_sdk/abi/mode_api.h>  // OperatorApi, kStorageBusy, kStorageNoSpace

namespace op {

extern const OperatorApi* api;

/// @brief Save a buffer as one of the mode's own files.
///
/// Creates `path`, or replaces what is already there, writes `size` bytes
/// and closes the file. Returns the bytes written on success. A negative return
/// is -1 on an error, @ref kStorageBusy while storage is busy, or
/// @ref kStorageNoSpace when there is no room left, and -1 before the mode is
/// fully loaded (while @ref op::api is null).
///
/// @param path Relative file name under the mode's storage area.
/// @param buf Source bytes.
/// @param size Number of bytes to write.
/// @return Bytes written, or a negative storage result.
inline int32_t write_file(const char* path, const uint8_t* buf, uint32_t size) {
    if (!api) return -1;
    int32_t handle = api->storage_file_open_write(path, kStorageOpenTruncate);
    if (handle < 0) return handle;
    int32_t written = api->storage_file_write(handle, buf, size);
    api->storage_file_close(handle);
    return written;
}

}  // namespace op
