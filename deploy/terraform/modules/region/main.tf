# One region on Hetzner Cloud: a server and a volume per shard, a private network, a firewall,
# k3s (server on node 0, agents on the rest) installed by cloud-init, then the yharnam chart.
# The chart runs the shards with hostNetwork (Hetzner's load balancers carry no UDP, and the
# server needs every player's own address and port; docs/DEPLOY.md).

locals {
  name      = "yharnam-${var.region}"
  zone      = { fsn1 = "eu-central", nbg1 = "eu-central", hel1 = "eu-central", ash = "us-east", hil = "us-west", sin = "ap-southeast" }[var.location]
  subnet    = cidrsubnet(var.network_cidr, 8, 1)
  node_ips  = [for i in range(var.shard_count) : cidrhost(local.subnet, 10 + i)]
  api_ip    = local.node_ips[0]
  data_path = "/mnt/yharnam"
  labels    = { app = "yharnam", region = var.region }
}

resource "hcloud_network" "region" {
  name     = local.name
  ip_range = var.network_cidr
  labels   = local.labels
}

resource "hcloud_network_subnet" "nodes" {
  network_id   = hcloud_network.region.id
  type         = "cloud"
  network_zone = local.zone
  ip_range     = local.subnet
}

resource "hcloud_placement_group" "shards" {
  name   = local.name
  type   = "spread" # no two shards on one physical host
  labels = local.labels
}

resource "hcloud_firewall" "shards" {
  name   = local.name
  labels = local.labels

  # The games: the link (UDP) and the two plain-HTTP ports they call at the link's address.
  rule {
    direction  = "in"
    protocol   = "udp"
    port       = "31314"
    source_ips = ["0.0.0.0/0", "::/0"]
  }
  rule {
    direction  = "in"
    protocol   = "tcp"
    port       = "20443"
    source_ips = ["0.0.0.0/0", "::/0"]
  }
  rule {
    direction  = "in"
    protocol   = "tcp"
    port       = "31315"
    source_ips = ["0.0.0.0/0", "::/0"]
  }
  dynamic "rule" {
    for_each = var.tls_ports
    content {
      direction  = "in"
      protocol   = "tcp"
      port       = tostring(rule.value)
      source_ips = ["0.0.0.0/0", "::/0"]
    }
  }
  rule {
    direction  = "in"
    protocol   = "tcp"
    port       = "22"
    source_ips = var.admin_cidrs
  }
  rule {
    direction  = "in"
    protocol   = "tcp"
    port       = "6443"
    source_ips = var.admin_cidrs
  }
  rule {
    direction  = "in"
    protocol   = "icmp"
    source_ips = ["0.0.0.0/0", "::/0"]
  }
  # Everything else (31313, 31320, the kubelet, k3s' own ports) is closed from the internet;
  # the nodes talk over the private network, which Hetzner firewalls do not filter.
}

# The shard's data: kept when its server is replaced (terraform taint, a new type).
resource "hcloud_volume" "data" {
  count     = var.shard_count
  name      = "${local.name}-${count.index}"
  size      = var.volume_size_gb
  location  = var.location
  format    = "ext4"
  labels    = merge(local.labels, { shard = tostring(count.index) })
  automount = false
  lifecycle {
    prevent_destroy = true # holds the accounts, worlds and the link key
  }
}

resource "random_password" "k3s_token" {
  length  = 48
  special = false
}

resource "hcloud_server" "node" {
  count              = var.shard_count
  name               = "${local.name}-${count.index}"
  server_type        = var.server_type
  image              = var.image
  location           = var.location
  ssh_keys           = var.ssh_keys
  placement_group_id = hcloud_placement_group.shards.id
  firewall_ids       = [hcloud_firewall.shards.id]
  labels             = merge(local.labels, { shard = tostring(count.index) })

  network {
    network_id = hcloud_network.region.id
    ip         = local.node_ips[count.index]
  }

  public_net {
    ipv4_enabled = true
    ipv6_enabled = true
  }

  user_data = templatefile("${path.module}/cloud-init.yaml.tftpl", {
    role         = count.index == 0 ? "server" : "agent"
    node_name    = "${local.name}-${count.index}"
    node_ip      = local.node_ips[count.index]
    api_ip       = local.api_ip
    k3s_version  = var.k3s_version
    k3s_token    = random_password.k3s_token.result
    volume_dev   = "/dev/disk/by-id/scsi-0HC_Volume_${hcloud_volume.data[count.index].id}"
    data_path    = local.data_path
    shard        = count.index
    server_ca    = tls_self_signed_cert.server_ca.cert_pem
    server_key   = tls_private_key.server_ca.private_key_pem
    client_ca    = tls_self_signed_cert.client_ca.cert_pem
    client_key   = tls_private_key.client_ca.private_key_pem
    network_cidr = var.network_cidr
  })

  # A changed cloud-init (a new k3s version) must not rebuild a live shard by itself: replace
  # it on purpose in a maintenance window (docs/DEPLOY.md).
  lifecycle {
    ignore_changes = [user_data, image, ssh_keys]
  }

  depends_on = [hcloud_network_subnet.nodes]
}

resource "hcloud_volume_attachment" "data" {
  count     = var.shard_count
  volume_id = hcloud_volume.data[count.index].id
  server_id = hcloud_server.node[count.index].id
  automount = false
}
