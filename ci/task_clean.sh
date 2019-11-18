#!/bin/bash
set -e
set -u
set -o pipefail
source ./ci/env.sh

echo "Cleanup data..."
BUILD_DIR=$1
cd $BUILD_DIR && find . -type f ! -name 'config.cmake' -delete && find . -type d -delete && cd ..