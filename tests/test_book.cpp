// Phase 6: HFT-shaped flat-array order book, verified against a slow std::map
// reference implementation.
#include "hft/book_view.hpp"
#include "hft/itch_message.hpp"
#include "test_harness.hpp"

#include <cstdint>
#include <limits>
#include <map>
#include <random>
#include <string>

namespace {

// Deliberately slow, obviously-correct reference: a std::map per side. This is
// exactly the kind of book the flat array replaces; we trust it and check the
// fast book against it.
class ReferenceBook {
 public:
  void apply(const hft::ItchMessage& m) {
    auto& book = (m.side == static_cast<std::uint8_t>(hft::Side::kBuy)) ? bids_ : asks_;
    auto& lvl = book[m.price];
    switch (static_cast<hft::MsgType>(m.type)) {
      case hft::MsgType::kAdd:
        lvl += m.size;
        break;
      case hft::MsgType::kExecute:
      case hft::MsgType::kCancel:
        lvl = (m.size >= lvl) ? 0u : lvl - m.size;
        break;
      case hft::MsgType::kDelete:
        lvl = 0;
        break;
      default:
        break;
    }
    if (lvl == 0) book.erase(m.price);
  }

  bool has_bid() const { return !bids_.empty(); }
  bool has_ask() const { return !asks_.empty(); }
  std::int64_t best_bid_price() const { return bids_.rbegin()->first; }  // highest
  std::int64_t best_ask_price() const { return asks_.begin()->first; }   // lowest
  std::uint32_t best_bid_size() const { return bids_.rbegin()->second; }
  std::uint32_t best_ask_size() const { return asks_.begin()->second; }

 private:
  std::map<std::int64_t, std::uint32_t> bids_;
  std::map<std::int64_t, std::uint32_t> asks_;
};

template <std::size_t N>
void check_match(const hft::BookView<N>& fast, const ReferenceBook& ref) {
  CHECK_EQ(fast.has_bid(), ref.has_bid());
  CHECK_EQ(fast.has_ask(), ref.has_ask());
  if (ref.has_bid()) {
    CHECK_EQ(fast.best_bid_price(), ref.best_bid_price());
    CHECK_EQ(fast.best_bid_size(), ref.best_bid_size());
  }
  if (ref.has_ask()) {
    CHECK_EQ(fast.best_ask_price(), ref.best_ask_price());
    CHECK_EQ(fast.best_ask_size(), ref.best_ask_size());
  }
}

// Invariants that must hold after EVERY message, independently of the
// reference book. The differential test says "the fast book agrees with a
// slow one"; this says "the fast book is internally coherent", and the two
// catch different things -- a bug mirrored in both implementations passes the
// first check and fails this one.
template <std::size_t N>
void check_invariants(const hft::BookView<N>& b, int /*step*/) {
  // Note what is NOT asserted here: that the book is uncrossed.
  //
  // BookView applies exactly what it is sent. If a stream adds a bid above an
  // existing ask, the resulting crossed book is a faithful representation of
  // that stream, not a bug in the book. The generators below draw both sides
  // independently, so they cross constantly. Asserting "bid < ask" against
  // them reports a defect in the test data as a defect in the code -- which is
  // what the first version of this helper did, 400,000 times.
  //
  // never_crosses_under_realistic_stream covers the uncrossed property, using
  // a generator that maintains it.

  // A side that reports a best price must have a non-zero size there:
  // an empty level is not a best level.
  if (b.has_bid()) CHECK(b.best_bid_size() > 0);
  if (b.has_ask()) CHECK(b.best_ask_size() > 0);

  // Top-of-book must agree with a direct lookup at that price.
  if (b.has_bid()) {
    CHECK_EQ(b.size_at(hft::Side::kBuy, b.best_bid_price()), b.best_bid_size());
  }
  if (b.has_ask()) {
    CHECK_EQ(b.size_at(hft::Side::kSell, b.best_ask_price()), b.best_ask_size());
  }

  // Imbalance is a ratio of non-negative sizes and cannot leave [-1, 1].
  const double imb = b.imbalance();
  CHECK(imb >= -1.0 && imb <= 1.0);
}

hft::ItchMessage mk(hft::MsgType t, hft::Side s, std::int64_t price, std::uint32_t size) {
  hft::ItchMessage m{};
  m.type = static_cast<std::uint8_t>(t);
  m.side = static_cast<std::uint8_t>(s);
  m.price = price;
  m.size = size;
  return m;
}

}  // namespace

