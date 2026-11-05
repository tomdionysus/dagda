//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/storage_pool.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <string_view>
#include <utility>

#include "dagda/store/placement.h"
#include "dagda/store/state_layout.h"
#include "dannan/identity/crypto.h"
#include "dannan/identity/durable_file.h"

namespace dagda::store {
namespace {

using dannan::contract::Lock;
using dannan::identity::NodeId;

std::string read_text(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot read " + path.string());
  std::string text((std::istreambuf_iterator<char>(in)), {});
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
  return text;
}

constexpr std::string_view key_check_label = "dagda/backend-key-check/19";
// Macha's marker, recognised only to be refused.
constexpr std::string_view backend_marker_v18 = "macha-backend-v18";
// How many held records the probe tries before it gives up on the key: a
// corrupt record is not a wrong key.
constexpr size_t probe_records = 8;

// A marker's fields: its domain and key check, node and token.
struct Marker {
  SealingScheme sealing;
  NodeId node;
  NodeId token;
  std::string check;
};

// Throws for a v18 marker, naming the migration; nothing for any other text
// that is not a v19 marker.
std::optional<Marker> parse_marker(const std::string& text, const std::filesystem::path& state) {
  std::vector<std::string> fields;
  for (size_t from = 0; from <= text.size();) {
    const auto end = std::min(text.find(' ', from), text.size());
    fields.push_back(text.substr(from, end - from));
    from = end + 1;
  }
  if (fields.size() == 3 && fields[0] == backend_marker_v18) throw std::runtime_error("backend is " + migrate_first(state));
  if (fields.size() != 5 || fields[0] != backend_marker_v19) return std::nullopt;
  const auto domain = domain::DomainId::parse(fields[3]);
  if (!domain || (fields[4] != backend_blind_check && fields[4].size() != 32)) return std::nullopt;
  const auto n = NodeId::parse(fields[1]);
  const auto t = NodeId::parse(fields[2]);
  if (!n || !t) return std::nullopt;
  return Marker{SealingScheme::v19(*domain), *n, *t, fields[4]};
}

}  // namespace

std::optional<std::string> backend_refusal_v18(const std::filesystem::path& state, const std::filesystem::path& backend) {
  const auto marker = backend / backend_marker_name;
  std::error_code ec;
  if (!std::filesystem::exists(marker, ec)) return std::nullopt;
  std::string text;
  try {
    text = read_text(marker);
  } catch (const std::exception&) {
    // Unreadable: the pool's own open reports it.
    return std::nullopt;
  }
  try {
    (void)parse_marker(text, state);
  } catch (const std::exception& error) {
    return "backend " + backend.string() + ": " + error.what();
  }
  return std::nullopt;
}

std::string backend_key_check(const domain::StorageKey& key, const domain::DomainId& domain, const NodeId& token) {
  Bytes message(key_check_label.begin(), key_check_label.end());
  message.insert(message.end(), domain.bytes.begin(), domain.bytes.end());
  message.insert(message.end(), token.bytes.begin(), token.bytes.end());
  const auto mac = dannan::identity::hmac_sha256(key.bytes(), message);
  return dannan::identity::hex(std::span<const uint8_t>(mac.data(), 16));
}

std::string backend_marker_text(const SealingScheme& sealing, const NodeId& node, const NodeId& token, const std::string& check) {
  return std::string(backend_marker_v19) + " " + dannan::identity::to_string(node) + " " + dannan::identity::to_string(token) + " " +
         domain::to_string(sealing.domain) + " " + check;
}

std::filesystem::path backend_identity_path(const std::filesystem::path& state, const std::filesystem::path& backend) {
  const auto text = backend.lexically_normal().string();
  const auto hash = dannan::identity::sha256({reinterpret_cast<const uint8_t*>(text.data()), text.size()});
  return state / "backend-identities" / (dannan::identity::hex(hash) + ".id");
}

namespace {}  // namespace

struct StoragePool::Backend {
  // Guards the small in-memory state below. No filesystem operation is
  // performed while it is held.
  mutable dannan::contract::Mutex mutex;
  StorageBackendOptions options DANNAN_GUARDED_BY(mutex);
  NodeId token DANNAN_GUARDED_BY(mutex);
  bool token_known DANNAN_GUARDED_BY(mutex) = false;
  bool online DANNAN_GUARDED_BY(mutex) = false;
  bool configured DANNAN_GUARDED_BY(mutex) = true;
  uint64_t generation DANNAN_GUARDED_BY(mutex) = 1;
  uint64_t instance_id DANNAN_GUARDED_BY(mutex) = 0;
  std::shared_ptr<DurabilityDomain> durability_domain DANNAN_GUARDED_BY(mutex);
  std::shared_ptr<LocalStore> store DANNAN_GUARDED_BY(mutex);
  // This node's key has been checked against the disk: by the marker's
  // check, or by the probe opening a record. Only then may a copy that does
  // not open be discarded as corrupt.
  bool key_checked DANNAN_GUARDED_BY(mutex) = false;
  // The held removals of every store this backend has let go, and one for
  // each going: with the current store's, never smaller than before.
  uint64_t departed_removals DANNAN_GUARDED_BY(mutex) = 0;
  std::string last_error DANNAN_GUARDED_BY(mutex);
};

StoragePool::StoragePool(std::filesystem::path state, NodeId node, NodeId epoch, StoragePoolOptions options, std::shared_ptr<const domain::StorageKey> key,
                         LocalStore::Services services)
    : _state(std::move(state)),
      _node(node),
      _epoch(epoch),
      _key(std::move(key)),
      _sealing(options.sealing),
      _role(options.role),
      _ledger_key(options.ledger_key),
      _services(std::move(services)),
      _durability_batch_window(options.durability_batch_window),
      _pack_threshold(options.pack_threshold),
      _pack_target_size(options.pack_target_size),
      _ledger_cache_bytes(options.ledger_cache_bytes),
      _service_monitor(_services.logger.child("pressure")) {
  if (_role == domain::Role::member && !_key) throw std::invalid_argument("storage pool needs a storage key");
  if (!_ledger_key)
    throw std::invalid_argument(
        "a pool's held ledgers are sealed under the node-local key (StoragePoolOptions::ledger_key): a node without custody cannot run a store");
  reconfigure(options.backends);
  refresh();
}

StoragePool::~StoragePool() = default;

std::vector<std::shared_ptr<StoragePool::Backend>> StoragePool::snapshot() const {
  Lock lock(_mutex);
  return _backends;
}

std::shared_ptr<DurabilityDomain> StoragePool::domain_for(const std::filesystem::path& path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) throw std::runtime_error("cannot stat storage durability domain " + path.string());
  const auto device = static_cast<uint64_t>(st.st_dev);
  Lock lock(_domain_mutex);
  if (const auto found = _domains_by_device.find(device); found != _domains_by_device.end() && !found->second->failed()) {
    found->second->add_representative(path);
    return found->second;
  }
  auto domain = std::make_shared<DurabilityDomain>(
      _next_domain_id++, path, DurabilityDomain::Services{_services.threads, _services.observations, _services.time}, _durability_batch_window);
  _domains_by_device[device] = domain;
  return domain;
}

