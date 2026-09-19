/* tools/bench_client.h -- shared synchronous client + environment banner for the
   benchmark tools (bench_e2e, bench_mt, and future ones).

   Two connection modes, and the difference matters:
     k_sync_call*  connect per call.  The latency it reports INCLUDES the
                   per-operation connect cost (TCP setup + two loops), so it is
                   the right tool for "what does a CLI/batch script see", NOT for
                   "what does the server cost".
     k_conn_*      one persistent connection for the whole loop.  This is the
                   mode that answers "what is the store's own per-op latency".

   Include AFTER code/kserver.h + code/kclient.h (it uses k_request_payload,
   k_rx, k_response_data, k_numeric_host).  Single file, C89, MSVC 6.0. */

#ifndef BENCH_CLIENT_H
#define BENCH_CLIENT_H

#ifndef BENCH_CFLAGS
#define BENCH_CFLAGS "(not recorded)"
#endif

#define K_LEN(s) ((k_u32)(sizeof(s)-1u))

/* ============ App transport: send one framed message via cemon ============ */
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

typedef struct k_sync_client{
  cemon *loop;
  cemon_socket *sock;
  k_rx rx;
  const k_u8 *payload;
  k_u32 payload_size;
  k_u8 request_type;
  k_response_data response;
  int done;
  int failed;
  int connected;   /* set by the CONNECT event; persistent mode sends from the caller */
  int send_now;    /* per-op mode: send from the CONNECT callback, as before */
} k_sync_client;

static int k_sync_response_frame(void *ud,k_u8 type,const k_u8 *payload,k_u32 size){
  k_sync_client *client=(k_sync_client *)ud;
  if(!client||type!=K_RESPONSE||k_response_decode(&client->response,payload,size)!=0) return -1;
  client->done=1;
  if(client->send_now) cemon_stop(client->loop);
  return 0;
}
static void k_sync_client_io(cemon_socket *sock,const cemon_event *event){
  k_sync_client *client=(k_sync_client *)cemon_getud(sock);
  if(!client||!event) return;
  if(event->type==CEMON_CONNECT){
    client->connected=1;
    if(client->send_now){
      if(k_send_frame(sock,K_CLIENT_MAGIC,client->request_type,client->payload,client->payload_size,0)!=0||cemon_recv(sock)!=0){
        client->failed=1;
        cemon_close(sock);
      }
    }
  }else if(event->type==CEMON_DATA){
    if(k_rx_feed(&client->rx,K_CLIENT_MAGIC,event->data,(k_u32)event->size,k_sync_response_frame,client)!=0){
      client->failed=1;
      cemon_close(sock);
    }else if(!client->done&&cemon_recv(sock)!=0){
      client->failed=1;
      cemon_close(sock);
    }
  }else if(event->type==CEMON_EOF){
    if(!client->done) client->failed=1;
    cemon_close(sock);
  }else if(event->type==CEMON_CLOSED){
    client->sock=0;
    if(!client->done) client->failed=1;
  }
}
/* ---- connect per call (legacy shape; latency includes the connect) ---- */
static int k_sync_call_payload(const char *host,unsigned short port,k_u8 type,const k_u8 *payload,k_u32 payload_size,unsigned int timeout_ms,k_response_data *response){
  k_sync_client client;
  k_u64 start_us,now_us;
  int poll_rc;
  if(!host||!response||(payload_size&&!payload)) return -1;
  memset(&client,0,sizeof(client));
  client.payload=payload;
  client.payload_size=payload_size;
  client.request_type=type;
  client.send_now=1;
  client.loop=cemon_create();
  if(!client.loop) return -1;
  client.sock=cemon_tcp_connect(client.loop,k_numeric_host(host),port,k_sync_client_io,&client);
  if(!client.sock){
    cemon_destroy(client.loop);
    return -1;
  }
  if(k_monotonic_us(&start_us)!=0) start_us=0;
  while(!client.done&&!client.failed){
    poll_rc=cemon_poll(client.loop,20);
    if(poll_rc!=0&&!client.done) client.failed=1;
    if(start_us&&k_monotonic_us(&now_us)==0&&now_us-start_us>(k_u64)timeout_ms*1000u) client.failed=1;
  }
  if(!cemon_should_stop(client.loop)) cemon_stop(client.loop);
  while(cemon_poll(client.loop,0)==0){}
  cemon_destroy(client.loop);
  k_rx_free(&client.rx);
  if(!client.done||client.failed){
    k_response_data_free(&client.response);
    return -1;
  }
  *response=client.response;
  return 0;
}
static int k_sync_call(const char *host,unsigned short port,k_u8 type,const void *key,k_u32 key_len,const void *value,k_u32 value_len,unsigned int timeout_ms,k_response_data *response){
  k_buf payload;
  int rc;
  if(k_request_payload(&payload,1u,type,key,key_len,value,value_len)!=0) return -1;
  rc=k_sync_call_payload(host,port,type,payload.data,payload.len,timeout_ms,response);
  k_buf_free(&payload);
  return rc;
}

