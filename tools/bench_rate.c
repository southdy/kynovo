/* tools/bench_rate.c -- client-throughput probe: drive a live kdbsvr with N pipelined
   writes at in-flight depth K and report, around the measured phase, the server's own
   counters (rounds, batch size, submit reasons, WAL records) plus per-op latency
   percentiles.  Every TPS/latency number quoted in the review notes comes from here, so
   it ships with the tree instead of living in a scratch file.

   run: bench_rate.exe <host:port> <N> <K> [value_size] [key] [unique] [mode]
   prints STATS_BEFORE, PHASE, LAT and STATS_AFTER lines (one per line, key=value).
   mode=get: issue N GETs instead of SETs and print a GET| summary plus VALUE|<body> for
   the first response - that is how the cluster tests check that a committed value is still
   readable after a failover (kdbctl is a prompt-driven REPL and needs a TTY, so a scripted
   read needs this path). */
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
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "bench_env.h"
#include "../code/kbase.h"
#include "../code/kproto.h"
#include "../code/kserver.h"
#include "../code/kclient.h"
#include "bench_client.h"

static int k_u64_cmp(const void *a,const void *b){
  k_u64 x=*(const k_u64 *)a,y=*(const k_u64 *)b;
  return x<y?-1:(x>y?1:0);
}
int main(int argc,char **argv){
  char *host,*colon;
  unsigned short port;
  const char *key=argc>5?argv[5]:"__rate__";
  int unique=argc>6?atoi(argv[6]):0;   /* 1 = append the request id to the key, so the
                                          workload has N DISTINCT keys and exercises the
                                          real treap depth (COW path length) */
  unsigned int key_len;
  char host_buf[64];
  int g_probe_status=-1;
  int get_mode=(argc>7&&strcmp(argv[7],"get")==0);
  unsigned int val_size=argc>4?(unsigned int)atoi(argv[4]):1u;
  int gi;
  unsigned char *value;
  int n=argc>2?atoi(argv[2]):2000;
  int k=argc>3?atoi(argv[3]):32;
  k_response_data resp;
  k_pipe pipe;
  k_u64 t0,t1,sent,got,latn;
  static k_u64 lats[200000];
  if(argc<4){ printf("usage: tmp_rate <host:port> <N> <K> [value_size] [key]\n"); return 1; }
  colon=strchr(argv[1],':');
  if(!colon){ printf("bad host:port\n"); return 1; }
  host=argv[1];
  *colon='\0';
  port=(unsigned short)atoi(colon+1);
  if(strlen(host)>=sizeof(host_buf)){ printf("host too long\n"); return 1; }
  strcpy(host_buf,host);
  host=host_buf;
  key_len=(unsigned int)strlen(key);
  if(!val_size) val_size=1u;
  value=(unsigned char *)malloc(val_size);
  if(!value){ printf("oom\n"); return 1; }
  memset(value,'x',val_size);
  memset(&resp,0,sizeof(resp));
  /* TEMP-DIAG (mode 'members'): does the SERVER answer the discovery query at all?  The
     CLI wedges after connect because its MEMBERS response never reaches the client frame
     handler; k_sync_call's read path is known-good, so this splits server vs client. */
  if(argc>7&&strcmp(argv[7],"members")==0){
    k_response_data m;
    memset(&m,0,sizeof(m));
    if(k_sync_call(host,port,K_REQ_MEMBERS,0,0,0,0,5000,&m)!=0) printf("MEMBERS|sync_call_failed\n");
    else printf("MEMBERS|status=%u len=%u body=%.*s\n",(unsigned)m.status,(unsigned)m.body_size,
                (int)(m.body_size>200u?200u:m.body_size),(const char *)m.body);
    k_response_data_free(&m);
    return 0;
  }
  /* dissertation 6.2: a server that is not the leader rejects the request and returns the
     address of the leader, and the client reconnects there ("the first option, which we
     recommend and which LogCabin implements").  Follow that hint once, exactly as the
     production client does - otherwise every response is a rejection, which a naive drain
     counts as a completion and which used to inflate these throughput numbers by orders of
     magnitude. */
  {
    k_response_data hint;
    memset(&hint,0,sizeof(hint));
    if(k_sync_call(host,port,K_REQ_SET,(const unsigned char *)key,key_len,value,val_size,5000,&hint)!=0){
      printf("PROBE|sync_call_failed\n");
    }else{
      g_probe_status=(int)hint.status;
      printf("PROBE|status=%u leader_id=%d leader=%s:%u body=%.*s\n",(unsigned)hint.status,
             hint.leader_id,hint.host,(unsigned)hint.port,
             (int)(hint.body_size>48u?48u:hint.body_size),(const char *)hint.body);
      if(hint.status==K_STATUS_REDIRECT&&hint.host[0]&&hint.port&&
         (strcmp(hint.host,host)!=0||hint.port!=port)){
        printf("REDIRECT|following leader %s:%u\n",hint.host,(unsigned)hint.port);
        strcpy(host_buf,hint.host);
        port=hint.port;
      }
    }
    k_response_data_free(&hint);
  }
  /* Say WHICH server is being measured: the redirect above can move the target, and a
     STATS reading that silently came from the leader instead of the port the caller named
     once looked exactly like three servers claiming leadership. */
  printf("TARGET|%s:%u\n",host,(unsigned)port);
  if(k_sync_call(host,port,K_REQ_STATS,0,0,0,0,5000,&resp)!=0){ printf("stats call failed\n"); return 1; }
  printf("STATS_BEFORE|%.*s\n",(int)resp.body_size,(const char *)resp.body);
  k_response_data_free(&resp);
  if(get_mode){
    int ok=0,empty=0,err=0;
    k_u32 bytes=0;
    for(gi=0;gi<n;gi++){
      char kbuf[64];
      const char *kp=key;
      unsigned int klen=key_len;
      if(unique){ klen=(unsigned int)sprintf(kbuf,"%s%d",key,gi); kp=kbuf; }
      memset(&resp,0,sizeof(resp));
      if(k_sync_call(host,port,K_REQ_GET,(const unsigned char *)kp,klen,0,0,5000,&resp)!=0){ err++; continue; }
      if(gi==0) printf("VALUE|key=%s size=%u body=%.*s\n",kp,(unsigned)resp.body_size,
                       (int)(resp.body_size>64u?64u:resp.body_size),(const char *)resp.body);
      if(resp.body_size){ ok++; bytes+=resp.body_size; }else empty++;
      k_response_data_free(&resp);
    }
    printf("GET|n=%d ok=%d empty=%d err=%d bytes=%u\n",n,ok,empty,err,(unsigned)bytes);
    return 0;
  }
  if(k_pipe_open(&pipe,host,port,5000)!=0){ printf("connect failed\n"); return 1; }
  if(k_pipe_send(&pipe,K_REQ_SET,key,key_len,value,val_size,1u)<0){ printf("warmup failed\n"); return 1; }
  { int guard=0; while(k_pipe_pending(&pipe)<1&&!pipe.failed&&guard<5000){ cemon_poll(pipe.loop,1); guard++; } }
  { k_u64 dummy; while(k_pipe_drain(&pipe,&dummy,1)>0){} }
  sent=0; latn=0;
  if(k_monotonic_us(&t0)!=0) t0=0;
  while(latn<(k_u64)n){
    while(pipe.outstanding<k&&sent<(k_u64)n){
      const unsigned char *kptr=(const unsigned char *)key;
      unsigned int klen=key_len;
      char kbuf[64];
      if(unique){
        klen=(unsigned int)sprintf(kbuf,"%s%llu",key,(unsigned long long)sent);
        kptr=(const unsigned char *)kbuf;
      }
      if(k_pipe_send(&pipe,K_REQ_SET,kptr,klen,value,val_size,(k_u32)(sent+2))<0) break;
      sent++;
    }
    if(k_pipe_await(&pipe,1,5000)==0) break;
    latn+=(k_u64)k_pipe_drain(&pipe,lats+latn,(int)(sizeof(lats)/sizeof(lats[0])-latn));
    got=latn;
    if(got>=(k_u64)n) break;
  }
  if(k_monotonic_us(&t1)!=0) t1=0;
  /* A phase is only meaningful when the server ACCEPTED the probe (K_STATUS_OK): a redirect
     means the ops were refused, and a drain counts refusals as completions. */
  printf("PHASE_VALID|%s (probe_status=%d, %s)\n",
         g_probe_status==K_STATUS_OK?"yes":"NO",
         g_probe_status,g_probe_status==K_STATUS_OK?"server accepted the write":
         "writes were refused: throughput below is NOT a commit rate");
  printf("PHASE|n=%llu k=%d wall_us=%llu ops_per_s=%llu\n",
         (unsigned long long)latn,k,(unsigned long long)(t1-t0),
         (unsigned long long)((t1>t0)?(latn*1000000u/(t1-t0)):0u));
  /* Per-op completion latency (for K=1 this is the request->response time of one
     synchronous call; for K>1 it includes the time the request waited in the pipeline).
     Median/p99 are what tell whether a lone write really waits the flush window. */
  if(latn){
    k_u64 *sorted=(k_u64 *)malloc((size_t)latn*sizeof(k_u64));
    if(sorted){
      k_u64 i;
      for(i=0;i<latn;i++) sorted[i]=lats[i];
      qsort(sorted,(size_t)latn,sizeof(k_u64),k_u64_cmp);
      printf("LAT|n=%llu min=%llu p50=%llu p90=%llu p99=%llu max=%llu\n",
             (unsigned long long)latn,
             (unsigned long long)sorted[0],
             (unsigned long long)sorted[latn/2],
             (unsigned long long)sorted[(latn*9)/10],
             (unsigned long long)sorted[(latn*99)/100],
             (unsigned long long)sorted[latn-1]);
      free(sorted);
    }
  }
  /* A response is not necessarily an ACCEPTED write: a follower answers an error at once,
     which a naive drain counts as a completion (and inflates ops/s by orders of magnitude).
     Issue one synchronous write and print its body so the caller can see what the server
     really said. */
  { k_response_data probe;
    memset(&probe,0,sizeof(probe));
    if(k_sync_call(host,port,K_REQ_SET,(const unsigned char *)key,key_len,value,val_size,5000,&probe)==0)
      printf("RESP|status=%u leader_id=%d leader=%s:%u body=%.*s\n",(unsigned)probe.status,
             probe.leader_id,probe.host,(unsigned)probe.port,
             (int)(probe.body_size>80u?80u:probe.body_size),(const char *)probe.body);
    else printf("RESP|sync_call_failed\n");
    k_response_data_free(&probe);
  }
  { const char *st=k_pipe_fetch_stats(&pipe,5000); if(st&&st[0]) printf("STATS_AFTER|%s\n",st); }
  k_pipe_close(&pipe);
  free(value);
  return 0;
}
