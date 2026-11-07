#pragma once

#include <compare>
#include <cstdint>
#include <functional>

namespace jrb {

/// Strongly typed, project-unique entity identifier. 0 means "no entity".
/// IDs are allocated monotonically per project and never reused, so a vector
/// that only ever appends new entities stays sorted by ID.
template <class Tag>
struct Id {
    std::uint64_t value = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    friend constexpr auto operator<=>(Id, Id) = default;
};

struct TrackTag;
struct PatternTag;
struct ClipTag;
struct NoteTag;

using TrackId = Id<TrackTag>;
using PatternId = Id<PatternTag>;
using ClipId = Id<ClipTag>;
using NoteId = Id<NoteTag>;

} // namespace jrb

template <class Tag>
struct std::hash<jrb::Id<Tag>> {
    std::size_t operator()(jrb::Id<Tag> id) const noexcept { return std::hash<std::uint64_t>{}(id.value); }
};
