/* tools/bench_mt.c -- multi-client concurrent SET throughput.  Spawns N
   pthreads, each running its own synchronous client, so N writes are in flight at
   once.  Reports throughput for N in {1,2,4,8,16} to expose the batched-flush
   ceiling (flush_item_limit path) that a single serial client can never reach.

   Two things the old version got wrong and this one fixes:
     - each op built its own connection, so the inverse of the reported number was
       not the store's cost;
     - the throughput denominator was the SLOWEST worker's self-timed span, which
       is an upper bound, not a measurement.  The denominator is now the wall time
       of the whole measured phase (all workers released together by a spin
       barrier), and the old figure is still printed as the upper bound.

   C89 + pthread.
   build: ./build.sh bench-mt
   run:   bench_mt.exe <host:port> [M] [--connect-per-op]
          M = SETs per worker (default 1000) */

#include <pthread.h>
#define VFS_IMPLEMENTATION
#include "../code/vfs.h"
#define TREAP_IMPLEMENTATION
#include "../code/treap.h"
#define RUNTIME_IMPLEMENTATION
#include "../code/runtime.h"
#define CEMON_IMPLEMENTATION
#include "../code/cemon.h"
#define RAFT_IMPLEMENTATION
#include "../code/raft.h"
#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#if defined(_WIN32)
#include <windows.h>
#endif
#else
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <time.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../code/kbase.h"
#include "../code/kproto.h"
#include "../code/kserver.h"
#include "../code/kclient.h"
#include "bench_client.h"

#define BENCH_MT_KEY "__bench__"
#define BENCH_MT_MAX_CLIENTS 128

typedef struct bench_mt_arg{
  const char *host;
  unsigned short port;
  int n;                 /* ops for this worker; set to -1 on failure */
  int connect_per_op;    /* 1 = legacy shape: connect per op */
  volatile int *start;   /* released by main once every worker is at the barrier */
  volatile int *arrived; /* incremented by each worker before it waits */
  k_u64 total_us;        /* this worker's own span (upper-bound material only) */
  k_u64 end_us;          /* absolute end on the shared monotonic clock */
} bench_mt_arg;

static void *bench_mt_worker(void *p){
  bench_mt_arg *a=(bench_mt_arg *)p;
  k_response_data resp;
  k_bench_conn conn;
  k_u64 start,end;
  int i,rc;
  if(!a->connect_per_op){
    if(k_conn_open(&conn,a->host,a->port,5000)!=0){
      (*a->arrived)++;
      a->n=-1;
      return 0;
    }
  }
  (*a->arrived)++;
  while(!*a->start){}   /* barrier: the phase clock starts when main releases us */
  if(k_monotonic_us(&start)!=0) start=0;
  for(i=0;i<a->n;i++){
    memset(&resp,0,sizeof(resp));
    if(a->connect_per_op) rc=k_sync_call(a->host,a->port,K_REQ_SET,BENCH_MT_KEY,K_LEN(BENCH_MT_KEY),"v",1u,5000,&resp);
    else rc=k_conn_call(&conn,K_REQ_SET,BENCH_MT_KEY,K_LEN(BENCH_MT_KEY),"v",1u,5000,&resp);
    if(rc!=0){
      /* one failed op: abort this worker, the throughput line is invalid */
      if(!a->connect_per_op) k_conn_close(&conn);
      k_response_data_free(&resp);
      a->n=-1;
      return 0;
    }
    k_response_data_free(&resp);
  }
  k_monotonic_us(&end);
  a->end_us=end;
  a->total_us=end>start?end-start:0u;
  if(!a->connect_per_op) k_conn_close(&conn);
  return 0;
}

/* Parse a comma list of client counts ("1,2,4,8,16,32,64"): with a synchronous
   client per connection, each client sends its next write only after its previous
   reply, so a client count that is too small cannot keep writes arriving inside the
   server's flush window (flush_timeout_ms) and the group commit never fills.  The
   list is a load-shape parameter, so it must be adjustable. */
