#!/usr/bin/env bash
# Smoke test of the chart on kind (docs/DEPLOY.md, "What was tested"):
#   deploy/kind/smoke.sh <bbload binary on this host> [players]
# Needs docker, kind, kubectl, helm and the images yharnam-shadnet:local, yharnam-backup:local
# and yharnam-bbload:local (server/hosting/Dockerfile, its `loadtest` target, Dockerfile.backup).
#  1. install (hostPort), the key made;
#  2. players from the host through kind's port mapping: Docker NATs them all to one address, and
#     the hosted per-address limits admit only a few (what any NAT in front of a shard does);
#  3. the shard with hostNetwork, players from the node's network each from its own address;
#  4. a backup (restic to a host folder), then a restore by the init container.
set -euo pipefail
export LANG=${LANG:-C.UTF-8} # Qt warns without a UTF-8 locale
here=$(cd "$(dirname "$0")" && pwd)
chart=$here/../helm/yharnam
bbload=${1:?usage: smoke.sh <bbload> [players]}
players=${2:-90}
cluster=${KIND_CLUSTER:-yharnam}
ns=yharnam
pod=yharnam-shard-0
admin() { kubectl -n "$ns" exec "$pod" -c shadnet -- shadnet --admin "$@"; }
ready() {
    kubectl -n "$ns" wait --for=condition=Ready "pod/$pod" --timeout=300s
}

mkdir -p /tmp/yharnam-kind-restic && chmod 777 /tmp/yharnam-kind-restic
if ! kind get clusters | grep -qx "$cluster"; then
    if [ "${KIND_OOM_SHIM:-0}" = 1 ]; then
        # The node's runc swapped for runc-oom-shim.sh while kubeadm waits for its pods.
        kind create cluster --name "$cluster" --config "$here/kind-config.yaml" --wait 180s &
        create=$!
        node=$cluster-control-plane
        until docker exec "$node" test -x /usr/local/sbin/runc 2>/dev/null; do sleep 2; done
        docker exec "$node" sh -c 'mv /usr/local/sbin/runc /usr/local/sbin/runc.real'
        docker cp "$here/runc-oom-shim.sh" "$node:/usr/local/sbin/runc"
        wait "$create"
    else
        kind create cluster --name "$cluster" --config "$here/kind-config.yaml" --wait 180s
    fi
fi
for image in yharnam-shadnet:local yharnam-backup:local yharnam-bbload:local; do
    kind load docker-image --name "$cluster" "$image"
done
kubectl create namespace "$ns" --dry-run=client -o yaml | kubectl apply -f -
kubectl -n "$ns" create secret generic kind-backup --from-literal=RESTIC_PASSWORD=kind-test \
    --dry-run=client -o yaml | kubectl apply -f -

echo "== 1. install (hostPort)"
helm upgrade --install yharnam "$chart" -n "$ns" -f "$chart/values/kind.yaml" --wait --timeout 5m
ready
kubectl -n "$ns" exec "$pod" -c shadnet -- ls -l /data/link_key.bin
admin key

echo "== 2. 12 games from the host via the port mapping (one NAT address)"
"$bbload" --server 127.0.0.1 --port 41314 --teams 4 --team-size 3 --seconds 20 --tag nat --json \
    | tail -1 || true
admin stats

echo "== 3. hostNetwork, $players games from their own addresses"
helm upgrade yharnam "$chart" -n "$ns" -f "$chart/values/kind.yaml" --set shards.expose=hostNetwork \
    --wait --timeout 5m
kubectl -n "$ns" delete pod "$pod" --wait # updateStrategy OnDelete: the window
ready
kubectl -n "$ns" delete job bbload --ignore-not-found
sed "s/\"TEAMS\"/\"$((players / 3))\"/; s/\"SECONDS\"/\"60\"/" "$here/bbload-job.yaml" | kubectl -n "$ns" apply -f -
kubectl -n "$ns" wait --for=condition=complete job/bbload --timeout=600s || true
kubectl -n "$ns" logs job/bbload | tail -3
admin stats

echo "== 4. backup, then restore"
key_before=$(kubectl -n "$ns" exec "$pod" -c shadnet -- sha256sum /data/link_key.bin)
kubectl -n "$ns" delete job backup-now --ignore-not-found
kubectl -n "$ns" create job backup-now --from=cronjob/yharnam-shard-0-backup
kubectl -n "$ns" wait --for=condition=complete job/backup-now --timeout=600s
kubectl -n "$ns" logs job/backup-now | tail -8
# Lose the key, then restore it at the next start.
kubectl -n "$ns" exec "$pod" -c shadnet -- rm /data/link_key.bin
helm upgrade yharnam "$chart" -n "$ns" -f "$chart/values/kind.yaml" --set shards.expose=hostNetwork \
    --set restore.shard=0 --set restore.id=smoke-$(date +%s) --wait --timeout 5m
kubectl -n "$ns" delete pod "$pod" --wait
ready
kubectl -n "$ns" logs "$pod" -c restore | tail -6
key_after=$(kubectl -n "$ns" exec "$pod" -c shadnet -- sha256sum /data/link_key.bin)
echo "key before: $key_before"
echo "key after:  $key_after"
[ "$key_before" = "$key_after" ] && echo "restore: the same link key is back"
admin stats
