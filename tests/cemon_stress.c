/* tests/cemon_stress.c -- cemon long-run drift + idle-CPU stress test.

   A. long run: a windowed producer posts for 60s while the loop drains; every
      5s segment prints the latency median and the process RSS, so drift or a
      leak shows up as a trend across segments.
   B. idle CPU: with no producer, run poll(10) for 5s and report process CPU%
      -- the direct evidence that the event-driven loop idles instead of
      spinning.

   C89 + runtime.h (worker thread).  build: ./build.sh cemon-stress */

#define CEMON_IMPLEMENTATION
#include "../code/cemon.h"
#define RUNTIME_IMPLEMENTATION
#include "../code/runtime.h"
#include "../code/kbase.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../tools/bench_env.h"

#ifdef _WIN32
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif
#define STRESS_YIELD() SwitchToThread()
#else
#include <sched.h>
#define STRESS_YIELD() sched_yield()
#endif

#define SEG_SECONDS  5
#define SEG_COUNT    12
#define SEG_CAP      20000
#define IDLE_SECONDS 5

static k_u64 stress_now(void){ k_u64 t=0; k_monotonic_us(&t); return t; }

#ifdef _WIN32
static k_u64 stress_rss(void){
  PROCESS_MEMORY_COUNTERS pmc;
  if(!GetProcessMemoryInfo(GetCurrentProcess(),&pmc,sizeof(pmc))) return 0;
  return (k_u64)pmc.WorkingSetSize;
}
static k_u64 stress_cpu_us(void){
  FILETIME ct,et,kt,ut;
  ULARGE_INTEGER k,u;
  if(!GetProcessTimes(GetCurrentProcess(),&ct,&et,&kt,&ut)) return 0;
  k.LowPart=kt.dwLowDateTime; k.HighPart=kt.dwHighDateTime;
  u.LowPart=ut.dwLowDateTime; u.HighPart=ut.dwHighDateTime;
  return (k_u64)((k.QuadPart+u.QuadPart)/10u);   /* 100ns -> us */
}
#else
static k_u64 stress_rss(void){ return 0; }
static k_u64 stress_cpu_us(void){ return 0; }
#endif

typedef struct{
  k_u64 post_us;
  void *ctx;
} lat_node;

typedef struct{
  cemon *loop;
  volatile int posted;
  volatile int handled;
  int window;
  volatile int stop;
  k_u64 *samples;
  volatile int n;
  int cap;
} stress_ctx;

static int stress_cmp(const void *a,const void *b){
  k_u64 x=*(const k_u64 *)a,y=*(const k_u64 *)b;
  return x<y?-1:(x>y?1:0);
}

static k_u64 stress_median(k_u64 *s,int n){
  if(n<=0) return 0;
  qsort(s,(size_t)n,sizeof(s[0]),stress_cmp);
  return s[n/2];
}

static void stress_cb(cemon *loop,void *ud){
  lat_node *node=(lat_node *)ud;
  stress_ctx *ctx=(stress_ctx *)node->ctx;
  (void)loop;
  if(ctx->n<ctx->cap){
    k_u64 now;
    k_monotonic_us(&now);
    ctx->samples[ctx->n]=now>node->post_us?now-node->post_us:0u;
    ctx->n++;
  }
  ctx->handled++;
  free(node);
}

static void stress_worker(runtime_ctx *rt,void *arg){
  stress_ctx *ctx=(stress_ctx *)arg;
  runtime_worker_ready(rt);
  while(!runtime_should_stop(rt)&&!ctx->stop){
    lat_node *node;
    if(ctx->posted-ctx->handled>=ctx->window){ STRESS_YIELD(); continue; }
    node=(lat_node *)malloc(sizeof(*node));
    if(!node) break;
    node->ctx=ctx;
    node->post_us=stress_now();
    if(cemon_post(ctx->loop,stress_cb,node)!=0){ free(node); continue; }
    ctx->posted++;
  }
  runtime_worker_exit(rt);
}

int main(void){
  char params[160];

  cemon *loop;
  runtime_ctx *rt;
  stress_ctx ctx;
  int seg;
  sprintf(params,"segments=see source window=64 SEG_CAP=see source (long-running stress, ~65 s)");
  bench_env_banner("cemon_stress",params);

  /* ---- A. long run ---- */
  memset(&ctx,0,sizeof(ctx));
  ctx.window=64;
  ctx.cap=SEG_CAP;
  ctx.samples=(k_u64 *)malloc((size_t)SEG_CAP*sizeof(k_u64));
  loop=cemon_create();
  if(!loop||!ctx.samples){ printf("setup failed\n"); return 1; }
  ctx.loop=loop;
  rt=runtime_create("thread",1,stress_worker,&ctx);
  if(!rt){ printf("runtime_create failed\n"); return 1; }
  runtime_wait_workers_ready(rt);
  printf("long run: %d segments x %ds, window=%d\n",SEG_COUNT,SEG_SECONDS,ctx.window);
  printf("%-6s %-14s %-10s %-12s\n","seg","median(us)","samples","rss(KB)");
  for(seg=0;seg<SEG_COUNT;seg++){
    k_u64 seg_end=stress_now()+(k_u64)SEG_SECONDS*1000000u;
    k_u64 med;
    ctx.n=0;
    while(stress_now()<seg_end) cemon_poll(loop,10);
    med=stress_median(ctx.samples,ctx.n);
    printf("%-6d %-14llu %-10d %-12llu\n",seg,(unsigned long long)med,ctx.n,
           (unsigned long long)(stress_rss()/1024u));
  }
  ctx.stop=1;
  runtime_stop(rt);
  runtime_wait_workers_exit(rt);
  runtime_destroy(rt);
  free(ctx.samples);

  /* ---- B. idle CPU (no producer) ---- */
  {
    k_u64 w0,w1,c0,c1;
    w0=stress_now(); c0=stress_cpu_us();
    while(stress_now()-w0<(k_u64)IDLE_SECONDS*1000000u) cemon_poll(loop,10);
    w1=stress_now(); c1=stress_cpu_us();
    printf("idle poll(10) %ds: wall=%llu us  cpu=%llu us  cpu%%=%llu\n",
           IDLE_SECONDS,(unsigned long long)(w1-w0),(unsigned long long)(c1-c0),
           (w1>w0)?(unsigned long long)((c1-c0)*100u/(w1-w0)):0u);
  }

  cemon_destroy(loop);
  return 0;
}