std::filesystem::path StoragePool::identity_path(const std::filesystem::path& backend) const { return backend_identity_path(_state, backend); }

std::optional<bool> StoragePool::probe_key(LocalStore& store, std::string& reason) const {
  LocalStore::Cursor cursor;
  bool exhausted = false;
  size_t tried = 0;
  while (tried < probe_records) {
    const auto id = store.next_object(cursor, exhausted);
    if (!id) {
      if (exhausted) break;
      continue;
    }
    ++tried;
    try {
      if (store.get(*id)) return true;
    } catch (const std::exception& error) {
      reason = "record " + to_string(*id) + " does not open under this node's key: " + error.what();
    }
  }
  if (!tried) return std::nullopt;
  return false;
}

bool StoragePool::key_checked(const std::shared_ptr<Backend>& backend) {
  Lock lock(backend->mutex);
  return backend->key_checked;
}

void StoragePool::deactivate(const std::shared_ptr<Backend>& backend, const std::shared_ptr<LocalStore>& expected, const std::string& reason,
                             uint64_t expected_generation) const {
  std::shared_ptr<LocalStore> retired;
  std::filesystem::path path;
  bool log = false;
  bool was_online = false;
  {
    Lock lock(backend->mutex);
    if ((expected_generation && backend->generation != expected_generation) || (expected && backend->store != expected)) return;
    path = backend->options.path;
    was_online = backend->online;
    log = backend->online || backend->last_error != reason;
    backend->online = false;
    if (backend->store) backend->departed_removals += backend->store->held_removals() + 1;
    retired = std::move(backend->store);
    backend->durability_domain.reset();
    backend->instance_id = 0;
    backend->last_error = reason;
  }
  // A store's destruction may join its threads. Never while holding backend
  // state.
  retired.reset();
  if (log) _services.logger.warn("storage backend offline " + path.string() + ": " + reason);
  if (was_online) _services.observations.event({_services.wall.unix_ms(), "backend_offline", {}, {{"path", path.string()}, {"reason", reason}}});
}

