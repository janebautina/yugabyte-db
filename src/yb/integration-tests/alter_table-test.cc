// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include <map>
#include <string>
#include <utility>
#include <boost/assign.hpp>
#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "yb/client/client.h"
#include "yb/client/client-test-util.h"
#include "yb/client/row_result.h"
#include "yb/client/schema.h"
#include "yb/gutil/gscoped_ptr.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/join.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/integration-tests/mini_cluster.h"
#include "yb/integration-tests/yb_mini_cluster_test_base.h"
#include "yb/master/mini_master.h"
#include "yb/master/master.h"
#include "yb/master/master.pb.h"
#include "yb/master/master-test-util.h"
#include "yb/server/hybrid_clock.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_tablet_manager.h"
#include "yb/util/atomic.h"
#include "yb/util/faststring.h"
#include "yb/util/random.h"
#include "yb/util/stopwatch.h"
#include "yb/util/test_util.h"

DECLARE_bool(enable_data_block_fsync);
DECLARE_bool(enable_maintenance_manager);
DECLARE_bool(flush_rocksdb_on_shutdown);
DECLARE_int32(heartbeat_interval_ms);
DECLARE_int32(flush_threshold_mb);
DECLARE_bool(use_hybrid_clock);

namespace yb {

using client::YBClient;
using client::YBClientBuilder;
using client::YBColumnSchema;
using client::YBError;
using client::YBInsert;
using client::YBRowResult;
using client::YBScanner;
using client::YBSchema;
using client::YBSchemaBuilder;
using client::YBSession;
using client::YBTable;
using client::YBTableAlterer;
using client::YBTableCreator;
using client::YBTableName;
using client::YBTableType;
using client::YBUpdate;
using client::YBValue;
using std::shared_ptr;
using master::AlterTableRequestPB;
using master::AlterTableResponsePB;
using master::MiniMaster;
using std::map;
using std::pair;
using std::vector;
using tablet::TabletPeer;
using tserver::MiniTabletServer;

class AlterTableTest : public YBMiniClusterTestBase<MiniCluster> {
 public:
  AlterTableTest()
    : stop_threads_(false),
      inserted_idx_(0) {

    YBSchemaBuilder b;
    b.AddColumn("c0")->Type(INT32)->NotNull()->PrimaryKey();
    b.AddColumn("c1")->Type(INT32)->NotNull();
    CHECK_OK(b.Build(&schema_));

    FLAGS_enable_data_block_fsync = false; // Keep unit tests fast.
    FLAGS_use_hybrid_clock = false;
    ANNOTATE_BENIGN_RACE(&FLAGS_flush_threshold_mb,
                         "safe to change at runtime");
    ANNOTATE_BENIGN_RACE(&FLAGS_enable_maintenance_manager,
                         "safe to change at runtime");
  }

  void SetUp() override {
    // Make heartbeats faster to speed test runtime.
    FLAGS_heartbeat_interval_ms = 10;

    YBMiniClusterTestBase::SetUp();

    MiniClusterOptions opts;
    opts.num_tablet_servers = num_replicas();
    cluster_.reset(new MiniCluster(env_.get(), opts));
    ASSERT_OK(cluster_->Start());
    ASSERT_OK(cluster_->WaitForTabletServerCount(num_replicas()));

    CHECK_OK(YBClientBuilder()
             .add_master_server_addr(cluster_->mini_master()->bound_rpc_addr_str())
             .default_admin_operation_timeout(MonoDelta::FromSeconds(60))
             .Build(&client_));

    CHECK_OK(client_->CreateNamespaceIfNotExists(kTableName.namespace_name()));

    // Add a table, make sure it reports itself.
    gscoped_ptr<YBTableCreator> table_creator(client_->NewTableCreator());
    CHECK_OK(table_creator->table_name(kTableName)
             .schema(&schema_)
             .table_type(YBTableType::YQL_TABLE_TYPE)
             .num_replicas(num_replicas())
             .Create());

    if (num_replicas() == 1) {
      tablet_peer_ = LookupTabletPeer();
    }
    LOG(INFO) << "Tablet successfully located";
  }

  void DoTearDown() override {
    tablet_peer_.reset();
    cluster_->Shutdown();
  }

  scoped_refptr<TabletPeer> LookupTabletPeer() {
    vector<scoped_refptr<TabletPeer> > peers;
    cluster_->mini_tablet_server(0)->server()->tablet_manager()->GetTabletPeers(&peers);
    CHECK_EQ(1, peers.size());
    return peers[0];
  }

