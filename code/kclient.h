/* kclient.h -- the kynovo client state machine core.  Pure: depends only on
   kbase.h / kproto.h (never cemon/cli/kserver).  All link I/O goes through the
   injected k_client_transport vtable; time via the injected now_us field;
   output via the injected k_client_output sink.  The cemon backend and the CLI
   live in the app layer.  Single translation unit use.

   NOT self-contained: include kbase.h + kproto.h first (kproto.h pulls in
   kbase.h itself). */
#ifndef KCLIENT_H
#define KCLIENT_H

/* ---- client retry policy ---- */
#define K_CLIENT_RETRY_MS 100u
/* Bounded wait for an ALREADY-SENT request.  The retry machinery above only covers "leader
   unknown / reconnect"; without this, a server that accepts a request and then loses its
   result (gate stuck, dropped outcome, killed worker) leaves the client waiting forever and
   every later command is refused because pending stays non-null. */
#define K_CLIENT_REQUEST_TIMEOUT_MS 5000u
#define K_CLIENT_RETRY_MAX 50

/* The client state machine performs ALL link I/O through this vtable and never
   calls cemon directly (mirrors k_server_transport).  A deterministic test
   harness swaps in a capture/feed backend; the cemon backend lives in the app
   layer.  connect() returns an opaque socket handle (0 = failed); recv() arms a
   continuation read; close() tears the socket down. */
typedef struct k_client_app k_client_app;
typedef struct k_client_transport{
  const char *name;
  void *(*connect)(k_client_app *app,const char *host,unsigned short port);
  int   (*send)(k_client_app *app,void *sock,k_u32 magic,k_u8 type,const void *payload,k_u32 size,int control);
  int   (*recv)(k_client_app *app,void *sock);
  void  (*close)(k_client_app *app,void *sock);
} k_client_transport;
/* printf-like output sink for the state machine (logs and response bodies).
   The app layer backs this with cli_print; a test harness captures it to a
   buffer and asserts on the text. */
typedef void (*k_client_output)(void *ud,const char *fmt,...);
typedef struct k_pending{
  struct k_pending *next;
  k_u32 id;
  k_u8 type;
  k_u8 *payload;
  k_u32 size;
  int sent;
  k_u64 sent_us;       /* when this frame was handed to the transport (per-request latency) */
  k_u64 deadline_us;   /* bounded wait for THIS request (0 = no bound) */
} k_pending;
typedef struct k_client_app{
  const k_client_transport *transport;
  void *loop;                    /* opaque; transport->connect only (app: cemon loop) */
  k_client_output output;        /* printf-like output sink */
  void *output_ud;
  void *sock;                    /* opaque connection handle */
  k_rx rx;
  char hosts[K_MAX_NODES*2][K_HOST_MAX];  /* endpoints: [0..seed_count) seeds, rest = discovered members */
  unsigned short ports[K_MAX_NODES*2];
  int host_count;      /* total endpoints */
  int seed_count;      /* immutable seed prefix length */
  int host_index;      /* current connection target */
  int discovering;     /* 1 = auto-discovery MEMBERS query in flight (suppress print) */
  k_pending *pending;
  k_u32 next_id;
  int connected;
  int reconnect_pending;
  int preserve_pending;
  int retry_pending;
  int send_pending_now;   /* a frame is queued and must be sent FROM THE LOOP, never inside a
                             framework completion callback: a send queued inside the cemon
                             connect completion was never transmitted, so the server saw no
                             request at all and the CLI hung after CONNECT (the bench client
                             sends from its own loop and works). */
  int retry_count;
  k_u64 retry_deadline_us;
  k_u64 pending_deadline_us;   /* 0 = no request in flight */
  k_u32 request_timeout_ms;    /* 0 = use K_CLIENT_REQUEST_TIMEOUT_MS */
  k_u32 last_done_id;   /* id of the most recent request that received a response */
  k_u8 last_done_status;/* that response's status: callers gate on OK, not on output text */
  int inflight_limit;   /* how many requests may be in flight at once: 0/1 = one at a time (the
                           default, and what interactive commands rely on); >1 = pipelining.
                           The list was always able to hold several (responses match by id);
                           this field is the policy that allows it. */
  k_u32 done_count;     /* responses matched to a request (success or failure) */
  k_u32 error_count;    /* matched responses whose status was neither OK nor NOT_FOUND */
  void (*on_done)(void *ud,k_u32 id,k_u8 status,k_u64 duration_us); /* per-completion hook (0=none) */
  void *on_done_ud;
  k_u64 now_us;        /* injected current time (0 = unknown) */
  int stopping;
} k_client_app;
static void k_pending_free(k_pending *pending){
  while(pending){
    k_pending *next=pending->next;
    K_FREE(pending->payload);
    K_FREE(pending);
    pending=next;
  }
}
/* How many requests are in flight. */
static k_u32 k_client_inflight_count(const k_client_app *app){
  const k_pending *pending;
  k_u32 count=0;
  if(!app) return 0;
  for(pending=app->pending;pending;pending=pending->next) count++;
  return count;
}
/* The configured in-flight capacity (never below 1 = today's one-at-a-time behaviour). */
static int k_client_inflight_limit(const k_client_app *app){
  return app&&app->inflight_limit>1?app->inflight_limit:1;
}
/* Append a freshly built pending to the END of the list: with pipelining the send order must
   follow the submission order (the list is walked forward when matching responses). */
