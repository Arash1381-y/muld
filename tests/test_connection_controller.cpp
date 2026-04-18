#include "connection_controller.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <unordered_map>
#include <vector>

using muld::ConnectionControlDecision;
using muld::ConnectionControlInput;
using muld::ConnectionController;

namespace {

std::unordered_map<std::uintptr_t, int> ToDesiredMap(
    const std::vector<ConnectionControlDecision>& decisions) {
  std::unordered_map<std::uintptr_t, int> out;
  for (const auto& decision : decisions) {
    out[decision.job_id] = decision.desired_connections;
  }
  return out;
}

}  // namespace

int main() {
  {
    ConnectionController controller(6);
    ConnectionControlInput in{
        .job_id = 1,
        .active = true,
        .desired_connections = 1,
        .active_connections = 1,
        .min_connections = 1,
        .max_connections = 5,
        .speed_limit_bps = 0,
        .throughput_bps = 1024.0 * 1024.0,
        .token_wait_ratio = 0.0,
    };

    auto d1 = controller.Tick({in});
    assert(d1.size() == 1);
    int desired = d1.front().desired_connections;
    assert(desired >= 1 && desired <= 5);

    // Under low wait and spare capacity, the controller should not regress.
    in.desired_connections = desired;
    in.throughput_bps *= 1.20;
    in.token_wait_ratio = 0.05;
    auto d2 = controller.Tick({in});
    assert(d2.size() == 1);
    assert(d2.front().desired_connections >= desired);
  }

  {
    ConnectionController controller(8);
    ConnectionControlInput in{
        .job_id = 2,
        .active = true,
        .desired_connections = 4,
        .active_connections = 4,
        .min_connections = 1,
        .max_connections = 6,
        .speed_limit_bps = 256 * 1024,
        .throughput_bps = 256.0 * 1024.0,
        .token_wait_ratio = 0.95,
    };

    auto warmup = controller.Tick({in});
    assert(warmup.size() == 1);
    in.desired_connections = warmup.front().desired_connections;

    auto reduced = controller.Tick({in});
    assert(reduced.size() == 1);
    assert(reduced.front().desired_connections < warmup.front().desired_connections);

    // Immediately after a downscale, cooldown prevents instant re-growth.
    in.desired_connections = reduced.front().desired_connections;
    in.token_wait_ratio = 0.0;
    auto cooldown = controller.Tick({in});
    assert(cooldown.size() == 1);
    assert(cooldown.front().desired_connections == reduced.front().desired_connections);
  }

  {
    ConnectionController controller(4);
    ConnectionControlInput a{
        .job_id = 11,
        .active = true,
        .desired_connections = 3,
        .active_connections = 3,
        .min_connections = 1,
        .max_connections = 4,
        .speed_limit_bps = 0,
        .throughput_bps = 900000.0,
        .token_wait_ratio = 0.9,
    };
    ConnectionControlInput b{
        .job_id = 22,
        .active = true,
        .desired_connections = 3,
        .active_connections = 3,
        .min_connections = 1,
        .max_connections = 4,
        .speed_limit_bps = 0,
        .throughput_bps = 900000.0,
        .token_wait_ratio = 0.1,
    };

    auto decisions = controller.Tick({a, b});
    assert(decisions.size() == 2);
    auto map = ToDesiredMap(decisions);
    assert(map.size() == 2);
    assert(map[11] >= 1 && map[22] >= 1);
    assert((map[11] + map[22]) <= 4);
  }

  std::cout << "[Test Passed] Connection controller hill-climbing logic is stable.\n";
  return 0;
}
