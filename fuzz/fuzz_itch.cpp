// libFuzzer target: the ITCH frame reader over arbitrary bytes.
//
// The reader walks a buffer handing out fixed-size frames. The property that
// matters is that it never reads past the end of what it was given, whatever
// the buffer length -- including lengths that are not a whole number of
// messages, which is the normal case for a partially-filled datagram.
//
// Build/run: see fuzz/fuzz_book.cpp.
#include "hft/itch_message.hpp"
#include "hft/itch_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  hft::ItchReader reader(reinterpret_cast<const std::byte*>(data), size);

  std::size_t seen = 0;
  while (reader.has_next()) {
    const hft::ItchMessage m = reader.next();
    // Consume every field so the parse cannot be optimised away.
    asm volatile("" : : "r,m"(m) : "memory");
    ++seen;
    if (seen > size) {  // cannot yield more messages than bytes
      std::fprintf(stderr, "reader produced more messages than input bytes\n");
      __builtin_trap();
    }
  }
  if (seen != size / hft::kMsgSize) {
    std::fprintf(stderr, "reader yielded %zu messages, expected %zu\n", seen, size / hft::kMsgSize);
    __builtin_trap();
  }
  return 0;
}
