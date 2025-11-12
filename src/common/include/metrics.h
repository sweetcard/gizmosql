// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace gizmosql {

// P2-2: Lightweight metrics system for observability
// Provides thread-safe counters, gauges, and histograms
// Metrics can be exported in Prometheus text format
//
// THREAD SAFETY: All operations are fully thread-safe with mutex protection
// MEMORY BOUNDED: Histograms limited to max 10,000 observations per metric

class Metrics {
 public:
  // Thread-safe singleton (C++11 guarantees thread-safe static initialization)
  static Metrics& GetInstance() {
    static Metrics instance;
    return instance;
  }

  // Counter: monotonically increasing value
  // THREAD SAFE: Protected by mutex
  void IncrementCounter(const std::string& name, int64_t value = 1) {
    std::lock_guard<std::mutex> lock(counter_mutex_);
    // Explicit initialization to 0 if new entry
    auto& counter = counters_[name];
    counter.store(counter.load(std::memory_order_relaxed) + value,
                  std::memory_order_relaxed);
  }

  int64_t GetCounter(const std::string& name) const {
    std::lock_guard<std::mutex> lock(counter_mutex_);
    auto it = counters_.find(name);
    return it != counters_.end() ? it->second.load(std::memory_order_relaxed) : 0;
  }

  // Gauge: value that can go up and down
  // THREAD SAFE: Protected by mutex
  void SetGauge(const std::string& name, int64_t value) {
    std::lock_guard<std::mutex> lock(gauge_mutex_);
    gauges_[name].store(value, std::memory_order_relaxed);
  }

  void IncrementGauge(const std::string& name, int64_t delta = 1) {
    std::lock_guard<std::mutex> lock(gauge_mutex_);
    auto& gauge = gauges_[name];
    gauge.store(gauge.load(std::memory_order_relaxed) + delta,
                std::memory_order_relaxed);
  }

  void DecrementGauge(const std::string& name, int64_t delta = 1) {
    std::lock_guard<std::mutex> lock(gauge_mutex_);
    auto& gauge = gauges_[name];
    gauge.store(gauge.load(std::memory_order_relaxed) - delta,
                std::memory_order_relaxed);
  }

  int64_t GetGauge(const std::string& name) const {
    std::lock_guard<std::mutex> lock(gauge_mutex_);
    auto it = gauges_.find(name);
    return it != gauges_.end() ? it->second.load(std::memory_order_relaxed) : 0;
  }

  // Histogram: track distribution of values
  // MEMORY BOUNDED: Max 10,000 observations per histogram (rolling window)
  void ObserveHistogram(const std::string& name, double value) {
    std::lock_guard<std::mutex> lock(histogram_mutex_);
    auto& hist = histograms_[name];

    // P1-1 FIX: Limit histogram size to prevent unbounded memory growth
    // Use deque for O(1) pop_front
    hist.observations.push_back(value);
    hist.sum += value;
    hist.count++;

    // Keep only last 10,000 observations (rolling window)
    constexpr size_t MAX_OBSERVATIONS = 10000;
    if (hist.observations.size() > MAX_OBSERVATIONS) {
      double oldest = hist.observations.front();
      hist.observations.pop_front();
      hist.sum -= oldest;
      // count keeps growing (total observations ever)
    }
  }

  struct HistogramStats {
    double min = 0.0;
    double max = 0.0;
    double mean = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    int64_t count = 0;
    int64_t observation_count = 0;  // Current observations in window
  };

