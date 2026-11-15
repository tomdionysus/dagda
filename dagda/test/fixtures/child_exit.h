//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

namespace dagda::test {

// Ends a forked child with `code`, as ::_exit does, for the cases that end a
// process without its stores' teardown. Under ThreadSanitizer a child inherits
// the parent's count of reports and its _exit answers any count with 66, so a
// child that did nothing wrong fails for a race found elsewhere in the run. Each
// child here counts from zero: one that raised no report of its own exits with
// `code`; one that did exits as ThreadSanitizer would. Outside ThreadSanitizer,
// ::_exit.
[[noreturn]] void child_exit(int code);

}  // namespace dagda::test
