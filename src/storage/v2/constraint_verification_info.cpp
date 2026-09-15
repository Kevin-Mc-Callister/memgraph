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

#include "storage/v2/constraint_verification_info.hpp"

#include <algorithm>

namespace memgraph::storage {

ConstraintVerificationInfo::ConstraintVerificationInfo() = default;

ConstraintVerificationInfo::ConstraintVerificationInfo(ConstraintRelevance relevance) : relevance_{relevance} {}

ConstraintVerificationInfo::~ConstraintVerificationInfo() = default;
ConstraintVerificationInfo::ConstraintVerificationInfo(ConstraintVerificationInfo &&) noexcept = default;
ConstraintVerificationInfo &ConstraintVerificationInfo::operator=(ConstraintVerificationInfo &&) noexcept = default;

void ConstraintVerificationInfo::AddedLabel(LabelId label, Vertex const *vertex) {
  // One set feeds both checks, so a label either kind is keyed on is reported to both.
  if (!relevance_.unique_labels.IsInteresting(label) && !relevance_.existence_labels.IsInteresting(label)) return;
  written_labels_.set(label);
  added_labels_.insert(vertex);
}

void ConstraintVerificationInfo::AddedProperty(PropertyId property, Vertex const *vertex) {
  if (!relevance_.unique_properties.IsInteresting(property)) return;
  written_properties_.set(property);
  added_properties_.insert(vertex);
}

void ConstraintVerificationInfo::RemovedProperty(PropertyId property, Vertex const *vertex) {
  if (!relevance_.existence_properties.IsInteresting(property)) return;
  removed_properties_.insert(vertex);
}

auto ConstraintVerificationInfo::GetVerticesForUniqueConstraintChecking() const -> std::unordered_set<Vertex const *> {
  std::unordered_set<Vertex const *> updated_vertices;

  updated_vertices.insert(added_labels_.begin(), added_labels_.end());
  updated_vertices.insert(added_properties_.begin(), added_properties_.end());

  return updated_vertices;
}

auto ConstraintVerificationInfo::GetVerticesForExistenceConstraintChecking() const
    -> std::unordered_set<Vertex const *> {
  std::unordered_set<Vertex const *> updated_vertices;

  updated_vertices.insert(added_labels_.begin(), added_labels_.end());
  updated_vertices.insert(removed_properties_.begin(), removed_properties_.end());

  return updated_vertices;
}

bool ConstraintVerificationInfo::NeedsUniqueConstraintVerification() const {
  return !added_labels_.empty() || !added_properties_.empty();
}

bool ConstraintVerificationInfo::NeedsExistenceConstraintVerification() const {
  return !added_labels_.empty() || !removed_properties_.empty();
}

bool ConstraintVerificationInfo::AffectsUniqueConstraint(LabelId label, std::set<PropertyId> const &properties) const {
  return written_labels_.test(label) ||
         std::ranges::any_of(properties, [this](PropertyId property) { return written_properties_.test(property); });
}

void ConstraintVerificationInfo::Clear() {
  added_labels_.clear();
  added_properties_.clear();
  removed_properties_.clear();
  written_labels_.reset();
  written_properties_.reset();
}
}  // namespace memgraph::storage
