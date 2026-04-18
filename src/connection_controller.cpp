#include "connection_controller.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace muld {

namespace {
constexpr double kThroughputAlpha = 0.35;
constexpr double kWaitAlpha = 0.50;
constexpr double kHighWaitThreshold = 0.55;
constexpr double kLowWaitThreshold = 0.20;
constexpr double kProbeGainThreshold = 0.08;
constexpr int kUpCooldownTicks = 1;
constexpr int kDownCooldownTicks = 2;
}  // namespace

ConnectionController::ConnectionController(int global_capacity)
    : global_capacity_(std::max(1, global_capacity)) {}

std::vector<ConnectionControlDecision> ConnectionController::Tick(
    const std::vector<ConnectionControlInput>& inputs) {
  std::vector<ConnectionControlDecision> decisions;
  decisions.reserve(inputs.size());

  std::unordered_set<std::uintptr_t> seen_ids;
  seen_ids.reserve(inputs.size());

  struct MutableDecision {
    ConnectionControlDecision decision;
    int min_connections = 1;
    double ema_wait_ratio = 0.0;
  };

  std::vector<MutableDecision> active;
  active.reserve(inputs.size());

  for (const auto& input : inputs) {
    seen_ids.insert(input.job_id);

    if (!input.active) {
      states_.erase(input.job_id);
      continue;
    }

    auto& state = states_[input.job_id];
    if (state.ema_throughput <= 0.0) {
      state.ema_throughput = std::max(0.0, input.throughput_bps);
    } else {
      state.ema_throughput =
          (1.0 - kThroughputAlpha) * state.ema_throughput +
          kThroughputAlpha * std::max(0.0, input.throughput_bps);
    }
    state.ema_wait_ratio =
        (1.0 - kWaitAlpha) * state.ema_wait_ratio +
        kWaitAlpha * std::clamp(input.token_wait_ratio, 0.0, 1.0);

    const int min_connections = std::max(1, input.min_connections);
    const int max_connections =
        std::max(min_connections, input.max_connections);
    int desired =
        std::clamp(input.desired_connections, min_connections, max_connections);

    if (state.cooldown_ticks > 0) {
      --state.cooldown_ticks;
    }

    const bool high_wait = state.ema_wait_ratio >= kHighWaitThreshold;
    const bool low_wait = state.ema_wait_ratio <= kLowWaitThreshold;

    if (high_wait && desired > min_connections) {
      // High token wait means connections are contending for a limited budget.
      desired -= 1;
      state.cooldown_ticks = kDownCooldownTicks;
      state.probe_active = false;
    } else if (state.cooldown_ticks == 0 && low_wait &&
               desired < max_connections) {
      if (!state.probe_active) {
        state.probe_active = true;
        state.probe_baseline = std::max(1.0, state.ema_throughput);
        desired += 1;
        state.cooldown_ticks = kUpCooldownTicks;
      } else {
        const double baseline = std::max(1.0, state.probe_baseline);
        const double gain =
            (state.ema_throughput - baseline) / std::max(1.0, baseline);
        if (gain < kProbeGainThreshold && desired > min_connections) {
          desired -= 1;
          state.cooldown_ticks = kDownCooldownTicks;
        }
        state.probe_active = false;
      }
    } else if (!low_wait) {
      state.probe_active = false;
    }

    active.push_back(MutableDecision{
        .decision = ConnectionControlDecision{
            .job_id = input.job_id, .desired_connections = desired},
        .min_connections = min_connections,
        .ema_wait_ratio = state.ema_wait_ratio,
    });
  }

  // Prune controller state for jobs that disappeared from manager snapshots.
  for (auto it = states_.begin(); it != states_.end();) {
    if (seen_ids.find(it->first) == seen_ids.end()) {
      it = states_.erase(it);
    } else {
      ++it;
    }
  }

  int desired_total = 0;
  for (const auto& item : active) {
    desired_total += item.decision.desired_connections;
  }

  if (desired_total > global_capacity_) {
    std::vector<std::size_t> order(active.size());
    for (std::size_t i = 0; i < active.size(); ++i) {
      order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
      if (active[lhs].ema_wait_ratio == active[rhs].ema_wait_ratio) {
        return active[lhs].decision.desired_connections >
               active[rhs].decision.desired_connections;
      }
      return active[lhs].ema_wait_ratio > active[rhs].ema_wait_ratio;
    });

    int excess = desired_total - global_capacity_;
    while (excess > 0) {
      bool reduced = false;
      for (const std::size_t idx : order) {
        auto& item = active[idx];
        if (item.decision.desired_connections > item.min_connections) {
          item.decision.desired_connections -= 1;
          --excess;
          reduced = true;
          if (excess == 0) {
            break;
          }
        }
      }
      if (!reduced) {
        break;
      }
    }
  }

  decisions.reserve(active.size());
  for (const auto& item : active) {
    decisions.push_back(item.decision);
  }
  return decisions;
}

void ConnectionController::Remove(std::uintptr_t job_id) {
  states_.erase(job_id);
}

}  // namespace muld