/* ---- persistent connection: connect once, then one round-trip per call ---- */
/* NOTE: kserver.h already owns the name k_conn (its client-connection record), so
   the benchmark connection carries a bench-specific name. */
typedef struct k_bench_conn{
  k_sync_client client;
} k_bench_conn;

static int k_conn_open(k_bench_conn *c,const char *host,unsigned short port,unsigned int timeout_ms){
  k_u64 start_us,now_us;
  if(!c||!host) return -1;
  memset(c,0,sizeof(*c));
  c->client.send_now=0;
  c->client.loop=cemon_create();
  if(!c->client.loop) return -1;
  if(k_monotonic_us(&start_us)!=0) start_us=0;
  c->client.sock=cemon_tcp_connect(c->client.loop,k_numeric_host(host),port,k_sync_client_io,&c->client);
  if(!c->client.sock){
    cemon_destroy(c->client.loop);
    c->client.loop=0;
    return -1;
  }
  while(!c->client.connected&&!c->client.failed){
    if(cemon_poll(c->client.loop,20)!=0&&!c->client.connected) c->client.failed=1;
    if(start_us&&k_monotonic_us(&now_us)==0&&now_us-start_us>(k_u64)timeout_ms*1000u) c->client.failed=1;
  }
  if(!c->client.connected||c->client.failed){
    cemon_close(c->client.sock);
    cemon_destroy(c->client.loop);
    c->client.loop=0;
    c->client.sock=0;
    return -1;
  }
  return 0;
}
static int k_conn_call(k_bench_conn *c,k_u8 type,const void *key,k_u32 key_len,const void *value,k_u32 value_len,unsigned int timeout_ms,k_response_data *response){
  k_buf payload;
  k_u64 start_us,now_us;
  int rc=-1;
  if(!c||!c->client.sock||!response) return -1;
  if(k_request_payload(&payload,1u,type,key,key_len,value,value_len)!=0) return -1;
  /* prev op finished: drop the response slot and rewind the rx cursor (the buffer
     is reused, so a persistent-mode op costs zero allocations) */
  c->client.rx.len=0;
  k_response_data_free(&c->client.response);
  memset(&c->client.response,0,sizeof(c->client.response));
  c->client.done=0;
  c->client.failed=0;
  if(k_monotonic_us(&start_us)!=0) start_us=0;
  if(k_send_frame(c->client.sock,K_CLIENT_MAGIC,type,payload.data,payload.len,0)!=0||cemon_recv(c->client.sock)!=0){
    k_buf_free(&payload);
    return -1;
  }
  k_buf_free(&payload);
  while(!c->client.done&&!c->client.failed){
    if(cemon_poll(c->client.loop,1)!=0&&!c->client.done) c->client.failed=1;
    if(start_us&&k_monotonic_us(&now_us)==0&&now_us-start_us>(k_u64)timeout_ms*1000u) c->client.failed=1;
  }
  if(!c->client.done||c->client.failed){
    k_response_data_free(&c->client.response);
    return -1;
  }
  *response=c->client.response;
  memset(&c->client.response,0,sizeof(c->client.response));
  rc=0;
  return rc;
}
static void k_conn_close(k_bench_conn *c){
  k_u64 start_us,now_us;
  int guard;
  if(!c||!c->client.loop) return;
  if(k_monotonic_us(&start_us)!=0) start_us=0;
  if(c->client.sock) cemon_close(c->client.sock);
  /* Stop the loop and drain it with a hard 20 ms wall-clock budget: a 1 ms poll
     per iteration makes a long drain expensive (200 x 1 ms), while an unbounded
     0 ms drain can spin forever if the loop keeps reporting work.  Callers that
     measure a phase must end the phase before this call (see bench_mt, which
     ends on the last worker's own timestamp). */
  if(!cemon_should_stop(c->client.loop)) cemon_stop(c->client.loop);
  guard=0;
  while(cemon_poll(c->client.loop,1)==0&&guard<64){
    guard++;
    if(start_us&&k_monotonic_us(&now_us)==0&&now_us-start_us>20000u) break;
  }
  k_rx_free(&c->client.rx);
  k_response_data_free(&c->client.response);
  cemon_destroy(c->client.loop);
  c->client.loop=0;
  c->client.sock=0;
}