bool StoragePool::activate(const std::shared_ptr<Backend>& backend) {
  StorageBackendOptions options;
  NodeId known_token;
  bool token_known = false;
  bool configured = false;
  bool was_online = false;
  uint64_t generation = 0;
  std::shared_ptr<LocalStore> existing;
  {
    Lock lock(backend->mutex);
    options = backend->options;
    known_token = backend->token;
    token_known = backend->token_known;
    configured = backend->configured;
    was_online = backend->online;
    generation = backend->generation;
    existing = backend->store;
  }
  if (!configured) return false;

  // Measured for the log only, on the real clock.
  const auto activate_started = dannan::contract::Clock::now();
  if (!was_online) _services.logger.debug("storage backend probe begin path=" + options.path.string());
  try {
    std::error_code ec;
    if (!std::filesystem::is_directory(options.path, ec) || ec) {
      deactivate(backend, existing, ec ? ec.message() : "configured path is not a directory", generation);
      return false;
    }

    const auto state_id = identity_path(options.path);
    const auto marker = options.path / backend_marker_name;
    const bool have_state = std::filesystem::exists(state_id, ec) && !ec;
    ec.clear();
    const bool have_marker = std::filesystem::exists(marker, ec) && !ec;

    NodeId token = known_token;
    // Whether the key has been proved against the marker; a blind-adopted
    // disk is probed below.
    bool checked = false;
    bool probe = false;
    if (have_state) {
      const auto expected = parse_marker(read_text(state_id), _state);
      if (!expected || expected->node != _node) throw std::runtime_error("invalid backend identity in node state");
      token = expected->token;
      token_known = true;
    }

    if (have_marker) {
      const auto found = parse_marker(read_text(marker), _state);
      if (!found) throw std::runtime_error("invalid backend marker");
      if (found->node != _node) throw std::runtime_error("backend belongs to another node identity");
      // A disk is its domain's for ever: it opens only in that domain.
      if (found->sealing != _sealing)
        throw std::runtime_error("backend belongs to domain " + domain::to_string(found->sealing.domain) + "; this pool is domain " +
                                 domain::to_string(_sealing.domain));
      if (token_known && found->token != token) throw std::runtime_error("backend identity mismatch (wrong disk mounted)");
      // A member proves it has the domain's key; a blind pool has none and
      // takes the disk as it is.
      if (_key && found->check != backend_blind_check) {
        if (found->check != backend_key_check(*_key, _sealing.domain, found->token))
          throw std::runtime_error("backend key check failed: this node lacks the key of domain " + domain::to_string(_sealing.domain));
        checked = true;
      }
      probe = _key && found->check == backend_blind_check;
      token = found->token;
      token_known = true;
      if (!have_state) dannan::identity::durable_replace_file(state_id, backend_marker_text(_sealing, _node, found->token, found->check) + "\n");
    } else if (have_state) {
      deactivate(backend, existing, "known backend marker is absent", generation);
      return false;
    } else {
      // A directory without a versioned backend marker is adoptable only when
      // empty, so a foreign layout is never read as current DATA.
      // The store's lock file aside: the node takes the lock before it opens
      // the pool.
      bool empty = true;
      for (std::filesystem::directory_iterator it(options.path, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().filename() != storage_lock_name) empty = false;
      if (ec) throw std::runtime_error("cannot inspect unversioned backend: " + ec.message());
      if (!empty)
        throw std::runtime_error("backend " + options.path.string() +
                                 " holds files but no backend marker, so it is not a Dagda backend: the pool adopts only an empty directory");
      token = NodeId::random();
      token_known = true;
      const auto check = _key ? backend_key_check(*_key, _sealing.domain, token) : std::string(backend_blind_check);
      dannan::identity::durable_replace_file(marker, backend_marker_text(_sealing, _node, token, check) + "\n");
      dannan::identity::durable_replace_file(state_id, backend_marker_text(_sealing, _node, token, check) + "\n");
      checked = _key != nullptr;
    }

    const auto settled = parse_marker(read_text(marker), _state);
    if (!settled || settled->node != _node || settled->token != token || settled->sealing != _sealing) throw std::runtime_error("backend marker changed");

    auto store = existing;
    std::shared_ptr<DurabilityDomain> durability_domain;
    uint64_t instance_id = 0;
    if (!store) {
      durability_domain = domain_for(options.path);
      {
        Lock lock(_domain_mutex);
        instance_id = _next_backend_instance++;
      }
      LocalStoreOptions local;
      local.limit = options.limit;
      local.reserve_free = options.reserve_free;
      local.pack_threshold = _pack_threshold;
      local.pack_target_size = _pack_target_size;
      // Named by the disk's own identity token: another disk in the same
      // place starts its own ledger.
      local.ledger_dir = _state / "ledger" / ("data-" + dannan::identity::to_string(token));
      local.ledger_cache_bytes = static_cast<size_t>(_ledger_cache_bytes / std::max<size_t>(1, _ledger_shares.load()));
      local.sealing = _sealing;
      local.role = _role;
      local.ledger_key = _ledger_key;
      store = std::make_shared<LocalStore>(options.path, local, _key,
                                           LocalStore::Services{_services.threads, _services.observations, _services.progress,
                                                                _services.logger.child("backend"), _services.time, _services.wall},
                                           LocalStoreMode::authoritative, durability_domain);
    } else {
      Lock lock(backend->mutex);
      durability_domain = backend->durability_domain;
      instance_id = backend->instance_id;
      checked = checked || backend->key_checked;
    }

    if (probe && !checked) {
      // The marker cannot vouch for the key: a record the disk holds must.
      // Nothing is discarded on the way, whatever the answer.
      std::string reason;
      const auto opened = probe_key(*store, reason);
      if (opened == false) {
        // The new store goes before the backend is reported: its threads
        // join here, outside every lock.
        store.reset();
        deactivate(backend, existing, "backend key probe failed: " + reason, generation);
        return false;
      }
      if (opened == true) {
        checked = true;
        // Proven: the marker carries the check from now on, so the next open
        // is refused at the marker if the key is wrong.
        const auto text = backend_marker_text(_sealing, _node, token, backend_key_check(*_key, _sealing.domain, token)) + "\n";
        dannan::identity::durable_replace_file(marker, text);
        dannan::identity::durable_replace_file(state_id, text);
      }
    }

    bool installed = false;
    {
      Lock lock(backend->mutex);
      if (backend->generation == generation && backend->configured && backend->options.path == options.path && backend->options.limit == options.limit &&
          backend->options.reserve_free == options.reserve_free) {
        backend->token = token;
        backend->token_known = token_known;
        backend->store = store;
        backend->durability_domain = durability_domain;
        backend->instance_id = instance_id;
        backend->key_checked = checked;
        backend->online = true;
        backend->last_error.clear();
        installed = true;
      }
    }
    if (!installed) return false;
    if (!was_online) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(dannan::contract::Clock::now() - activate_started);
      _services.logger.info("storage backend online " + options.path.string() + " elapsed_ms=" + std::to_string(elapsed.count()) +
                            " accounting=" + std::string(store->scan_complete() ? "ready" : "reconciling"));
      _services.observations.event(
          {_services.wall.unix_ms(), "backend_online", {{"elapsed_ms", static_cast<uint64_t>(elapsed.count())}}, {{"path", options.path.string()}}});
    }
    return true;
  } catch (const std::exception& error) {
    deactivate(backend, existing, error.what(), generation);
    return false;
  }
}

void StoragePool::reconfigure(const std::vector<StorageBackendOptions>& backends) {
  std::vector<std::shared_ptr<LocalStore>> retired;
  {
    Lock lock(_mutex);
    std::set<std::filesystem::path> wanted;
    for (const auto& options : backends)
      if (!wanted.insert(options.path.lexically_normal()).second)
        throw std::invalid_argument("duplicate storage backend: " + options.path.lexically_normal().string());
    _ledger_shares.store(std::max<size_t>(1, backends.size()));
    for (const auto& options : backends) {
      const auto normalized = options.path.lexically_normal();
      const auto existing = std::find_if(_backends.begin(), _backends.end(), [&](const auto& backend) {
        Lock backend_lock(backend->mutex);
        return backend->options.path.lexically_normal() == normalized;
      });
      if (existing == _backends.end()) {
        auto backend = std::make_shared<Backend>();
        {
          Lock backend_lock(backend->mutex);
          backend->options = options;
          backend->configured = true;
        }
        _backends.push_back(std::move(backend));
        continue;
      }
      auto& backend = **existing;
      Lock backend_lock(backend.mutex);
      backend.configured = true;
      if (backend.options.limit != options.limit || backend.options.reserve_free != options.reserve_free) {
        backend.options.limit = options.limit;
        backend.options.reserve_free = options.reserve_free;
        ++backend.generation;
        backend.online = false;
        if (backend.store) backend.departed_removals += backend.store->held_removals() + 1;
        retired.push_back(std::move(backend.store));
      }
    }
    for (auto& backend : _backends) {
      Lock backend_lock(backend->mutex);
      if (!wanted.contains(backend->options.path.lexically_normal())) {
        backend->configured = false;
        backend->online = false;
        ++backend->generation;
        if (backend->store) backend->departed_removals += backend->store->held_removals() + 1;
        retired.push_back(std::move(backend->store));
        backend->last_error = "removed from configuration";
      }
    }
  }
  // Retired stores go after every pool and backend lock is released.
  retired.clear();
}

