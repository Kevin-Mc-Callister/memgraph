// Copyright 2026 Memgraph Ltd.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.txt; by using this file, you agree to be bound by the terms of the Business Source
// License, and you may not use this file except in compliance with the Business Source License.
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0, included in the file
// licenses/APL.txt.

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "storage/v2/inmemory/storage.hpp"
#include "storage/v2/inmemory/unique_constraints.hpp"
#include "tests/test_commit_args_helper.hpp"
#include "utils/resource_lock.hpp"

namespace memgraph::storage {

// Observe the actual skiplist size: live vertex counts and reclaimed delta counts
// cannot reveal obsolete constraint entries left behind by a skipped sweep.
struct InMemoryUniqueConstraintsTestAccess {
  static uint64_t EntryCount(InMemoryUniqueConstraints const &constraints, LabelId label,
                             std::set<PropertyId> const &properties) {
    auto constraint = constraints.GetIndividualConstraint(label, properties);
    return constraint ? constraint->skiplist.size() : 0;
  }
};

}  // namespace memgraph::storage

namespace ms = memgraph::storage;

class StorageV2UniqueConstraintGcTest : public testing::TestWithParam<bool> {
 protected:
  void SetUp() override {
    ms::Config config;
    config.gc.type = ms::Config::Gc::Type::NONE;
    config.salient.items.delta_on_identical_property_update = GetParam();
    storage_ = std::make_unique<ms::InMemoryStorage>(config);
    item_ = storage_->NameToLabel("Item");
    other_ = storage_->NameToLabel("Other");
    key_ = storage_->NameToProperty("key");
    other_key_ = storage_->NameToProperty("other_key");
    payload_ = storage_->NameToProperty("payload");
  }

  void CreateUniqueConstraint(ms::LabelId label, ms::PropertyId property) {
    auto acc = storage_->ReadOnlyAccess();
    auto result = acc->CreateUniqueConstraint(label, {property});
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(*result, ms::UniqueConstraints::CreationStatus::SUCCESS);
    ASSERT_TRUE(acc->PrepareForCommitPhase(memgraph::tests::MakeMainCommitArgs()).has_value());
  }

  void CreateExistenceConstraint(ms::LabelId label, ms::PropertyId property) {
    auto acc = storage_->ReadOnlyAccess();
    ASSERT_TRUE(acc->CreateExistenceConstraint(label, property).has_value());
    ASSERT_TRUE(acc->PrepareForCommitPhase(memgraph::tests::MakeMainCommitArgs()).has_value());
  }

  void SeedVertices() {
    {
      auto acc = storage_->Access(ms::WRITE);
      for (int64_t i = 0; i < kVertexCount; ++i) {
        auto vertex = acc->CreateVertex();
        vertices_.push_back(vertex.Gid());
        ASSERT_TRUE(vertex.AddLabel(item_).has_value());
        ASSERT_TRUE(vertex.SetProperty(key_, ms::PropertyValue{i}).has_value());
        ASSERT_TRUE(vertex.SetProperty(other_key_, ms::PropertyValue{i}).has_value());
        ASSERT_TRUE(vertex.SetProperty(payload_, ms::PropertyValue{0}).has_value());
      }
      ASSERT_TRUE(acc->PrepareForCommitPhase(memgraph::tests::MakeMainCommitArgs()).has_value());
    }
    // Drain the creation deltas before the workload. Otherwise their label/key
    // arming would accidentally sweep entries produced by the first updates.
    CollectGarbage();
    ASSERT_EQ(EntryCount(item_, key_), kVertexCount);
  }

  void CollectGarbage() {
    storage_->FreeMemory(
        memgraph::utils::ResourceLockGuard{storage_->main_lock_, memgraph::utils::ResourceLockGuard::UNIQUE}, false);
  }

  uint64_t EntryCount(ms::LabelId label, ms::PropertyId property) const {
    auto const &constraints =
        static_cast<ms::InMemoryUniqueConstraints const &>(*storage_->constraints_.unique_constraints_);
    return ms::InMemoryUniqueConstraintsTestAccess::EntryCount(constraints, label, {property});
  }

