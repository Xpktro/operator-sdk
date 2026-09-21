# Storage and Files {#storage-and-files}

Modes read files through the Storage API, scoped to their own `/extras/{mode_name}/` folder.

## Storage API

The @ref OperatorApi function table exposes a handle-based streaming storage family (@ref OperatorApi::storage_file_open "storage_file_open", @ref OperatorApi::storage_file_read "storage_file_read", @ref OperatorApi::storage_file_seek "storage_file_seek", @ref OperatorApi::storage_file_close "storage_file_close") plus the non-streaming read entry @ref OperatorApi::storage_read "storage_read". A symmetric write family (@ref OperatorApi::storage_file_open_write "storage_file_open_write", @ref OperatorApi::storage_file_write "storage_file_write", @ref OperatorApi::storage_file_close "storage_file_close") together with @ref OperatorApi::storage_delete "storage_delete" and @ref OperatorApi::storage_enumerate "storage_enumerate" lets a mode save, remove, and browse its own files. Every entry is scoped to the mode's own `/extras/{mode_name}/` folder, so a relative path resolves inside it. See [ABI Reference](reference-abi.md) for entry indices and full signatures.

**Picking files with `FilePicker`.** A @ref op::params::FilePicker "FilePicker" parameter is declared once and read through the typed `param<>` accessor. The FilePicker screen records the user's choice, and `param<MyPicker>()` returns a `FilePickerValue` carrying the resolved filename. Declaring the `FilePicker` widget itself is covered in @ref parameters.

```cpp
inline constexpr op::params::FilePicker MidiFile{
    .name      = "MIDI FILE",
    .extension = ".mid",
};

OP_MODE_PARAMS(MidiFile);

void process(OpMidiMessage*, std::uint8_t, std::uint32_t) {
    auto file = param<MidiFile>();
    if (file.empty()) {
        // User has not picked anything yet, render an empty/idle UI.
        return;
    }
    const int32_t handle = op::api->storage_file_open(file.filename);
    // ... use the streaming family from here ...
}
```

`file.filename` is a `char[N+1]` sized to the FilePicker's `max_filename_len + 1`, NUL-terminated. `file.empty()` is true iff `filename[0] == '\0'`. Under the hood `param<>` calls @ref OperatorApi::get_param_filename "get_param_filename" for you. Modes can call that ABI entry directly when they need control over the destination buffer, but the typed wrapper is preferred.

**Picked files and mode-owned files.** A file the user chooses resolves through `param<MyPicker>()` (or @ref OperatorApi::get_param_filename "get_param_filename"), which hands back the filename itself. A mode that owns its files writes them with the streaming write family and browses them with @ref OperatorApi::storage_enumerate "storage_enumerate", which packs the mode's own file names NUL-separated into a caller-supplied buffer.

**Names cannot begin with a dot.** Some operating systems tend to create files with information neither the device or any user would need during normal device operation. These are mostly files whose names begin with a single dot, so any file written in the device storage beginning with a dot is refused returning `-1`. If anyone places such files using the USB drive mode, they will be erased when the device reboots. This rule is enforced in both the device and the simulator.

### Handling `kStorageBusy`

The storage API entries that may be called from `process()` can return `-2` (`kStorageBusy`) in addition to the usual `-1` (I/O error). This happens when the other core is in the middle of a flash operation. The device runs a mode on Core 1 and its interface on Core 0, a split @ref how-operator-works describes in full.

**Affected entries** (Core-1-reachable):

- `storage_file_open`, `storage_file_close`
- `storage_file_read`, `storage_file_seek`
- `storage_read`
- `get_param_filename` (reserved by the contract. It never answers `-2` today, and later firmware may.)

**Calls made while your mode loads** (from `init`) never see `kStorageBusy`.

@warning If your mode calls storage functions inside `process()` (running on Core 1), check for `kStorageBusy` before treating `n < 0` as fatal:

```cpp
int32_t n = op::api->storage_file_read(handle, buf, size);
if (n == /* kStorageBusy = */ -2) {
    // The other core was writing to flash. Retry next process() call.
    // Don't advance file position state, let the buffer drain in
    // the meantime if you have one.
    return;
}
if (n < 0) {
    // Real I/O error (file closed, hardware fault, etc.), handle
    // as before.
    return;
}
// n bytes successfully read, proceed.
```

The same pattern applies to every Core-1-reachable storage entry: distinguish `-2` (transient, retry next `process()`) from `-1` (real error, propagate or surface to UI). The midi-player sample at `samples/midi-player/storage_reader.h` and `samples/midi-player/main.cpp` demonstrates the retry plumbing in a templated SmfReader adapter.

**Conservative fallback:** Modes that don't differentiate (test only `n < 0`) still get safe behavior. `kStorageBusy` is treated as an error, which means the mode may surface a transient stutter but cannot corrupt state or hang the device.

**Why `kStorageBusy` happens:** both cores share the device's flash. While the other core is writing to it, saving a file or persisting settings, a read from your `process()` cannot go through at that exact moment. Rather than make your `process()` wait, the storage call returns `kStorageBusy` so you can try again on the next cycle. It is brief, and it never corrupts or loses data.

## Storage write discipline {#storage-write-discipline}

Writing to the device's storage wears it a little each time, so writes are worth treating as expensive and keeping rare. Most modes never write at all, so this section is for a mode that wants file persistence of its own.

**Gathering on Core 1, writing on a gesture.** Changes accumulate in instance state while `process()` runs and go out once. An explicit Save is a good moment, as is the point where the user leaves the mode's screen with changes outstanding.

**Writing from Core 0.** Storage writes happen on Core 0, so a write started from `process()` comes back as `kStorageBusy`. `storage_file_open_write`, `storage_file_write` and `storage_file_close` belong in an input gesture or an explicit Save, both of which run on Core 0.

**A full store ends the write.** A write to a full volume returns `kStorageNoSpace` (`-3`). The file is closed and the user told, since only they can free the space.

**Small files go through `op::write_file`.** For data that fits in one buffer, the opt-in `#include <operator_sdk/storage.h>` gives `op::write_file(path, buf, size)`, which creates the file, writes the buffer and closes it in one call. A large recording is better streamed in pieces, holding a write handle across gestures.

**Saving an inbound bulk dump.** The handler runs on the real-time core, so the dump's fragments accumulate in instance state as they arrive and the file goes out from a gesture or a Save. Receiving SysEx is covered in @ref receiving-sysex.
