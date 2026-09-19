#!/usr/bin/env bash
# =============================================================================
# kynovo -- build script (single-translation-unit C89 Raft ordered KV)
#
# All artifacts go to ./build (not %TEMP%). Reason: unsigned MinGW binaries in
# %TEMP% are the top trigger for heuristic false positives by security software
# (Huorong/OneSEC/Defender) -- "unsigned + networking + in Temp" looks like a
# trojan drop. Keeping artifacts in a predictable in-repo build/ directory
# greatly reduces false positives. For extra safety, add
#     D:\kynovo\build
# to the trust list in Huorong/OneSEC (that directory is rebuilt by
# `./build.sh clean`, so excluding it is safe).
#
# This script only invokes gcc: no downloads, no network, no obfuscation --
# it is not itself recognized as a threat.
#
# Usage:
#   ./build.sh                       # build all nine targets
#   ./build.sh kdbsvr             # server application binary
#   ./build.sh kdbctl                # CLI application binary
#   ./build.sh selftest              # single-process self-test binary
#   ./build.sh bench                 # build + run the fsync microbenchmark
#   ./build.sh bench-e2e             # single-client end-to-end benchmark (build only)
#   ./build.sh bench-mt              # multi-threaded concurrent benchmark (build only)
#   ./build.sh bench-persist         # persistent-connection SET latency (build only)
#   ./build.sh cemon-bench           # build + run the cemon/runtime layered benchmark
#   ./build.sh cemon-stress          # build + run cemon long-run + idle-CPU stress (~70s)
#   ./build.sh test                  # raft_test driver only
#   ./build.sh fuzz                  # raft_fuzz driver only
#   ./build.sh cfuzz                 # raft_cluster_fuzz driver only
#   ./build.sh kclient               # kclient_test driver only
#   ./build.sh kserver               # kserver_test driver only
#   ./build.sh kclusterfuzz          # kserver_cluster_fuzz driver only
#   ./build.sh run-test              # build raft_test + run it (218 cases)
#   ./build.sh run-fuzz [seed [n]]   # build raft_fuzz + run n iterations from seed
#   ./build.sh run-cfuzz [seed [n]]  # build raft_cluster_fuzz + run n iterations
#   ./build.sh run-kclient           # build kclient_test + run it
#   ./build.sh run-kserver           # build kserver_test + run it
#   ./build.sh run-kclusterfuzz [seed [n]]  # build kserver_cluster_fuzz + run n iterations
#   ./build.sh run-selftest          # build selftest + run it
#   ./build.sh coverage              # build+run all drivers, aggregate gcov line+branch report
#   ./build.sh sanitize              # build the six test drivers with UBSan traps
#   ./build.sh run-sanitize          # build + run them (UB -> SIGILL, last seed reproduces)
#   ./build.sh clean                 # remove build/
# =============================================================================
set -u
# Toolchain: overridable, but the default is the pinned MinGW-w64 15.2.0 (see the
# PATH note below).  The version is checked because an older gcc builds raft.h/
# cemon.h against headers that lack LPFN_ACCEPTEX (a confusing late compile error).
MINGW_BIN="${MINGW_BIN:-/d/MinW64-15.2.0/bin}"
BUILD_DIR="${BUILD_DIR:-build}"
# the flag string the benchmark banners print (keep in sync with the gcc lines below)
BENCH_CFLAGS="-std=c89 -O2 -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function"
RC=0

# Force MinGW-w64 15.2.0 first on PATH. Prepend unconditionally: a first-chance
# `command -v gcc` would accept a stray gcc already on PATH (e.g. the older
# /d/MinGW 9.2.0), which then breaks the build (its mswsock.h lacks LPFN_ACCEPTEX).
export PATH="$MINGW_BIN:$PATH"
if ! command -v gcc >/dev/null 2>&1; then
    echo "error: gcc not found under $MINGW_BIN" >&2
    exit 1
fi
GCC_VERSION="$(gcc -dumpversion 2>/dev/null || echo unknown)"
if [ "$GCC_VERSION" != "15.2.0" ]; then
    echo "warning: expected gcc 15.2.0 under $MINGW_BIN, found $GCC_VERSION;" \
         "older mingw headers may lack LPFN_ACCEPTEX" >&2