  template <typename Mutation>
  void RepeatedWrites(Mutation mutation) {
    for (int64_t transaction = 1; transaction <= 128; ++transaction) {
      {
        auto acc = storage_->Access(ms::WRITE);
        for (int64_t i = 0; i < kVertexCount; ++i) {
          auto vertex = acc->FindVertex(vertices_[i], ms::View::OLD);
          ASSERT_TRUE(vertex.has_value());
          ASSERT_NO_FATAL_FAILURE(mutation(*vertex, i, transaction));
        }
        ASSERT_TRUE(acc->PrepareForCommitPhase(memgraph::tests::MakeMainCommitArgs()).has_value());
      }
      if (transaction % 16 == 0) {
        CollectGarbage();
        EXPECT_EQ(EntryCount(item_, key_), kVertexCount) << "after " << transaction << " committed transactions";
      }
    }
  }

  static constexpr int64_t kVertexCount = 4;
  std::unique_ptr<ms::InMemoryStorage> storage_;
  std::vector<ms::Gid> vertices_;
  ms::LabelId item_;
  ms::LabelId other_;
  ms::PropertyId key_;
  ms::PropertyId other_key_;
  ms::PropertyId payload_;
};

TEST_P(StorageV2UniqueConstraintGcTest, UnconstrainedSetPropertyDoesNotAccumulateEntries) {
  ASSERT_NO_FATAL_FAILURE(CreateUniqueConstraint(item_, key_));
  ASSERT_NO_FATAL_FAILURE(SeedVertices());

  ASSERT_NO_FATAL_FAILURE(RepeatedWrites([&](ms::VertexAccessor &vertex, int64_t, int64_t transaction) {
    ASSERT_TRUE(vertex.SetProperty(payload_, ms::PropertyValue{transaction}).has_value());
  }));
}

TEST_P(StorageV2UniqueConstraintGcTest, UnconstrainedUpdatePropertiesDoesNotAccumulateEntries) {
  ASSERT_NO_FATAL_FAILURE(CreateUniqueConstraint(item_, key_));
  ASSERT_NO_FATAL_FAILURE(SeedVertices());

  ASSERT_NO_FATAL_FAILURE(RepeatedWrites([&](ms::VertexAccessor &vertex, int64_t, int64_t transaction) {
    auto properties = std::map<ms::PropertyId, ms::PropertyValue>{{payload_, ms::PropertyValue{transaction}}};
    ASSERT_TRUE(vertex.UpdateProperties(properties).has_value());
  }));
}

TEST_P(StorageV2UniqueConstraintGcTest, IdenticalSetPropertyKeepsEntriesBounded) {
  ASSERT_NO_FATAL_FAILURE(CreateUniqueConstraint(item_, key_));
  ASSERT_NO_FATAL_FAILURE(SeedVertices());

  ASSERT_NO_FATAL_FAILURE(RepeatedWrites([&](ms::VertexAccessor &vertex, int64_t i, int64_t) {
    ASSERT_TRUE(vertex.SetProperty(key_, ms::PropertyValue{i}).has_value());
  }));
}

TEST_P(StorageV2UniqueConstraintGcTest, IdenticalUpdatePropertiesKeepsEntriesBounded) {
  ASSERT_NO_FATAL_FAILURE(CreateUniqueConstraint(item_, key_));
  ASSERT_NO_FATAL_FAILURE(SeedVertices());

  ASSERT_NO_FATAL_FAILURE(RepeatedWrites([&](ms::VertexAccessor &vertex, int64_t i, int64_t) {
    auto properties = std::map<ms::PropertyId, ms::PropertyValue>{{key_, ms::PropertyValue{i}}};
    ASSERT_TRUE(vertex.UpdateProperties(properties).has_value());
  }));
}

// Verification is selected per vertex. A key write for one constraint must not
// leave obsolete entries in another constraint whose key has no matching delta.
TEST_P(StorageV2UniqueConstraintGcTest, OtherUniqueKeyWriteKeepsExistingConstraintEntriesBounded) {
  ASSERT_NO_FATAL_FAILURE(CreateUniqueConstraint(item_, key_));
  ASSERT_NO_FATAL_FAILURE(CreateUniqueConstraint(item_, other_key_));
  ASSERT_NO_FATAL_FAILURE(SeedVertices());

  ASSERT_NO_FATAL_FAILURE(RepeatedWrites([&](ms::VertexAccessor &vertex, int64_t i, int64_t transaction) {
    ASSERT_TRUE(vertex.SetProperty(other_key_, ms::PropertyValue{transaction * kVertexCount + i}).has_value());
  }));
  EXPECT_EQ(EntryCount(item_, other_key_), kVertexCount);
}