  HistogramStats GetHistogramStats(const std::string& name) const {
    std::lock_guard<std::mutex> lock(histogram_mutex_);
    auto it = histograms_.find(name);
    if (it == histograms_.end()) {
      return HistogramStats{};
    }

    const auto& hist = it->second;
    if (hist.observations.empty()) {
      return HistogramStats{};
    }

    // Convert deque to vector for sorting
    std::vector<double> sorted(hist.observations.begin(), hist.observations.end());
    std::sort(sorted.begin(), sorted.end());

    HistogramStats stats;
    stats.count = hist.count;
    stats.observation_count = static_cast<int64_t>(sorted.size());
    stats.min = sorted.front();
    stats.max = sorted.back();
    stats.mean = hist.sum / sorted.size();  // Mean of current window

    // P0-4 FIX: Correct percentile calculation
    // Use proper index formula: (size - 1) * percentile / 100
    size_t n = sorted.size();
    stats.p50 = sorted[std::min((n - 1) * 50 / 100, n - 1)];
    stats.p95 = sorted[std::min((n - 1) * 95 / 100, n - 1)];
    stats.p99 = sorted[std::min((n - 1) * 99 / 100, n - 1)];

    return stats;
  }

  // P0-2 FIX: Thread-safe export with mutex protection
  // Export metrics in Prometheus text format
  std::string ExportPrometheusFormat() const {
    std::ostringstream ss;

    // P0-2 FIX: Lock during entire export to prevent iterator invalidation
    {
      std::lock_guard<std::mutex> lock(counter_mutex_);
      for (const auto& [name, value] : counters_) {
        ss << "# TYPE " << name << " counter\n";
        ss << name << " " << value.load(std::memory_order_relaxed) << "\n";
      }
    }

    {
      std::lock_guard<std::mutex> lock(gauge_mutex_);
      for (const auto& [name, value] : gauges_) {
        ss << "# TYPE " << name << " gauge\n";
        ss << name << " " << value.load(std::memory_order_relaxed) << "\n";
      }
    }

    {
      std::lock_guard<std::mutex> lock(histogram_mutex_);
      for (const auto& [name, hist] : histograms_) {
        if (hist.observations.empty()) continue;

        auto stats = GetHistogramStatsUnsafe(hist);

        ss << "# TYPE " << name << " summary\n";
        ss << name << "_count " << stats.count << "\n";
        ss << name << "_sum " << hist.sum << "\n";
        ss << name << "{quantile=\"0.5\"} " << stats.p50 << "\n";
        ss << name << "{quantile=\"0.95\"} " << stats.p95 << "\n";
        ss << name << "{quantile=\"0.99\"} " << stats.p99 << "\n";
      }
    }

    return ss.str();
  }

  // Export metrics as JSON
  std::string ExportJSON() const {
    std::ostringstream ss;
    ss << "{\n";

    // Counters
    ss << "  \"counters\": {\n";
    {
      std::lock_guard<std::mutex> lock(counter_mutex_);
      bool first = true;
      for (const auto& [name, value] : counters_) {
        if (!first) ss << ",\n";
        ss << "    \"" << name << "\": " << value.load(std::memory_order_relaxed);
        first = false;
      }
    }
    ss << "\n  },\n";

    // Gauges
    ss << "  \"gauges\": {\n";
    {
      std::lock_guard<std::mutex> lock(gauge_mutex_);
      bool first = true;
      for (const auto& [name, value] : gauges_) {
        if (!first) ss << ",\n";
        ss << "    \"" << name << "\": " << value.load(std::memory_order_relaxed);
        first = false;
      }
    }
    ss << "\n  },\n";

    // Histograms
    ss << "  \"histograms\": {\n";
    {
      std::lock_guard<std::mutex> lock(histogram_mutex_);
      bool first = true;
      for (const auto& [name, hist] : histograms_) {
        if (hist.observations.empty()) continue;
        if (!first) ss << ",\n";

        auto stats = GetHistogramStatsUnsafe(hist);
        ss << "    \"" << name << "\": {\n";
        ss << "      \"count\": " << stats.count << ",\n";
        ss << "      \"observation_count\": " << stats.observation_count << ",\n";
        ss << "      \"mean\": " << stats.mean << ",\n";
        ss << "      \"min\": " << stats.min << ",\n";
        ss << "      \"max\": " << stats.max << ",\n";
        ss << "      \"p50\": " << stats.p50 << ",\n";
        ss << "      \"p95\": " << stats.p95 << ",\n";
        ss << "      \"p99\": " << stats.p99 << "\n";
        ss << "    }";
        first = false;
      }
    }
    ss << "\n  }\n";

    ss << "}\n";
    return ss.str();
  }

