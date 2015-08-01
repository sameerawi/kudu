// Copyright (c) 2013, Cloudera, inc.
// Confidential Cloudera Information: Covered by NDA.

#include "kudu/tablet/tablet_peer.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <gflags/gflags.h>

#include "kudu/consensus/consensus.h"
#include "kudu/consensus/consensus_meta.h"
#include "kudu/consensus/local_consensus.h"
#include "kudu/consensus/log.h"
#include "kudu/consensus/log_util.h"
#include "kudu/consensus/opid_util.h"
#include "kudu/consensus/log_anchor_registry.h"
#include "kudu/consensus/quorum_util.h"
#include "kudu/consensus/raft_consensus.h"
#include "kudu/gutil/mathlimits.h"
#include "kudu/gutil/stl_util.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/gutil/sysinfo.h"
#include "kudu/tablet/transactions/transaction_driver.h"
#include "kudu/tablet/transactions/alter_schema_transaction.h"
#include "kudu/tablet/transactions/write_transaction.h"
#include "kudu/tablet/tablet_bootstrap.h"
#include "kudu/tablet/tablet_metrics.h"
#include "kudu/tablet/tablet_peer_mm_ops.h"
#include "kudu/tablet/tablet.pb.h"
#include "kudu/util/logging.h"
#include "kudu/util/metrics.h"
#include "kudu/util/stopwatch.h"
#include "kudu/util/threadpool.h"
#include "kudu/util/trace.h"

