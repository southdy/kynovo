/* tools/bench_env.h -- environment banner for the benchmark tools.

   Prints what a latency/throughput number must be read against: the machine, the
   compiler, the actual build flags, the OS, the measured timer granularity and
   the tool's own parameters.  Without this a benchmark number is not comparable
   to anything and cannot be re-derived later.

   Self-sufficient for its own needs (kbase.h + stdio/stdlib/string), so it can be
   included before or after the tool's other headers.  C89, MSVC 6.0. */

#ifndef BENCH_ENV_H
#define BENCH_ENV_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../code/kbase.h"

#ifndef BENCH_CFLAGS
#define BENCH_CFLAGS "(not recorded: build without -DBENCH_CFLAGS)"
#endif

/* Effective Sleep(1) granularity: the timer resolution the process actually sees
   (timeBeginPeriod from any process is process/system wide, so this can differ
   from the 15.6 ms default).  A benchmark that waits on timers must report it. */
static k_u64 bench_timer_granularity_us(void){
#if defined(_WIN32)
  /* Sleep/GetVersion/GetNativeSystemInfo: kbase.h pulls in <windows.h> on WIN32 */

  k_u64 t0,t1,best=0;
  int i;
  for(i=0;i<20;i++){
    k_monotonic_us(&t0);
    Sleep(1);
    k_monotonic_us(&t1);
    if(t1>t0&&(best==0||t1-t0<best)) best=t1-t0;
  }
  return best;
#else
  return 0;
#endif
}

static void bench_env_banner(const char *tool,const char *params){
#if defined(_WIN32)
  const char *ident,*ncpu;
  SYSTEM_INFO si;
  ident=getenv("PROCESSOR_IDENTIFIER");
  ncpu=getenv("NUMBER_OF_PROCESSORS");
  memset(&si,0,sizeof(si));
  GetNativeSystemInfo(&si);
  printf("== %s: env ==\n",tool?tool:"bench");
  printf("   cpu      : %s / %s logical\n",ident?ident:"?",ncpu?ncpu:"?");
#if defined(_MSC_VER)
  printf("   compiler : MSVC _MSC_VER=%d\n",_MSC_VER);
#else
  printf("   compiler : gcc %s\n",__VERSION__);
#endif
  printf("   build    : %s\n",BENCH_CFLAGS);
  printf("   os       : windows %lu.%lu (arch %lu)\n",
         (unsigned long)(GetVersion()&0xffu),(unsigned long)((GetVersion()>>8)&0xffu),
         (unsigned long)si.wProcessorArchitecture);
  printf("   timer    : Sleep(1) granularity = %" K_U64_FMT " us (measured)\n",bench_timer_granularity_us());
  printf("   params   : %s\n",params?params:"(none)");
#else
  printf("== %s: env ==\n",tool?tool:"bench");
  printf("   compiler : gcc %s\n",__VERSION__);
  printf("   build    : %s\n",BENCH_CFLAGS);
  printf("   params   : %s\n",params?params:"(none)");
#endif
}

#endif /* BENCH_ENV_H */