static void k_client_append_pending(k_client_app *app,k_pending *pending){
  k_pending **link;
  for(link=&app->pending;*link;link=&(*link)->next) {}
  *link=pending;
  app->retry_count=0;
  app->retry_pending=0;
}
/* After a reconnect/redirect every in-flight request must go out again on the NEW
   connection.  Delivery is at-least-once (a request that was already executed before the
   link died may be repeated) - the store's commands are idempotent or carry explicit
   preconditions, which is the documented contract. */
static void k_client_mark_all_unsent(k_client_app *app){
  k_pending *pending;
  if(!app) return;
  for(pending=app->pending;pending;pending=pending->next) pending->sent=0;
}
static int k_client_send_pending(k_client_app *app){
  k_pending *pending;
  if(!app||!app->connected||!app->sock) return 0;
  for(pending=app->pending;pending;pending=pending->next){
    if(!pending->sent){
      if(app->transport->send(app,app->sock,K_CLIENT_MAGIC,pending->type,pending->payload,pending->size,0)!=0) return -1;
      pending->sent=1;
      pending->sent_us=app->now_us;
    }
  }
  return 0;
}
static void k_client_remove_pending(k_client_app *app,k_pending *pending){
  k_pending **link;
  if(!app||!pending) return;
  for(link=&app->pending;*link;link=&(*link)->next){
    if(*link==pending){
      *link=pending->next;
      break;
    }
  }
  pending->next=0;
  k_pending_free(pending);
}
/* Arm a bounded retry for the pending request when the leader is unknown or a
   reconnect fails: keep the request (do not drop it) and resend it after a short
   delay, mirroring k_follow_call's retry loop.  Returns 0 if armed, -1 when the
   retry bound is exhausted (caller should give up). */
static int k_client_retry_arm(k_client_app *app){
  if(!app) return -1;
  if(app->retry_count>=K_CLIENT_RETRY_MAX) return -1;
  app->retry_count++;
  app->retry_deadline_us=app->now_us+(k_u64)K_CLIENT_RETRY_MS*1000u;
  app->retry_pending=1;
  return 0;
}
/* Return the index of endpoint host:port in the client's endpoint list, or -1
   if absent.  Used to deduplicate the list and to locate a redirect target
   without disturbing the known topology. */
static int k_client_find_endpoint(const k_client_app *app,const char *host,unsigned short port){
  int i;
  if(!app||!host) return -1;
  for(i=0;i<app->host_count;i++){
    if(app->ports[i]==port&&strcmp(app->hosts[i],host)==0) return i;
  }
  return -1;
}
/* Apply a MEMBERS response ("id@host:client_port:peer_port, ...") to the endpoint
   list, replacing the discovered-member portion [seed_count..] while keeping the
   immutable seed prefix.  Reuses k_cluster_parse (same wire format as cluster-spec). */
