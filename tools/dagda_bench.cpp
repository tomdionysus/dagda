//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// dagda-bench: figures to compare, recorded and never asserted. Not in the gate.
//
// The log prototype of the three-systems design (TODO/2026-11-13-three-systems-design.md,
// "The commit rate, measured first", 2): throwaway, so the log's rate is known before its
// format is fixed (the maintainer, 2026-11-13). Three processes on Dannan: two replicas, which
// group-commit what arrives to an append-only segment and sync before answering, and an
// appender outside the replica set, which sends each tick's entries to both at once and
// times every message from its arrival to both replicas' answers (k = 2): Lir's PUBACK
// path. Each message is an append, then an ack per edge a tick later, then a retire.
//
//   dagda-bench log-replica  --key FILE --state DIR --port N [--bootstrap HOST:PORT]
//                            [--files shared|per-log] [--deferred yes|no]
//   dagda-bench log-appender --key FILE --state DIR --port N --bootstrap HOST:PORT
//                            [--rate N] [--tick-ms N] [--edges N] [--bytes N] [--seconds N]
//                            [--logs N] [--group log|node] [--hold-ticks N]
//
// --hold-ticks keeps each message live that many ticks before its acks (and its retire a
// tick after), so the scan at the end finds a backlog, as a dead publisher leaves one.
//
// --logs spreads the messages by hash over that many logs (Lir's 256 shards). --group log
// sends one call per log per tick, --group node one per replica node per tick carrying
// every log's entries (the Lir session's proposal). --files per-log gives each log its
// own segment, synced one by one; shared puts every log in one segment, one sync a group.
// --deferred yes (built against Dannan 0.7, DAGDA_BENCH_DEFERRED) answers each append
// from the committer after its sync, holding no worker while it waits; no, the default,
// waits for the sync in the handler.
//
// A replica runs until SIGTERM or SIGINT and then prints its figures as one JSON line;
// the appender prints its own and stops after --seconds.

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dannan/node.h"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
namespace asio = boost::asio;

constexpr dannan::net::MessageType log_append{4000};
// The lease-change scan: a replica reads back every live entry of a third of the logs
// from its shared segment, as a new lease holder resumes what a dead node owed.
constexpr dannan::net::MessageType log_scan{4001};
constexpr uint64_t segment_bytes = 64ULL * 1024 * 1024;

std::atomic<bool> stopping{false};

struct Options {
  std::string mode;
  std::filesystem::path key;
  std::filesystem::path state;
  uint16_t port = 0;
  std::vector<dannan::net::Endpoint> bootstrap;
  uint32_t rate = 8000;
  uint32_t tick_ms = 10;
  uint32_t edges = 2;
  uint32_t bytes = 1024;
  uint32_t seconds = 60;
  uint32_t logs = 1;
  uint32_t hold_ticks = 1;
  std::string group = "node";
  std::string files = "shared";
  bool deferred = false;
};

[[noreturn]] void usage(const std::string& why) {
  std::cerr << "dagda-bench: " << why << "\n"
            << "usage: dagda-bench log-replica --key FILE --state DIR --port N [--bootstrap HOST:PORT]\n"
            << "       dagda-bench log-appender --key FILE --state DIR --port N --bootstrap HOST:PORT\n"
            << "                   [--rate N] [--tick-ms N] [--edges N] [--bytes N] [--seconds N]\n";
  std::exit(2);
}

