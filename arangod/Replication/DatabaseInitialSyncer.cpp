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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "DatabaseInitialSyncer.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/Exceptions.h"
#include "Basics/HybridLogicalClock.h"
#include "Basics/ReadLocker.h"
#include "Basics/Result.h"
#include "Basics/RocksDBUtils.h"
#include "Basics/ScopeGuard.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringBuffer.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/system-functions.h"
#include "Indexes/Index.h"
#include "IResearch/IResearchAnalyzerFeature.h"
#include "IResearch/IResearchCommon.h"
#include "Logger/Logger.h"
#include "Network/Methods.h"
#include "Network/NetworkFeature.h"
#include "Replication/DatabaseReplicationApplier.h"
#include "Replication/GlobalReplicationApplier.h"
#include "Replication/ReplicationFeature.h"
#include "Replication/utilities.h"
#include "Rest/CommonDefines.h"
#include "RestHandler/RestReplicationHandler.h"
#include "SimpleHttpClient/SimpleHttpClient.h"
#include "SimpleHttpClient/SimpleHttpResult.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/PhysicalCollection.h"
#include "StorageEngine/StorageEngine.h"
#include "StorageEngine/TransactionState.h"
#include "Transaction/Helpers.h"
#include "Transaction/Methods.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/CollectionGuard.h"
#include "Utils/OperationOptions.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/LogicalView.h"
#include "VocBase/ManagedDocumentResult.h"
#include "VocBase/voc-types.h"
#include "VocBase/vocbase.h"

#include <velocypack/Builder.h>
#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>
#include <velocypack/Validator.h>
#include <array>
#include <cstring>

namespace {

/// @brief maximum internal value for chunkSize
size_t const maxChunkSize = 10 * 1024 * 1024;

std::chrono::milliseconds sleepTimeFromWaitTime(double waitTime) {
  if (waitTime < 1.0) {
    return std::chrono::milliseconds(100);
  }
  if (waitTime < 5.0) {
    return std::chrono::milliseconds(200);
  }
  if (waitTime < 20.0) {
    return std::chrono::milliseconds(500);
  }
  if (waitTime < 60.0) {
    return std::chrono::seconds(1);
  }

  return std::chrono::seconds(2);
}

bool isVelocyPack(arangodb::httpclient::SimpleHttpResult const& response) {
  bool found = false;
  std::string const& cType = response.getHeaderField(
      arangodb::StaticStrings::ContentTypeHeader, found);
  return found && cType == arangodb::StaticStrings::MimeTypeVPack;
}

std::string const kTypeString = "type";
std::string const kDataString = "data";

arangodb::Result removeRevisions(
    arangodb::transaction::Methods& trx,
    arangodb::LogicalCollection& collection,
    std::vector<arangodb::RevisionId>& toRemove,
    arangodb::ReplicationMetricsFeature::InitialSyncStats& stats) {
  using arangodb::PhysicalCollection;
  using arangodb::Result;

  if (!toRemove.empty()) {
    PhysicalCollection* physical = collection.getPhysical();

    arangodb::OperationOptions options;
    options.silent = true;
    options.ignoreRevs = true;
    options.isRestore = true;
    options.waitForSync = false;

    arangodb::transaction::BuilderLeaser tempBuilder(&trx);

    double t = TRI_microtime();
    for (auto const& rid : toRemove) {
      auto documentId = arangodb::LocalDocumentId::create(rid);

      tempBuilder->clear();
      auto r = physical->lookupDocument(trx, documentId, *tempBuilder,
                                        /*readCache*/ true, /*fillCache*/ false,
                                        arangodb::ReadOwnWrites::yes);

      if (r.ok()) {
        r = physical->remove(trx, documentId, rid, tempBuilder->slice(),
                             options);
      }

      if (r.ok()) {
        ++stats.numDocsRemoved;
      } else if (r.isNot(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND)) {
        // ignore not found, we remove conflicting docs ahead of time
        stats.waitedForRemovals += TRI_microtime() - t;
        return r;
      }
    }

    stats.waitedForRemovals += TRI_microtime() - t;
  }

  return {};
}

arangodb::Result fetchRevisions(
    arangodb::NetworkFeature& netFeature, arangodb::transaction::Methods& trx,
    arangodb::DatabaseInitialSyncer::Configuration& config,
    arangodb::Syncer::SyncerState& state,
    arangodb::LogicalCollection& collection, std::string const& leader,
    std::vector<arangodb::RevisionId>& toFetch,
    arangodb::ReplicationMetricsFeature::InitialSyncStats& stats) {
  using arangodb::PhysicalCollection;
  using arangodb::RestReplicationHandler;
  using arangodb::Result;
  using arangodb::basics::StringUtils::concatT;

  if (toFetch.empty()) {
    return Result();  // nothing to do
  }

  arangodb::OperationOptions options;
  options.silent = true;
  options.ignoreRevs = true;
  options.isRestore = true;
  options.validate = false;  // no validation during replication
  options.indexOperationMode = arangodb::IndexOperationMode::internal;
  options.checkUniqueConstraintsInPreflight = true;
  options.waitForSync = false;  // no waitForSync during replication
  if (!state.leaderId.empty()) {
    options.isSynchronousReplicationFrom = state.leaderId;
  }

  PhysicalCollection* physical = collection.getPhysical();

  std::string path = arangodb::replutils::ReplicationUrl + "/" +
                     RestReplicationHandler::Revisions + "/" +
                     RestReplicationHandler::Documents;
  auto headers = arangodb::replutils::createHeaders();

  config.progress.set("fetching documents by revision for collection '" +
                      collection.name() + "' from " + path);

  arangodb::transaction::BuilderLeaser tempBuilder(&trx);

  auto removeConflict = [&](auto const& conflictingKey) -> Result {
    std::pair<arangodb::LocalDocumentId, arangodb::RevisionId> lookupResult;
    auto r = physical->lookupKey(&trx, conflictingKey, lookupResult,
                                 arangodb::ReadOwnWrites::yes);

    if (r.ok()) {
      TRI_ASSERT(lookupResult.first.isSet());
      TRI_ASSERT(lookupResult.second.isSet());
      auto [documentId, revisionId] = lookupResult;

      tempBuilder->clear();
      r = physical->lookupDocument(trx, documentId, *tempBuilder,
                                   /*readCache*/ true, /*fillCache*/ false,
                                   arangodb::ReadOwnWrites::yes);

      if (r.ok()) {
        TRI_ASSERT(tempBuilder->slice().isObject());
        r = physical->remove(trx, documentId, revisionId, tempBuilder->slice(),
                             options);
      }
    }

    // if a conflict document cannot be removed because it doesn't exist,
    // we do not care, because the goal is deletion anyway. if it fails
    // for some other reason, the following re-insert will likely complain.
    // so intentionally no special error handling here.
    if (r.ok()) {
      ++stats.numDocsRemoved;
    }

    return r;
  };

  std::size_t numUniqueIndexes = [&]() {
    std::size_t numUnique = 0;
    for (auto const& idx : collection.getIndexes()) {
      numUnique += idx->unique() ? 1 : 0;
    }
    return numUnique;
  }();

  std::size_t current = 0;
  auto guard = arangodb::scopeGuard(
      [&current, &stats]() noexcept { stats.numDocsRequested += current; });

  char ridBuffer[arangodb::basics::maxUInt64StringSize];
  std::deque<arangodb::network::FutureRes> futures;
  std::deque<std::unordered_set<arangodb::RevisionId>> shoppingLists;

  arangodb::network::ConnectionPool* pool = netFeature.pool();

  std::size_t queueSize = 10;
  if ((config.leader.majorVersion < 3) ||
      (config.leader.majorVersion == 3 && config.leader.minorVersion < 9) ||
      (config.leader.majorVersion == 3 && config.leader.minorVersion == 9 &&
       config.leader.patchVersion < 1)) {
    queueSize = 1;
  }
  while (current < toFetch.size() || !futures.empty()) {
    // Send some requests off if not enough in flight and something to go
    while (futures.size() < queueSize && current < toFetch.size()) {
      VPackBuilder requestBuilder;
      std::unordered_set<arangodb::RevisionId> shoppingList;
      uint64_t count = 0;
      {
        VPackArrayBuilder list(&requestBuilder);
        std::size_t i;
        for (i = 0; i < 5000 && current + i < toFetch.size(); ++i) {
          requestBuilder.add(toFetch[current + i].toValuePair(ridBuffer));
          shoppingList.insert(toFetch[current + i]);
          ++count;
        }
        current += i;
      }

      arangodb::network::RequestOptions reqOptions;
      reqOptions.param("collection", leader)
          .param("serverId", state.localServerIdString)
          .param("batchId", std::to_string(config.batch.id));
      reqOptions.database = config.vocbase.name();
      reqOptions.timeout = arangodb::network::Timeout(25.0);
      auto buffer = requestBuilder.steal();
      auto f = arangodb::network::sendRequestRetry(
          pool, config.leader.endpoint, arangodb::fuerte::RestVerb::Put, path,
          std::move(*buffer), reqOptions);
      futures.emplace_back(std::move(f));
      shoppingLists.emplace_back(std::move(shoppingList));
      ++stats.numDocsRequests;
      LOG_TOPIC("eda42", DEBUG, arangodb::Logger::REPLICATION)
          << "Have requested a chunk of " << count << " revision(s) from "
          << config.leader.serverId << " at " << config.leader.endpoint
          << " for collection " << leader
          << " length of queue: " << futures.size();
    }

    if (!futures.empty()) {
      TRI_ASSERT(futures.size() == shoppingLists.size());
      auto& f = futures.front();
      double tWait = TRI_microtime();
      auto& val = f.get();
      stats.waitedForDocs += TRI_microtime() - tWait;
      Result res = val.combinedResult();
      if (res.fail()) {
        return Result(
            TRI_ERROR_REPLICATION_INVALID_RESPONSE,
            concatT("got invalid response from leader at ",
                    config.leader.endpoint, path, ": ", res.errorMessage()));
      }

      VPackSlice docs = val.slice();
      if (!docs.isArray()) {
        return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                      concatT("got invalid response from leader at ",
                              config.leader.endpoint, path,
                              ": response is not an array"));
      }

      config.progress.set("applying documents by revision for collection '" +
                          collection.name() + "'");

      auto& sl = shoppingLists.front();  // corresponds to future
      for (VPackSlice leaderDoc : VPackArrayIterator(docs)) {
        if (!leaderDoc.isObject()) {
          return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                        std::string("got invalid response from leader at ") +
                            config.leader.endpoint + path +
                            ": response document entry is not an object");
        }

        VPackSlice keySlice = leaderDoc.get(arangodb::StaticStrings::KeyString);
        if (!keySlice.isString()) {
          return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                        std::string("got invalid response from leader at ") +
                            state.leader.endpoint +
                            ": document key is invalid");
        }

        VPackSlice revSlice = leaderDoc.get(arangodb::StaticStrings::RevString);
        if (!revSlice.isString()) {
          return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                        std::string("got invalid response from leader at ") +
                            state.leader.endpoint +
                            ": document revision is invalid");
        }

        options.indexOperationMode = arangodb::IndexOperationMode::internal;