  void ShutdownTS() {
    // Drop the tablet_peer_ reference since the tablet peer becomes invalid once
    // we shut down the server. Additionally, if we hold onto the reference,
    // we'll end up calling the destructor from the test code instead of the
    // normal location, which can cause crashes, etc.
    tablet_peer_.reset();
    if (cluster_->mini_tablet_server(0)->server() != nullptr) {
      cluster_->mini_tablet_server(0)->Shutdown();
    }
  }

  void RestartTabletServer(int idx = 0) {
    tablet_peer_.reset();
    if (cluster_->mini_tablet_server(idx)->server()) {
      ASSERT_OK(cluster_->mini_tablet_server(idx)->Restart());
    } else {
      ASSERT_OK(cluster_->mini_tablet_server(idx)->Start());
    }

    ASSERT_OK(cluster_->mini_tablet_server(idx)->WaitStarted());
    if (idx == 0) {
      tablet_peer_ = LookupTabletPeer();
    }
  }

  Status WaitAlterTableCompletion(const YBTableName& table_name, int attempts) {
    int wait_time = 1000;
    for (int i = 0; i < attempts; ++i) {
      bool in_progress;
      RETURN_NOT_OK(client_->IsAlterTableInProgress(table_name, &in_progress));
      if (!in_progress) {
        return Status::OK();
      }

      SleepFor(MonoDelta::FromMicroseconds(wait_time));
      wait_time = std::min(wait_time * 5 / 4, 1000000);
    }

    return STATUS(TimedOut, "AlterTable not completed within the timeout");
  }

  Status AddNewI32Column(const YBTableName& table_name,
                         const string& column_name,
                         int32_t default_value) {
    return AddNewI32Column(table_name, column_name, default_value,
                           MonoDelta::FromSeconds(60));
  }

  Status AddNewI32Column(const YBTableName& table_name,
                         const string& column_name,
                         int32_t default_value,
                         const MonoDelta& timeout) {
    gscoped_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(table_name));
    table_alterer->AddColumn(column_name)->Type(INT32)->
      NotNull()->Default(YBValue::FromInt(default_value));
    return table_alterer->timeout(timeout)->Alter();
  }

  enum VerifyPattern {
    C1_MATCHES_INDEX,
    C1_IS_DEADBEEF,
    C1_DOESNT_EXIST
  };

  void VerifyRows(int start_row, int num_rows, VerifyPattern pattern);

  void InsertRows(int start_row, int num_rows);

  void UpdateRow(int32_t row_key, const map<string, int32_t>& updates);

  void ScanToStrings(vector<string>* rows);

  void InserterThread();
  void UpdaterThread();
  void ScannerThread();

  Status CreateSplitTable(const YBTableName& table_name) {
    vector<const YBPartialRow*> split_rows;
    for (int32_t i = 1; i < 10; i++) {
      YBPartialRow* row = schema_.NewRow();
      CHECK_OK(row->SetInt32(0, i * 100));
      split_rows.push_back(row);
    }

    RETURN_NOT_OK(client_->CreateNamespaceIfNotExists(table_name.namespace_name()));

    gscoped_ptr<YBTableCreator> table_creator(client_->NewTableCreator());
    return table_creator->table_name(table_name)
        .schema(&schema_)
        .num_replicas(num_replicas())
        .split_rows(split_rows)
        .Create();
  }

 protected:
  virtual int num_replicas() const { return 1; }

  static const YBTableName kTableName;

  shared_ptr<YBClient> client_;

  YBSchema schema_;

  scoped_refptr<TabletPeer> tablet_peer_;

  AtomicBool stop_threads_;

  // The index of the last row inserted by InserterThread.
  // UpdaterThread uses this to figure out which rows can be
  // safely updated.
  AtomicInt<int32_t> inserted_idx_;
};

// Subclass which creates three servers and a replicated cluster.
class ReplicatedAlterTableTest : public AlterTableTest {
 protected:
  int num_replicas() const override { return 3; }
};

const YBTableName AlterTableTest::kTableName("my_keyspace", "fake-table");

// Simple test to verify that the "alter table" command sent and executed
// on the TS handling the tablet of the altered table.
// TODO: create and verify multiple tablets when the client will support that.
TEST_F(AlterTableTest, TestTabletReports) {
  ASSERT_EQ(0, tablet_peer_->tablet()->metadata()->schema_version());
  ASSERT_OK(AddNewI32Column(kTableName, "new-i32", 0));
  ASSERT_EQ(1, tablet_peer_->tablet()->metadata()->schema_version());
}

