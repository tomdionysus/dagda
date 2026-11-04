//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include "dagda/contract/claim_store.h"
#include "dagda/contract/object_ledger.h"
#include "dagda/contract/object_store.h"
#include "dannan/contract/published.h"
#include "dannan/contract/time_source.h"

namespace dagda::ledger {

// The object ledger (requirement L14): claims forward to the ClaimStore,
// holdings to the class's object store, and it owns the published horizons.
// The owner keeps all three stores, and the time source a page's budget is
// checked against, alive for the ledger's lifetime.
class RetentionLedger final : public contract::ObjectLedger {
 public:
  RetentionLedger(contract::ClaimStore& claims, const contract::ObjectStore& data, const contract::ObjectStore& control,
                  const dannan::contract::TimeSource& time = dannan::contract::steady_time_source()) noexcept
      : _claims(claims), _data(data), _control(control), _time(time) {}

  dannan::sched::Page<ObjectId, ObjectId> claimed(contract::RetentionClass, dannan::sched::Cursor<ObjectId> from, dannan::sched::Budget&) const override;
  bool held(contract::RetentionClass, const ObjectId&) const override;
  contract::HeldView held_view(contract::RetentionClass type) const override { return (type == contract::RetentionClass::data ? _data : _control).held_view(); }
  InventoryHandle inventory() const override { return _inventory.handle(); }
  ReleaseHandle release() const override { return _release.handle(); }
  void publish(InventoryHandle) override;
  bool publish(contract::ReleaseBuild) override;
  bool retained(contract::RetentionClass type, const ObjectId& id) const override { return _claims.retained(type, id); }
  size_t release_unreferenced(contract::RetentionClass, const contract::ReleaseHorizon&, size_t operation_budget) override;
  size_t prune_unclaimed(contract::RetentionClass, size_t operation_budget) override;
  bool compact_if_needed(size_t record_threshold) override { return _claims.compact_if_needed(record_threshold); }

 private:
  contract::ClaimStore& _claims;
  const contract::ObjectStore& _data;
  const contract::ObjectStore& _control;
  const dannan::contract::TimeSource& _time;
  dannan::contract::Published<contract::InventoryHorizon> _inventory;
  dannan::contract::Published<contract::ReleaseHorizon> _release;
};

}  // namespace dagda::ledger