        // we need a retry loop here for unique indexes (we will always have at
        // least one unique index, which is the primary index, but there can be
        // more). as documents can be presented in any state on the follower,
        // simply inserting them in leader order may trigger a unique constraint
        // violation on the follower. in this case we may need to remove the
        // conflicting document. this can happen multiple times if there are
        // multiple unique indexes! we can only stop trying once we have tried
        // often enough, or if inserting succeeds.
        std::size_t tries = 1 + numUniqueIndexes;
        while (tries-- > 0) {
          if (tries == 0) {
            options.indexOperationMode = arangodb::IndexOperationMode::normal;
          }

          arangodb::RevisionId rid = arangodb::RevisionId::fromSlice(leaderDoc);

          double tInsert = TRI_microtime();
          Result res = trx.insert(collection.name(), leaderDoc, options).result;
          stats.waitedForInsertions += TRI_microtime() - tInsert;

          options.indexOperationMode = arangodb::IndexOperationMode::internal;

          // We must see our own writes, because we may have to remove
          if (res.ok()) {
            sl.erase(rid);
            ++stats.numDocsInserted;
            break;
          }

          if (!res.is(TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED)) {
            auto errorNumber = res.errorNumber();
            res.reset(errorNumber, concatT(TRI_errno_string(errorNumber), ": ",
                                           res.errorMessage()));
            return res;
          }

          // conflicting documents (that we just inserted), as documents may be
          // replicated in unexpected order.
          arangodb::ManagedDocumentResult mdr;
          if (physical->readDocument(&trx, arangodb::LocalDocumentId(rid.id()),
                                     mdr, arangodb::ReadOwnWrites::yes)) {
            // already have exactly this revision. no need to insert
            sl.erase(rid);
            break;
          }

          // remove conflict and retry
          // errorMessage() is this case contains the conflicting key
          auto inner = removeConflict(res.errorMessage());
          if (inner.fail()) {
            return res;
          }
        }
      }
      // Here we expect that a lot if not all of our shopping list has come
      // back from the leader and the shopping list is empty, however, it
      // is possible that this is not the case, since the leader reserves
      // the right to send fewer documents. In this case, we have to order
      // another batch:
      if (!sl.empty()) {
        // Take out the nonempty list:
        std::unordered_set<arangodb::RevisionId> newList = std::move(sl);
        // Sort in ascending order to speed up iterator lookup on leader:
        std::vector<arangodb::RevisionId> v;
        v.reserve(newList.size());
        for (auto it = newList.begin(); it != newList.end(); ++it) {
          v.push_back(*it);
        }
        std::sort(v.begin(), v.end());
        VPackBuilder requestBuilder;
        {
          VPackArrayBuilder list(&requestBuilder);
          for (auto const& r : v) {
            requestBuilder.add(r.toValuePair(ridBuffer));
          }
        }

        arangodb::network::RequestOptions reqOptions;
        reqOptions.param("collection", leader)
            .param("serverId", state.localServerIdString)
            .param("batchId", std::to_string(config.batch.id));
        reqOptions.timeout = arangodb::network::Timeout(25.0);
        reqOptions.database = config.vocbase.name();
        auto buffer = requestBuilder.steal();
        auto f = arangodb::network::sendRequestRetry(
            pool, config.leader.endpoint, arangodb::fuerte::RestVerb::Put, path,
            std::move(*buffer), reqOptions);
        futures.emplace_back(std::move(f));
        shoppingLists.emplace_back(std::move(newList));
        ++stats.numDocsRequests;
        LOG_TOPIC("eda45", DEBUG, arangodb::Logger::REPLICATION)
            << "Have re-requested a chunk of " << shoppingLists.back().size()
            << " revision(s) from " << config.leader.serverId << " at "
            << config.leader.endpoint << " for collection " << leader
            << " queue length: " << futures.size();
      }
      LOG_TOPIC("eda44", DEBUG, arangodb::Logger::REPLICATION)
          << "Have applied a chunk of " << docs.length() << " documents from "
          << config.leader.serverId << " at " << config.leader.endpoint
          << " for collection " << leader
          << " queue length: " << futures.size();
      futures.pop_front();
      shoppingLists.pop_front();
      TRI_ASSERT(futures.size() == shoppingLists.size());
      res = trx.state()->performIntermediateCommitIfRequired(collection.id());
      if (res.fail()) {
        return res;
      }
    }
  }

  return Result();
}
}  // namespace

namespace arangodb {

/// @brief helper struct to prevent multiple starts of the replication
/// in the same database. used for single-server replication only.
struct MultiStartPreventer {
  MultiStartPreventer(MultiStartPreventer const&) = delete;
  MultiStartPreventer& operator=(MultiStartPreventer const&) = delete;

  MultiStartPreventer(TRI_vocbase_t& vocbase, bool preventStart)
      : vocbase(vocbase), preventedStart(false) {
    if (preventStart) {
      TRI_ASSERT(!ServerState::instance()->isClusterRole());

      auto res = vocbase.replicationApplier()->preventStart();
      if (res.fail()) {
        THROW_ARANGO_EXCEPTION(res);
      }
      preventedStart = true;
    }
  }

  ~MultiStartPreventer() {
    if (preventedStart) {
      // reallow starting
      TRI_ASSERT(!ServerState::instance()->isClusterRole());
      vocbase.replicationApplier()->allowStart();
    }
  }

  TRI_vocbase_t& vocbase;
  bool preventedStart;
};

DatabaseInitialSyncer::Configuration::Configuration(
    ReplicationApplierConfiguration const& a, replutils::BatchInfo& bat,
    replutils::Connection& c, bool f, replutils::LeaderInfo& l,
    replutils::ProgressInfo& p, SyncerState& s, TRI_vocbase_t& v)
    : applier{a},
      batch{bat},
      connection{c},
      flushed{f},
      leader{l},
      progress{p},
      state{s},
      vocbase{v} {}

bool DatabaseInitialSyncer::Configuration::isChild() const noexcept {
  return state.isChildSyncer;
}

DatabaseInitialSyncer::DatabaseInitialSyncer(
    TRI_vocbase_t& vocbase,
    ReplicationApplierConfiguration const& configuration)
    : InitialSyncer(
          configuration,
          [this](std::string const& msg) -> void { setProgress(msg); }),
      _config{_state.applier, _batch,        _state.connection,
              false,          _state.leader, _progress,
              _state,         vocbase},
      _lastCancellationCheck(std::chrono::steady_clock::now()),
      _isClusterRole(ServerState::instance()->isClusterRole()),
      _quickKeysNumDocsLimit(
          vocbase.server().getFeature<ReplicationFeature>().quickKeysLimit()) {
  _state.vocbases.try_emplace(vocbase.name(), vocbase);

#ifdef ARANGODB_ENABLE_FAILURE_TESTS
  adjustQuickKeysNumDocsLimit();
#endif
  if (configuration._database.empty()) {
    _state.databaseName = vocbase.name();
  }
}

std::shared_ptr<DatabaseInitialSyncer> DatabaseInitialSyncer::create(
    TRI_vocbase_t& vocbase,
    ReplicationApplierConfiguration const& configuration) {
  // enable make_shared on a class with a private constructor
  struct Enabler final : public DatabaseInitialSyncer {
    Enabler(TRI_vocbase_t& vocbase,
            ReplicationApplierConfiguration const& configuration)
        : DatabaseInitialSyncer(vocbase, configuration) {}
  };

  return std::make_shared<Enabler>(vocbase, configuration);
}

/// @brief return information about the leader
replutils::LeaderInfo DatabaseInitialSyncer::leaderInfo() const {
  return _config.leader;
}

/// @brief run method, performs a full synchronization
Result DatabaseInitialSyncer::runWithInventory(bool incremental,
                                               VPackSlice dbInventory,
                                               char const* context) {
  if (!_config.connection.valid()) {
    return Result(TRI_ERROR_INTERNAL, "invalid endpoint");
  }

  double const startTime = TRI_microtime();

  try {
    bool const preventMultiStart = !_isClusterRole;
    MultiStartPreventer p(vocbase(), preventMultiStart);

    setAborted(false);

    _config.progress.set("fetching leader state");

    LOG_TOPIC("0a10d", DEBUG, Logger::REPLICATION)
        << "client: getting leader state to dump " << vocbase().name();

    Result r;
    if (!_config.isChild()) {
      // enable patching of collection count for ShardSynchronization Job
      std::string patchCount;
      if (_config.applier._skipCreateDrop &&
          _config.applier._restrictType ==
              ReplicationApplierConfiguration::RestrictType::Include &&
          _config.applier._restrictCollections.size() == 1) {
        patchCount = *_config.applier._restrictCollections.begin();
      }

      // with a 3.8 leader, this call combines fetching the leader state with
      // starting the batch. this saves us one request per shard. a 3.7 leader
      // will does not return the leader state together with this call, so we
      // need to be prepared for not yet getting it here
      r = batchStart(context, patchCount);

      if (r.ok() && !_config.leader.serverId.isSet()) {
        // a 3.7 leader, which does not return leader state when stating a
        // batch. so we need to fetch the leader state in addition
        r = _config.leader.getState(_config.connection, _config.isChild(),
                                    context);
      }

      if (r.fail()) {
        return r;
      }

      TRI_ASSERT(!_config.leader.endpoint.empty());
      TRI_ASSERT(_config.leader.serverId.isSet());
      TRI_ASSERT(_config.leader.majorVersion != 0);

      LOG_TOPIC("6fd2b", DEBUG, Logger::REPLICATION)
          << "client: got leader state";

      startRecurringBatchExtension();
    }

    VPackSlice collections, views;
    if (dbInventory.isObject()) {
      collections = dbInventory.get("collections");  // required
      views = dbInventory.get("views");              // optional
    }
    VPackBuilder inventoryResponse;  // hold response data
    if (!collections.isArray()) {
      // caller did not supply an inventory, we need to fetch it
      r = fetchInventory(inventoryResponse);
      if (!r.ok()) {
        return r;
      }
      // we do not really care about the state response
      collections = inventoryResponse.slice().get("collections");
      if (!collections.isArray()) {
        return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                      "collections section is missing from response");
      }
      views = inventoryResponse.slice().get("views");
    }

    // strip eventual objectIDs and then dump the collections
    auto pair = rocksutils::stripObjectIds(collections);
    r = handleCollectionsAndViews(pair.first, views, incremental);

    if (r.fail()) {
      LOG_TOPIC("12556", DEBUG, Logger::REPLICATION)
          << "Error during initial sync: " << r.errorMessage();
    }

    LOG_TOPIC("055df", DEBUG, Logger::REPLICATION)
        << "initial synchronization with leader took: "
        << Logger::FIXED(TRI_microtime() - startTime, 6) << " s. status: "
        << (r.errorMessage().empty() ? "all good" : r.errorMessage());

    if (r.ok() && _onSuccess) {
      r = _onSuccess(*this);
    }

    return r;
  } catch (arangodb::basics::Exception const& ex) {
    return Result(ex.code(), ex.what());
  } catch (std::exception const& ex) {
    return Result(TRI_ERROR_INTERNAL, ex.what());
  } catch (...) {
    return Result(TRI_ERROR_INTERNAL, "an unknown exception occurred");
  }
}

/// @brief fetch the server's inventory, public method for TailingSyncer
Result DatabaseInitialSyncer::getInventory(VPackBuilder& builder) {
  if (!_state.connection.valid()) {
    return Result(TRI_ERROR_INTERNAL, "invalid endpoint");
  }

  auto r = batchStart(nullptr);
  if (r.fail()) {
    return r;
  }

  auto sg = arangodb::scopeGuard([&]() noexcept { batchFinish(); });

  // caller did not supply an inventory, we need to fetch it
  return fetchInventory(builder);
}

