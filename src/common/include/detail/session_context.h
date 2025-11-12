// src/common/include/session_context.h
#pragma once
#include <memory>
#include <string>
#include <chrono>
#include <atomic>
#include <duckdb.hpp>

struct ClientSession {
  std::shared_ptr<duckdb::Connection> connection;
  std::string session_id; // from session middleware
  std::string username; // from bearer auth middleware (JWT sub/email/etc.)
  std::string role; // from JWT claims (e.g. "role") or header
  std::string peer; // client ip:port (ctx.peer())
  std::optional<std::string> active_sql_handle;

  // Session lifetime tracking (for TTL)
  // Using atomic int64_t to store nanoseconds since epoch for thread safety
  std::atomic<int64_t> created_at_ns;
  std::atomic<int64_t> last_activity_ns;

  // Helper methods for time_point access
  void set_created_at(std::chrono::steady_clock::time_point tp) {
    created_at_ns.store(tp.time_since_epoch().count(), std::memory_order_relaxed);
  }

  std::chrono::steady_clock::time_point get_created_at() const {
    return std::chrono::steady_clock::time_point(
        std::chrono::steady_clock::duration(created_at_ns.load(std::memory_order_relaxed)));
  }

  void set_last_activity(std::chrono::steady_clock::time_point tp) {
    last_activity_ns.store(tp.time_since_epoch().count(), std::memory_order_relaxed);
  }

  std::chrono::steady_clock::time_point get_last_activity() const {
    return std::chrono::steady_clock::time_point(
        std::chrono::steady_clock::duration(last_activity_ns.load(std::memory_order_relaxed)));
  }

  // Configurable timeouts (defaults: 1 hour idle, 24 hours max lifetime)
  std::chrono::seconds idle_timeout = std::chrono::seconds(3600);    // 1 hour
  std::chrono::seconds max_lifetime = std::chrono::seconds(86400);   // 24 hours
};
