# One region: the root every environments/<region> runs (deploy/terraform/tf.sh <region> ...).

module "dns" {
  source    = "../modules/dns-cloudflare"
  count     = var.cloudflare_zone_id == "" ? 0 : 1
  zone_id   = var.cloudflare_zone_id
  zone_name = var.dns_zone
  region    = var.region
  ipv4      = module.region.shard_ipv4
  ipv6      = module.region.shard_ipv6
}

module "region" {
  source             = "../modules/region"
  region             = var.region
  location           = var.location
  shard_count        = var.shard_count
  server_type        = var.server_type
  volume_size_gb     = var.volume_size_gb
  network_cidr       = var.network_cidr
  ssh_keys           = var.ssh_keys
  admin_cidrs        = var.admin_cidrs
  tls_ports          = var.tls_ports
  k3s_version        = var.k3s_version
  image_tag          = var.image_tag
  image_repository   = var.image_repository
  chart_path         = "${path.root}/../../helm/yharnam"
  chart_values_files = [for f in var.chart_values_files : "${path.root}/${f}"]
  extra_values       = var.extra_values
  backup_repository  = var.backup_repository
  backup_env         = var.backup_env
  backup_password    = var.backup_password
  # With DNS: the shards' names (what the launcher and the directory hand out); without: IPs.
  public_names = var.cloudflare_zone_id == "" ? [] : [for i in range(var.shard_count) : "s${i}.${var.region}.${var.dns_zone}"]
}

output "shard_ipv4" {
  value = module.region.shard_ipv4
}

output "shard_names" {
  value = var.cloudflare_zone_id == "" ? module.region.shard_ipv4 : module.dns[0].names
}

output "kube_host" {
  value = module.region.kube_host
}

output "kubeconfig" {
  value     = module.region.kubeconfig
  sensitive = true
}
