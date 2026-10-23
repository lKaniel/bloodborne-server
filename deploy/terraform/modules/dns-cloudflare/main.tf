# DNS for one region on Cloudflare: s<n>.<region>.<zone> -> shard n (A and AAAA), unproxied
# (Cloudflare's proxy carries no UDP and would hide the players' addresses).
# Another DNS host: a module with the same inputs and the `names` output (docs/DEPLOY.md).
terraform {
  required_providers {
    cloudflare = {
      source  = "cloudflare/cloudflare"
      version = "~> 4.48"
    }
  }
}

variable "zone_id" {
  type = string
}

variable "zone_name" {
  description = "The zone's name, e.g. yharnam.example.net."
  type        = string
}

variable "region" {
  type = string
}

variable "ipv4" {
  type = list(string)
}

variable "ipv6" {
  type    = list(string)
  default = []
}

variable "ttl" {
  type    = number
  default = 300
}

locals {
  names = [for i, _ in var.ipv4 : "s${i}.${var.region}.${var.zone_name}"]
}

resource "cloudflare_record" "a" {
  count   = length(var.ipv4)
  zone_id = var.zone_id
  name    = "s${count.index}.${var.region}"
  type    = "A"
  content = var.ipv4[count.index]
  ttl     = var.ttl
  proxied = false
}

resource "cloudflare_record" "aaaa" {
  count   = length(var.ipv6)
  zone_id = var.zone_id
  name    = "s${count.index}.${var.region}"
  type    = "AAAA"
  content = var.ipv6[count.index]
  ttl     = var.ttl
  proxied = false
}

output "names" {
  description = "Each shard's public name, by ordinal."
  value       = local.names
}
