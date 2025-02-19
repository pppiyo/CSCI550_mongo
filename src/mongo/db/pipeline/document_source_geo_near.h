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

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/field_path.h"

namespace mongo {

class DocumentSourceGeoNear : public DocumentSource {
public:
    static constexpr StringData kKeyFieldName = "key"_sd;
    static constexpr StringData kStageName = "$geoNear"_sd;

    /**
     * Only exposed for testing.
     */
    static boost::intrusive_ptr<DocumentSourceGeoNear> create(
        const boost::intrusive_ptr<ExpressionContext>&);

    const char* getSourceName() const final {
        return DocumentSourceGeoNear::kStageName.rawData();
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        return {StreamType::kStreaming,
                PositionRequirement::kCustom,
                HostTypeRequirement::kAnyShard,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kNotAllowed,
                TransactionRequirement::kAllowed,
                LookupRequirement::kAllowed,
                UnionRequirement::kAllowed};
    }

    void validatePipelinePosition(bool alreadyOptimized,
                                  Pipeline::SourceContainer::const_iterator pos,
                                  const Pipeline::SourceContainer& container) const final {
        // This stage must be in the first position in the pipeline after optimization.
        uassert(40603,
                str::stream() << getSourceName()
                              << " was not the first stage in the pipeline after optimization. Is "
                                 "optimization disabled or inhibited?",
                !alreadyOptimized || pos == container.cbegin());
    }

    /**
     * DocumentSourceGeoNear should always be replaced by a DocumentSourceGeoNearCursor before
     * executing a pipeline, so this method should never be called.
     */
    GetNextResult doGetNext() final {
        MONGO_UNREACHABLE;
    }

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    boost::intrusive_ptr<DocumentSource> optimize() final;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pCtx);


    bool hasQuery() const override;

    /**
     * A query predicate to apply to the documents in addition to the "near" predicate.
     */
    BSONObj getQuery() const override {
        return query;
    };

    /**
     * Set the query predicate to apply to the documents in addition to the "near" predicate.
     */
    void setQuery(BSONObj newQuery) {
        query = newQuery.getOwned();
    };

    /**
     * The field in which the computed distance will be stored.
     */
    FieldPath getDistanceField() const {
        return *distanceField;
    }

    /**
     * The field in which the matching point will be stored, if requested.
     */
    boost::optional<FieldPath> getLocationField() const {
        return includeLocs;
    }

    /**
     * The field over which to apply the "near" predicate, if specified.
     */
    boost::optional<FieldPath> getKeyField() const {
        return keyFieldPath;
    }

    /**
     * Set the field over which to apply the "near" predicate.
     */
    void setKeyField(const FieldPath& newPath) {
        keyFieldPath = newPath;
    }

    /**
     * A scaling factor to apply to the distance, if specified by the user.
     */
    boost::optional<double> getDistanceMultiplier() const {
        return distanceMultiplier;
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    /**
     * Returns true if the $geoNear specification requires the geoNear point metadata.
     */
    bool needsGeoNearPoint() const;

    /**
     * Converts this $geoNear aggregation stage into an equivalent $near or $nearSphere query on
     * 'nearFieldName'.
     */
    BSONObj asNearQuery(StringData nearFieldName);

    /**
     * In a sharded cluster, this becomes a merge sort by distance, from nearest to furthest.
     */
    boost::optional<DistributedPlanLogic> distributedPlanLogic() final;

protected:
    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) final;

private:
    explicit DocumentSourceGeoNear(const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /**
     * If this stage immediately follows an $_internalUnpackBucket, split it up into several stages
     * including an explicit $sort.
     *
     * Does nothing if not immediately following an $_internalUnpackBucket.
     */
    Pipeline::SourceContainer::iterator splitForTimeseries(Pipeline::SourceContainer::iterator itr,
                                                           Pipeline::SourceContainer* container);


    /**
     * Parses the fields in the object 'options', throwing if an error occurs.
     */
    void parseOptions(BSONObj options, const boost::intrusive_ptr<ExpressionContext>& pCtx);

    // These fields describe the command to run.
    // 'near' and 'distanceField' are required; the rest are optional.
    boost::intrusive_ptr<Expression> _nearGeometry;

    std::unique_ptr<FieldPath> distanceField;  // Using unique_ptr because FieldPath can't be empty
    BSONObj query;
    bool spherical;
    boost::optional<double> maxDistance;
    boost::optional<double> minDistance;
    boost::optional<double> distanceMultiplier;
    boost::optional<FieldPath> includeLocs;
    boost::optional<FieldPath> keyFieldPath;
};
}  // namespace mongo
