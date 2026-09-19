#!/bin/bash
# After the fix: kdbctl must WORK WITHOUT A CONSOLE, with output redirected to a file.
cd "$(dirname "$0")/../.." || exit 1
ROOT="$(pwd -W 2>/dev/null || pwd)"   # native form the disk:// backend expects
export PATH="/d/MinW64-15.2.0/bin:$PATH"; export MSYS2_ARG_CONV_EXCL='*'
K=./build/kdbctl.exe
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1; sleep 1
rm -rf build/cl15 && mkdir -p build/cl15
./build/kdbsvr.exe init "disk://$ROOT/build/cl15/a" >/dev/null
(./build/kdbsvr.exe server 1 8601 8602 "disk://$ROOT/build/cl15/a" "1@127.0.0.1:8601:8602" > build/cl15/a.log 2>&1 &)
sleep 6
echo "=== one-shot, stdout REDIRECTED TO A FILE ==="
timeout 30 $K 127.0.0.1:8601 SET __p__ hello-script > build/cl15/set.txt 2>&1; echo "  SET exit=$? bytes=$(wc -c < build/cl15/set.txt)"; cat build/cl15/set.txt
timeout 30 $K 127.0.0.1:8601 GET __p__ > build/cl15/get.txt 2>&1; echo "  GET exit=$? bytes=$(wc -c < build/cl15/get.txt)"; cat build/cl15/get.txt
timeout 30 $K 127.0.0.1:8601 STATS > build/cl15/stats.txt 2>&1; echo "  STATS exit=$? bytes=$(wc -c < build/cl15/stats.txt)"; head -c 200 build/cl15/stats.txt; echo
timeout 30 $K 127.0.0.1:8601 BOGUS > build/cl15/bogus.txt 2>&1; echo "  BOGUS exit=$?"; cat build/cl15/bogus.txt
echo "=== also through a pipe ==="
timeout 30 $K 127.0.0.1:8601 GET __p__ 2>&1 | head -2
echo "=== interactive path still alive? (plain pipe of a REPL line, no tty) ==="
printf 'GET __p__\n' | timeout 20 $K 127.0.0.1:8601 2>&1 | head -3
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
echo CL15_DONE
