#!/bin/bash
cd "$(dirname "$0")/../.." || exit 1
ROOT="$(pwd -W 2>/dev/null || pwd)"   # native form the disk:// backend expects
export PATH="/d/MinW64-15.2.0/bin:$PATH"; export MSYS2_ARG_CONV_EXCL='*'
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1; sleep 1
rm -rf build/cl14 && mkdir -p build/cl14
./build/kdbsvr.exe init "disk://$ROOT/build/cl14/a" >/dev/null
(./build/kdbsvr.exe server 1 8501 8502 "disk://$ROOT/build/cl14/a" "1@127.0.0.1:8501:8502" > build/cl14/a.log 2>&1 &)
sleep 6
echo "--- is the server up? ---"; timeout 30 ./build/kdbctl.exe 127.0.0.1:8501 STATS 2>&1 | head -3
echo "--- instrumented one-shot ---"; timeout 30 ./build/kdbsvr.exe 2>/dev/null | head -0; timeout 40 ./build/kdbctl.exe 127.0.0.1:8501 SET __p__ v1 2>&1 | head -8
echo "--- plain REPL via pipe for comparison (known: no output) ---"; printf 'GET __p__\n' | timeout 30 ./build/kdbctl.exe 127.0.0.1:8501 2>&1 | head -4
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
echo CL14_DONE