static void k_client_apply_members(k_client_app *app,const char *text){
  k_cluster cluster;
  char buf[2048];
  int i;
  if(!app||!text||strlen(text)>=sizeof(buf)) return;
  memcpy(buf,text,strlen(text)+1u);
  if(k_cluster_parse(&cluster,buf)!=0) return;
  app->host_count=app->seed_count;
  for(i=0;i<cluster.count&&app->host_count<K_MAX_NODES*2;i++){
    /* Skip any endpoint already known: a member whose address equals the seed
       would otherwise be listed twice, and a duplicate entry wastes a failover
       retry on the same dead node. */
    if(k_client_find_endpoint(app,cluster.nodes[i].host,cluster.nodes[i].client_port)>=0) continue;
    memcpy(app->hosts[app->host_count],cluster.nodes[i].host,strlen(cluster.nodes[i].host)+1u);
    app->ports[app->host_count]=cluster.nodes[i].client_port;
    app->host_count++;
  }
}
/* Parse a comma-separated seed list "host:port,host:port,..." into the endpoint
   list prefix [0..seed_count).  The client connects to these in order and appends
   members discovered via MEMBERS. */
static int k_client_seed_parse(k_client_app *app,const char *seed_list){
  const char *cursor,*end;
  char *colon;
  char host[K_HOST_MAX];
  int port;
  size_t item_len;
  int count=0;
  if(!app||!seed_list||!seed_list[0]) return -1;
  cursor=seed_list;
  while(*cursor&&count<K_MAX_NODES*2){
    end=strchr(cursor,',');
    item_len=end?(size_t)(end-cursor):strlen(cursor);
    if(!item_len||item_len>=sizeof(host)) return -1;
    memcpy(host,cursor,item_len);
    host[item_len]='\0';
    colon=strchr(host,':');
    if(!colon) return -1;
    *colon='\0';
    if(!host[0]||strlen(host)>=K_HOST_MAX||k_parse_uint(colon+1,65535,&port)!=0) return -1;
    memcpy(app->hosts[count],host,strlen(host)+1u);
    app->ports[count]=(unsigned short)port;
    count++;
    if(!end) break;
    cursor=end+1;
  }
  if(!count) return -1;
  app->host_count=count;
  app->seed_count=count;
  return 0;
}
static void k_client_discover(k_client_app *app);
static int k_client_response_frame(void *ud,k_u8 type,const k_u8 *payload,k_u32 size){
  k_client_app *app=(k_client_app *)ud;
  k_response_data response;
  k_pending *pending;
  if(!app||type!=K_RESPONSE||k_response_decode(&response,payload,size)!=0) return -1;
  for(pending=app->pending;pending;pending=pending->next) if(pending->id==response.request_id) break;
  if(!pending){
    k_response_data_free(&response);
    return 0;
  }
  app->last_done_id=pending->id;
  app->last_done_status=response.status;
  app->done_count++;
  if(response.status!=K_STATUS_OK&&response.status!=K_STATUS_NOT_FOUND&&response.status!=K_STATUS_REDIRECT) app->error_count++;
  if(app->on_done){
    k_u64 elapsed=(pending->sent_us&&app->now_us>=pending->sent_us)?(app->now_us-pending->sent_us):0u;
    app->on_done(app->on_done_ud,pending->id,response.status,elapsed);
  }
  if(response.status==K_STATUS_REDIRECT&&response.host[0]&&response.port){
    /* Switch to the leader WITHOUT clobbering the endpoint list: overwriting
       hosts[host_index] here would erase the node we are abandoning, so a later
       leader death could leave no address for the sole surviving follower. */
    int target=k_client_find_endpoint(app,response.host,response.port);
    if(target<0){
      target=(app->host_count<K_MAX_NODES*2)?app->host_count++:app->host_index;
      memcpy(app->hosts[target],response.host,strlen(response.host)+1u);
      app->ports[target]=response.port;
    }
    app->host_index=target;
    k_client_mark_all_unsent(app);
    app->preserve_pending=1;
    app->reconnect_pending=1;
    app->output(app->output_ud,"redirecting to %s:%u",app->hosts[app->host_index],(unsigned)app->ports[app->host_index]);
    k_response_data_free(&response);
    return 0;
  }
  if(response.status==K_STATUS_OK){
    if(pending->type==K_REQ_MEMBERS){
      k_client_apply_members(app,(const char *)response.body);
      if(!app->discovering) app->output(app->output_ud,"%.*s",(int)response.body_size,response.body?(const char *)response.body:"");
    }else if(pending->type==K_REQ_GET||pending->type==K_REQ_MGET||pending->type==K_REQ_RGET||pending->type==K_REQ_MSET||pending->type==K_REQ_RSET||pending->type==K_REQ_MDEL||pending->type==K_REQ_RDEL||pending->type==K_REQ_COUNT||pending->type==K_REQ_MIN||pending->type==K_REQ_MAX||pending->type==K_REQ_CAS||pending->type==K_REQ_FCALL||pending->type==K_REQ_INFO||pending->type==K_REQ_STATS||pending->type==K_REQ_HELP||pending->type==K_REQ_TOPOLOGY||pending->type==K_REQ_SHUTDOWN||pending->type==K_REQ_MEMBER) app->output(app->output_ud,"%.*s",(int)response.body_size,response.body?(const char *)response.body:"");
    else app->output(app->output_ud,"ok");
  }else if(response.status==K_STATUS_NOT_FOUND) app->output(app->output_ud,"(not found)");
  else if(response.status==K_STATUS_CONFLICT) app->output(app->output_ud,"conflict: %.*s",(int)response.body_size,response.body?(const char *)response.body:"");
  else if(response.status==K_STATUS_REDIRECT){
    if(k_client_retry_arm(app)==0){
      pending->sent=0;
      app->output(app->output_ud,"leader unknown, retrying");
      k_response_data_free(&response);
      return 0;
    }
    app->output(app->output_ud,"leader unavailable");
  }
  else app->output(app->output_ud,"error: %.*s",(int)response.body_size,response.body?(const char *)response.body:"");
  /* A MEMBERS discovery that terminated for ANY reason (success or failure) must
     clear the in-flight flag; otherwise a later manual `members` is silently
     suppressed and k_client_discover's `discovering` check prevents re-arming. */
  if(pending->type==K_REQ_MEMBERS) app->discovering=0;
  k_client_remove_pending(app,pending);
  k_response_data_free(&response);
  return 0;
}
/* ---- pure inbound connection-state handlers (feed the state machine) ----
   These carry the client's failover/redirect/retry strategy with zero cemon or
   link I/O: the io callback below is the only thing that touches sockets, and
   it just dispatches here then arms the continuation read.  A deterministic
   harness feeds CEMON_* events by calling these directly and asserts on the
   resulting output frames. */
