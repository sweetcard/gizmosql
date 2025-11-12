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
#include <memory>
#include <vector>

#include "query_result_cache.h"

namespace gizmosql::ddb {

// P2-3: RecordBatchReader that caches results as they are read
// Uses shared_ptr to cache for safe lifetime management
class CachingRecordBatchReader : public arrow::RecordBatchReader {
 public:
  CachingRecordBatchReader(
      std::shared_ptr<arrow::RecordBatchReader> underlying_reader,
      std::shared_ptr<QueryResultCache> cache,
      const std::string& cache_key)
      : underlying_reader_(std::move(underlying_reader)),
        cache_(std::move(cache)),
        cache_key_(cache_key) {}

  std::shared_ptr<arrow::Schema> schema() const override {
    return underlying_reader_->schema();
  }

  arrow::Status ReadNext(std::shared_ptr<arrow::RecordBatch>* batch) override {
    ARROW_RETURN_NOT_OK(underlying_reader_->ReadNext(batch));

    if (*batch) {
      // Cache this batch
      cached_batches_.push_back(*batch);
    } else {
      // End of stream - add all batches to cache
      if (!cached_batches_.empty()) {
        cache_->Put(cache_key_, cached_batches_, schema());
      }
    }

    return arrow::Status::OK();
  }

 private:
  std::shared_ptr<arrow::RecordBatchReader> underlying_reader_;
  std::shared_ptr<QueryResultCache> cache_;  // shared_ptr for safe lifetime
  std::string cache_key_;
  std::vector<std::shared_ptr<arrow::RecordBatch>> cached_batches_;
};

// P2-3: RecordBatchReader that reads from cached results
class CachedResultReader : public arrow::RecordBatchReader {
 public:
  CachedResultReader(
      std::vector<std::shared_ptr<arrow::RecordBatch>> batches,
      std::shared_ptr<arrow::Schema> schema)
      : batches_(std::move(batches)),
        schema_(std::move(schema)),
        current_index_(0) {}

  std::shared_ptr<arrow::Schema> schema() const override {
    return schema_;
  }

  arrow::Status ReadNext(std::shared_ptr<arrow::RecordBatch>* batch) override {
    if (current_index_ < batches_.size()) {
      *batch = batches_[current_index_++];
    } else {
      *batch = nullptr;  // End of stream
    }
    return arrow::Status::OK();
  }

 private:
  std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
  std::shared_ptr<arrow::Schema> schema_;
  size_t current_index_;
};

}  // namespace gizmosql::ddb