/// @brief check whether the initial synchronization should be aborted
bool DatabaseInitialSyncer::isAborted() const {
  if (vocbase().server().isStopping() ||
      (vocbase().replicationApplier() != nullptr &&
       vocbase().replicationApplier()->stopInitialSynchronization())) {
    return true;
  }

  if (_state.isChildSyncer) {
    // this syncer is used as a child syncer of the GlobalInitialSyncer.
    // now check if parent was aborted
    ReplicationFeature& replication =
        vocbase().server().getFeature<ReplicationFeature>();
    GlobalReplicationApplier* applier = replication.globalReplicationApplier();
    if (applier != nullptr && applier->stopInitialSynchronization()) {
      return true;
    }
  }

  if (_checkCancellation) {
    // execute custom check for abortion only every few seconds, in case
    // it is expensive
    constexpr auto checkFrequency = std::chrono::seconds(5);

    auto now = std::chrono::steady_clock::now();
    TRI_IF_FAILURE("Replication::forceCheckCancellation") {
      // always force the cancellation check!
      _lastCancellationCheck = now - checkFrequency;
    }

    if (now - _lastCancellationCheck >= checkFrequency) {
      _lastCancellationCheck = now;
      if (_checkCancellation()) {
        return true;
      }
    }
  }

  return Syncer::isAborted();
}

void DatabaseInitialSyncer::setProgress(std::string const& msg) {
  _config.progress.message = msg;

  if (_config.applier._verbose) {
    LOG_TOPIC("c6f5f", INFO, Logger::REPLICATION) << msg;
  } else {
    LOG_TOPIC("d15ed", DEBUG, Logger::REPLICATION) << msg;
  }

  if (!_isClusterRole) {
    auto* applier = _config.vocbase.replicationApplier();

    if (applier != nullptr) {
      applier->setProgress(msg);
    }
  }
}

/// @brief handle a single dump marker
Result DatabaseInitialSyncer::parseCollectionDumpMarker(
    transaction::Methods& trx, LogicalCollection* coll, VPackSlice marker,
    FormatHint& hint) {
  if (!ADB_LIKELY(marker.isObject())) {
    return TRI_ERROR_REPLICATION_INVALID_RESPONSE;
  }

  // format auto-detection. this is executed only once per batch
  if (hint == FormatHint::AutoDetect) {
    if (marker.get(StaticStrings::KeyString).isString()) {
      // _key present
      hint = FormatHint::NoEnvelope;
    } else if (marker.get(kDataString).isObject()) {
      hint = FormatHint::Envelope;
    } else {
      return TRI_ERROR_REPLICATION_INVALID_RESPONSE;
    }
  }

  TRI_ASSERT(hint == FormatHint::Envelope || hint == FormatHint::NoEnvelope);

  VPackSlice doc;
  TRI_replication_operation_e type = REPLICATION_INVALID;

  if (hint == FormatHint::Envelope) {
    // input is wrapped in a {"type":2300,"data":{...}} envelope
    VPackSlice s = marker.get(kTypeString);
    if (s.isNumber()) {
      type = static_cast<TRI_replication_operation_e>(s.getNumber<int>());
    }
    doc = marker.get(kDataString);
    if (!doc.isObject()) {
      return TRI_ERROR_REPLICATION_INVALID_RESPONSE;
    }
  } else if (hint == FormatHint::NoEnvelope) {
    // input is just a document, without any {"type":2300,"data":{...}} envelope
    type = REPLICATION_MARKER_DOCUMENT;
    doc = marker;
  }

  if (!ADB_LIKELY(doc.isObject())) {
    return TRI_ERROR_REPLICATION_INVALID_RESPONSE;
  }

  std::string conflictingDocumentKey;
  return applyCollectionDumpMarker(trx, coll, type, doc,
                                   conflictingDocumentKey);
}

/// @brief apply the data from a collection dump
Result DatabaseInitialSyncer::parseCollectionDump(
    transaction::Methods& trx, LogicalCollection* coll,
    httpclient::SimpleHttpResult* response, uint64_t& markersProcessed) {
  TRI_ASSERT(response != nullptr);
  TRI_ASSERT(!trx.isSingleOperationTransaction());

  FormatHint hint = FormatHint::AutoDetect;

  basics::StringBuffer const& data = response->getBody();
  char const* p = data.begin();
  char const* end = p + data.length();

  if (isVelocyPack(*response)) {
    // received a velocypack response from the leader

    // intentional copy
    VPackOptions validationOptions =
        basics::VelocyPackHelper::strictRequestValidationOptions;

    // allow custom types being sent here
    validationOptions.disallowCustom = false;

    VPackValidator validator(&validationOptions);

    // now check the sub-format of the velocypack data we received...
    VPackSlice s(reinterpret_cast<uint8_t const*>(p));

    if (s.isArray()) {
      // got one big velocypack array with all documents in it.
      // servers >= 3.10 will send this if we send the "array=true" request
      // parameter. older versions are not able to send this format, but will
      // send each document as a single velocypack slice.

      size_t remaining = static_cast<size_t>(end - p);
      // throws if the data is invalid
      validator.validate(p, remaining, /*isSubPart*/ false);

      markersProcessed += s.length();

      OperationOptions options;
      options.silent = true;
      options.ignoreRevs = true;
      options.isRestore = true;
      options.validate = false;
      options.returnOld = false;
      options.returnNew = false;
      options.checkUniqueConstraintsInPreflight = false;
      options.isSynchronousReplicationFrom = _state.leaderId;

      auto opRes = trx.insert(coll->name(), s, options);
      if (opRes.fail()) {
        return opRes.result;
      }

      VPackSlice resultSlice = opRes.slice();
      if (resultSlice.isArray()) {
        for (VPackSlice it : VPackArrayIterator(resultSlice)) {
          VPackSlice s = it.get(StaticStrings::Error);
          if (!s.isTrue()) {
            continue;
          }
          // found an error
          auto errorCode =
              ErrorCode{it.get(StaticStrings::ErrorNum).getNumber<int>()};
          VPackSlice msg = it.get(StaticStrings::ErrorMessage);
          return Result(errorCode, msg.copyString());
        }
      }

    } else {
      // received a VelocyPack response from the leader, with one document
      // per slice (multiple slices in the same response)
      try {
        while (p < end) {
          size_t remaining = static_cast<size_t>(end - p);
          // throws if the data is invalid
          validator.validate(p, remaining, /*isSubPart*/ true);

          VPackSlice marker(reinterpret_cast<uint8_t const*>(p));
          Result r = parseCollectionDumpMarker(trx, coll, marker, hint);

          TRI_ASSERT(!r.is(TRI_ERROR_ARANGO_TRY_AGAIN));
          if (r.fail()) {
            r.reset(r.errorNumber(),
                    std::string("received invalid dump data for collection '") +
                        coll->name() + "'");
            return r;
          }
          ++markersProcessed;
          p += marker.byteSize();
        }
      } catch (velocypack::Exception const& e) {
        LOG_TOPIC("b9f4f", ERR, Logger::REPLICATION)
            << "Error parsing VPack response: " << e.what();
        return Result(TRI_ERROR_HTTP_CORRUPTED_JSON, e.what());
      }
    }
  } else {
    // received a JSONL response from the leader, with one document per line
    // buffer must end with a NUL byte
    TRI_ASSERT(*end == '\0');
    LOG_TOPIC("bad5d", DEBUG, Logger::REPLICATION)
        << "using json for chunk contents";

    VPackBuilder builder;
    VPackParser parser(
        builder, &basics::VelocyPackHelper::strictRequestValidationOptions);

    while (p < end) {
      char const* q = strchr(p, '\n');
      if (q == nullptr) {
        q = end;
      }

      if (q - p < 2) {
        // we are done
        return Result();
      }

      TRI_ASSERT(q <= end);
      try {
        builder.clear();
        parser.parse(p, static_cast<size_t>(q - p));
      } catch (velocypack::Exception const& e) {
        LOG_TOPIC("746ea", ERR, Logger::REPLICATION)
            << "while parsing collection dump: " << e.what();
        return Result(TRI_ERROR_HTTP_CORRUPTED_JSON, e.what());
      }

      p = q + 1;

      Result r = parseCollectionDumpMarker(trx, coll, builder.slice(), hint);
      TRI_ASSERT(!r.is(TRI_ERROR_ARANGO_TRY_AGAIN));
      if (r.fail()) {
        return r;
      }

      ++markersProcessed;
    }
  }

  // reached the end
  return Result();
}

/// @brief order a new chunk from the /dump API
void DatabaseInitialSyncer::fetchDumpChunk(
    std::shared_ptr<Syncer::JobSynchronizer> sharedStatus,
    std::string const& baseUrl, arangodb::LogicalCollection* coll,
    std::string const& leaderColl, int batch, TRI_voc_tick_t fromTick,
    uint64_t chunkSize) {
  using ::arangodb::basics::StringUtils::itoa;

  if (isAborted()) {
    sharedStatus->gotResponse(Result(TRI_ERROR_REPLICATION_APPLIER_STOPPED));
    return;
  }

  try {
    std::string const typeString =
        (coll->type() == TRI_COL_TYPE_EDGE ? "edge" : "document");

    if (!_config.isChild()) {
      batchExtend();
    }

    // assemble URL to call
    std::string url =
        baseUrl + "&from=" + itoa(fromTick) + "&chunkSize=" + itoa(chunkSize);

    if (_config.flushed) {
      url += "&flush=false";
    } else {
      // only flush WAL once
      url += "&flush=true";
      _config.flushed = true;
    }

    bool isVPack = false;
    auto headers = replutils::createHeaders();
    if (_config.leader.version() >= 30800) {
      // from 3.8 onwards, it is safe and also faster to retrieve vpack-encoded
      // dumps. in previous versions there may be vpack encoding issues for the
      // /_api/replication/dump responses.
      headers[StaticStrings::Accept] = StaticStrings::MimeTypeVPack;
      isVPack = true;
    }

    _config.progress.set(
        std::string("fetching leader collection dump for collection '") +
        coll->name() + "', type: " + typeString +
        ", format: " + (isVPack ? "vpack" : "json") + ", id: " + leaderColl +
        ", batch " + itoa(batch) + ", url: " + url);

    double t = TRI_microtime();

    // send request
    std::unique_ptr<httpclient::SimpleHttpResult> response;
    _config.connection.lease([&](httpclient::SimpleHttpClient* client) {
      response.reset(client->retryRequest(rest::RequestType::GET, url, nullptr,
                                          0, headers));
    });

    t = TRI_microtime() - t;

    if (replutils::hasFailed(response.get())) {
      sharedStatus->gotResponse(
          replutils::buildHttpError(response.get(), url, _config.connection),
          t);
      return;
    }

    // success!
    sharedStatus->gotResponse(std::move(response), t);
  } catch (basics::Exception const& ex) {
    sharedStatus->gotResponse(Result(ex.code(), ex.what()));
  } catch (std::exception const& ex) {
    sharedStatus->gotResponse(Result(TRI_ERROR_INTERNAL, ex.what()));
  }
}

