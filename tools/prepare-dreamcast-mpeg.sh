#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
root=.deps/dreamcast-pl-mpeg
revision=24b588bd6a63e8dae58b45c030a2018c381a31ab
mkdir -p "$root"
fetch() {
    local name=$1 destination=$2 hash=$3
    if [[ ! -f "$root/$destination" ]]; then
        curl --fail --location --retry 2 \
            "https://raw.githubusercontent.com/Dreamcast-Projects/pl_mpeg/$revision/$name" \
            -o "$root/$destination.part"
        printf '%s  %s\n' "$hash" "$root/$destination.part" | sha256sum --check
        mv "$root/$destination.part" "$root/$destination"
    fi
    printf '%s  %s\n' "$hash" "$root/$destination" | sha256sum --check
}
fetch pl_mpeg.h pl_mpeg.upstream.h 85d6256d21440077018cf6632101b040421180693181bf6bd5ca6bd911501745
fetch LICENSE LICENSE 9f88c1e2da2ea20c2f672b33976aadcfe1b6ede81ebaa4cc393fa9510721d611
cp "$root/pl_mpeg.upstream.h" "$root/pl_mpeg.h.part"
patch --silent "$root/pl_mpeg.h.part" < tools/dreamcast-pl-mpeg-copy.patch
if ! cmp -s "$root/pl_mpeg.h.part" "$root/pl_mpeg.h"; then
    mv "$root/pl_mpeg.h.part" "$root/pl_mpeg.h"
else
    rm "$root/pl_mpeg.h.part"
fi