HFT_TEST(basic_top_of_book) {
  const std::int64_t base = hft::price_from_double(99.00);
  const std::int64_t tick = hft::kPriceScale / 100;  // 1 cent
  hft::BookView<4096> b(base, tick);

  CHECK(!b.has_bid());
  CHECK(!b.has_ask());

  b.apply(mk(hft::MsgType::kAdd, hft::Side::kBuy, hft::price_from_double(99.98), 300));
  b.apply(mk(hft::MsgType::kAdd, hft::Side::kBuy, hft::price_from_double(99.99), 500));
  b.apply(mk(hft::MsgType::kAdd, hft::Side::kSell, hft::price_from_double(100.01), 200));

  CHECK(b.has_bid());
  CHECK(b.has_ask());
  CHECK_EQ(b.best_bid_price(), hft::price_from_double(99.99));  // higher bid wins
  CHECK_EQ(b.best_bid_size(), 500u);
  CHECK_EQ(b.best_ask_price(), hft::price_from_double(100.01));
  CHECK_EQ(b.best_ask_size(), 200u);

  // Consume the whole top bid level -> next-best (99.98) becomes the touch.
  b.apply(mk(hft::MsgType::kExecute, hft::Side::kBuy, hft::price_from_double(99.99), 500));
  CHECK_EQ(b.best_bid_price(), hft::price_from_double(99.98));
  CHECK_EQ(b.best_bid_size(), 300u);

  // Delete the remaining bid level -> bid side empty.
  b.apply(mk(hft::MsgType::kDelete, hft::Side::kBuy, hft::price_from_double(99.98), 0));
  CHECK(!b.has_bid());
}

HFT_TEST(imbalance_signs) {
  const std::int64_t base = hft::price_from_double(99.00);
  const std::int64_t tick = hft::kPriceScale / 100;
  hft::BookView<4096> b(base, tick);
  b.apply(mk(hft::MsgType::kAdd, hft::Side::kBuy, hft::price_from_double(100.00), 900));
  b.apply(mk(hft::MsgType::kAdd, hft::Side::kSell, hft::price_from_double(100.01), 100));
  // Heavy bid -> positive imbalance ~ (900-100)/1000 = 0.8
  CHECK_NEAR(b.imbalance(), 0.8, 1e-9);
}

HFT_TEST(out_of_range_is_ignored_and_counted) {
  const std::int64_t base = hft::price_from_double(100.00);
  const std::int64_t tick = hft::kPriceScale / 100;
  hft::BookView<16> b(base, tick);  // tiny 16-tick window
  b.apply(mk(hft::MsgType::kAdd, hft::Side::kBuy, hft::price_from_double(50.00), 100));    // below
  b.apply(mk(hft::MsgType::kAdd, hft::Side::kSell, hft::price_from_double(200.00), 100));  // above
  CHECK(!b.has_bid());
  CHECK(!b.has_ask());
  CHECK_EQ(b.out_of_range(), 2u);
}