// Verify that adding an existing column will return an "already present" error
TEST_F(AlterTableTest, TestAddExistingColumn) {
  ASSERT_EQ(0, tablet_peer_->tablet()->metadata()->schema_version());

  {
    Status s = AddNewI32Column(kTableName, "c1", 0);
    ASSERT_TRUE(s.IsAlreadyPresent());
    ASSERT_STR_CONTAINS(s.ToString(), "The column already exists: c1");
  }

  ASSERT_EQ(0, tablet_peer_->tablet()->metadata()->schema_version());
}

// Verify that adding a NOT NULL column without defaults will return an error.
//
// This doesn't use the YBClient because it's trying to make an invalid request.
// Our APIs for the client are designed such that it's impossible to send such
// a request.
TEST_F(AlterTableTest, TestAddNotNullableColumnWithoutDefaults) {
  ASSERT_EQ(0, tablet_peer_->tablet()->metadata()->schema_version());

  {
    AlterTableRequestPB req;
    kTableName.SetIntoTableIdentifierPB(req.mutable_table());

    AlterTableRequestPB::Step *step = req.add_alter_schema_steps();
    step->set_type(AlterTableRequestPB::ADD_COLUMN);
    ColumnSchemaToPB(ColumnSchema("c2", INT32),
                     step->mutable_add_column()->mutable_schema());
    AlterTableResponsePB resp;
    Status s = cluster_->mini_master()->master()->catalog_manager()->AlterTable(
      &req, &resp, nullptr);
    ASSERT_TRUE(s.IsInvalidArgument());
    ASSERT_STR_CONTAINS(s.ToString(), "column `c2`: NOT NULL columns must have a default");
  }

  ASSERT_EQ(0, tablet_peer_->tablet()->metadata()->schema_version());
}

// Adding a nullable column with no default value should be equivalent
// to a NULL default.
TEST_F(AlterTableTest, TestAddNullableColumnWithoutDefault) {
  InsertRows(0, 1);
  ASSERT_OK(tablet_peer_->tablet()->Flush(tablet::FlushMode::kSync));

  {
    gscoped_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
    table_alterer->AddColumn("new")->Type(INT32);
    ASSERT_OK(table_alterer->Alter());
  }

  InsertRows(1, 1);

  vector<string> rows;
  ScanToStrings(&rows);
  ASSERT_EQ(2, rows.size());
  EXPECT_EQ("(int32 c0=0, int32 c1=0, int32 new=NULL)", rows[0]);
  EXPECT_EQ("(int32 c0=16777216, int32 c1=1, int32 new=NULL)", rows[1]);
}

// Verify that, if a tablet server is down when an alter command is issued,
// it will eventually receive the command when it restarts.
TEST_F(AlterTableTest, TestAlterOnTSRestart) {
  ASSERT_EQ(0, tablet_peer_->tablet()->metadata()->schema_version());

  ShutdownTS();

  // Send the Alter request
  {
    Status s = AddNewI32Column(kTableName, "new-32", 10,
                               MonoDelta::FromMilliseconds(500));
    ASSERT_TRUE(s.IsTimedOut());
  }

  // Verify that the Schema is the old one
  YBSchema schema;
  bool alter_in_progress = false;
  ASSERT_OK(client_->GetTableSchema(kTableName, &schema));
  ASSERT_TRUE(schema_.Equals(schema));
  ASSERT_OK(client_->IsAlterTableInProgress(kTableName, &alter_in_progress));
  ASSERT_TRUE(alter_in_progress);

  // Restart the TS and wait for the new schema
  RestartTabletServer();
  ASSERT_OK(WaitAlterTableCompletion(kTableName, 50));
  ASSERT_EQ(1, tablet_peer_->tablet()->metadata()->schema_version());
}

// Verify that nothing is left behind on cluster shutdown with pending async tasks
TEST_F(AlterTableTest, TestShutdownWithPendingTasks) {
  DontVerifyClusterBeforeNextTearDown();
  ASSERT_EQ(0, tablet_peer_->tablet()->metadata()->schema_version());

  ShutdownTS();

  // Send the Alter request
  {
    Status s = AddNewI32Column(kTableName, "new-i32", 10,
                               MonoDelta::FromMilliseconds(500));
    ASSERT_TRUE(s.IsTimedOut());
  }
}

