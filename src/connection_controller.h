#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace muld {

struct ConnectionControlInput {
  std::uintptr_t job_id = 0;
  bool active = false;
  int desired_connections = 1;
  int active_connections = 0;
  int min_connections = 1;
  int max_connections = 1;
  std::size_t speed_limit_bps = 0;
  double throughput_bps = 0.0;
  double token_wait_ratio = 0.0;
};

struct ConnectionControlDecision {
  std::uintptr_t job_id = 0;
  int desired_connections = 1;
};

class ConnectionController {
 public:
  explicit ConnectionController(int global_capacity);

  std::vector<ConnectionControlDecision> Tick(
      const std::vector<ConnectionControlInput>& inputs);
  void Remove(std::uintptr_t job_id);

 private:
  struct JobState {
    double ema_throughput = 0.0;
    double ema_wait_ratio = 0.0;
    int cooldown_ticks = 0;
    bool probe_active = false;
    double probe_baseline = 0.0;
  };

  int global_capacity_;
  std::unordered_map<std::uintptr_t, JobState> states_;
};

}  // namespace muld
