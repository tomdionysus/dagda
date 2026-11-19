#!/bin/sh
#
# Dagda - The Distributed Object Store
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# Builds the Debian packages (libdagda<version>, libdagda-dev, dagda-tools and
# debug symbols) on Debian trixie from a git checkout, natively for the
# machine's architecture, with Dannan's packages installed. The packages are
# written to build-deb/.
#
#   sudo apt install dpkg-dev && sudo apt build-dep ./
#   tools/build-deb.sh [dpkg-buildpackage options]
#
# The version comes from `git describe`: the tag 0.3.1 gives 0.3.1, and a build
# three commits later 0.3.1+git3.gabc1234, which sorts after 0.3.1 and before
# 0.3.2; where no tag is reachable (develop), the tree's version and the commit,
# 0.3.1+git.gabc1234. Uncommitted changes add .dirty. DEB_BUILD_OPTIONS=nocheck skips the
# tests. debian/control names the library's package for the CMake version.

set -eu

top=$(git rev-parse --show-toplevel)
cd "$top"

project=$(sed -nE 's/^project\(dagda VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' CMakeLists.txt)

# Release tags sit on main, which develop does not reach: a build where none is
# reachable is the tree's version and the commit.
if describe=$(git describe --tags --match '[0-9]*.[0-9]*.[0-9]*' --dirty 2>/dev/null); then
    version=$(echo "$describe" | sed -E 's/-([0-9]+)-g([0-9a-f]+)/+git\1.g\2/; s/-dirty$/.dirty/')
else
    describe=$(git describe --always --dirty)
    version="$project+git.g$(echo "$describe" | sed -E 's/-dirty$/.dirty/')"
fi
library=$(sed -n 's/^Package: \(libdagda[0-9].*\)/\1/p' debian/control)
if [ "$library" != "libdagda$project" ]; then
    echo "build-deb.sh: debian/control names $library and CMakeLists.txt says $project" >&2
    exit 1
fi
# A build of a tag is a release, and has to be the version the tree says.
# Between tags the tree may already carry the next one.
case "$version" in
    *+git*) ;;
    "$project" | "$project".dirty) ;;
    *)
        echo "build-deb.sh: the tag says $describe and CMakeLists.txt says $project" >&2
        exit 1
        ;;
esac

# One job per CPU unless the caller says otherwise.
case " ${DEB_BUILD_OPTIONS:-} " in
    *" parallel="*) ;;
    *) export DEB_BUILD_OPTIONS="${DEB_BUILD_OPTIONS:+$DEB_BUILD_OPTIONS }parallel=$(nproc)" ;;
esac

cat >debian/changelog <<END
dagda ($version) trixie; urgency=medium

  * Built from $describe.

 -- tomdionysus <tomdionysus@users.noreply.github.com>  $(date -R)
END

dpkg-buildpackage -us -uc -b "$@"

mkdir -p build-deb
for f in ../libdagda*_"$version"_*.deb ../dagda*_"$version"_*.deb ../dagda_"$version"_*.buildinfo ../dagda_"$version"_*.changes; do
    if [ -e "$f" ]; then mv "$f" build-deb/; fi
done
ls -l build-deb/*"$version"*
