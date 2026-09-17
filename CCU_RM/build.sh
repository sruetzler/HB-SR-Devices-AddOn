#!/bin/sh
# Compatibility entry point for the former addon-only project.
set -eu
PROJECT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
exec sh "$PROJECT_DIR/scripts/build-addon.sh" "$@"
