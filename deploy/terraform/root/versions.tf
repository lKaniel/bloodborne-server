terraform {
  required_version = ">= 1.6"
  required_providers {
    hcloud     = { source = "hetznercloud/hcloud", version = "~> 1.49" }
    helm       = { source = "hashicorp/helm", version = "~> 2.17" }
    kubernetes = { source = "hashicorp/kubernetes", version = "~> 2.35" }
    cloudflare = { source = "cloudflare/cloudflare", version = "~> 4.48" }
    tls        = { source = "hashicorp/tls", version = "~> 4.0" }
    random     = { source = "hashicorp/random", version = "~> 3.6" }
  }

  # Remote state, one key a region: tf.sh passes environments/<region>/backend.hcl
  # (docs/DEPLOY.md, "Terraform state"). Any S3 API works: Hetzner Object Storage, B2, R2, AWS.
  backend "s3" {}
}
