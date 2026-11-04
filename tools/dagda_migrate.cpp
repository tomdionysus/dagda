//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// dagda-migrate: migrates a node's layout v18 state and disks (Macha's) to
// layout v19 in place (dagda/migrate/migrate.h), with the node stopped. It
// is the only way a v18 disk becomes v19: the store refuses one at open.
//
//   dagda-migrate --state DIR --node-state DIR --cluster-key FILE KEY
//                 [--backend DIR]... [--control DIR] [--cache DIR] [--dry-run]
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
// takes it. --dry-run classifies and reports without writing.
//
// Exit 0 when the state is v19 (migrated now, or already); 1 when refused
// or failed, the reason on stderr; 2 for a usage error.

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "dagda/migrate/migrate.h"
#include "dagda/version.h"
#include "dannan/identity/cluster_key.h"
#include "dannan/identity/key_custody.h"

namespace {

struct Arguments {
  std::map<std::string, std::string> options;
  std::vector<std::string> backends;
  bool dry_run = false;
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
    "                     [--backend DIR]... [--control DIR] [--cache DIR] [--dry-run]\n"
    "       dagda-migrate --version\n";

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
    const std::filesystem::path state = need(arguments, "state");
    const std::filesystem::path node_state = need(arguments, "node-state");
    const auto cluster = dannan::identity::load_cluster_key(need(arguments, "cluster-key"));
    const auto custody = dannan::identity::KeyCustody::open(node_state, key_source(arguments));
    dagda::migrate::MigrationOptions options;
    if (arguments.options.contains("control")) options.control = arguments.options.at("control");
    if (arguments.options.contains("cache")) options.cache = arguments.options.at("cache");
    options.dry_run = arguments.dry_run;
    options.say = [](std::string_view line) { std::cout << line << (line.ends_with('\n') ? "" : "\n") << std::flush; };
    std::vector<std::filesystem::path> backends(arguments.backends.begin(), arguments.backends.end());
    const auto report = dagda::migrate::migrate_state(state, backends, cluster, custody, options);
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
