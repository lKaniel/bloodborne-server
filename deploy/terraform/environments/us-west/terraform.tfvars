# Region us-west. A new region: copy this folder, change these lines and backend.hcl's key.
# Secrets come from the environment, never from here: TF_VAR_hcloud_token, TF_VAR_backup_password,
# TF_VAR_backup_env='{AWS_ACCESS_KEY_ID="...",AWS_SECRET_ACCESS_KEY="..."}', TF_VAR_cloudflare_api_token.
region       = "us-west"
location     = "hil"
shard_count  = 1
server_type  = "ccx13"
network_cidr = "10.0.0.0/16"
ssh_keys     = ["ops"]
admin_cidrs  = ["203.0.113.10/32"] # the operators' and CI's addresses
image_tag    = "sha-0000000"       # CI's tag for the build every game runs

chart_values_files = ["../../helm/yharnam/values/us-west.yaml"]

backup_repository = "s3:https://s3.us-west-004.backblazeb2.com/yharnam-backups"

cloudflare_zone_id = "" # set (and TF_VAR_cloudflare_api_token) for s<n>.us-west.<dns_zone> records
dns_zone           = "yharnam.example.net"
