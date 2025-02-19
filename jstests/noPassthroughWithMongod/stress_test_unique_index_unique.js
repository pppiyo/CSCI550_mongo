/**
 * Tests that unique indexes can be built with a large number of unique values.
 *
 * @tags: [
 *  # This workload is too impactful on ephemeralForTest
 *  requires_wiredtiger,
 * ]
 */

(function() {
"use strict";

let coll = db.stress_test_unique_index_unique;
coll.drop();

const kNumDocs = 500000;  // ~15 MB

function loadCollectionWithDocs(collection, numDocs) {
    const kMaxChunkSize = 100000;

    let inserted = 0;
    while (inserted < numDocs) {
        let docs = [];
        for (let i = 0; i < kMaxChunkSize && inserted + docs.length < numDocs; i++) {
            docs.push({"a": inserted + i});
        }
        collection.insertMany(docs);
        inserted += docs.length;
    }
}

loadCollectionWithDocs(coll, kNumDocs);

assert.commandWorked(coll.createIndex({a: 1}, {unique: true}));
})();
