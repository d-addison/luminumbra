#pragma once

#include <filesystem>
#include <memory>
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

private:
    struct State;
    std::unique_ptr<State> m_state;
};
} // namespace Luminumbra::Persistence
