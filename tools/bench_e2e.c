/* tools/bench_e2e.c -- end-to-end single-client SET/GET latency + throughput.

   Measures the SAME loop in two connection modes so the two costs can be told
   apart (the old version only had the first mode and reported it as if it were
   the store's own latency):
     connect-per-op : a new cemon loop + TCP connection per operation.  This is
                      what a one-shot CLI / batch script sees.  Its latency
                      INCLUDES TCP setup (typically ~2-3 ms on Windows loopback).
     persistent     : one connection for the whole loop; one round-trip per
                      operation.  This is the store's own per-op cost.
   The delta between the two medians is the connect setup cost, printed on its own
   line so a reader can subtract it rather than guess.

   Single file, C89, MSVC 6.0.
   build: ./build.sh bench-e2e
   run:   bench_e2e.exe <host:port> [N]     (default N=1000) */

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
#include "../code/kbase.h"
#include "../code/kproto.h"
#include "../code/kserver.h"
#include "../code/kclient.h"
#include "bench_client.h"

static int bench_cmp_u64(const void *a,const void *b){
  k_u64 x=*(const k_u64 *)a,y=*(const k_u64 *)b;
  return x<y?-1:(x>y?1:0);
}
static k_u64 bench_median(k_u64 *samples,int n){
  qsort(samples,n,sizeof(samples[0]),bench_cmp_u64);
  return samples[n/2];
}
static void bench_report(const char *mode,const char *label,k_u64 *samples,int n){
  k_u64 median,p99,avg,total;
  int i;
  qsort(samples,n,sizeof(samples[0]),bench_cmp_u64);
  median=samples[n/2];
  p99=samples[(n*99)/100];
  total=0;
  for(i=0;i<n;i++) total+=samples[i];
  avg=total/(k_u64)n;
  printf("%-15s %-4s median=%6" K_U64_FMT " us  p99=%7" K_U64_FMT " us  avg=%6" K_U64_FMT " us  serial-throughput=%7" K_U64_FMT " ops/s\n",
         mode,label,median,p99,avg,(median?((k_u64)1000000u/median):0u));
}

int main(int argc,char **argv){
  k_response_data resp;
  k_bench_conn conn;
  k_u64 *samples;
  k_u64 t0,t1;
  k_u64 med_conn_set,med_pers_set;
  char *host,*colon;
  char params[192];
  unsigned short port;
  int n,i,rc;
  if(argc<2){ printf("usage: bench_e2e <host:port> [N]\n"); return 1; }
  colon=strchr(argv[1],':');
  if(!colon){ printf("bad host:port\n"); return 1; }
  host=argv[1];
  *colon='\0';
  port=(unsigned short)atoi(colon+1);
  n=argc>=3?atoi(argv[2]):1000;
  if(n<=0) n=1000;
  sprintf(params,"host=%.80s:%u N=%d sample=per-op wall (k_monotonic_us)",host,(unsigned)port,n);
  bench_env_banner("bench_e2e",params);
  samples=(k_u64 *)malloc((size_t)n*sizeof(k_u64));
  if(!samples){ printf("oom\n"); return 1; }

  /* ---------- mode 1: connect per operation (legacy shape) ---------- */
  printf("\n[1] connect per op -- each op builds its own loop + TCP connection\n");
  memset(&resp,0,sizeof(resp));
  if(k_sync_call(host,port,K_REQ_SET,"__bench__",9u,"v",1u,5000,&resp)!=0||resp.status!=K_STATUS_OK){
    printf("warm-up SET failed\n");
    free(samples);
    return 1;
  }
  k_response_data_free(&resp);
  for(i=0;i<n;i++){
    memset(&resp,0,sizeof(resp));
    k_monotonic_us(&t0);
    rc=k_sync_call(host,port,K_REQ_SET,"__bench__",9u,"v",1u,5000,&resp);
    k_monotonic_us(&t1);
    if(rc!=0){ printf("SET %d failed\n",i); free(samples); return 1; }
    k_response_data_free(&resp);
    samples[i]=t1>t0?t1-t0:0u;
  }
  med_conn_set=bench_median(samples,n);
  bench_report("connect-per-op","SET",samples,n);
  for(i=0;i<n;i++){
    memset(&resp,0,sizeof(resp));
    k_monotonic_us(&t0);
    rc=k_sync_call(host,port,K_REQ_GET,"__bench__",9u,0,0u,5000,&resp);
    k_monotonic_us(&t1);
    if(rc!=0){ printf("GET %d failed\n",i); free(samples); return 1; }
    k_response_data_free(&resp);
    samples[i]=t1>t0?t1-t0:0u;
  }
  bench_report("connect-per-op","GET",samples,n);

  /* ---------- mode 2: one persistent connection ---------- */
  printf("\n[2] persistent connection -- one connect, one round-trip per op\n");
  if(k_conn_open(&conn,host,port,5000)!=0){ printf("persistent connect failed\n"); free(samples); return 1; }
  memset(&resp,0,sizeof(resp));
  if(k_conn_call(&conn,K_REQ_SET,"__bench__",9u,"v",1u,5000,&resp)!=0||resp.status!=K_STATUS_OK){
    printf("persistent warm-up SET failed\n");
    k_response_data_free(&resp);
    k_conn_close(&conn);
    free(samples);
    return 1;
  }
  k_response_data_free(&resp);
  for(i=0;i<n;i++){
    memset(&resp,0,sizeof(resp));
    k_monotonic_us(&t0);
    rc=k_conn_call(&conn,K_REQ_SET,"__bench__",9u,"v",1u,5000,&resp);
    k_monotonic_us(&t1);
    if(rc!=0){ printf("persistent SET %d failed\n",i); free(samples); return 1; }
    k_response_data_free(&resp);
    samples[i]=t1>t0?t1-t0:0u;
  }
  med_pers_set=bench_median(samples,n);
  bench_report("persistent","SET",samples,n);
  for(i=0;i<n;i++){
    memset(&resp,0,sizeof(resp));
    k_monotonic_us(&t0);
    rc=k_conn_call(&conn,K_REQ_GET,"__bench__",9u,0,0u,5000,&resp);
    k_monotonic_us(&t1);
    if(rc!=0){ printf("persistent GET %d failed\n",i); free(samples); return 1; }
    k_response_data_free(&resp);
    samples[i]=t1>t0?t1-t0:0u;
  }
  bench_report("persistent","GET",samples,n);
  k_conn_close(&conn);

  /* ---------- attribution ---------- */
  printf("\n[derived] SET connect setup cost = connect-per-op median - persistent median\n");
  if(med_conn_set>med_pers_set)
    printf("          %" K_U64_FMT " us - %" K_U64_FMT " us = %" K_U64_FMT " us per operation\n",med_conn_set,med_pers_set,med_conn_set-med_pers_set);
  else
    printf("          %" K_U64_FMT " us <= %" K_U64_FMT " us (no measurable setup cost in these samples)\n",med_conn_set,med_pers_set);
  free(samples);
  return 0;
}
