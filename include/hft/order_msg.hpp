// Order and execution-report wire messages (engine <-> exchange).
//
// Same philosophy as ItchMessage: packed for a deterministic wire layout, but
// fields ordered widest-first so they stay naturally aligned (fixed 40 bytes).
//
// The `md_timestamp` field is the spine of the end-to-end latency measurement
// (Phase 10): the feed handler stamps each market-data message with its arrival
// TSC, the strategy copies that stamp into the OrderRequest it emits, and the
// exchange echoes it back in the ExecReport. Subtracting it from "now" at any
// downstream point gives the in-process latency from data arrival to that point.
#pragma once

#include <cstddef>
#include <cstdint>

namespace hft {

enum class OrderAction : std::uint8_t {
  kNew    = 'N',
  kCancel = 'C',
};

enum class ExecStatus : std::uint8_t {
  kAck      = 'A',
  kFilled   = 'F',
  kPartial  = 'P',
  kRejected = 'R',
  kCanceled = 'X',
};

#pragma pack(push, 1)
struct OrderRequest {
  std::uint64_t order_id;      // @0  engine-assigned, monotonic
  std::uint64_t md_timestamp;  // @8  carried market-data arrival TSC (e2e tag)
  std::int64_t  price;         // @16 fixed-point limit price
  std::uint32_t symbol;        // @24 packed ticker
  std::uint32_t size;          // @28 shares
  std::uint8_t  side;          // @32 Side
  std::uint8_t  action;        // @33 OrderAction
  std::uint16_t flags;         // @34
  std::uint32_t pad;           // @36 -> 40 bytes
};

struct ExecReport {
  std::uint64_t order_id;      // @0  matches the OrderRequest
  std::uint64_t md_timestamp;  // @8  echoed back for e2e latency
  std::int64_t  fill_price;    // @16
  std::uint32_t symbol;        // @24
  std::uint32_t fill_size;     // @28
  std::uint8_t  side;          // @32 Side
  std::uint8_t  status;        // @33 ExecStatus
  std::uint16_t flags;         // @34
  std::uint32_t pad;           // @36 -> 40 bytes
};
#pragma pack(pop)

inline constexpr std::size_t kOrderSize = 40;
inline constexpr std::size_t kExecSize = 40;

static_assert(sizeof(OrderRequest) == kOrderSize, "OrderRequest must be 40 bytes");
static_assert(sizeof(ExecReport) == kExecSize, "ExecReport must be 40 bytes");
static_assert(alignof(OrderRequest) == 1 && alignof(ExecReport) == 1);
static_assert(offsetof(OrderRequest, md_timestamp) == 8);
static_assert(offsetof(OrderRequest, price) == 16);
static_assert(offsetof(ExecReport, md_timestamp) == 8);
static_assert(offsetof(ExecReport, fill_price) == 16);
// Widest fields naturally aligned despite packing.
static_assert(offsetof(OrderRequest, price) % 8 == 0);
static_assert(offsetof(ExecReport, fill_price) % 8 == 0);

inline OrderAction order_action(const OrderRequest& o) noexcept {
  return static_cast<OrderAction>(o.action);
}
inline ExecStatus exec_status(const ExecReport& e) noexcept {
  return static_cast<ExecStatus>(e.status);
}

}  // namespace hft
