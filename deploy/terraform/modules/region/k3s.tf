# The cluster's CAs and an admin client certificate, made here and written onto node 0 before
# k3s first starts: the helm and kubernetes providers authenticate with them, so no kubeconfig
# has to be copied off the server. The keys are in the Terraform state and in the server's user
# data (visible to the Hetzner project's members): keep both private.

resource "tls_private_key" "server_ca" {
  algorithm   = "ECDSA"
  ecdsa_curve = "P256"
}

resource "tls_self_signed_cert" "server_ca" {
  private_key_pem       = tls_private_key.server_ca.private_key_pem
  is_ca_certificate     = true
  validity_period_hours = 87600
  allowed_uses          = ["cert_signing", "key_encipherment", "digital_signature"]
  subject {
    common_name = "k3s-server-ca@${local.name}"
  }
}

resource "tls_private_key" "client_ca" {
  algorithm   = "ECDSA"
  ecdsa_curve = "P256"
}

resource "tls_self_signed_cert" "client_ca" {
  private_key_pem       = tls_private_key.client_ca.private_key_pem
  is_ca_certificate     = true
  validity_period_hours = 87600
  allowed_uses          = ["cert_signing", "key_encipherment", "digital_signature"]
  subject {
    common_name = "k3s-client-ca@${local.name}"
  }
}

resource "tls_private_key" "admin" {
  algorithm   = "ECDSA"
  ecdsa_curve = "P256"
}

resource "tls_cert_request" "admin" {
  private_key_pem = tls_private_key.admin.private_key_pem
  subject {
    common_name  = "yharnam-terraform"
    organization = "system:masters"
  }
}

resource "tls_locally_signed_cert" "admin" {
  cert_request_pem      = tls_cert_request.admin.cert_request_pem
  ca_private_key_pem    = tls_private_key.client_ca.private_key_pem
  ca_cert_pem           = tls_self_signed_cert.client_ca.cert_pem
  validity_period_hours = 8760
  early_renewal_hours   = 720
  allowed_uses          = ["client_auth", "digital_signature", "key_encipherment"]
}

# The API answers once k3s is up (cloud-init takes a minute or two); the chart waits for it.
resource "terraform_data" "api_ready" {
  triggers_replace = [hcloud_server.node[0].id]
  provisioner "local-exec" {
    command     = <<-EOT
      for i in $(seq 1 90); do
        code=$(curl -s -o /dev/null -w '%%{http_code}' --cacert <(printf '%s' "$CA") https://${hcloud_server.node[0].ipv4_address}:6443/readyz || true)
        [ "$code" != "000" ] && exit 0
        sleep 10
      done
      echo "k3s API at ${hcloud_server.node[0].ipv4_address}:6443 never answered" >&2; exit 1
    EOT
    interpreter = ["bash", "-c"]
    environment = { CA = tls_self_signed_cert.server_ca.cert_pem }
  }
}