void StoragePool::refresh() {
  size_t online = 0;
  for (const auto& backend : snapshot()) {
    bool configured = false;
    {
      Lock lock(backend->mutex);
      configured = backend->configured;
    }
    if (configured && activate(backend)) ++online;
  }
  _online_backends.store(online, std::memory_order_relaxed);
}

std::vector<std::shared_ptr<StoragePool::Backend>> StoragePool::ranked(const ObjectId& id) const {
  std::vector<dannan::membership::NodeRecord> candidates;
  std::vector<std::pair<NodeId, std::shared_ptr<Backend>>> lookup;
  for (const auto& backend : snapshot()) {
    NodeId token;
    uint64_t capacity = 0;
    bool eligible = false;
    {
      Lock lock(backend->mutex);
      eligible = backend->configured && backend->token_known;
      token = backend->token;
      capacity = backend->options.limit;
    }
    if (!eligible) continue;
    candidates.push_back(placement_candidate(token, capacity));
    lookup.emplace_back(token, backend);
  }

  std::vector<std::shared_ptr<Backend>> out;
  for (const auto& placed : data_order(id.bytes, candidates, 1)) {
    const auto it = std::find_if(lookup.begin(), lookup.end(), [&](const auto& item) { return item.first == placed; });
    if (it != lookup.end()) out.push_back(it->second);
  }
  return out;
}

bool StoragePool::put(const ObjectId& id, std::span<const uint8_t> data) {
  if (_role != domain::Role::member) throw std::logic_error("a blind pool holds ciphertext only: put_sealed");
  for (const auto& backend : ranked(id)) {
    std::shared_ptr<LocalStore> store;
    std::filesystem::path path;
    {
      Lock lock(backend->mutex);
      if (!backend->online || !backend->store) continue;
      store = backend->store;
      path = backend->options.path;
    }
    try {
      // put() also reaffirms an existing object's age so GC cannot race a
      // write reusing an old orphan: never short-circuit through has(). One
      // sample is one device operation: ranking and locks are not charged to
      // the disk.
      bool stored = false;
      {
        DiskServiceTimer timer(&_service_monitor, data.size(), _services.time);
        stored = store->put(id, data);
      }
      if (stored) return true;
    } catch (const std::exception& error) {
      _services.logger.debug("storage write failed " + path.string() + ": " + error.what());
      deactivate(backend, store, error.what());
    }
  }
  return false;
}

std::optional<DurabilityTicket> StoragePool::put_deferred(const ObjectId& id, std::span<const uint8_t> data) {
  if (_role != domain::Role::member) throw std::logic_error("a blind pool holds ciphertext only: put_sealed");
  // The provisional write path: a deferred write puts every extent here.
  for (const auto& backend : ranked(id)) {
    std::shared_ptr<LocalStore> store;
    std::filesystem::path path;
    uint64_t backend_instance = 0;
    uint64_t domain = 0;
    {
      Lock lock(backend->mutex);
      if (!backend->online || !backend->store || !backend->durability_domain || !backend->instance_id) continue;
      store = backend->store;
      path = backend->options.path;
      backend_instance = backend->instance_id;
      domain = backend->durability_domain->id();
    }
    try {
      std::optional<uint64_t> generation;
      {
        DiskServiceTimer timer(&_service_monitor, data.size(), _services.time);
        generation = store->put_deferred(id, data);
      }
      if (!generation) continue;

      // The ticket names the exact backend incarnation that took the object.
      // If it went away or reopened while the put was in progress, the new
      // incarnation must not satisfy the old publication merely because it
      // shares a filesystem.
      {
        Lock lock(backend->mutex);
        if (!backend->online || backend->store != store || backend->instance_id != backend_instance || !backend->durability_domain ||
            backend->durability_domain->id() != domain)
          continue;
      }
      return DurabilityTicket{_epoch, domain, *generation, backend_instance};
    } catch (const std::exception& error) {
      _services.logger.debug("storage deferred write failed " + path.string() + ": " + error.what());
      deactivate(backend, store, error.what());
    }
  }
  return std::nullopt;
}

void StoragePool::durability_barrier(const DurabilityTicket& ticket, DurabilityUrgency urgency) {
  if (!ticket.valid()) throw std::invalid_argument("invalid storage durability ticket");
  if (ticket.epoch != _epoch) throw StaleDurabilityTicket("storage durability ticket is from another process");
  for (const auto& backend : snapshot()) {
    std::shared_ptr<LocalStore> store;
    std::filesystem::path path;
    {
      Lock lock(backend->mutex);
      if (!backend->online || !backend->store || !backend->durability_domain || backend->instance_id != ticket.backend_instance ||
          backend->durability_domain->id() != ticket.domain)
        continue;
      store = backend->store;
      path = backend->options.path;
    }
    try {
      store->durability_barrier(ticket.generation, urgency);
      {
        Lock lock(backend->mutex);
        if (!backend->online || backend->store != store || backend->instance_id != ticket.backend_instance || !backend->durability_domain ||
            backend->durability_domain->id() != ticket.domain)
          throw StaleDurabilityTicket("storage durability placement changed while awaiting barrier");
      }
      return;
    } catch (const std::exception& error) {
      _services.logger.warn("storage durability barrier failed " + path.string() + ": " + error.what());
      deactivate(backend, store, error.what());
      throw;
    }
  }
  throw StaleDurabilityTicket("storage durability placement is no longer available");
}

