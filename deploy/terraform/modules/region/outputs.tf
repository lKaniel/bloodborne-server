output "shard_ipv4" {
  description = "Public IPv4 of each shard, by ordinal."
  value       = hcloud_server.node[*].ipv4_address
}

output "shard_ipv6" {
  value = hcloud_server.node[*].ipv6_address
}

output "kube_host" {
  description = "The region's Kubernetes API (reachable from admin_cidrs only)."
  value       = "https://${hcloud_server.node[0].ipv4_address}:6443"
}

output "kube_ca_certificate" {
  value = tls_self_signed_cert.server_ca.cert_pem
}

output "kube_client_certificate" {
  value = tls_locally_signed_cert.admin.cert_pem
}

output "kube_client_key" {
  value     = tls_private_key.admin.private_key_pem
  sensitive = true
}

output "kubeconfig" {
  description = "terraform output -raw kubeconfig > ~/.kube/yharnam-<region>"
  sensitive   = true
  value = yamlencode({
    apiVersion      = "v1"
    kind            = "Config"
    current-context = local.name
    clusters = [{
      name    = local.name
      cluster = { server = "https://${hcloud_server.node[0].ipv4_address}:6443", certificate-authority-data = base64encode(tls_self_signed_cert.server_ca.cert_pem) }
    }]
    users = [{
      name = "${local.name}-admin"
      user = { client-certificate-data = base64encode(tls_locally_signed_cert.admin.cert_pem), client-key-data = base64encode(tls_private_key.admin.private_key_pem) }
    }]
    contexts = [{ name = local.name, context = { cluster = local.name, user = "${local.name}-admin", namespace = "yharnam" } }]
  })
}
