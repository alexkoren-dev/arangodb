////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2022 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "ApplicationFeatures/ApplicationServer.h"
#include "Pregel/WorkerConfig.h"
#include "Pregel/Algorithm.h"
#include "Pregel/PregelFeature.h"
#include "Pregel/Utils.h"
#include "VocBase/vocbase.h"

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::pregel;

namespace {
std::vector<ShardID> const emptyEdgeCollectionRestrictions;
}

WorkerConfig::WorkerConfig(TRI_vocbase_t* vocbase) : _vocbase(vocbase) {}

std::string const& WorkerConfig::database() const { return _vocbase->name(); }

void WorkerConfig::updateConfig(PregelFeature& feature, VPackSlice params) {
  VPackSlice coordID = params.get(Utils::coordinatorIdKey);
  VPackSlice vertexShardMap = params.get(Utils::vertexShardsKey);
  VPackSlice edgeShardMap = params.get(Utils::edgeShardsKey);
  VPackSlice edgeCollectionRestrictions =
      params.get(Utils::edgeCollectionRestrictionsKey);
  VPackSlice execNum = params.get(Utils::executionNumberKey);
  VPackSlice collectionPlanIdMap = params.get(Utils::collectionPlanIdMapKey);
  VPackSlice globalShards = params.get(Utils::globalShardListKey);
  VPackSlice async = params.get(Utils::asyncModeKey);

  if (!coordID.isString() || !edgeShardMap.isObject() ||
      !vertexShardMap.isObject() || !execNum.isInteger() ||
      !collectionPlanIdMap.isObject() || !globalShards.isArray()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "Supplied bad parameters to worker");
  }
  _executionNumber = execNum.getUInt();
  _coordinatorId = coordID.copyString();
  _asynchronousMode = async.getBool();

  // memory mapping
  // start off with default value
  _useMemoryMaps = feature.useMemoryMaps();
  if (VPackSlice memoryMaps = params.get(Utils::useMemoryMapsKey);
      memoryMaps.isBoolean()) {
    // update from user config if specified there
    _useMemoryMaps = memoryMaps.getBool();
  }

  VPackSlice userParams = params.get(Utils::userParametersKey);

  // parallelism
  _parallelism = WorkerConfig::parallelism(feature, userParams);

  // list of all shards, equal on all workers. Used to avoid storing strings of
  // shard names
  // Instead we have an index identifying a shard name in this vector
  PregelShard i = 0;
  for (VPackSlice shard : VPackArrayIterator(globalShards)) {
    ShardID s = shard.copyString();
    _globalShardIDs.push_back(s);
    _pregelShardIDs.try_emplace(s, i++);  // Cache these ids
  }

  // To access information based on a user defined collection name we need the
  for (auto it : VPackObjectIterator(collectionPlanIdMap)) {
    _collectionPlanIdMap.try_emplace(it.key.copyString(),
                                     it.value.copyString());
  }

  // Ordered list of shards for each vertex collection on the CURRENT db server
  // Order matters because the for example the third vertex shard, will only
  // every have
  // edges in the third edge shard. This should speed up the startup
  for (auto pair : VPackObjectIterator(vertexShardMap)) {
    CollectionID cname = pair.key.copyString();

    std::vector<ShardID> shards;
    for (VPackSlice shardSlice : VPackArrayIterator(pair.value)) {
      ShardID shard = shardSlice.copyString();
      shards.push_back(shard);
      _localVertexShardIDs.push_back(shard);
      _localPregelShardIDs.insert(_pregelShardIDs[shard]);
      _localPShardIDs_hash.insert(_pregelShardIDs[shard]);
      _shardToCollectionName.try_emplace(shard, cname);
    }
    _vertexCollectionShards.try_emplace(std::move(cname), std::move(shards));
  }

  // Ordered list of edge shards for each collection
  for (auto pair : VPackObjectIterator(edgeShardMap)) {
    CollectionID cname = pair.key.copyString();

    std::vector<ShardID> shards;
    for (VPackSlice shardSlice : VPackArrayIterator(pair.value)) {
      ShardID shard = shardSlice.copyString();
      shards.push_back(shard);
      _localEdgeShardIDs.push_back(shard);
      _shardToCollectionName.try_emplace(shard, cname);
    }
    _edgeCollectionShards.try_emplace(std::move(cname), std::move(shards));
  }

  if (edgeCollectionRestrictions.isObject()) {
    for (auto pair : VPackObjectIterator(edgeCollectionRestrictions)) {
      CollectionID cname = pair.key.copyString();

      std::vector<ShardID> shards;
      for (VPackSlice shardSlice : VPackArrayIterator(pair.value)) {
        shards.push_back(shardSlice.copyString());
      }
      _edgeCollectionRestrictions.try_emplace(std::move(cname),
                                              std::move(shards));
    }
  }
}

size_t WorkerConfig::parallelism(PregelFeature& feature, VPackSlice params) {
  // start off with default parallelism
  size_t parallelism = feature.defaultParallelism();
  if (params.isObject()) {
    // then update parallelism value from user config
    if (VPackSlice parallel = params.get(Utils::parallelismKey);
        parallel.isInteger()) {
      // limit parallelism to configured bounds
      parallelism =
          std::clamp(parallel.getNumber<size_t>(), feature.minParallelism(),
                     feature.maxParallelism());
    }
  }
  return parallelism;
}

std::vector<ShardID> const& WorkerConfig::edgeCollectionRestrictions(
    ShardID const& shard) const {
  auto it = _edgeCollectionRestrictions.find(shard);
  if (it != _edgeCollectionRestrictions.end()) {
    return (*it).second;
  }
  return ::emptyEdgeCollectionRestrictions;
}

PregelID WorkerConfig::documentIdToPregel(std::string const& documentID) const {
  size_t pos = documentID.find("/");
  if (pos == std::string::npos) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   "not a valid document id");
  }
  std::string_view docRef(documentID);
  std::string_view collPart = docRef.substr(0, pos);
  std::string_view keyPart = docRef.substr(pos + 1);

  ShardID responsibleShard;

  auto& ci = _vocbase->server().getFeature<ClusterFeature>().clusterInfo();
  Utils::resolveShard(ci, this, std::string(collPart), StaticStrings::KeyString,
                      keyPart, responsibleShard);

  PregelShard source = this->shardId(responsibleShard);
  return PregelID(source, std::string(keyPart));
}