static int k_client_on_connected(k_client_app *app){
  if(!app) return -1;
  app->connected=1;
  k_client_mark_all_unsent(app);
  app->preserve_pending=0;
  app->output(app->output_ud,"connected to %s:%u",app->hosts[app->host_index],(unsigned)app->ports[app->host_index]);
  app->send_pending_now=1;   /* flushed by the loop, not from inside the connect completion */
  k_client_discover(app);
  return 0;
}
static int k_client_on_received(k_client_app *app,const void *data,k_u32 size){
  if(!app) return -1;
  return k_rx_feed(&app->rx,K_CLIENT_MAGIC,data,size,k_client_response_frame,app);
}
static void k_client_on_closed(k_client_app *app){
  if(!app) return;
  app->sock=0;
  app->connected=0;
  k_rx_free(&app->rx);
  if(!app->preserve_pending&&app->pending){
    k_pending *pending;
    app->output(app->output_ud,"connection closed; pending request failed");
    for(pending=app->pending;pending;pending=pending->next) app->error_count++;
    if(app->pending->type==K_REQ_MEMBERS) app->discovering=0;
    k_pending_free(app->pending);
    app->pending=0;
    /* the request is gone: a pending retry (leader-unknown) would fire a
       pointless reconnect on a now-empty queue, so disarm it */
    app->retry_pending=0;
    app->retry_count=0;
  }
  app->preserve_pending=0;
  if(!app->stopping&&!app->reconnect_pending){
    /* spontaneous close (server died / connect failed): fail over and reconnect */
    if(app->host_count>1) app->host_index=(app->host_index+1)%app->host_count;
    app->reconnect_pending=1;
  }
}

