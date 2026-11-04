//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/lease/keeper.h"

#include <boost/asio/post.hpp>
#include <utility>

namespace dagda::lease {

using dannan::contract::Lock;
using std::chrono::duration_cast;
using std::chrono::milliseconds;

namespace {

std::string describe(const Lease& lease) { return dannan::identity::to_string(lease.holder) + " at epoch " + std::to_string(lease.epoch); }

std::string ms(milliseconds value) { return std::to_string(value.count()) + " ms"; }

}  // namespace

const char* outcome_name(Acquisition::Outcome outcome) noexcept {
  switch (outcome) {
    case Acquisition::Outcome::held:
      return "held";
    case Acquisition::Outcome::taken:
      return "taken";
    case Acquisition::Outcome::held_elsewhere:
      return "held_elsewhere";
    case Acquisition::Outcome::waiting:
      return "waiting";
    case Acquisition::Outcome::lost:
      return "lost";
    case Acquisition::Outcome::failed:
      return "failed";
  }
  return "failed";
}

LeaseKeeper::LeaseKeeper(const Membership& membership, const dannan::contract::TimeSource& time, const dannan::contract::WallClock& wall, LeaseOptions options)
    : _membership(membership), _time(time), _wall(wall), _options(options), _self(membership.self()) {}

Decision LeaseKeeper::decide(const std::string& name, const std::optional<Lease>& current, size_t rank) {
  if (!current) return takeable(name, current, rank, "no holder");
  if (current->holder == _self) {
    forget(name);
    return {Decision::Kind::held_here, std::nullopt, "held here at epoch " + std::to_string(current->epoch)};
  }
  const auto absent = _membership.absent_since(current->holder);
  if (!absent) {
    forget(name);
    return {Decision::Kind::held_elsewhere, std::nullopt, "held by member " + describe(*current)};
  }
  const auto away = duration_cast<milliseconds>(_time.now() - *absent);
  if (away < _options.absence) {
    forget(name);
    return {Decision::Kind::held_elsewhere, std::nullopt, "held by " + describe(*current) + ", absent " + ms(away) + " of " + ms(_options.absence)};
  }
  return takeable(name, current, rank, "holder " + describe(*current) + " absent " + ms(away));
}

Decision LeaseKeeper::vacant(const std::string& name, const std::optional<Lease>& current, size_t rank) {
  return takeable(name, current, rank, "vacant by the owner's word");
}

Decision LeaseKeeper::takeable(const std::string& name, const std::optional<Lease>& current, size_t rank, std::string why) {
  const auto now = _time.now();
  {
    Lock lock(_mutex);
    const auto since = _takeable_since.try_emplace(name, now).first->second;
    const auto wait = _options.rank_step * static_cast<milliseconds::rep>(rank);
    if (now - since < wait) {
      return {Decision::Kind::waiting, std::nullopt,
              std::move(why) + "; rank " + std::to_string(rank) + " waits " + ms(wait) + ", " + ms(duration_cast<milliseconds>(now - since)) + " passed"};
    }
    _takeable_since.erase(name);
  }
  return {Decision::Kind::take, next(current), std::move(why)};
}

void LeaseKeeper::forget(const std::string& name) {
  Lock lock(_mutex);
  _takeable_since.erase(name);
}

Standing LeaseKeeper::standing(const Lease& mine, const std::optional<Lease>& current) const {
  if (!current) return {false, current, "no record"};
  if (current->holder == _self && current->epoch >= mine.epoch) return {true, current, "held here at epoch " + std::to_string(current->epoch)};
  return {false, current, "superseded by " + describe(*current)};
}

Lease LeaseKeeper::next(const std::optional<Lease>& current) const { return Lease{_self, current ? current->epoch + 1 : 1, _wall.unix_ms()}; }

void LeaseKeeper::acquire(LeaseRecords& records, const std::string& scope, const std::string& name, std::chrono::milliseconds deadline,
                          boost::asio::any_io_executor executor, Done done, size_t rank) {
  // Rank waits are kept per lease: the scope and name, apart.
  const auto key = scope + std::string(1, '\0') + name;
  const auto current = records.read(scope, name);
  auto decision = decide(key, current, rank);
  if (decision.kind != Decision::Kind::take) {
    Acquisition out;
    out.lease = current;
    out.why = std::move(decision.why);
    out.outcome = decision.kind == Decision::Kind::held_here ? Acquisition::Outcome::held
                  : decision.kind == Decision::Kind::waiting ? Acquisition::Outcome::waiting
                                                             : Acquisition::Outcome::held_elsewhere;
    boost::asio::post(executor, [done = std::move(done), out = std::move(out)]() mutable { done(std::move(out)); });
    return;
  }
  const auto proposed = *decision.proposed;
  records.join(scope, name, proposed, deadline, executor,
               [self = _self, proposed, why = std::move(decision.why), done = std::move(done)](LeaseRecords::Joined joined) {
                 Acquisition out;
                 out.lease = joined.standing;
                 if (!joined.failure.empty()) {
                   out.outcome = Acquisition::Outcome::failed;
                   out.why = why + "; not written: " + joined.failure;
                 } else if (joined.standing && joined.standing->holder == self && joined.standing->epoch >= proposed.epoch) {
                   out.outcome = Acquisition::Outcome::taken;
                   out.why = why + "; taken at epoch " + std::to_string(joined.standing->epoch);
                 } else {
                   out.outcome = Acquisition::Outcome::lost;
                   out.why = why + "; " + (joined.standing ? "stands for " + describe(*joined.standing) : std::string("no record stands"));
                 }
                 done(std::move(out));
               });
}

}  // namespace dagda::lease
