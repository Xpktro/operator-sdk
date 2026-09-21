#pragma once
// Feeds the SMF parser from an open file.
//
// The caller owns the handle. MIDI Player opens it when the pick changes and
// closes it when the pick changes again, the parse fails, or the mode goes, so
// this only holds a copy of it and the size that was found.

#include <operator_sdk.h>

#include <cstdint>

namespace op::samples::midi_player {

class StorageFileReader {
public:
    constexpr StorageFileReader(int32_t handle, uint32_t file_size) noexcept
        : handle_(handle)
        , size_(file_size) { }

    // Read from an absolute offset. Reading near the end of the file returns what
    // was there, so a short count is a valid answer.
    //
    // Returns the byte count, -1 on error, or kStorageBusy while the store is
    // mid-write, which means the read is worth making again next tick. The caller
    // has to keep those two apart, since a busy read leaves the file exactly as it
    // was.
    int32_t read_at(uint32_t offset, uint8_t* buffer, uint32_t count) const noexcept {
        if (!::op::api) return -1;
        const int32_t sought = ::op::api->storage_file_seek(handle_, offset);
        if (sought == kStorageBusy) return kStorageBusy;
        if (sought != 0) return -1;
        return ::op::api->storage_file_read(handle_, buffer, count);
    }

    uint32_t size() const noexcept {
        return size_;
    }

private:
    int32_t handle_;
    uint32_t size_;
};

}  // namespace op::samples::midi_player