Options parse(int argc, char** argv) {
  if (argc < 2) usage("a mode is required");
  Options out;
  out.mode = argv[1];
  if (out.mode != "log-replica" && out.mode != "log-appender") usage("unknown mode " + out.mode);
  for (int i = 2; i < argc; ++i) {
    const std::string option = argv[i];
    if (i + 1 >= argc) usage("expected a value after " + option);
    const std::string value = argv[++i];
    if (option == "--key") {
      out.key = value;
    } else if (option == "--state") {
      out.state = value;
    } else if (option == "--port") {
      out.port = static_cast<uint16_t>(std::stoi(value));
    } else if (option == "--bootstrap") {
      const auto colon = value.rfind(':');
      if (colon == std::string::npos) usage("expected HOST:PORT, got " + value);
      out.bootstrap.push_back({value.substr(0, colon), static_cast<uint16_t>(std::stoi(value.substr(colon + 1)))});
    } else if (option == "--rate") {
      out.rate = static_cast<uint32_t>(std::stoul(value));
    } else if (option == "--tick-ms") {
      out.tick_ms = static_cast<uint32_t>(std::stoul(value));
    } else if (option == "--edges") {
      out.edges = static_cast<uint32_t>(std::stoul(value));
    } else if (option == "--bytes") {
      out.bytes = static_cast<uint32_t>(std::stoul(value));
    } else if (option == "--seconds") {
      out.seconds = static_cast<uint32_t>(std::stoul(value));
    } else if (option == "--hold-ticks") {
      out.hold_ticks = std::max<uint32_t>(1, static_cast<uint32_t>(std::stoul(value)));
    } else if (option == "--logs") {
      out.logs = std::max<uint32_t>(1, static_cast<uint32_t>(std::stoul(value)));
    } else if (option == "--group") {
      if (value != "log" && value != "node") usage("--group is log or node");
      out.group = value;
    } else if (option == "--deferred") {
      out.deferred = value == "yes";
#if !defined(DAGDA_BENCH_DEFERRED)
      if (out.deferred) usage("--deferred needs a build against Dannan 0.7 (DAGDA_BENCH_DEFERRED)");
#endif
    } else if (option == "--files") {
      if (value != "shared" && value != "per-log") usage("--files is shared or per-log");
      out.files = value;
    } else {
      usage("unknown option " + option);
    }
  }
  if (out.key.empty() || out.state.empty() || out.port == 0) usage("--key, --state and --port are required");
  if (out.mode == "log-appender" && out.bootstrap.empty()) usage("the appender needs --bootstrap");
  return out;
}

// ---- The entry format: throwaway, as the prototype is. -------------------------------

enum class Kind : uint8_t { append = 0, ack = 1, retire = 2 };

struct Entry {
  Kind kind = Kind::append;
  uint32_t log = 0;
  uint64_t origin = 0;
  uint64_t seq = 0;
  uint8_t stage = 0;
  std::string stream;
  std::vector<uint8_t> bytes;
};

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void put_u64(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

std::vector<uint8_t> encode(const std::vector<Entry>& entries) {
  std::vector<uint8_t> out;
  put_u32(out, static_cast<uint32_t>(entries.size()));
  for (const auto& e : entries) {
    out.push_back(static_cast<uint8_t>(e.kind));
    put_u32(out, e.log);
    put_u64(out, e.origin);
    put_u64(out, e.seq);
    out.push_back(e.stage);
    put_u32(out, static_cast<uint32_t>(e.stream.size()));
    out.insert(out.end(), e.stream.begin(), e.stream.end());
    put_u32(out, static_cast<uint32_t>(e.bytes.size()));
    out.insert(out.end(), e.bytes.begin(), e.bytes.end());
  }
  return out;
}

class Reader {
 public:
  explicit Reader(const std::vector<uint8_t>& data) : _data(data) {}
  uint8_t u8() {
    need(1);
    return _data[_at++];
  }
  uint32_t u32() {
    need(4);
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(_data[_at++]) << (8 * i);
    return v;
  }
  uint64_t u64() {
    need(8);
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(_data[_at++]) << (8 * i);
    return v;
  }
  size_t skip(size_t n) {
    need(n);
    const auto at = _at;
    _at += n;
    return at;
  }

 private:
  void need(size_t n) const {
    if (_data.size() - _at < n) throw std::runtime_error("short entry");
  }
  const std::vector<uint8_t>& _data;
  size_t _at = 0;
};

// ---- Figures. ----------------------------------------------------------------------

struct Samples {
  std::vector<double> values;
  void add(double v) { values.push_back(v); }
  double at(double q) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const auto index = static_cast<size_t>(q * static_cast<double>(values.size() - 1));
    return values[index];
  }
  std::string json(const std::string& unit) {
    char line[256];
    std::snprintf(line, sizeof(line), "{\"n\":%zu,\"p50_%s\":%.3f,\"p90_%s\":%.3f,\"p99_%s\":%.3f,\"max_%s\":%.3f}", values.size(), unit.c_str(),
                  at(0.50), unit.c_str(), at(0.90), unit.c_str(), at(0.99), unit.c_str(), at(1.0));
    return line;
  }
};

