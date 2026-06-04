// Project version + compile-time environment introspection.
#pragma once

namespace hft {

inline constexpr int kVersionMajor = 0;
inline constexpr int kVersionMinor = 1;
inline constexpr int kVersionPatch = 0;

inline constexpr const char* version_string() { return "0.1.0"; }

// True when built for the full (Linux) engine. Networking and thread-affinity
// code paths are only compiled when this is true.
inline constexpr bool kLinuxEngine =
#if defined(HFT_LINUX) && HFT_LINUX
    true;
#else
    false;
#endif

}  // namespace hft
