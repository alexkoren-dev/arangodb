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

#pragma once

#include "Basics/Common.h"
#include "Basics/ReadWriteLock.h"
#include "ClusterEngine/ClusterSelectivityEstimates.h"
#include "ClusterEngine/Common.h"
#include "StorageEngine/PhysicalCollection.h"
#include "VocBase/Identifiers/IndexId.h"
#include "VocBase/LogicalCollection.h"

namespace rocksdb {
class Transaction;
}

namespace arangodb {
namespace cache {
class Cache;
}

class LogicalCollection;
class ManagedDocumentResult;
class Result;
class RocksDBPrimaryIndex;
class RocksDBVPackIndex;
class LocalDocumentId;

class ClusterCollection final : public PhysicalCollection {
 public:
  explicit ClusterCollection(LogicalCollection& collection,
                             ClusterEngineType sengineType,
                             arangodb::velocypack::Slice const& info);
  ClusterCollection(LogicalCollection& collection,
                    PhysicalCollection const*);  // use in cluster only!!!!!

  ~ClusterCollection();

  /// @brief fetches current index selectivity estimates
  /// if allowUpdate is true, will potentially make a cluster-internal roundtrip
  /// to fetch current values!
  IndexEstMap clusterIndexEstimates(bool allowUpdating,
                                    TransactionId tid) override;

  /// @brief flushes the current index selectivity estimates
  void flushClusterIndexEstimates() override;

  std::string const& path() const override;

  arangodb::Result updateProperties(velocypack::Slice const& slice,
                                    bool doSync) override;

  virtual PhysicalCollection* clone(
      LogicalCollection& collection) const override;

  /// @brief export properties
  void getPropertiesVPack(velocypack::Builder&) const override;

  /// @brief return the figures for a collection
  futures::Future<OperationResult> figures(
      bool details, OperationOptions const& options) override;

  /// @brief closes an open collection
  ErrorCode close() override;

  RevisionId revision(arangodb::transaction::Methods* trx) const override;
  uint64_t numberDocuments(transaction::Methods* trx) const override;

  ////////////////////////////////////
  // -- SECTION Indexes --
  ///////////////////////////////////

  void prepareIndexes(velocypack::Slice indexesSlice) override;

  std::shared_ptr<Index> createIndex(velocypack::Slice info, bool restore,
                                     bool& created) override;

  /// @brief Drop an index with the given iid.
  bool dropIndex(IndexId iid) override;
  std::unique_ptr<IndexIterator> getAllIterator(
      transaction::Methods* trx, ReadOwnWrites readOwnWrites) const override;
  std::unique_ptr<IndexIterator> getAnyIterator(
      transaction::Methods* trx) const override;

  std::unique_ptr<IndexIterator> getSortedAllIterator(
      transaction::Methods* trx) const;

  ////////////////////////////////////
  // -- SECTION DML Operations --
  ///////////////////////////////////

  Result truncate(transaction::Methods& trx, OperationOptions& options,
                  bool& usedRangeDelete) override;

  void deferDropCollection(
      std::function<bool(LogicalCollection&)> const& callback) override;

  Result lookupKey(transaction::Methods* trx, std::string_view key,
                   std::pair<LocalDocumentId, RevisionId>& result,
                   ReadOwnWrites) const override;

  Result read(transaction::Methods*, std::string_view key,
              IndexIterator::DocumentCallback const& cb,
              ReadOwnWrites) const override;

  Result read(transaction::Methods* trx, LocalDocumentId const& token,
              IndexIterator::DocumentCallback const& cb,
              ReadOwnWrites) const override;

  bool readDocument(transaction::Methods* trx, LocalDocumentId const& token,
                    ManagedDocumentResult& result,
                    ReadOwnWrites) const override;

  Result lookupDocument(transaction::Methods& trx, LocalDocumentId token,
                        velocypack::Builder& builder, bool readCache,
                        bool fillCache,
                        ReadOwnWrites readOwnWrites) const override;

  Result insert(transaction::Methods& trx, RevisionId newRevisionId,
                velocypack::Slice newDocument,
                OperationOptions const& options) override;

  Result update(transaction::Methods& trx, LocalDocumentId previousDocumentId,
                RevisionId previousRevisionId,
                velocypack::Slice previousDocument, RevisionId newRevisionId,
                velocypack::Slice newDocument,
                OperationOptions const& options) override;

  Result replace(transaction::Methods& trx, LocalDocumentId previousDocumentId,
                 RevisionId previousRevisionId,
                 velocypack::Slice previousDocument, RevisionId newRevisionId,
                 velocypack::Slice newDocument,
                 OperationOptions const& options) override;

  Result remove(transaction::Methods& trx, LocalDocumentId previousDocumentId,
                RevisionId previousRevisionId,
                velocypack::Slice previousDocument,
                OperationOptions const& options) override;

 protected:
  /// @brief Inject figures that are specific to StorageEngine
  void figuresSpecific(bool details, arangodb::velocypack::Builder&) override;

 private:
  void addIndex(std::shared_ptr<arangodb::Index> idx);

  // keep locks just to adhere to behavior in other collections
  mutable basics::ReadWriteLock _exclusiveLock;
  ClusterEngineType _engineType;
  velocypack::Builder _info;
  ClusterSelectivityEstimates _selectivityEstimates;
};

}  // namespace arangodb