// The process's peak resident memory (getrusage gives bytes on macOS, KiB on Linux) and
// its open descriptors.
uint64_t max_rss_kib() {
  rusage usage{};
  ::getrusage(RUSAGE_SELF, &usage);
#if defined(__APPLE__)
  return static_cast<uint64_t>(usage.ru_maxrss) / 1024;
#else
  return static_cast<uint64_t>(usage.ru_maxrss);
#endif
}
size_t open_files() {
  size_t n = 0;
  std::error_code ec;
  for (auto it = std::filesystem::directory_iterator("/dev/fd", ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) ++n;
  return n;
}

double ms_since(Clock::time_point start, Clock::time_point end = Clock::now()) { return std::chrono::duration<double, std::milli>(end - start).count(); }

// ---- The node. ---------------------------------------------------------------------

struct Running {
  asio::io_context io;
  std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work;
  std::vector<std::thread> threads;
  std::unique_ptr<dannan::Node> node;

  explicit Running(const Options& options, const std::string& name) {
    work.emplace(asio::make_work_guard(io));
    for (int i = 0; i < 4; ++i) threads.emplace_back([this] { io.run(); });
    std::filesystem::create_directories(options.state / "dannan");
    dannan::NodeConfig config;
    config.state_path = options.state / "dannan";
    config.key = dannan::identity::load_cluster_key(options.key);
    config.listen_host = "127.0.0.1";
    config.advertise_host = "127.0.0.1";
    config.port = options.port;
    config.bootstrap = options.bootstrap;
    config.inbound = dannan::reach::Inbound::yes;
    config.node_name = name;
    config.heartbeat = 1000ms;
    config.dead_after = 6000ms;
    config.version = "dagda-bench";
    node = std::make_unique<dannan::Node>(io, config);
  }
  // Stops the node and joins the io threads, so no handler or completion runs after;
  // called before a mode's figures and locals go.
  void shutdown() {
    if (node) {
      // Stopped and destroyed while the io threads still run: a stop after them waits for ever.
      node->stop();
      node.reset();
    }
    work.reset();
    io.stop();
    for (auto& thread : threads)
      if (thread.joinable()) thread.join();
  }
  ~Running() { shutdown(); }
};

// ---- The replica: a group commit to an append-only segment. ------------------------

class Replica {
 public:
  Replica(std::filesystem::path dir, bool per_log) : _dir(std::move(dir)), _per_log(per_log) {
    std::filesystem::create_directories(_dir);
    open_segment();
    _committer = std::thread([this] { commit_loop(); });
  }
  ~Replica() {
    {
      std::lock_guard lock(_mutex);
      _closing = true;
    }
    _wake.notify_all();
    _committer.join();
    if (_fd >= 0) ::close(_fd);
    for (auto& [log, fd] : _log_fds) ::close(fd);
  }

  // Waits for the group holding these entries to be synced: the handler's answer.
  void append(std::vector<uint8_t> payload) {
    auto done = std::make_shared<std::promise<void>>();
    auto synced = done->get_future();
    {
      std::lock_guard lock(_mutex);
      Pending pending;
      pending.payload = std::move(payload);
      pending.done = done;
      _pending.push_back(std::move(pending));
    }
    _wake.notify_one();
    synced.get();
  }

#if defined(DAGDA_BENCH_DEFERRED)
  // Queues the entries and returns: the committer answers after the group's sync.
  void append_deferred(std::vector<uint8_t> payload, dannan::net::Answer answer) {
    {
      std::lock_guard lock(_mutex);
      _pending.push_back({std::move(payload), nullptr, std::move(answer)});
    }
    _wake.notify_one();
  }
#endif

  // Every live entry of the logs with log % 3 == 0, read back from the shared segment
  // by its offset: the entries, their bytes and the time, as JSON.
  std::string scan() {
    std::vector<Live> wanted;
    {
      std::lock_guard index_lock(_index_mutex);
      for (const auto& [dot, live] : _live)
        if (live.log % 3 == 0) wanted.push_back(live);
    }
    const auto start = Clock::now();
    std::map<uint32_t, int> fds;
    uint64_t bytes = 0;
    std::vector<uint8_t> buffer;
    for (const auto& live : wanted) {
      auto found = fds.find(live.segment);
      if (found == fds.end()) {
        const int fd = ::open((_dir / ("segment-" + std::to_string(live.segment))).c_str(), O_RDONLY);
        if (fd < 0) continue;
        found = fds.emplace(live.segment, fd).first;
      }
      buffer.resize(live.length);
      if (::pread(found->second, buffer.data(), live.length, static_cast<off_t>(live.offset)) == static_cast<ssize_t>(live.length)) bytes += live.length;
    }
    for (auto& [segment, fd] : fds) ::close(fd);
    char out[200];
    std::snprintf(out, sizeof(out), "{\"entries\":%zu,\"bytes\":%llu,\"ms\":%.3f}", wanted.size(), static_cast<unsigned long long>(bytes), ms_since(start));
    return out;
  }

  std::string figures() {
    std::lock_guard lock(_figures_mutex);
    return "{\"event\":\"replica\",\"groups\":" + std::to_string(_groups) + ",\"calls\":" + std::to_string(_calls) +
           ",\"entries\":" + std::to_string(_entries) + ",\"bytes\":" + std::to_string(_bytes) + ",\"live\":" + std::to_string(_live.size()) +
           ",\"segments_deleted\":" + std::to_string(_deleted) + ",\"sync\":" + _sync_ms.json("ms") + ",\"calls_per_group\":" + _group_calls.json("calls") +
           ",\"logs_per_group\":" + _logs_per_group.json("logs") + ",\"max_rss_kib\":" + std::to_string(max_rss_kib()) +
           ",\"open_files\":" + std::to_string(open_files()) + "}";
  }

 private:
  struct Pending {
    std::vector<uint8_t> payload;
    std::shared_ptr<std::promise<void>> done;
#if defined(DAGDA_BENCH_DEFERRED)
    dannan::net::Answer answer;
#endif
  };
  struct Live {
    uint32_t segment = 0;
    uint64_t offset = 0;
    uint32_t length = 0;
    uint32_t log = 0;
    uint32_t acks = 0;
  };

  static void write_all(int fd, const std::vector<uint8_t>& out) {
    size_t done = 0;
    while (done < out.size()) {
      const auto n = ::write(fd, out.data() + done, out.size() - done);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) throw std::runtime_error("segment write failed");
      done += static_cast<size_t>(n);
    }
  }
  static void sync(int fd) {
    int rc;
    do {
      rc = ::fsync(fd);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) throw std::runtime_error("segment sync failed");
  }

  // Each entry to its own log's segment, then every touched segment synced in turn: the
  // time of all the syncs.
  double write_per_log(const std::vector<Pending>& group) {
    std::map<uint32_t, std::vector<uint8_t>> by_log;
    for (const auto& p : group) {
      Reader in(p.payload);
      const auto count = in.u32();
      for (uint32_t i = 0; i < count; ++i) {
        const auto begin = in.skip(0);
        (void)in.u8();
        const auto log = in.u32();
        (void)in.u64();
        (void)in.u64();
        (void)in.u8();
        in.skip(in.u32());
        in.skip(in.u32());
        const auto after = in.skip(0);
        auto& out = by_log[log];
        out.insert(out.end(), p.payload.begin() + static_cast<std::ptrdiff_t>(begin), p.payload.begin() + static_cast<std::ptrdiff_t>(after));
      }
    }
    for (const auto& [log, bytes] : by_log) {
      auto found = _log_fds.find(log);
      if (found == _log_fds.end()) {
        const auto path = _dir / ("log-" + std::to_string(log));
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0600);
        if (fd < 0) throw std::runtime_error("cannot open " + path.string());
        found = _log_fds.emplace(log, fd).first;
      }
      write_all(found->second, bytes);
    }
    const auto before = Clock::now();
    for (const auto& [log, bytes] : by_log) sync(_log_fds.at(log));
    std::lock_guard lock(_figures_mutex);
    _logs_per_group.add(static_cast<double>(by_log.size()));
    return ms_since(before);
  }

  void open_segment() {
    ++_segment;
    const auto path = _dir / ("segment-" + std::to_string(_segment));
    _fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0600);
    if (_fd < 0) throw std::runtime_error("cannot open " + path.string());
    _written = 0;
  }

  void commit_loop() {
    for (;;) {
      std::vector<Pending> group;
      {
        std::unique_lock lock(_mutex);
        _wake.wait(lock, [&] { return _closing || !_pending.empty(); });
        if (_pending.empty() && _closing) return;
        group.swap(_pending);
      }
      std::vector<uint8_t> out;
      for (const auto& p : group) out.insert(out.end(), p.payload.begin(), p.payload.end());
      double sync_ms = 0;
      if (_per_log) {
        sync_ms = write_per_log(group);
      } else {
        write_all(_fd, out);
        const auto before = Clock::now();
        sync(_fd);
        sync_ms = ms_since(before);
      }
      const auto base = _written;
      _written += out.size();
      index(group, base);
      if (_written >= segment_bytes) open_segment();
      for (auto& p : group) {
#if defined(DAGDA_BENCH_DEFERRED)
        if (p.answer) {
          p.answer(dannan::net::Message{dannan::net::message::ok, {}, {}});
          continue;
        }
#endif
        p.done->set_value();
      }
      std::lock_guard lock(_figures_mutex);
      ++_groups;
      _calls += group.size();
      _bytes += out.size();
      _sync_ms.add(sync_ms);
      _group_calls.add(static_cast<double>(group.size()));
    }
  }

  // The live index the design keeps: dot to segment and offset, acks counted, a
  // retire erasing the entry; a segment with nothing live left is deleted whole.
  void index(const std::vector<Pending>& group, uint64_t base) {
    std::lock_guard index_lock(_index_mutex);
    uint64_t offset = base;
    for (const auto& p : group) {
      Reader in(p.payload);
      const auto count = in.u32();
      for (uint32_t i = 0; i < count; ++i) {
        const auto begin = in.skip(0);
        const auto kind = static_cast<Kind>(in.u8());
        const auto log = in.u32();
        const auto dot = std::make_pair(in.u64(), in.u64());
        (void)in.u8();
        in.skip(in.u32());
        in.skip(in.u32());
        const auto end = in.skip(0);
        if (kind == Kind::append) {
          _live[dot] = Live{_segment, offset + begin, static_cast<uint32_t>(end - begin), log, 0};
          ++_segment_live[_segment];
        } else if (auto found = _live.find(dot); found != _live.end()) {
          if (kind == Kind::ack) {
            ++found->second.acks;
          } else {
            const auto segment = found->second.segment;
            _live.erase(found);
            if (--_segment_live[segment] == 0 && segment != _segment) {
              std::filesystem::remove(_dir / ("segment-" + std::to_string(segment)));
              _segment_live.erase(segment);
              ++_deleted;
            }
          }
        }
        ++_entries;
      }
      offset += p.payload.size();
    }
  }

  const std::filesystem::path _dir;
  const bool _per_log;
  std::map<uint32_t, int> _log_fds;
  std::mutex _mutex;
  std::condition_variable _wake;
  std::vector<Pending> _pending;
  bool _closing = false;
  std::thread _committer;
  int _fd = -1;
  uint32_t _segment = 0;
  uint64_t _written = 0;
  std::mutex _index_mutex;
  std::map<std::pair<uint64_t, uint64_t>, Live> _live;
  std::map<uint32_t, uint64_t> _segment_live;

  std::mutex _figures_mutex;
  uint64_t _groups = 0, _calls = 0, _entries = 0, _bytes = 0, _deleted = 0;
  Samples _sync_ms;
  Samples _group_calls;
  Samples _logs_per_group;
};