// Verify that the new schema is applied/reported even when
// the TS is going down with the alter operation in progress.
// On TS restart the master should:
//  - get the new schema state, and mark the alter as complete
//  - get the old schema state, and ask the TS again to perform the alter.
TEST_F(AlterTableTest, TestRestartTSDuringAlter) {
  if (!AllowSlowTests()) {
    LOG(INFO) << "Skipping slow test";
    return;
  }

  ASSERT_EQ(0, tablet_peer_->tablet()->metadata()->schema_version());

  Status s = AddNewI32Column(kTableName, "new-i32", 10,
                             MonoDelta::FromMilliseconds(1));
  ASSERT_TRUE(s.IsTimedOut());

  // Restart the TS while alter is running
  for (int i = 0; i < 3; i++) {
    SleepFor(MonoDelta::FromMicroseconds(500));
    RestartTabletServer();
  }

  // Wait for the new schema
  ASSERT_OK(WaitAlterTableCompletion(kTableName, 50));
  ASSERT_EQ(1, tablet_peer_->tablet()->metadata()->schema_version());
}

TEST_F(AlterTableTest, TestGetSchemaAfterAlterTable) {
  ASSERT_OK(AddNewI32Column(kTableName, "new-i32", 10));

  YBSchema s;
  ASSERT_OK(client_->GetTableSchema(kTableName, &s));
}

void AlterTableTest::InsertRows(int start_row, int num_rows) {
  shared_ptr<YBSession> session = client_->NewSession();
  shared_ptr<YBTable> table;
  CHECK_OK(session->SetFlushMode(YBSession::MANUAL_FLUSH));
  session->SetTimeoutMillis(15 * 1000);
  CHECK_OK(client_->OpenTable(kTableName, &table));

  // Insert a bunch of rows with the current schema
  for (int i = start_row; i < start_row + num_rows; i++) {
    shared_ptr<YBInsert> insert(table->NewInsert());
    // Endian-swap the key so that we spew inserts randomly
    // instead of just a sequential write pattern. This way
    // compactions may actually be triggered.
    int32_t key = bswap_32(i);
    CHECK_OK(insert->mutable_row()->SetInt32(0, key));

    if (table->schema().num_columns() > 1) {
      CHECK_OK(insert->mutable_row()->SetInt32(1, i));
    }

    CHECK_OK(session->Apply(insert));

    if (i % 50 == 0) {
      FlushSessionOrDie(session);
    }
  }

  FlushSessionOrDie(session);
}

void AlterTableTest::UpdateRow(int32_t row_key,
                               const map<string, int32_t>& updates) {
  shared_ptr<YBSession> session = client_->NewSession();
  shared_ptr<YBTable> table;
  CHECK_OK(client_->OpenTable(kTableName, &table));
  CHECK_OK(session->SetFlushMode(YBSession::MANUAL_FLUSH));
  session->SetTimeoutMillis(15 * 1000);
  shared_ptr<YBUpdate> update(table->NewUpdate());
  int32_t key = bswap_32(row_key); // endian swap to match 'InsertRows'
  CHECK_OK(update->mutable_row()->SetInt32(0, key));
  typedef map<string, int32_t>::value_type entry;
  for (const entry& e : updates) {
    CHECK_OK(update->mutable_row()->SetInt32(e.first, e.second));
  }
  CHECK_OK(session->Apply(update));
  FlushSessionOrDie(session);
}

void AlterTableTest::ScanToStrings(vector<string>* rows) {
  shared_ptr<YBTable> table;
  CHECK_OK(client_->OpenTable(kTableName, &table));
  ScanTableToStrings(table.get(), rows);
  std::sort(rows->begin(), rows->end());
}

// Verify that the 'num_rows' starting with 'start_row' fit the given pattern.
// Note that the 'start_row' here is not a row key, but the pre-transformation row
// key (InsertRows swaps endianness so that we random-write instead of sequential-write)
void AlterTableTest::VerifyRows(int start_row, int num_rows, VerifyPattern pattern) {
  shared_ptr<YBTable> table;
  CHECK_OK(client_->OpenTable(kTableName, &table));
  YBScanner scanner(table.get());
  CHECK_OK(scanner.SetSelection(YBClient::LEADER_ONLY));
  CHECK_OK(scanner.Open());

  int verified = 0;
  vector<YBRowResult> results;
  while (scanner.HasMoreRows()) {
    CHECK_OK(scanner.NextBatch(&results));

    for (const YBRowResult& row : results) {
      int32_t key = 0;
      CHECK_OK(row.GetInt32(0, &key));
      int32_t row_idx = bswap_32(key);
      if (row_idx < start_row || row_idx >= start_row + num_rows) {
        // Outside the range we're verifying
        continue;
      }
      verified++;

      if (pattern == C1_DOESNT_EXIST) {
        continue;
      }

      int32_t c1 = 0;
      CHECK_OK(row.GetInt32(1, &c1));

      switch (pattern) {
        case C1_MATCHES_INDEX:
          CHECK_EQ(row_idx, c1);
          break;
        case C1_IS_DEADBEEF:
          CHECK_EQ(0xdeadbeef, c1);
          break;
        default:
          LOG(FATAL);
      }
    }
  }
  CHECK_EQ(verified, num_rows);
}

