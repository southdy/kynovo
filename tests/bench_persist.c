/* tests/bench_persist.c -- persistent-connection SET benchmark.

   bench_e2e / bench_mt create a fresh cemon loop + TCP connection for EVERY op,
   so their per-op latency includes client-side setup.  This one uses kclient's
   persistent-connection state machine (the same one kdbctl drives): one connect,
   N round trips -- so the reported latency is the server's, not the client's.

   C89.  build: ./build.sh bench-persist */

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
#include <mmsystem.h>
#else
#include <time.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "../tools/bench_env.h"
#include "../code/kbase.h"
#include "../code/kproto.h"
#include "../code/kserver.h"
#include "../code/kclient.h"

static k_u64 bnow(void){ k_u64 t=0; k_monotonic_us(&t); return t; }
static int cmpu64(const void *a,const void *b){
  k_u64 x=*(const k_u64*)a,y=*(const k_u64*)b;
  return x<y?-1:(x>y?1:0);
}

static int k_send_frame(cemon_socket *sock,k_u32 magic,k_u8 type,const void *payload,k_u32 size,int control){
  k_u8 *frame;
  k_u32 total;
  int rc;
  if(!sock||size>K_FRAME_MAX||(size&&!payload)) return -1;
  total=K_FRAME_HEADER+size;
  frame=(k_u8 *)K_MALLOC(total);
  if(!frame) return -1;
  k_frame_header_build(frame,magic,type,size);
  if(size) memcpy(frame+K_FRAME_HEADER,payload,size);
  rc=control?cemon_send_control(sock,frame,(int)total):cemon_send(sock,frame,(int)total);
  K_FREE(frame);
  return rc;
}

typedef struct{
  volatile int responses;
} bench_out;

static void bench_output(void *ud,const char *fmt,...){
  bench_out *o=(bench_out *)ud;
  va_list ap;
  (void)fmt;
  va_start(ap,fmt);
  va_end(ap);
  o->responses++;
}

static void bench_client_io(cemon_socket *sock,const cemon_event *event){
  k_client_app *app=(k_client_app *)cemon_getud(sock);
  if(!app||!event) return;
  if(event->type==CEMON_CONNECT){
    if(k_client_on_connected(app)!=0||app->transport->recv(app,app->sock)!=0) app->transport->close(app,app->sock);
  }else if(event->type==CEMON_DATA){
    if(k_client_on_received(app,event->data,(k_u32)event->size)!=0||app->transport->recv(app,app->sock)!=0) app->transport->close(app,app->sock);
  }else if(event->type==CEMON_EOF){
    app->transport->close(app,app->sock);
  }else if(event->type==CEMON_CLOSED){
    k_client_on_closed(app);
  }
}
static void *t_connect(k_client_app *app,const char *host,unsigned short port){
  return cemon_tcp_connect((cemon *)app->loop,k_numeric_host(host),port,bench_client_io,app);
}
static int t_send(k_client_app *app,void *sock,k_u32 magic,k_u8 type,const void *payload,k_u32 size,int control){
  (void)app;
  return k_send_frame((cemon_socket *)sock,magic,type,payload,size,control);
}
static int t_recv(k_client_app *app,void *sock){ (void)app; return cemon_recv((cemon_socket *)sock); }
static void t_close(k_client_app *app,void *sock){ (void)app; cemon_close((cemon_socket *)sock); }
static const k_client_transport t_cemon={"cemon",t_connect,t_send,t_recv,t_close};

int main(int argc,char **argv){
  const char *seed=argc>=2?argv[1]:"127.0.0.1:27091";
  int n=argc>=3?atoi(argv[2]):500;
  cemon *loop;
  k_client_app app;
  bench_out out;
  k_u64 *s;
  int i,ok=0;
  char params[160];

#ifdef _WIN32
  timeBeginPeriod(1);
#endif
  sprintf(params,"server=%.80s ops=%d",seed,n);
  bench_env_banner("bench_persist",params);
  memset(&app,0,sizeof(app));
  memset(&out,0,sizeof(out));
  if(k_client_seed_parse(&app,seed)!=0){ printf("seed parse failed\n"); return 1; }
  loop=cemon_create();
  if(!loop){ printf("cemon_create failed\n"); return 1; }
  app.loop=loop;
  app.transport=&t_cemon;
  app.output=bench_output;
  app.output_ud=&out;
  if(k_client_connect(&app)!=0){ printf("connect call failed\n"); return 1; }
  {
    int spins=0;
    while(!app.connected&&!app.stopping&&spins<2000){
      cemon_poll(loop,20);
      k_monotonic_us(&app.now_us);
      k_client_poll(&app);
      spins++;
    }
  }
  if(!app.connected){ printf("not connected (stopping=%d)\n",app.stopping); return 1; }

  s=(k_u64 *)malloc((size_t)n*sizeof(k_u64));
  if(!s) return 1;
  for(i=0;i<n;i++){
    char key[48],val[48];
    k_u64 t0,t1;
    int spins=0;
    sprintf(key,"pk-%d",i); sprintf(val,"pv-%d",i);
    t0=bnow();
    if(k_client_queue(&app,K_REQ_SET,key,(k_u32)strlen(key),val,(k_u32)strlen(val))!=0) break;
    /* the request's lifetime is tracked by app.pending: it is cleared when the
       matching response arrives.  (Do NOT count output() calls -- k_client_queue
       prints "wait for the current request" when one is already in flight.) */
    while(app.pending&&!app.stopping&&spins<100000){
      k_u64 p0=bnow(),p1;
      int rc=cemon_poll(loop,20);
      p1=bnow();
      k_monotonic_us(&app.now_us);
      k_client_poll(&app);
      if(ok<4) fprintf(stderr,"  [op %d] poll#%d rc=%d took=%" K_U64_FMT " us\n",ok+1,spins,rc,(k_u64)(p1-p0));
      spins++;
    }
    t1=bnow();
    if(app.pending) break;
    s[ok++]=t1-t0;
    if(ok==1||ok==2||ok==3||ok==50||ok==200) fprintf(stderr,"[op %d] spins=%d dt=%" K_U64_FMT " us\n",ok,spins,(k_u64)(t1-t0));
  }
  if(ok>0){
    qsort(s,(size_t)ok,sizeof(s[0]),cmpu64);
    printf("persistent SET: n=%d median=%" K_U64_FMT " us  p99=%" K_U64_FMT "  min=%" K_U64_FMT "  max=%" K_U64_FMT "\n",
           ok,(k_u64)s[ok/2],
           (k_u64)s[(int)(((k_u64)ok*99u)/100u)],
           (k_u64)s[0],(k_u64)s[ok-1]);
  }else printf("persistent SET: all failed\n");
  free(s);
  app.stopping=1;
  cemon_stop(loop);
  while(cemon_poll(loop,0)==0){}
  cemon_destroy(loop);
  k_pending_free(app.pending);
  k_rx_free(&app.rx);
#ifdef _WIN32
  timeEndPeriod(1);
#endif
  return 0;
}
