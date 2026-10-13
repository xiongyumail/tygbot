#!/usr/bin/env bash
set -euo pipefail
test_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
test_dir=$(mktemp -d -t tygbot-esphome-tests-XXXXXXXX)
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I "$test_root/core" -I /usr/include/cjson \
  "$test_root/core/core.c" "$test_root/core/auth.c" "$test_root/tests/test_core.c" -lcjson -lmbedcrypto -o "$test_dir/test_core"
"$test_dir/test_core"
printf 'Test executable retained at %s\n' "$test_dir/test_core"
