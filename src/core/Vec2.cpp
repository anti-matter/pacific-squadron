// Non-inline pieces kept out of the header so the static library has a
// translation unit (keeps some linkers happy and allows future changes
// without header rebuilds).
#include "core/Vec2.h"

namespace ww {
// Currently everything in Vec2.h is constexpr/inline; this TU exists for
// library structure and future non-inline helpers.
}  // namespace ww
