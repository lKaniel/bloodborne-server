#!/bin/sh
# Seeds /data on first start, then runs shadNet in the foreground. UNTESTED.
set -eu
cd /data
for f in worlds.cfg scoreboards.cfg; do
    [ -f "$f" ] || cp "/opt/shadnet/defaults/$f" "$f"
done
if [ ! -f shadnet.cfg ]; then
    # Public-host defaults; shadNet writes every other key with its default on first load.
    cat > shadnet.cfg <<EOF
[General]
Host=0.0.0.0
; Games reach the TCP port through the link's tunnel (phase 11): keep it on 127.0.0.1.
TcpLocalOnly=true
; docs/HOSTED_SERVER.md: rate limits, bans (banned.txt), team-only worlds, own names only.
Hosted=true
HostedBuild=${SHADNET_BUILD:-}
BloodborneSeamlessCoop=true
BloodborneWorldPath=bloodborne_worlds
BackendStore=db/backend.db
StatsEnabled=false
EOF
fi
mkdir -p db bloodborne_worlds
# No --watchdog: Docker/systemd restart the process instead.
exec /opt/shadnet/shadnet "$@"