// Test inserting/updating some data, dropping a column, and adding a new one
// with the same name. Data should not "reappear" from the old column.
//
// This is a regression test for KUDU-461.
TEST_F(AlterTableTest, TestDropAndAddNewColumn) {
  // Reduce flush threshold so that we get both on-disk data
  // for the alter as well as in-MRS data.
  // This also increases chances of a race.
  FLAGS_flush_threshold_mb = 3;

  const int kNumRows = AllowSlowTests() ? 100000 : 1000;
  InsertRows(0, kNumRows);

  LOG(INFO) << "Verifying initial pattern";
  VerifyRows(0, kNumRows, C1_MATCHES_INDEX);

  LOG(INFO) << "Dropping and adding back c1";
  gscoped_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
  ASSERT_OK(table_alterer->DropColumn("c1")
            ->Alter());

  ASSERT_OK(AddNewI32Column(kTableName, "c1", 0xdeadbeef));

  LOG(INFO) << "Verifying that the new default shows up";
  VerifyRows(0, kNumRows, C1_IS_DEADBEEF);


}

TEST_F(AlterTableTest, TestCompactionAfterDrop) {
  LOG(INFO) << "Inserting rows";
  InsertRows(0, 3);

  std::string docdb_dump = tablet_peer_->tablet()->DocDBDumpStrInTest();
  // DocDB should not be empty right now.
  ASSERT_NE(0, docdb_dump.length());

  LOG(INFO) << "Dropping c1";
  gscoped_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
  ASSERT_OK(table_alterer->DropColumn("c1")->Alter());

  LOG(INFO) << "Forcing compaction";
  tablet_peer_->tablet()->ForceRocksDBCompactInTest();

  docdb_dump = tablet_peer_->tablet()->DocDBDumpStrInTest();

  LOG(INFO) << "Checking that docdb is empty";
  ASSERT_EQ(0, docdb_dump.length());

  ASSERT_OK(cluster_->RestartSync());
  tablet_peer_ = LookupTabletPeer();
}

// This tests the scenario where the log entries immediately after last RocksDB flush are for a
// different schema than the one that was last flushed to the superblock.
TEST_F(AlterTableTest, TestLogSchemaReplay) {
  vector<string> rows;

  ASSERT_OK(AddNewI32Column(kTableName, "c2", 12345));
  InsertRows(0, 2);
  UpdateRow(1, { {"c1", 0} });

  LOG(INFO) << "Flushing RocksDB";
  ASSERT_OK(tablet_peer_->tablet()->Flush(tablet::FlushMode::kSync));

  UpdateRow(0, { {"c1", 1}, {"c2", 10001} });

  LOG(INFO) << "Dropping c1";
  gscoped_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
  ASSERT_OK(table_alterer->DropColumn("c1")->Alter());

  UpdateRow(1, { {"c2", 10002} });

  ASSERT_NO_FATALS(ScanToStrings(&rows));
  ASSERT_EQ(2, rows.size());
  ASSERT_EQ("(int32 c0=0, int32 c2=10001)", rows[0]);
  ASSERT_EQ("(int32 c0=16777216, int32 c2=10002)", rows[1]);

  google::FlagSaver flag_saver;
  // Restart without flushing RocksDB
  FLAGS_flush_rocksdb_on_shutdown = false;
  LOG(INFO) << "Restarting tablet";
  ASSERT_NO_FATALS(RestartTabletServer());

  ASSERT_NO_FATALS(ScanToStrings(&rows));
  ASSERT_EQ(2, rows.size());
  ASSERT_EQ("(int32 c0=0, int32 c2=10001)", rows[0]);
  ASSERT_EQ("(int32 c0=16777216, int32 c2=10002)", rows[1]);
}

