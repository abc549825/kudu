// Copyright (c) 2014, Cloudera, inc.
#ifndef KUDU_CONSENSUS_LOG_CACHE_H
#define KUDU_CONSENSUS_LOG_CACHE_H

#include <map>
#include <string>
#include <tr1/memory>
#include <tr1/unordered_set>
#include <vector>

#include "kudu/consensus/consensus.pb.h"
#include "kudu/consensus/opid_util.h"
#include "kudu/consensus/ref_counted_replicate.h"
#include "kudu/gutil/gscoped_ptr.h"
#include "kudu/gutil/macros.h"
#include "kudu/util/async_util.h"
#include "kudu/util/locks.h"
#include "kudu/util/metrics.h"
#include "kudu/util/status.h"

namespace kudu {

class MetricEntity;
class MemTracker;

namespace log {
class Log;
class LogReader;
} // namespace log

namespace consensus {

class ReplicateMsg;

// The id for the server-wide log cache MemTracker.
extern const char kLogCacheTrackerId[];

// Write-through cache for the log.
//
// This stores a set of log messages by their index. New operations
// can be appended to the end as they are written to the log. Readers
// fetch entries that were explicitly appended, or they can fetch older
// entries which are asynchronously fetched from the disk.
class LogCache {
 public:
  LogCache(const scoped_refptr<MetricEntity>& metric_entity,
           const scoped_refptr<log::Log>& log,
           const std::string& local_uuid,
           const std::string& tablet_id,
           const std::string& parent_tracker_id = kLogCacheTrackerId);
  ~LogCache();

  // Initialize the cache.
  //
  // 'preceding_op' is the current latest op. The next AppendOperation() call
  // must follow this op.
  //
  // Requires that the cache is empty.
  void Init(const OpId& preceding_op);

  // Read operations from the log, following 'after_op_index'.
  // If such an op exists in the log, an OK result will always include at least one
  // operation.
  //
  // The result will be limited such that the total ByteSize() of the returned ops
  // is less than max_size_bytes, unless that would result in an empty result, in
  // which case exactly one op is returned.
  //
  // The OpId which precedes the returned ops is returned in *preceding_op.
  // The index of this OpId will match 'after_op_index'.
  //
  // If the ops being requested are not available in the log, this will synchronously
  // read these ops from disk. Therefore, this function may take a substantial amount
  // of time and should not be called with important locks held, etc.
  Status ReadOps(int64_t after_op_index,
                 int max_size_bytes,
                 std::vector<ReplicateRefPtr>* messages,
                 OpId* preceding_op);

  // Append the operations into the log and the cache.
  // When the messages have completed writing into the on-disk log, fires 'callback'.
  //
  // If the cache memory limit is exceeded, the entries may no longer be in the cache
  // when the callback fires.
  //
  // Returns non-OK if the Log append itself fails.
  Status AppendOperations(const std::vector<ReplicateRefPtr>& msgs,
                          const StatusCallback& callback);

  // Return true if an operation with the given index has been written through
  // the cache. The operation may not necessarily be durable yet -- it could still be
  // en route to the log.
  bool HasOpBeenWritten(int64_t log_index) const;

  // Evict any operations with op index <= 'index'.
  void EvictThroughOp(int64_t index);

  // Return the number of bytes of memory currently in use by the cache.
  int64_t BytesUsed() const;

  int64_t num_cached_ops() const {
    return metrics_.log_cache_total_num_ops->value();
  }

  // Dump the current contents of the cache to the log.
  void DumpToLog() const;

  // Dumps the contents of the cache to the provided string vector.
  void DumpToStrings(std::vector<std::string>* lines) const;

  void DumpToHtml(std::ostream& out) const;

  std::string StatsString() const;

  std::string ToString() const;

 private:
  FRIEND_TEST(LogCacheTest, TestAppendAndGetMessages);
  FRIEND_TEST(LogCacheTest, TestReplaceMessages);
  friend class LogCacheTest;

  // Look up the OpId for the given operation index.
  // If it is not in the cache, this consults the on-disk log index and thus
  // may take a non-trivial amount of time due to IO.
  //
  // Returns a bad Status if the log index fails to load (eg. due to an IO error).
  Status LookupOpId(int64_t op_index, OpId* op_id) const;

  // Try to evict the oldest operations from the queue, stopping either when
  // 'bytes_to_evict' bytes have been evicted, or the op with index
  // 'stop_after_index' has been evicted, whichever comes first.
  void EvictSomeUnlocked(int64_t stop_after_index, int64_t bytes_to_evict);

  // Update metrics and MemTracker to account for the removal of the
  // given message.
  void AccountForMessageRemovalUnlocked(const ReplicateRefPtr& msg);

  // Return a string with stats
  std::string StatsStringUnlocked() const;

  std::string ToStringUnlocked() const;

  std::string LogPrefixUnlocked() const;

  void LogCallback(int64_t last_idx_in_batch,
                   bool borrowed_memory,
                   const StatusCallback& user_callback,
                   const Status& log_status);

  scoped_refptr<log::Log> const log_;

  // The UUID of the local peer.
  const std::string local_uuid_;

  // The id of the tablet.
  const std::string tablet_id_;

  mutable simple_spinlock lock_;

  // An ordered map that serves as the buffer for the cached messages.
  // Maps from log index -> ReplicateMsg
  typedef std::map<uint64_t, ReplicateRefPtr> MessageCache;
  MessageCache cache_;

  // The next log index to append. Each append operation must either
  // start with this log index, or go backward (but never skip forward).
  int64_t next_sequential_op_index_;

  // Any operation with an index >= min_pinned_op_ may not be
  // evicted from the cache. This is used to prevent ops from being evicted
  // until they successfully have been appended to the underlying log.
  // Protected by lock_.
  int64_t min_pinned_op_index_;

  // Pointer to a parent memtracker for all log caches. This
  // exists to compute server-wide cache size and enforce a
  // server-wide memory limit.  When the first instance of a log
  // cache is created, a new entry is added to MemTracker's static
  // map; subsequent entries merely increment the refcount, so that
  // the parent tracker can be deleted if all log caches are
  // deleted (e.g., if all tablets are deleted from a server, or if
  // the server is shutdown).
  std::tr1::shared_ptr<MemTracker> parent_tracker_;

  // A MemTracker for this instance.
  std::tr1::shared_ptr<MemTracker> tracker_;

  // The log reader used to fill the cache when a caller requests older
  // entries.
  log::LogReader* log_reader_;

  struct Metrics {
    explicit Metrics(const scoped_refptr<MetricEntity>& metric_entity);

    // Keeps track of the total number of operations in the cache.
    scoped_refptr<AtomicGauge<int64_t> > log_cache_total_num_ops;

    // Keeps track of the memory consumed by the cache, in bytes.
    scoped_refptr<AtomicGauge<int64_t> > log_cache_size_bytes;
  };
  Metrics metrics_;

  DISALLOW_COPY_AND_ASSIGN(LogCache);
};

} // namespace consensus
} // namespace kudu
#endif /* KUDU_CONSENSUS_LOG_CACHE_H */
