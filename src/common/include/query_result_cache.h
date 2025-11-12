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

#include <arrow/record_batch.h>
#include <arrow/type.h>
#include <chrono>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace gizmosql {

// P2-3: Query Result Cache
// Caches query results to dramatically reduce latency for repeated queries
// Features:
// - LRU eviction policy
// - TTL-based expiration
// - Thread-safe with shared_mutex (read-heavy workload optimized)
// - Configurable size limits
// - Automatic invalidation on write operations

class QueryResultCache {
 public:
  struct CacheEntry {
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    std::shared_ptr<arrow::Schema> schema;
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point last_accessed;
    int64_t total_rows;
    int64_t access_count;
  };

  struct Config {
    bool enabled = true;
    size_t max_entries = 1000;
    std::chrono::seconds ttl = std::chrono::seconds(300);  // 5 minutes
    int64_t max_result_rows = 100000;  // Don't cache very large results
    bool cache_readonly_queries_only = true;
  };

  explicit QueryResultCache(const Config& config = Config{})
      : config_(config) {}

  // Check if a query result is cached
  // Note: Uses unique_lock for simplicity (Get operations are fast)
  bool Get(const std::string& cache_key,
           std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
           std::shared_ptr<arrow::Schema>& schema) {
    if (!config_.enabled) {
      return false;
    }

    std::unique_lock lock(mutex_);

    auto it = cache_map_.find(cache_key);
    if (it == cache_map_.end()) {
      return false;  // Cache miss
    }

    auto& entry = it->second;

    // Check TTL
    auto now = std::chrono::steady_clock::now();
    auto age = std::chrono::duration_cast<std::chrono::seconds>(now - entry.created_at);
    if (age > config_.ttl) {
      // Entry expired (will be cleaned up later)
      return false;
    }

    // Update access time and count
    entry.last_accessed = now;
    entry.access_count++;

    // Move to front of LRU list (most recently used)
    lru_list_.erase(entry.lru_iterator);
    lru_list_.push_front(cache_key);
    entry.lru_iterator = lru_list_.begin();

    // Copy results (shared_ptr copy is cheap)
    batches = entry.batches;
    schema = entry.schema;

    return true;  // Cache hit
  }

  // Add a query result to the cache
  void Put(const std::string& cache_key,
           const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
           const std::shared_ptr<arrow::Schema>& schema) {
    if (!config_.enabled) {
      return;
    }

    // Calculate total rows
    int64_t total_rows = 0;
    for (const auto& batch : batches) {
      total_rows += batch->num_rows();
    }

    // Don't cache very large results
    if (total_rows > config_.max_result_rows) {
      return;
    }

    std::unique_lock lock(mutex_);

    // If key already exists, remove it first
    auto it = cache_map_.find(cache_key);
    if (it != cache_map_.end()) {
      lru_list_.erase(it->second.lru_iterator);
      cache_map_.erase(it);
    }

    // Evict LRU entry if cache is full
    while (cache_map_.size() >= config_.max_entries) {
      if (lru_list_.empty()) {
        break;
      }
      const std::string& lru_key = lru_list_.back();
      cache_map_.erase(lru_key);
      lru_list_.pop_back();
    }

    // Add new entry
    lru_list_.push_front(cache_key);
    auto now = std::chrono::steady_clock::now();

    CacheEntry entry;
    entry.batches = batches;
    entry.schema = schema;
    entry.created_at = now;
    entry.last_accessed = now;
    entry.total_rows = total_rows;
    entry.access_count = 0;
    entry.lru_iterator = lru_list_.begin();

    cache_map_[cache_key] = std::move(entry);
  }

  // Invalidate all cached entries (call on write operations)
  void InvalidateAll() {
    std::unique_lock lock(mutex_);
    cache_map_.clear();
    lru_list_.clear();
  }

  // Invalidate entries matching a pattern (e.g., table name)
  void InvalidatePattern(const std::string& pattern) {
    std::unique_lock lock(mutex_);

    auto it = cache_map_.begin();
    while (it != cache_map_.end()) {
      if (it->first.find(pattern) != std::string::npos) {
        lru_list_.erase(it->second.lru_iterator);
        it = cache_map_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // Clean up expired entries
  void CleanupExpired() {
    std::unique_lock lock(mutex_);

    auto now = std::chrono::steady_clock::now();
    auto it = cache_map_.begin();
    while (it != cache_map_.end()) {
      auto age = std::chrono::duration_cast<std::chrono::seconds>(
          now - it->second.created_at);
      if (age > config_.ttl) {
        lru_list_.erase(it->second.lru_iterator);
        it = cache_map_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // Get cache statistics
  struct Stats {
    size_t entry_count;
    int64_t total_rows;
    int64_t total_access_count;
  };

  Stats GetStats() const {
    std::shared_lock lock(mutex_);

    Stats stats{};
    stats.entry_count = cache_map_.size();
    stats.total_rows = 0;
    stats.total_access_count = 0;

    for (const auto& [key, entry] : cache_map_) {
      stats.total_rows += entry.total_rows;
      stats.total_access_count += entry.access_count;
    }

    return stats;
  }

  // Update configuration
  void SetConfig(const Config& config) {
    std::unique_lock lock(mutex_);
    config_ = config;

    // If cache was disabled, clear it
    if (!config_.enabled) {
      cache_map_.clear();
      lru_list_.clear();
    }
  }

  Config GetConfig() const {
    std::shared_lock lock(mutex_);
    return config_;
  }

  // Clear all entries
  void Clear() {
    std::unique_lock lock(mutex_);
    cache_map_.clear();
    lru_list_.clear();
  }

 private:
  struct CacheEntryInternal {
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    std::shared_ptr<arrow::Schema> schema;
    std::chrono::steady_clock::time_point created_at;
    mutable std::chrono::steady_clock::time_point last_accessed;
    int64_t total_rows;
    mutable int64_t access_count;
    std::list<std::string>::iterator lru_iterator;
  };

  Config config_;
  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, CacheEntryInternal> cache_map_;
  std::list<std::string> lru_list_;  // Front = most recently used
};

// Generate cache key from SQL query and parameters
inline std::string GenerateCacheKey(
    const std::string& sql,
    const std::vector<std::string>& parameters = {}) {
  std::string key = sql;

  if (!parameters.empty()) {
    key += "|PARAMS:";
    for (size_t i = 0; i < parameters.size(); ++i) {
      if (i > 0) key += ",";
      key += parameters[i];
    }
  }

  return key;
}

// Check if SQL is a write operation that should invalidate cache
inline bool IsWriteOperation(const std::string& sql) {
  std::string upper_sql = sql;
  std::transform(upper_sql.begin(), upper_sql.end(), upper_sql.begin(),
                 [](unsigned char c) { return std::toupper(c); });

  // Trim leading whitespace
  size_t start = upper_sql.find_first_not_of(" \t\n\r");
  if (start != std::string::npos) {
    upper_sql = upper_sql.substr(start);
  }

  // Check for write operations (C++17 compatible)
  return (upper_sql.rfind("INSERT", 0) == 0) ||
         (upper_sql.rfind("UPDATE", 0) == 0) ||
         (upper_sql.rfind("DELETE", 0) == 0) ||
         (upper_sql.rfind("DROP", 0) == 0) ||
         (upper_sql.rfind("CREATE", 0) == 0) ||
         (upper_sql.rfind("ALTER", 0) == 0) ||
         (upper_sql.rfind("TRUNCATE", 0) == 0) ||
         (upper_sql.rfind("REPLACE", 0) == 0) ||
         (upper_sql.rfind("MERGE", 0) == 0);
}

}  // namespace gizmosql