/// @brief incrementally fetch data from a collection
Result DatabaseInitialSyncer::fetchCollectionDump(
    arangodb::LogicalCollection* coll, std::string const& leaderColl,
    TRI_voc_tick_t maxTick) {
  using ::arangodb::basics::StringUtils::boolean;
  using ::arangodb::basics::StringUtils::concatT;
  using ::arangodb::basics::StringUtils::itoa;
  using ::arangodb::basics::StringUtils::uint64;
  using ::arangodb::basics::StringUtils::urlEncode;

  if (isAborted()) {
    return Result(TRI_ERROR_REPLICATION_APPLIER_STOPPED);
  }

  std::string const typeString =
      (coll->type() == TRI_COL_TYPE_EDGE ? "edge" : "document");

  // statistics which will update the global replication metrics,
  // periodically reset
  ReplicationMetricsFeature::InitialSyncStats stats(
      vocbase().server().getFeature<ReplicationMetricsFeature>(), true);

  // local statistics that will be ever increasing inside this method
  ReplicationMetricsFeature::InitialSyncStats cumulativeStats(
      vocbase().server().getFeature<ReplicationMetricsFeature>(), false);

  TRI_ASSERT(_config.batch.id);  // should not be equal to 0

  // assemble base URL
  // note: the "useEnvelope" URL parameter is sent to signal the leader that
  // we don't need the dump data packaged in an extra {"type":2300,"data":{...}}
  // envelope per document.
  // older, incompatible leaders will simply ignore this parameter and will
  // still send the documents inside these envelopes. that means when we receive
  // the documents, we need to disambiguate the two different formats.
  std::string baseUrl =
      replutils::ReplicationUrl + "/dump?collection=" + urlEncode(leaderColl) +
      "&batchId=" + std::to_string(_config.batch.id) + "&includeSystem=" +
      std::string(_config.applier._includeSystem ? "true" : "false") +
      "&useEnvelope=false" + "&serverId=" + _state.localServerIdString;

  if (ServerState::instance()->isDBServer() && !_config.isChild() &&
      _config.applier._skipCreateDrop &&
      _config.applier._restrictType ==
          ReplicationApplierConfiguration::RestrictType::Include &&
      _config.applier._restrictCollections.size() == 1 &&
      !hasDocuments(*coll)) {
    // DB server doing shard synchronization. now try to fetch everything in a
    // single VPack array. note: only servers >= 3.10 will honor this URL
    // parameter. servers that are not capable of this format will simply ignore
    // it and send the old format. the syncer has code to tell the two formats
    // apart. note: we can only add this flag if we are sure there are no
    // documents present locally. everything else is not safe.
    baseUrl += "&array=true";
  }

  if (maxTick > 0) {
    baseUrl += "&to=" + itoa(maxTick + 1);
  }

  // state variables for the dump
  TRI_voc_tick_t fromTick = 0;
  int batch = 1;
  uint64_t chunkSize = _config.applier._chunkSize;

  double const startTime = TRI_microtime();

  // the shared status will wait in its destructor until all posted
  // requests have been completed/canceled!
  auto self = shared_from_this();
  auto sharedStatus = std::make_shared<Syncer::JobSynchronizer>(self);

  // order initial chunk. this will block until the initial response
  // has arrived
  fetchDumpChunk(sharedStatus, baseUrl, coll, leaderColl, batch, fromTick,
                 chunkSize);

  while (true) {
    std::unique_ptr<httpclient::SimpleHttpResult> dumpResponse;

    // block until we either got a response or were shut down
    Result res = sharedStatus->waitForResponse(dumpResponse);

    // update our statistics
    ++stats.numDumpRequests;
    stats.waitedForDump += sharedStatus->time();

    if (res.fail()) {
      // no response or error or shutdown
      return res;
    }

    // now we have got a response!
    TRI_ASSERT(dumpResponse != nullptr);

    if (dumpResponse->hasContentLength()) {
      stats.numDumpBytesReceived += dumpResponse->getContentLength();
    }

    bool found;
    std::string header = dumpResponse->getHeaderField(
        StaticStrings::ReplicationHeaderCheckMore, found);
    if (!found) {
      return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                    std::string("got invalid response from leader at ") +
                        _config.leader.endpoint + ": required header " +
                        StaticStrings::ReplicationHeaderCheckMore +
                        " is missing in dump response");
    }

    TRI_voc_tick_t tick;
    bool checkMore = boolean(header);

    if (checkMore) {
      header = dumpResponse->getHeaderField(
          StaticStrings::ReplicationHeaderLastIncluded, found);
      if (!found) {
        return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                      std::string("got invalid response from leader at ") +
                          _config.leader.endpoint + ": required header " +
                          StaticStrings::ReplicationHeaderLastIncluded +
                          " is missing in dump response");
      }

      tick = uint64(header);

      if (tick > fromTick) {
        fromTick = tick;
      } else {
        // we got the same tick again, this indicates we're at the end
        checkMore = false;
      }
    }

    // increase chunk size for next fetch
    if (chunkSize < ::maxChunkSize) {
      chunkSize = static_cast<uint64_t>(chunkSize * 1.25);

      if (chunkSize > ::maxChunkSize) {
        chunkSize = ::maxChunkSize;
      }
    }

    if (checkMore && !isAborted()) {
      // already fetch next batch in the background, by posting the
      // request to the scheduler, which can run it asynchronously
      sharedStatus->request([this, self, baseUrl, sharedStatus, coll,
                             leaderColl, batch, fromTick, chunkSize]() {
        TRI_IF_FAILURE("Replication::forceCheckCancellation") {
          // we intentionally sleep here for a while, so the next call gets
          // executed after the scheduling thread has thrown its
          // TRI_ERROR_INTERNAL exception for our failure point
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        fetchDumpChunk(sharedStatus, baseUrl, coll, leaderColl, batch + 1,
                       fromTick, chunkSize);
      });
      TRI_IF_FAILURE("Replication::forceCheckCancellation") {
        // forcefully abort replication once we have scheduled the job
        THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL);
      }
    }

    SingleCollectionTransaction trx(
        transaction::StandaloneContext::Create(vocbase()), *coll,
        AccessMode::Type::EXCLUSIVE);

    // do not index the operations in our own transaction
    trx.addHint(transaction::Hints::Hint::NO_INDEXING);

    res = trx.begin();

    if (!res.ok()) {
      return Result(res.errorNumber(), concatT("unable to start transaction: ",
                                               res.errorMessage()));
    }

    double t = TRI_microtime();
    TRI_ASSERT(!trx.isSingleOperationTransaction());
    res = parseCollectionDump(trx, coll, dumpResponse.get(),
                              stats.numDumpDocuments);

    if (res.fail()) {
      TRI_ASSERT(!res.is(TRI_ERROR_ARANGO_TRY_AGAIN));
      return res;
    }

    res = trx.commit();

    double applyTime = TRI_microtime() - t;
    stats.waitedForDumpApply += applyTime;

    cumulativeStats += stats;

    _config.progress.set(
        std::string("fetched leader collection dump for collection '") +
        coll->name() + "', type: " + typeString + ", id: " + leaderColl +
        ", batch " + itoa(batch) + ", markers processed so far: " +
        itoa(cumulativeStats.numDumpDocuments) + ", bytes received so far: " +
        itoa(cumulativeStats.numDumpBytesReceived) +
        ", apply time for batch: " + std::to_string(applyTime) + " s");

    if (!res.ok()) {
      return res;
    }

    if (!checkMore || fromTick == 0) {
      // done
      _config.progress.set(
          std::string("finished initial dump for collection '") + coll->name() +
          "', type: " + typeString + ", id: " + leaderColl +
          ", markers processed: " + itoa(cumulativeStats.numDumpDocuments) +
          ", bytes received: " + itoa(cumulativeStats.numDumpBytesReceived) +
          ", dump requests: " +
          std::to_string(cumulativeStats.numDumpRequests) +
          ", waited for dump: " +
          std::to_string(cumulativeStats.waitedForDump) + " s" +
          ", apply time: " +
          std::to_string(cumulativeStats.waitedForDumpApply) + " s" +
          ", total time: " + std::to_string(TRI_microtime() - startTime) +
          " s");
      return Result();
    }

    batch++;

    if (isAborted()) {
      return Result(TRI_ERROR_REPLICATION_APPLIER_STOPPED);
    }

    // update the global metrics so the changes become visible early
    stats.publish();
    // keep the cumulativeStats intact here!
  }

  TRI_ASSERT(false);
  return Result(TRI_ERROR_INTERNAL);
}

/// @brief incrementally fetch data from a collection
Result DatabaseInitialSyncer::fetchCollectionSync(
    arangodb::LogicalCollection* coll, std::string const& leaderColl,
    TRI_voc_tick_t maxTick) {
  if (coll->syncByRevision() && _config.leader.version() >= 30800) {
    // local collection should support revisions, and leader is at least aware
    // of the revision-based protocol, so we can query it to find out if we
    // can use the new protocol; will fall back to old one if leader collection
    // is an old variant
    return fetchCollectionSyncByRevisions(coll, leaderColl, maxTick);
  }
  return fetchCollectionSyncByKeys(coll, leaderColl, maxTick);
}

