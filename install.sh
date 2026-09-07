#!/usr/bin/env bash
# Install both Pi services from the dedicated server directory.
set -Eeuo pipefail
PROJECT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
exec bash "$PROJECT_DIR/server/install.sh" "$@"
