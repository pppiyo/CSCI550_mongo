/**
 * Tests that deleting the index on a hashed shard key blocks its orphan documents from being
 * deleted and allows other range deletion processes to continue.
 *
 * @tags: [
 *   requires_fcv_50,
 * ]
 */
(function() {
'use strict';

load("jstests/sharding/libs/find_chunks_util.js");
load("jstests/libs/fail_point_util.js");

const rangeDeleterBatchSize = 50;

const st = new ShardingTest({
    other: {
        enableBalancer: false,
        shardOptions: {setParameter: {rangeDeleterBatchSize: rangeDeleterBatchSize}},
    }
});

// Setup database and collection for test
const dbName = 'db';
const db = st.getDB(dbName);
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
const coll = db['test'];
const collWithIndex = db['collWithIndex'];

function setUpCollection(collectionName, nss) {
    // This creates an index on the hashed shard key.
    assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {_id: 'hashed'}}));

    // Insert some documents into the collection.
    const numDocs = 1000;
    let bulk = collectionName.initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; i++) {
        bulk.insert({_id: i});
    }
    assert.commandWorked(bulk.execute());

    // Move a chunk to create orphan documents.
    const chunk =
        findChunksUtil.findOneChunkByNs(st.s.getDB('config'), nss, {shard: st.shard0.shardName});
    assert.commandWorked(
        db.adminCommand({moveChunk: nss, bounds: [chunk.min, chunk.max], to: st.shard1.shardName}));
}

// Pause range deletion on shard0.
let suspendRangeDeletionFailpoint = configureFailPoint(st.shard0, "suspendRangeDeletion");
setUpCollection(coll, coll.getFullName());
setUpCollection(collWithIndex, collWithIndex.getFullName());
assert.commandWorked(coll.dropIndex({"_id": "hashed"}));
suspendRangeDeletionFailpoint.off();

// Verify that the range deletion document for db.test persists while the document for
// db.collWithIndex is successfully deleted.
assert.eq(1, st.shard0.getDB("config").getCollection("rangeDeletions").countDocuments({
    nss: coll.getFullName()
}));

assert.soon(() => {
    return st.shard0.getDB("config").getCollection("rangeDeletions").countDocuments({
        nss: collWithIndex.getFullName()
    }) === 0;
});

assert.eq(1, st.shard0.getDB("config").getCollection("rangeDeletions").countDocuments({
    nss: coll.getFullName()
}));

// Rebuild the hashed shard key index for db.test and ensure the pending range deletion completes.
assert.commandWorked(coll.createIndex({"_id": "hashed"}));
assert.soon(() => {
    return st.shard0.getDB("config").getCollection("rangeDeletions").countDocuments({
        nss: coll.getFullName()
    }) === 0;
});

st.stop();
})();
