/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/repl/data_replicator_external_state.h"

namespace mongo {
namespace repl {

class ReplicationCoordinator;
class ReplicationCoordinatorExternalState;

/**
 * Data replicator external state implementation using a replication coordinator.
 */

class DataReplicatorExternalStateImpl : public DataReplicatorExternalState {
public:
    DataReplicatorExternalStateImpl(
        ReplicationCoordinator* replicationCoordinator,
        ReplicationCoordinatorExternalState* replicationCoordinatorExternalState);

    executor::TaskExecutor* getTaskExecutor() const override;
    std::shared_ptr<executor::TaskExecutor> getSharedTaskExecutor() const;

    OpTimeWithTerm getCurrentTermAndLastCommittedOpTime() override;

    void processMetadata(const rpc::ReplSetMetadata& replMetadata,
                         rpc::OplogQueryMetadata oqMetadata) override;

    ChangeSyncSourceAction shouldStopFetching(const HostAndPort& source,
                                              const rpc::ReplSetMetadata& replMetadata,
                                              const rpc::OplogQueryMetadata& oqMetadata,
                                              const OpTime& previousOpTimeFetched,
                                              const OpTime& lastOpTimeFetched) const override;

    ChangeSyncSourceAction shouldStopFetchingOnError(
        const HostAndPort& source, const OpTime& lastOpTimeFetched) const override;

    std::unique_ptr<OplogBuffer> makeInitialSyncOplogBuffer(OperationContext* opCtx) const override;

    /**
     * These arguments are passed directly to OplogApplierImpl's constructor along with some other
     * parameters owned by this DataReplicationExternalStateImpl.
     * These arguments are required by OplogApplier to get the next applier batch and to apply
     * a batch of operations in parallel.
     * None of the the arguments will be owned by OplogApplierImpl.
     */
    std::unique_ptr<OplogApplier> makeOplogApplier(
        OplogBuffer* oplogBuffer,
        OplogApplier::Observer* observer,
        ReplicationConsistencyMarkers* consistencyMarkers,
        StorageInterface* storageInterface,
        const OplogApplier::Options& options,
        ThreadPool* writerPool) final;

    StatusWith<ReplSetConfig> getCurrentConfig() const override;

    StatusWith<BSONObj> loadLocalConfigDocument(OperationContext* opCtx) const override;

    Status storeLocalConfigDocument(OperationContext* opCtx, const BSONObj& config) override;

    StatusWith<LastVote> loadLocalLastVoteDocument(OperationContext* opCtx) const override;

    JournalListener* getReplicationJournalListener() override;

protected:
    ReplicationCoordinator* getReplicationCoordinator() const;
    ReplicationCoordinatorExternalState* getReplicationCoordinatorExternalState() const;

private:
    // Not owned by us.
    ReplicationCoordinator* _replicationCoordinator;

    // Not owned by us.
    ReplicationCoordinatorExternalState* _replicationCoordinatorExternalState;
};


}  // namespace repl
}  // namespace mongo