int run_replica(const Options& options) {
  Running running(options, "replica-" + std::to_string(options.port));
  Replica replica(options.state / "log", options.files == "per-log");
  auto& routes = running.node->context().routes();
  routes.add(log_append, {"bench_log_append", dannan::net::Lanes::control, dannan::net::Frames::any, false});
  routes.add(log_scan, {"bench_log_scan", dannan::net::Lanes::control, dannan::net::Frames::any, false},
             [&](const dannan::membership::NodeRecord&, dannan::sched::FrameType, const dannan::net::Message&) {
               const auto result = replica.scan();
               return dannan::net::Handled::processed({dannan::net::message::ok, dannan::net::Bytes(result.begin(), result.end()), {}});
             });
  if (options.deferred) {
#if defined(DAGDA_BENCH_DEFERRED)
    routes.bind_deferred(log_append, [&](const dannan::membership::NodeRecord&, dannan::sched::FrameType, const dannan::net::Message& request,
                                         dannan::net::Answer& answer) {
      replica.append_deferred(request.payload, std::move(answer));
      return dannan::net::Handled::deferred();
    });
#endif
  } else {
    routes.bind(log_append, [&](const dannan::membership::NodeRecord&, dannan::sched::FrameType, const dannan::net::Message& request) {
      replica.append(request.payload);
      return dannan::net::Handled::processed();
    });
  }
  running.node->start();
  std::printf("{\"event\":\"listening\",\"port\":%u}\n", static_cast<unsigned>(running.node->port()));
  std::fflush(stdout);
  while (!stopping) std::this_thread::sleep_for(50ms);
  running.shutdown();
  std::printf("%s\n", replica.figures().c_str());
  std::fflush(stdout);
  return 0;
}

