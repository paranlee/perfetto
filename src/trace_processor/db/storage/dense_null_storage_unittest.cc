/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/db/storage/dense_null_storage.h"
#include <cstdint>
#include <memory>
#include <vector>

#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/db/storage/fake_storage.h"
#include "src/trace_processor/db/storage/numeric_storage.h"
#include "src/trace_processor/db/storage/types.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace storage {
namespace {

using testing::ElementsAre;
using testing::IsEmpty;
using Range = RowMap::Range;

std::vector<uint32_t> ToIndexVector(RangeOrBitVector& r_or_bv) {
  RowMap rm;
  if (r_or_bv.IsBitVector()) {
    rm = RowMap(std::move(r_or_bv).TakeIfBitVector());
  } else {
    Range range = std::move(r_or_bv).TakeIfRange();
    rm = RowMap(range.start, range.end);
  }
  return rm.GetAllIndices();
}

TEST(DenseNullStorage, NoFilteringSearch) {
  std::vector<uint32_t> data{0, 1, 0, 1, 0};
  auto numeric =
      std::make_unique<NumericStorage<uint32_t>>(&data, ColumnType::kUint32);

  BitVector bv{0, 1, 0, 1, 0};
  DenseNullStorage storage(std::move(numeric), &bv);

  auto res = storage.Search(FilterOp::kGe, SqlValue::Long(0), Range(0, 5));
  ASSERT_THAT(ToIndexVector(res), ElementsAre(1, 3));
}

TEST(DenseNullStorage, RestrictInputSearch) {
  std::vector<uint32_t> data{0, 1, 0, 1, 0};
  auto numeric =
      std::make_unique<NumericStorage<uint32_t>>(&data, ColumnType::kUint32);

  BitVector bv{0, 1, 0, 1, 0};
  DenseNullStorage storage(std::move(numeric), &bv);

  auto res = storage.Search(FilterOp::kGe, SqlValue::Long(0), Range(1, 3));
  ASSERT_THAT(ToIndexVector(res), ElementsAre(1));
}

TEST(DenseNullStorage, RangeFilterSearch) {
  auto fake = FakeStorage::SearchSubset(5, Range(1, 3));

  BitVector bv{0, 1, 0, 1, 0};
  DenseNullStorage storage(std::move(fake), &bv);

  auto res = storage.Search(FilterOp::kGe, SqlValue::Long(0), Range(0, 5));
  ASSERT_THAT(ToIndexVector(res), ElementsAre(1));
}

TEST(DenseNullStorage, BitvectorFilterSearch) {
  auto fake = FakeStorage::SearchSubset(5, BitVector({0, 1, 1, 0, 0}));

  BitVector bv{0, 1, 0, 1, 0};
  DenseNullStorage storage(std::move(fake), &bv);

  auto res = storage.Search(FilterOp::kGe, SqlValue::Long(0), Range(0, 5));
  ASSERT_THAT(ToIndexVector(res), ElementsAre(1));
}

TEST(DenseNullStorage, IsNullSearch) {
  auto fake = FakeStorage::SearchSubset(5, BitVector({1, 1, 0, 0, 1}));

  BitVector bv{1, 0, 0, 1, 1};
  DenseNullStorage storage(std::move(fake), &bv);

  auto res = storage.Search(FilterOp::kIsNull, SqlValue(), Range(0, 5));
  ASSERT_THAT(ToIndexVector(res), ElementsAre(0, 1, 2, 4));
}

TEST(DenseNullStorage, IndexSearch) {
  std::vector<uint32_t> data{1, 0, 0, 1, 1, 1};
  auto numeric =
      std::make_unique<NumericStorage<uint32_t>>(&data, ColumnType::kUint32);

  BitVector bv{1, 0, 0, 1, 1, 1};
  DenseNullStorage storage(std::move(numeric), &bv);

  std::vector<uint32_t> index({5, 2, 3, 4, 1});
  auto res = storage.IndexSearch(FilterOp::kGe, SqlValue::Long(0), index.data(),
                                 static_cast<uint32_t>(index.size()), false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(0, 2, 3));
}

TEST(DenseNullStorage, IsNullIndexSearch) {
  auto fake = FakeStorage::SearchSubset(6, BitVector({0, 0, 0, 1, 1, 1}));

  BitVector bv{0, 1, 0, 1, 1, 1};
  DenseNullStorage storage(std::move(fake), &bv);

  std::vector<uint32_t> index({5, 2, 3, 4, 1});
  auto res = storage.IndexSearch(FilterOp::kIsNull, SqlValue(), index.data(),
                                 static_cast<uint32_t>(index.size()), false);
  ASSERT_THAT(ToIndexVector(res), ElementsAre(0, 1, 2, 3));
}

}  // namespace
}  // namespace storage
}  // namespace trace_processor
}  // namespace perfetto