TEST_P(StorageV2UniqueConstraintGcTest, MixedUpdatePropertiesKeepsIdenticalConstraintEntriesBounded) {
  ASSERT_NO_FATAL_FAILURE(CreateUniqueConstraint(item_, key_));
  ASSERT_NO_FATAL_FAILURE(CreateUniqueConstraint(item_, other_key_));
  ASSERT_NO_FATAL_FAILURE(SeedVertices());

  ASSERT_NO_FATAL_FAILURE(RepeatedWrites([&](ms::VertexAccessor &vertex, int64_t i, int64_t transaction) {
    auto properties = std::map<ms::PropertyId, ms::PropertyValue>{
        {key_, ms::PropertyValue{i}}, {other_key_, ms::PropertyValue{transaction * kVertexCount + i}}};
    ASSERT_TRUE(vertex.UpdateProperties(properties).has_value());
  }));
  EXPECT_EQ(EntryCount(item_, other_key_), kVertexCount);
}

TEST_P(StorageV2UniqueConstraintGcTest, PropertyConstrainedOnOtherLabelKeepsEntriesBounded) {
  ASSERT_NO_FATAL_FAILURE(CreateUniqueConstraint(item_, key_));
  ASSERT_NO_FATAL_FAILURE(CreateUniqueConstraint(other_, other_key_));
  ASSERT_NO_FATAL_FAILURE(SeedVertices());

  ASSERT_NO_FATAL_FAILURE(RepeatedWrites([&](ms::VertexAccessor &vertex, int64_t, int64_t transaction) {
    ASSERT_TRUE(vertex.SetProperty(other_key_, ms::PropertyValue{transaction}).has_value());
  }));
  EXPECT_EQ(EntryCount(other_, other_key_), 0);
}

TEST_P(StorageV2UniqueConstraintGcTest, OtherUniqueLabelAdditionKeepsExistingConstraintEntriesBounded) {
  ASSERT_NO_FATAL_FAILURE(CreateUniqueConstraint(item_, key_));
  ASSERT_NO_FATAL_FAILURE(CreateUniqueConstraint(other_, other_key_));
  ASSERT_NO_FATAL_FAILURE(SeedVertices());

  ASSERT_NO_FATAL_FAILURE(RepeatedWrites([&](ms::VertexAccessor &vertex, int64_t, int64_t) {
    ASSERT_TRUE(vertex.RemoveLabel(other_).has_value());
    ASSERT_TRUE(vertex.AddLabel(other_).has_value());
  }));
  EXPECT_EQ(EntryCount(other_, other_key_), kVertexCount);
}

TEST_P(StorageV2UniqueConstraintGcTest, ExistenceLabelAdditionKeepsExistingUniqueEntriesBounded) {
  ASSERT_NO_FATAL_FAILURE(CreateUniqueConstraint(item_, key_));
  ASSERT_NO_FATAL_FAILURE(CreateExistenceConstraint(other_, payload_));
  ASSERT_NO_FATAL_FAILURE(SeedVertices());

  ASSERT_NO_FATAL_FAILURE(RepeatedWrites([&](ms::VertexAccessor &vertex, int64_t, int64_t) {
    ASSERT_TRUE(vertex.RemoveLabel(other_).has_value());
    ASSERT_TRUE(vertex.AddLabel(other_).has_value());
  }));
}

// Initializing an emptied property store produces key deltas even when the new
// key equals the value from before the clear.
TEST_P(StorageV2UniqueConstraintGcTest, ClearAndInitPropertiesKeepsEntriesBounded) {
  ASSERT_NO_FATAL_FAILURE(CreateUniqueConstraint(item_, key_));
  ASSERT_NO_FATAL_FAILURE(SeedVertices());

  ASSERT_NO_FATAL_FAILURE(RepeatedWrites([&](ms::VertexAccessor &vertex, int64_t i, int64_t transaction) {
    ASSERT_TRUE(vertex.ClearProperties().has_value());
    auto properties = std::map<ms::PropertyId, ms::PropertyValue>{{key_, ms::PropertyValue{i}},
                                                                  {payload_, ms::PropertyValue{transaction}}};
    auto result = vertex.InitProperties(properties);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(*result);
  }));
}