bool StoragePool::durability_covered(const DurabilityTicket& ticket) const {
  if (!ticket.valid() || ticket.epoch != _epoch) return false;
  for (const auto& backend : snapshot()) {
    Lock lock(backend->mutex);
    if (!backend->online || !backend->store || !backend->durability_domain || backend->instance_id != ticket.backend_instance ||
        backend->durability_domain->id() != ticket.domain)
      continue;
    return backend->durability_domain->durable_generation() >= ticket.generation;
  }
  return false;
}

void StoragePool::observe_get(size_t bytes, uint64_t elapsed_ms) const {
  _diag_gets.fetch_add(1, std::memory_order_relaxed);
  _diag_get_bytes.fetch_add(bytes, std::memory_order_relaxed);
  _diag_get_ms.fetch_add(elapsed_ms, std::memory_order_relaxed);
  auto maximum = _diag_get_max_ms.load(std::memory_order_relaxed);
  while (maximum < elapsed_ms && !_diag_get_max_ms.compare_exchange_weak(maximum, elapsed_ms, std::memory_order_relaxed)) {
  }

  if (!_services.logger.enabled(dannan::status::LogLevel::all)) return;
  // A log line's window: the real clock.
  const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(dannan::contract::Clock::now().time_since_epoch()).count();
  auto previous = _diag_get_report_ns.load(std::memory_order_relaxed);
  constexpr int64_t report_ns = 5'000'000'000LL;
  if (!previous) {
    int64_t unset = 0;
    (void)_diag_get_report_ns.compare_exchange_strong(unset, now_ns, std::memory_order_relaxed);
    return;
  }
  if (now_ns - previous < report_ns) return;
  if (!_diag_get_report_ns.compare_exchange_strong(previous, now_ns, std::memory_order_relaxed)) return;

  const auto window_ms = static_cast<uint64_t>((now_ns - previous) / 1'000'000LL);
  const auto gets = _diag_gets.exchange(0, std::memory_order_relaxed);
  const auto total_bytes = _diag_get_bytes.exchange(0, std::memory_order_relaxed);
  const auto total_ms = _diag_get_ms.exchange(0, std::memory_order_relaxed);
  const auto max_ms = _diag_get_max_ms.exchange(0, std::memory_order_relaxed);
  if (!gets) return;
  _services.logger.trace("DIAG storage-get window_ms=" + std::to_string(window_ms) + " gets=" + std::to_string(gets) + " bytes=" + std::to_string(total_bytes) +
                         " avg_ms=" + std::to_string(total_ms / gets) + " max_ms=" + std::to_string(max_ms));
}

bool StoragePool::put_sealed(const ObjectId& id, const LocalStore::SealedObject& sealed) {
  // The record's fault, not a backend's.
  if (sealed.ciphertext.size() != sealed.plain_size) throw std::invalid_argument("sealed object size mismatch");
  for (const auto& backend : ranked(id)) {
    std::shared_ptr<LocalStore> store;
    std::filesystem::path path;
    uint64_t generation = 0;
    {
      Lock lock(backend->mutex);
      if (!backend->online || !backend->store) continue;
      store = backend->store;
      path = backend->options.path;
      generation = backend->generation;
    }
    try {
      if (store->put_sealed(id, sealed)) return true;
    } catch (const std::exception& error) {
      deactivate(backend, store, error.what(), generation);
    }
  }
  return false;
}

std::optional<LocalStore::SealedObject> StoragePool::get_sealed(const ObjectId& id) const {
  for (const auto& backend : ranked(id)) {
    std::shared_ptr<LocalStore> store;
    std::filesystem::path path;
    {
      Lock lock(backend->mutex);
      if (!backend->online || !backend->store) continue;
      store = backend->store;
      path = backend->options.path;
    }
    try {
      if (auto sealed = store->get_sealed(id)) return sealed;
    } catch (const std::exception& error) {
      _services.logger.warn("unreadable local record " + to_string(id) + " on " + path.string() + ": " + error.what());
    }
  }
  return std::nullopt;
}

std::optional<dannan::identity::Hash256> StoragePool::held_digest(const ObjectId& id) const {
  const auto sealed = get_sealed(id);
  if (!sealed) return std::nullopt;
  return blind_digest(sealed->nonce, sealed->tag, sealed->ciphertext);
}

std::optional<Bytes> StoragePool::get(const ObjectId& id) const {
  // A blind pool cannot open what it holds; a get is a programming error,
  // never a discarded object.
  if (_role != domain::Role::member) throw std::logic_error("a blind pool holds ciphertext only: get_sealed");
  // Reads feed the same pressure signal: slow reads starve a viewer as slow
  // writes do.
  for (const auto& backend : ranked(id)) {
    std::shared_ptr<LocalStore> store;
    std::filesystem::path path;
    {
      Lock lock(backend->mutex);
      if (!backend->online || !backend->store) continue;
      store = backend->store;
      path = backend->options.path;
    }
    try {
      if (!store->has(id)) continue;
      const auto started = dannan::contract::Clock::now();
      std::optional<Bytes> data;
      {
        DiskServiceTimer timer(&_service_monitor, 0, _services.time);
        data = store->get(id);
        // The sample's expectation depends on size; without it every read is
        // judged against the fixed overhead alone.
        timer.note_bytes(data ? data->size() : 0);
      }
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(dannan::contract::Clock::now() - started);
      if (_services.logger.enabled(dannan::status::LogLevel::all))
        _services.logger.trace("DIAG backend-get path=" + path.string() + " id=" + to_string(id) + " result=" + std::to_string(data ? 1 : 0) +
                               " bytes=" + std::to_string(data ? data->size() : 0) + " ms=" + std::to_string(elapsed.count()));
      observe_get(data ? data->size() : 0, static_cast<uint64_t>(elapsed.count()));
      return data;
    } catch (const ObjectReadError& error) {
      // The device, not the object: it stays, and the next backend is asked.
      _services.logger.warn("unreadable local object " + to_string(id) + " on " + path.string() + " kept (read failed): " + error.what());
    } catch (const std::exception& error) {
      // Corrupt content is an object failure, not a backend failure, once the
      // key is known to be the disk's; before that it may be the key.
      if (!key_checked(backend)) {
        _services.logger.warn("unreadable local object " + to_string(id) + " on " + path.string() + " kept (key unchecked): " + error.what());
        continue;
      }
      _services.logger.warn("discarding unreadable local object " + to_string(id) + " on " + path.string() + ": " + error.what());
      try {
        (void)store->remove(id);
      } catch (...) {
      }
    }
  }
  return std::nullopt;
}