/// @brief incrementally fetch data from a collection using keys as the primary
/// document identifier
Result DatabaseInitialSyncer::fetchCollectionSyncByKeys(
    arangodb::LogicalCollection* coll, std::string const& leaderColl,
    TRI_voc_tick_t maxTick) {
  using ::arangodb::basics::StringUtils::concatT;
  using ::arangodb::basics::StringUtils::urlEncode;

  if (!_config.isChild()) {
    batchExtend();
  }

  ReplicationMetricsFeature::InitialSyncStats stats(
      coll->vocbase().server().getFeature<ReplicationMetricsFeature>(), true);

  // We'll do one quick attempt at getting the keys on the leader.
  // We might receive the keys or a count of the keys. In the first case we
  // continue with the sync. If we get a document count, we'll estimate a
  // pessimistic wait of roughly 1e9/day and repeat the call without a quick
  // option.
  std::string const baseUrl = replutils::ReplicationUrl + "/keys";
  std::string url = baseUrl + "?collection=" + urlEncode(leaderColl) +
                    "&to=" + std::to_string(maxTick) +
                    "&serverId=" + _state.localServerIdString +
                    "&batchId=" + std::to_string(_config.batch.id);

  std::string msg = "fetching collection keys for collection '" + coll->name() +
                    "' from " + url;
  _config.progress.set(msg);

  // use a lower bound for maxWaitTime of 180M microseconds, i.e. 180 seconds
  constexpr uint64_t lowerBoundForWaitTime = 180000000;

  // note: maxWaitTime has a unit of microseconds
  uint64_t maxWaitTime = _config.applier._initialSyncMaxWaitTime;
  maxWaitTime = std::max<uint64_t>(maxWaitTime, lowerBoundForWaitTime);

  // the following two variables can be modified by the "keysCall" lambda
  VPackBuilder builder;
  VPackSlice slice;

  auto keysCall = [&](bool quick) {
    // send an initial async request to collect the collection keys on the other
    // side
    // sending this request in a blocking fashion may require very long to
    // complete,
    // so we're sending the x-arango-async header here
    auto headers = replutils::createHeaders();
    headers[StaticStrings::Async] = "store";

    std::unique_ptr<httpclient::SimpleHttpResult> response;
    _config.connection.lease([&](httpclient::SimpleHttpClient* client) {
      response.reset(client->retryRequest(rest::RequestType::POST,
                                          (quick) ? url + "&quick=true" : url,
                                          nullptr, 0, headers));
    });
    ++stats.numKeysRequests;

    if (replutils::hasFailed(response.get())) {
      ++stats.numFailedConnects;
      return replutils::buildHttpError(response.get(), url, _config.connection);
    }

    bool found = false;
    std::string jobId = response->getHeaderField(StaticStrings::AsyncId, found);

    if (!found) {
      ++stats.numFailedConnects;
      return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                    std::string("got invalid response from leader at ") +
                        _config.leader.endpoint + url +
                        ": could not find 'X-Arango-Async' header");
    }

    double const startTime = TRI_microtime();

    headers = replutils::createHeaders();

    while (true) {
      if (!_config.isChild()) {
        batchExtend();
      }

      std::string const jobUrl = "/_api/job/" + jobId;
      _config.connection.lease([&](httpclient::SimpleHttpClient* client) {
        response.reset(client->request(rest::RequestType::PUT, jobUrl, nullptr,
                                       0, headers));
      });

      double waitTime = TRI_microtime() - startTime;

      if (response != nullptr && response->isComplete()) {
        if (response->hasContentLength()) {
          stats.numSyncBytesReceived += response->getContentLength();
        }
        if (response->hasHeaderField(StaticStrings::AsyncId)) {
          // job is done, got the actual response
          break;
        }
        if (response->getHttpReturnCode() == 404) {
          // unknown job, we can abort
          ++stats.numFailedConnects;
          stats.waitedForInitial += waitTime;
          return Result(TRI_ERROR_REPLICATION_NO_RESPONSE,
                        std::string("job not found on leader at ") +
                            _config.leader.endpoint);
        }
      }

      TRI_ASSERT(maxWaitTime >= lowerBoundForWaitTime);

      if (static_cast<uint64_t>(waitTime * 1000.0 * 1000.0) >= maxWaitTime) {
        ++stats.numFailedConnects;
        stats.waitedForInitial += waitTime;
        return Result(
            TRI_ERROR_REPLICATION_NO_RESPONSE,
            std::string("timed out waiting for response from leader at ") +
                _config.leader.endpoint);
      }

      if (isAborted()) {
        stats.waitedForInitial += waitTime;
        return Result(TRI_ERROR_REPLICATION_APPLIER_STOPPED);
      }

      std::chrono::milliseconds sleepTime = ::sleepTimeFromWaitTime(waitTime);
      std::this_thread::sleep_for(sleepTime);
    }

    stats.waitedForInitial += TRI_microtime() - startTime;

    if (replutils::hasFailed(response.get())) {
      ++stats.numFailedConnects;
      return replutils::buildHttpError(response.get(), url, _config.connection);
    }

    Result r = replutils::parseResponse(builder, response.get());

    if (r.fail()) {
      ++stats.numFailedConnects;
      return Result(
          TRI_ERROR_REPLICATION_INVALID_RESPONSE,
          concatT("got invalid response from leader at ",
                  _config.leader.endpoint, url, ": ", r.errorMessage()));
    }

    slice = builder.slice();
    if (!slice.isObject()) {
      ++stats.numFailedConnects;
      return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                    std::string("got invalid response from leader at ") +
                        _config.leader.endpoint + url +
                        ": response is no object");
    }

    return Result();
  };

  auto ck = keysCall(true);
  if (!ck.ok()) {
    return ck;
  }
  VPackSlice const c = slice.get("count");
  if (!c.isNumber()) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  std::string("got invalid response from master at ") +
                      _config.leader.endpoint + url +
                      ": response count not a number");
  }

  uint64_t ndocs = c.getNumber<uint64_t>();

#ifdef ARANGODB_ENABLE_FAILURE_TESTS
  if (ndocs > _quickKeysNumDocsLimit && slice.hasKey("id")) {
    LOG_TOPIC("6e1b3", ERR, Logger::REPLICATION)
        << "client: DatabaseInitialSyncer::run - expected only document count "
           "for quick call";
    TRI_ASSERT(false);
  }
#endif

  if (!slice.hasKey("id")) {  // we only have count
    // calculate a wait time proportional to the number of documents on the
    // leader if we get 1M documents back, the wait time is 80M microseconds,
    // i.e. 80 seconds if we get 10M documents back, the wait time is 800M
    // microseconds, i.e. 800 seconds
    // ...
    maxWaitTime = std::max<uint64_t>(maxWaitTime, ndocs * 80);

    // there is an additional lower bound for the wait time as defined initially
    // in
    //    _config.applier._initialSyncMaxWaitTime
    // we also apply an additional lower bound of 180 seconds here, in case that
    // value is configured too low, for whatever reason
    maxWaitTime = std::max<uint64_t>(maxWaitTime, lowerBoundForWaitTime);

    TRI_ASSERT(maxWaitTime >= lowerBoundForWaitTime);

    // note: keysCall() can modify the "slice" variable
    ck = keysCall(false);
    if (!ck.ok()) {
      return ck;
    }
  }

  VPackSlice const keysId = slice.get("id");
  if (!keysId.isString()) {
    ++stats.numFailedConnects;
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  std::string("got invalid response from leader at ") +
                      _config.leader.endpoint + url +
                      ": response does not contain valid 'id' attribute");
  }

  auto sg = arangodb::scopeGuard([&]() noexcept {
    try {
      url = baseUrl + "/" + keysId.copyString();
      std::string msg =
          "deleting remote collection keys object for collection '" +
          coll->name() + "' from " + url;
      _config.progress.set(msg);

      // now delete the keys we ordered
      std::unique_ptr<httpclient::SimpleHttpResult> response;
      _config.connection.lease([&](httpclient::SimpleHttpClient* client) {
        auto headers = replutils::createHeaders();
        response.reset(client->retryRequest(rest::RequestType::DELETE_REQ, url,
                                            nullptr, 0, headers));
      });
    } catch (std::exception const& ex) {
      LOG_TOPIC("f8b31", ERR, Logger::REPLICATION)
          << "Failed to deleting remote collection keys object for collection "
          << coll->name() << ex.what();
    }
  });

  VPackSlice const count = slice.get("count");

  if (!count.isNumber()) {
    ++stats.numFailedConnects;
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  std::string("got invalid response from leader at ") +
                      _config.leader.endpoint + url +
                      ": response does not contain valid 'count' attribute");
  }

  if (count.getNumber<size_t>() <= 0) {
    // remote collection has no documents. now truncate our local collection
    SingleCollectionTransaction trx(
        transaction::StandaloneContext::Create(vocbase()), *coll,
        AccessMode::Type::EXCLUSIVE);
    trx.addHint(transaction::Hints::Hint::INTERMEDIATE_COMMITS);
    trx.addHint(transaction::Hints::Hint::ALLOW_RANGE_DELETE);
    Result res = trx.begin();

    if (!res.ok()) {
      return Result(res.errorNumber(), concatT("unable to start transaction: ",
                                               res.errorMessage()));
    }

    OperationOptions options;

    if (!_state.leaderId.empty()) {
      options.isSynchronousReplicationFrom = _state.leaderId;
    }

    OperationResult opRes = trx.truncate(coll->name(), options);

    if (opRes.fail()) {
      return Result(opRes.errorNumber(),
                    concatT("unable to truncate collection '", coll->name(),
                            "': ", TRI_errno_string(opRes.errorNumber())));
    }

    return trx.finish(opRes.result);
  }

  // now we can fetch the complete chunk information from the leader
  try {
    return coll->vocbase()
        .server()
        .getFeature<EngineSelectorFeature>()
        .engine()
        .handleSyncKeys(*this, *coll, keysId.copyString());
  } catch (arangodb::basics::Exception const& ex) {
    return Result(ex.code(), ex.what());
  } catch (std::exception const& ex) {
    return Result(TRI_ERROR_INTERNAL, ex.what());
  } catch (...) {
    return Result(TRI_ERROR_INTERNAL);
  }
}

/// @brief order a new chunk from the /revisions API
void DatabaseInitialSyncer::fetchRevisionsChunk(
    std::shared_ptr<Syncer::JobSynchronizer> sharedStatus,
    std::string const& baseUrl, arangodb::LogicalCollection* coll,
    std::string const& leaderColl, std::string const& requestPayload,
    RevisionId requestResume) {
  if (isAborted()) {
    sharedStatus->gotResponse(Result(TRI_ERROR_REPLICATION_APPLIER_STOPPED));
    return;
  }

  try {
    std::string const typeString =
        (coll->type() == TRI_COL_TYPE_EDGE ? "edge" : "document");

    if (!_config.isChild()) {
      batchExtend();
    }

    using ::arangodb::basics::StringUtils::urlEncode;

    // assemble URL to call
    std::string url = baseUrl + "&" + StaticStrings::RevisionTreeResume + "=" +
                      urlEncode(requestResume.toString());

    bool isVPack = false;
    auto headers = replutils::createHeaders();
    if (_config.leader.version() >= 31000) {
      headers[StaticStrings::Accept] = StaticStrings::MimeTypeVPack;
      isVPack = true;
    }

    _config.progress.set(
        std::string(
            "fetching leader collection revision ranges for collection '") +
        coll->name() + "', type: " + typeString + ", format: " +
        (isVPack ? "vpack" : "json") + ", id: " + leaderColl + ", url: " + url);

    double t = TRI_microtime();

    // send request
    std::unique_ptr<httpclient::SimpleHttpResult> response;
    _config.connection.lease([&](httpclient::SimpleHttpClient* client) {
      response.reset(client->retryRequest(rest::RequestType::PUT, url,
                                          requestPayload.data(),
                                          requestPayload.size(), headers));
    });

    t = TRI_microtime() - t;

    if (replutils::hasFailed(response.get())) {
      sharedStatus->gotResponse(
          replutils::buildHttpError(response.get(), url, _config.connection),
          t);
      return;
    }

    // success!
    sharedStatus->gotResponse(std::move(response), t);
  } catch (basics::Exception const& ex) {
    sharedStatus->gotResponse(Result(ex.code(), ex.what()));
  } catch (std::exception const& ex) {
    sharedStatus->gotResponse(Result(TRI_ERROR_INTERNAL, ex.what()));
  }
}