  // Reset all metrics (for testing only - not thread-safe with concurrent increments)
  void Reset() {
    std::lock_guard<std::mutex> c_lock(counter_mutex_);
    std::lock_guard<std::mutex> g_lock(gauge_mutex_);
    std::lock_guard<std::mutex> h_lock(histogram_mutex_);

    for (auto& [name, value] : counters_) {
      value.store(0, std::memory_order_relaxed);
    }
    for (auto& [name, value] : gauges_) {
      value.store(0, std::memory_order_relaxed);
    }
    histograms_.clear();
  }

 private:
  Metrics() = default;
  ~Metrics() = default;
  Metrics(const Metrics&) = delete;
  Metrics& operator=(const Metrics&) = delete;

  struct Histogram {
    std::deque<double> observations;  // P1-1 FIX: deque for O(1) pop_front
    double sum = 0.0;
    int64_t count = 0;  // Total observations ever (including evicted)
  };

  // Helper function for histogram stats (caller must hold lock)
  HistogramStats GetHistogramStatsUnsafe(const Histogram& hist) const {
    if (hist.observations.empty()) {
      return HistogramStats{};
    }

    std::vector<double> sorted(hist.observations.begin(), hist.observations.end());
    std::sort(sorted.begin(), sorted.end());

    HistogramStats stats;
    stats.count = hist.count;
    stats.observation_count = static_cast<int64_t>(sorted.size());
    stats.min = sorted.front();
    stats.max = sorted.back();
    stats.mean = hist.sum / sorted.size();

    // P0-4 FIX: Correct percentile calculation
    size_t n = sorted.size();
    stats.p50 = sorted[std::min((n - 1) * 50 / 100, n - 1)];
    stats.p95 = sorted[std::min((n - 1) * 95 / 100, n - 1)];
    stats.p99 = sorted[std::min((n - 1) * 99 / 100, n - 1)];

    return stats;
  }

  // P0-1 FIX: Separate mutexes for each map to reduce contention
  // Note: We need mutexes because std::map itself is not thread-safe for insertions
  mutable std::mutex counter_mutex_;
  mutable std::mutex gauge_mutex_;
  mutable std::mutex histogram_mutex_;

  // P0-3 FIX: Atomics are explicitly initialized to 0 in increment operations
  std::map<std::string, std::atomic<int64_t>> counters_;
  std::map<std::string, std::atomic<int64_t>> gauges_;
  std::map<std::string, Histogram> histograms_;
};

// Convenience macros for common metrics
#define METRICS_INCREMENT_COUNTER(name, value) \
  gizmosql::Metrics::GetInstance().IncrementCounter(name, value)

#define METRICS_SET_GAUGE(name, value) \
  gizmosql::Metrics::GetInstance().SetGauge(name, value)

#define METRICS_INCREMENT_GAUGE(name) \
  gizmosql::Metrics::GetInstance().IncrementGauge(name, 1)

#define METRICS_DECREMENT_GAUGE(name) \
  gizmosql::Metrics::GetInstance().DecrementGauge(name, 1)

#define METRICS_OBSERVE_HISTOGRAM(name, value) \
  gizmosql::Metrics::GetInstance().ObserveHistogram(name, value)

// RAII helper for tracking operation duration (in milliseconds)
// NOTE: Metrics are recorded in milliseconds, not seconds
// For Prometheus, use PromQL functions to convert if needed
class ScopedTimer {
 public:
  explicit ScopedTimer(const std::string& metric_name)
      : metric_name_(metric_name), start_(std::chrono::steady_clock::now()) {}

  ~ScopedTimer() {
    auto end = std::chrono::steady_clock::now();
    auto duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start_).count();
    METRICS_OBSERVE_HISTOGRAM(metric_name_, static_cast<double>(duration_ms));
  }

 private:
  std::string metric_name_;
  std::chrono::steady_clock::time_point start_;
};

#define METRICS_SCOPED_TIMER(name) gizmosql::ScopedTimer _timer_##__LINE__(name)

}  // namespace gizmosql
