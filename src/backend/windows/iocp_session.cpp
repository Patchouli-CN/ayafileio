#include "iocp_session.hpp"

namespace ayafileio::backend::windows {

std::optional<uint64_t> refresh_session_size(Session& session) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(session.handle, &size) || size.QuadPart < 0) return std::nullopt;
    return static_cast<uint64_t>(size.QuadPart);
}

void refresh_session_size_after_short_io(Session& session) {
    if (auto size = refresh_session_size(session)) session.state.set_cached_size(*size);
}

} // namespace ayafileio::backend::windows