HFT_TEST(matches_reference_over_random_stream) {
  // Window: base 90.00, 4096 cent-ticks -> covers [90.00, 130.95]. Keep all
  // generated prices inside it so fast and reference books are comparable.
  const std::int64_t base = hft::price_from_double(90.00);
  const std::int64_t tick = hft::kPriceScale / 100;
  constexpr std::size_t N = 4096;
  hft::BookView<N> fast(base, tick);
  ReferenceBook ref;

  std::mt19937_64 rng(0xB00C);
  std::uniform_int_distribution<int> tick_off(0, static_cast<int>(N) - 1);
  std::uniform_int_distribution<int> which(0, 9);
  std::uniform_int_distribution<std::uint32_t> sz(1, 2000);

  for (int i = 0; i < 200'000; ++i) {
    const std::int64_t price = base + static_cast<std::int64_t>(tick_off(rng)) * tick;
    const hft::Side side = (rng() & 1) ? hft::Side::kBuy : hft::Side::kSell;
    const int roll = which(rng);
    hft::MsgType type;
    if (roll < 5)
      type = hft::MsgType::kAdd;  // 50% add
    else if (roll < 8)
      type = hft::MsgType::kExecute;  // 30% execute
    else if (roll < 9)
      type = hft::MsgType::kCancel;  // 10% cancel
    else
      type = hft::MsgType::kDelete;  // 10% delete

    const hft::ItchMessage m = mk(type, side, price, sz(rng));
    fast.apply(m);
    ref.apply(m);

    // Invariants are cheap, so they run after every single message rather
    // than on the sampling interval the cross-check uses.
    check_invariants(fast, i);
    if ((i & 0x3FF) == 0) check_match(fast, ref);  // compare periodically
  }
  check_match(fast, ref);             // and at the end
  CHECK_EQ(fast.out_of_range(), 0u);  // we kept everything in-window
}

HFT_TEST(invariants_hold_under_adversarial_stream) {
  // The differential test draws prices uniformly across 4096 ticks, so the two
  // sides rarely meet and the crossing logic is barely exercised. This one
  // concentrates every message into an eight-tick band so both sides fight
  // over the same levels constantly, which is where a crossed book or a stale
  // top-of-book pointer would appear.
  const std::int64_t base = hft::price_from_double(90.00);
  const std::int64_t tick = hft::kPriceScale / 100;
  constexpr std::size_t N = 4096;
  hft::BookView<N> fast(base, tick);
  ReferenceBook ref;

  std::mt19937_64 rng(0xDEADBEEF);
  std::uniform_int_distribution<int> tick_off(2000, 2007);  // eight adjacent ticks
  std::uniform_int_distribution<int> which(0, 9);
  std::uniform_int_distribution<std::uint32_t> sz(0, 50);  // includes 0-size

  for (int i = 0; i < 200'000; ++i) {
    const std::int64_t price = base + static_cast<std::int64_t>(tick_off(rng)) * tick;
    const hft::Side side = (rng() & 1) ? hft::Side::kBuy : hft::Side::kSell;
    const int roll = which(rng);
    hft::MsgType type = hft::MsgType::kAdd;
    if (roll >= 5 && roll < 8)
      type = hft::MsgType::kExecute;
    else if (roll == 8)
      type = hft::MsgType::kCancel;
    else if (roll == 9)
      type = hft::MsgType::kDelete;

    const hft::ItchMessage m = mk(type, side, price, sz(rng));
    fast.apply(m);
    ref.apply(m);
    check_invariants(fast, i);
  }
  check_match(fast, ref);
}

HFT_TEST(never_crosses_under_realistic_stream) {
  // A venue never publishes a crossed book, so a stream that respects the
  // spread must produce one that stays uncrossed at every step. The generator
  // keeps all bids strictly below all asks by construction; any crossing
  // observed here is the book losing track of its own top, which is the
  // failure this case exists to catch.
  const std::int64_t base = hft::price_from_double(90.00);
  const std::int64_t tick = hft::kPriceScale / 100;
  constexpr std::size_t N = 4096;
  constexpr int kMidTick = 2000;
  hft::BookView<N> fast(base, tick);
  ReferenceBook ref;

  std::mt19937_64 rng(0xC0FFEE);
  std::uniform_int_distribution<int> depth(1, 12);
  std::uniform_int_distribution<int> which(0, 9);
  std::uniform_int_distribution<std::uint32_t> sz(1, 500);

  for (int i = 0; i < 200'000; ++i) {
    const bool is_bid = (rng() & 1) != 0;
    // Bids strictly below the mid, asks strictly above it: the two sides can
    // never occupy the same tick, so the stream itself never crosses.
    const int off = is_bid ? kMidTick - depth(rng) : kMidTick + depth(rng);
    const std::int64_t price = base + static_cast<std::int64_t>(off) * tick;
    const hft::Side side = is_bid ? hft::Side::kBuy : hft::Side::kSell;

    const int roll = which(rng);
    hft::MsgType type = hft::MsgType::kAdd;
    if (roll >= 6 && roll < 8)
      type = hft::MsgType::kExecute;
    else if (roll == 8)
      type = hft::MsgType::kCancel;
    else if (roll == 9)
      type = hft::MsgType::kDelete;

    const hft::ItchMessage m = mk(type, side, price, sz(rng));
    fast.apply(m);
    ref.apply(m);
    check_invariants(fast, i);

    if (fast.two_sided() && !(fast.best_bid_price() < fast.best_ask_price())) {
      ::hft::test::report_failure(__FILE__, __LINE__,
                                  "crossed book at step " + std::to_string(i) + ": bid " +
                                      std::to_string(fast.best_bid_price()) + " >= ask " +
                                      std::to_string(fast.best_ask_price()));
      break;
    }
  }
  check_match(fast, ref);
}

