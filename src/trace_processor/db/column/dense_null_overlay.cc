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

#include "src/trace_processor/db/column/dense_null_overlay.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/db/column/column.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/tp_metatrace.h"

#include "protos/perfetto/trace_processor/metatrace_categories.pbzero.h"
#include "protos/perfetto/trace_processor/serialization.pbzero.h"

namespace perfetto::trace_processor::column {

DenseNullOverlay::DenseNullOverlay(std::unique_ptr<Column> inner,
                                   const BitVector* non_null)
    : inner_(std::move(inner)), non_null_(non_null) {}

SearchValidationResult DenseNullOverlay::ValidateSearchConstraints(
    SqlValue sql_val,
    FilterOp op) const {
  if (op == FilterOp::kIsNull) {
    return SearchValidationResult::kOk;
  }

  return inner_->ValidateSearchConstraints(sql_val, op);
}

RangeOrBitVector DenseNullOverlay::Search(FilterOp op,
                                          SqlValue sql_val,
                                          Range in) const {
  PERFETTO_TP_TRACE(metatrace::Category::DB, "DenseNullOverlay::Search");

  if (op == FilterOp::kIsNull) {
    switch (inner_->ValidateSearchConstraints(sql_val, op)) {
      case SearchValidationResult::kNoData: {
        // There is no need to search in underlying storage. It's enough to
        // intersect the |non_null_|.
        BitVector res = non_null_->IntersectRange(in.start, in.end);
        res.Not();
        res.Resize(in.end, false);
        return RangeOrBitVector(std::move(res));
      }
      case SearchValidationResult::kAllData:
        return RangeOrBitVector(in);
      case SearchValidationResult::kOk:
        break;
    }
  }

  RangeOrBitVector inner_res = inner_->Search(op, sql_val, in);
  BitVector res;
  if (inner_res.IsRange()) {
    // If the inner storage returns a range, mask out the appropriate values in
    // |non_null_| which matches the range. Then, resize to |in.end| as this
    // is mandated by the API contract of |Storage::Search|.
    Range inner_range = std::move(inner_res).TakeIfRange();
    PERFETTO_DCHECK(inner_range.end <= in.end);
    PERFETTO_DCHECK(inner_range.start >= in.start);
    res = non_null_->IntersectRange(inner_range.start, inner_range.end);
    res.Resize(in.end, false);
  } else {
    res = std::move(inner_res).TakeIfBitVector();
  }

  if (op == FilterOp::kIsNull) {
    // For IS NULL, we need to add any rows in |non_null_| which are zeros: we
    // do this by taking the appropriate number of rows, inverting it and then
    // bitwise or-ing the result with it.
    BitVector non_null_copy = non_null_->Copy();
    non_null_copy.Resize(in.end);
    non_null_copy.Not();
    res.Or(non_null_copy);
  } else {
    // For anything else, we just need to ensure that any rows which are null
    // are removed as they would not match.
    res.And(*non_null_);
  }

  PERFETTO_DCHECK(res.size() == in.end);
  return RangeOrBitVector(std::move(res));
}

RangeOrBitVector DenseNullOverlay::IndexSearch(FilterOp op,
                                               SqlValue sql_val,
                                               Indices indices) const {
  PERFETTO_TP_TRACE(metatrace::Category::DB, "DenseNullOverlay::IndexSearch");

  if (op == FilterOp::kIsNull) {
    switch (inner_->ValidateSearchConstraints(sql_val, op)) {
      case SearchValidationResult::kNoData: {
        BitVector::Builder null_indices(indices.size);
        for (const uint32_t* it = indices.data;
             it != indices.data + indices.size; it++) {
          null_indices.Append(!non_null_->IsSet(*it));
        }
        // There is no need to search in underlying storage. We should just
        // check if the index is set in |non_null_|.
        return RangeOrBitVector(std::move(null_indices).Build());
      }
      case SearchValidationResult::kAllData:
        return RangeOrBitVector(Range(0, indices.size));
      case SearchValidationResult::kOk:
        break;
    }
  }

  RangeOrBitVector inner_res = inner_->IndexSearch(op, sql_val, indices);
  if (inner_res.IsRange()) {
    Range inner_range = std::move(inner_res).TakeIfRange();
    BitVector::Builder builder(indices.size, inner_range.start);
    for (uint32_t i = inner_range.start; i < inner_range.end; ++i) {
      builder.Append(non_null_->IsSet(indices.data[i]));
    }
    return RangeOrBitVector(std::move(builder).Build());
  }

  BitVector::Builder builder(indices.size);
  for (uint32_t i = 0; i < indices.size; ++i) {
    builder.Append(non_null_->IsSet(indices.data[i]));
  }
  BitVector non_null = std::move(builder).Build();

  BitVector res = std::move(inner_res).TakeIfBitVector();

  if (op == FilterOp::kIsNull) {
    BitVector null = std::move(non_null);
    null.Not();
    res.Or(null);
  } else {
    res.And(non_null);
  }

  PERFETTO_DCHECK(res.size() == indices.size);
  return RangeOrBitVector(std::move(res));
}

Range DenseNullOverlay::OrderedIndexSearch(FilterOp op,
                                           SqlValue sql_val,
                                           Indices indices) const {
  // For NOT EQUAL the further analysis needs to be done by the caller.
  PERFETTO_CHECK(op != FilterOp::kNe);

  PERFETTO_TP_TRACE(metatrace::Category::DB,
                    "DenseNullOverlay::OrderedIndexSearch");

  // We assume all NULLs are ordered to be in the front. We are looking for the
  // first index that points to non NULL value.
  const uint32_t* first_non_null =
      std::partition_point(indices.data, indices.data + indices.size,
                           [this](uint32_t i) { return !non_null_->IsSet(i); });

  uint32_t non_null_offset =
      static_cast<uint32_t>(std::distance(indices.data, first_non_null));
  uint32_t non_null_size = static_cast<uint32_t>(
      std::distance(first_non_null, indices.data + indices.size));

  if (op == FilterOp::kIsNull) {
    return Range(0, non_null_offset);
  }

  if (op == FilterOp::kIsNotNull) {
    switch (inner_->ValidateSearchConstraints(sql_val, op)) {
      case SearchValidationResult::kNoData:
        return Range();
      case SearchValidationResult::kAllData:
        return Range(non_null_offset, indices.size);
      case SearchValidationResult::kOk:
        break;
    }
  }

  Range inner_range = inner_->OrderedIndexSearch(
      op, sql_val,
      Indices{first_non_null, non_null_size, Indices::State::kNonmonotonic});
  return Range(inner_range.start + non_null_offset,
               inner_range.end + non_null_offset);
}

void DenseNullOverlay::StableSort(uint32_t*, uint32_t) const {
  // TODO(b/307482437): Implement.
  PERFETTO_FATAL("Not implemented");
}

void DenseNullOverlay::Sort(uint32_t*, uint32_t) const {
  // TODO(b/307482437): Implement.
  PERFETTO_FATAL("Not implemented");
}

void DenseNullOverlay::Serialize(StorageProto* storage) const {
  auto* null_overlay = storage->set_dense_null_overlay();
  non_null_->Serialize(null_overlay->set_bit_vector());
  inner_->Serialize(null_overlay->set_storage());
}

}  // namespace perfetto::trace_processor::column
