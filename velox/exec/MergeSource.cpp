/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "velox/exec/MergeSource.h"

#include <boost/circular_buffer.hpp>
#include "velox/exec/Merge.h"

namespace facebook::velox::exec {
namespace {
class MergeSourceData {
 public:
  MergeSourceData(
      const std::shared_ptr<const RowType>& type,
      memory::MappedMemory* mappedMemory,
      const RowVectorPtr& input)
      : data_(std::make_unique<RowContainer>(type->children(), mappedMemory)) {
    if (input) {
      fetchRows(input);
    }
  }

  void clear() {
    rows_.clear();
    data_->clear();
    currentPos_ = 0;
  }

  void fetchRows(const RowVectorPtr& input) {
    SelectivityVector allRows(input->size());
    rows_.reserve(input->size());
    for (int i = 0; i < input->size(); ++i) {
      rows_.emplace_back(data_->newRow());
    }

    DecodedVector decoded;
    for (int col = 0; col < input->childrenSize(); ++col) {
      decoded.decode(*input->childAt(col), allRows);
      for (int i = 0; i < input->size(); ++i) {
        data_->store(decoded, i, rows_[i], col);
      }
    }
  }

  bool hasNext() const {
    return currentPos_ < rows_.size();
  }

  char* next() {
    VELOX_DCHECK_LE(currentPos_, rows_.size());
    return rows_[currentPos_++];
  }

 private:
  size_t currentPos_ = 0;
  std::unique_ptr<RowContainer> data_;
  std::vector<char*> rows_;
};

class LocalMergeSource : public MergeSource {
 public:
  LocalMergeSource(
      const std::shared_ptr<const RowType>& rowType,
      memory::MappedMemory* mappedMemory,
      int queueSize)
      : queue_(LocalMergeSourceQueue(rowType, mappedMemory, queueSize)) {}

  BlockingReason next(ContinueFuture* future, char** row) override {
    auto lockedQueue = queue_.wlock();
    return lockedQueue->next(future, row);
  }

  BlockingReason enqueue(RowVectorPtr input, ContinueFuture* future) override {
    auto lockedQueue = queue_.wlock();
    return lockedQueue->enqueue(input, future);
  }

 private:
  class LocalMergeSourceQueue {
   public:
    LocalMergeSourceQueue(
        const std::shared_ptr<const RowType>& rowType,
        memory::MappedMemory* mappedMemory,
        int queueSize)
        : rowType_(rowType),
          mappedMemory_(mappedMemory->sharedPtr()),
          data_(queueSize) {
      VELOX_CHECK(mappedMemory_);
    }

    BlockingReason next(ContinueFuture* future, char** row) {
      *row = nullptr;

      if (data_.empty()) {
        if (atEnd_) {
          return BlockingReason::kNotBlocked;
        }
        consumerPromises_.emplace_back("LocalMergeSourceQueue::next");
        *future = consumerPromises_.back().getSemiFuture();
        return BlockingReason::kWaitForExchange;
      }

      if (data_.front().hasNext()) {
        *row = data_.front().next();
        return BlockingReason::kNotBlocked;
      }

      // advance to next batch.
      data_.pop_front();

      // Notify any producers.
      notifyProducers();

      return next(future, row);
    }

    BlockingReason enqueue(RowVectorPtr input, ContinueFuture* future) {
      if (!input) {
        atEnd_ = true;
        notifyConsumers();
        return BlockingReason::kNotBlocked;
      }
      VELOX_CHECK(!data_.full(), "LocalMergeSourceQueue is full");
      data_.push_back(MergeSourceData(rowType_, mappedMemory_.get(), input));
      notifyConsumers();

      if (data_.full()) {
        producerPromises_.emplace_back("LocalMergeSourceQueue::enqueue");
        *future = producerPromises_.back().getSemiFuture();
        return BlockingReason::kWaitForConsumer;
      }
      return BlockingReason::kNotBlocked;
    }

