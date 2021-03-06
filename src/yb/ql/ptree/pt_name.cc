//--------------------------------------------------------------------------------------------------
// Copyright (c) YugaByte, Inc.
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
//
// Treenode definitions for all name nodes.
//--------------------------------------------------------------------------------------------------

#include "yb/ql/ptree/pt_name.h"
#include "yb/ql/ptree/sem_context.h"

namespace yb {
namespace ql {

//--------------------------------------------------------------------------------------------------

PTName::PTName(MemoryContext *memctx,
               YBLocation::SharedPtr loc,
               const MCSharedPtr<MCString>& name)
    : TreeNode(memctx, loc),
      name_(name) {
}

PTName::~PTName() {
}

CHECKED_STATUS PTName::SetupPrimaryKey(SemContext *sem_context) {
  PTColumnDefinition *column = sem_context->GetColumnDefinition(*name_);
  if (column == nullptr) {
    LOG(INFO) << "Column \"" << *name_ << "\" doesn't exist";
    return sem_context->Error(this, "Column does not exist", ErrorCode::UNDEFINED_COLUMN);
  }
  column->set_is_primary_key();

  // Add the analyzed column to table.
  PTCreateTable *table = sem_context->current_create_table_stmt();
  RETURN_NOT_OK(table->AppendPrimaryColumn(sem_context, column));

  return Status::OK();
}

CHECKED_STATUS PTName::SetupHashAndPrimaryKey(SemContext *sem_context) {
  PTColumnDefinition *column = sem_context->GetColumnDefinition(*name_);
  if (column == nullptr) {
    LOG(INFO) << "Column \"" << *name_ << "\" doesn't exist";
    return sem_context->Error(this, "Column does not exist", ErrorCode::UNDEFINED_COLUMN);
  }
  column->set_is_hash_key();

  // Add the analyzed column to table.
  PTCreateTable *table = sem_context->current_create_table_stmt();
  RETURN_NOT_OK(table->AppendHashColumn(sem_context, column));

  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

PTNameAll::PTNameAll(MemoryContext *memctx, YBLocation::SharedPtr loc)
    : PTName(memctx, loc, MCMakeShared<MCString>(memctx, "*")) {
}

PTNameAll::~PTNameAll() {
}

//--------------------------------------------------------------------------------------------------

PTQualifiedName::PTQualifiedName(MemoryContext *memctx,
                                 YBLocation::SharedPtr loc,
                                 const PTName::SharedPtr& ptname)
    : PTName(memctx, loc),
      ptnames_(memctx) {
  Append(ptname);
}

PTQualifiedName::PTQualifiedName(MemoryContext *memctx,
                                 YBLocation::SharedPtr loc,
                                 const MCSharedPtr<MCString>& name)
    : PTName(memctx, loc),
      ptnames_(memctx) {
  Append(PTName::MakeShared(memctx, loc, name));
}

PTQualifiedName::~PTQualifiedName() {
}

void PTQualifiedName::Append(const PTName::SharedPtr& ptname) {
  ptnames_.push_back(ptname);
}

void PTQualifiedName::Prepend(const PTName::SharedPtr& ptname) {
  ptnames_.push_front(ptname);
}

CHECKED_STATUS PTQualifiedName::Analyze(SemContext *sem_context) {
  // We don't support qualified name yet except for a keyspace.
  // Support only the names like: '<keyspace_name>.<table_name>'.
  if (ptnames_.size() >= 3) {
    return sem_context->Error(this, ErrorCode::FEATURE_NOT_SUPPORTED);
  }

  return Status::OK();
}

}  // namespace ql
}  // namespace yb