static int k_client_connect(k_client_app *app){
  if(!app||app->sock) return -1;
  app->sock=app->transport->connect(app,k_numeric_host(app->hosts[app->host_index]),app->ports[app->host_index]);
  return app->sock?0:-1;
}
static int k_client_queue(k_client_app *app,k_u8 type,const void *key,k_u32 key_len,const void *value,k_u32 value_len){
  k_pending *pending;
  k_buf payload;
  if(!app) return -1;
  if(app->pending&&app->inflight_limit<=1){
    app->output(app->output_ud,"wait for the current request");
    return 0;
  }
  if((int)k_client_inflight_count(app)>=k_client_inflight_limit(app)){
    app->output(app->output_ud,"wait for the current request");
    return 0;
  }
  pending=(k_pending *)K_CALLOC(1,sizeof(*pending));
  if(!pending) return -1;
  pending->id=++app->next_id;
  pending->type=type;
  if(k_request_payload(&payload,pending->id,type,key,key_len,value,value_len)!=0){
    K_FREE(pending);
    return -1;
  }
  pending->payload=payload.data;
  pending->size=payload.len;
  pending->sent_us=0;
  pending->deadline_us=app->now_us+(k_u64)(app->request_timeout_ms?app->request_timeout_ms:K_CLIENT_REQUEST_TIMEOUT_MS)*1000u;
  k_client_append_pending(app,pending);
  app->pending_deadline_us=pending->deadline_us;   /* kept for callers that watch a single bound */
  k_response_data_free(0);
  if(app->connected) app->send_pending_now=1;   /* sent from the loop, never from this call site:
                                                   call sites include the connect completion */
  return 0;
}
static void k_client_discover(k_client_app *app){
  if(!app||app->pending||app->discovering) return;
  app->discovering=1;
  if(k_client_queue(app,K_REQ_MEMBERS,0,0,0,0)!=0) app->discovering=0;
}
static int k_client_queue_rget(k_client_app *app,const void *begin,k_u32 begin_len,const void *end,k_u32 end_len,int direction,k_u32 limit){
  k_pending *pending;
  k_buf payload;
  if(!app) return -1;
  if(app->pending&&app->inflight_limit<=1){
    app->output(app->output_ud,"wait for the current request");
    return 0;
  }
  if((int)k_client_inflight_count(app)>=k_client_inflight_limit(app)){
    app->output(app->output_ud,"wait for the current request");
    return 0;
  }
  pending=(k_pending *)K_CALLOC(1,sizeof(*pending));
  if(!pending) return -1;
  pending->id=++app->next_id;
  pending->type=K_REQ_RGET;
  if(k_rget_request_payload(&payload,pending->id,begin,begin_len,end,end_len,direction,limit)!=0){
    K_FREE(pending);
    return -1;
  }
  pending->payload=payload.data;
  pending->size=payload.len;
  pending->sent_us=0;
  pending->deadline_us=app->now_us+(k_u64)(app->request_timeout_ms?app->request_timeout_ms:K_CLIENT_REQUEST_TIMEOUT_MS)*1000u;
  k_client_append_pending(app,pending);
  app->pending_deadline_us=pending->deadline_us;   /* kept for callers that watch a single bound */
  k_response_data_free(0);
  if(app->connected) app->send_pending_now=1;   /* sent from the loop, never from this call site:
                                                   call sites include the connect completion */
  return 0;
}
static int k_client_queue_member(k_client_app *app,int subcmd,const int *ids,int id_count,const char (*hosts)[K_HOST_MAX],const unsigned short *client_ports,const unsigned short *peer_ports){
  k_pending *pending;
  k_buf payload;
  int i;
  if(!app) return -1;
  if(app->pending&&app->inflight_limit<=1){ app->output(app->output_ud,"wait for the current request"); return 0; }
  if((int)k_client_inflight_count(app)>=k_client_inflight_limit(app)){ app->output(app->output_ud,"wait for the current request"); return 0; }
  pending=(k_pending *)K_CALLOC(1,sizeof(*pending));
  if(!pending) return -1;
  pending->id=++app->next_id;
  pending->type=K_REQ_MEMBER;
  memset(&payload,0,sizeof(payload));
  k_buf_u32(&payload,pending->id);
  k_buf_u8(&payload,(k_u8)subcmd);
  k_buf_u32(&payload,(k_u32)id_count);
  if(subcmd==K_MEMBER_ADD){
    for(i=0;i<id_count;i++){
      k_u32 host_len=(k_u32)strlen(hosts[i]);
      k_buf_u32(&payload,(k_u32)ids[i]);
      k_buf_u32(&payload,host_len);
      k_buf_bytes(&payload,hosts[i],host_len);
      k_buf_u16(&payload,client_ports[i]);
      k_buf_u16(&payload,peer_ports[i]);
    }
  }else{
    for(i=0;i<id_count;i++) k_buf_u32(&payload,(k_u32)ids[i]);
  }
  if(payload.err||payload.len>K_FRAME_MAX){ k_buf_free(&payload); K_FREE(pending); return -1; }
  pending->payload=payload.data;
  pending->size=payload.len;
  pending->sent_us=0;
  pending->deadline_us=app->now_us+(k_u64)(app->request_timeout_ms?app->request_timeout_ms:K_CLIENT_REQUEST_TIMEOUT_MS)*1000u;
  k_client_append_pending(app,pending);
  app->pending_deadline_us=pending->deadline_us;   /* kept for callers that watch a single bound */
  k_response_data_free(0);
  if(app->connected) app->send_pending_now=1;   /* sent from the loop, never from this call site:
                                                   call sites include the connect completion */
  return 0;
}