HFT_TEST(zero_size_add_must_not_become_top_of_book) {
  // Regression: apply() called on_size_increased for every Add regardless of
  // size, so a zero-size Add at a better price installed an empty level as the
  // top of book. has_bid() then reported true with best_bid_size() == 0, and
  // imbalance() computed a signal from a level holding nothing.
  const std::int64_t base = hft::price_from_double(90.00);
  const std::int64_t tick = hft::kPriceScale / 100;
  hft::BookView<4096> b(base, tick);

  b.apply(mk(hft::MsgType::kAdd, hft::Side::kBuy, base + 100 * tick, 500));
  b.apply(mk(hft::MsgType::kAdd, hft::Side::kSell, base + 300 * tick, 400));

  // Zero-size adds at strictly better prices on both sides.
  b.apply(mk(hft::MsgType::kAdd, hft::Side::kBuy, base + 200 * tick, 0));
  b.apply(mk(hft::MsgType::kAdd, hft::Side::kSell, base + 200 * tick, 0));

  CHECK_EQ(b.best_bid_price(), base + 100 * tick);
  CHECK_EQ(b.best_bid_size(), 500u);
  CHECK_EQ(b.best_ask_price(), base + 300 * tick);
  CHECK_EQ(b.best_ask_size(), 400u);
  check_invariants(b, 0);
}

HFT_TEST(out_of_window_prices_never_corrupt_the_book) {
  // Prices outside the window must be counted and dropped, never folded back
  // into a valid index. Walk far past both edges, including values that would
  // wrap if the index arithmetic were done in an unsigned type.
  const std::int64_t base = hft::price_from_double(90.00);
  const std::int64_t tick = hft::kPriceScale / 100;
  constexpr std::size_t N = 256;
  hft::BookView<N> b(base, tick);

  b.apply(mk(hft::MsgType::kAdd, hft::Side::kBuy, base + 10 * tick, 100));
  b.apply(mk(hft::MsgType::kAdd, hft::Side::kSell, base + 20 * tick, 100));
  const std::int64_t bid = b.best_bid_price();
  const std::int64_t ask = b.best_ask_price();

  const std::int64_t far[] = {
      base - tick,                                 // one below the window
      base - 1'000'000 * tick,                     // far below
      base + static_cast<std::int64_t>(N) * tick,  // one past the top
      base + 1'000'000 * tick,                     // far above
      std::numeric_limits<std::int64_t>::min() / 2,
      std::numeric_limits<std::int64_t>::max() / 2,
  };
  std::uint64_t expected_dropped = 0;
  for (const std::int64_t p : far) {
    for (const hft::Side s : {hft::Side::kBuy, hft::Side::kSell}) {
      for (const hft::MsgType ty : {hft::MsgType::kAdd, hft::MsgType::kExecute,
                                    hft::MsgType::kCancel, hft::MsgType::kDelete}) {
        b.apply(mk(ty, s, p, 500));
        ++expected_dropped;
        check_invariants(b, 0);
      }
    }
  }
  CHECK_EQ(b.out_of_range(), expected_dropped);
  // Top of book is exactly as it was before the out-of-window traffic.
  CHECK_EQ(b.best_bid_price(), bid);
  CHECK_EQ(b.best_ask_price(), ask);
  CHECK_EQ(b.best_bid_size(), 100u);
  CHECK_EQ(b.best_ask_size(), 100u);
}

HFT_TEST_MAIN()
