// src/common/include/session_context.h
#pragma once
#include <memory>
#include <string>
#include <chrono>
#include <duckdb.hpp>

struct ClientSession {
  std::shared_ptr<duckdb::Connection> connection;
  std::string session_id; // from session middleware
  std::string username; // from bearer auth middleware (JWT sub/email/etc.)
  std::string role; // from JWT claims (e.g. "role") or header
  std::string peer; // client ip:port (ctx.peer())
  std::optional<std::string> active_sql_handle;

  // Session lifetime tracking (for TTL)
  std::chrono::steady_clock::time_point created_at;
  std::chrono::steady_clock::time_point last_activity;

  // Configurable timeouts (defaults: 1 hour idle, 24 hours max lifetime)
  std::chrono::seconds idle_timeout = std::chrono::seconds(3600);    // 1 hour
  std::chrono::seconds max_lifetime = std::chrono::seconds(86400);   // 24 hours
};