/* ================= Pipelined connection: K requests in flight, ONE connection ================= */
/* A synchronous client sends its next write only after the previous reply, so it can
   never keep several writes inside the server's group-commit window.  Scaling that
   way needs one thread + one connection per client (128 threads on a 12-core box
   starves the load generator itself).  This client instead keeps up to K requests
   outstanding on a single connection and matches replies by their echoed request id,
   which is the load shape a group-commit server is designed for. */
#define K_PIPE_MAX 4096   /* the burst size is the pipeline depth: K >= N fires the whole batch at once */
#define K_PIPE_RING 8192

typedef struct k_pipe{
  cemon *loop;
  cemon_socket *sock;
  k_rx rx;
  int connected;
  int failed;
  int outstanding;
  k_u32 *cookies;          /* slot -> cookie (0 = free) */
  k_u64 *sent_us;          /* slot -> send timestamp */
  k_u64 lat_ring[K_PIPE_RING];
  int ring_head,ring_tail;
  k_u64 ring_drops;
  char stats[1024];        /* body of the last STATS reply (sentinel cookie below) */
  int stats_len;
} k_pipe;

#define K_PIPE_STATS_COOKIE 0xfffffff0u

static int k_pipe_frame(void *ud,k_u8 type,const k_u8 *payload,k_u32 size){
  k_pipe *p=(k_pipe *)ud;
  k_response_data r;
  int i;
  k_u64 now;
  if(!p||type!=K_RESPONSE) return -1;
  memset(&r,0,sizeof(r));
  if(k_response_decode(&r,payload,size)!=0) return -1;
  if(r.request_id==K_PIPE_STATS_COOKIE&&r.body){
    p->stats_len=(r.body_size<(k_u32)sizeof(p->stats)-1u)?(int)r.body_size:(int)sizeof(p->stats)-1;
    memcpy(p->stats,r.body,(size_t)p->stats_len);
    p->stats[p->stats_len]='\0';
  }
  for(i=0;i<K_PIPE_MAX;i++){
    if(p->cookies[i]==r.request_id){
      p->cookies[i]=0;
      if(p->outstanding>0) p->outstanding--;
      if(k_monotonic_us(&now)==0){
        int next=(p->ring_head+1)%K_PIPE_RING;
        if(next!=p->ring_tail){
          p->lat_ring[p->ring_head]=now-p->sent_us[i];
          p->ring_head=next;
        }else p->ring_drops++;
      }
      break;
    }
  }
  k_response_data_free(&r);
  return 0;
}
static void k_pipe_io(cemon_socket *sock,const cemon_event *event){
  k_pipe *p=(k_pipe *)cemon_getud(sock);
  if(!p||!event) return;
  if(event->type==CEMON_CONNECT){
    p->connected=1;
  }else if(event->type==CEMON_DATA){
    if(k_rx_feed(&p->rx,K_CLIENT_MAGIC,event->data,(k_u32)event->size,k_pipe_frame,p)!=0){
      p->failed=1;
      cemon_close(sock);
    }else if(cemon_recv(sock)!=0){
      p->failed=1;
      cemon_close(sock);
    }
  }else if(event->type==CEMON_EOF){
    p->failed=1;
    cemon_close(sock);
  }else if(event->type==CEMON_CLOSED){
    p->sock=0;
    p->failed=1;
  }
}
static int k_pipe_open(k_pipe *p,const char *host,unsigned short port,unsigned int timeout_ms){
  k_u64 start,now;
  if(!p||!host) return -1;
  memset(p,0,sizeof(*p));
  p->cookies=(k_u32 *)K_MALLOC(sizeof(k_u32)*K_PIPE_MAX);
  p->sent_us=(k_u64 *)K_MALLOC(sizeof(k_u64)*K_PIPE_MAX);
  if(!p->cookies||!p->sent_us) return -1;
  memset(p->cookies,0,sizeof(k_u32)*K_PIPE_MAX);
  memset(p->sent_us,0,sizeof(k_u64)*K_PIPE_MAX);
  p->loop=cemon_create();
  if(!p->loop) return -1;
  if(k_monotonic_us(&start)!=0) start=0;
  p->sock=cemon_tcp_connect(p->loop,k_numeric_host(host),port,k_pipe_io,p);
  if(!p->sock){
    cemon_destroy(p->loop);
    p->loop=0;
    return -1;
  }
  while(!p->connected&&!p->failed){
    if(cemon_poll(p->loop,20)!=0&&!p->connected) p->failed=1;
    if(start&&k_monotonic_us(&now)==0&&now-start>(k_u64)timeout_ms*1000u) p->failed=1;
  }
  if(!p->connected||p->failed){
    cemon_close(p->sock);
    cemon_destroy(p->loop);
    p->loop=0;
    p->sock=0;
    return -1;
  }
  return 0;
}
/* Returns the slot used, or -1 when K requests are already outstanding / on error. */
static int k_pipe_send(k_pipe *p,k_u8 type,const void *key,k_u32 key_len,const void *value,k_u32 value_len,k_u32 cookie){
  k_buf payload;
  int i,slot=-1;
  if(!p||!p->loop||!p->sock||p->failed) return -1;
  if(p->outstanding>=K_PIPE_MAX) return -1;
  for(i=0;i<K_PIPE_MAX;i++){
    if(p->cookies[i]==0){ slot=i; break; }
  }
  if(slot<0) return -1;
  if(k_request_payload(&payload,cookie,type,key,key_len,value,value_len)!=0) return -1;
  if(k_send_frame(p->sock,K_CLIENT_MAGIC,type,payload.data,payload.len,0)!=0){
    k_buf_free(&payload);
    return -1;
  }
  k_buf_free(&payload);
  if(cemon_recv(p->sock)!=0){
    p->failed=1;
    return -1;
  }
  p->cookies[slot]=cookie;
  if(k_monotonic_us(&p->sent_us[slot])!=0) p->sent_us[slot]=0;
  p->outstanding++;
  return slot;
}
static int k_pipe_pending(k_pipe *p){
  if(!p) return 0;
  return (p->ring_head-p->ring_tail+K_PIPE_RING)%K_PIPE_RING;
}
/* Poll until at least `want` completions are queued (or timeout).  Returns the
   number queued, or -1 on failure. */