// Tests that a renamed table can still be altered. This is a regression test, we used to not carry
// over column ids after a table rename.
TEST_F(AlterTableTest, TestRenameTableAndAdd) {
  gscoped_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
  YBTableName new_name(kTableName.namespace_name(), "someothername");
  ASSERT_OK(table_alterer->RenameTo(new_name)
            ->Alter());

  ASSERT_OK(AddNewI32Column(new_name, "new", 0xdeadbeef));
}

// Test restarting a tablet server several times after various
// schema changes.
// This is a regression test for KUDU-462.
TEST_F(AlterTableTest, TestBootstrapAfterAlters) {
  vector<string> rows;

  ASSERT_OK(AddNewI32Column(kTableName, "c2", 12345));
  InsertRows(0, 1);
  ASSERT_OK(tablet_peer_->tablet()->Flush(tablet::FlushMode::kSync));
  InsertRows(1, 1);

  UpdateRow(0, { {"c1", 10001} });
  UpdateRow(1, { {"c1", 10002} });

  ASSERT_NO_FATALS(ScanToStrings(&rows));
  ASSERT_EQ(2, rows.size());
  ASSERT_EQ("(int32 c0=0, int32 c1=10001, int32 c2=12345)", rows[0]);
  ASSERT_EQ("(int32 c0=16777216, int32 c1=10002, int32 c2=12345)", rows[1]);

  LOG(INFO) << "Dropping c1";
  gscoped_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
  ASSERT_OK(table_alterer->DropColumn("c1")->Alter());

  ASSERT_NO_FATALS(ScanToStrings(&rows));
  ASSERT_EQ(2, rows.size());
  ASSERT_EQ("(int32 c0=0, int32 c2=12345)", rows[0]);
  ASSERT_EQ("(int32 c0=16777216, int32 c2=12345)", rows[1]);

  // Test that restart doesn't fail when trying to replay updates or inserts
  // with the dropped column.
  ASSERT_NO_FATALS(RestartTabletServer());

  ASSERT_NO_FATALS(ScanToStrings(&rows));
  ASSERT_EQ(2, rows.size());
  ASSERT_EQ("(int32 c0=0, int32 c2=12345)", rows[0]);
  ASSERT_EQ("(int32 c0=16777216, int32 c2=12345)", rows[1]);

  // Add back a column called 'c2', but should not materialize old data.
  ASSERT_OK(AddNewI32Column(kTableName, "c1", 20000));
  ASSERT_NO_FATALS(ScanToStrings(&rows));
  ASSERT_EQ(2, rows.size());
  ASSERT_EQ("(int32 c0=0, int32 c2=12345, int32 c1=20000)", rows[0]);
  ASSERT_EQ("(int32 c0=16777216, int32 c2=12345, int32 c1=20000)", rows[1]);

  ASSERT_NO_FATALS(RestartTabletServer());
  ASSERT_NO_FATALS(ScanToStrings(&rows));
  ASSERT_EQ(2, rows.size());
  ASSERT_EQ("(int32 c0=0, int32 c2=12345, int32 c1=20000)", rows[0]);
  ASSERT_EQ("(int32 c0=16777216, int32 c2=12345, int32 c1=20000)", rows[1]);
}

TEST_F(AlterTableTest, TestCompactAfterUpdatingRemovedColumn) {
  // Disable maintenance manager, since we manually flush/compact
  // in this test.
  FLAGS_enable_maintenance_manager = false;

  vector<string> rows;

  ASSERT_OK(AddNewI32Column(kTableName, "c2", 12345));
  InsertRows(0, 1);
  ASSERT_OK(tablet_peer_->tablet()->Flush(tablet::FlushMode::kSync));
  InsertRows(1, 1);
  ASSERT_OK(tablet_peer_->tablet()->Flush(tablet::FlushMode::kSync));


  ASSERT_NO_FATALS(ScanToStrings(&rows));
  ASSERT_EQ(2, rows.size());
  ASSERT_EQ("(int32 c0=0, int32 c1=0, int32 c2=12345)", rows[0]);
  ASSERT_EQ("(int32 c0=16777216, int32 c1=1, int32 c2=12345)", rows[1]);

  // Add a delta for c1.
  UpdateRow(0, { {"c1", 54321} });

  // Drop c1.
  LOG(INFO) << "Dropping c1";
  gscoped_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
  ASSERT_OK(table_alterer->DropColumn("c1")->Alter());

  ASSERT_NO_FATALS(ScanToStrings(&rows));
  ASSERT_EQ(2, rows.size());
  ASSERT_EQ("(int32 c0=0, int32 c2=12345)", rows[0]);

  // Compact
  ASSERT_OK(tablet_peer_->tablet()->Compact(tablet::Tablet::FORCE_COMPACT_ALL));
}


