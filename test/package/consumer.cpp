//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Built against the installed package: the node's header, which reaches
// Dannan, Asio and OpenSSL, and the library's own version at run time.
#include <cstdio>
#include <cstring>

#include "dagda/node.h"
#include "dagda/version.h"

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const char* version = dagda::library_version();
  std::printf("consumer linked dagda %s on dannan %s\n", version, dagda::dannan_version_built_against());
  return std::strcmp(version, argv[1]) == 0 ? 0 : 1;
}
