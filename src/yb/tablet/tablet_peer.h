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

#ifndef YB_TABLET_TABLET_PEER_H_
#define YB_TABLET_TABLET_PEER_H_

#include <atomic>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "yb/consensus/consensus.h"
#include "yb/consensus/consensus_meta.h"
#include "yb/consensus/log.h"
#include "yb/gutil/callback.h"
#include "yb/gutil/ref_counted.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/rpc/rpc_fwd.h"

#include "yb/tablet/transaction_coordinator.h"
#include "yb/tablet/transaction_participant.h"
#include "yb/tablet/operation_order_verifier.h"
#include "yb/tablet/operations/operation_tracker.h"
#include "yb/tablet/preparer.h"
#include "yb/tablet/tablet_options.h"
#include "yb/tablet/tablet_fwd.h"

#include "yb/util/metrics.h"
#include "yb/util/semaphore.h"

using yb::consensus::StateChangeContext;

namespace yb {

namespace consensus {
class RaftConsensus;
}

namespace log {
class LogAnchorRegistry;
}

namespace tserver {
class CatchUpServiceTest;
class UpdateTransactionResponsePB;
}

class MaintenanceManager;
class MaintenanceOp;
class ThreadPool;

namespace tablet {

// A peer in a tablet consensus configuration, which coordinates writes to tablets.
// Each time Write() is called this class appends a new entry to a replicated
// state machine through a consensus algorithm, which makes sure that other
// peers see the same updates in the same order. In addition to this, this
// class also splits the work and coordinates multi-threaded execution.
class TabletPeer : public RefCountedThreadSafe<TabletPeer>,
                   public consensus::ReplicaOperationFactory,
                   public TransactionParticipantContext,
                   public TransactionCoordinatorContext {
 public:
  typedef std::map<int64_t, int64_t> MaxIdxToSegmentSizeMap;

  TabletPeer(const scoped_refptr<TabletMetadata>& meta,
             const consensus::RaftPeerPB& local_peer_pb, ThreadPool* apply_pool,
             Callback<void(std::shared_ptr<StateChangeContext> context)> mark_dirty_clbk);

  // Initializes the TabletPeer, namely creating the Log and initializing
  // Consensus.
  CHECKED_STATUS InitTabletPeer(const std::shared_ptr<TabletClass> &tablet,
                                const std::shared_future<client::YBClientPtr> &client_future,
                                const scoped_refptr<server::Clock> &clock,
                                const std::shared_ptr<rpc::Messenger> &messenger,
                                const scoped_refptr<log::Log> &log,
                                const scoped_refptr<MetricEntity> &metric_entity,
                                ThreadPool* raft_pool,
                                ThreadPool* tablet_prepare_pool);

  // Starts the TabletPeer, making it available for Write()s. If this
  // TabletPeer is part of a consensus configuration this will connect it to other peers
  // in the consensus configuration.
  CHECKED_STATUS Start(const consensus::ConsensusBootstrapInfo& info);

  // Shutdown this tablet peer.
  // If a shutdown is already in progress, blocks until that shutdown is complete.
  void Shutdown();

  // Check that the tablet is in a RUNNING state.
  CHECKED_STATUS CheckRunning() const;

  // Check that the tablet is in a SHUTDOWN/NOT_STARTED state.
  CHECKED_STATUS CheckShutdownOrNotStarted() const;

  // Wait until the tablet is in a RUNNING state or if there's a timeout.
  // TODO have a way to wait for any state?
  CHECKED_STATUS WaitUntilConsensusRunning(const MonoDelta& timeout);

  // Submits a write to a tablet and executes it asynchronously.
  // The caller is expected to build and pass a WriteOperationState that points
  // to the RPC WriteRequest, WriteResponse, RpcContext and to the tablet's
  // MvccManager.
  // The operation_state is deallocated after use by this function.
  CHECKED_STATUS SubmitWrite(
      std::unique_ptr<WriteOperationState> operation_state, MonoTime deadline);

  void Submit(std::unique_ptr<Operation> operation);

  HybridTime Now() override;

  void UpdateClock(HybridTime hybrid_time) override;

  std::unique_ptr<UpdateTxnOperationState> CreateUpdateTransactionState(
      tserver::TransactionStatePB* request) override;

  void SubmitUpdateTransaction(std::unique_ptr<UpdateTxnOperationState> state) override;

  void GetTabletStatusPB(TabletStatusPB* status_pb_out) const;

  // Used by consensus to create and start a new ReplicaOperation.
  CHECKED_STATUS StartReplicaOperation(
      const scoped_refptr<consensus::ConsensusRound>& round,
      HybridTime propagated_safe_time) override;

  // This is an override of a ReplicaOperationFactory method. This is called from
  // UpdateReplica -> EnqueuePreparesUnlocked on Raft heartbeats.
  void SetPropagatedSafeTime(HybridTime ht) override;

  consensus::Consensus* consensus() const;

  std::shared_ptr<consensus::Consensus> shared_consensus() const;

  TabletClass* tablet() const {
    std::lock_guard<simple_spinlock> lock(lock_);
    return tablet_.get();
  }

  std::shared_ptr<TabletClass> shared_tablet() const {
    std::lock_guard<simple_spinlock> lock(lock_);
    return tablet_;
  }

  const TabletStatePB state() const {
    return state_.load(std::memory_order_acquire);
  }

  // Returns the current Raft configuration.
  const consensus::RaftConfigPB RaftConfig() const;

  TabletStatusListener* status_listener() const {
    return status_listener_.get();
  }

  // Sets the tablet to a BOOTSTRAPPING state, indicating it is starting up.
  void SetBootstrapping() {
    CHECK_OK(UpdateState(TabletStatePB::NOT_STARTED, TabletStatePB::BOOTSTRAPPING, ""));
  }

  Status UpdateState(TabletStatePB expected, TabletStatePB new_state, string error_message) {
    TabletStatePB old = expected;
    return (state_.compare_exchange_strong(old, new_state, std::memory_order_acq_rel) ?
        Status::OK() : STATUS_FORMAT(
            InvalidArgument, "$0 Expected state:$1, got:$2",
            error_message, TabletStatePB_Name(expected), TabletStatePB_Name(old)));
  }

  // sets the tablet state to FAILED additionally setting the error to the provided
  // one.
  void SetFailed(const Status& error) {
    DCHECK(error_.get(std::memory_order_acquire) == nullptr);
    error_ = MakeAtomicUniquePtr<Status>(error);
    state_.store(TabletStatePB::FAILED, std::memory_order_release);
  }

  // Returns the error that occurred, when state is FAILED.
  CHECKED_STATUS error() const {
    Status *error;
    if ((error = error_.get(std::memory_order_acquire)) != nullptr) {
      // Once the error_ is set, we do not reset it to nullptr
      return *error;
    }
    return Status::OK();
  }

  // Returns a human-readable string indicating the state of the tablet.
  // Typically this looks like "NOT_STARTED", "TABLET_DATA_COPYING",
  // etc. For use in places like the Web UI.
  std::string HumanReadableState() const;

  // Adds list of transactions in-flight at the time of the call to
  // 'out'. OperationStatusPB objects are used to allow this method
  // to be used by both the web-UI and ts-cli.
  void GetInFlightOperations(Operation::TraceType trace_type,
                             std::vector<consensus::OperationStatusPB>* out) const;

  // Returns the minimum known log index that is in-memory or in-flight.
  // Used for selection of log segments to delete during Log GC.
  CHECKED_STATUS GetEarliestNeededLogIndex(int64_t* log_index) const;

  // Returns a map of log index -> segment size, of all the segments that currently cannot be GCed
  // because in-memory structures have anchors in them.
  //
  // Returns a non-ok status if the tablet isn't running.
  CHECKED_STATUS GetMaxIndexesToSegmentSizeMap(MaxIdxToSegmentSizeMap* idx_size_map) const;

  // Returns the amount of bytes that would be GC'd if RunLogGC() was called.
  //
  // Returns a non-ok status if the tablet isn't running.
  CHECKED_STATUS GetGCableDataSize(int64_t* retention_size) const;

  // Return a pointer to the Log.
  // TabletPeer keeps a reference to Log after Init().
  log::Log* log() const {
    return log_.get();
  }

  server::Clock& clock() const override {
    return *clock_;
  }

  const server::ClockPtr& clock_ptr() const override {
    return clock_;
  }

  const std::shared_future<client::YBClientPtr>& client_future() const override {
    return client_future_;
  }

  consensus::Consensus::LeaderStatus LeaderStatus() const override;

  HybridTime HtLeaseExpiration() const override;

  const scoped_refptr<log::LogAnchorRegistry>& log_anchor_registry() const {
    return log_anchor_registry_;
  }

  // Returns the tablet_id of the tablet managed by this TabletPeer.
  // Returns the correct tablet_id even if the underlying tablet is not available
  // yet.
  const std::string& tablet_id() const override { return tablet_id_; }

  // Convenience method to return the permanent_uuid of this peer.
  const std::string& permanent_uuid() const;

  Result<OperationDriverPtr> NewOperationDriver(std::unique_ptr<Operation> operation,
                                                consensus::DriverType type);

  Result<OperationDriverPtr> NewLeaderOperationDriver(std::unique_ptr<Operation> operation);
  Result<OperationDriverPtr> NewReplicaOperationDriver(std::unique_ptr<Operation> operation);

  // Tells the tablet's log to garbage collect.
  CHECKED_STATUS RunLogGC();

  // Register the maintenance ops associated with this peer's tablet, also invokes
  // Tablet::RegisterMaintenanceOps().
  void RegisterMaintenanceOps(MaintenanceManager* maintenance_manager);

  // Unregister the maintenance ops associated with this peer's tablet.
  // This method is not thread safe.
  void UnregisterMaintenanceOps();

  // Return pointer to the transaction tracker for this peer.
  const OperationTracker* operation_tracker() const { return &operation_tracker_; }

  const scoped_refptr<TabletMetadata>& tablet_metadata() const {
    return meta_;
  }

  TableType table_type();

  // Return the total on-disk size of this tablet replica, in bytes.
  // Caller should hold the lock_.
  uint64_t OnDiskSize() const;

 protected:
  friend class RefCountedThreadSafe<TabletPeer>;
  friend class TabletPeerTest;
  FRIEND_TEST(TabletPeerTest, TestDMSAnchorPreventsLogGC);
  FRIEND_TEST(TabletPeerTest, TestActiveOperationPreventsLogGC);

  ~TabletPeer();

  // Wait until the TabletPeer is fully in SHUTDOWN state.
  void WaitUntilShutdown();

  // After bootstrap is complete and consensus is setup this initiates the transactions
  // that were not complete on bootstrap.
  // Not implemented yet. See .cc file.
  CHECKED_STATUS StartPendingOperations(consensus::RaftPeerPB::Role my_role,
                                        const consensus::ConsensusBootstrapInfo& bootstrap_info);

  scoped_refptr<OperationDriver> CreateOperationDriver();

  virtual std::unique_ptr<Operation> CreateOperation(consensus::ReplicateMsg* replicate_msg);

  const scoped_refptr<TabletMetadata> meta_;

  const std::string tablet_id_;

  const consensus::RaftPeerPB local_peer_pb_;

  // The atomics state_, error_ and has_consensus_ maintain information about the tablet peer.
  // While modifying the other fields in tablet peer, state_ is modified last.
  // error_ is set before state_ is set to an error state.
  std::atomic<enum TabletStatePB> state_;
  AtomicUniquePtr<Status> error_;
  std::atomic<bool> has_consensus_ = {false};

  OperationTracker operation_tracker_;
  OperationOrderVerifier operation_order_verifier_;
  scoped_refptr<log::Log> log_;
  std::shared_ptr<TabletClass> tablet_;
  std::shared_ptr<rpc::Messenger> messenger_;
  std::shared_ptr<consensus::RaftConsensus> consensus_;
  gscoped_ptr<TabletStatusListener> status_listener_;
  simple_spinlock prepare_replicate_lock_;

  // Lock protecting state_ as well as smart pointers to collaborating
  // classes such as tablet_ and consensus_.
  mutable simple_spinlock lock_;

  // Lock taken during Init/Shutdown which ensures that only a single thread
  // attempts to perform major lifecycle operations (Init/Shutdown) at once.
  // This must be acquired before acquiring lock_ if they are acquired together.
  // We don't just use lock_ since the lifecycle operations may take a while
  // and we'd like other threads to be able to quickly poll the state_ variable
  // during them in order to reject RPCs, etc.
  mutable simple_spinlock state_change_lock_;

  std::unique_ptr<Preparer> prepare_thread_;

  // Pool that executes apply tasks for transactions. This is a multi-threaded
  // pool, constructor-injected by either the Master (for system tables) or
  // the Tablet server.
  ThreadPool* apply_pool_;

  scoped_refptr<server::Clock> clock_;

  scoped_refptr<log::LogAnchorRegistry> log_anchor_registry_;

  // Function to mark this TabletPeer's tablet as dirty in the TSTabletManager.
  // This function must be called any time the cluster membership or cluster
  // leadership changes. Note that this function is called synchronously on the followers
  // or leader via the consensus round completion callback of NonTxRoundReplicationFinished.
  // Hence this should be a relatively lightweight function - e.g., update in-memory only state
  // and defer any other heavy duty operations to a thread pool.
  Callback<void(std::shared_ptr<consensus::StateChangeContext> context)> mark_dirty_clbk_;

  // List of maintenance operations for the tablet that need information that only the peer
  // can provide.
  std::vector<MaintenanceOp*> maintenance_ops_;

  // Cache the permanent of the tablet UUID to retrieve it without a lock in the common case.
  mutable std::atomic<bool> cached_permanent_uuid_initialized_ { false };
  mutable std::string cached_permanent_uuid_;

 private:
  std::shared_future<client::YBClientPtr> client_future_;

  DISALLOW_COPY_AND_ASSIGN(TabletPeer);
};

typedef scoped_refptr<TabletPeer> TabletPeerPtr;

}  // namespace tablet
}  // namespace yb

#endif /* YB_TABLET_TABLET_PEER_H_ */
