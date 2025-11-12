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

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace gizmosql {

// P2-2: Lightweight metrics system for observability
// Provides thread-safe counters, gauges, and histograms
// Metrics can be exported in Prometheus text format

class Metrics {
 public:
  static Metrics& GetInstance() {
    static Metrics instance;
    return instance;
  }

  // Counter: monotonically increasing value
  void IncrementCounter(const std::string& name, int64_t value = 1) {
    counters_[name].fetch_add(value, std::memory_order_relaxed);
  }

  int64_t GetCounter(const std::string& name) const {
    auto it = counters_.find(name);
    return it != counters_.end() ? it->second.load(std::memory_order_relaxed) : 0;
  }

  // Gauge: value that can go up and down
  void SetGauge(const std::string& name, int64_t value) {
    gauges_[name].store(value, std::memory_order_relaxed);
  }

  void IncrementGauge(const std::string& name, int64_t delta = 1) {
    gauges_[name].fetch_add(delta, std::memory_order_relaxed);
  }

  void DecrementGauge(const std::string& name, int64_t delta = 1) {
    gauges_[name].fetch_sub(delta, std::memory_order_relaxed);
  }

  int64_t GetGauge(const std::string& name) const {
    auto it = gauges_.find(name);
    return it != gauges_.end() ? it->second.load(std::memory_order_relaxed) : 0;
  }

  // Histogram: track distribution of values
  void ObserveHistogram(const std::string& name, double value) {
    std::lock_guard<std::mutex> lock(histogram_mutex_);
    auto& hist = histograms_[name];
    hist.observations.push_back(value);
    hist.sum += value;
    hist.count++;
  }

  struct HistogramStats {
    double min = 0.0;
    double max = 0.0;
    double mean = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    int64_t count = 0;
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

    auto sorted = hist.observations;
    std::sort(sorted.begin(), sorted.end());

    HistogramStats stats;
    stats.count = hist.count;
    stats.min = sorted.front();
    stats.max = sorted.back();
    stats.mean = hist.sum / hist.count;
    stats.p50 = sorted[sorted.size() * 50 / 100];
    stats.p95 = sorted[sorted.size() * 95 / 100];
    stats.p99 = sorted[sorted.size() * 99 / 100];

    return stats;
  }

  // Export metrics in Prometheus text format
  std::string ExportPrometheusFormat() const {
    std::ostringstream ss;

    // Export counters
    for (const auto& [name, value] : counters_) {
      ss << "# TYPE " << name << " counter\n";
      ss << name << " " << value.load(std::memory_order_relaxed) << "\n";
    }

    // Export gauges
    for (const auto& [name, value] : gauges_) {
      ss << "# TYPE " << name << " gauge\n";
      ss << name << " " << value.load(std::memory_order_relaxed) << "\n";
    }

    // Export histograms
    std::lock_guard<std::mutex> lock(histogram_mutex_);
    for (const auto& [name, hist] : histograms_) {
      if (hist.count == 0) continue;

      auto stats = GetHistogramStatsUnsafe(hist);

      ss << "# TYPE " << name << " summary\n";
      ss << name << "_count " << stats.count << "\n";
      ss << name << "_sum " << hist.sum << "\n";
      ss << name << "{quantile=\"0.5\"} " << stats.p50 << "\n";
      ss << name << "{quantile=\"0.95\"} " << stats.p95 << "\n";
      ss << name << "{quantile=\"0.99\"} " << stats.p99 << "\n";
    }

    return ss.str();
  }

  // Export metrics as JSON
  std::string ExportJSON() const {
    std::ostringstream ss;
    ss << "{\n";

    // Counters
    ss << "  \"counters\": {\n";
    bool first = true;
    for (const auto& [name, value] : counters_) {
      if (!first) ss << ",\n";
      ss << "    \"" << name << "\": " << value.load(std::memory_order_relaxed);
      first = false;
    }
    ss << "\n  },\n";

    // Gauges
    ss << "  \"gauges\": {\n";
    first = true;
    for (const auto& [name, value] : gauges_) {
      if (!first) ss << ",\n";
      ss << "    \"" << name << "\": " << value.load(std::memory_order_relaxed);
      first = false;
    }
    ss << "\n  },\n";

    // Histograms
    ss << "  \"histograms\": {\n";
    std::lock_guard<std::mutex> lock(histogram_mutex_);
    first = true;
    for (const auto& [name, hist] : histograms_) {
      if (hist.count == 0) continue;
      if (!first) ss << ",\n";

      auto stats = GetHistogramStatsUnsafe(hist);
      ss << "    \"" << name << "\": {\n";
      ss << "      \"count\": " << stats.count << ",\n";
      ss << "      \"mean\": " << stats.mean << ",\n";
      ss << "      \"min\": " << stats.min << ",\n";
      ss << "      \"max\": " << stats.max << ",\n";
      ss << "      \"p50\": " << stats.p50 << ",\n";
      ss << "      \"p95\": " << stats.p95 << ",\n";
      ss << "      \"p99\": " << stats.p99 << "\n";
      ss << "    }";
      first = false;
    }
    ss << "\n  }\n";

    ss << "}\n";
    return ss.str();
  }

  // Reset all metrics (useful for testing)
  void Reset() {
    for (auto& [name, value] : counters_) {
      value.store(0, std::memory_order_relaxed);
    }
    for (auto& [name, value] : gauges_) {
      value.store(0, std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lock(histogram_mutex_);
    histograms_.clear();
  }

 private:
  Metrics() = default;
  ~Metrics() = default;
  Metrics(const Metrics&) = delete;
  Metrics& operator=(const Metrics&) = delete;

  struct Histogram {
    std::vector<double> observations;
    double sum = 0.0;
    int64_t count = 0;
  };

  HistogramStats GetHistogramStatsUnsafe(const Histogram& hist) const {
    if (hist.observations.empty()) {
      return HistogramStats{};
    }

    auto sorted = hist.observations;
    std::sort(sorted.begin(), sorted.end());

    HistogramStats stats;
    stats.count = hist.count;
    stats.min = sorted.front();
    stats.max = sorted.back();
    stats.mean = hist.sum / hist.count;
    stats.p50 = sorted[sorted.size() * 50 / 100];
    stats.p95 = sorted[sorted.size() * 95 / 100];
    stats.p99 = sorted[sorted.size() * 99 / 100];

    return stats;
  }

  mutable std::map<std::string, std::atomic<int64_t>> counters_;
  mutable std::map<std::string, std::atomic<int64_t>> gauges_;
  mutable std::map<std::string, Histogram> histograms_;
  mutable std::mutex histogram_mutex_;
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

// RAII helper for tracking operation duration
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
