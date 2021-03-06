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
#pragma once

#include <algorithm>
#include <optional>
#include <unordered_set>

namespace facebook::velox::test {

// Struct that caries metadata about a vector of nullable elements.
template <typename T>
class VectorMakerStats {
 public:
  void addElement(const T& val) {
    distinctSet_.insert(val);
  }

  size_t distinctCount() const {
    return distinctSet_.size();
  }

  std::optional<T> min;
  std::optional<T> max;
  size_t nullCount{0};
  bool isSorted{false};

 private:
  std::unordered_set<T> distinctSet_;
};

// Generates VectorMakerStats for a given vector of nullable elements.
template <typename T>
VectorMakerStats<T> genVectorMakerStats(
    const std::vector<std::optional<T>>& data) {
  VectorMakerStats<T> result;

  // Count distinct and null elements.
  for (const auto& val : data) {
    if (val == std::nullopt) {
      ++result.nullCount;
    } else {
      result.addElement(*val);
    }
  }

  // Sorted state.
  result.isSorted = std::is_sorted(data.begin(), data.end());

  // Calculate min and max (skip null elements).
  for (const auto& val : data) {
    if (val != std::nullopt) {
      result.min =
          (result.min == std::nullopt) ? val : std::min(*result.min, *val);
      result.max =
          (result.max == std::nullopt) ? val : std::max(*result.max, *val);
    }
  }
  return result;
}

// Generates VectorMakerStats for a given vector of non-nullable elements.
template <typename T>
VectorMakerStats<T> genVectorMakerStats(const std::vector<T>& data) {
  VectorMakerStats<T> result;
  for (const auto& val : data) {
    result.addElement(val);
  }

  result.isSorted = std::is_sorted(data.begin(), data.end());
  const auto& [min, max] = std::minmax_element(data.begin(), data.end());
  if (min != data.end()) {
    result.min = *min;
  }
  if (max != data.end()) {
    result.max = *max;
  }
  return result;
}

} // namespace facebook::velox::test
