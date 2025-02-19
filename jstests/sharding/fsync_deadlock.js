/*
This test runs a cross-shard transaction where the transaction holds onto the collection locks, then
runs an fsyncLock which should fail and timeout as the global S lock cannot be taken.
 * @tags: [
 *   requires_sharding,
 *   requires_fsync,
 * ]
 */

load('jstests/libs/fail_point_util.js');  // For configureFailPoint
load('jstests/libs/parallelTester.js');

const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    config: 1,
});
const shard0Primary = st.rs0.getPrimary();
const shard1Primary = st.rs1.getPrimary();

// Set up a sharded collection with two chunks
const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 0}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: ns, find: {x: MinKey}, to: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 1}, to: st.shard1.shardName}));

const isMultiversion =
    jsTest.options().shardMixedBinVersions || jsTest.options().useRandomBinVersionsWithinReplicaSet;

if (isMultiversion) {
    // Force the recipient shard to refresh its routing table for the collection since the recipient
    // refresh during chunk migration (in 5.0 and previous versions) is only best-effort, and mongos
    // would only retry a transaction on a StaleConfig error if the error is thrown by the first
    // participant shard.
    assert.commandWorked(shard1Primary.adminCommand({_flushRoutingTableCacheUpdates: ns}));
}

function waitForFsyncLockToWaitForLock(st, numThreads) {
    assert.soon(() => {
        let ops = st.s.getDB('admin')
                      .aggregate([
                          {$currentOp: {allUsers: true, idleConnections: true}},
                          {$match: {desc: "fsyncLockWorker", waitingForLock: true}},
                      ])
                      .toArray();
        if (ops.length != numThreads) {
            jsTest.log("Num operations: " + ops.length + ", expected: " + numThreads);
            jsTest.log(ops);
            return false;
        }
        return true;
    });
}

function runTxn(mongosHost, dbName, collName) {
    const mongosConn = new Mongo(mongosHost);
    jsTest.log("Starting a cross-shard transaction with shard0 and shard1 as the participants " +
               "and shard0 as the coordinator shard");
    const lsid = {id: UUID()};
    const txnNumber = NumberLong(35);
    assert.commandWorked(mongosConn.getDB(dbName).runCommand({
        insert: collName,
        documents: [{x: -1}],
        lsid,
        txnNumber,
        startTransaction: true,
        autocommit: false,
    }));
    assert.commandWorked(mongosConn.getDB(dbName).runCommand({
        insert: collName,
        documents: [{x: 1}],
        lsid,
        txnNumber,
        autocommit: false,
    }));
    jsTest.log("Committing the cross-shard transaction");
    assert.commandWorked(
        mongosConn.adminCommand({commitTransaction: 1, lsid, txnNumber, autocommit: false}));
    jsTest.log("Committed the cross-shard transaction");
}

function runFsyncLock(primaryHost) {
    let primaryConn = new Mongo(primaryHost);
    let ret = assert.commandFailed(
        primaryConn.adminCommand({fsync: 1, lock: true, fsyncLockAcquisitionTimeoutMillis: 5000}));
    let errmsg = "Fsync lock timed out";
    assert.eq(ret.errmsg.includes(errmsg), true);
}

// Run a cross-shard transaction that has shard0 as the coordinator. Make the TransactionCoordinator
// thread hang right before the commit decision is written (i.e. after the transaction has entered
// the "prepared" state).
// This way the txn thread holds onto the collection locks
let writeDecisionFp = configureFailPoint(shard0Primary, "hangBeforeWritingDecision");
let txnThread = new Thread(runTxn, st.s.host, dbName, collName);
txnThread.start();
writeDecisionFp.wait();

let fsyncLockThread = new Thread(runFsyncLock, st.s.host);
fsyncLockThread.start();

// Wait for fsyncLockWorker threads on the shard primaries to wait for the global S lock (enqueued
// in the conflict queue).
waitForFsyncLockToWaitForLock(st, 2 /*blocked fsyncLockWorker threads*/);

// Unpause the TransactionCoordinator.
// The transaction thread can now acquire the IX locks since the blocking fsyncLock request (with an
// incompatible Global S lock) has been removed from the conflict queue
writeDecisionFp.off();

fsyncLockThread.join();
txnThread.join();
st.stop();
