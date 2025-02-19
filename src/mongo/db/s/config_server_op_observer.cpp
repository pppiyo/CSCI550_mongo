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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/config_server_op_observer.h"

#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/topology_time_ticker.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/cluster_identity_loader.h"

namespace mongo {

ConfigServerOpObserver::ConfigServerOpObserver() = default;

ConfigServerOpObserver::~ConfigServerOpObserver() = default;

void ConfigServerOpObserver::onDelete(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      const UUID& uuid,
                                      StmtId stmtId,
                                      const OplogDeleteEntryArgs& args) {
    if (nss == VersionType::ConfigNS) {
        if (!repl::ReplicationCoordinator::get(opCtx)->getMemberState().rollback()) {
            uasserted(40302, "cannot delete config.version document while in --configsvr mode");
        } else {
            // TODO (SERVER-34165): this is only used for rollback via refetch and can be removed
            // with it.
            // Throw out any cached information related to the cluster ID.
            ShardingCatalogManager::get(opCtx)->discardCachedConfigDatabaseInitializationState();
            ClusterIdentityLoader::get(opCtx)->discardCachedClusterId();
        }
    }
}

repl::OpTime ConfigServerOpObserver::onDropCollection(OperationContext* opCtx,
                                                      const NamespaceString& collectionName,
                                                      const UUID& uuid,
                                                      std::uint64_t numRecords,
                                                      const CollectionDropType dropType) {
    if (collectionName == VersionType::ConfigNS) {
        if (!repl::ReplicationCoordinator::get(opCtx)->getMemberState().rollback()) {
            uasserted(40303, "cannot drop config.version document while in --configsvr mode");
        } else {
            // TODO (SERVER-34165): this is only used for rollback via refetch and can be removed
            // with it.
            // Throw out any cached information related to the cluster ID.
            ShardingCatalogManager::get(opCtx)->discardCachedConfigDatabaseInitializationState();
            ClusterIdentityLoader::get(opCtx)->discardCachedClusterId();
        }
    }

    return {};
}

void ConfigServerOpObserver::_onReplicationRollback(OperationContext* opCtx,
                                                    const RollbackObserverInfo& rbInfo) {
    if (rbInfo.configServerConfigVersionRolledBack) {
        // Throw out any cached information related to the cluster ID.
        ShardingCatalogManager::get(opCtx)->discardCachedConfigDatabaseInitializationState();
        ClusterIdentityLoader::get(opCtx)->discardCachedClusterId();
    }

    if (rbInfo.rollbackNamespaces.find(ShardType::ConfigNS) != rbInfo.rollbackNamespaces.end()) {
        // If some entries were rollbacked from config.shards we might need to discard some tick
        // points from the TopologyTimeTicker
        const auto lastApplied = repl::ReplicationCoordinator::get(opCtx)->getMyLastAppliedOpTime();
        TopologyTimeTicker::get(opCtx).onReplicationRollback(lastApplied);
    }
}

void ConfigServerOpObserver::onInserts(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       const UUID& uuid,
                                       std::vector<InsertStatement>::const_iterator begin,
                                       std::vector<InsertStatement>::const_iterator end,
                                       bool fromMigrate) {
    if (nss != ShardType::ConfigNS) {
        return;
    }

    if (!topology_time_ticker_utils::inRecoveryMode(opCtx)) {
        boost::optional<Timestamp> maxTopologyTime;
        for (auto it = begin; it != end; it++) {
            Timestamp newTopologyTime = it->doc[ShardType::topologyTime.name()].timestamp();
            if (newTopologyTime != Timestamp()) {
                if (!maxTopologyTime || newTopologyTime > *maxTopologyTime) {
                    maxTopologyTime = newTopologyTime;
                }
            }
        }

        if (maxTopologyTime) {
            opCtx->recoveryUnit()->onCommit(
                [opCtx, maxTopologyTime](boost::optional<Timestamp> commitTime) mutable {
                    invariant(commitTime);
                    TopologyTimeTicker::get(opCtx).onNewLocallyCommittedTopologyTimeAvailable(
                        *commitTime, *maxTopologyTime);
                });
        }
    }
}

void ConfigServerOpObserver::onApplyOps(OperationContext* opCtx,
                                        const std::string& dbName,
                                        const BSONObj& applyOpCmd) {
    if (dbName != ShardType::ConfigNS.db()) {
        return;
    }

    if (topology_time_ticker_utils::inRecoveryMode(opCtx)) {
        return;
    }

    if (applyOpCmd.firstElementFieldNameStringData() != "applyOps") {
        return;
    }

    const auto& updatesElem = applyOpCmd["applyOps"];
    if (updatesElem.type() != Array) {
        return;
    }

    auto updates = updatesElem.Array();
    if (updates.size() != 2) {
        return;
    }

    if (updates[0].type() != Object) {
        return;
    }
    auto removeShard = updates[0].Obj();
    if (removeShard["op"].str() != "d") {
        return;
    }
    if (removeShard["ns"].str() != ShardType::ConfigNS.ns()) {
        return;
    }

    if (updates[1].type() != Object) {
        return;
    }
    auto updateShard = updates[1].Obj();
    if (updateShard["op"].str() != "u") {
        return;
    }
    if (updateShard["ns"].str() != ShardType::ConfigNS.ns()) {
        return;
    }

    auto updateElem = updateShard["o"];
    if (updateElem.type() != BSONType::Object) {
        return;
    }

    auto updateObj = updateElem.embeddedObject();
    if (update_oplog_entry::extractUpdateType(updateObj) ==
        update_oplog_entry::UpdateType::kReplacement) {
        return;
    }

    auto newTopologyTime =
        update_oplog_entry::extractNewValueForField(updateObj, ShardType::topologyTime())
            .timestamp();

    opCtx->recoveryUnit()->onCommit(
        [opCtx, newTopologyTime](boost::optional<Timestamp> commitTime) mutable {
            invariant(commitTime);
            TopologyTimeTicker::get(opCtx).onNewLocallyCommittedTopologyTimeAvailable(
                *commitTime, newTopologyTime);
        });
}

void ConfigServerOpObserver::onMajorityCommitPointUpdate(ServiceContext* service,
                                                         const repl::OpTime& newCommitPoint) {
    Timestamp newCommitPointTime = newCommitPoint.getTimestamp();

    // TopologyTime must always be <= ConfigTime, so ticking them separately is fine as long as
    // ConfigTime is done first.
    VectorClockMutable::get(service)->tickConfigTimeTo(LogicalTime(newCommitPointTime));

    // Letting the TopologyTimeTicker know that the majority commit point was advanced
    TopologyTimeTicker::get(service).onMajorityCommitPointUpdate(service, newCommitPoint);
}

}  // namespace mongo
