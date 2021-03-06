// ----------------------------------------------------------------------
// File: StagingArea.hh
// Author: Georgios Bitzes - CERN
// ----------------------------------------------------------------------

/************************************************************************
 * quarkdb - a redis-like highly available key-value store              *
 * Copyright (C) 2016 CERN/Switzerland                                  *
 *                                                                      *
 * This program is free software: you can redistribute it and/or modify *
 * it under the terms of the GNU General Public License as published by *
 * the Free Software Foundation, either version 3 of the License, or    *
 * (at your option) any later version.                                  *
 *                                                                      *
 * This program is distributed in the hope that it will be useful,      *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

#ifndef QUARKDB_STAGING_AREA_H
#define QUARKDB_STAGING_AREA_H

#include <mutex>
#include <string_view>
#include "KeyDescriptor.hh"
#include "utils/SmartBuffer.hh"
#include "storage/VersionedHashRevisionTracker.hh"
#include "StateMachine.hh"

namespace quarkdb {

#define THROW_ON_ERROR(st) { rocksdb::Status st2 = st; if(!st2.ok()) qdb_throw(st2.ToString()); }

class StagingArea {
public:
  StagingArea(StateMachine &sm, bool onlyreads = false)
  : stateMachine(sm), bulkLoad(stateMachine.inBulkLoad()), readOnly(onlyreads),
    /* construct writeBatchWithIndex with default arguments for all, apart from
       overwrite_key, which we set to true. This allows iterating over the
       batch + the DB. */
    writeBatchWithIndex(rocksdb::BytewiseComparator(), 0, true, 0) {

    if(!bulkLoad && !readOnly) {
      stateMachine.writeMtx.lock();
    }

    if(readOnly) {
      // Acquire snapshot.
      snapshot.reset(new StateMachine::Snapshot(sm.db.get()));
    }
  }

  ~StagingArea() {
    if(!bulkLoad && !readOnly) {
      stateMachine.writeMtx.unlock();
    }
  }

  // Read the given key from the write batch, without touching the DB.
  rocksdb::Status readFromWriteBatch(std::string_view key, std::string &value) {
    if(readOnly) qdb_throw("cannot call inWriteBatch() on a readonly staging area");
    if(bulkLoad) {
      return rocksdb::Status::NotFound();
    }

    return writeBatchWithIndex.GetFromBatch(rocksdb::DBOptions(), key, &value);
  }

  rocksdb::Status getForUpdate(std::string_view slice, std::string &value) {
    if(readOnly) qdb_throw("cannot call getForUpdate() on a readonly staging area");
    if(bulkLoad) {
      return rocksdb::Status::NotFound();
    }

    return writeBatchWithIndex.GetFromBatchAndDB(stateMachine.db.get(),
      rocksdb::ReadOptions(), slice, &value);
  }

  rocksdb::Status exists(std::string_view slice) {
    if(bulkLoad) {
      // No reads during bulkload mode.
      return rocksdb::Status::NotFound();
    }

    if(readOnly) {
      std::string ignore;
      return stateMachine.db->Get(snapshot->opts(), slice, &ignore);
    }

    rocksdb::PinnableSlice ignored;
    return writeBatchWithIndex.GetFromBatchAndDB(stateMachine.db.get(), rocksdb::ReadOptions(), slice, &ignored);
  }

  rocksdb::Status get(std::string_view slice, std::string &value) {
    if(bulkLoad) {
      return rocksdb::Status::NotFound();
    }

    if(readOnly) {
      return stateMachine.db->Get(snapshot->opts(), slice, &value);
    }

    return writeBatchWithIndex.GetFromBatchAndDB(stateMachine.db.get(), rocksdb::ReadOptions(), slice, &value);
  }

  void put(std::string_view slice, std::string_view value) {
    if(readOnly) qdb_throw("cannot call put() on a readonly staging area");
    if(bulkLoad) {
      if(slice[0] == char(InternalKeyType::kDescriptor)) {
        // Ignore key descriptors, we'll build them all at the end
        return;
      }

      // rocksdb transactions have to build an internal index to implement
      // repeatable reads on the same tx. In bulkload mode we don't allow reads,
      // so let's use the much faster write batch.
      writeBatch.Put(slice, value);
      return;
    }

    THROW_ON_ERROR(writeBatchWithIndex.Put(slice, value));
  }

  void del(std::string_view slice) {
    if(readOnly) qdb_throw("cannot call del() on a readonly staging area");
    if(bulkLoad) qdb_throw("no deletions allowed during bulk load");
    THROW_ON_ERROR(writeBatchWithIndex.Delete(slice));
  }

  // SingleDelete() has a performance advantage over del(), but can be used
  // if and only if we're certain only a _SINGLE_ entry for this key exists
  // across _ALL_ rocksdb compaction layers.
  //
  // The tombstone by single delete will annihilate upon meeting its target
  // key, without polluting multiple compaction layers for a potentially very
  // long time.
  //
  // For keys that are single-deleted: Never overwrite them, and never call
  // del() on them.
  void singleDelete(std::string_view slice) {
    if(readOnly) qdb_throw("cannot call singleDelete() on a readonly staging area");
    if(bulkLoad) qdb_throw("no deletions allowed during bulk load");
    THROW_ON_ERROR(writeBatchWithIndex.SingleDelete(slice));
  }

  rocksdb::Status commit(LogIndex index) {
    if(readOnly) qdb_throw("cannot call commit() on a readonly staging area");
    if(bulkLoad) {
      qdb_assert(index == 0);
      stateMachine.commitBatch(writeBatch);
      return rocksdb::Status::OK();
    }

    stateMachine.commitTransaction(writeBatchWithIndex, index);
    return rocksdb::Status::OK();
  }

  StateMachine::IteratorPtr getIterator(bool withInternalKeys = false) {
    if(readOnly) {
      // Return an iterator that views only the current snapshot.
      rocksdb::ReadOptions opts = snapshot->opts();
      if(withInternalKeys) {
        opts.iter_start_seqnum = 1;
      }
      return StateMachine::IteratorPtr(stateMachine.db->NewIterator(opts));
    }

    if(bulkLoad) {
      // No reading
      return StateMachine::IteratorPtr(rocksdb::NewEmptyIterator());
    }

    // Return an iterator which takes into account keys both in WriteBatchWithIndex,
    // and the DB.
    rocksdb::ReadOptions opts;
    if(withInternalKeys) {
      opts.iter_start_seqnum = 1;
    }

    return StateMachine::IteratorPtr(
      writeBatchWithIndex.NewIteratorWithBase(stateMachine.db->NewIterator(opts))
    );
  }

  VersionedHashRevisionTracker& getRevisionTracker() {
    return revisionTracker;
  }

private:
  friend class StateMachine;
  StateMachine &stateMachine;
  bool bulkLoad = false;
  bool readOnly = false;

  std::unique_ptr<StateMachine::Snapshot> snapshot;
  rocksdb::WriteBatch writeBatch;
  rocksdb::WriteBatchWithIndex writeBatchWithIndex;
  VersionedHashRevisionTracker revisionTracker;
};

}

#endif
