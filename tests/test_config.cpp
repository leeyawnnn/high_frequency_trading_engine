// Phase 5: JSON config loader.
#include "test_harness.hpp"
#include "hft/config.hpp"

#include <string>

HFT_TEST(parse_nested_and_typed_access) {
  const std::string json = R"({
    "_comment": "ignored",
    "cores": { "feed_handler": 2, "strategy": 3, "order_gateway": 4 },
    "network": { "feed_port": 31337, "transport": "udp" },
    "strategy": { "imbalance_threshold": 0.30, "enabled": true },
    "risk": { "max_position": 1000 }
  })";
  hft::Config c = hft::Config::parse(json);

  CHECK_EQ(c.get_int("cores.feed_handler", -1), 2);
  CHECK_EQ(c.get_int("cores.order_gateway", -1), 4);
  CHECK_EQ(c.get_int("network.feed_port", 0), 31337);
  CHECK(c.get_string("network.transport", "") == "udp");
  CHECK_NEAR(c.get_double("strategy.imbalance_threshold", 0.0), 0.30, 1e-12);
  CHECK(c.get_bool("strategy.enabled", false));
  CHECK_EQ(c.get_int("risk.max_position", 0), 1000);
}

HFT_TEST(missing_keys_return_defaults) {
  hft::Config c = hft::Config::parse(R"({ "a": { "b": 1 } })");
  CHECK_EQ(c.get_int("a.b", -1), 1);
  CHECK_EQ(c.get_int("a.missing", -7), -7);       // missing leaf
  CHECK_EQ(c.get_int("x.y.z", 99), 99);           // missing branch
  CHECK_EQ(c.get_int("a", 42), 42);               // object isn't an int
  CHECK(c.get_string("a.b", "fallback") == "fallback");  // wrong type -> default
}

HFT_TEST(handles_negatives_and_floats) {
  hft::Config c = hft::Config::parse(R"({ "v": -12.5, "e": 1e3, "z": 0 })");
  CHECK_NEAR(c.get_double("v", 0.0), -12.5, 1e-12);
  CHECK_NEAR(c.get_double("e", 0.0), 1000.0, 1e-12);
  CHECK_EQ(c.get_int("z", -1), 0);
}

HFT_TEST(malformed_throws) {
  bool threw = false;
  try {
    hft::Config::parse(R"({ "a": )");  // truncated
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
}

HFT_TEST_MAIN()
