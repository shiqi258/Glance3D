#ifndef g3d_options_diff_h
#define g3d_options_diff_h

#include <string_view>

namespace f3d
{
class options;
}

namespace g3d
{
/**
 * The name of the first option whose value differs between @p a and @p b, empty when every option
 * is equal. One generated member comparison per option (options_generated.h): comparing through
 * f3d::options::isSame instead looks each name up in the generated getter chain, some 7000 string
 * compares for all options -- too slow for window_impl::IsG3DFrameStale, which asks every tick.
 */
std::string_view firstDifferentOption(const f3d::options& a, const f3d::options& b);
}

#endif
