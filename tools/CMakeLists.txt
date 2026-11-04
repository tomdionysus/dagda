#
# Dagda - The Store
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# dagda-migrate: migrates a node's layout v18 state and disks (Macha's) to
# layout v19 in place, with the node stopped. --version prints the
# repository version, as the tag and the CMake project version state it.
add_executable(dagda-migrate dagda_migrate.cpp)
target_link_libraries(dagda-migrate PRIVATE dagda dagda_warnings)
