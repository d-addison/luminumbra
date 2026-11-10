#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace Luminumbra::Persistence {
// Streams into an exclusively created sibling. Commit checks flush, close and
// ownership before replacing the destination. False leaves the destination intact.
// Publication is atomic visibility, without a post-publication durability flush.
class ReplacementOutputFile {
public:
    explicit ReplacementOutputFile(const std::filesystem::path& destination);
    ~ReplacementOutputFile();
    ReplacementOutputFile(const ReplacementOutputFile&) = delete;
    ReplacementOutputFile& operator=(const ReplacementOutputFile&) = delete;
    [[nodiscard]] bool is_open() const;
    bool write(std::string_view bytes);
    bool commit();
    // Publish <destination>.regions.json first, with a durable undo record;
    // destination replacement is the commit point. Null removes the companion.
    // Companion bytes and undo storage are bounded to 96 MiB.
    bool commit_with_companion(const std::string* bytes);

private:
    bool prepare();
    bool publish();
    struct State;
    std::unique_ptr<State> m_state;
};
// Holds the directory's interprocess publication lock, recovering an interrupted
// pair before admission. Keep it alive across BOTH replay and companion reads.
// An invalid/unrecoverable journal refuses admission without consuming a pair.
class ReplayPairReadGuard {
public:
    explicit ReplayPairReadGuard(const std::filesystem::path& replay);
    ~ReplayPairReadGuard();
    ReplayPairReadGuard(const ReplayPairReadGuard&) = delete;
    ReplayPairReadGuard& operator=(const ReplayPairReadGuard&) = delete;
    [[nodiscard]] bool is_open() const;

private:
    struct State;
    std::unique_ptr<State> m_state;
};
} // namespace Luminumbra::Persistence