   private:
    const std::shared_ptr<const RowType> rowType_;
    // Since LocalMergeSource's lifetime is same as Tasks, keep mappedMemory's
    // shared pointer to prevent prematurely when the LocalMerge operator is
    // destroyed.
    std::shared_ptr<memory::MappedMemory> mappedMemory_;

    bool atEnd_ = false;
    boost::circular_buffer<MergeSourceData> data_;
    std::vector<VeloxPromise<bool>> consumerPromises_;
    std::vector<VeloxPromise<bool>> producerPromises_;

    void notifyConsumers() {
      for (auto& promise : consumerPromises_) {
        promise.setValue(true);
      }
      consumerPromises_.clear();
    }

    void notifyProducers() {
      for (auto& promise : producerPromises_) {
        promise.setValue(true);
      }
      producerPromises_.clear();
    }
  };

  folly::Synchronized<LocalMergeSourceQueue> queue_;
};

class MergeExchangeSource : public MergeSource {
 public:
  MergeExchangeSource(MergeExchange* mergeExchange, const std::string& taskId)
      : mergeExchange_(mergeExchange),
        client_(std::make_shared<ExchangeClient>(0)),
        data_(
            mergeExchange->outputType(),
            mergeExchange->mappedMemory(),
            nullptr) {
    client_->addRemoteTaskId(taskId);
    client_->noMoreRemoteTasks();
  }

  BlockingReason next(ContinueFuture* future, char** row) override {
    *row = nullptr;
    if (atEnd_) {
      return BlockingReason::kNotBlocked;
    }
    if (data_.hasNext()) {
      *row = data_.next();
      return BlockingReason::kNotBlocked;
    }

    if (!currentPage_) {
      currentPage_ = client_->next(&atEnd_, future);
      if (atEnd_) {
        return BlockingReason::kNotBlocked;
      }

      if (!currentPage_) {
        return BlockingReason::kWaitForExchange;
      }
    }
    if (!inputStream_) {
      inputStream_ = std::make_unique<ByteStream>();
      mergeExchange_->stats().rawInputBytes += currentPage_->byteSize();
      currentPage_->prepareStreamForDeserialize(inputStream_.get());
    }

    data_.clear();

    if (!inputStream_->atEnd()) {
      RowVectorPtr result;
      VectorStreamGroup::read(
          inputStream_.get(),
          mergeExchange_->pool(),
          mergeExchange_->outputType(),
          &result);

      mergeExchange_->stats().inputPositions += result->size();
      mergeExchange_->stats().inputBytes += result->retainedSize();

      data_.fetchRows(result);
    }

    // Since VectorStreamGroup::read() may cause inputStream to be at end,
    // check again and reset currentPage_ and inputStream_ here.
    if (inputStream_->atEnd()) {
      // Reached end of the stream.
      currentPage_ = nullptr;
      inputStream_ = nullptr;
    }

    if (data_.hasNext()) {
      *row = data_.next();
    }
    return BlockingReason::kNotBlocked;
  }

 private:
  MergeExchange* mergeExchange_;
  std::shared_ptr<ExchangeClient> client_;
  std::unique_ptr<ByteStream> inputStream_;
  std::unique_ptr<SerializedPage> currentPage_;
  MergeSourceData data_;
  bool atEnd_ = false;

  BlockingReason enqueue(RowVectorPtr input, ContinueFuture* future) override {
    VELOX_FAIL();
  }
};
} // namespace

std::shared_ptr<MergeSource> MergeSource::createLocalMergeSource(
    const std::shared_ptr<const RowType>& rowType,
    memory::MappedMemory* mappedMemory) {
  // Buffer up to 2 vectors from each source before blocking to wait
  // for consumers.
  static const int kDefaultQueueSize = 2;
  return std::make_shared<LocalMergeSource>(
      rowType, mappedMemory, kDefaultQueueSize);
}

std::shared_ptr<MergeSource> MergeSource::createMergeExchangeSource(
    MergeExchange* mergeExchange,
    const std::string& taskId) {
  return std::make_shared<MergeExchangeSource>(mergeExchange, taskId);
}
} // namespace facebook::velox::exec