// Thread which inserts rows into the table.
// After each batch of rows is inserted, inserted_idx_ is updated
// to communicate how much data has been written (and should now be
// updateable)
void AlterTableTest::InserterThread() {
  shared_ptr<YBSession> session = client_->NewSession();
  shared_ptr<YBTable> table;
  CHECK_OK(session->SetFlushMode(YBSession::MANUAL_FLUSH));
  session->SetTimeoutMillis(15 * 1000);

  CHECK_OK(client_->OpenTable(kTableName, &table));
  int32_t i = 0;
  while (!stop_threads_.Load()) {
    shared_ptr<YBInsert> insert(table->NewInsert());
    // Endian-swap the key so that we spew inserts randomly
    // instead of just a sequential write pattern. This way
    // compactions may actually be triggered.
    int32_t key = bswap_32(i++);
    CHECK_OK(insert->mutable_row()->SetInt32(0, key));
    CHECK_OK(insert->mutable_row()->SetInt32(1, i));
    CHECK_OK(session->Apply(insert));

    if (i % 50 == 0) {
      FlushSessionOrDie(session);
      inserted_idx_.Store(i);
    }
  }

  FlushSessionOrDie(session);
  inserted_idx_.Store(i);
}

// Thread which follows behind the InserterThread and generates random
// updates across the previously inserted rows.
void AlterTableTest::UpdaterThread() {
  shared_ptr<YBSession> session = client_->NewSession();
  shared_ptr<YBTable> table;
  CHECK_OK(session->SetFlushMode(YBSession::MANUAL_FLUSH));
  session->SetTimeoutMillis(15 * 1000);

  CHECK_OK(client_->OpenTable(kTableName, &table));

  Random rng(1);
  int32_t i = 0;
  while (!stop_threads_.Load()) {
    shared_ptr<YBUpdate> update(table->NewUpdate());

    int32_t max = inserted_idx_.Load();
    if (max == 0) {
      // Inserter hasn't inserted anything yet, so we have nothing to update.
      SleepFor(MonoDelta::FromMicroseconds(100));
      continue;
    }
    // Endian-swap the key to match the way the InserterThread generates
    // keys to insert.
    int32_t key = bswap_32(rng.Uniform(max));
    CHECK_OK(update->mutable_row()->SetInt32(0, key));
    CHECK_OK(update->mutable_row()->SetInt32(1, i));
    CHECK_OK(session->Apply(update));

    if (i++ % 50 == 0) {
      FlushSessionOrDie(session);
    }
  }

  FlushSessionOrDie(session);
}

// Thread which loops reading data from the table.
// No verification is performed.
void AlterTableTest::ScannerThread() {
  shared_ptr<YBTable> table;
  CHECK_OK(client_->OpenTable(kTableName, &table));
  while (!stop_threads_.Load()) {
    YBScanner scanner(table.get());
    int inserted_at_scanner_start = inserted_idx_.Load();
    CHECK_OK(scanner.Open());
    int count = 0;
    vector<YBRowResult> results;
    while (scanner.HasMoreRows()) {
      CHECK_OK(scanner.NextBatch(&results));
      count += results.size();
    }

    LOG(INFO) << "Scanner saw " << count << " rows";
    // We may have gotten more rows than we expected, because inserts
    // kept going while we set up the scan. But, we should never get
    // fewer.
    CHECK_GE(count, inserted_at_scanner_start)
      << "We didn't get as many rows as expected";
  }
}

// Test altering a table while also sending a lot of writes,
// checking for races between the two.
TEST_F(AlterTableTest, TestAlterUnderWriteLoad) {
  // Increase chances of a race between flush and alter.
  FLAGS_flush_threshold_mb = 3;

  scoped_refptr<Thread> writer;
  CHECK_OK(Thread::Create(
      "test", "inserter", std::bind(&AlterTableTest::InserterThread, this), &writer));

  scoped_refptr<Thread> updater;
  CHECK_OK(
      Thread::Create("test", "updater", std::bind(&AlterTableTest::UpdaterThread, this), &updater));

  scoped_refptr<Thread> scanner;
  CHECK_OK(
      Thread::Create("test", "scanner", std::bind(&AlterTableTest::ScannerThread, this), &scanner));

  // Add columns until we reach 10.
  for (int i = 2; i < 10; i++) {
    if (AllowSlowTests()) {
      // In slow test mode, let more writes accumulate in between
      // alters, so that we get enough writes to cause flushes,
      // compactions, etc.
      SleepFor(MonoDelta::FromSeconds(3));
    }

    ASSERT_OK(AddNewI32Column(kTableName,
                                     strings::Substitute("c$0", i),
                                     i));
  }

  stop_threads_.Store(true);
  writer->Join();
  updater->Join();
  scanner->Join();
}

