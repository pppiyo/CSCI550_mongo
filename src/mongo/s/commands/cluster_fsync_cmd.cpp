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

#include "mongo/platform/basic.h"

#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

class FsyncCommand : public ErrmsgCommandDeprecated {
public:
    FsyncCommand() : ErrmsgCommandDeprecated("fsync") {}

    std::string help() const override {
        return "invoke fsync on all shards belonging to the cluster";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        ActionSet actions;
        actions.addAction(ActionType::fsync);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    void unlockLockedShards(OperationContext* opCtx, const std::string& dbname) {

        auto request = OpMsgRequest::fromDBAndBody(dbname, BSON("fsyncUnlock" << 1));
        auto response = CommandHelpers::runCommandDirectly(opCtx, request);
    }

    bool errmsgRun(OperationContext* opCtx,
                   const std::string& dbname,
                   const BSONObj& cmdObj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {

        BSONObj fsyncCmdObj = cmdObj;
        if (cmdObj["lock"].trueValue()) {
            auto forBackupField = BSON("forBackup" << true);
            fsyncCmdObj = fsyncCmdObj.addFields(forBackupField);
        }

        auto shardResults = scatterGatherUnversionedTargetConfigServerAndShards(
            opCtx,
            dbname,
            applyReadWriteConcern(
                opCtx, this, CommandHelpers::filterCommandRequestForPassthrough(fsyncCmdObj)),
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            Shard::RetryPolicy::kIdempotent);

        BSONObjBuilder rawResult;
        const auto response = appendRawResponses(opCtx, &errmsg, &rawResult, shardResults);

        // This field has had dummy value since MMAP went away. It is undocumented.
        // Maintaining it so as not to cause unnecessary user pain across upgrades.
        result.append("numFiles", 1);
        result.append("all", rawResult.obj());
        if (!response.responseOK) {
            if (cmdObj["lock"].trueValue()) {
                unlockLockedShards(opCtx, dbname);
            }
            return false;
        }

        return true;
    }

} clusterFsyncCmd;

}  // namespace
}  // namespace mongo
