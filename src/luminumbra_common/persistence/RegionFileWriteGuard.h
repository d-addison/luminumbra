#pragma once

#include <cstddef>
#include <filesystem>

namespace Luminumbra::Persistence::detail {

struct RegionFileSync;

// Protect an open native reader against replacement/deletion (also on Windows).
// Destroy every stream before this guard. Never hold it across another public
// persistence call: shared mutex acquisition is non-recursive.
class FilePublicationReadGuard {
public:
    explicit FilePublicationReadGuard(const std::filesystem::path& path);
    ~FilePublicationReadGuard();
    FilePublicationReadGuard(const FilePublicationReadGuard&) = delete;
    FilePublicationReadGuard& operator=(const FilePublicationReadGuard&) = delete;

private:
    RegionFileSync* m_sync;
};

// Serializes complete read/merge/publish transactions for LMR1, its manifest,
// and sibling save files. Publication readers do not acquire this writer lock.
// Recursive ownership permits composing a transaction with the public save
// helpers on the same thread. Fixed stripes
// bound storage; unrelated filenames can share a writer stripe.
class RegionFileWriteGuard {
public:
    explicit RegionFileWriteGuard(const std::filesystem::path& path);
    ~RegionFileWriteGuard();
    RegionFileWriteGuard(const RegionFileWriteGuard&) = delete;
    RegionFileWriteGuard& operator=(const RegionFileWriteGuard&) = delete;

    // Const contention diagnostic for this stripe, excluding its current owner.
    // Does not inspect files, acquire the writer lock, or change admission.
    std::size_t waiting_writers() const;

private:
    RegionFileSync* m_sync;
};

} // namespace Luminumbra::Persistence::detail