// SET n = map clears the old map before updating with the new map. Restoring the
// identical key still produces key deltas that must arm constraint cleanup.
TEST_P(StorageV2UniqueConstraintGcTest, ClearAndUpdatePropertiesKeepsEntriesBounded) {
  ASSERT_NO_FATAL_FAILURE(CreateUniqueConstraint(item_, key_));
  ASSERT_NO_FATAL_FAILURE(SeedVertices());

  ASSERT_NO_FATAL_FAILURE(RepeatedWrites([&](ms::VertexAccessor &vertex, int64_t i, int64_t transaction) {
    ASSERT_TRUE(vertex.ClearProperties().has_value());
    auto properties = std::map<ms::PropertyId, ms::PropertyValue>{{key_, ms::PropertyValue{i}},
                                                                  {payload_, ms::PropertyValue{transaction}}};
    ASSERT_TRUE(vertex.UpdateProperties(properties).has_value());
  }));
}

TEST_P(StorageV2UniqueConstraintGcTest, ClearPropertiesRemovesObsoleteConstraintEntries) {
  ASSERT_NO_FATAL_FAILURE(CreateUniqueConstraint(item_, key_));
  ASSERT_NO_FATAL_FAILURE(SeedVertices());

  {
    auto acc = storage_->Access(ms::WRITE);
    for (auto gid : vertices_) {
      auto vertex = acc->FindVertex(gid, ms::View::OLD);
      ASSERT_TRUE(vertex.has_value());
      ASSERT_TRUE(vertex->ClearProperties().has_value());
    }
    ASSERT_TRUE(acc->PrepareForCommitPhase(memgraph::tests::MakeMainCommitArgs()).has_value());
  }
  CollectGarbage();
  EXPECT_EQ(EntryCount(item_, key_), 0);
}

TEST_P(StorageV2UniqueConstraintGcTest, PeriodicCommitForgetsPreviouslyVerifiedChanges) {
  ASSERT_NO_FATAL_FAILURE(CreateUniqueConstraint(item_, key_));
  ASSERT_NO_FATAL_FAILURE(SeedVertices());

  {
    auto acc = storage_->Access(ms::WRITE);
    for (int64_t i = 0; i < kVertexCount; ++i) {
      auto vertex = acc->FindVertex(vertices_[i], ms::View::OLD);
      ASSERT_TRUE(vertex.has_value());
      ASSERT_TRUE(vertex->SetProperty(key_, ms::PropertyValue{kVertexCount + i}).has_value());
    }
    ASSERT_TRUE(acc->PeriodicCommit(memgraph::tests::MakeMainCommitArgs()).has_value());
    auto const entries_after_key_change = EntryCount(item_, key_);
    ASSERT_EQ(entries_after_key_change, 2 * kVertexCount);

    // The accessor survives each periodic commit, but its verification state
    // belongs only to the segment that produced it. There is no new key delta
    // in the remaining segments to arm cleanup of additional entries.
    for (int64_t segment = 1; segment <= 128; ++segment) {
      for (auto gid : vertices_) {
        auto vertex = acc->FindVertex(gid, ms::View::OLD);
        ASSERT_TRUE(vertex.has_value());
        ASSERT_TRUE(vertex->SetProperty(payload_, ms::PropertyValue{segment}).has_value());
      }
      ASSERT_TRUE(acc->PeriodicCommit(memgraph::tests::MakeMainCommitArgs()).has_value());
      if (segment % 16 == 0) {
        EXPECT_EQ(EntryCount(item_, key_), entries_after_key_change) << "after " << segment << " periodic commits";
      }
    }
    ASSERT_TRUE(acc->PrepareForCommitPhase(memgraph::tests::MakeMainCommitArgs()).has_value());
  }
  CollectGarbage();
  EXPECT_EQ(EntryCount(item_, key_), kVertexCount);
}

INSTANTIATE_TEST_SUITE_P(DeltaOnIdenticalPropertyUpdate, StorageV2UniqueConstraintGcTest, testing::Bool(),
                         [](testing::TestParamInfo<bool> const &info) { return info.param ? "True" : "False"; });