/// @brief incrementally fetch data from a collection using keys as the primary
/// document identifier
Result DatabaseInitialSyncer::fetchCollectionSyncByRevisions(
    arangodb::LogicalCollection* coll, std::string const& leaderColl,
    TRI_voc_tick_t maxTick) {
  using ::arangodb::basics::StringUtils::concatT;
  using ::arangodb::basics::StringUtils::urlEncode;
  using ::arangodb::transaction::Hints;

  ReplicationMetricsFeature::InitialSyncStats stats(
      vocbase().server().getFeature<ReplicationMetricsFeature>(), true);

  double const startTime = TRI_microtime();

  if (!_config.isChild()) {
    batchExtend();
  }

  std::unique_ptr<containers::RevisionTree> treeLeader;
  std::string const baseUrl =
      replutils::ReplicationUrl + "/" + RestReplicationHandler::Revisions;

  // get leader tree
  {
    std::string url = baseUrl + "/" + RestReplicationHandler::Tree +
                      "?collection=" + urlEncode(leaderColl) +
                      "&onlyPopulated=true" + "&to=" + std::to_string(maxTick) +
                      "&serverId=" + _state.localServerIdString +
                      "&batchId=" + std::to_string(_config.batch.id);

    std::string msg = "fetching collection revision tree for collection '" +
                      coll->name() + "' from " + url;
    _config.progress.set(msg);

    auto headers = replutils::createHeaders();
    std::unique_ptr<httpclient::SimpleHttpResult> response;
    double t = TRI_microtime();
    _config.connection.lease([&](httpclient::SimpleHttpClient* client) {
      response.reset(client->retryRequest(rest::RequestType::GET, url, nullptr,
                                          0, headers));
    });
    stats.waitedForInitial += TRI_microtime() - t;

    if (replutils::hasFailed(response.get())) {
      if (response &&
          response->getHttpReturnCode() ==
              static_cast<int>(rest::ResponseCode::NOT_IMPLEMENTED)) {
        // collection on leader doesn't support revisions-based protocol,
        // fallback
        return fetchCollectionSyncByKeys(coll, leaderColl, maxTick);
      }
      stats.numFailedConnects++;
      return replutils::buildHttpError(response.get(), url, _config.connection);
    }

    if (response->hasContentLength()) {
      stats.numSyncBytesReceived += response->getContentLength();
    }

    auto body = response->getBodyVelocyPack();
    if (!body) {
      ++stats.numFailedConnects;
      return Result(
          TRI_ERROR_INTERNAL,
          "received improperly formed response when fetching revision tree");
    }
    treeLeader = containers::RevisionTree::deserialize(body->slice());
    if (!treeLeader) {
      ++stats.numFailedConnects;
      return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                    std::string("got invalid response from leader at ") +
                        _config.leader.endpoint + url +
                        ": response does not contain a valid revision tree");
    }

    if (treeLeader->count() == 0) {
      // remote collection has no documents. now truncate our local collection
      SingleCollectionTransaction trx(
          transaction::StandaloneContext::Create(vocbase()), *coll,
          AccessMode::Type::EXCLUSIVE);
      trx.addHint(transaction::Hints::Hint::INTERMEDIATE_COMMITS);
      trx.addHint(transaction::Hints::Hint::ALLOW_RANGE_DELETE);
      Result res = trx.begin();

      if (!res.ok()) {
        return Result(
            res.errorNumber(),
            concatT("unable to start transaction: ", res.errorMessage()));
      }

      OperationOptions options;

      if (!_state.leaderId.empty()) {
        options.isSynchronousReplicationFrom = _state.leaderId;
      }

      OperationResult opRes = trx.truncate(coll->name(), options);

      if (opRes.fail()) {
        return Result(opRes.errorNumber(),
                      concatT("unable to truncate collection '", coll->name(),
                              "': ", TRI_errno_string(opRes.errorNumber())));
      }

      return trx.finish(opRes.result);
    }
  }

  if (isAborted()) {
    return Result(TRI_ERROR_REPLICATION_APPLIER_STOPPED);
  }

  PhysicalCollection* physical = coll->getPhysical();
  TRI_ASSERT(physical);
  auto context =
      arangodb::transaction::StandaloneContext::Create(coll->vocbase());
  TransactionId blockerId = context->generateId();
  physical->placeRevisionTreeBlocker(blockerId);

  auto blockerGuard = scopeGuard([&]() noexcept {  // remove blocker afterwards
    try {
      physical->removeRevisionTreeBlocker(blockerId);
    } catch (std::exception const& ex) {
      LOG_TOPIC("e020c", ERR, Logger::REPLICATION)
          << "Failed to remove revision tree blocker: " << ex.what();
    }
  });
  std::unique_ptr<arangodb::SingleCollectionTransaction> trx;
  transaction::Options options;
  // We do intermediate commits relatively frequently, since this is good
  // for performance, and we actually have no transactional needs here.
  options.intermediateCommitCount = 10000;
  TRI_IF_FAILURE("IncrementalReplicationFrequentIntermediateCommit") {
    options.intermediateCommitCount = 1000;
  }
  try {
    trx = std::make_unique<arangodb::SingleCollectionTransaction>(
        context, *coll, arangodb::AccessMode::Type::EXCLUSIVE, options);
  } catch (arangodb::basics::Exception const& ex) {
    if (ex.code() == TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND) {
      bool locked = false;
      TRI_vocbase_col_status_e status = coll->tryFetchStatus(locked);
      if (!locked) {
        // fall back to unsafe method as last resort
        status = coll->status();
      }
      if (status == TRI_vocbase_col_status_e::TRI_VOC_COL_STATUS_DELETED) {
        // TODO handle
        setAborted(true);  // probably wrong?
        return Result(TRI_ERROR_REPLICATION_APPLIER_STOPPED);
      }
    }
    return Result(ex.code());
  }

  // we must be able to read our own writes here - otherwise the end result
  // can be wrong. do not enable NO_INDEXING here!

  // turn on intermediate commits as the number of keys to delete can be huge
  // here
  trx->addHint(Hints::Hint::INTERMEDIATE_COMMITS);
  Result res = trx->begin();
  if (!res.ok()) {
    return Result(res.errorNumber(),
                  concatT("unable to start transaction: ", res.errorMessage()));
  }
  auto guard = scopeGuard([trx = trx.get()]() noexcept {
    auto res = basics::catchToResult([&]() -> Result {
      if (trx->status() == transaction::Status::RUNNING) {
        return trx->abort();
      }
      return {};
    });
    if (res.fail()) {
      LOG_TOPIC("1a537", ERR, Logger::REPLICATION)
          << "Failed to abort transaction: " << res;
    }
  });

  // diff with local tree
  // std::pair<std::size_t, std::size_t> fullRange = treeLeader->range();
  auto treeLocal = physical->revisionTree(*trx);
  if (!treeLocal) {
    // local collection does not support syncing by revision, fall back to keys
    guard.fire();
    return fetchCollectionSyncByKeys(coll, leaderColl, maxTick);
  }
  // make sure revision tree blocker is removed
  blockerGuard.fire();

  std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges =
      treeLeader->diff(*treeLocal);
  if (ranges.empty()) {
    // no differences, done!
    setProgress("no differences between two revision trees, ending");
    return Result{};
  }

  // now lets get the actual ranges and handle the differences

  {
    VPackBuilder requestBuilder;
    {
      char ridBuffer[arangodb::basics::maxUInt64StringSize];
      VPackArrayBuilder list(&requestBuilder);
      for (auto& pair : ranges) {
        VPackArrayBuilder range(&requestBuilder);
        requestBuilder.add(
            basics::HybridLogicalClock::encodeTimeStampToValuePair(pair.first,
                                                                   ridBuffer));
        requestBuilder.add(
            basics::HybridLogicalClock::encodeTimeStampToValuePair(pair.second,
                                                                   ridBuffer));
      }
    }

    std::unique_ptr<ReplicationIterator> iter =
        physical->getReplicationIterator(
            ReplicationIterator::Ordering::Revision, *trx);
    if (!iter) {
      return Result(TRI_ERROR_INTERNAL, "could not get replication iterator");
    }

    RevisionReplicationIterator& local =
        *static_cast<RevisionReplicationIterator*>(iter.get());

    uint64_t const documentsFound = treeLocal->count();

    std::vector<RevisionId> toFetch;
    std::vector<RevisionId> toRemove;

    std::string const requestPayload = requestBuilder.slice().toJson();
    std::string const url = baseUrl + "/" + RestReplicationHandler::Ranges +
                            "?collection=" + urlEncode(leaderColl) +
                            "&serverId=" + _state.localServerIdString +
                            "&batchId=" + std::to_string(_config.batch.id);
    RevisionId requestResume{ranges[0].first};  // start with beginning
    RevisionId iterResume = requestResume;
    std::size_t chunk = 0;

    // the shared status will wait in its destructor until all posted
    // requests have been completed/canceled!
    auto self = shared_from_this();
    auto sharedStatus = std::make_shared<Syncer::JobSynchronizer>(self);

    // order initial chunk. this will block until the initial response
    // has arrived
    fetchRevisionsChunk(sharedStatus, url, coll, leaderColl, requestPayload,
                        requestResume);

    // Builder will be recycled
    VPackBuilder responseBuilder;

    auto& nf = coll->vocbase().server().getFeature<arangodb::NetworkFeature>();
    while (requestResume < RevisionId::max()) {
      std::unique_ptr<httpclient::SimpleHttpResult> chunkResponse;

      // block until we either got a response or were shut down
      Result res = sharedStatus->waitForResponse(chunkResponse);

      // update our statistics
      ++stats.numKeysRequests;
      stats.waitedForKeys += sharedStatus->time();

      if (res.fail()) {
        // no response or error or shutdown
        return res;
      }

      // now we have got a response!
      TRI_ASSERT(chunkResponse != nullptr);

      if (chunkResponse->hasContentLength()) {
        stats.numSyncBytesReceived += chunkResponse->getContentLength();
      }

      VPackSlice slice;

      if (::isVelocyPack(*chunkResponse)) {
        // velocypack body...

        // intentional copy of options
        VPackOptions validationOptions =
            basics::VelocyPackHelper::strictRequestValidationOptions;
        // allow custom types being sent here
        validationOptions.disallowCustom = false;
        VPackValidator validator(&validationOptions);

        validator.validate(chunkResponse->getBody().begin(),
                           chunkResponse->getBody().length(),
                           /*isSubPart*/ false);

        slice = VPackSlice(
            reinterpret_cast<uint8_t const*>(chunkResponse->getBody().begin()));
      } else {
        // JSON body...
        // recycle builder
        responseBuilder.clear();
        Result r =
            replutils::parseResponse(responseBuilder, chunkResponse.get());
        if (r.fail()) {
          ++stats.numFailedConnects;
          return Result(
              TRI_ERROR_REPLICATION_INVALID_RESPONSE,
              concatT("got invalid response from leader at ",
                      _config.leader.endpoint, url, ": ", r.errorMessage()));
        }
        slice = responseBuilder.slice();
      }

      if (!slice.isObject()) {
        ++stats.numFailedConnects;
        return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                      std::string("got invalid response from leader at ") +
                          _config.leader.endpoint + url +
                          ": response is not an object");
      }

      VPackSlice resumeSlice = slice.get("resume");
      if (!resumeSlice.isNone() && !resumeSlice.isString()) {
        ++stats.numFailedConnects;
        return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                      std::string("got invalid response from leader at ") +
                          _config.leader.endpoint + url +
                          ": response field 'resume' is not a number");
      }
      requestResume = resumeSlice.isNone() ? RevisionId::max()
                                           : RevisionId::fromSlice(resumeSlice);

      if (requestResume < RevisionId::max() && !isAborted()) {
        // already fetch next chunk in the background, by posting the
        // request to the scheduler, which can run it asynchronously
        sharedStatus->request([this, self, url, sharedStatus, coll, leaderColl,
                               requestResume, &requestPayload]() {
          fetchRevisionsChunk(sharedStatus, url, coll, leaderColl,
                              requestPayload, requestResume);
        });
      }

      VPackSlice rangesSlice = slice.get("ranges");
      if (!rangesSlice.isArray()) {
        ++stats.numFailedConnects;
        return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                      std::string("got invalid response from leader at ") +
                          _config.leader.endpoint + url +
                          ": response field 'ranges' is not an array");
      }

      for (VPackSlice leaderSlice : VPackArrayIterator(rangesSlice)) {
        if (!leaderSlice.isArray()) {
          ++stats.numFailedConnects;
          return Result(
              TRI_ERROR_REPLICATION_INVALID_RESPONSE,
              std::string("got invalid response from leader at ") +
                  _config.leader.endpoint + url +
                  ": response field 'ranges' entry is not a revision range");
        }
        auto& currentRange = ranges[chunk];
        if (!local.hasMore() ||
            local.revision() < RevisionId{currentRange.first}) {
          local.seek(std::max(iterResume, RevisionId{currentRange.first}));
        }

        RevisionId removalBound =
            leaderSlice.isEmptyArray()
                ? RevisionId{currentRange.second}.next()
                : RevisionId::fromSlice(leaderSlice.at(0));
        TRI_ASSERT(RevisionId{currentRange.first} <= removalBound);
        TRI_ASSERT(removalBound <= RevisionId{currentRange.second}.next());
        RevisionId mixedBound = leaderSlice.isEmptyArray()
                                    ? RevisionId{currentRange.second}
                                    : RevisionId::fromSlice(leaderSlice.at(
                                          leaderSlice.length() - 1));
        TRI_ASSERT(RevisionId{currentRange.first} <= mixedBound);
        TRI_ASSERT(mixedBound <= RevisionId{currentRange.second});

        while (local.hasMore() && local.revision() < removalBound) {
          toRemove.emplace_back(local.revision());
          iterResume = std::max(iterResume, local.revision().next());
          local.next();
        }

        std::size_t index = 0;
        while (local.hasMore() && local.revision() <= mixedBound) {
          RevisionId leaderRev = RevisionId::fromSlice(leaderSlice.at(index));

          if (local.revision() < leaderRev) {
            toRemove.emplace_back(local.revision());
            iterResume = std::max(iterResume, local.revision().next());
            local.next();
          } else if (leaderRev < local.revision()) {
            toFetch.emplace_back(leaderRev);
            ++index;
            iterResume = std::max(iterResume, leaderRev.next());
          } else {
            TRI_ASSERT(local.revision() == leaderRev);
            // match, no need to remove local or fetch from leader
            ++index;
            iterResume = std::max(iterResume, leaderRev.next());
            local.next();
          }
        }
        for (; index < leaderSlice.length(); ++index) {
          RevisionId leaderRev = RevisionId::fromSlice(leaderSlice.at(index));
          // fetch any leftovers
          toFetch.emplace_back(leaderRev);
          iterResume = std::max(iterResume, leaderRev.next());
        }

        while (local.hasMore() &&
               local.revision() <= std::min(requestResume.previous(),
                                            RevisionId{currentRange.second})) {
          toRemove.emplace_back(local.revision());
          iterResume = std::max(iterResume, local.revision().next());
          local.next();
        }

        if (requestResume > RevisionId{currentRange.second}) {
          ++chunk;
        }
      }

      res = ::removeRevisions(*trx, *coll, toRemove, stats);
      if (res.fail()) {
        return res;
      }
      toRemove.clear();

      res = ::fetchRevisions(nf, *trx, _config, _state, *coll, leaderColl,
                             toFetch, stats);
      if (res.fail()) {
        return res;
      }
      toFetch.clear();

      res = trx->state()->performIntermediateCommitIfRequired(coll->id());

      if (res.fail()) {
        return res;
      }
    }

    // adjust counts
    {
      uint64_t numberDocumentsAfterSync =
          documentsFound + stats.numDocsInserted - stats.numDocsRemoved;
      uint64_t numberDocumentsDueToCounter =
          coll->numberDocuments(trx.get(), transaction::CountType::Normal);

      setProgress(std::string("number of remaining documents in collection '") +
                  coll->name() +
                  "': " + std::to_string(numberDocumentsAfterSync) +
                  ", number of documents due to collection count: " +
                  std::to_string(numberDocumentsDueToCounter));

      if (numberDocumentsAfterSync != numberDocumentsDueToCounter) {
        int64_t diff = static_cast<int64_t>(numberDocumentsAfterSync) -
                       static_cast<int64_t>(numberDocumentsDueToCounter);

        LOG_TOPIC("118bf", WARN, Logger::REPLICATION)
            << "number of remaining documents in collection '" << coll->name()
            << "' is " << numberDocumentsAfterSync << " and differs from "
            << "number of documents returned by collection count "
            << numberDocumentsDueToCounter
            << ", documents found: " << documentsFound
            << ", num docs inserted: " << stats.numDocsInserted
            << ", num docs removed: " << stats.numDocsRemoved << ", a diff of "
            << diff << " will be applied";

        // patch the document counter of the collection and the transaction
        trx->documentCollection()->getPhysical()->adjustNumberDocuments(*trx,
                                                                        diff);
      }
    }

    res = trx->commit();
    if (res.fail()) {
      return res;
    }
    TRI_ASSERT(requestResume == RevisionId::max());
  }

  setProgress(
      std::string("incremental tree sync statistics for collection '") +
      coll->name() +
      "': keys requests: " + std::to_string(stats.numKeysRequests) +
      ", docs requests: " + std::to_string(stats.numDocsRequests) +
      ", bytes received: " + std::to_string(stats.numSyncBytesReceived) +
      ", number of documents requested: " +
      std::to_string(stats.numDocsRequested) +
      ", number of documents inserted: " +
      std::to_string(stats.numDocsInserted) +
      ", number of documents removed: " + std::to_string(stats.numDocsRemoved) +
      ", waited for initial: " + std::to_string(stats.waitedForInitial) +
      " s, " + "waited for keys: " + std::to_string(stats.waitedForKeys) +
      " s, " + "waited for docs: " + std::to_string(stats.waitedForDocs) +
      " s, " + "waited for insertions: " +
      std::to_string(stats.waitedForInsertions) + " s, " +
      "waited for removals: " + std::to_string(stats.waitedForRemovals) +
      " s, " + "total time: " + std::to_string(TRI_microtime() - startTime) +
      " s");

  return Result{};
}

