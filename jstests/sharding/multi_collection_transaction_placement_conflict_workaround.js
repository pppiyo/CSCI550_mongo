/*
 * Tests that multi-document transactions fail with MigrationConflict (and TransientTransactionError
 * label) if a collection or database placement changes have occurred later than the transaction
 * data snapshot timestamp.
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallel_shell_helpers.js");

const st = new ShardingTest({mongos: 1, shards: 2});

// Test transaction with concurrent chunk migration.
{
    const dbName = 'test';
    const collName1 = 'foo';
    const collName2 = 'bar';
    const ns1 = dbName + '.' + collName1;
    const ns2 = dbName + '.' + collName2;

    let coll1 = st.s.getDB(dbName)[collName1];
    let coll2 = st.s.getDB(dbName)[collName2];
    // Setup initial state:
    //   ns1: unsharded collection on shard0, with documents: {a: 0}
    //   ns2: sharded collection with chunks both on shard0 and shard1, with documents: {x: -1}, {x:
    //   1}
    st.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName});

    st.adminCommand({shardCollection: ns2, key: {x: 1}});
    assert.commandWorked(st.splitAt(ns2, {x: 0}));
    assert.commandWorked(st.moveChunk(ns2, {x: -1}, st.shard0.shardName));
    assert.commandWorked(st.moveChunk(ns2, {x: 0}, st.shard1.shardName));

    assert.commandWorked(coll1.insert({a: 1}));

    assert.commandWorked(coll2.insert({x: -1}));
    assert.commandWorked(coll2.insert({x: 1}));

    // Start a multi-document transaction and make one read on shard0
    const session = st.s.startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl1 = sessionDB.getCollection(collName1);
    const sessionColl2 = sessionDB.getCollection(collName2);
    session.startTransaction();  // Default is local RC. With snapshot RC there's no bug.
    assert.eq(1, sessionColl1.find().itcount());

    // While the transaction is still open, move ns2's [0, 100) chunk to shard0.
    assert.commandWorked(st.moveChunk(ns2, {x: 0}, st.shard0.shardName));
    // Refresh the router so that it doesn't send a stale SV to the shard, which would cause the txn
    // to be aborted.
    assert.eq(2, coll2.find().itcount());

    // Trying to read coll2 will result in an error. Note that this is not retryable even with
    // enableStaleVersionAndSnapshotRetriesWithinTransactions enabled because the first statement
    // aleady had an active snapshot open on the same shard this request is trying to contact.
    let err = assert.throwsWithCode(() => {
        sessionColl2.find().itcount();
    }, ErrorCodes.MigrationConflict);

    assert.contains("TransientTransactionError", err.errorLabels, tojson(err));
}

// Test transaction with concurrent move primary.
{
    const dbName1 = 'test';
    const dbName2 = 'test2';
    const collName1 = 'foo';
    const collName2 = 'foo';

    function runTest(readConcernLevel) {
        st.getDB(dbName1).dropDatabase();
        st.getDB(dbName2).dropDatabase();
        st.adminCommand({enableSharding: dbName1, primaryShard: st.shard0.shardName});
        st.adminCommand({enableSharding: dbName2, primaryShard: st.shard1.shardName});

        const coll1 = st.getDB(dbName1)[collName1];
        coll1.insert({x: 1, c: 0});

        const coll2 = st.getDB(dbName2)[collName2];
        coll2.insert({x: 2, c: 0});

        // Set failpoint to hang the movePrimary cloner after having created the collections on the
        // destination shard but before cloning their documents.
        let clonerFp = configureFailPoint(st.rs0.getPrimary(),
                                          "movePrimaryClonerHangBeforeStartCloneDocuments");

        let awaitMovePrimary = startParallelShell(
            funWithArgs(function(dbName, toShard) {
                assert.commandWorked(db.adminCommand({movePrimary: dbName, to: toShard}));
            }, dbName2, st.shard0.shardName), st.s.port);

        clonerFp.wait();

        // Start a multi-document transaction. Execute one statement that will target shard0.
        let session = st.s.startSession();
        session.startTransaction({readConcern: {level: readConcernLevel}});
        assert.eq(1, session.getDatabase(dbName1)[collName1].find().itcount());

        // Wait movePrimary to commit moving dbName2 from shard1 to shard0.
        clonerFp.off();
        awaitMovePrimary();

        // Make sure the router has fresh routing info to avoid causing the transaction to fail due
        // to StaleConfig.
        assert.eq(1, coll2.find().itcount());

        // Execute a second statement, now on dbName2. This statement will be routed to shard0
        // (since there's no historical routing for databases). Expect it to fail with
        // MigrationConflict error.
        let err = assert.throwsWithCode(() => {
            session.getDatabase(dbName2)[collName2].find().itcount();
        }, ErrorCodes.MigrationConflict);

        assert.contains("TransientTransactionError", err.errorLabels, tojson(err));
    }

    runTest('majority');
    runTest('snapshot');
}

st.stop();
})();
