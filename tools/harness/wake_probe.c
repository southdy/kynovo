/* wake_probe.c -- measure the threading substrate the WAL path rides on, with NO storage involved.
   This is the control for the engine's handoff numbers: same runtime seam (task queue + result
   queue), same one-worker shape as the WAL worker, but the "work" is a spin of a known length.

   usage: wake_probe <empty|busy> <rounds> [busy_us]
     empty   the worker returns the task at once
     busy    the worker spins busy_us (default 200) before returning it - emulates the fsync

   Per round it posts one task and waits for its result, recording three gaps in microseconds:
     handoff    main posted -> the worker picked the task up   (compare STATS handoff_us_max)
     wake       worker posted the result -> main saw it        (compare STATS wake_pre_us_max)
     round      main posted -> main saw the result             (handoff + wake + work)
   Verdict lines: WAKEPROBE|ok ... plus one line per gap with min/p50/p99/max.  A run that records no
   round prints WAKEPROBE|fail: zero rounds must never be read as zero latency.

   Build (POSIX needs -pthread):
     gcc -std=c89 -O2 -Wall -Wextra -o build/wake_probe.exe tools/harness/wake_probe.c -pthread
*/
#define KBASE_IMPLEMENTATION
#include "../../code/kbase.h"
#define RUNTIME_IMPLEMENTATION
#include "../../code/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAKEPROBE_MAX 20000
#define WAKEPROBE_SLOW_US 1000u

static k_u64 g_pick_us[WAKEPROBE_MAX];
static k_u64 g_wake_us[WAKEPROBE_MAX];
static k_u64 g_round_us[WAKEPROBE_MAX];
static k_u64 g_posted_us;      /* worker writes; main reads it after seeing the matching result */
static int g_busy_us;
static int g_slow_handoff;
static int g_slow_wake;

static k_u64 probe_now(void){
  k_u64 t=0;
  if(k_monotonic_us(&t)!=0) return 0;
  return t;
}
static int cmp_u64(const void *a,const void *b){
  k_u64 x=*(const k_u64 *)a,y=*(const k_u64 *)b;
  return x<y?-1:(x>y?1:0);
}
static void worker_entry(runtime_ctx *rt,void *arg){
  runtime_fn fn;
  void *task;
  k_u64 spin_until;
  (void)arg;
  runtime_worker_ready(rt);
  while(!runtime_should_stop(rt)){
    fn=0;
    task=0;
    if(runtime_task_poll(rt,50,&fn,&task)<0) break;
    if(!task) continue;
    if(g_busy_us>0){
      spin_until=probe_now()+(k_u64)g_busy_us;
      while(probe_now()<spin_until){}
    }
    g_posted_us=probe_now();
    if(runtime_result_post(rt,0,task)!=0) break;
  }
  runtime_worker_exit(rt);
}
static void report(const char *what,const k_u64 *v,int n,int slow){
  printf("WAKEPROBE|%-7s min=%" K_U64_FMT " p50=%" K_U64_FMT " p90=%" K_U64_FMT " p99=%" K_U64_FMT
         " max=%" K_U64_FMT " slow_over_1ms=%d\n",
         what,(k_u64)v[0],(k_u64)v[n/2],(k_u64)v[(int)((k_u64)n*90u/100u)],
         (k_u64)v[(int)((k_u64)n*99u/100u)],(k_u64)v[n-1],slow);
}
int main(int argc,char **argv){
  runtime_ctx *rt;
  runtime_fn fn;
  void *task,*got;
  const char *mode;
  int rounds,i,n;
  k_u64 t0,t1,pick,wake,round;
  if(argc<3){
    printf("usage: wake_probe <empty|busy> <rounds> [busy_us]\n");
    return 2;
  }
  mode=argv[1];
  rounds=atoi(argv[2]);
  g_busy_us=(argc>=4)?atoi(argv[3]):200;
  if(strcmp(mode,"busy")!=0) g_busy_us=0;
  if(rounds<1||rounds>WAKEPROBE_MAX){
    printf("wake_probe: rounds must be 1..%d\n",WAKEPROBE_MAX);
    return 2;
  }
  rt=runtime_create("thread",1,worker_entry,0);   /* "thread" = real OS workers, as the WAL path uses */
  if(!rt){
    printf("WAKEPROBE|fail reason=cannot-create-runtime\n");
    return 1;
  }
  runtime_wait_workers_ready(rt);
  n=0;
  g_slow_handoff=0;
  g_slow_wake=0;
  for(i=0;i<rounds;i++){
    task=(void *)K_MALLOC(1);
    if(!task) break;
    g_posted_us=0;
    t0=probe_now();
    if(runtime_task_post(rt,0,task)!=0){
      K_FREE(task);
      break;
    }
    got=0;
    while(!got){
      fn=0;
      if(runtime_result_poll(rt,50,&fn,&got)<0) break;
      if(got&&got!=task){ K_FREE(got); got=0; break; }   /* nothing else posts here; be safe anyway */
    }
    t1=probe_now();
    if(!got||!g_posted_us||!t0||!t1){
      printf("WAKEPROBE|fail reason=round-incomplete at round %d\n",i);
      return 1;
    }
    pick=(g_posted_us>=t0)?(g_posted_us-t0):0u;
    wake=(t1>=g_posted_us)?(t1-g_posted_us):0u;
    round=t1-t0;
    g_pick_us[n]=pick;
    g_wake_us[n]=wake;
    g_round_us[n]=round;
    if(pick>WAKEPROBE_SLOW_US) g_slow_handoff++;
    if(wake>WAKEPROBE_SLOW_US) g_slow_wake++;
    n++;
    K_FREE(task);
  }
  runtime_stop(rt);
  runtime_wait_workers_exit(rt);
  runtime_destroy(rt);
  if(n<1){
    printf("WAKEPROBE|fail reason=no-rounds\n");
    return 1;
  }
  qsort(g_pick_us,n,sizeof(k_u64),cmp_u64);
  qsort(g_wake_us,n,sizeof(k_u64),cmp_u64);
  qsort(g_round_us,n,sizeof(k_u64),cmp_u64);
  printf("WAKEPROBE|ok mode=%s busy_us=%d rounds=%d\n",mode,g_busy_us,n);
  report("handoff",g_pick_us,n,g_slow_handoff);
  report("wake",g_wake_us,n,g_slow_wake);
  report("round",g_round_us,n,0);
  return 0;
}