/// @brief incrementally fetch data from a collection
/// @brief changes the properties of a collection, based on the VelocyPack
/// provided
Result DatabaseInitialSyncer::changeCollection(arangodb::LogicalCollection* col,
                                               VPackSlice const& slice) {
  arangodb::CollectionGuard guard(&vocbase(), col->id());

  return guard.collection()->properties(slice);
}

/// @brief whether or not the collection has documents
bool DatabaseInitialSyncer::hasDocuments(
    arangodb::LogicalCollection const& col) {
  return col.getPhysical()->hasDocuments();
}

/// @brief handle the information about a collection
Result DatabaseInitialSyncer::handleCollection(VPackSlice const& parameters,
                                               VPackSlice const& indexes,
                                               bool incremental,
                                               SyncPhase phase) {
  using ::arangodb::basics::StringUtils::concatT;
  using ::arangodb::basics::StringUtils::itoa;

  if (isAborted()) {
    return Result(TRI_ERROR_REPLICATION_APPLIER_STOPPED);
  }

  if (!parameters.isObject() || !indexes.isArray()) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE);
  }

  if (!_config.isChild()) {
    batchExtend();
  }

  std::string const leaderName =
      basics::VelocyPackHelper::getStringValue(parameters, "name", "");

  DataSourceId const leaderCid{
      basics::VelocyPackHelper::extractIdValue(parameters)};

  if (leaderCid.empty()) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  "collection id is missing in response");
  }

  std::string const leaderUuid = basics::VelocyPackHelper::getStringValue(
      parameters, "globallyUniqueId", "");

  VPackSlice const type = parameters.get("type");

  if (!type.isNumber()) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  "collection type is missing in response");
  }

  std::string const typeString =
      (type.getNumber<int>() == 3 ? "edge" : "document");

  std::string const collectionMsg = "collection '" + leaderName + "', type " +
                                    typeString + ", id " + itoa(leaderCid.id());

  // phase handling
  if (phase == PHASE_VALIDATE) {
    // validation phase just returns ok if we got here (aborts above if data is
    // invalid)
    _config.progress.processedCollections.try_emplace(leaderCid, leaderName);

    return Result();
  }

  // drop and re-create collections locally
  // -------------------------------------------------------------------------------------

  if (phase == PHASE_DROP_CREATE) {
    auto col = resolveCollection(vocbase(), parameters);

    if (col == nullptr) {
      // not found...
      col = vocbase().lookupCollection(leaderName);

      if (col != nullptr &&
          (col->name() != leaderName ||
           (!leaderUuid.empty() && col->guid() != leaderUuid))) {
        // found another collection with the same name locally.
        // in this case we must drop it because we will run into duplicate
        // name conflicts otherwise
        try {
          auto res =
              vocbase().dropCollection(col->id(), true, -1.0).errorNumber();

          if (res == TRI_ERROR_NO_ERROR) {
            col = nullptr;
          }
        } catch (...) {
        }
      }
    }

    if (col != nullptr) {
      if (!incremental) {
        // first look up the collection
        if (col != nullptr) {
          bool truncate = false;

          if (col->name() == StaticStrings::UsersCollection) {
            // better not throw away the _users collection. otherwise it is gone
            // and this may be a problem if the
            // server crashes in-between.
            truncate = true;
          }

          if (truncate) {
            // system collection
            _config.progress.set("truncating " + collectionMsg);

            SingleCollectionTransaction trx(
                transaction::StandaloneContext::Create(vocbase()), *col,
                AccessMode::Type::EXCLUSIVE);
            trx.addHint(transaction::Hints::Hint::INTERMEDIATE_COMMITS);
            trx.addHint(transaction::Hints::Hint::ALLOW_RANGE_DELETE);
            Result res = trx.begin();

            if (!res.ok()) {
              return Result(res.errorNumber(),
                            concatT("unable to truncate ", collectionMsg, ": ",
                                    res.errorMessage()));
            }

            OperationOptions options;

            if (!_state.leaderId.empty()) {
              options.isSynchronousReplicationFrom = _state.leaderId;
            }

            OperationResult opRes = trx.truncate(col->name(), options);

            if (opRes.fail()) {
              return Result(opRes.errorNumber(),
                            concatT("unable to truncate ", collectionMsg, ": ",
                                    TRI_errno_string(opRes.errorNumber())));
            }

            res = trx.finish(opRes.result);

            if (!res.ok()) {
              return Result(res.errorNumber(),
                            concatT("unable to truncate ", collectionMsg, ": ",
                                    res.errorMessage()));
            }
          } else {
            // drop a regular collection
            if (_config.applier._skipCreateDrop) {
              _config.progress.set("dropping " + collectionMsg +
                                   " skipped because of configuration");
              return Result();
            }
            _config.progress.set("dropping " + collectionMsg);

            auto res =
                vocbase().dropCollection(col->id(), true, -1.0).errorNumber();

            if (res != TRI_ERROR_NO_ERROR) {
              return Result(res, concatT("unable to drop ", collectionMsg, ": ",
                                         TRI_errno_string(res)));
            }
          }
        }
      } else {
        // incremental case
        TRI_ASSERT(incremental);

        // collection is already present
        _config.progress.set("checking/changing parameters of " +
                             collectionMsg);
        return changeCollection(col.get(), parameters);
      }
    }
    // When we get here, col is a nullptr anyway!

    std::string msg = "creating " + collectionMsg;
    if (_config.applier._skipCreateDrop) {
      msg += " skipped because of configuration";
      _config.progress.set(msg);
      return Result();
    }
    _config.progress.set(msg);

    LOG_TOPIC("7093d", DEBUG, Logger::REPLICATION)
        << "Dump is creating collection " << parameters.toJson();

    LogicalCollection* col2 = nullptr;
    auto r = createCollection(vocbase(), parameters, &col2);

    if (r.fail()) {
      return Result(r.errorNumber(),
                    concatT("unable to create ", collectionMsg, ": ",
                            TRI_errno_string(r.errorNumber()),
                            ". Collection info ", parameters.toJson()));
    }

    return r;
  }

  // sync collection data
  // -------------------------------------------------------------------------------------

  else if (phase == PHASE_DUMP) {
    _config.progress.set("dumping data for " + collectionMsg);

    std::shared_ptr<LogicalCollection> col =
        resolveCollection(vocbase(), parameters);

    if (col == nullptr) {
      return Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
                    std::string("cannot dump: ") + collectionMsg +
                        " not found on leader. Collection info " +
                        parameters.toJson());
    }

    std::string const& leaderColl =
        !leaderUuid.empty() ? leaderUuid : itoa(leaderCid.id());
    auto res = incremental && hasDocuments(*col)
                   ? fetchCollectionSync(col.get(), leaderColl,
                                         _config.leader.lastLogTick)
                   : fetchCollectionDump(col.get(), leaderColl,
                                         _config.leader.lastLogTick);

    if (!res.ok()) {
      return res;
    } else if (isAborted()) {
      return res.reset(TRI_ERROR_REPLICATION_APPLIER_STOPPED);
    }

    if (leaderName == StaticStrings::UsersCollection) {
      reloadUsers();
    } else if (leaderName == StaticStrings::AnalyzersCollection &&
               ServerState::instance()->isSingleServer() &&
               vocbase()
                   .server()
                   .hasFeature<iresearch::IResearchAnalyzerFeature>()) {
      vocbase()
          .server()
          .getFeature<iresearch::IResearchAnalyzerFeature>()
          .invalidate(vocbase());
    }

    // schmutz++ creates indexes on DBServers
    if (_config.applier._skipCreateDrop) {
      _config.progress.set("creating indexes for " + collectionMsg +
                           " skipped because of configuration");
      return res;
    }

    // now create indexes
    TRI_ASSERT(indexes.isArray());
    VPackValueLength const numIdx = indexes.length();
    if (numIdx > 0) {
      if (!_config.isChild()) {
        batchExtend();
      }

      _config.progress.set("creating " + std::to_string(numIdx) +
                           " index(es) for " + collectionMsg);

      try {
        for (auto const& idxDef : VPackArrayIterator(indexes)) {
          if (idxDef.isObject()) {
            VPackSlice const type = idxDef.get(StaticStrings::IndexType);
            if (type.isString()) {
              _config.progress.set("creating index of type " +
                                   type.copyString() + " for " + collectionMsg);
            }
          }

          createIndexInternal(idxDef, *col);
        }
      } catch (arangodb::basics::Exception const& ex) {
        return res.reset(ex.code(), ex.what());
      } catch (std::exception const& ex) {
        return res.reset(TRI_ERROR_INTERNAL, ex.what());
      } catch (...) {
        return res.reset(TRI_ERROR_INTERNAL);
      }
    }

    return res;
  }

  // we won't get here
  TRI_ASSERT(false);
  return Result(TRI_ERROR_INTERNAL);
}