// ---- The appender: Lir's PUBACK path. ----------------------------------------------

int run_appender(const Options& options) {
  Running running(options, "appender-" + std::to_string(options.port));
  running.node->start();
  auto& context = running.node->context();
  const auto self = context.identity().id();

  // The two replicas: every active node but this one.
  std::vector<dannan::membership::NodeRecord> replicas;
  const auto found_by = Clock::now() + 30s;
  while (Clock::now() < found_by && !stopping) {
    replicas.clear();
    for (const auto& node : context.nodes().active())
      if (node.id != self) replicas.push_back(node);
    if (replicas.size() >= 2) break;
    std::this_thread::sleep_for(100ms);
  }
  if (replicas.size() < 2) {
    std::cerr << "dagda-bench: found " << replicas.size() << " replicas, need 2\n";
    return 1;
  }
  replicas.resize(2);

  uint64_t origin = 0;
  std::memcpy(&origin, self.bytes.data(), sizeof(origin));
  std::mutex figures_mutex;
  Samples puback_ms;
  Samples call_ms;
  uint64_t failures = 0;
  std::map<std::string, uint64_t> reasons;
  std::atomic<uint64_t> in_flight{0};
  uint64_t entries_sent = 0;

  struct Message {
    uint64_t seq;
    Clock::time_point arrived;
  };
  const auto tick = std::chrono::milliseconds(std::max<uint32_t>(options.tick_ms, 1));
  const bool unbatched = options.tick_ms == 0;
  const double per_ms = options.rate / 1000.0;
  std::vector<uint8_t> body(options.bytes, 0x5a);
  uint64_t seq = 0;
  const auto log_of = [&](uint64_t s) { return static_cast<uint32_t>((s * 0x9E3779B97F4A7C15ULL >> 32) % options.logs); };
  std::deque<std::vector<uint64_t>> to_ack;     // the previous tick's messages, acked this tick
  std::deque<std::vector<uint64_t>> to_retire;  // the tick before's, retired this tick

  const auto send = [&](std::vector<Entry> entries, std::vector<Message> messages) {
    entries_sent += entries.size();
    const auto payload = encode(entries);
    struct Group {
      std::atomic<int> left{2};
      std::atomic<bool> failed{false};
      Clock::time_point sent = Clock::now();
      std::vector<Message> messages;
    };
    auto group = std::make_shared<Group>();
    group->messages = std::move(messages);
    in_flight += 1;
    for (const auto& replica : replicas) {
      context.routes().call(replica, log_append, payload, dannan::sched::FrameType::control, 5000ms, running.io.get_executor(),
                            [&, group](dannan::net::CallOutcome outcome) {
                              if (!outcome.reply || outcome.reply->type != dannan::net::message::ok) {
                                group->failed = true;
                                std::string why = outcome.reply ? "reply type " + std::to_string(static_cast<unsigned>(outcome.reply->type)) + " " +
                                                                      std::string(outcome.reply->payload.begin(), outcome.reply->payload.end()).substr(0, 60)
                                                                : outcome.failure;
                                std::lock_guard lock(figures_mutex);
                                ++reasons[why];
                              }
                              if (--group->left != 0) return;
                              const auto now = Clock::now();
                              std::lock_guard lock(figures_mutex);
                              --in_flight;
                              if (group->failed) {
                                failures += group->messages.size();
                                return;
                              }
                              call_ms.add(ms_since(group->sent, now));
                              for (const auto& m : group->messages) puback_ms.add(ms_since(m.arrived, now));
                            });
    }
  };

  const auto start = Clock::now();
  const auto until = start + std::chrono::seconds(options.seconds);
  auto next = start;
  double owed = 0;
  auto last = start;
  while (!stopping && Clock::now() < until) {
    const auto now = Clock::now();
    owed += ms_since(last, now) * per_ms;
    last = now;
    const auto count = static_cast<uint64_t>(owed);
    owed -= static_cast<double>(count);
    // Arrivals are spread across the tick that gathers them: each message waits for
    // the tick's end, as Lir's batch does.
    const double spacing = count > 0 ? std::chrono::duration<double, std::milli>(unbatched ? 1ms : tick).count() / static_cast<double>(count) : 0;
    std::vector<Entry> entries;
    std::vector<Message> messages;
    std::vector<uint64_t> this_tick;
    for (uint64_t i = 0; i < count; ++i) {
      const auto s = ++seq;
      this_tick.push_back(s);
      const auto arrived = now - std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(spacing * static_cast<double>(count - i)));
      if (unbatched) {
        send({Entry{Kind::append, log_of(s), origin, s, 0, "topic/" + std::to_string(s % 64), body}}, {Message{s, Clock::now()}});
      } else {
        entries.push_back(Entry{Kind::append, log_of(s), origin, s, 0, "topic/" + std::to_string(s % 64), body});
        messages.push_back(Message{s, arrived});
      }
    }
    if (to_ack.size() >= options.hold_ticks) {
      for (const auto s : to_ack.front())
        for (uint32_t edge = 0; edge < options.edges; ++edge) entries.push_back(Entry{Kind::ack, log_of(s), origin, s, 1, "", std::vector<uint8_t>(16, 0x01)});
      to_ack.pop_front();
    }
    if (to_retire.size() > options.hold_ticks) {
      for (const auto s : to_retire.front()) entries.push_back(Entry{Kind::retire, log_of(s), origin, s, 2, "", {}});
      to_retire.pop_front();
    }
    to_ack.push_back(this_tick);
    to_retire.push_back(std::move(this_tick));
    if (entries.empty()) {
    } else if (options.group == "node" || options.logs == 1) {
      send(std::move(entries), std::move(messages));
    } else {
      // One call per log with entries this tick, each carrying that log's messages.
      std::map<uint32_t, std::pair<std::vector<Entry>, std::vector<Message>>> by_log;
      for (auto& e : entries) by_log[e.log].first.push_back(std::move(e));
      for (auto& m : messages) by_log[log_of(m.seq)].second.push_back(m);
      for (auto& [log, part] : by_log) send(std::move(part.first), std::move(part.second));
    }
    next += unbatched ? 1ms : tick;
    std::this_thread::sleep_until(next);
  }
  const auto drained_by = Clock::now() + 10s;
  while (in_flight > 0 && Clock::now() < drained_by) std::this_thread::sleep_for(10ms);
  const auto elapsed_s = ms_since(start) / 1000.0;
  // The lease-change scan, asked of the first replica with what is live as the run ends.
  std::string scan = "null";
  {
    const auto outcome = context.routes().call_and_wait(replicas.front(), log_scan, {}, dannan::sched::FrameType::control, 30000ms);
    if (outcome.reply && outcome.reply->type == dannan::net::message::ok) scan.assign(outcome.reply->payload.begin(), outcome.reply->payload.end());
  }
  running.shutdown();
  std::lock_guard lock(figures_mutex);
  std::printf(
      "{\"event\":\"appender\",\"logs\":%u,\"group\":\"%s\",\"rate\":%u,\"tick_ms\":%u,\"edges\":%u,\"bytes\":%u,\"seconds\":%.1f,\"messages\":%llu,\"entries_per_s\":%.0f,"
      "\"failed\":%llu,\"puback\":%s,\"call\":%s,\"scan\":%s,\"reasons\":%s}\n",
      options.logs, options.group.c_str(), options.rate, options.tick_ms, options.edges, options.bytes, elapsed_s, static_cast<unsigned long long>(seq),
      static_cast<double>(entries_sent) / elapsed_s, static_cast<unsigned long long>(failures), puback_ms.json("ms").c_str(), call_ms.json("ms").c_str(), scan.c_str(), [&] {
        std::string out = "{";
        for (const auto& [why, n] : reasons) {
          std::string clean;
          for (const char c : why) clean += (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20) ? '_' : c;
          out += (out.size() > 1 ? "," : "") + std::string("\"") + clean + "\":" + std::to_string(n);
        }
        return out + "}";
      }().c_str());
  std::fflush(stdout);
  return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  const auto options = parse(argc, argv);
  signal(SIGTERM, [](int) { stopping = true; });
  signal(SIGINT, [](int) { stopping = true; });
  try {
    return options.mode == "log-replica" ? run_replica(options) : run_appender(options);
  } catch (const std::exception& error) {
    std::cerr << "dagda-bench: " << error.what() << "\n";
    return 1;
  }
}
