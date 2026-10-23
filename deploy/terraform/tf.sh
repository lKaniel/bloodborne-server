#!/usr/bin/env bash
# deploy/terraform/tf.sh <region> <terraform command> [args]: runs the shared root (root/) for
# environments/<region>, with that region's state (backend.hcl) and its own .terraform folder.
#   ./tf.sh eu init
#   ./tf.sh eu plan
#   ./tf.sh eu apply
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
region=${1:?usage: tf.sh <region> <command> [args]}
shift
env=$here/environments/$region
[ -f "$env/terraform.tfvars" ] || { echo "no $env/terraform.tfvars" >&2; exit 1; }
export TF_DATA_DIR=$here/root/.terraform-$region
cmd=${1:?command}
shift
case $cmd in
init) exec terraform -chdir="$here/root" init -reconfigure -backend-config="$env/backend.hcl" "$@" ;;
plan | apply | destroy | import | refresh | console)
    exec terraform -chdir="$here/root" "$cmd" -var-file="$env/terraform.tfvars" "$@" ;;
*) exec terraform -chdir="$here/root" "$cmd" "$@" ;;
esac
