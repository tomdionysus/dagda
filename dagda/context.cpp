//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/context.h"

#include <algorithm>
#include <atomic>
#include <boost/asio/post.hpp>
#include <future>
#include <memory>
#include <utility>

#include "dagda/contract/stopping.h"
#include "dagda/node.h"

namespace dagda {

namespace {

// A blocking form waits this much past its deadline before it calls the
// completion lost: the completion is due by the deadline, so the margin only
// covers the executor.
constexpr std::chrono::milliseconds completion_margin{5000};

void check_deadline(std::chrono::milliseconds deadline) {
  if (deadline.count() <= 0) throw std::invalid_argument("a deadline must be positive");
}

// Starts the asynchronous form on the node's executor and waits for its
// completion: the blocking forms are this and nothing else.
template <class T, class Start>
T await(Node& node, std::chrono::milliseconds deadline, Start start) {
  auto promise = std::make_shared<std::promise<T>>();
  auto future = promise->get_future();
  start(node.dannan().time().executor(), [promise](T value) { promise->set_value(std::move(value)); });
  if (future.wait_for(deadline + completion_margin) != std::future_status::ready)
    throw std::runtime_error("the store's completion did not arrive by its deadline");
  return future.get();
}

constexpr const char* heads_off = "this node runs with NodeOptions::heads off, so it opens no head space";
constexpr const char* space_not_open = "no such head space is open here";
constexpr const char* node_stopping = "the node is stopping";

// Runs `job` on the spaces' thread and answers `done` with its result on `executor`,
// never inline, or at `deadline` with `late`, whichever is first. Nothing starts for a
// caller the deadline has already answered. Refused outright, `refused` answers.
template <class T>
void run_on_spaces(Node& node, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, std::function<void(T)> done, T late, T refused,
                   std::function<T()> job) {
  auto answered = std::make_shared<std::atomic<bool>>(false);
  auto shared_done = std::make_shared<std::function<void(T)>>(std::move(done));
  const auto answer = [answered, shared_done, executor](T value) {
    if (answered->exchange(true)) return;
    boost::asio::post(executor, [shared_done, value = std::move(value)] { (*shared_done)(value); });
  };
  auto* spaces = node.spaces();
  if (!spaces || !spaces->submit([answered, answer, job = std::move(job), refused, spaces] {
        if (answered->load()) return;
        if (spaces->stopping()) {
          answer(refused);
          return;
        }
        answer(job());
      })) {
    answer(std::move(refused));
    return;
  }
  node.dannan().time().after(deadline, [answer, late = std::move(late)] { answer(late); });
}

std::optional<WriteEvidence> known(WriteEvidence evidence) {
  if (!evidence.ticket.valid()) return std::nullopt;
  return evidence;
}

}  // namespace

// ---- Version ----------------------------------------------------------------

Context::Context(Node& node) : _domains(node), _objects(node), _claims(node), _leases(node), _heads(node), _tickets(node) {}

uint32_t Context::version() const noexcept { return context_version; }

VersionCheck Context::check_version(uint32_t built_against) const noexcept {
  VersionCheck out;
  out.built_against = built_against;
  out.served = context_version;
  out.accepted = built_against == context_version || built_against == context_version_frozen;
  if (!out.accepted)
    out.why = "dagda::Context version " + std::to_string(built_against) + " was asked for and this library serves version " + std::to_string(context_version) +
              ", and version " + std::to_string(context_version_frozen) + " beside it: rebuild the hosted system against this Dagda";
  return out;
}

void Context::require_version(uint32_t built_against) const {
  const auto check = check_version(built_against);
  if (!check.accepted) throw ContextVersionMismatch(check.why);
}

// ---- Domains ----------------------------------------------------------------

std::vector<domain::Domain> Context::Domains::list() const { return {_node.options().domain}; }

std::optional<domain::Domain> Context::Domains::describe(const domain::DomainId& id) const {
  if (id != _node.options().domain.id) return std::nullopt;
  return _node.options().domain;
}

// ---- Objects ----------------------------------------------------------------

void Context::Objects::write(store::Bytes data, WriteOptions options, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor,
                             WriteDone done) {
  check_deadline(deadline);
  options.deadline = deadline;
  const auto id = store::object_id(data);
  _node.store().write(id, std::move(data), std::move(options), std::move(executor), std::move(done));
}

WriteEvidence Context::Objects::write(std::span<const uint8_t> data, WriteOptions options, std::chrono::milliseconds deadline) {
  check_deadline(deadline);
  return await<WriteEvidence>(_node, deadline, [&](boost::asio::any_io_executor executor, WriteDone done) {
    write(store::Bytes(data.begin(), data.end()), options, deadline, std::move(executor), std::move(done));
  });
}

void Context::Objects::read(const ObjectId& id, ReadOptions options, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, ReadDone done) {
  check_deadline(deadline);
  options.deadline = deadline;
  _node.store().read(id, std::move(options), std::move(executor), std::move(done));
}

ReadResult Context::Objects::read(const ObjectId& id, ReadOptions options, std::chrono::milliseconds deadline) {
  check_deadline(deadline);
  return await<ReadResult>(_node, deadline,
                           [&](boost::asio::any_io_executor executor, ReadDone done) { read(id, options, deadline, std::move(executor), std::move(done)); });
}

bool Context::Objects::has(const ObjectId& id) const { return _node.store().has(id); }

void Context::Objects::remove(std::vector<ObjectId> ids, contract::RetentionClock observed, WriteScope writer, std::chrono::milliseconds deadline,
                              boost::asio::any_io_executor executor, RemoveDone done) {
  check_deadline(deadline);
  RemoveEvidence base;
  base.sought = ids;
  std::sort(base.sought.begin(), base.sought.end());
  base.sought.erase(std::unique(base.sought.begin(), base.sought.end()), base.sought.end());
  // Whichever comes first, the delete's end or the deadline, answers; the
  // other is dropped.
  auto answered = std::make_shared<std::atomic<bool>>(false);
  auto shared_done = std::make_shared<RemoveDone>(std::move(done));
  _node.dannan().time().after(deadline, [answered, shared_done, base, executor] {
    if (answered->exchange(true)) return;
    auto evidence = base;
    evidence.why = "the deadline passed with the delete still running; it may yet land on some nodes, and deleting again is harmless";
    boost::asio::post(executor, [shared_done, evidence = std::move(evidence)] { (*shared_done)(evidence); });
  });
  _node.store().remove(std::move(ids), std::move(observed), std::move(writer), deadline, std::move(executor), [answered, shared_done](RemoveEvidence evidence) {
    if (answered->exchange(true)) return;
    (*shared_done)(std::move(evidence));
  });
}

RemoveEvidence Context::Objects::remove(std::vector<ObjectId> ids, contract::RetentionClock observed, WriteScope writer, std::chrono::milliseconds deadline) {
  check_deadline(deadline);
  return await<RemoveEvidence>(_node, deadline, [&](boost::asio::any_io_executor executor, RemoveDone done) {
    remove(std::move(ids), std::move(observed), std::move(writer), deadline, std::move(executor), std::move(done));
  });
}

// ---- Claims -----------------------------------------------------------------

void Context::Claims::retain(contract::RetentionClass type, std::vector<ObjectId> ids, contract::RetentionDot dot, WriteScope writer,
                             std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, Done done, Scope scope) {
  check_deadline(deadline);
  if (dot.sequence == 0 || dot.origin == contract::NodeId{}) throw std::invalid_argument("a claim's dot needs an origin and a sequence");
  ClaimEvidence base;
  base.type = type;
  base.sought = ids.size();
  // Whichever comes first, the claim's end or the deadline, answers; the
  // other is dropped.
  auto answered = std::make_shared<std::atomic<bool>>(false);
  auto shared_done = std::make_shared<Done>(std::move(done));
  _node.dannan().time().after(deadline, [answered, shared_done, base, executor] {
    if (answered->exchange(true)) return;
    auto evidence = base;
    evidence.why = "the deadline passed with the claim still running; it may yet land, and claiming again is harmless";
    boost::asio::post(executor, [shared_done, evidence = std::move(evidence)] { (*shared_done)(evidence); });
  });
  _node.store().retain(type, std::move(ids), dot, std::move(writer), scope, std::move(executor), [answered, shared_done, base](store::RetainOutcome outcome) {
    if (answered->exchange(true)) return;
    auto evidence = base;
    evidence.finished = outcome.ran;
    evidence.unheld = std::move(outcome.unheld);
    evidence.held_here = outcome.held_here;
    evidence.why = std::move(outcome.why);
    (*shared_done)(std::move(evidence));
  });
}

ClaimEvidence Context::Claims::retain(contract::RetentionClass type, std::vector<ObjectId> ids, contract::RetentionDot dot, WriteScope writer,
                                      std::chrono::milliseconds deadline, Scope scope) {
  check_deadline(deadline);
  return await<ClaimEvidence>(_node, deadline, [&](boost::asio::any_io_executor executor, Done done) {
    retain(type, std::move(ids), dot, std::move(writer), deadline, std::move(executor), std::move(done), scope);
  });
}

bool Context::Claims::retained(contract::RetentionClass type, const ObjectId& id) const { return _node.claims().retained(type, id); }

// ---- Leases -----------------------------------------------------------------

void Context::Leases::acquire(const std::string& scope, const std::string& name, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor,
                              Done done, size_t rank) {
  check_deadline(deadline);
  _node.lease_keeper().acquire(_node.lease_records(), scope, name, deadline, std::move(executor), std::move(done), rank);
}

lease::Acquisition Context::Leases::acquire(const std::string& scope, const std::string& name, std::chrono::milliseconds deadline, size_t rank) {
  check_deadline(deadline);
  return await<lease::Acquisition>(
      _node, deadline, [&](boost::asio::any_io_executor executor, Done done) { acquire(scope, name, deadline, std::move(executor), std::move(done), rank); });
}

std::optional<lease::Lease> Context::Leases::read(const std::string& scope, const std::string& name) const { return _node.lease_records().read(scope, name); }

lease::Standing Context::Leases::standing(const std::string& scope, const std::string& name, const lease::Lease& mine) const {
  return _node.lease_keeper().standing(mine, _node.lease_records().read(scope, name));
}

// ---- Heads ------------------------------------------------------------------

heads::SpaceId Context::Heads::id_of(std::string_view name) const { return heads::space_id(_node.options().domain.id, name); }

void Context::Heads::open(heads::SpaceSpec spec, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, Opened done) {
  check_deadline(deadline);
  SpaceOpened base;
  base.space = id_of(spec.name);
  auto late = base;
  late.why = "the deadline passed with the open still running; it may yet land, and opening again says so";
  auto refused = base;
  refused.why = _node.spaces() ? node_stopping : heads_off;
  auto shared_spec = std::make_shared<heads::SpaceSpec>(std::move(spec));
  run_on_spaces<SpaceOpened>(_node, deadline, std::move(executor), std::move(done), std::move(late), std::move(refused), [node = &_node, base, shared_spec] {
    auto out = base;
    try {
      auto& space = node->spaces()->open(std::move(*shared_spec));
      out.generation = space.replica().generation();
      out.opened = true;
    } catch (const std::exception& error) {
      out.why = error.what();
    }
    return out;
  });
}

SpaceOpened Context::Heads::open(heads::SpaceSpec spec, std::chrono::milliseconds deadline) {
  check_deadline(deadline);
  return await<SpaceOpened>(_node, deadline,
                            [&](boost::asio::any_io_executor executor, Opened done) { open(std::move(spec), deadline, std::move(executor), std::move(done)); });
}

HeadReadEvidence Context::Heads::current(const heads::SpaceId& space) const {
  HeadReadEvidence out;
  auto* spaces = _node.spaces();
  auto* found = spaces ? spaces->find(space) : nullptr;
  if (!found) {
    out.why = spaces ? space_not_open : heads_off;
    return out;
  }
  out.finished = true;
  out.view = found->current();
  return out;
}

void Context::Heads::read(const heads::SpaceId& space, HeadRead how, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, Read done,
                          dannan::sched::WorkContext work) {
  check_deadline(deadline);
  HeadReadEvidence late;
  late.why = "the deadline passed with the read still running";
  HeadReadEvidence refused;
  refused.why = _node.spaces() ? node_stopping : heads_off;
  run_on_spaces<HeadReadEvidence>(_node, deadline, std::move(executor), std::move(done), std::move(late), std::move(refused),
                                  [node = &_node, space, how, work] {
                                    HeadReadEvidence out;
                                    auto* found = node->spaces()->find(space);
                                    if (!found) {
                                      out.why = space_not_open;
                                      return out;
                                    }
                                    try {
                                      switch (how) {
                                        case HeadRead::current:
                                          out.view = found->current(work);
                                          break;
                                        case HeadRead::local:
                                          out.view = found->local(work);
                                          break;
                                        case HeadRead::converged:
                                          out.view = found->converged(work);
                                          break;
                                      }
                                      out.finished = true;
                                    } catch (const contract::MaintenanceStopping& stopping) {
                                      out.stopped = true;
                                      out.why = stopping.what();
                                    } catch (const std::exception& error) {
                                      out.why = error.what();
                                    }
                                    return out;
                                  });
}

HeadReadEvidence Context::Heads::read(const heads::SpaceId& space, HeadRead how, std::chrono::milliseconds deadline, dannan::sched::WorkContext work) {
  check_deadline(deadline);
  return await<HeadReadEvidence>(
      _node, deadline, [&](boost::asio::any_io_executor executor, Read done) { read(space, how, deadline, std::move(executor), std::move(done), work); });
}

void Context::Heads::commit(const heads::SpaceId& space, heads::MutationFactory mutation, std::chrono::milliseconds deadline,
                            boost::asio::any_io_executor executor, Committed done, std::optional<heads::ExternalIdentity> identity, size_t retries,
                            dannan::sched::WorkContext work, bool supplied) {
  check_deadline(deadline);
  MutationEvidence late;
  late.why = "the deadline passed with the mutation still running; it may yet land, and its identity, if it has one, will tell";
  MutationEvidence refused;
  refused.why = _node.spaces() ? node_stopping : heads_off;
  run_on_spaces<MutationEvidence>(_node, deadline, std::move(executor), std::move(done), std::move(late), std::move(refused),
                                  [node = &_node, space, mutation = std::move(mutation), identity, retries, work, supplied] {
                                    MutationEvidence out;
                                    auto* found = node->spaces()->find(space);
                                    if (!found) {
                                      out.why = space_not_open;
                                      return out;
                                    }
                                    try {
                                      out.outcome =
                                          supplied ? found->mutate_delta(work, mutation, retries, identity) : found->mutate(work, mutation, retries, identity);
                                      out.finished = true;
                                    } catch (const contract::MaintenanceStopping& stopping) {
                                      out.stopped = true;
                                      out.why = stopping.what();
                                    } catch (const std::exception& error) {
                                      out.why = error.what();
                                    }
                                    return out;
                                  });
}

void Context::Heads::mutate(const heads::SpaceId& space, heads::MutationFactory mutation, std::chrono::milliseconds deadline,
                            boost::asio::any_io_executor executor, Committed done, std::optional<heads::ExternalIdentity> identity, size_t retries,
                            dannan::sched::WorkContext work) {
  commit(space, std::move(mutation), deadline, std::move(executor), std::move(done), identity, retries, work, false);
}

MutationEvidence Context::Heads::mutate(const heads::SpaceId& space, heads::MutationFactory mutation, std::chrono::milliseconds deadline,
                                        std::optional<heads::ExternalIdentity> identity, size_t retries, dannan::sched::WorkContext work) {
  check_deadline(deadline);
  return await<MutationEvidence>(_node, deadline, [&](boost::asio::any_io_executor executor, Committed done) {
    mutate(space, std::move(mutation), deadline, std::move(executor), std::move(done), identity, retries, work);
  });
}

void Context::Heads::mutate_delta(const heads::SpaceId& space, heads::MutationFactory mutation, std::chrono::milliseconds deadline,
                                  boost::asio::any_io_executor executor, Committed done, std::optional<heads::ExternalIdentity> identity, size_t retries,
                                  dannan::sched::WorkContext work) {
  commit(space, std::move(mutation), deadline, std::move(executor), std::move(done), identity, retries, work, true);
}

MutationEvidence Context::Heads::mutate_delta(const heads::SpaceId& space, heads::MutationFactory mutation, std::chrono::milliseconds deadline,
                                              std::optional<heads::ExternalIdentity> identity, size_t retries, dannan::sched::WorkContext work) {
  check_deadline(deadline);
  return await<MutationEvidence>(_node, deadline, [&](boost::asio::any_io_executor executor, Committed done) {
    mutate_delta(space, std::move(mutation), deadline, std::move(executor), std::move(done), identity, retries, work);
  });
}

heads::Space* Context::Heads::find(const heads::SpaceId& space, std::string& why) const {
  auto* spaces = _node.spaces();
  auto* found = spaces ? spaces->find(space) : nullptr;
  if (!found) why = spaces ? space_not_open : heads_off;
  return found;
}

HeadWatch Context::Heads::watch(const heads::SpaceId& space, boost::asio::any_io_executor executor, std::function<void(heads::HeadMoved)> notice) {
  HeadWatch out;
  auto* found = find(space, out.why);
  if (!found) return out;
  out.token = found->watch(std::move(executor), std::move(notice));
  out.watching = true;
  return out;
}

void Context::Heads::unwatch(const heads::SpaceId& space, uint64_t token) {
  std::string why;
  if (auto* found = find(space, why)) found->unwatch(token);
}

HeadsEvidence Context::Heads::heads(const heads::SpaceId& space) const {
  HeadsEvidence out;
  auto* spaces = _node.spaces();
  auto* found = spaces ? spaces->find(space) : nullptr;
  if (!found) {
    out.why = spaces ? space_not_open : heads_off;
    return out;
  }
  out.known = true;
  out.report = found->heads();
  out.owed = found->owed_backlog();
  if (auto* exchange = spaces->exchange(space)) {
    out.status = exchange->status();
    out.convergence = exchange->demand();
  }
  return out;
}

// ---- Tickets ----------------------------------------------------------------

std::optional<WriteEvidence> Context::Tickets::status(Ticket ticket) const { return _node.store().status(ticket); }

void Context::Tickets::wait(Ticket ticket, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, Done done) {
  check_deadline(deadline);
  _node.store().wait(ticket, deadline, std::move(executor), [done = std::move(done)](WriteEvidence evidence) { done(known(std::move(evidence))); });
}

std::optional<WriteEvidence> Context::Tickets::wait(Ticket ticket, std::chrono::milliseconds deadline) {
  check_deadline(deadline);
  return await<std::optional<WriteEvidence>>(
      _node, deadline, [&](boost::asio::any_io_executor executor, Done done) { wait(ticket, deadline, std::move(executor), std::move(done)); });
}

void Context::Tickets::wait_complete(Ticket ticket, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, Done done) {
  check_deadline(deadline);
  _node.store().wait_complete(ticket, deadline, std::move(executor), [done = std::move(done)](WriteEvidence evidence) { done(known(std::move(evidence))); });
}

std::optional<WriteEvidence> Context::Tickets::wait_complete(Ticket ticket, std::chrono::milliseconds deadline) {
  check_deadline(deadline);
  return await<std::optional<WriteEvidence>>(
      _node, deadline, [&](boost::asio::any_io_executor executor, Done done) { wait_complete(ticket, deadline, std::move(executor), std::move(done)); });
}

}  // namespace dagda
