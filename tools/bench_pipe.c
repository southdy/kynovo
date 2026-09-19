/* tools/bench_pipe.c -- pipelined (asynchronous) client benchmark.

   Why this tool exists: the server implements group commit (writes accumulate in
   write_head and flush when the batch age reaches flush_timeout_ms).  A SYNCHRONOUS
   client sends its next write only after the previous reply, so it can never keep
   two writes inside that window: durable throughput then looks flat (~110-230 ops/s)
   even though the mechanism is fine.  Getting concurrency by adding synchronous
   clients works (measured: 127 -> 3324 ops/s from 1 to 64 clients) but costs one
   thread + one connection each, and at ~128 threads the load generator starves
   itself on a 12-core box.

   This tool keeps K requests in flight on ONE connection and matches replies by the
   echoed request id, which is the load shape the server is designed for.  It reports
   the two numbers that matter for a group-commit system:
     latency@K   per-operation round-trip while K are outstanding
     throughput@K aggregate ops/s on that single connection.

   build: ./build.sh bench-pipe
   run:   bench_pipe.exe <host:port> [N] [K list e.g. 1,2,4,8,16,32,64] */

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
#include <windows.h>
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

#define PIPE_N 2000
#define PIPE_MAXK 64
#define PIPE_KEY "__pipe__"

static int cmp_u64(const void *a,const void *b){
  k_u64 x=*(const k_u64 *)a,y=*(const k_u64 *)b;
  return x<y?-1:(x>y?1:0);
}
static int parse_list(const char *arg,int *out,int max){
  const char *p=arg;
  int n=0,v,digits;
  while(*p&&n<max){
    v=0; digits=0;
    while(*p>='0'&&*p<='9'){ v=v*10+(*p-'0'); p++; digits++; }
    if(!digits) return -1;
    if(v<1||v>PIPE_MAXK) return -1;
    out[n++]=v;
    if(*p==','){ p++; continue; }
    if(*p==0) break;
    return -1;
  }
  return n;
}

int main(int argc,char **argv){
  static k_u64 lats[PIPE_N+4096];
  int ks[PIPE_MAXK];
  int kn=6;
  char *host,*colon;
  char params[256];
  unsigned short port;
  int n,i,ci,k,rc,sent,got,latn;
  k_u64 t0,t1,median,p99,maxv,sum;
  if(argc<2){ printf("usage: bench_pipe <host:port> [N] [K list]  (K>=N fires the whole burst at once)\n"); return 1; }
  colon=strchr(argv[1],':');
  if(!colon){ printf("bad host:port\n"); return 1; }
  host=argv[1];
  *colon='\0';
  port=(unsigned short)atoi(colon+1);
  n=PIPE_N;
  ks[0]=1; ks[1]=2; ks[2]=4; ks[3]=8; ks[4]=16; ks[5]=32;
  for(i=2;i<argc;i++){
    if(strchr(argv[i],',')!=0){
      int parsed=parse_list(argv[i],ks,PIPE_MAXK);
      if(parsed<=0){ printf("bad K list '%s'\n",argv[i]); return 1; }
      kn=parsed;
    }else if(atoi(argv[i])>0) n=atoi(argv[i]);
  }
  {
    char ktext[128];
    ktext[0]='\0';
    for(i=0;i<kn;i++){
      char one[16];
      sprintf(one,"%s%d",i?",":"",ks[i]);
      if(strlen(ktext)+strlen(one)<sizeof(ktext)-1) strcat(ktext,one);
    }
    sprintf(params,"host=%.80s:%u N=%d K={%s} mode=pipelined single connection",host,(unsigned)port,n,ktext);
  }
  bench_env_banner("bench_pipe",params);
  printf("\nK = requests in flight on ONE connection; latency = send -> matching reply\n\n");
  for(ci=0;ci<kn;ci++){
    k_pipe pipe;
    k=ks[ci];
    if(k_pipe_open(&pipe,host,port,5000)!=0){ printf("K=%-3d: connect failed\n",k); continue; }
    /* warm-up: one synchronous op so the connection + key are ready */
    if(k_pipe_send(&pipe,K_REQ_SET,PIPE_KEY,9u,"v",1u,1u)<0){ printf("K=%-3d: warm-up send failed\n",k); k_pipe_close(&pipe); continue; }
    { int guard=0; while(k_pipe_pending(&pipe)<1&&!pipe.failed&&guard<5000){ cemon_poll(pipe.loop,1); guard++; } }
    k_pipe_drain(&pipe,lats,1);
    sent=0; got=0; latn=0;
    if(k_monotonic_us(&t0)!=0) t0=0;
    while(got<n){
      while(pipe.outstanding<k&&sent<n){
        if(k_pipe_send(&pipe,K_REQ_SET,PIPE_KEY,9u,"v",1u,(k_u32)(sent+2))<0) break;
        sent++;
      }
      rc=k_pipe_await(&pipe,1,5000);
      if(rc<0){ printf("K=%-3d: connection failed after %d ops\n",k,got); break; }
      if(rc==0) break;                       /* timeout */
      latn+=k_pipe_drain(&pipe,lats+latn,(int)(sizeof(lats)/sizeof(lats[0]))-latn);
      got=latn;
    }
    if(k_monotonic_us(&t1)!=0) t1=0;
    if(latn>0){
      qsort(lats,latn,sizeof(lats[0]),cmp_u64);
      median=lats[latn/2];
      p99=lats[(latn*99)/100];
      maxv=lats[latn-1];
      sum=0;
      for(i=0;i<latn;i++) sum+=lats[i];
      printf("K=%-3d: n=%-5d throughput=%9lu ops/s (single connection)\n",k,latn,
             (t1>t0)?(unsigned long)((k_u64)latn*1000000u/(t1-t0)):0ul);
      printf("        per-op latency: median=%6lu us  p99=%8lu us  max=%8lu us  avg=%6lu us  (ring_drops=%lu)\n",
             (unsigned long)median,(unsigned long)p99,(unsigned long)maxv,
             (unsigned long)(sum/(k_u64)latn),(unsigned long)pipe.ring_drops);
    }else{
      printf("K=%-3d: no completed ops\n",k);
    }
    {
      const char *st=k_pipe_fetch_stats(&pipe,5000);
      if(st&&st[0]) printf("        server flush accounting: %s\n",st);
    }
    k_pipe_close(&pipe);
  }
  return 0;
}
