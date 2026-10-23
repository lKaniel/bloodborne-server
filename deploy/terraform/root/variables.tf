variable "hcloud_token" {
  description = "Hetzner Cloud API token (read/write) of the project. Better: TF_VAR_hcloud_token."
  type        = string
  sensitive   = true
}

variable "region" {
  type = string
}

variable "location" {
  type = string
}

variable "shard_count" {
  type    = number
  default = 1
}

variable "server_type" {
  type    = string
  default = "ccx13"
}

variable "volume_size_gb" {
  type    = number
  default = 20
}

variable "network_cidr" {
  type    = string
  default = "10.0.0.0/16"
}

variable "ssh_keys" {
  type = list(string)
}

variable "admin_cidrs" {
  type = list(string)
}

variable "tls_ports" {
  type    = list(number)
  default = []
}

variable "k3s_version" {
  type    = string
  default = "v1.31.2+k3s1"
}

variable "image_tag" {
  description = "sha-<commit> from CI (.github/workflows/deploy.yml)."
  type        = string
}

variable "image_repository" {
  type    = string
  default = "ghcr.io/lkaniel/yharnam-shadnet"
}

variable "chart_values_files" {
  description = "Relative to this folder, e.g. [\"../../helm/yharnam/values/eu.yaml\"]."
  type        = list(string)
  default     = []
}

variable "extra_values" {
  type    = any
  default = {}
}

variable "backup_repository" {
  type    = string
  default = ""
}

variable "backup_env" {
  type      = map(string)
  default   = {}
  sensitive = true
}

variable "backup_password" {
  type      = string
  default   = ""
  sensitive = true
}

variable "cloudflare_api_token" {
  description = "Cloudflare token with DNS edit on the zone. Empty with an empty zone id: no DNS records."
  type        = string
  default     = ""
  sensitive   = true
}

variable "cloudflare_zone_id" {
  type    = string
  default = ""
}

variable "dns_zone" {
  description = "The zone's name (yharnam.example.net): shards are s<n>.<region>.<zone>."
  type        = string
  default     = ""
}
