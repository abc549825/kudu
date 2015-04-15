// Copyright (c) 2015, Cloudera, inc.
// Confidential Cloudera Information: Covered by NDA.

#include "kudu/fs/block_manager_metrics.h"

#include "kudu/util/metrics.h"

METRIC_DEFINE_gauge_uint64(block_manager_blocks_open_reading,
                           kudu::MetricUnit::kBlocks,
                           "Number of data blocks currently open for reading");

METRIC_DEFINE_gauge_uint64(block_manager_blocks_open_writing,
                           kudu::MetricUnit::kBlocks,
                           "Number of data blocks currently open for writing");

METRIC_DEFINE_counter(block_manager_total_writable_blocks,
                      kudu::MetricUnit::kBlocks,
                      "Number of data blocks opened for writing since service start");

METRIC_DEFINE_counter(block_manager_total_readable_blocks,
                      kudu::MetricUnit::kBlocks,
                      "Number of data blocks opened for reading since service start");

METRIC_DEFINE_counter(block_manager_total_bytes_written,
                      kudu::MetricUnit::kBytes,
                      "Number of bytes of block data written since service start");

METRIC_DEFINE_counter(block_manager_total_bytes_read,
                      kudu::MetricUnit::kBytes,
                      "Number of bytes of block data read since service start");

namespace kudu {
namespace fs {
namespace internal {

#define MINIT(x) x(METRIC_block_manager_##x.Instantiate(entity))
#define GINIT(x) x(METRIC_block_manager_##x.Instantiate(entity, 0))
BlockManagerMetrics::BlockManagerMetrics(const scoped_refptr<MetricEntity>& entity)
  : GINIT(blocks_open_reading),
    GINIT(blocks_open_writing),
    MINIT(total_readable_blocks),
    MINIT(total_writable_blocks),
    MINIT(total_bytes_read),
    MINIT(total_bytes_written) {
}
#undef GINIT
#undef MINIT

} // namespace internal
} // namespace fs
} // namespace kudu
