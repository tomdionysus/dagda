//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// dagda-migrate: migrates a node's layout v18 state and disks (Macha's) to
// layout v19 in place (dagda/migrate/migrate.h), with the node stopped. It
// is the only way a v18 disk becomes v19: the store refuses one at open.
//
//   dagda-migrate --state DIR --node-state DIR --cluster-key FILE KEY
//                 [--backend DIR]... [--control DIR] [--cache DIR] [--dry-run] [--reverse]
//   dagda-migrate --version
//
// --state is Dagda's state directory (the storage-layout marker, the
// ledgers, the control store); --node-state is Dannan's (where the node's
// key custody lives); --cluster-key the cluster key file; --backend each
// DATA backend's path as the node's configuration names it, repeated;
// --control the control store's path when it is not <state>/metadata-objects;
// --cache the persistent cache's path, if the node has one. KEY opens the
// node's custody: --passphrase-env VAR (the passphrase in that environment
// variable) or --credential NAME [--credentials-dir DIR], as dannan-node
// takes it. --dry-run classifies and reports without writing. --heads moves
// Macha's metadata (<state>/metadata/) and the cache seed to the heads' layout
// too, the port's later stage; a state already v19 runs that pass alone; --space
// names the head space, macha/namespace unless given. --reverse goes back from
// v19 to v18, the metadata included, so the node can return to Macha 0.93.x.
//
// Exit 0 when the state is v19 (migrated now, or already); 1 when refused
// or failed, the reason on stderr; 2 for a usage error.
//
// The proof that the data survived (dagda/migrate/manifest.h, docs/migration.md):
//
//   dagda-migrate --state DIR --node-state DIR --cluster-key FILE KEY [--backend DIR]...
//                 [--control DIR] [--cache DIR] --write-manifest FILE [--resume]
//   dagda-migrate --compare-before FILE --compare-after FILE [--expect-before 18] [--expect-after 19]
//
// --write-manifest reads the whole state and writes nothing under it, with the
// node stopped: once before the migration and once after. Exit 0 when the
// manifest is whole, which says nothing of what is in it. --compare-before reports
// every difference by name. Exit 0 only when nothing differs and everything was
// checked; 1 for any difference, anything NOT CHECKED, or a manifest that is not
// whole.

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "dagda/migrate/manifest.h"
#include "dagda/migrate/migrate.h"
#include "dagda/version.h"
#include "dannan/identity/cluster_key.h"
#include "dannan/identity/key_custody.h"
#include "dannan/identity/node_identity.h"

namespace {

struct Arguments {
  std::map<std::string, std::string> options;
  std::vector<std::string> backends;
  bool dry_run = false;
  bool reverse = false;
  bool resume = false;
  bool heads = false;
  bool version = false;
};

Arguments parse(int argc, char** argv) {
  Arguments out;
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    if (option == "--version") {
      out.version = true;
      continue;
    }
    if (option == "--dry-run") {
      out.dry_run = true;
      continue;
    }
    if (option == "--reverse") {
      out.reverse = true;
      continue;
    }
    if (option == "--resume") {
      out.resume = true;
      continue;
    }
    if (option == "--heads") {
      out.heads = true;
      continue;
    }
    if (option.rfind("--", 0) != 0 || i + 1 >= argc) throw std::invalid_argument("expected --option value, at " + option);
    const auto value = std::string(argv[++i]);
    if (option == "--backend")
      out.backends.push_back(value);
    else
      out.options[option.substr(2)] = value;
  }
  return out;
}

std::string need(const Arguments& arguments, const std::string& name) {
  const auto found = arguments.options.find(name);
  if (found == arguments.options.end()) throw std::invalid_argument("--" + name + " is required");
  return found->second;
}

dannan::identity::KeySource key_source(const Arguments& arguments) {
  if (const auto env = arguments.options.find("passphrase-env"); env != arguments.options.end()) {
    const char* value = std::getenv(env->second.c_str());
    if (!value || !*value) throw std::invalid_argument("the passphrase variable " + env->second + " is empty");
    const std::string passphrase(value);
    return dannan::identity::KeySource::typed_passphrase(
        [passphrase] { return dannan::identity::Secret(std::span(reinterpret_cast<const uint8_t*>(passphrase.data()), passphrase.size())); });
  }
  if (const auto credential = arguments.options.find("credential"); credential != arguments.options.end()) {
    const auto directory =
        arguments.options.contains("credentials-dir") ? std::filesystem::path(arguments.options.at("credentials-dir")) : std::filesystem::path();
    return dannan::identity::KeySource::systemd_credential(credential->second, directory);
  }
  throw std::invalid_argument("the custody key's source: --passphrase-env VAR or --credential NAME");
}

constexpr const char* usage =
    "usage: dagda-migrate --state DIR --node-state DIR --cluster-key FILE (--passphrase-env VAR | --credential NAME [--credentials-dir DIR])\n"
    "                     [--backend DIR]... [--control DIR] [--cache DIR] [--heads] [--space NAME] [--dry-run] [--reverse]\n"
    "       dagda-migrate --state DIR --node-state DIR --cluster-key FILE KEY [--backend DIR]... [--control DIR] [--cache DIR]\n"
    "                     [--space NAME] --write-manifest FILE [--resume]\n"
    "       dagda-migrate --compare-before FILE --compare-after FILE [--expect-before 18] [--expect-after 19]\n"
    "       dagda-migrate --version\n";