fi

mkdir -p "$BUILD_DIR"

# Test drivers are built at the SAME optimization level as the shipped binary
# (-O2) and with full warnings (-Wall -Wextra): tests must be at least as strict
# as production, never looser. (-w hides the test code's own warnings and -O1
# tests a different code path than the shipped -O2.)
# -Wno-unused-function: the single-header libs (raft.h) and the test.h framework
# expose helpers a given driver does not call (e.g. raft_inspect, unused
# _test_fail_* assert printers). That is an unused-public-API false positive, not
# dead code; every OTHER warning stays on.
# -Wdeclaration-after-statement: MSVC 6.0's strict C89 compiler rejects a
# declaration after a statement; gcc -std=c89 accepts it silently unless this
# flag is on.  Catches a whole class of MSVC-6.0-only breakage that a plain
# -Wall -Wextra build misses.
build_kdbsvr() {
    gcc -std=c89 -O2 -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function -pthread code/kdbsvr.c -o "$BUILD_DIR/kdbsvr.exe" -lws2_32 -lwinmm
}
build_kdbctl() {
    gcc -std=c89 -O2 -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function -pthread code/kdbctl.c -o "$BUILD_DIR/kdbctl.exe" -lws2_32
}
build_selftest() {
    gcc -std=c89 -O2 -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function -pthread tests/selftest.c -o "$BUILD_DIR/selftest.exe" -lws2_32
}
build_bench() {
    gcc -std=c89 -O2 -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function -DBENCH_CFLAGS="\"$BENCH_CFLAGS\"" tools/bench_fsync.c -o "$BUILD_DIR/bench_fsync.exe"
}
build_bench_e2e() {
    gcc -std=c89 -O2 -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function -pthread -DBENCH_CFLAGS="\"$BENCH_CFLAGS\"" tools/bench_e2e.c -o "$BUILD_DIR/bench_e2e.exe" -lws2_32
}
build_bench_mt() {
    gcc -std=c89 -O2 -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function -pthread -DBENCH_CFLAGS="\"$BENCH_CFLAGS\"" tools/bench_mt.c -o "$BUILD_DIR/bench_mt.exe" -lws2_32
}
build_bench_rate() {
    gcc -std=c89 -O2 -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function -pthread -DBENCH_CFLAGS="\"$BENCH_CFLAGS\"" tools/bench_rate.c -o "$BUILD_DIR/bench_rate.exe" -lws2_32
}
build_bench_pipe() {
    gcc -std=c89 -O2 -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function -pthread -DBENCH_CFLAGS="\"$BENCH_CFLAGS\"" tools/bench_pipe.c -o "$BUILD_DIR/bench_pipe.exe" -lws2_32
}
build_cemon_bench() {
    gcc -std=c89 -O2 -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function -pthread -DBENCH_CFLAGS="\"$BENCH_CFLAGS\"" tests/cemon_bench.c -o "$BUILD_DIR/cemon_bench.exe" -lws2_32 -lwinmm
}
build_bench_persist() {
    gcc -std=c89 -O2 -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function -Wno-unused-variable -pthread -DBENCH_CFLAGS="\"$BENCH_CFLAGS\"" tests/bench_persist.c -o "$BUILD_DIR/bench_persist.exe" -lws2_32 -lwinmm
}
build_cemon_stress() {
    gcc -std=c89 -O2 -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function -pthread -DBENCH_CFLAGS="\"$BENCH_CFLAGS\"" tests/cemon_stress.c -o "$BUILD_DIR/cemon_stress.exe" -lws2_32 -lpsapi
}
build_test() {
    gcc -std=c89 -O2 -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function tests/raft_test.c -o "$BUILD_DIR/raft_test.exe"
}
build_fuzz() {
    gcc -std=c89 -O2 -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function tests/raft_fuzz.c -o "$BUILD_DIR/raft_fuzz.exe"
}
build_cfuzz() {
    gcc -std=c89 -O2 -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function tests/raft_cluster_fuzz.c -o "$BUILD_DIR/raft_cluster_fuzz.exe"
}
build_kclient() {
    gcc -std=c89 -O2 -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function tests/kclient_test.c -o "$BUILD_DIR/kclient_test.exe"
}
build_kserver() {
    gcc -std=c89 -O2 -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function tests/kserver_test.c -pthread -o "$BUILD_DIR/kserver_test.exe"
}
build_cemon_test() {
    gcc -std=c89 -O2 -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function -pthread tests/cemon_test.c -o "$BUILD_DIR/cemon_test.exe" -lws2_32
}
build_kclusterfuzz() {
    gcc -std=c89 -O2 -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function tests/kserver_cluster_fuzz.c -o "$BUILD_DIR/kserver_cluster_fuzz.exe"
}
# Undefined-behaviour sanitizer. MinGW-w64 ships no libasan/libubsan runtime, so
# the runtime-linked ASan+UBSan is unavailable here (that needs Linux/clang, per
# the fuzz file headers). UBSan TRAP mode needs no runtime: it emits inline traps
# instead of runtime calls, so UB -> SIGILL (exit 132) and the last printed seed
# reproduces the crash.
build_san() {
    gcc -std=c89 -fsanitize=undefined -fsanitize-trap=undefined -O1 -g tests/raft_test.c          -o "$BUILD_DIR/raft_test_san.exe" \
    && gcc -std=c89 -fsanitize=undefined -fsanitize-trap=undefined -O1 -g tests/raft_fuzz.c          -o "$BUILD_DIR/raft_fuzz_san.exe" \
    && gcc -std=c89 -fsanitize=undefined -fsanitize-trap=undefined -O1 -g tests/raft_cluster_fuzz.c  -o "$BUILD_DIR/raft_cluster_fuzz_san.exe" \
    && gcc -std=c89 -fsanitize=undefined -fsanitize-trap=undefined -O1 -g tests/kclient_test.c         -o "$BUILD_DIR/kclient_test_san.exe" \
    && gcc -std=c89 -fsanitize=undefined -fsanitize-trap=undefined -O1 -g tests/kserver_test.c         -o "$BUILD_DIR/kserver_test_san.exe" \
    && gcc -std=c89 -fsanitize=undefined -fsanitize-trap=undefined -O1 -g tests/kserver_cluster_fuzz.c -o "$BUILD_DIR/kserver_cluster_fuzz_san.exe" \
    && gcc -std=c89 -fsanitize=undefined -fsanitize-trap=undefined -O1 -g -pthread tests/cemon_test.c -o "$BUILD_DIR/cemon_test_san.exe" -lws2_32
}
# Code coverage (gcov).  Compiles every driver with --coverage, runs them, then
# gcov each and aggregates the per-driver .gcov reports into per-file
# line+branch coverage.  lcov/genhtml are unavailable on MinGW, so aggregation
# is a tiny stdlib Python script (tools/cov_report.py) that takes the UNION of
# coverage across drivers (a line/branch is covered if ANY driver hit it).
# Reports "taken at least once" as the branch metric (the strict gcov branch
# number), which is the figure that exposes untested rare paths.
build_coverage() {
    rm -rf "$BUILD_DIR/cov"
    mkdir -p "$BUILD_DIR/cov"
    gcc -std=c89 -O0 -g --coverage -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function -pthread tests/selftest.c -o "$BUILD_DIR/cov/selftest.exe" -lws2_32 || RC=1
    gcc -std=c89 -O0 -g --coverage -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function tests/raft_test.c -o "$BUILD_DIR/cov/raft_test.exe" || RC=1
    gcc -std=c89 -O0 -g --coverage -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function tests/raft_fuzz.c -o "$BUILD_DIR/cov/raft_fuzz.exe" || RC=1
    gcc -std=c89 -O0 -g --coverage -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function tests/raft_cluster_fuzz.c -o "$BUILD_DIR/cov/raft_cluster_fuzz.exe" || RC=1
    gcc -std=c89 -O0 -g --coverage -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function tests/kclient_test.c -o "$BUILD_DIR/cov/kclient_test.exe" || RC=1
    gcc -std=c89 -O0 -g --coverage -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function tests/kserver_test.c -o "$BUILD_DIR/cov/kserver_test.exe" || RC=1
    gcc -std=c89 -O0 -g --coverage -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function tests/kserver_cluster_fuzz.c -o "$BUILD_DIR/cov/kserver_cluster_fuzz.exe" || RC=1
    # cemon_bench is the second cemon.h driver (selftest alone left cemon.h at one
    # driver).  cemon_stress (~65s) stays out of the coverage run on purpose: it
    # has its own target and would dominate the turn-around; run `./build.sh
    # cemon-stress` when you want its cemon.h contribution.
    gcc -std=c89 -O0 -g --coverage -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function -pthread tests/cemon_bench.c -o "$BUILD_DIR/cov/cemon_bench.exe" -lws2_32 -lwinmm || RC=1
}
run_coverage_driver() {
    local name="$1"; shift
    local log="$BUILD_DIR/cov/logs/$name.log"
    if ( cd "$BUILD_DIR/cov" && "$@" ) >"$log" 2>&1; then
        echo "  pass $name"
        return 0
    fi
    echo "  FAIL $name  (exit $?; log: $log)" >&2
    return 1
}
run_coverage() {
    local d gcno
    mkdir -p "$BUILD_DIR/cov/logs"
    # Every driver keeps its OWN exit status.  The previous version ran all
    # seven inside one subshell, so only the LAST command's status was
    # observed: a failing driver (e.g. raft_cluster_fuzz) was hidden by a
    # later passing one while its output went to /dev/null, and the coverage
    # step reported success on a RED build.  Now each driver logs to its own
    # file, a failure is named, and the aggregation still runs (the numbers
    # are useful) but RC is set so the target cannot look green.
    run_coverage_driver selftest             ./selftest.exe               || RC=1
    run_coverage_driver raft_test            ./raft_test.exe              || RC=1
    run_coverage_driver raft_fuzz            ./raft_fuzz.exe 1 2000       || RC=1
    run_coverage_driver raft_cluster_fuzz    ./raft_cluster_fuzz.exe 1 5  || RC=1
    run_coverage_driver kclient_test         ./kclient_test.exe           || RC=1
    run_coverage_driver kserver_test         ./kserver_test.exe           || RC=1
    run_coverage_driver kserver_cluster_fuzz ./kserver_cluster_fuzz.exe 1 5 || RC=1
    run_coverage_driver cemon_bench        ./cemon_bench.exe            || RC=1
    rm -f ./*.gcov
    rm -rf "$BUILD_DIR/cov/gcov"
    mkdir -p "$BUILD_DIR/cov/gcov"
    for gcno in "$BUILD_DIR/cov"/*.gcno; do
        d="$(basename "$gcno" .gcno)"
        mkdir -p "$BUILD_DIR/cov/gcov/$d"
        # gcov runs from the repo ROOT so relative Source paths (code/raft.h)
        # resolve; -o points at the .gcno/.gcda directory (build/cov).
        gcov -b -c -o "$BUILD_DIR/cov" "$gcno" >/dev/null 2>&1
        mv ./*.gcov "$BUILD_DIR/cov/gcov/$d/" 2>/dev/null
    done
    python tools/cov_report.py "$BUILD_DIR/cov/gcov" || RC=1
    if [ "$RC" != 0 ]; then
        echo "coverage: ONE OR MORE DRIVERS FAILED -- the numbers above cover a RED build" >&2
        echo "coverage: per-driver output is in $BUILD_DIR/cov/logs/" >&2
    fi
}

build_all() {
    build_kdbsvr || RC=1
    build_kdbctl    || RC=1
    build_selftest  || RC=1
    build_test      || RC=1
    build_fuzz      || RC=1
    build_cfuzz     || RC=1
    build_kclient   || RC=1
    build_kserver   || RC=1
    build_kclusterfuzz || RC=1
    build_cemon_test || RC=1
    # Benchmarks are compiled here too (not just via their own targets): they must
    # be inside the -Wall -Wextra -Wdeclaration-after-statement gate, otherwise
    # they rot silently (they were in NO aggregation target before).
    build_bench || RC=1
    build_bench_e2e || RC=1
    build_bench_mt || RC=1
    build_bench_pipe || RC=1
    build_bench_rate || RC=1
    build_bench_persist || RC=1
    build_cemon_bench || RC=1
    build_cemon_stress || RC=1
}

# build/ is a PURE OUTPUT directory: nothing hand-written may live there (harnesses and records
# were moved to tools/harness/, tools/archive/ and doc/measurements/).  `clean` therefore removes it
# wholesale, exactly like a fresh checkout.
do_clean() {
    rm -rf "$BUILD_DIR"
    echo "removed $BUILD_DIR/ (pure output directory; tooling lives in tools/, records in doc/measurements/)"
}

case "${1:-all}" in
    all)          build_all ;;
    kdbsvr)    build_kdbsvr || RC=1 ;;
    kdbctl)       build_kdbctl || RC=1 ;;
    selftest)     build_selftest || RC=1 ;;
    bench)        build_bench && "$BUILD_DIR/bench_fsync.exe" || RC=1 ;;
    bench-e2e)    build_bench_e2e || RC=1 ;;
    bench-mt)     build_bench_mt || RC=1 ;;
    bench-pipe)   build_bench_pipe && "$BUILD_DIR/bench_pipe.exe" "${2:-127.0.0.1:19601}" "${3:-200}" || RC=1 ;;
    bench-rate)   build_bench_rate && "$BUILD_DIR/bench_rate.exe" "${2:-127.0.0.1:7001}" "${3:-2000}" "${4:-32}" "${5:-1}" "${6:-__rate__}" || RC=1 ;;
    bench-persist) build_bench_persist || RC=1 ;;
    cemon-bench)  build_cemon_bench && "$BUILD_DIR/cemon_bench.exe" || RC=1 ;;
    cemon-stress) build_cemon_stress && "$BUILD_DIR/cemon_stress.exe" || RC=1 ;;
    test)         build_test || RC=1 ;;
    fuzz)         build_fuzz || RC=1 ;;
    cfuzz)        build_cfuzz || RC=1 ;;
    kclient)      build_kclient || RC=1 ;;
    kserver)      build_kserver || RC=1 ;;
    kclusterfuzz) build_kclusterfuzz || RC=1 ;;
    cemon-test)   build_cemon_test || RC=1 ;;
    run-cemon-test) build_cemon_test && "$BUILD_DIR/cemon_test.exe" || RC=1 ;;
    run-test)     build_test  && "$BUILD_DIR/raft_test.exe" || RC=1 ;;
    run-fuzz)     build_fuzz  && "$BUILD_DIR/raft_fuzz.exe" "${2:-1}" "${3:-2000}" || RC=1 ;;
    run-cfuzz)    build_cfuzz && "$BUILD_DIR/raft_cluster_fuzz.exe" "${2:-1}" "${3:-1000}" || RC=1 ;;
    run-kclient)  build_kclient && "$BUILD_DIR/kclient_test.exe" || RC=1 ;;
    run-kserver)  build_kserver && "$BUILD_DIR/kserver_test.exe" || RC=1 ;;
    run-kclusterfuzz) build_kclusterfuzz && "$BUILD_DIR/kserver_cluster_fuzz.exe" "${2:-1}" "${3:-100}" || RC=1 ;;
    run-selftest) build_selftest && "$BUILD_DIR/selftest.exe" || RC=1 ;;
    coverage)     build_coverage && run_coverage || RC=1 ;;
    sanitize)     build_san || RC=1 ;;
    run-sanitize) build_san && "$BUILD_DIR/raft_test_san.exe" \
                              && "$BUILD_DIR/raft_fuzz_san.exe" "${2:-1}" "${3:-2000}" \
                              && "$BUILD_DIR/raft_cluster_fuzz_san.exe" "${2:-1}" "${3:-1000}" \
                              && "$BUILD_DIR/kclient_test_san.exe" \
                              && "$BUILD_DIR/kserver_test_san.exe" \
                              && "$BUILD_DIR/kserver_cluster_fuzz_san.exe" "${2:-1}" "${3:-100}" \
                              && "$BUILD_DIR/cemon_test_san.exe" || RC=1 ;;
    clean)        do_clean; exit 0 ;;
    *)            echo "unknown target: $1" >&2; exit 2 ;;
esac

exit $RC
