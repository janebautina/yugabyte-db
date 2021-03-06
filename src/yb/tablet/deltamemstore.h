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
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
#ifndef YB_TABLET_DELTAMEMSTORE_H
#define YB_TABLET_DELTAMEMSTORE_H

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest_prod.h>

#include "yb/common/columnblock.h"
#include "yb/common/rowblock.h"
#include "yb/common/schema.h"
#include "yb/consensus/log_anchor_registry.h"
#include "yb/gutil/atomicops.h"
#include "yb/gutil/gscoped_ptr.h"
#include "yb/gutil/macros.h"
#include "yb/tablet/concurrent_btree.h"
#include "yb/tablet/delta_key.h"
#include "yb/tablet/delta_tracker.h"
#include "yb/tablet/delta_stats.h"
#include "yb/tablet/mvcc.h"
#include "yb/util/atomic.h"
#include "yb/util/memory/arena.h"

namespace yb {

class MemTracker;
class RowChangeList;

namespace tablet {

class DeltaFileWriter;
class DeltaStats;
class DMSIterator;
class Mutation;

struct DMSTreeTraits : public btree::BTreeTraits {
  typedef ThreadSafeArena ArenaType;
};

// In-memory storage for data which has been recently updated.
// This essentially tracks a 'diff' per row, which contains the
// modified columns.

class DeltaMemStore : public DeltaStore,
                      public std::enable_shared_from_this<DeltaMemStore> {
 public:
  DeltaMemStore(int64_t id, int64_t rs_id,
                log::LogAnchorRegistry* log_anchor_registry,
                const std::shared_ptr<MemTracker>& parent_tracker = std::shared_ptr<MemTracker>());

  virtual CHECKED_STATUS Init() override;

  virtual bool Initted() override {
    return true;
  }

  // Update the given row in the database.
  // Copies the data, as well as any referenced values into this DMS's local
  // arena.
  CHECKED_STATUS Update(HybridTime hybrid_time, rowid_t row_idx,
                const RowChangeList &update,
                const consensus::OpId& op_id);

  size_t Count() const {
    return tree_->count();
  }

  bool Empty() const {
    return tree_->empty();
  }

  // Dump a debug version of the tree to the logs. This is not thread-safe, so
  // is only really useful in unit tests.
  void DebugPrint() const;

  // Flush the DMS to the given file writer.
  // Returns statistics in *stats.
  CHECKED_STATUS FlushToFile(DeltaFileWriter *dfw,
                     gscoped_ptr<DeltaStats>* stats);

  // Create an iterator for applying deltas from this DMS.
  //
  // The projection passed here must be the same as the schema of any
  // RowBlocks which are passed in, or else bad things will happen.
  //
  // 'snapshot' is the MVCC state which determines which transactions
  // should be considered committed (and thus applied by the iterator).
  //
  // Returns Status::OK and sets 'iterator' to the new DeltaIterator, or
  // returns Status::NotFound if the mutations within this delta store
  // cannot include 'snap'.
  virtual CHECKED_STATUS NewDeltaIterator(const Schema *projection,
                                  const MvccSnapshot &snap,
                                  DeltaIterator** iterator) const override;

  virtual CHECKED_STATUS CheckRowDeleted(rowid_t row_idx, bool *deleted) const override;

  virtual uint64_t EstimateSize() const override {
    return memory_footprint();
  }

  const int64_t id() const { return id_; }

  typedef btree::CBTree<DMSTreeTraits> DMSTree;
  typedef btree::CBTreeIterator<DMSTreeTraits> DMSTreeIter;

  size_t memory_footprint() const {
    return arena_->memory_footprint();
  }

  virtual std::string ToString() const override {
    return "DMS";
  }

  // Get the minimum log index for this DMS, -1 if it wasn't set.
  int64_t MinLogIndex() const {
    return anchorer_.minimum_log_index();
  }

  // The returned stats will always be empty, and the number of columns unset.
  virtual const DeltaStats& delta_stats() const override {
    return delta_stats_;
  }

 private:
  friend class DMSIterator;

  const DMSTree& tree() const {
    return *tree_;
  }

  const int64_t id_;    // DeltaMemStore ID.
  const int64_t rs_id_; // Rowset ID.

  std::shared_ptr<MemTracker> mem_tracker_;
  std::shared_ptr<MemoryTrackingBufferAllocator> allocator_;

  std::shared_ptr<ThreadSafeArena> arena_;

  // Concurrent B-Tree storing <key index> -> RowChangeList
  gscoped_ptr<DMSTree> tree_;

  log::MinLogIndexAnchorer anchorer_;

  const DeltaStats delta_stats_;

  // It's possible for multiple mutations to apply to the same row
  // in the same hybrid_time (e.g. if a batch contains multiple updates for that
  // row). In that case, we need to append a sequence number to the delta key
  // in the underlying tree, so that the later operations will sort after
  // the earlier ones. This atomic integer serves to provide such a sequence
  // number, and is only used in the case that such a collision occurs.
  AtomicInt<Atomic32> disambiguator_sequence_number_;

  DISALLOW_COPY_AND_ASSIGN(DeltaMemStore);
};

// Iterator over the deltas currently in the delta memstore.
// This iterator is a wrapper around the underlying tree iterator
// which snapshots sets of deltas on a per-block basis, and allows
// the caller to then apply the deltas column-by-column. This supports
// column-by-column predicate evaluation, and lazily loading columns
// only after predicates have passed.
//
// See DeltaStore for more details on usage and the implemented
// functions.
class DMSIterator : public DeltaIterator {
 public:
  CHECKED_STATUS Init(ScanSpec *spec) override;

