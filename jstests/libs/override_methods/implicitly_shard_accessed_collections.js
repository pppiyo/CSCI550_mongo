/**
 * Loading this file overrides DB.prototype.getCollection() with a function that attempts to shard
 * the collection before returning it.
 *
 * The DB.prototype.getCollection() function is called whenever an undefined property is accessed
 * on the db object.
 *
 * DBCollection.prototype.drop() function will re-shard any non-denylisted collection that is
 * dropped in a sharded cluster.
 */

/**
 * Settings for the converting implictily accessed collections to sharded collections.
 */
const ImplicitlyShardAccessCollSettings = (function() {
    let mode = 0;  // Default to hashed shard key.

    return {
        Modes: {
            kUseHashedSharding: 0,
            kHashedMoveToSingleShard: 1,
        },
        setMode: function(newMode) {
            if (newMode !== 0 && newMode !== 1) {
                throw new Error("Cannot set mode to unknown mode: " + newMode);
            }

            mode = newMode;
        },
        getMode: function() {
            return mode;
        },
    };
})();

(function() {
'use strict';

load("jstests/libs/override_methods/override_helpers.js");  // For 'OverrideHelpers'.
load("jstests/libs/fixture_helpers.js");                    // For 'FixtureHelpers'.

// Save a reference to the original methods in the IIFE's scope.
// This scoping allows the original methods to be called by the overrides below.
var originalGetCollection = DB.prototype.getCollection;
var originalCreateCollection = DB.prototype.createCollection;
var originalDBCollectionDrop = DBCollection.prototype.drop;
var originalStartParallelShell = startParallelShell;
var originalRunCommand = Mongo.prototype.runCommand;

var testMayRunDropInParallel = false;

// Denylisted namespaces that should not be sharded.
var denylistedNamespaces = [
    /\$cmd/,
    /^admin\./,
    /^config\./,
    /\.system\./,
    /enxcol_\..*\.esc/,
    /enxcol_\..*\.ecc/,
    /enxcol_\..*\.ecoc/,
];

const kZoneName = 'moveToHereForMigrationPassthrough';

function shardCollection(collection) {
    return shardCollectionWithSpec(
        {db: collection.getDB(), collName: collection.getName(), shardKey: {_id: 'hashed'}});
}

function shardCollectionWithSpec({db, collName, shardKey, timeseriesSpec}) {
    // Don't attempt to shard if this operation is running on mongoD.
    if (!FixtureHelpers.isMongos(db)) {
        return;
    }

    var dbName = db.getName();
    var fullName = dbName + "." + collName;

    for (var ns of denylistedNamespaces) {
        if (fullName.match(ns)) {
            return;
        }
    }

    var res = db.adminCommand({enableSharding: dbName});

    // enableSharding may only be called once for a database.
    if (res.code !== ErrorCodes.AlreadyInitialized) {
        assert.commandWorked(res, "enabling sharding on the '" + dbName + "' db failed");
    }

    let shardCollCmd = {shardCollection: fullName, key: shardKey, collation: {locale: "simple"}};
    if (timeseriesSpec) {
        shardCollCmd["timeseries"] = timeseriesSpec;
    }
    res = db.adminCommand(shardCollCmd);

    let checkResult = function(res, opDescription) {
        if (res.ok === 0 && testMayRunDropInParallel) {
            // We ignore ConflictingOperationInProgress error responses from the
            // "shardCollection" command if it's possible the test was running a "drop" command
            // concurrently. We could retry running the "shardCollection" command, but tests
            // that are likely to trigger this case are also likely running the "drop" command
            // in a loop. We therefore just let the test continue with the collection being
            // unsharded.
            assert.commandFailedWithCode(res, ErrorCodes.ConflictingOperationInProgress);
            jsTest.log("Ignoring failure while " + opDescription +
                       " due to a concurrent drop operation: " + tojson(res));
        } else {
            assert.commandWorked(res, opDescription + " failed");
        }
    };

    checkResult(res, 'shard ' + fullName);

    // Set the entire chunk range to a single zone, so balancer will be forced to move the
    // evenly distributed chunks to a shard (selected at random).
    if (res.ok === 1 &&
        ImplicitlyShardAccessCollSettings.getMode() ===
            ImplicitlyShardAccessCollSettings.Modes.kHashedMoveToSingleShard) {
        let shardName =
            db.getSiblingDB('config').shards.aggregate([{$sample: {size: 1}}]).toArray()[0]._id;

        checkResult(db.adminCommand({addShardToZone: shardName, zone: kZoneName}),
                    'add ' + shardName + ' to zone ' + kZoneName);
        checkResult(db.adminCommand({
            updateZoneKeyRange: fullName,
            min: {_id: MinKey},
            max: {_id: MaxKey},
            zone: kZoneName
        }),
                    'set zone for ' + fullName);

        // Wake up the balancer.
        checkResult(db.adminCommand({balancerStart: 1}), 'turn on balancer');
    }
}

DB.prototype.createCollection = function() {
    const createCollResult = originalCreateCollection.apply(this, arguments);

    if (!createCollResult.ok || arguments.length < 2 || arguments[1] == null ||
        !isObject(arguments[1]) || !arguments[1].timeseries || !arguments[1].timeseries.timeField) {
        return createCollResult;
    }

    // We check feature flags on both primary and secondaries in case a step down happens after this
    // check.
    const featureResults = FixtureHelpers
                               .runCommandOnAllShards({
                                   db: this.getSiblingDB('admin'),
                                   cmdObj: {getParameter: 1, featureFlagShardedTimeSeries: 1}
                               })
                               .map(result => assert.commandWorked(result));

    // The feature can only be used if the version associated with the feature is greater than or
    // equal to the FCV version. The getParameter does not consider the FCV value when checking for
    // whether the feature flag is enabled. So we run an additional getParameter command to fetch
    // the FCV state.
    const fcvResult =
        assert.commandWorked(FixtureHelpers.getPrimaryForNodeHostingDatabase(this).adminCommand(
            {getParameter: 1, featureCompatibilityVersion: 1}));
    const isTimeseriesShardingEnabled = featureResults.every(
        result => result.featureFlagShardedTimeSeries.value &&
            MongoRunner.compareBinVersions(fcvResult.featureCompatibilityVersion.version,
                                           result.featureFlagShardedTimeSeries.version) >= 0);
    if (!isTimeseriesShardingEnabled) {
        return createCollResult;
    }

    const timeField = arguments[1]["timeseries"]["timeField"];
    shardCollectionWithSpec({
        db: this,
        collName: arguments[0],
        shardKey: {[timeField]: 1},
        timeseriesSpec: arguments[1]["timeseries"]
    });

    return createCollResult;
};

DB.prototype.getCollection = function() {
    var collection = originalGetCollection.apply(this, arguments);

    // The following "collStats" command can behave unexpectedly when running in a causal
    // consistency suite with secondary read preference. "collStats" does not support causal
    // consistency, making it possible to see a stale view of the collection if run on a
    // secondary, potentially causing shardCollection() to be called when it shouldn't.
    // E.g. if the collection has just been sharded but not yet visible on the
    // secondary, we could end up calling shardCollection on it again, which would fail.
    //
    // The workaround is to use a TestData flag to temporarily bypass the read preference
    // override.
    const testDataDoNotOverrideReadPreferenceOriginal = TestData.doNotOverrideReadPreference;
    let collStats;

    try {
        TestData.doNotOverrideReadPreference = true;
        collStats = this.runCommand({collStats: collection.getName()});
        if (!collStats.ok && collStats.codeName == "CommandNotSupportedOnView") {
            // In case we catch CommandNotSupportedOnView it means the collection was actually a
            // view and should be returned without attempting to shard it (which is not allowed)
            return collection;
        }
    } finally {
        TestData.doNotOverrideReadPreference = testDataDoNotOverrideReadPreferenceOriginal;
    }

    // If the collection is already sharded or is non-empty, do not attempt to shard.
    if (collStats.sharded || collStats.count > 0) {
        return collection;
    }

    // Attempt to enable sharding on database and collection if not already done.
    if (!TestData.implicitlyShardOnCreateCollectionOnly) {
        shardCollection(collection);
    }

    return collection;
};

DBCollection.prototype.drop = function() {
    var dropResult = originalDBCollectionDrop.apply(this, arguments);

    // Attempt to enable sharding on database and collection if not already done.
    if (!TestData.implicitlyShardOnCreateCollectionOnly) {
        shardCollection(this);
    }

    return dropResult;
};

// The mapReduce command has a special requirement where the command must indicate the output
// collection is sharded, so we must be sure to add this information in this passthrough.
Mongo.prototype.runCommand = function(dbName, cmdObj, options) {
    // Skip any commands that are not mapReduce or do not have an 'out' option.
    if (typeof cmdObj !== 'object' || cmdObj === null ||
        (!cmdObj.hasOwnProperty('mapreduce') && !cmdObj.hasOwnProperty('mapReduce')) ||
        !cmdObj.hasOwnProperty('out')) {
        return originalRunCommand.apply(this, arguments);
    }

    const originalCmdObj = Object.merge({}, cmdObj);

    // SERVER-5448 'jsMode' is not supported through mongos. The 'jsMode' should not impact the
    // results at all, so can be safely deleted in the sharded environment.
    delete cmdObj.jsMode;

    // Modify the output options to specify that the collection is sharded.
    let outputSpec = cmdObj.out;
    if (typeof (outputSpec) === "string") {
        this.getDB(dbName)[outputSpec].drop();  // This will implicitly shard it.
        outputSpec = {replace: outputSpec};
    } else if (typeof (outputSpec) !== "object") {
        // This is a malformed command, just send it along.
        return originalRunCommand.apply(this, arguments);
    } else if (!outputSpec.hasOwnProperty("sharded")) {
        let outputColl = null;
        if (outputSpec.hasOwnProperty("replace")) {
            outputColl = outputSpec.replace;
        } else if (outputSpec.hasOwnProperty("merge")) {
            outputColl = outputSpec.merge;
        } else if (outputSpec.hasOwnProperty("reduce")) {
            outputColl = outputSpec.reduce;
        }

        if (outputColl === null) {
            // This is a malformed command, just send it along.
            return originalRunCommand.apply(this, arguments);
        }
        this.getDB(dbName)[outputColl].drop();  // This will implicitly shard it.
        outputSpec.sharded = true;
    }

    cmdObj.out = outputSpec;
    jsTestLog('Overriding mapReduce command. Original command: ' + tojson(originalCmdObj) +
              ' New command: ' + tojson(cmdObj));
    return originalRunCommand.apply(this, arguments);
};

// Tests may use a parallel shell to run the "drop" command concurrently with other
// operations. This can cause the "shardCollection" command to return a
// ConflictingOperationInProgress error response.
startParallelShell = function() {
    testMayRunDropInParallel = true;
    return originalStartParallelShell.apply(this, arguments);
};

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/implicitly_shard_accessed_collections.js");
}());
