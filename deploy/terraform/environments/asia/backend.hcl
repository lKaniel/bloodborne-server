# Remote state for region asia (docs/DEPLOY.md, "Terraform state"). Credentials:
# AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY of the state bucket, in the environment.
bucket                      = "yharnam-tfstate"
key                         = "regions/asia.tfstate"
region                      = "eu-central-1" # any value: only AWS checks it
endpoints                   = { s3 = "https://fsn1.your-objectstorage.com" }
skip_credentials_validation = true
skip_region_validation      = true
skip_requesting_account_id  = true
skip_metadata_api_check     = true
skip_s3_checksum            = true
use_path_style              = true
# Terraform >= 1.10: a lock file next to the state (no DynamoDB). Older: run one apply at a time.
# use_lockfile = true
