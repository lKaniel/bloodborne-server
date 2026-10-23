variable "region" {
  description = "Short region name: in resource names, the shards' names (<region>-<n>) and DNS (s<n>.<region>.<zone>)."
  type        = string
  validation {
    condition     = can(regex("^[a-z][a-z0-9-]{1,20}$", var.region))
    error_message = "region: lower-case letters, digits and dashes."
  }
}

variable "location" {
  description = "Hetzner location: fsn1, nbg1, hel1 (EU), ash (US East), hil (US West), sin (Singapore)."
  type        = string
  validation {
    condition     = contains(["fsn1", "nbg1", "hel1", "ash", "hil", "sin"], var.location)
    error_message = "location must be one of fsn1, nbg1, hel1, ash, hil, sin."
  }
}

variable "shard_count" {
  description = "Shards in the region: one server (and one volume) each. ~500 players a shard."
  type        = number
  default     = 1
  validation {
    condition     = var.shard_count >= 1 && var.shard_count <= 20
    error_message = "shard_count: 1 to 20."
  }
}

variable "server_type" {
  description = "Hetzner type of each shard server. Dedicated vCPUs (ccx*) keep the link thread's latency steady; cpx* is cheaper."
  type        = string
  default     = "ccx13"
}

variable "image" {
  description = "Hetzner OS image of the servers."
  type        = string
  default     = "ubuntu-24.04"
}

variable "volume_size_gb" {
  description = "Data volume of each shard (accounts, worlds, link key): survives the server being replaced."
  type        = number
  default     = 20
}

variable "network_cidr" {
  description = "Private network of the region (k3s traffic between nodes)."
  type        = string
  default     = "10.0.0.0/16"
}

variable "ssh_keys" {
  description = "Names or ids of SSH keys already in the Hetzner project."
  type        = list(string)
}

variable "admin_cidrs" {
  description = "Addresses allowed to SSH (22) and the Kubernetes API (6443): the operators' and CI's."
  type        = list(string)
  validation {
    condition     = length(var.admin_cidrs) > 0 && !contains(var.admin_cidrs, "0.0.0.0/0")
    error_message = "admin_cidrs: at least one address, and not the whole internet."
  }
}

variable "tls_ports" {
  description = "Extra public TCP ports for the TLS front (the chart's tls.caddy.sites), e.g. [443, 8443]. Empty: none."
  type        = list(number)
  default     = []
}

variable "k3s_version" {
  description = "k3s release (https://github.com/k3s-io/k3s/releases)."
  type        = string
  default     = "v1.31.2+k3s1"
}

variable "chart_path" {
  description = "The Helm chart (deploy/helm/yharnam) or a chart reference."
  type        = string
}

variable "chart_values_files" {
  description = "Helm values files for the region, applied in order (deploy/helm/yharnam/values/<region>.yaml)."
  type        = list(string)
  default     = []
}

variable "image_tag" {
  description = "shadNet image tag (sha-<commit>): every shard and every game run one build."
  type        = string
}

variable "image_repository" {
  type    = string
  default = "ghcr.io/lkaniel/yharnam-shadnet"
}

variable "public_names" {
  description = "Public DNS names of the shards, by ordinal (the DNS module's output). Empty: their IPv4 addresses."
  type        = list(string)
  default     = []
}

variable "backup_repository" {
  description = "restic repository base for the chart's backup CronJob (each shard: <base>/<region>-<n>). Empty: no backups (the chart warns)."
  type        = string
  default     = ""
}

variable "backup_env" {
  description = "The backup target's credentials as env: AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY (S3, B2's S3 API, Hetzner Object Storage), or B2_ACCOUNT_ID/B2_ACCOUNT_KEY."
  type        = map(string)
  default     = {}
  sensitive   = true
}

variable "backup_password" {
  description = "restic's repository password. Keep a copy off the cluster: without it the backups do not open."
  type        = string
  default     = ""
  sensitive   = true
}

variable "extra_values" {
  description = "Further chart values (a map, merged last)."
  type        = any
  default     = {}
}
