//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

// The order a pass visits loose objects in (TODO/2026-11-19-migrate-speed.md). Measured
// on live-a and live-b (the Macha session, 2026-11-19): a loose object costs a seek
// to its inode and another to its bytes, and visited in id order a 4 MiB object took
// about 45 ms; with the inodes read first and the files visited by where their bytes
// start, 25 to 33 ms whole and about 6 ms for the first 4 KiB. Every loose object there
// is one extent.
namespace dagda::migrate {

// The indexes of `files` in the order their bytes start on the device: the first extent
// FIEMAP reports, on Linux, each file written out first so its blocks are given;
// elsewhere, or for a file whose place is unknown or kept in its inode, the inode
// number, after every file placed. Ties keep the given order. A run that counts in this
// order (the migration's progress record) still checks the file at its point, as a
// file system may move a file between runs. Each file is opened and closed, which
// brings its inode in. Throws std::runtime_error for a file that cannot be opened.
// Waits on the device; thread-safe.
std::vector<size_t> disk_order(const std::vector<std::filesystem::path>& files);

}  // namespace dagda::migrate
