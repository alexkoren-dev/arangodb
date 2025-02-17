/* global fail, AQL_EXPLAIN*/
////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2021-2021 ArangoDB GmbH, Cologne, Germany
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
/// @author Andrei Lobov
////////////////////////////////////////////////////////////////////////////////

'use strict';

const jsunity = require("jsunity");
const arangodb = require("@arangodb");
const analyzers = require("@arangodb/analyzers");
const db = arangodb.db;
const aql = arangodb.aql;
const {assertTrue, assertFalse, assertEqual} = jsunity.jsUnity.assertions;
const useIndexes = 'use-indexes';
const removeFilterCoveredByIndex = "remove-filter-covered-by-index";
const moveFiltersIntoEnumerate = "move-filters-into-enumerate";
const useIndexForSort = "use-index-for-sort";
const sleep = require('internal').sleep;
const errors = require('internal').errors;

function optimizerRuleInvertedIndexTestSuite() {
  const colName = 'UnitTestInvertedIndexCollection';
  let col;
  const docs = 10000;
  return {
    setUpAll: function () {
      col = db._create(colName);
      analyzers.save("my_geo", "geojson",{type: 'point'}, ["frequency", "norm", "position"]);
      col.ensureIndex({type: 'inverted',
                       name: 'InvertedIndexUnsorted',
                       fields: ['data_field', {name:'norm_field', analyzer: 'text_en'},
                                {name:'norm_field2', analyzer: 'text_en'},
                                {name:'searchField', searchField:true},
                                {name:'geo_field', analyzer:'my_geo'},
                                {name:'custom_field', analyzer:'text_en'}]});
      col.ensureIndex({type: 'inverted',
                       name: 'InvertedIndexSorted',
                       fields: ['data_field',
                                {name:'geo_field', analyzer:'my_geo'},
                                {name:'custom_field', analyzer:'text_en'}],
                        primarySort:{fields:[{field: "count", direction:"desc"}]}});
      let data = [];
      for (let i = 0; i < docs; i++) {
        if (i % 10 === 0) {
          data.push({count:i,
                     norm_field: 'fOx',
                     searchField:i,
                     data_field:'value' + i % 100,
                     custom_field:"quick brown",
                     geo_field:{type: 'Point', coordinates: [37.615895, 55.7039]}});
        } else {
          data.push({count:i,
                     norm_field2: 'BOX',
                     data_field:'value' + i % 100,
                     custom_field: i,
                     geo_field:{type: 'Point', coordinates: [27.615895, 15.7039]}});  
        }
      }
      col.insert(data);

      let syncIndex = function(indexName) {
        const syncQuery = aql`
          FOR d IN ${col} OPTIONS {indexHint: ${indexName}, waitForSync:true, forceIndexHint:true}
            FILTER STARTS_WITH(d.data_field, 'value') COLLECT WITH COUNT INTO c RETURN c`;
        db._query(syncQuery);
      };

      syncIndex("InvertedIndexSorted");
      syncIndex("InvertedIndexUnsorted");
    },
    tearDownAll: function () {
      col.drop();
      try { analyzers.remove("my_geo", true); } catch (e) {}
    },
    testCreateDuplicate: function () {
       let idx = col.ensureIndex({type: 'inverted',
                        name: 'InvertedIndexUnsorted_duplicate',
                        fields: ['data_field', {name:'norm_field', analyzer: 'text_en'},
                                {name:'norm_field2', analyzer: 'text_en'},
                                {name:'searchField', searchField:true},
                                {name:'geo_field', analyzer:'my_geo'},
                                {name:'custom_field', analyzer:'text_en'}]});
       assertEqual("InvertedIndexUnsorted", idx.name);       
    },
    testIndexNotHinted: function () {
      const query = aql`
        FOR d IN ${col}
          FILTER d.data_field == 'value1'
          RETURN d`;
      const res = AQL_EXPLAIN(query.query, query.bindVars);
      const appliedRules = res.plan.rules;
      assertFalse(appliedRules.includes(useIndexes));
      assertFalse(appliedRules.includes(removeFilterCoveredByIndex));
    },
    testIndexHintedUnsorted: function () {
      const query = aql`
        FOR d IN ${col} OPTIONS {indexHint: "InvertedIndexUnsorted"}
          FILTER d.data_field == 'value1'
          RETURN d`;
      const res = AQL_EXPLAIN(query.query, query.bindVars);
      const appliedRules = res.plan.rules;
      assertTrue(appliedRules.includes(useIndexes));
      assertTrue(appliedRules.includes(removeFilterCoveredByIndex));
      let executeRes = db._query(query.query, query.bindVars);
      assertEqual(docs / 100, executeRes.toArray().length);
    },
    testIndexHintedUnsortedPartial: function () {
      const query = aql`
        FOR d IN ${col} OPTIONS {indexHint: "InvertedIndexUnsorted"}
          FILTER d.data_field == 'value1' AND d.count == 1
          RETURN d`;
      const res = AQL_EXPLAIN(query.query, query.bindVars);
      const appliedRules = res.plan.rules;
      assertTrue(appliedRules.includes(useIndexes));
      assertTrue(appliedRules.includes(removeFilterCoveredByIndex));
      assertTrue(appliedRules.includes(moveFiltersIntoEnumerate));
      let executeRes = db._query(query.query, query.bindVars);
      assertEqual(1, executeRes.toArray().length);
    },
    testIndexGeo: function () {
      const query = aql`
        FOR d IN ${col} OPTIONS {indexHint: "InvertedIndexUnsorted"}
          FILTER GEO_INTERSECTS(d.geo_field, {type: 'Point', coordinates: [37.615895, 55.7039]})
          COLLECT WITH COUNT INTO c
          RETURN c`;
      const res = AQL_EXPLAIN(query.query, query.bindVars);
      const appliedRules = res.plan.rules;
      assertTrue(appliedRules.includes(useIndexes));
      assertTrue(appliedRules.includes(removeFilterCoveredByIndex));
      let executeRes = db._query(query.query, query.bindVars);
      assertEqual(docs/10, executeRes.toArray()[0]);
    },
    testIndexHintedSorted: function () {
      const query = aql`
        FOR d IN ${col} OPTIONS {indexHint: "InvertedIndexSorted"}
          FILTER d.data_field == 'value1'
          SORT d.count DESC
          RETURN d`;
      const res = AQL_EXPLAIN(query.query, query.bindVars);
      const appliedRules = res.plan.rules;
      assertTrue(appliedRules.includes(useIndexes));
      assertTrue(appliedRules.includes(removeFilterCoveredByIndex));
      assertTrue(appliedRules.includes(useIndexForSort));
      let executeRes = db._query(query.query, query.bindVars).toArray();
      assertEqual(docs / 100, executeRes.length);
      for(let i = 1; i < executeRes.length; ++i) {
        assertTrue(executeRes[i-1].count > executeRes[i].count);
      }
    },
    testIndexHintedSortedWrongOrder: function () {
      const query = aql`
        FOR d IN ${col} OPTIONS {indexHint: "InvertedIndexSorted"}
          FILTER d.data_field == 'value1'
          SORT d.count ASC
          RETURN d`;
      const res = AQL_EXPLAIN(query.query, query.bindVars);
      const appliedRules = res.plan.rules;
      assertTrue(appliedRules.includes(useIndexes));
      assertTrue(appliedRules.includes(removeFilterCoveredByIndex));
      assertFalse(appliedRules.includes(useIndexForSort));
      let executeRes = db._query(query.query, query.bindVars).toArray();
      assertEqual(docs / 100, executeRes.length);
      for(let i = 1; i < executeRes.length; ++i) {
        assertTrue(executeRes[i-1].count < executeRes[i].count);
      }
    },
    testIndexHintedUnsortedWithSort: function () {
      const query = aql`
        FOR d IN ${col} OPTIONS {indexHint: "InvertedIndexUnsorted"}
          FILTER d.data_field == 'value1'
          SORT d.count DESC
          RETURN d`;
      const res = AQL_EXPLAIN(query.query, query.bindVars);
      const appliedRules = res.plan.rules;
      assertTrue(appliedRules.includes(useIndexes));
      assertTrue(appliedRules.includes(removeFilterCoveredByIndex));
      assertFalse(appliedRules.includes(useIndexForSort));
      let executeRes = db._query(query.query, query.bindVars).toArray();
      assertEqual(docs / 100, executeRes.length);
      for(let i = 1; i < executeRes.length; ++i) {
        assertTrue(executeRes[i-1].count > executeRes[i].count);
      }
    },
    testIndexHintedRemove: function () {
      col.save({data_field:'remove_me'});
      let syncWait = 100;
      const syncQuery = aql`
        FOR d IN ${col} OPTIONS {indexHint: "InvertedIndexUnsorted"}
          FILTER STARTS_WITH(d.data_field, 'remove') COLLECT WITH COUNT INTO c  RETURN c`;
      let count  = db._query(syncQuery).toArray()[0];
      while (count < 1 && (--syncWait) > 0) {
        sleep(1);
        count  = db._query(syncQuery).toArray()[0];
      }
      const query = aql`
        FOR d IN ${col} OPTIONS {indexHint: "InvertedIndexUnsorted"}
          FILTER d.data_field == 'remove_me'
           REMOVE d IN ${col}`;
      const res = AQL_EXPLAIN(query.query, query.bindVars);
      const appliedRules = res.plan.rules;
      assertTrue(appliedRules.includes(useIndexes));
      assertTrue(appliedRules.includes(removeFilterCoveredByIndex));
      db._query(query.query, query.bindVars);
      const checkQuery = aql`
        FOR d IN ${col} 
          FILTER STARTS_WITH(d.data_field, 'remove') COLLECT WITH COUNT INTO c  RETURN c`;
      count  = db._query(checkQuery).toArray()[0];
      assertEqual(0, count);
    },
    testEmptyFields: function() {
      col.ensureIndex({type: 'inverted',
                       name: 'AllFieldsEmpty',
                       includeAllFields:true,
                       fields: [],
                       primarySort:{fields:[{field: "count", direction:"desc"}]}});
      col.dropIndex('AllFieldsEmpty');
      col.ensureIndex({type: 'inverted',
                       name: 'AllFieldsEmpty2',
                       includeAllFields:true,
                       primarySort:{fields:[{field: "count", direction:"desc"}]}});
      col.dropIndex('AllFieldsEmpty2');
      try {
        col.ensureIndex({type: 'inverted',
                       name: 'AllFieldsEmpty3',
                       includeAllFields:false,
                       fields: [],
                       primarySort:{fields:[{field: "count", direction:"desc"}]}});
        fail();
      } catch(e) {
        assertEqual(errors.ERROR_BAD_PARAMETER.code,
                    e.errorNum);
      }
      try {
        col.ensureIndex({type: 'inverted',
                       name: 'AllFieldsEmpty4',
                       includeAllFields:false,
                       primarySort:{fields:[{field: "count", direction:"desc"}]}});
        fail();
      } catch(e) {
        assertEqual(errors.ERROR_BAD_PARAMETER.code,
                    e.errorNum);
      }
    },
    testIndexHintedArrayComparisonAnyIn: function () {
      const query = aql`
        FOR d IN ${col} OPTIONS {indexHint: "InvertedIndexUnsorted"}
          FILTER [NOEVAL('value1'), 'value2'] ANY IN d.data_field
          SORT d.count DESC
          RETURN d`;
      const res = AQL_EXPLAIN(query.query, query.bindVars);
      const appliedRules = res.plan.rules;
      assertTrue(appliedRules.includes(useIndexes));
      assertTrue(appliedRules.includes(removeFilterCoveredByIndex));
      let executeRes = db._query(query.query, query.bindVars).toArray();
      assertEqual(docs / 50, executeRes.length);
    },
    testIndexHintedArrayComparisonAllIn: function () {
      const query = aql`
        FOR d IN ${col} OPTIONS {indexHint: "InvertedIndexUnsorted"}
          FILTER [NOEVAL('value1'), 'value2'] ALL IN d.data_field
          SORT d.count DESC
          RETURN d`;
      const res = AQL_EXPLAIN(query.query, query.bindVars);
      const appliedRules = res.plan.rules;
      assertTrue(appliedRules.includes(useIndexes));
      assertTrue(appliedRules.includes(removeFilterCoveredByIndex));
      let executeRes = db._query(query.query, query.bindVars).toArray();
      assertEqual(0, executeRes.length);
    },
    testIndexHintedArrayComparisonNoneIn: function () {
      const query = aql`
        FOR d IN ${col} OPTIONS {indexHint: "InvertedIndexUnsorted"}
          FILTER [NOEVAL('value1'), 'value2'] NONE IN d.data_field
          SORT d.count DESC
          RETURN d`;
      const res = AQL_EXPLAIN(query.query, query.bindVars);
      const appliedRules = res.plan.rules;
      assertTrue(appliedRules.includes(useIndexes));
      assertTrue(appliedRules.includes(removeFilterCoveredByIndex));
      let executeRes = db._query(query.query, query.bindVars).toArray();
      assertEqual(docs - docs / 50, executeRes.length);
    },
    testIndexHintedArrayComparisonAnyNotIn: function () {
      const query = aql`
        FOR d IN ${col} OPTIONS {indexHint: "InvertedIndexUnsorted"}
          FILTER [NOEVAL('value1'), 'value2'] ANY NOT IN d.data_field
          SORT d.count DESC
          RETURN d`;
      const res = AQL_EXPLAIN(query.query, query.bindVars);
      const appliedRules = res.plan.rules;
      assertTrue(appliedRules.includes(useIndexes));
      assertTrue(appliedRules.includes(removeFilterCoveredByIndex));
      let executeRes = db._query(query.query, query.bindVars).toArray();
      assertEqual(docs, executeRes.length);
    },
    testIndexHintedArrayComparisonAnyGE: function () {
      const query = aql`
        FOR d IN ${col} OPTIONS {indexHint: "InvertedIndexUnsorted"}
          FILTER [NOEVAL('value1'), 'value2'] ANY >= d.data_field
          SORT d.count DESC
          RETURN d`;
      const res = AQL_EXPLAIN(query.query, query.bindVars);
      const appliedRules = res.plan.rules;
      assertTrue(appliedRules.includes(useIndexes));
      assertTrue(appliedRules.includes(removeFilterCoveredByIndex));
      let executeRes = db._query(query.query, query.bindVars).toArray();
      // value2, value1, value0, value10..value19
      assertEqual(1300, executeRes.length);
    },
    testIndexHintedArrayComparisonAnyGT: function () {
      const query = aql`
        FOR d IN ${col} OPTIONS {indexHint: "InvertedIndexUnsorted"}
          FILTER [NOEVAL('value1'), 'value2'] ANY > d.data_field
          SORT d.count DESC
          RETURN d`;
      const res = AQL_EXPLAIN(query.query, query.bindVars);
      const appliedRules = res.plan.rules;
      assertTrue(appliedRules.includes(useIndexes));
      assertTrue(appliedRules.includes(removeFilterCoveredByIndex));
      let executeRes = db._query(query.query, query.bindVars).toArray();
      // value1, value0, value10..value19
      assertEqual(docs/100 * 12, executeRes.length);
    },
    testIndexHintedArrayComparisonAnyLE: function () {
      const query = aql`
        FOR d IN ${col} OPTIONS {indexHint: "InvertedIndexUnsorted"}
          FILTER [NOEVAL('value1'), 'value2'] ANY <= d.data_field
          SORT d.count DESC
          RETURN d`;
      const res = AQL_EXPLAIN(query.query, query.bindVars);
      const appliedRules = res.plan.rules;
      assertTrue(appliedRules.includes(useIndexes));
      assertTrue(appliedRules.includes(removeFilterCoveredByIndex));
      let executeRes = db._query(query.query, query.bindVars).toArray();
      // value0 will be rejected
      assertEqual(docs - docs/100, executeRes.length);
    },
    testIndexHintedArrayComparisonAnyLT: function () {
      const query = aql`
        FOR d IN ${col} OPTIONS {indexHint: "InvertedIndexUnsorted"}
          FILTER [NOEVAL('value1'), 'value2'] ANY < d.data_field
          SORT d.count DESC
          RETURN d`;
      const res = AQL_EXPLAIN(query.query, query.bindVars);
      const appliedRules = res.plan.rules;
      assertTrue(appliedRules.includes(useIndexes));
      assertTrue(appliedRules.includes(removeFilterCoveredByIndex));
      let executeRes = db._query(query.query, query.bindVars).toArray();
      // value0 and value1 will be rejected
      assertEqual(docs - docs/50, executeRes.length);
    },
    testIndexHintedArrayComparisonAnyEQ: function () {
      const query = aql`
        FOR d IN ${col} OPTIONS {indexHint: "InvertedIndexUnsorted"}
          FILTER [NOEVAL('value1'), 'value2'] ANY == d.data_field
          SORT d.count DESC
          RETURN d`;
      const res = AQL_EXPLAIN(query.query, query.bindVars);
      const appliedRules = res.plan.rules;
      assertTrue(appliedRules.includes(useIndexes));
      assertTrue(appliedRules.includes(removeFilterCoveredByIndex));
      let executeRes = db._query(query.query, query.bindVars).toArray();
      assertEqual(docs/50, executeRes.length);
    },
    testIndexHintedArrayComparisonAnyNE: function () {
      const query = aql`
        FOR d IN ${col} OPTIONS {indexHint: "InvertedIndexUnsorted"}
          FILTER [NOEVAL('value1'), 'value2'] ANY != d.data_field
          SORT d.count DESC
          RETURN d`;
      const res = AQL_EXPLAIN(query.query, query.bindVars);
      const appliedRules = res.plan.rules;
      assertTrue(appliedRules.includes(useIndexes));
      assertTrue(appliedRules.includes(removeFilterCoveredByIndex));
      let executeRes = db._query(query.query, query.bindVars).toArray();
      assertEqual(docs, executeRes.length);
    },
    testIndexHintedArrayComparisonAnyNE_IndexAccess: function () {
      const query = aql`
        FOR d IN ${col} OPTIONS {indexHint: "InvertedIndexUnsorted"}
          FILTER [[NOEVAL('value1'), 'value2'], ['value1', 'value1']][NOEVAL(0)] ANY != d.data_field
          SORT d.count DESC
          RETURN d`;
      const res = AQL_EXPLAIN(query.query, query.bindVars);
      const appliedRules = res.plan.rules;
      assertTrue(appliedRules.includes(useIndexes));
      assertTrue(appliedRules.includes(removeFilterCoveredByIndex));
      let executeRes = db._query(query.query, query.bindVars).toArray();
      assertEqual(docs, executeRes.length);
    },
    testIndexHintedArrayComparisonAnyNE_Fcall: function () {
      const query = aql`
        FOR d IN ${col} OPTIONS {indexHint: "InvertedIndexUnsorted"}
          FILTER NOEVAL([NOEVAL('value1'), 'value2']) ANY != d.data_field
          SORT d.count DESC
          RETURN d`;
      const res = AQL_EXPLAIN(query.query, query.bindVars);
      const appliedRules = res.plan.rules;
      assertTrue(appliedRules.includes(useIndexes));
      assertTrue(appliedRules.includes(removeFilterCoveredByIndex));
      let executeRes = db._query(query.query, query.bindVars).toArray();
      assertEqual(docs, executeRes.length);
    },
    testIndexHintedArrayComparisonAnyNE_NonArray: function () {
      try {
        const query = aql`
          FOR d IN ${col} OPTIONS {indexHint: "InvertedIndexUnsorted"}
            FILTER TOKENS("Bar", "text_en")[0] ANY != d.data_field
            SORT d.count DESC
            RETURN d`;
        db._query(query.query, query.bindVars).toArray();
        // query should fail but do not crash the server
        fail();
      } catch (e) {
        assertEqual(errors.ERROR_BAD_PARAMETER.code,
                    e.errorNum);
      }
    },
    testIndexHintedSearchFieldIgnored: function () {
      const query = aql`
        FOR d IN ${col} OPTIONS {indexHint: "InvertedIndexUnsorted"}
          FILTER d.searchField == 1
          SORT d.count DESC
          RETURN d`;
      const res = AQL_EXPLAIN(query.query, query.bindVars);
      const appliedRules = res.plan.rules;
      assertFalse(appliedRules.includes(useIndexes));
    },
    testDisjunctionOptimized: function () {
      const query = aql`
        FOR d IN ${col} OPTIONS {indexHint: "InvertedIndexUnsorted"}
          FILTER d.norm_field == 'fox' OR d.norm_field2 == 'box'
          RETURN d`;
      const res = AQL_EXPLAIN(query.query, query.bindVars);
      const appliedRules = res.plan.rules;
      assertTrue(appliedRules.includes(useIndexes));
      assertTrue(appliedRules.includes(removeFilterCoveredByIndex));
      let executeRes = db._query(query.query, query.bindVars).toArray();
      assertEqual(docs, executeRes.length);
    },
  };
}

jsunity.run(optimizerRuleInvertedIndexTestSuite);

return jsunity.done();
