#!/bin/sh
# Only for sandboxes that may not lower oom_score_adj (the 2026-10-23 test box: runc fails with
# "failed to update /proc/self/oom_score_adj: Permission denied" for every pod). Installed by
# smoke.sh as the kind node's runc when KIND_OOM_SHIM=1; the real runc moves to runc.real.
# It drops negative oomScoreAdj from each container's config before runc creates it.
prev=""
for a in "$@"; do
    if [ "$prev" = "--bundle" ] && [ -f "$a/config.json" ]; then
        sed -i 's/"oomScoreAdj":-[0-9]*/"oomScoreAdj":0/' "$a/config.json"
    fi
    prev=$a
done
exec /usr/local/sbin/runc.real "$@"