TEST_F(AlterTableTest, TestInsertAfterAlterTable) {
  YBTableName kSplitTableName("my_keyspace", "split-table");

  // Create a new table with 10 tablets.
  //
  // With more tablets, there's a greater chance that the TS will heartbeat
  // after some but not all tablets have finished altering.
  ASSERT_OK(CreateSplitTable(kSplitTableName));

  // Add a column, and immediately try to insert a row including that
  // new column.
  ASSERT_OK(AddNewI32Column(kSplitTableName, "new-i32", 10));
  shared_ptr<YBTable> table;
  ASSERT_OK(client_->OpenTable(kSplitTableName, &table));
  shared_ptr<YBInsert> insert(table->NewInsert());
  ASSERT_OK(insert->mutable_row()->SetInt32("c0", 1));
  ASSERT_OK(insert->mutable_row()->SetInt32("c1", 1));
  ASSERT_OK(insert->mutable_row()->SetInt32("new-i32", 1));
  shared_ptr<YBSession> session = client_->NewSession();
  ASSERT_OK(session->SetFlushMode(YBSession::MANUAL_FLUSH));
  session->SetTimeoutMillis(15000);
  ASSERT_OK(session->Apply(insert));
  Status s = session->Flush();
  if (!s.ok()) {
    ASSERT_EQ(1, session->CountPendingErrors());
    client::CollectedErrors errors;
    bool overflow;
    session->GetPendingErrors(&errors, &overflow);
    ASSERT_FALSE(overflow);
    ASSERT_EQ(1, errors.size());
    ASSERT_OK(errors[0]->status()); // will fail
  }
}

// Issue a bunch of alter tables in quick succession. Regression for a bug
// seen in an earlier implementation of "alter table" where these could
// conflict with each other.
TEST_F(AlterTableTest, TestMultipleAlters) {
  YBTableName kSplitTableName("my_keyspace", "split-table");
  const size_t kNumNewCols = 10;
  const int32_t kDefaultValue = 10;

  // Create a new table with 10 tablets.
  //
  // With more tablets, there's a greater chance that the TS will heartbeat
  // after some but not all tablets have finished altering.
  ASSERT_OK(CreateSplitTable(kSplitTableName));

  // Issue a bunch of new alters without waiting for them to finish.
  for (int i = 0; i < kNumNewCols; i++) {
    gscoped_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kSplitTableName));
    table_alterer->AddColumn(strings::Substitute("new_col$0", i))
      ->Type(INT32)->NotNull()
      ->Default(YBValue::FromInt(kDefaultValue));
    ASSERT_OK(table_alterer->wait(false)->Alter());
  }

  // Now wait. This should block on all of them.
  WaitAlterTableCompletion(kSplitTableName, 50);

  // All new columns should be present.
  YBSchema new_schema;
  ASSERT_OK(client_->GetTableSchema(kSplitTableName, &new_schema));
  ASSERT_EQ(kNumNewCols + schema_.num_columns(), new_schema.num_columns());
}

TEST_F(ReplicatedAlterTableTest, TestReplicatedAlter) {
  const int kNumRows = 100;
  InsertRows(0, kNumRows);

  LOG(INFO) << "Verifying initial pattern";
  VerifyRows(0, kNumRows, C1_MATCHES_INDEX);

  LOG(INFO) << "Dropping and adding back c1";
  gscoped_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
  ASSERT_OK(table_alterer->DropColumn("c1")->Alter());

  ASSERT_OK(AddNewI32Column(kTableName, "c1", 0xdeadbeef));

  bool alter_in_progress;
  ASSERT_OK(client_->IsAlterTableInProgress(kTableName, &alter_in_progress));
  ASSERT_FALSE(alter_in_progress);

  LOG(INFO) << "Verifying that the new default shows up";
  VerifyRows(0, kNumRows, C1_IS_DEADBEEF);
}

}  // namespace yb
