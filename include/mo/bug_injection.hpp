#pragma once
#include <string_view>
// Deliberate mutations for verification builds only; no branch or evaluation
// exists in ordinary builds.
#ifdef MO_INJECT_BUG
#define MO_BUG(name) (std::string_view(MO_INJECT_BUG) == std::string_view(name))
#else
#define MO_BUG(name) false
#endif