static int k_pipe_await(k_pipe *p,int want,unsigned int timeout_ms){
  k_u64 start,now;
  if(!p) return -1;
  if(k_monotonic_us(&start)!=0) start=0;
  while(k_pipe_pending(p)<want&&!p->failed){
    if(cemon_poll(p->loop,1)!=0&&k_pipe_pending(p)<want) break;
    if(start&&k_monotonic_us(&now)==0&&now-start>(k_u64)timeout_ms*1000u) break;
  }
  if(p->failed) return -1;
  return k_pipe_pending(p);
}
/* Drain completed latencies into out[]; returns how many were copied. */
static int k_pipe_drain(k_pipe *p,k_u64 *out,int max){
  int n=0;
  if(!p||!out) return 0;
  while(n<max&&p->ring_tail!=p->ring_head){
    out[n++]=p->lat_ring[p->ring_tail];
    p->ring_tail=(p->ring_tail+1)%K_PIPE_RING;
  }
  return n;
}
/* Query K_REQ_STATS on this connection and return its body (server counters such as
   flush_batches/flush_writes: the direct measurement of group-commit batch size). */
static const char *k_pipe_fetch_stats(k_pipe *p,int timeout_ms){
  int guard=0;
  k_u64 start,now;
  if(!p||!p->loop) return 0;
  p->stats[0]='\0';
  p->stats_len=0;
  if(k_pipe_send(p,K_REQ_STATS,0,0,0,0,K_PIPE_STATS_COOKIE)<0) return 0;
  if(k_monotonic_us(&start)!=0) start=0;
  while(p->stats_len==0&&!p->failed){
    if(cemon_poll(p->loop,1)!=0&&p->stats_len==0) break;
    guard++;
    if(start&&k_monotonic_us(&now)==0&&now-start>(k_u64)timeout_ms*1000u) break;
    if(guard>2000000) break;
  }
  { k_u64 dummy; while(k_pipe_drain(p,&dummy,1)>0){} }   /* keep the latency ring clean */
  return p->stats_len?p->stats:"";
}
static void k_pipe_close(k_pipe *p){
  if(!p||!p->loop) return;
  if(p->sock) cemon_close(p->sock);
  if(!cemon_should_stop(p->loop)) cemon_stop(p->loop);
  { int guard=0; k_u64 s,n; if(k_monotonic_us(&s)!=0) s=0;
    while(cemon_poll(p->loop,1)==0&&guard<64){ guard++; if(s&&k_monotonic_us(&n)==0&&n-s>20000u) break; } }
  k_rx_free(&p->rx);
  if(p->cookies) K_FREE(p->cookies);
  if(p->sent_us) K_FREE(p->sent_us);
  cemon_destroy(p->loop);
  p->loop=0;
  p->sock=0;
  p->cookies=0;
  p->sent_us=0;
}

#include "bench_env.h"

#endif /* BENCH_CLIENT_H */