bool StoragePool::has(const ObjectId& id) const {
  for (const auto& backend : ranked(id)) {
    std::shared_ptr<LocalStore> store;
    {
      Lock lock(backend->mutex);
      if (backend->online) store = backend->store;
    }
    if (!store) continue;
    try {
      if (store->has(id)) return true;
    } catch (...) {
    }
  }
  return false;
}

std::optional<DurabilityTicket> StoragePool::reassert_durable(const ObjectId& id) {
  for (const auto& backend : ranked(id)) {
    std::shared_ptr<LocalStore> store;
    std::filesystem::path path;
    uint64_t backend_instance = 0;
    uint64_t domain = 0;
    {
      Lock lock(backend->mutex);
      if (!backend->online || !backend->store || !backend->durability_domain || !backend->instance_id) continue;
      store = backend->store;
      path = backend->options.path;
      backend_instance = backend->instance_id;
      domain = backend->durability_domain->id();
    }
    try {
      if (!store->has(id)) continue;
      // Anything this incarnation wrote reaches disk before the ticket is
      // handed out; what a previous incarnation wrote is on disk by
      // construction, since the index was rebuilt from the packs.
      store->durability_barrier();
      return DurabilityTicket{_epoch, domain, store->durable_generation(), backend_instance};
    } catch (const std::exception& error) {
      _services.logger.warn("storage durability reassertion failed " + path.string() + ": " + error.what());
    }
  }
  return std::nullopt;
}

bool StoragePool::valid(const ObjectId& id) const {
  for (const auto& backend : ranked(id)) {
    std::shared_ptr<LocalStore> store;
    {
      Lock lock(backend->mutex);
      if (backend->online) store = backend->store;
    }
    if (store && store->valid(id)) return true;
  }
  return false;
}

contract::HeldView StoragePool::held_view() const {
  struct Part {
    NodeId token;
    contract::HeldView view;
  };
  std::vector<Part> parts;
  contract::HeldIdentity identity{{}, 0, true};
  for (const auto& backend : snapshot()) {
    std::shared_ptr<LocalStore> store;
    NodeId token;
    {
      Lock lock(backend->mutex);
      identity.removals += backend->departed_removals;
      if (backend->online) store = backend->store;
      token = backend->token;
    }
    if (!store) continue;
    auto view = store->held_view();
    identity.removals += view.identity.removals;
    if (!view.identity.complete) {
      identity.complete = false;
      continue;
    }
    parts.push_back({token, std::move(view)});
  }
  // By disk, so each union lines up with the one before.
  std::sort(parts.begin(), parts.end(), [](const Part& a, const Part& b) { return a.token < b.token; });
  std::vector<ledger::ObjectTrie::Snapshot> tries;
  for (auto& part : parts) tries.insert(tries.end(), part.view.trie->parts().begin(), part.view.trie->parts().end());
  std::shared_ptr<const ledger::TrieUnion> trie;
  {
    Lock lock(_union_mutex);
    _union = std::make_shared<const ledger::TrieUnion>(std::move(tries), _union.get());
    trie = _union;
  }
  identity.hash = trie->root_hash();
  return {identity, [trie](const ObjectId& id) { return trie->get(id).has_value(); }, std::move(trie)};
}

bool StoragePool::remove(const ObjectId& id) {
  bool removed = false;
  for (const auto& backend : snapshot()) {
    std::shared_ptr<LocalStore> store;
    {
      Lock lock(backend->mutex);
      if (backend->online) store = backend->store;
    }
    if (!store) continue;
    try {
      removed = store->remove(id) || removed;
    } catch (...) {
    }
  }
  return removed;
}

std::vector<ObjectId> StoragePool::list() const {
  _full_list_scans.fetch_add(1, std::memory_order_relaxed);
  std::set<ObjectId> unique;
  for (const auto& backend : snapshot()) {
    std::shared_ptr<LocalStore> store;
    {
      Lock lock(backend->mutex);
      if (backend->online) store = backend->store;
    }
    if (!store) continue;
    try {
      const auto ids = store->list();
      unique.insert(ids.begin(), ids.end());
    } catch (...) {
    }
  }
  return {unique.begin(), unique.end()};
}

std::optional<StoragePool::CursorItem> StoragePool::next_physical(Cursor& cursor, bool& pass_complete) const {
  pass_complete = false;
  struct View {
    std::shared_ptr<Backend> backend;
    std::shared_ptr<LocalStore> store;
    std::filesystem::path path;
  };
  std::vector<View> views;
  for (const auto& backend : snapshot()) {
    Lock lock(backend->mutex);
    if (backend->online && backend->store) views.push_back({backend, backend->store, backend->options.path});
  }
  if (views.empty()) {
    cursor = {};
    pass_complete = true;
    return std::nullopt;
  }

  // A change of backends or of a backend's store invalidates only the
  // maintenance cursor; foreground operations are independent of it.
  if (cursor.backend_count != views.size() || cursor.backend_index >= views.size() || (cursor.store && cursor.store != views[cursor.backend_index].store)) {
    cursor = {};
    cursor.backend_count = views.size();
  }
  if (!cursor.store) cursor.store = views[cursor.backend_index].store;

  while (cursor.completed_backends < views.size()) {
    auto& view = views[cursor.backend_index];
    if (cursor.store != view.store) {
      cursor.store = view.store;
      cursor.local = {};
    }
    bool exhausted = false;
    if (const auto id = view.store->next_object(cursor.local, exhausted)) return CursorItem{view.backend, view.store, view.path, *id};
    if (!exhausted) return std::nullopt;

    ++cursor.completed_backends;
    cursor.backend_index = (cursor.backend_index + 1) % views.size();
    cursor.store = views[cursor.backend_index].store;
    cursor.local = {};
  }

  cursor = {};
  pass_complete = true;
  return std::nullopt;
}