  CHECKED_STATUS SeekToOrdinal(rowid_t row_idx) override;

  CHECKED_STATUS PrepareBatch(size_t nrows, PrepareFlag flag) override;

  CHECKED_STATUS ApplyUpdates(size_t col_to_apply, ColumnBlock *dst) override;

  CHECKED_STATUS ApplyDeletes(SelectionVector *sel_vec) override;

  CHECKED_STATUS CollectMutations(vector<Mutation *> *dst, Arena *arena) override;

  CHECKED_STATUS FilterColumnIdsAndCollectDeltas(const vector<ColumnId>& col_ids,
                                         vector<DeltaKeyAndUpdate>* out,
                                         Arena* arena) override;

  string ToString() const override;

  virtual bool HasNext() override;

 private:
  FRIEND_TEST(TestDeltaMemStore, TestIteratorDoesUpdates);
  FRIEND_TEST(TestDeltaMemStore, TestCollectMutations);
  friend class DeltaMemStore;

  // Initialize the iterator.
  // The projection passed here must be the same as the schema of any
  // RowBlocks which are passed in, or else bad things will happen.
  // The pointer must also remain valid for the lifetime of the iterator.
  DMSIterator(const std::shared_ptr<const DeltaMemStore> &dms,
              const Schema *projection, MvccSnapshot snapshot);

  const std::shared_ptr<const DeltaMemStore> dms_;

  // MVCC state which allows us to ignore uncommitted transactions.
  const MvccSnapshot mvcc_snapshot_;

  gscoped_ptr<DeltaMemStore::DMSTreeIter> iter_;

  bool initted_;

  // The index at which the last PrepareBatch() call was made
  rowid_t prepared_idx_;

  // The number of rows for which the last PrepareBatch() call was made
  uint32_t prepared_count_;

  // Whether there are prepared blocks built through PrepareBatch().
  enum PreparedFor {
    NOT_PREPARED,
    PREPARED_FOR_APPLY,
    PREPARED_FOR_COLLECT
  };
  PreparedFor prepared_for_;

  // True if SeekToOrdinal() been called at least once.
  bool seeked_;

  // The schema of the row blocks that will be passed to PrepareBatch(), etc.
  const Schema* projection_;

  // State when prepared_for_ == PREPARED_FOR_APPLY
  // ------------------------------------------------------------
  struct ColumnUpdate {
    rowid_t row_id;
    void* new_val_ptr;
    uint8_t new_val_buf[16];
  };
  typedef std::deque<ColumnUpdate> UpdatesForColumn;
  std::vector<UpdatesForColumn> updates_by_col_;
  struct DeleteOrReinsert {
    rowid_t row_id;
    bool exists;
  };
  std::deque<DeleteOrReinsert> deletes_and_reinserts_;

  // State when prepared_for_ == PREPARED_FOR_COLLECT
  // ------------------------------------------------------------
  struct PreparedDelta {
    DeltaKey key;
    Slice val;
  };
  std::deque<PreparedDelta> prepared_deltas_;

  // Temporary buffer used for RowChangeList projection.
  faststring delta_buf_;

  DISALLOW_COPY_AND_ASSIGN(DMSIterator);
};

} // namespace tablet
} // namespace yb

#endif // YB_TABLET_DELTAMEMSTORE_H