void say_line(std::string_view line) { std::cout << line << (line.ends_with('\n') ? "" : "\n") << std::flush; }

uint32_t layout_option(const Arguments& arguments, const std::string& name, uint32_t otherwise) {
  const auto found = arguments.options.find(name);
  if (found == arguments.options.end()) return otherwise;
  if (found->second != "18" && found->second != "19") throw std::invalid_argument("--" + name + " is 18 or 19");
  return found->second == "18" ? 18 : 19;
}

// Exit 0 only for a comparison that passed; the last line says which it was.
int compare(const Arguments& arguments) {
  dagda::migrate::ComparisonOptions options;
  options.before_layout = layout_option(arguments, "expect-before", 18);
  options.after_layout = layout_option(arguments, "expect-after", 19);
  options.say = say_line;
  try {
    const auto report = dagda::migrate::compare_manifests(need(arguments, "compare-before"), need(arguments, "compare-after"), options);
    if (report.passed()) {
      std::cout << "RESULT: PASS\n";
      return 0;
    }
    std::cout << "RESULT: FAIL: " << report.difference_count << " differences, " << report.not_checked_count << " not checked\n";
    return 1;
  } catch (const dagda::migrate::ManifestDamaged& error) {
    std::cout << "RESULT: FAIL: " << error.what() << "\n";
    return 1;
  }
}

}  // namespace

int main(int argc, char** argv) {
  Arguments arguments;
  try {
    arguments = parse(argc, argv);
    if (arguments.version) {
      std::printf("%s (dannan %s)\n", dagda::library_version(), dagda::dannan_version_built_against());
      return 0;
    }
    if (argc < 2) throw std::invalid_argument("no arguments");
  } catch (const std::invalid_argument& error) {
    std::cerr << "dagda-migrate: " << error.what() << "\n" << usage;
    return 2;
  }
  try {
    if (arguments.options.contains("compare-before") || arguments.options.contains("compare-after")) return compare(arguments);
    const std::filesystem::path state = need(arguments, "state");
    const std::filesystem::path node_state = need(arguments, "node-state");
    const auto cluster = dannan::identity::load_cluster_key(need(arguments, "cluster-key"));
    const auto custody = dannan::identity::KeyCustody::open(node_state, key_source(arguments));
    dagda::migrate::MigrationOptions options;
    if (arguments.options.contains("control")) options.control = arguments.options.at("control");
    if (arguments.options.contains("cache")) options.cache = arguments.options.at("cache");
    options.dry_run = arguments.dry_run;
    options.heads = arguments.heads;
    if (arguments.options.contains("space")) options.space_name = arguments.options.at("space");
    // Read, never made: load_or_create would mint an id no disk carries.
    if (!std::filesystem::exists(node_state / "node-id"))
      throw std::runtime_error("no node-id in " + node_state.string() + ": --node-state is the directory holding the id the node's disks carry");
    options.node = dannan::identity::load_or_create_node_id(node_state);
    if (arguments.options.contains("write-manifest")) {
      dagda::migrate::ManifestOptions pass;
      pass.backends.assign(arguments.backends.begin(), arguments.backends.end());
      pass.control = options.control;
      pass.cache = options.cache;
      pass.node = options.node;
      pass.node_state = node_state;
      pass.resume = arguments.resume;
      pass.metadata = dagda::migrate::metadata_manifest_reader(options.space_name);
      pass.say = say_line;
      // A test's way to stop a pass where a kill would find it: after this many
      // durable checkpoints the tool waits, saying so, for the signal that ends it.
      if (const char* hold = std::getenv("DAGDA_MIGRATE_TEST_HOLD_AFTER_CHECKPOINTS")) {
        const auto after = std::stoull(hold);
        pass.after_checkpoint = [after, seen = uint64_t{0}]() mutable {
          if (++seen != after) return;
          std::cout << "holding after checkpoint " << seen << std::endl;
          for (;;) ::pause();
        };
      }
      const auto summary = dagda::migrate::write_manifest(arguments.options.at("write-manifest"), state, cluster, custody, pass);
      std::cout << summary.summary();
      return 0;
    }
    options.say = say_line;
    std::vector<std::filesystem::path> backends(arguments.backends.begin(), arguments.backends.end());
    const auto report = arguments.reverse ? dagda::migrate::migrate_state_back(state, backends, cluster, custody, options)
                                          : dagda::migrate::migrate_state(state, backends, cluster, custody, options);
    (void)report;
    return 0;
  } catch (const std::invalid_argument& error) {
    std::cerr << "dagda-migrate: " << error.what() << "\n" << usage;
    return 2;
  } catch (const std::exception& error) {
    std::cerr << "dagda-migrate: " << error.what() << "\n";
    return 1;
  }
}