std::optional<ObjectId> StoragePool::next_object(Cursor& cursor, bool& pass_complete) const {
  const auto item = next_physical(cursor, pass_complete);
  return item ? std::optional<ObjectId>{item->id} : std::nullopt;
}

bool StoragePool::older_than(const ObjectId& id, std::chrono::milliseconds age) const {
  for (const auto& backend : snapshot()) {
    std::shared_ptr<LocalStore> store;
    {
      Lock lock(backend->mutex);
      if (backend->online) store = backend->store;
    }
    if (!store) continue;
    try {
      if (store->has(id) && store->older_than(id, age)) return true;
    } catch (...) {
    }
  }
  return false;
}

StoragePool::MaintenanceResult StoragePool::rebalance_step(uint64_t budget_bytes, size_t operation_budget, const std::function<bool()>& should_yield) {
  MaintenanceResult result;
  // With one backend online there is nowhere to move anything, and checking
  // where each object sits would read the whole store.
  size_t online = 0;
  for (const auto& backend : snapshot()) {
    Lock lock(backend->mutex);
    online += backend->online ? 1 : 0;
  }
  if (online < 2) {
    result.complete = true;
    return result;
  }
  while (!operation_budget || result.objects < operation_budget) {
    if (should_yield && should_yield()) {
      result.yielded = true;
      return result;
    }
    bool pass_complete = false;
    const auto item = next_physical(_rebalance_cursor, pass_complete);
    if (!item) {
      result.complete = pass_complete;
      return result;
    }
    ++result.objects;
    const auto id = item->id;

    const auto order = ranked(id);
    if (order.empty()) continue;
    const auto preferred = order.front();

    struct Holder {
      std::shared_ptr<Backend> backend;
      std::shared_ptr<LocalStore> store;
    };
    std::vector<Holder> holders;
    uint64_t estimated = 0;
    for (const auto& backend : snapshot()) {
      std::shared_ptr<LocalStore> store;
      {
        Lock lock(backend->mutex);
        if (backend->online) store = backend->store;
      }
      if (!store) continue;
      try {
        // Nothing re-checks this decision downstream: counting a corrupt copy
        // as a holder could delete the last valid one elsewhere as a
        // duplicate.
        if (store->valid(id)) {
          holders.push_back({backend, store});
          estimated = std::max(estimated, store->stored_size(id));
        }
      } catch (...) {
      }
    }
    if (holders.empty()) continue;

    bool preferred_has = std::find_if(holders.begin(), holders.end(), [&](const Holder& holder) { return holder.backend == preferred; }) != holders.end();
    if (!preferred_has) {
      if (budget_bytes && result.bytes && result.bytes + estimated > budget_bytes) return result;

      std::optional<Bytes> data;
      for (const auto& holder : holders) {
        try {
          // Maintenance bypasses get(), so it is timed here to count toward
          // device pressure.
          DiskServiceTimer timer(&_service_monitor, 0, _services.time);
          data = holder.store->get(id);
          timer.note_bytes(data ? data->size() : 0);
          if (data) break;
        } catch (...) {
        }
      }

      std::shared_ptr<LocalStore> preferred_store;
      {
        Lock lock(preferred->mutex);
        if (preferred->online) preferred_store = preferred->store;
      }
      if (data && preferred_store) {
        try {
          bool stored = false;
          {
            DiskServiceTimer timer(&_service_monitor, data->size(), _services.time);
            stored = preferred_store->put(id, *data);
          }
          if (stored && preferred_store->valid(id)) {
            result.bytes += data->size();
            holders.push_back({preferred, preferred_store});
            preferred_has = true;
          }
        } catch (...) {
        }
      }
    }

    if (preferred_has) {
      for (const auto& holder : holders) {
        if (holder.backend == preferred) continue;
        try {
          (void)holder.store->remove(id);
        } catch (...) {
        }
      }
    }
    if (budget_bytes && result.bytes >= budget_bytes) return result;
  }
  return result;
}

StoragePool::MaintenanceResult StoragePool::scrub_step(uint64_t budget_bytes, size_t operation_budget, const std::function<bool()>& should_yield) {
  MaintenanceResult result;
  while (!operation_budget || result.objects < operation_budget) {
    if (should_yield && should_yield()) {
      result.yielded = true;
      return result;
    }
    bool pass_complete = false;
    const auto item = next_physical(_scrub_cursor, pass_complete);
    if (!item) {
      result.complete = pass_complete;
      return result;
    }
    ++result.objects;
    try {
      // A scrub is the heaviest reader; it counts toward device pressure.
      std::optional<Bytes> data;
      {
        DiskServiceTimer timer(&_service_monitor, 0, _services.time);
        data = item->store->get(item->id);
        timer.note_bytes(data ? data->size() : 0);
      }
      if (data) result.bytes += data->size();
    } catch (const ObjectReadError& error) {
      _services.logger.warn("unreadable local object " + to_string(item->id) + " on " + item->path.string() + " kept (read failed): " + error.what());
    } catch (const std::exception& error) {
      if (!key_checked(item->backend)) {
        _services.logger.warn("unreadable local object " + to_string(item->id) + " on " + item->path.string() + " kept (key unchecked): " + error.what());
        continue;
      }
      _services.logger.warn("removing corrupt local object " + to_string(item->id) + " on " + item->path.string() + ": " + error.what());
      try {
        (void)item->store->remove(item->id);
      } catch (...) {
      }
    }
    if (budget_bytes && result.bytes >= budget_bytes) return result;
  }
  return result;
}

