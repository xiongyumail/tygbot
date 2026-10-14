#!/usr/bin/env bash
set -euo pipefail
test_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
test_dir=$(mktemp -d -t tygbot-esphome-tests-XXXXXXXX)
# Ubuntu 24.04 的 mbedTLS 2.28 把 MD5 接口标为弃用；固件用 ESP-IDF 的 mbedTLS 3.x，不受影响。
cc -std=c11 -Wall -Wextra -Werror -Wno-deprecated-declarations -g -fsanitize=address,undefined \
  -I "$test_root/core" -I /usr/include/cjson \
  "$test_root/core/core.c" "$test_root/core/auth.c" "$test_root/tests/test_core.c" -lcjson -lmbedcrypto -o "$test_dir/test_core"
"$test_dir/test_core"
printf 'Test executable retained at %s\n' "$test_dir/test_core"