/* ---- deterministic per-poll advance (pure: reconnect + retry strategy) ----
   The cemon poll loop feeds the current wall-clock time, then this advances the
   reconnect/retry state machine.  A test harness calls it directly with an
   injected now_us. */
/* The client has a request in flight: callers must pump the loop instead of dispatching a new
   command (the enqueue would be refused and - with no pump - nothing would ever progress). */
static int k_client_busy(const k_client_app *app){
  return app&&app->pending!=0;
}
static void k_client_poll(k_client_app *app){
  if(!app) return;
  if(app->reconnect_pending){
    if(app->sock) app->transport->close(app,app->sock);
    else{
      app->reconnect_pending=0;
      if(k_client_connect(app)!=0){
        if(k_client_retry_arm(app)==0){
          app->output(app->output_ud,"reconnect failed, retrying");
        }else{
          app->output(app->output_ud,"redirect connection failed");
          k_pending_free(app->pending);app->pending=0;
        }
      }
    }
  }
  if(app->pending){
    /* Bounded wait, swept over the LIST because pipelining can have several in flight.  Each
       expired request is reported explicitly and released so the session stays usable; a late
       response is dropped by the id lookup in the response handler. */
    k_pending **link=&app->pending;
    while(*link){
      k_pending *pending=*link;
      if(pending->deadline_us&&app->now_us>=pending->deadline_us){
        app->output(app->output_ud,"request timed out: no response from the server (outcome unknown)");
        /* A timed-out discovery must clear its flag, otherwise every later MEMBERS reply is
           printed as the automatic one and the user sees nothing at all. */
        if(pending->type==K_REQ_MEMBERS) app->discovering=0;
        app->error_count++;
        *link=pending->next;
        pending->next=0;
        k_pending_free(pending);
        continue;
      }
      link=&(*link)->next;
    }
    app->pending_deadline_us=app->pending?app->pending->deadline_us:0;
  }
  if(app->retry_pending&&app->now_us>=app->retry_deadline_us){
    app->retry_pending=0;
    if(app->sock){
      if(k_client_send_pending(app)!=0) app->transport->close(app,app->sock);
    }else if(k_client_connect(app)!=0){
      if(k_client_retry_arm(app)!=0){
        app->output(app->output_ud,"leader unavailable");
        k_pending_free(app->pending);app->pending=0;
      }
    }
  }
  /* Flush a queued frame here, in the loop, so that no request is ever sent from inside a
     framework completion callback (see the send_pending_now comment in k_client_app). */
  if(app->send_pending_now){
    app->send_pending_now=0;
    if(app->connected&&app->sock&&k_client_send_pending(app)!=0) app->transport->close(app,app->sock);
  }
}

#endif