StoragePool::MaintenanceResult StoragePool::gc_step(const contract::IdLookup& live, const std::vector<ObjectId>& protected_ids,
                                                    std::chrono::milliseconds orphan_grace, size_t operation_budget, const std::function<bool()>& should_yield,
                                                    const ClaimCheck& claimed, ledger::UnreferencedSince* sightings, uint64_t now_unix_ms,
                                                    std::chrono::milliseconds unreferenced_grace) {
  MaintenanceResult result;
  while (!operation_budget || result.objects < operation_budget) {
    if (should_yield && should_yield()) {
      result.yielded = true;
      return result;
    }
    bool pass_complete = false;
    const auto item = next_physical(_gc_cursor, pass_complete);
    if (!item) {
      result.complete = pass_complete;
      if (pass_complete && sightings) sightings->pass_complete();
      return result;
    }
    ++result.objects;
    const auto& id = item->id;
    if (live.contains(id) || std::binary_search(protected_ids.begin(), protected_ids.end(), id) || (claimed && claimed(id))) {
      if (sightings) sightings->forget(id);
      continue;
    }
    if (sightings && !sightings->matured(id, now_unix_ms, unreferenced_grace)) {
      result.deferred = true;
      continue;
    }

    try {
      // Checked again just before the irreversible delete, and the object
      // marked: a claim may land after this slice took its inventory, but not
      // between this check and the delete, which holds no stripe.
      const auto deletion = _claim_locks.deletion(id, [&] { return claimed && claimed(id); });
      if (deletion.claimed()) continue;
      // Age is the orphan grace for DATA no committed reference reached yet.
      // A put of an object already held refreshes it, so a new write cannot
      // race an ancient orphan with identical bytes.
      const auto bytes = item->store->stored_size(id);
      if (item->store->remove_if_older_than(id, orphan_grace)) {
        result.bytes += bytes;
        if (sightings) sightings->forget(id);
      } else {
        result.deferred = true;
      }
    } catch (const std::exception& error) {
      _services.logger.debug("garbage collection skipped local object " + to_string(id) + " on " + item->path.string() + ": " + error.what());
    }
  }
  return result;
}

uint64_t StoragePool::rebalance_once(uint64_t budget_bytes) {
  uint64_t total = 0;
  while (true) {
    const auto step = rebalance_step(budget_bytes ? budget_bytes - total : 0, 256);
    total += step.bytes;
    if (step.complete || (budget_bytes && total >= budget_bytes)) return total;
  }
}

LocalStore::VerifyResult StoragePool::verify_step(size_t directories) {
  LocalStore::VerifyResult total;
  total.complete = true;
  for (const auto& backend : snapshot()) {
    std::shared_ptr<LocalStore> store;
    {
      Lock lock(backend->mutex);
      if (backend->online) store = backend->store;
    }
    if (!store) continue;
    const auto step = store->verify_step(directories);
    total.directories += step.directories;
    total.recorded += step.recorded;
    total.lost += step.lost;
    total.complete = total.complete && step.complete;
  }
  return total;
}

size_t StoragePool::compact_packs(std::stop_token stop) {
  size_t visited = 0;
  for (const auto& backend : snapshot()) {
    if (stop.stop_requested()) break;
    std::shared_ptr<LocalStore> store;
    std::filesystem::path path;
    {
      Lock lock(backend->mutex);
      if (!backend->online || !backend->store) continue;
      store = backend->store;
      path = backend->options.path;
    }
    try {
      (void)store->compact_packs(stop);
      ++visited;
    } catch (const std::exception& error) {
      // A failed compaction must not make authoritative bytes disappear: the
      // backend stays online and it is retried later; reads use the old
      // durable representation unless a replacement was already installed.
      _services.logger.warn("DATA pack compaction deferred path=" + path.string() + " error=" + error.what());
    }
  }
  return visited;
}

uint64_t StoragePool::used() const {
  uint64_t total = 0;
  for (const auto& backend : snapshot()) {
    std::shared_ptr<LocalStore> store;
    {
      Lock lock(backend->mutex);
      if (backend->online) store = backend->store;
    }
    if (store) total += store->used();
  }
  return total;
}

uint64_t StoragePool::limit() const {
  uint64_t total = 0;
  for (const auto& backend : snapshot()) {
    uint64_t capacity = 0;
    bool include = false;
    {
      Lock lock(backend->mutex);
      include = backend->configured && backend->token_known;
      capacity = backend->options.limit;
    }
    if (include) {
      if (capacity > std::numeric_limits<uint64_t>::max() - total) return std::numeric_limits<uint64_t>::max();
      total += capacity;
    }
  }
  return total;
}

bool StoragePool::every_backend_online() const {
  for (const auto& backend : snapshot()) {
    Lock lock(backend->mutex);
    if (backend->configured && (!backend->online || !backend->store)) return false;
  }
  return true;
}

bool StoragePool::degraded() const {
  bool configured = false;
  for (const auto& backend : snapshot()) {
    Lock lock(backend->mutex);
    if (!backend->configured) continue;
    if (backend->online && backend->store) return false;
    configured = true;
  }
  return configured;
}

LocalStoreDiagnostics StoragePool::diagnostics() const {
  LocalStoreDiagnostics total;
  for (const auto& backend : snapshot()) {
    std::shared_ptr<LocalStore> store;
    {
      Lock lock(backend->mutex);
      if (backend->online) store = backend->store;
    }
    if (!store) continue;
    const auto current = store->diagnostics();
    total.loose_reaffirmation_fast_paths += current.loose_reaffirmation_fast_paths;
    total.loose_reaffirmation_full_validations += current.loose_reaffirmation_full_validations;
    total.presence_index_entries += current.presence_index_entries;
    total.pack_recovery_truncated_tails += current.pack_recovery_truncated_tails;
    total.pack_recovery_skipped_regions += current.pack_recovery_skipped_regions;
    total.pack_recovery_skipped_bytes += current.pack_recovery_skipped_bytes;
    total.own_syncs += current.own_syncs;
  }
  return total;
}

uint64_t StoragePool::physical_barriers() const {
  uint64_t total = 0;
  Lock lock(_domain_mutex);
  for (const auto& [device, domain] : _domains_by_device) total += domain->physical_barriers();
  return total;
}

void StoragePool::time_advanced() {
  std::vector<std::shared_ptr<DurabilityDomain>> domains;
  {
    Lock lock(_domain_mutex);
    for (const auto& [device, domain] : _domains_by_device) domains.push_back(domain);
  }
  for (const auto& domain : domains) domain->time_advanced();
}

}  // namespace dagda::store