static int bench_parse_counts(const char *arg,int *out,int max){
  const char *p=arg;
  int n=0,v,digits;
  while(*p&&n<max){
    v=0; digits=0;
    while(*p>='0'&&*p<='9'){ v=v*10+(*p-'0'); p++; digits++; }
    if(!digits) return -1;
    if(v<1||v>BENCH_MT_MAX_CLIENTS) return -1;
    out[n++]=v;
    if(*p==','){ p++; continue; }
    if(*p==0) break;
    return -1;
  }
  return n;
}
int main(int argc,char **argv){
  int counts[BENCH_MT_MAX_CLIENTS];
  int count_n=5;
  char counts_text[192];
  bench_mt_arg args[BENCH_MT_MAX_CLIENTS];
  pthread_t tid[BENCH_MT_MAX_CLIENTS];
  char *host,*colon;
  char params[320];
  unsigned short port;
  int m,ci,i,ok,connect_per_op;
  volatile int start_flag,arrived;
  k_u64 slowest,total_ops,phase_start,phase_end,measured_us,wall_us,aggregate,upper;
  if(argc<2){ printf("usage: bench_mt <host:port> [M] [--connect-per-op] [clients e.g. 1,2,4,8,16,32,64]\n"); return 1; }
  colon=strchr(argv[1],':');
  if(!colon){ printf("bad host:port\n"); return 1; }
  host=argv[1];
  *colon='\0';
  port=(unsigned short)atoi(colon+1);
  connect_per_op=0;
  m=1000;
  counts[0]=1; counts[1]=2; counts[2]=4; counts[3]=8; counts[4]=16;
  for(i=2;i<argc;i++){
    if(strcmp(argv[i],"--connect-per-op")==0) connect_per_op=1;
    else if(strchr(argv[i],',')!=0){
      int parsed=bench_parse_counts(argv[i],counts,BENCH_MT_MAX_CLIENTS);
      if(parsed<=0){ printf("bad client-count list '%s'\n",argv[i]); return 1; }
      count_n=parsed;
    }
    else if(atoi(argv[i])>0) m=atoi(argv[i]);
  }
  counts_text[0]='\0';
  for(i=0;i<count_n;i++){
    char one[16];
    sprintf(one,"%s%d",i?",":"",counts[i]);
    if(strlen(counts_text)+strlen(one)<sizeof(counts_text)-1) strcat(counts_text,one);
  }
  sprintf(params,"host=%.80s:%u M=%d N={%s} connection=%s",host,(unsigned)port,m,counts_text,
          connect_per_op?"connect-per-op":"persistent");
  bench_env_banner("bench_mt",params);
  printf("\nthroughput denominator = wall time of the released phase (all workers start together)\n");
  printf("upper-bound column      = N*M / slowest worker's own span (what the old version printed)\n\n");
  for(ci=0;ci<count_n;ci++){
    int nc=counts[ci];
    printf("-- round N=%d starting\n",nc);
    fflush(stdout);
    /* warm-up: one SET so first-connect cost is excluded */
    {
      k_response_data r;
      memset(&r,0,sizeof(r));
      if(k_sync_call(host,port,K_REQ_SET,BENCH_MT_KEY,K_LEN(BENCH_MT_KEY),"v",1u,5000,&r)==0) k_response_data_free(&r);
    }
    start_flag=0;
    arrived=0;
    for(i=0;i<nc;i++){
      args[i].host=host;
      args[i].port=port;
      args[i].n=m;
      args[i].connect_per_op=connect_per_op;
      args[i].start=&start_flag;
      args[i].arrived=&arrived;
      args[i].total_us=0u;
      args[i].end_us=0u;
      if(pthread_create(&tid[i],0,bench_mt_worker,&args[i])!=0){ printf("pthread_create failed\n"); return 1; }
    }
    /* wait until every worker has connected (or failed) before starting the clock */
    while(arrived<nc){}
    printf("   round N=%d: all workers connected\n",nc);
    fflush(stdout);
    k_monotonic_us(&phase_start);
    start_flag=1;
    ok=1;
    slowest=0u;
    for(i=0;i<nc;i++){
      pthread_join(tid[i],0);
      if(args[i].n<0) ok=0;
      if(args[i].total_us>slowest) slowest=args[i].total_us;
    }
    k_monotonic_us(&phase_end);
    wall_us=phase_end>phase_start?phase_end-phase_start:0u;
    /* end the measured phase at the last worker's own end, so connection teardown
       is not charged to the throughput */
    measured_us=0u;
    for(i=0;i<nc;i++)
      if(args[i].end_us>phase_start&&args[i].end_us-phase_start>measured_us)
        measured_us=args[i].end_us-phase_start;
    if(!measured_us) measured_us=wall_us;
    total_ops=(k_u64)nc*(k_u64)m;
    aggregate=measured_us?((total_ops*(k_u64)1000000u)/measured_us):0u;
    upper=slowest?((total_ops*(k_u64)1000000u)/slowest):0u;
    if(!ok) printf("N=%-3d: some worker failed\n",nc);
    else printf("N=%-3d: measured=%7" K_U64_FMT " ms (wall=%" K_U64_FMT " ms)  aggregate=%8" K_U64_FMT " ops/s  (upper-bound=%8" K_U64_FMT " ops/s, slowest=%" K_U64_FMT " ms)\n",
                nc,measured_us/1000u,wall_us/1000u,aggregate,upper,slowest/1000u);
  }
  return 0;
}
