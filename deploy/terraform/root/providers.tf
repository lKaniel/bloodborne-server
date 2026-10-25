provider "hcloud" {
  token = var.hcloud_token
}

# The region's own cluster, with the admin certificate the module made (no kubeconfig file).
provider "kubernetes" {
  host                   = module.region.kube_host
  cluster_ca_certificate = module.region.kube_ca_certificate
  client_certificate     = module.region.kube_client_certificate
  client_key             = module.region.kube_client_key
}

provider "helm" {
  kubernetes {
    host                   = module.region.kube_host
    cluster_ca_certificate = module.region.kube_ca_certificate
    client_certificate     = module.region.kube_client_certificate
    client_key             = module.region.kube_client_key
  }
}

# Only used when cloudflare_zone_id is set; the placeholder token keeps the provider quiet
# otherwise (it is never called then).
provider "cloudflare" {
  api_token = var.cloudflare_api_token != "" ? var.cloudflare_api_token : "0000000000000000000000000000000000000000"
}
