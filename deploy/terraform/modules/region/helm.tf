# What runs on the cluster: the namespace, one PersistentVolume a shard on its node's Hetzner
# volume (static, so a rebuilt cluster finds the same folder, link key included), the backup
# credentials, and the chart. The helm and kubernetes providers are configured by the caller
# from this module's kube_* outputs (deploy/terraform/root/providers.tf).

locals {
  namespace = "yharnam"
  release   = "yharnam"
  shard_sts = "yharnam-shard" # the chart's <release>-shard when the release is "yharnam"
  addresses = length(var.public_names) > 0 ? var.public_names : hcloud_server.node[*].ipv4_address
}

resource "kubernetes_namespace_v1" "yharnam" {
  metadata {
    name   = local.namespace
    labels = { "pod-security.kubernetes.io/enforce" = "privileged" } # hostNetwork shards
  }
  depends_on = [terraform_data.api_ready]
}

resource "kubernetes_storage_class_v1" "local" {
  metadata {
    name = "yharnam-local"
  }
  storage_provisioner = "kubernetes.io/no-provisioner"
  reclaim_policy      = "Retain"
  volume_binding_mode = "WaitForFirstConsumer"
  depends_on          = [terraform_data.api_ready]
}

resource "kubernetes_persistent_volume_v1" "shard" {
  count = var.shard_count
  metadata {
    name   = "${local.name}-shard-${count.index}"
    labels = { "yharnam.dev/shard" = tostring(count.index) }
  }
  spec {
    capacity                         = { storage = "${var.volume_size_gb}Gi" }
    access_modes                     = ["ReadWriteOnce"]
    persistent_volume_reclaim_policy = "Retain"
    storage_class_name               = kubernetes_storage_class_v1.local.metadata[0].name
    claim_ref {
      namespace = local.namespace
      name      = "data-${local.shard_sts}-${count.index}"
    }
    persistent_volume_source {
      local {
        path = "${local.data_path}/shard-${count.index}"
      }
    }
    node_affinity {
      required {
        node_selector_term {
          match_expressions {
            key      = "kubernetes.io/hostname"
            operator = "In"
            values   = [hcloud_server.node[count.index].name]
          }
        }
      }
    }
  }
}

resource "kubernetes_secret_v1" "backup" {
  count = var.backup_repository == "" ? 0 : 1
  metadata {
    name      = "yharnam-backup"
    namespace = kubernetes_namespace_v1.yharnam.metadata[0].name
  }
  data = merge(var.backup_env, { RESTIC_PASSWORD = var.backup_password })
}

resource "helm_release" "yharnam" {
  name      = local.release
  namespace = kubernetes_namespace_v1.yharnam.metadata[0].name
  chart     = var.chart_path
  # The region's values files, then extra_values (a map) last.
  values           = concat([for f in var.chart_values_files : file(f)], [yamlencode(var.extra_values)])
  create_namespace = false
  wait             = true
  timeout          = 600

  # What Terraform knows: the servers, their names and addresses, the volumes.
  set {
    name  = "region"
    value = var.region
  }
  set {
    name  = "image.repository"
    value = var.image_repository
  }
  set {
    name  = "image.tag"
    value = var.image_tag
  }
  set {
    name  = "shards.count"
    value = var.shard_count
  }
  set {
    name  = "shards.expose"
    value = "hostNetwork"
  }
  set {
    name  = "shards.persistence.storageClass"
    value = kubernetes_storage_class_v1.local.metadata[0].name
  }
  set {
    name  = "shards.persistence.size"
    value = "${var.volume_size_gb}Gi"
  }
  set_list {
    name  = "shards.publicAddresses"
    value = local.addresses
  }
  set_list {
    name  = "networkPolicy.nodeCidrs"
    value = [var.network_cidr]
  }
  dynamic "set" {
    for_each = var.backup_repository == "" ? {} : { "backup.enabled" = "true", "backup.repository" = var.backup_repository, "backup.existingSecret" = "yharnam-backup" }
    content {
      name  = set.key
      value = set.value
    }
  }

  depends_on = [kubernetes_persistent_volume_v1.shard, kubernetes_secret_v1.backup, hcloud_volume_attachment.data]
}