/// @brief fetch the server's inventory
arangodb::Result DatabaseInitialSyncer::fetchInventory(VPackBuilder& builder) {
  std::string url = replutils::ReplicationUrl +
                    "/inventory?serverId=" + _state.localServerIdString +
                    "&batchId=" + std::to_string(_config.batch.id);
  if (_config.applier._includeSystem) {
    url += "&includeSystem=true";
  }
  if (_config.applier._includeFoxxQueues) {
    url += "&includeFoxxQueues=true";
  }

  // use an optmization here for shard synchronization: only fetch the inventory
  // including a single shard. this can greatly reduce the size of the response.
  if (ServerState::instance()->isDBServer() && !_config.isChild() &&
      _config.applier._skipCreateDrop &&
      _config.applier._restrictType ==
          ReplicationApplierConfiguration::RestrictType::Include &&
      _config.applier._restrictCollections.size() == 1) {
    url += "&collection=" + arangodb::basics::StringUtils::urlEncode(*(
                                _config.applier._restrictCollections.begin()));
  }

  // send request
  _config.progress.set("fetching leader inventory from " + url);
  std::unique_ptr<httpclient::SimpleHttpResult> response;
  _config.connection.lease([&](httpclient::SimpleHttpClient* client) {
    auto headers = replutils::createHeaders();
    response.reset(
        client->retryRequest(rest::RequestType::GET, url, nullptr, 0, headers));
  });

  if (replutils::hasFailed(response.get())) {
    if (!_config.isChild()) {
      batchFinish();
    }
    return replutils::buildHttpError(response.get(), url, _config.connection);
  }

  Result r = replutils::parseResponse(builder, response.get());

  if (r.fail()) {
    return Result(
        r.errorNumber(),
        std::string("got invalid response from leader at ") +
            _config.leader.endpoint + url +
            ": invalid response type for initial data. expecting array");
  }

  VPackSlice slice = builder.slice();
  if (!slice.isObject()) {
    LOG_TOPIC("3b1e6", DEBUG, Logger::REPLICATION)
        << "client: DatabaseInitialSyncer::run - inventoryResponse is not an "
           "object";

    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  std::string("got invalid response from leader at ") +
                      _config.leader.endpoint + url + ": invalid JSON");
  }

  return r;
}

/// @brief handle the inventory response of the leader
Result DatabaseInitialSyncer::handleCollectionsAndViews(
    VPackSlice const& collSlices, VPackSlice const& viewSlices,
    bool incremental) {
  TRI_ASSERT(collSlices.isArray());

  std::vector<std::pair<VPackSlice, VPackSlice>> systemCollections;
  std::vector<std::pair<VPackSlice, VPackSlice>> collections;
  for (VPackSlice it : VPackArrayIterator(collSlices)) {
    if (!it.isObject()) {
      return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                    "collection declaration is invalid in response");
    }

    VPackSlice const parameters = it.get("parameters");

    if (!parameters.isObject()) {
      return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                    "collection parameters declaration is invalid in response");
    }

    VPackSlice const indexes = it.get("indexes");

    if (!indexes.isArray()) {
      return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                    "collection indexes declaration is invalid in response");
    }

    std::string const leaderName =
        basics::VelocyPackHelper::getStringValue(parameters, "name", "");

    if (leaderName.empty()) {
      return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                    "collection name is missing in response");
    }

    if (TRI_ExcludeCollectionReplication(leaderName,
                                         _config.applier._includeSystem,
                                         _config.applier._includeFoxxQueues)) {
      continue;
    }

    if (basics::VelocyPackHelper::getBooleanValue(parameters, "deleted",
                                                  false)) {
      // we don't care about deleted collections
      continue;
    }

    if (_config.applier._restrictType !=
        ReplicationApplierConfiguration::RestrictType::None) {
      auto const it = _config.applier._restrictCollections.find(leaderName);
      bool found = (it != _config.applier._restrictCollections.end());

      if (_config.applier._restrictType ==
              ReplicationApplierConfiguration::RestrictType::Include &&
          !found) {
        // collection should not be included
        continue;
      } else if (_config.applier._restrictType ==
                     ReplicationApplierConfiguration::RestrictType::Exclude &&
                 found) {
        // collection should be excluded
        continue;
      }
    }

    if (leaderName == StaticStrings::AnalyzersCollection) {
      // _analyzers collection has to be restored before view creation
      systemCollections.emplace_back(parameters, indexes);
    } else {
      collections.emplace_back(parameters, indexes);
    }
  }

  // STEP 1: validate collection declarations from leader
  // ----------------------------------------------------------------------------------

  // STEP 2: drop and re-create collections locally if they are also present on
  // the leader
  //  ------------------------------------------------------------------------------------

  // iterate over all collections from the leader...
  std::array<SyncPhase, 2> phases{{PHASE_VALIDATE, PHASE_DROP_CREATE}};
  for (auto const& phase : phases) {
    Result r = iterateCollections(systemCollections, incremental, phase);

    if (r.fail()) {
      return r;
    }

    r = iterateCollections(collections, incremental, phase);

    if (r.fail()) {
      return r;
    }
  }

  // STEP 3: restore data for system collections
  // ----------------------------------------------------------------------------------
  auto const res =
      iterateCollections(systemCollections, incremental, PHASE_DUMP);

  if (res.fail()) {
    return res;
  }

  // STEP 4: now that the collections exist create the "arangosearch" views
  // this should be faster than re-indexing afterwards
  // We don't create "search-alias" view because inverted indexes don't exist
  // yet
  // ----------------------------------------------------------------------------------

  if (!_config.applier._skipCreateDrop &&
      _config.applier._restrictCollections.empty() && viewSlices.isArray()) {
    // views are optional, and 3.3 and before will not send any view data
    auto r = handleViewCreation(
        viewSlices, arangodb::iresearch::StaticStrings::ViewArangoSearchType);
    if (r.fail()) {
      LOG_TOPIC("96cda", ERR, Logger::REPLICATION)
          << "Error during initial sync view creation: " << r.errorMessage();
      return r;
    }
  } else {
    _config.progress.set("view creation skipped because of configuration");
  }

  // STEP 5: sync collection data from leader and create initial indexes
  // ----------------------------------------------------------------------------------
  // now load the data into the collections
  auto r = iterateCollections(collections, incremental, PHASE_DUMP);
  if (r.fail()) {
    return r;
  }

  // STEP 6 load "search-alias" views
  // ----------------------------------------------------------------------------------
  return handleViewCreation(
      viewSlices, arangodb::iresearch::StaticStrings::ViewSearchAliasType);
}

/// @brief iterate over all collections from an array and apply an action
Result DatabaseInitialSyncer::iterateCollections(
    std::vector<std::pair<VPackSlice, VPackSlice>> const& collections,
    bool incremental, SyncPhase phase) {
  std::string phaseMsg("starting phase " + translatePhase(phase) + " with " +
                       std::to_string(collections.size()) + " collections");
  _config.progress.set(phaseMsg);

  for (auto const& collection : collections) {
    VPackSlice const parameters = collection.first;
    VPackSlice const indexes = collection.second;

    Result res = handleCollection(parameters, indexes, incremental, phase);

    if (res.fail()) {
      return res;
    }
  }

  // all ok
  return Result();
}

/// @brief create non-existing views locally
Result DatabaseInitialSyncer::handleViewCreation(VPackSlice views,
                                                 std::string_view type) {
  if (!views.isArray()) {
    return {TRI_ERROR_BAD_PARAMETER};
  }
  auto check = [&](VPackSlice definition) noexcept {
    // I want to cause fail in createView if definition is invalid
    if (!definition.isObject()) {
      return true;
    }
    if (!definition.hasKey(StaticStrings::DataSourceType)) {
      return true;
    }
    auto sliceType = definition.get(StaticStrings::DataSourceType);
    if (!sliceType.isString()) {
      return true;
    }
    return sliceType.stringView() != type;
  };
  for (VPackSlice slice : VPackArrayIterator(views)) {
    if (check(slice)) {
      continue;
    }
    Result res = createView(vocbase(), slice);
    if (res.fail()) {
      return res;
    }
  }
  return {};
}

Result DatabaseInitialSyncer::batchStart(char const* context,
                                         std::string const& patchCount) {
  return _config.batch.start(_config.connection, _config.progress,
                             _config.leader, _config.state.syncerId, context,
                             patchCount);
}

Result DatabaseInitialSyncer::batchExtend() {
  return _config.batch.extend(_config.connection, _config.progress,
                              _config.state.syncerId);
}

Result DatabaseInitialSyncer::batchFinish() noexcept {
  return _config.batch.finish(_config.connection, _config.progress,
                              _config.state.syncerId);
}

#ifdef ARANGODB_ENABLE_FAILURE_TESTS
/// @brief patch quickKeysNumDocsLimit for testing
void DatabaseInitialSyncer::adjustQuickKeysNumDocsLimit() {
  TRI_IF_FAILURE("RocksDBRestReplicationHandler::quickKeysNumDocsLimit100") {
    _quickKeysNumDocsLimit = 100;
  }
}
#endif

}  // namespace arangodb