namespace kudu {
namespace tablet {

METRIC_DEFINE_histogram(tablet, op_prepare_queue_length, "Operation Prepare Queue Length",
                        MetricUnit::kTasks,
                        "Number of operations waiting to be prepared within this tablet. "
                        "High queue lengths indicate that the server is unable to process "
                        "operations as fast as they are being written to the WAL.",
                        10000, 2);

METRIC_DEFINE_histogram(tablet, op_prepare_queue_time, "Operation Prepare Queue Time",
                        MetricUnit::kMicroseconds,
                        "Time that operations spent waiting in the prepare queue before being "
                        "processed. High queue times indicate that the server is unable to "
                        "process operations as fast as they are being written to the WAL.",
                        10000000, 2);

METRIC_DEFINE_histogram(tablet, op_prepare_run_time, "Operation Prepare Run Time",
                        MetricUnit::kMicroseconds,
                        "Time that operations spent being prepared in the tablet. "
                        "High values may indicate that the server is under-provisioned or "
                        "that operations are experiencing high contention with one another for "
                        "locks.",
                        10000000, 2);

using consensus::Consensus;
using consensus::ConsensusBootstrapInfo;
using consensus::ConsensusMetadata;
using consensus::ConsensusOptions;
using consensus::ConsensusRound;
using consensus::LocalConsensus;
using consensus::OpId;
using consensus::RaftConfigPB;
using consensus::RaftPeerPB;
using consensus::RaftConsensus;
using consensus::ALTER_SCHEMA_OP;
using consensus::WRITE_OP;
using log::Log;
using log::LogAnchorRegistry;
using rpc::Messenger;
using strings::Substitute;
using tserver::TabletServerErrorPB;

// ============================================================================
//  Tablet Peer
// ============================================================================
TabletPeer::TabletPeer(const scoped_refptr<TabletMetadata>& meta,
                       const consensus::RaftPeerPB& local_peer_pb,
                       ThreadPool* apply_pool,
                       const Closure& mark_dirty_clbk)
  : meta_(meta),
    tablet_id_(meta->tablet_id()),
    local_peer_pb_(local_peer_pb),
    state_(BOOTSTRAPPING),
    status_listener_(new TabletStatusListener(meta)),
    apply_pool_(apply_pool),
    log_anchor_registry_(new LogAnchorRegistry()),
    mark_dirty_clbk_(mark_dirty_clbk) {
}

TabletPeer::~TabletPeer() {
  UnregisterMaintenanceOps();
  boost::lock_guard<simple_spinlock> lock(lock_);
  CHECK_EQ(state_, SHUTDOWN);
}

Status TabletPeer::Init(const shared_ptr<Tablet>& tablet,
                        const scoped_refptr<server::Clock>& clock,
                        const shared_ptr<Messenger>& messenger,
                        const scoped_refptr<Log>& log,
                        const scoped_refptr<MetricEntity>& metric_entity,
                        const shared_ptr<MemTracker>& parent_mem_tracker) {

  DCHECK(tablet) << "A TabletPeer must be provided with a Tablet";
  DCHECK(log) << "A TabletPeer must be provided with a Log";

  RETURN_NOT_OK(ThreadPoolBuilder("prepare").set_max_threads(1).Build(&prepare_pool_));
  prepare_pool_->SetQueueLengthHistogram(
      METRIC_op_prepare_queue_length.Instantiate(metric_entity));
  prepare_pool_->SetQueueTimeMicrosHistogram(
      METRIC_op_prepare_queue_time.Instantiate(metric_entity));
  prepare_pool_->SetRunTimeMicrosHistogram(
      METRIC_op_prepare_run_time.Instantiate(metric_entity));

  {
    boost::lock_guard<simple_spinlock> lock(lock_);
    CHECK_EQ(state_, BOOTSTRAPPING);
    tablet_ = tablet;
    clock_ = clock;
    messenger_ = messenger;
    log_ = log;

    ConsensusOptions options;
    options.tablet_id = meta_->tablet_id();

    TRACE("Creating consensus instance");

    gscoped_ptr<ConsensusMetadata> cmeta;
    RETURN_NOT_OK(ConsensusMetadata::Load(meta_->fs_manager(), tablet_id_,
                                          meta_->fs_manager()->uuid(), &cmeta));

    if (cmeta->committed_config().local()) {
      consensus_.reset(new LocalConsensus(options,
                                          cmeta.Pass(),
                                          meta_->fs_manager()->uuid(),
                                          clock_,
                                          this,
                                          log_.get()));
    } else {
      consensus_ = RaftConsensus::Create(options,
                                         cmeta.Pass(),
                                         local_peer_pb_,
                                         metric_entity,
                                         clock_,
                                         this,
                                         messenger_,
                                         log_.get(),
                                         parent_mem_tracker,
                                         mark_dirty_clbk_);
    }
  }

  if (tablet_->metrics() != NULL) {
    TRACE("Starting instrumentation");
    txn_tracker_.StartInstrumentation(tablet_->GetMetricEntity());
  }

  TRACE("TabletPeer::Init() finished");
  VLOG(2) << "T " << tablet_id() << " P " << consensus_->peer_uuid() << ": Peer Initted";
  return Status::OK();
}

Status TabletPeer::Start(const ConsensusBootstrapInfo& bootstrap_info) {

  TRACE("Starting consensus");

  VLOG(2) << "T " << tablet_id() << " P " << consensus_->peer_uuid() << ": Peer starting";

  VLOG(2) << "RaftConfig before starting: " << consensus_->CommittedConfig().DebugString();

  RETURN_NOT_OK(consensus_->Start(bootstrap_info));
  {
    boost::lock_guard<simple_spinlock> lock(lock_);
    CHECK_EQ(state_, BOOTSTRAPPING);
    state_ = RUNNING;
  }

  // Because we changed the tablet state, we need to re-report the tablet to the master.
  mark_dirty_clbk_.Run();

  return Status::OK();
}

const consensus::RaftConfigPB TabletPeer::RaftConfig() const {
  CHECK(consensus_) << "consensus is null";
  return consensus_->CommittedConfig();
}

TabletStatePB TabletPeer::Shutdown() {

  LOG(INFO) << "Initiating TabletPeer shutdown for tablet: " << tablet_id_;

  TabletStatePB prev_state;
  {
    boost::lock_guard<simple_spinlock> lock(lock_);
    if (state_ == QUIESCING || state_ == SHUTDOWN) {
      return state_;
    }
    prev_state = state_;
    state_ = QUIESCING;
  }

  if (tablet_) tablet_->UnregisterMaintenanceOps();

  if (consensus_) consensus_->Shutdown();

  // TODO: KUDU-183: Keep track of the pending tasks and send an "abort" message.
  LOG_SLOW_EXECUTION(WARNING, 1000,
      Substitute("TabletPeer: tablet $0: Waiting for Transactions to complete", tablet_id())) {
    txn_tracker_.WaitForAllToFinish();
  }

  if (prepare_pool_) {
    prepare_pool_->Shutdown();
  }

  if (log_) {
    WARN_NOT_OK(log_->Close(), "Error closing the Log.");
  }

  if (VLOG_IS_ON(1)) {
    VLOG(1) << "TabletPeer: tablet " << tablet_id() << " shut down!";
  }

  {
    boost::lock_guard<simple_spinlock> lock(lock_);
    state_ = SHUTDOWN;
  }
  return prev_state;
}

Status TabletPeer::CheckRunning() const {
  {
    boost::lock_guard<simple_spinlock> lock(lock_);
    if (state_ != RUNNING) {
      return Status::IllegalState(Substitute("The tablet is not in a running state: $0",
                                             TabletStatePB_Name(state_)));
    }
  }
  return Status::OK();
}

Status TabletPeer::WaitUntilConsensusRunning(const MonoDelta& timeout) {
  MonoTime start(MonoTime::Now(MonoTime::FINE));

  int backoff_exp = 0;
  const int kMaxBackoffExp = 8;
  while (true) {
    bool has_consensus = false;
    TabletStatePB cached_state;
    {
      boost::lock_guard<simple_spinlock> lock(lock_);
      cached_state = state_;
      if (consensus_) {
        has_consensus = true; // consensus_ is a set-once object.
      }
    }
    if (cached_state == QUIESCING || cached_state == SHUTDOWN) {
      return Status::IllegalState(
          Substitute("The tablet is already shutting down or shutdown. State: $0",
                     TabletStatePB_Name(cached_state)));
    }
    if (cached_state == RUNNING && has_consensus && consensus_->IsRunning()) {
      break;
    }
    MonoTime now(MonoTime::Now(MonoTime::FINE));
    MonoDelta elapsed(now.GetDeltaSince(start));
    if (elapsed.MoreThan(timeout)) {
      return Status::TimedOut(Substitute("Consensus is not running after waiting for $0. State; $1",
                                         elapsed.ToString(), TabletStatePB_Name(cached_state)));
    }
    SleepFor(MonoDelta::FromMilliseconds(1 << backoff_exp));
    backoff_exp = std::min(backoff_exp + 1, kMaxBackoffExp);
  }
  return Status::OK();
}

Status TabletPeer::SubmitWrite(WriteTransactionState *state) {
  RETURN_NOT_OK(CheckRunning());

  gscoped_ptr<WriteTransaction> transaction(new WriteTransaction(state, consensus::LEADER));
  scoped_refptr<TransactionDriver> driver;
  NewLeaderTransactionDriver(transaction.PassAs<Transaction>(), &driver);
  return driver->ExecuteAsync();
}

Status TabletPeer::SubmitAlterSchema(gscoped_ptr<AlterSchemaTransactionState> state) {
  RETURN_NOT_OK(CheckRunning());

  gscoped_ptr<AlterSchemaTransaction> transaction(
      new AlterSchemaTransaction(state.release(), consensus::LEADER));
  scoped_refptr<TransactionDriver> driver;
  NewLeaderTransactionDriver(transaction.PassAs<Transaction>(), &driver);
  return driver->ExecuteAsync();
}

void TabletPeer::GetTabletStatusPB(TabletStatusPB* status_pb_out) const {
  boost::lock_guard<simple_spinlock> lock(lock_);
  DCHECK(status_pb_out != NULL);
  DCHECK(status_listener_.get() != NULL);
  status_pb_out->set_tablet_id(status_listener_->tablet_id());
  status_pb_out->set_table_name(status_listener_->table_name());
  status_pb_out->set_last_status(status_listener_->last_status());
  status_pb_out->set_start_key(status_listener_->start_key());
  status_pb_out->set_end_key(status_listener_->end_key());
  status_pb_out->set_state(state_);
  if (tablet_) {
    status_pb_out->set_estimated_on_disk_size(tablet_->EstimateOnDiskSize());
  }
}

Status TabletPeer::RunLogGC() {
  if (!CheckRunning().ok()) {
    return Status::OK();
  }
  int64_t min_log_index;
  int32_t num_gced;
  GetEarliestNeededLogIndex(&min_log_index);
  Status s = log_->GC(min_log_index, &num_gced);
  if (!s.ok()) {
    s = s.CloneAndPrepend("Unexpected error while running Log GC from TabletPeer");
    LOG(ERROR) << s.ToString();
  }
  return Status::OK();
}

void TabletPeer::GetInFlightTransactions(Transaction::TraceType trace_type,
                                         vector<consensus::TransactionStatusPB>* out) const {
  vector<scoped_refptr<TransactionDriver> > pending_transactions;
  txn_tracker_.GetPendingTransactions(&pending_transactions);
  BOOST_FOREACH(const scoped_refptr<TransactionDriver>& driver, pending_transactions) {
    if (driver->state() != NULL) {
      consensus::TransactionStatusPB status_pb;
      status_pb.mutable_op_id()->CopyFrom(driver->GetOpId());
      switch (driver->tx_type()) {
        case Transaction::WRITE_TXN:
          status_pb.set_tx_type(consensus::WRITE_OP);
          break;
        case Transaction::ALTER_SCHEMA_TXN:
          status_pb.set_tx_type(consensus::ALTER_SCHEMA_OP);
          break;
      }
      status_pb.set_description(driver->ToString());
      int64_t running_for_micros =
          MonoTime::Now(MonoTime::FINE).GetDeltaSince(driver->start_time()).ToMicroseconds();
      status_pb.set_running_for_micros(running_for_micros);
      if (trace_type == Transaction::TRACE_TXNS) {
        status_pb.set_trace_buffer(driver->trace()->DumpToString(true));
      }
      out->push_back(status_pb);
    }
  }
}

void TabletPeer::GetEarliestNeededLogIndex(int64_t* min_index) const {
  // First, we anchor on the last OpId in the Log to establish a lower bound
  // and avoid racing with the other checks. This limits the Log GC candidate
  // segments before we check the anchors.
  {
    OpId last_log_op;
    log_->GetLatestEntryOpId(&last_log_op);
    *min_index = last_log_op.index();
  }

  // If we never have written to the log, no need to proceed.
  if (*min_index == 0) return;

  // Next, we interrogate the anchor registry.
  // Returns OK if minimum known, NotFound if no anchors are registered.
  {
    int64_t min_anchor_index;
    Status s = log_anchor_registry_->GetEarliestRegisteredLogIndex(&min_anchor_index);
    if (PREDICT_FALSE(!s.ok())) {
      DCHECK(s.IsNotFound()) << "Unexpected error calling LogAnchorRegistry: " << s.ToString();
    } else {
      *min_index = std::min(*min_index, min_anchor_index);
    }
  }

  // Next, interrogate the TransactionTracker.
  vector<scoped_refptr<TransactionDriver> > pending_transactions;
  txn_tracker_.GetPendingTransactions(&pending_transactions);
  BOOST_FOREACH(const scoped_refptr<TransactionDriver>& driver, pending_transactions) {
    OpId tx_op_id = driver->GetOpId();
    // A transaction which doesn't have an opid hasn't been submitted for replication yet and
    // thus has no need to anchor the log.
    if (tx_op_id.IsInitialized()) {
      *min_index = std::min(*min_index, tx_op_id.index());
    }
  }
}

Status TabletPeer::GetMaxIndexesToSegmentSizeMap(MaxIdxToSegmentSizeMap* idx_size_map) const {
  RETURN_NOT_OK(CheckRunning());
  int64_t min_op_idx;
  GetEarliestNeededLogIndex(&min_op_idx);
  log_->GetMaxIndexesToSegmentSizeMap(min_op_idx, idx_size_map);
  return Status::OK();
}

Status TabletPeer::GetGCableDataSize(int64_t* retention_size) const {
  RETURN_NOT_OK(CheckRunning());
  int64_t min_op_idx;
  GetEarliestNeededLogIndex(&min_op_idx);
  log_->GetGCableDataSize(min_op_idx, retention_size);
  return Status::OK();
}

Status TabletPeer::StartReplicaTransaction(const scoped_refptr<ConsensusRound>& round) {
  {
    boost::lock_guard<simple_spinlock> lock(lock_);
    CHECK(state_ == RUNNING || state_ == BOOTSTRAPPING)
      << "Bad state: " << TabletStatePB_Name(state_);
  }

  consensus::ReplicateMsg* replicate_msg = round->replicate_msg();
  DCHECK(replicate_msg->has_timestamp());
  gscoped_ptr<Transaction> transaction;
  switch (replicate_msg->op_type()) {
    case WRITE_OP:
    {
      DCHECK(replicate_msg->has_write_request()) << "WRITE_OP replica"
          " transaction must receive a WriteRequestPB";
      transaction.reset(new WriteTransaction(
          new WriteTransactionState(this, &replicate_msg->write_request()),
          consensus::REPLICA));
      break;
    }
    case ALTER_SCHEMA_OP:
    {
      DCHECK(replicate_msg->has_alter_schema_request()) << "ALTER_SCHEMA_OP replica"
          " transaction must receive an AlterSchemaRequestPB";
      transaction.reset(
          new AlterSchemaTransaction(
              new AlterSchemaTransactionState(this, &replicate_msg->alter_schema_request(), NULL),
              consensus::REPLICA));
      break;
    }
    default:
      LOG(FATAL) << "Unsupported Operation Type";
  }

  // TODO(todd) Look at wiring the stuff below on the driver
  TransactionState* state = transaction->state();
  state->set_consensus_round(round);
  Timestamp ts(replicate_msg->timestamp());
  state->set_timestamp(ts);
  clock_->Update(ts);

  scoped_refptr<TransactionDriver> driver;
  NewReplicaTransactionDriver(transaction.Pass(), &driver);

  // Unretained is required to avoid a refcount cycle.
  state->consensus_round()->SetConsensusReplicatedCallback(
      Bind(&TransactionDriver::ReplicationFinished, Unretained(driver.get())));

  RETURN_NOT_OK(driver->ExecuteAsync());
  return Status::OK();
}

void TabletPeer::NewLeaderTransactionDriver(gscoped_ptr<Transaction> transaction,
                                            scoped_refptr<TransactionDriver>* driver) {
  scoped_refptr<TransactionDriver> tx_driver = new TransactionDriver(
    &txn_tracker_,
    consensus_.get(),
    log_.get(),
    prepare_pool_.get(),
    apply_pool_,
    &txn_order_verifier_);
  tx_driver->Init(transaction.Pass(), consensus::LEADER);
  driver->swap(tx_driver);
}

void TabletPeer::NewReplicaTransactionDriver(gscoped_ptr<Transaction> transaction,
                                             scoped_refptr<TransactionDriver>* driver) {
  scoped_refptr<TransactionDriver> tx_driver = new TransactionDriver(
    &txn_tracker_,
    consensus_.get(),
    log_.get(),
    prepare_pool_.get(),
    apply_pool_,
    &txn_order_verifier_);
  tx_driver->Init(transaction.Pass(), consensus::REPLICA);
  driver->swap(tx_driver);
}

void TabletPeer::RegisterMaintenanceOps(MaintenanceManager* maint_mgr) {
  DCHECK(maintenance_ops_.empty());

  gscoped_ptr<MaintenanceOp> mrs_flush_op(new FlushMRSOp(this));
  maint_mgr->RegisterOp(mrs_flush_op.get());
  maintenance_ops_.push_back(mrs_flush_op.release());

  gscoped_ptr<MaintenanceOp> dms_flush_op(new FlushDeltaMemStoresOp(this));
  maint_mgr->RegisterOp(dms_flush_op.get());
  maintenance_ops_.push_back(dms_flush_op.release());

  gscoped_ptr<MaintenanceOp> log_gc(new LogGCOp(this));
  maint_mgr->RegisterOp(log_gc.get());
  maintenance_ops_.push_back(log_gc.release());

  tablet_->RegisterMaintenanceOps(maint_mgr);
}

void TabletPeer::UnregisterMaintenanceOps() {
  BOOST_FOREACH(MaintenanceOp* op, maintenance_ops_) {
    op->Unregister();
  }
  STLDeleteElements(&maintenance_ops_);
}

Status FlushInflightsToLogCallback::WaitForInflightsAndFlushLog() {
  VLOG(1) << "T " << tablet_->metadata()->tablet_id()
      <<  ": Waiting for in-flight transactions to commit.";
  LOG_SLOW_EXECUTION(WARNING, 200, "Committing in-flights took a long time.") {
    tablet_->mvcc_manager()->WaitForAllInFlightToCommit();
  }
  VLOG(1) << "T " << tablet_->metadata()->tablet_id()
      << ": Waiting for the log queue to be flushed.";
  LOG_SLOW_EXECUTION(WARNING, 200, "Flushing the Log queue took a long time.") {
    RETURN_NOT_OK(log_->WaitUntilAllFlushed());
  }
  return Status::OK();
}


}  // namespace tablet
}  // namespace kudu
