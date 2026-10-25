{{- define "yharnam.fullname" -}}
{{- if contains .Chart.Name .Release.Name -}}
{{- .Release.Name | trunc 40 | trimSuffix "-" -}}
{{- else -}}
{{- printf "%s-%s" .Release.Name .Chart.Name | trunc 40 | trimSuffix "-" -}}
{{- end -}}
{{- end -}}

{{- define "yharnam.shard" -}}{{ include "yharnam.fullname" . }}-shard{{- end -}}

{{- define "yharnam.labels" -}}
helm.sh/chart: {{ .Chart.Name }}-{{ .Chart.Version | replace "+" "_" }}
app.kubernetes.io/instance: {{ .Release.Name }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
app.kubernetes.io/part-of: yharnam
yharnam.dev/region: {{ .Values.region | quote }}
{{- end -}}

{{- define "yharnam.shardSelector" -}}
app.kubernetes.io/name: shadnet
app.kubernetes.io/instance: {{ .Release.Name }}
app.kubernetes.io/component: shard
{{- end -}}

{{- define "yharnam.image" -}}
{{- $tag := .image.tag | default .default -}}
{{- printf "%s:%s" .image.repository $tag -}}
{{- end -}}

{{- define "yharnam.serviceAccountName" -}}
{{- if .Values.serviceAccount.create -}}
{{- .Values.serviceAccount.name | default (include "yharnam.fullname" .) -}}
{{- else -}}
{{- .Values.serviceAccount.name | default "default" -}}
{{- end -}}
{{- end -}}

{{- define "yharnam.hostMode" -}}
{{- if has .Values.shards.expose (list "hostNetwork" "hostPort") }}true{{ end -}}
{{- end -}}

{{- define "yharnam.directorySecret" -}}
{{- .Values.directory.existingSecret | default (printf "%s-directory" (include "yharnam.fullname" .)) -}}
{{- end -}}

{{/* Fail early on combinations that cannot work. */}}
{{- define "yharnam.validate" -}}
{{- if not (has .Values.shards.expose (list "hostNetwork" "hostPort" "loadBalancer")) -}}
{{- fail "shards.expose must be hostNetwork, hostPort or loadBalancer" -}}
{{- end -}}
{{- if not (has .Values.tls.mode (list "none" "caddy" "ingress")) -}}
{{- fail "tls.mode must be none, caddy or ingress" -}}
{{- end -}}
{{- if lt (int .Values.shards.count) 1 -}}
{{- fail "shards.count must be at least 1" -}}
{{- end -}}
{{- if and .Values.backup.enabled (not .Values.backup.repository) -}}
{{- fail "backup.enabled needs backup.repository" -}}
{{- end -}}
{{- if and .Values.backup.enabled (not .Values.backup.existingSecret) -}}
{{- fail "backup.enabled needs backup.existingSecret (RESTIC_PASSWORD and the target's keys)" -}}
{{- end -}}
{{- end -}}

{{- define "yharnam.shardLabels" -}}
{{ include "yharnam.labels" . }}
app.kubernetes.io/name: shadnet
app.kubernetes.io/component: shard
{{- end -}}
