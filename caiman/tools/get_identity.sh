#!/bin/bash
# SPDX-License-Identifier: MIT
# Copy the iPhone identity that HackiOS's xnuloader/mkidentity.sh made from your own IPSW into
# probe/identity.h (git-ignored: it is Apple's data), for make LOADER=1.
#
#   caiman/tools/get_identity.sh [path/to/hakios-xnu-kernel]      (default ~/caiman-work/hakios-xnu-kernel)
#
# The names become fixed arrays: the loader is linked position-independent with no relocations, and a
# table of pointers would need them.
set -e
src=${1:-$HOME/caiman-work/hakios-xnu-kernel}/xnuloader/identity.h
dst=$(cd "$(dirname "$0")/../probe" && pwd)/identity.h
[ -f "$src" ] || { echo "get_identity.sh: no $src: run xnuloader/mkidentity.sh there first" >&2; exit 1; }
sed 's/^struct identity_prop { const char \*name; unsigned offset, length; };$/struct identity_prop { char name[64]; unsigned offset, length; };/' \
	"$src" > "$dst"
grep -q 'char name\[64\]' "$dst" || { echo "get_identity.sh: $src has another layout" >&2; rm -f "$dst"; exit 1; }
echo "get_identity.sh: $dst"
