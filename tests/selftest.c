/* ============================================================================
 * selftest.c -- the kynovo single-process self-test.
 *
 *   selftest
 *
 *   build: ./build.sh selftest
 * ============================================================================
 */
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

#define K_LEN(s) ((k_u32)(sizeof(s)-1u))
/* ================= App transport: send one framed message via cemon ================= */
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

/* ================= App layer: cemon adapter =================
   io callbacks, cemon transport backend, tick, serve, run -- everything that
   touches cemon stays here (NOT in kserver.h) so the server core compiles and
   tests without a socket layer. */
static void k_server_peer_io(cemon_socket *sock,const cemon_event *event){
  void *ud;
  k_server *server;
  k_conn *conn;
  if(!sock||!event) return;
  ud=cemon_getud(sock);
  if(!ud) return;
  if(*(const int *)ud==K_UD_SERVER){
    server=(k_server *)ud;
    if(event->type==CEMON_ACCEPT){ k_server_peer_accepted(server,event->data); return; }
    return;
  }
  if(*(const int *)ud!=K_UD_CONN) return;
  conn=(k_conn *)ud;
  if(event->type==CEMON_CONNECT){
    k_server_peer_dialed(conn);
  }else if(event->type==CEMON_DATA){
    k_server_peer_received(conn,event->data,(k_u32)event->size);
  }else if(event->type==CEMON_EOF){
    conn->server->transport->close(conn->sock);
  }else if(event->type==CEMON_CLOSED){
    k_conn_closed(conn);
  }
}
static void k_server_client_io(cemon_socket *sock,const cemon_event *event){
  void *ud;
  k_server *server;
  k_conn *conn;
  if(!sock||!event) return;
  ud=cemon_getud(sock);
  if(!ud) return;
  if(*(const int *)ud==K_UD_SERVER){
    server=(k_server *)ud;
    if(event->type==CEMON_ACCEPT){ k_server_client_accepted(server,event->data); return; }
    return;
  }
  if(*(const int *)ud!=K_UD_CONN) return;
  conn=(k_conn *)ud;
  if(event->type==CEMON_DATA){
    k_server_client_received(conn,event->data,(k_u32)event->size);
  }else if(event->type==CEMON_EOF){
    conn->server->transport->close(conn->sock);
  }else if(event->type==CEMON_CLOSED){
    k_conn_closed(conn);
  }
}
/* cemon transport backend: the production k_server_transport implementation.
   The server core sees only the vtable; this backend adapts it to real sockets. */
static int k_transport_cemon_send(void *sock,k_u32 magic,k_u8 type,const void *payload,k_u32 size,int control){
  return k_send_frame((cemon_socket *)sock,magic,type,payload,size,control);
}
static void k_transport_cemon_close(void *sock){
  cemon_close((cemon_socket *)sock);
}
static int k_transport_cemon_recv(void *sock){
  return cemon_recv((cemon_socket *)sock);
}
static void *k_transport_cemon_dial(k_server *server,const k_node_spec *node){
  return cemon_tcp_connect((cemon *)server->loop,k_numeric_host(node->host),node->peer_port,k_server_peer_io,0);
}
static void k_transport_cemon_setud(void *sock,void *ud){
  cemon_setud((cemon_socket *)sock,ud);
}
static const k_server_transport k_transport_cemon={"cemon",k_transport_cemon_send,k_transport_cemon_close,k_transport_cemon_recv,k_transport_cemon_dial,k_transport_cemon_setud};
/* Reference production driver (kept in step with code/kdbsvr.c): the same
   sub-millisecond-carrying clock, so this harness cannot mask a logical clock
   that runs faster than wall time (k_server_elapsed_step in kserver.h). */
static void k_server_tick(cemon *loop,void *ud){
  k_server *server=(k_server *)ud;
  k_u64 now_us;
  unsigned int elapsed_ms;
  (void)loop;
  if(!server) return;
  if(k_monotonic_us(&now_us)!=0) return;
  elapsed_ms=k_server_elapsed_step(now_us,&server->last_tick_us,1000u);
  if(!elapsed_ms) return;
  k_server_advance(server,elapsed_ms);
}
/* App-layer cemon attachment (stays in the app, not kserver.h): bind the event
   loop, listen on the client/peer ports, arm the tick timer, then admit and
   run the initial reconnect.  k_server_open above is pure init (no link I/O). */
static void k_server_on_stop_cemon(void *loop){
  cemon_stop((cemon *)loop);
}
static int k_server_serve(k_server *server,cemon *loop){
  int local_index;
  local_index=k_cluster_index(&server->cluster,server->id);
  if(local_index<0) return -1;
  server->loop=loop;
  server->transport=&k_transport_cemon;
  server->on_stop=k_server_on_stop_cemon;
  if(!cemon_tcp_listen(loop,k_numeric_host(server->cluster.nodes[local_index].host),server->peer_port,k_server_peer_io,server)) return -1;
  if(!cemon_tcp_listen(loop,k_numeric_host(server->cluster.nodes[local_index].host),server->client_port,k_server_client_io,server)) return -1;
  if(!cemon_after(loop,server->cfg.poll_ms,server->cfg.poll_ms,k_server_tick,server)) return -1;
  server->admission=1;
  if(k_monotonic_us(&server->last_tick_us)!=0) server->last_tick_us=0;
  k_server_reconnect(server);
  return 0;
}
static int k_server_run(k_server *server){
  int rc=0;
  while(cemon_poll((cemon *)server->loop,(int)server->cfg.poll_ms)==0){}
  if(!server->stopped||server->fatal) rc=-1;
  return rc;
}

/* ================= Test: sync/follow helpers ================= */
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
} k_sync_client;
static int k_sync_response_frame(void *ud,k_u8 type,const k_u8 *payload,k_u32 size){
  k_sync_client *client=(k_sync_client *)ud;
  if(!client||type!=K_RESPONSE||k_response_decode(&client->response,payload,size)!=0) return -1;
  client->done=1;
  cemon_stop(client->loop);
  return 0;
}
static void k_sync_client_io(cemon_socket *sock,const cemon_event *event){
  k_sync_client *client=(k_sync_client *)cemon_getud(sock);
  if(!client||!event) return;
  if(event->type==CEMON_CONNECT){
    if(k_send_frame(sock,K_CLIENT_MAGIC,client->request_type,client->payload,client->payload_size,0)!=0||cemon_recv(sock)!=0){
      client->failed=1;
      cemon_close(sock);
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
static int k_sync_call_payload(const char *host,unsigned short port,k_u8 type,const k_u8 *payload,k_u32 payload_size,unsigned int timeout_ms,k_response_data *response){
  k_sync_client client;
  k_u64 start_us,now_us;
  int poll_rc;
  if(!host||!response||(payload_size&&!payload)) return -1;
  memset(&client,0,sizeof(client));
  client.payload=payload;
  client.payload_size=payload_size;
  client.request_type=type;
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
static int k_sync_rget_call(const char *host,unsigned short port,const void *begin,k_u32 begin_len,const void *end,k_u32 end_len,int direction,k_u32 limit,unsigned int timeout_ms,k_response_data *response){
  k_buf payload;
  int rc;
  if(k_rget_request_payload(&payload,1u,begin,begin_len,end,end_len,direction,limit)!=0) return -1;
  rc=k_sync_call_payload(host,port,K_REQ_RGET,payload.data,payload.len,timeout_ms,response);
  k_buf_free(&payload);
  return rc;
}
static int k_follow_call(const k_cluster *cluster,int start_index,k_u8 type,const void *key,k_u32 key_len,const void *value,k_u32 value_len,k_response_data *response){
  int index=start_index,attempt,next_index;
  k_response_data current;
  for(attempt=0;attempt<100;attempt++){
    memset(&current,0,sizeof(current));
    if(index>=0&&index<cluster->count&&k_sync_call(cluster->nodes[index].host,cluster->nodes[index].client_port,type,key,key_len,value,value_len,1000u,&current)==0){
      if(current.status!=K_STATUS_REDIRECT){
        *response=current;
        return 0;
      }
      next_index=k_cluster_index(cluster,current.leader_id);
      k_response_data_free(&current);
      if(next_index>=0) index=next_index;
    }
    runtime_msleep(50u);
  }
  return -1;
}
static int k_follow_member(const k_cluster *cluster,int start_index,int subcmd,const int *ids,int id_count,const char (*hosts)[K_HOST_MAX],const unsigned short *client_ports,const unsigned short *peer_ports,k_response_data *response){
  int index=start_index,attempt,next_index,i;
  int catchup_retries=0;
  k_response_data current;
  k_buf payload;
  memset(&payload,0,sizeof(payload));
  k_buf_u32(&payload,1u);
  k_buf_u8(&payload,(k_u8)subcmd);
  k_buf_u32(&payload,(k_u32)id_count);
  if(subcmd==K_MEMBER_ADD){
    for(i=0;i<id_count;i++){
      if(hosts){
        k_u32 host_len=(k_u32)strlen(hosts[i]);
        k_buf_u32(&payload,(k_u32)ids[i]);
        k_buf_u32(&payload,host_len);
        k_buf_bytes(&payload,hosts[i],host_len);
        k_buf_u16(&payload,client_ports[i]);
        k_buf_u16(&payload,peer_ports[i]);
      }else{
        int node_index=k_cluster_index(cluster,ids[i]);
        if(node_index<0){ k_buf_free(&payload); return -1; }
        k_buf_u32(&payload,(k_u32)ids[i]);
        k_buf_u32(&payload,(k_u32)strlen(cluster->nodes[node_index].host));
        k_buf_bytes(&payload,cluster->nodes[node_index].host,(k_u32)strlen(cluster->nodes[node_index].host));
        k_buf_u16(&payload,cluster->nodes[node_index].client_port);
        k_buf_u16(&payload,cluster->nodes[node_index].peer_port);
      }
    }
  }else{
    for(i=0;i<id_count;i++) k_buf_u32(&payload,(k_u32)ids[i]);
  }
  if(payload.err){ k_buf_free(&payload); return -1; }
  for(attempt=0;attempt<100;attempt++){
    memset(&current,0,sizeof(current));
    if(index>=0&&index<cluster->count&&k_sync_call_payload(cluster->nodes[index].host,cluster->nodes[index].client_port,K_REQ_MEMBER,payload.data,payload.len,5000u,&current)==0){
      if(current.status==K_STATUS_ERROR&&current.body_size>=15u&&current.body&&memcmp(current.body,"catch-up failed",15u)==0&&catchup_retries<10){
        /* catch-up hit its election_min*10 deadline, usually because a
           concurrent snapshot delayed the new peer; retry once it lands */
        catchup_retries++;
        k_response_data_free(&current);
        runtime_msleep(300u);
        continue;
      }
      if(current.status!=K_STATUS_REDIRECT){
        *response=current;
        k_buf_free(&payload);
        return 0;
      }
      next_index=k_cluster_index(cluster,current.leader_id);
      k_response_data_free(&current);
      if(next_index>=0) index=next_index;
    }
    runtime_msleep(50u);
  }
  k_buf_free(&payload);
  return -1;
}
/* ================= Test: embedded cluster harness ================= */
static void k_server_worker(runtime_ctx *runtime,void *arg){
  runtime_fn fn;
  void *task_arg=0;
  k_server *server;
  int rc;
  (void)arg;
  rc=runtime_task_poll(runtime,-1,&fn,&task_arg);
  if(rc<=0||!task_arg){
    runtime_worker_ready(runtime);
    runtime_worker_exit(runtime);
    return;
  }
  server=(k_server *)task_arg;
  server->runtime=runtime;
  if(k_server_open(server)==0){
    cemon *loop=cemon_create();
    if(loop&&k_server_serve(server,loop)==0){
      server->started=1;
      runtime_worker_ready(runtime);
      k_server_run(server);
    }else{
      server->started=-1;
      runtime_worker_ready(runtime);
    }
    if(loop) cemon_destroy(loop);
  }else{
    server->started=-1;
    runtime_worker_ready(runtime);
  }
  k_server_release(server);
  runtime_worker_exit(runtime);
}
static unsigned int k_process_id(void){
#if defined(_WIN32)
  return (unsigned int)GetCurrentProcessId();
#else
  return (unsigned int)getpid();
#endif
}
static void k_embedded_cluster(k_cluster *cluster,unsigned short base_port,int base_id){
  int i;
  memset(cluster,0,sizeof(*cluster));
  cluster->count=3;
  for(i=0;i<3;i++){
    cluster->nodes[i].id=base_id+i;
    strcpy(cluster->nodes[i].host,"127.0.0.1");
    cluster->nodes[i].client_port=(unsigned short)(base_port+i);
    cluster->nodes[i].peer_port=(unsigned short)(base_port+10+i);
  }
}
static void k_embedded_bases(k_server servers[3],const k_cluster *cluster,const char *tag,int base_id){
  k_u64 now_us=0;
  unsigned int pid=k_process_id();
  int i;
  k_monotonic_us(&now_us);
  for(i=0;i<3;i++){
    char base[K_URI_MAX];
    sprintf(base,"disk://kdb-%s-%u-%" K_U64_FMT "-n%d",tag,pid,now_us,base_id+i);
    k_server_init(&servers[i],base_id+i,cluster->nodes[i].client_port,cluster->nodes[i].peer_port,base,cluster);
  }
}
static int k_wal_files_valid(const char *base){
  k_wal_meta_state meta;
  char path[K_URI_MAX];
  k_u8 header[K_WAL_HEADER_SIZE];
  vfs_file *file;
  if(k_wal_meta_load(base,&meta)!=1||meta.generation==0||k_path_wal_segment(path,base,meta.record.segment)!=0) return 0;
  file=vfs_open(path);
  if(!file) return 0;
  if(vfs_read(file,meta.record.offset,header,sizeof(header))!=0){
    vfs_close(file);
    return 0;
  }
  vfs_close(file);
  return k_read_u32(header)==K_WAL_MAGIC&&k_read_u32(header+4)==K_WAL_VERSION&&k_read_u64(header+8)==meta.generation&&meta.record_size==(k_u64)K_WAL_HEADER_SIZE+(k_u64)k_read_u32(header+16);
}
static void k_selftest_snap_sweep(const char *base);   /* defined below; used here and in the single-node cleanup */
static void k_embedded_clean(k_server servers[3]){
  int i;
  char path[K_URI_MAX];
  for(i=0;i<3;i++){
    k_restore restore;
    k_wal_meta_state meta;
    k_u64 segment,max_segment;
    int wal_exists=0;
    int restore_rc=k_state_load(servers[i].base,&restore,&meta,&wal_exists);
    if(restore_rc>0){
      if(restore.persist.last_included_index>0&&k_path_snapshot(path,servers[i].base,restore.persist.last_included_index)==0) vfs_unlink(path);
      k_restore_free(&restore);
    }
    if(servers[i].snapshot.index>0&&k_path_snapshot(path,servers[i].base,servers[i].snapshot.index)==0) vfs_unlink(path);
    if(servers[i].last_snapshot_task_index>0&&k_path_snapshot(path,servers[i].base,servers[i].last_snapshot_task_index)==0) vfs_unlink(path);
    if(wal_exists){
      max_segment=meta.record.segment>meta.next.segment?meta.record.segment:meta.next.segment;
      for(segment=0;;segment++){
        if(k_path_wal_segment(path,servers[i].base,segment)==0) vfs_unlink(path);
        if(segment==max_segment) break;
      }
    }
    if(k_path_suffix(path,servers[i].base,".wal.meta")==0) vfs_unlink(path);
    if(k_path_suffix(path,servers[i].base,".cfg")==0) vfs_unlink(path);
    k_selftest_snap_sweep(servers[i].base);
  }
}
static runtime_ctx *k_embedded_start(k_server servers[3]){
  runtime_ctx *runtime;
  int i;
  runtime=runtime_create("thread",3,k_server_worker,0);
  if(!runtime) return 0;
  for(i=0;i<3;i++){
    if(runtime_task_post(runtime,0,&servers[i])!=0){
      runtime_stop(runtime);
      runtime_destroy(runtime);
      return 0;
    }
  }
  runtime_wait_workers_ready(runtime);
  for(i=0;i<3;i++){
    if(servers[i].started!=1){
      runtime_stop(runtime);
      runtime_wait_workers_exit(runtime);
      runtime_destroy(runtime);
      return 0;
    }
  }
  return runtime;
}
static int k_embedded_shutdown(const k_cluster *cluster){
  int i,rc=0;
  k_response_data response;
  for(i=0;i<cluster->count;i++){
    memset(&response,0,sizeof(response));
    if(k_sync_call(cluster->nodes[i].host,cluster->nodes[i].client_port,K_REQ_SHUTDOWN,0,0,0,0,2000u,&response)!=0||response.status!=K_STATUS_OK) rc=-1;
    k_response_data_free(&response);
  }
  return rc;
}
/* Liveness probe budget.  Body: the poll loops below bound WALL time with
   k_deadline_us, so a single probe must not be able to blow that deadline on its
   own.  The old value was 2000 ms while the shortest deadline was 400 ms, i.e. one
   stuck probe overshot the deadline 5x and the deadline could not mean what it
   says.  250 ms keeps every deadline (smallest 400 ms) larger than one full probe.
   Derivation: a connect-per-op loopback GET measured median 3.0 ms / p99 5.5 ms and
   a persistent one median 31 us / p99 152 us (tools/bench_e2e.c on an idle machine),
   so 250 ms is ~45x the worst probe p99 - a probe timing out means the node is
   wedged or unreachable, not that it is slow. */
#define SELFTEST_PROBE_MS 250u
/* "the cluster must react": at least one full probe plus room for an election or
   apply to land (used by the follow-the-new-leader polls). */
#define SELFTEST_REACT_MS 400u
/* NOTE on the remaining deadlines (2000/3000/4000/5000/15000 ms): they are kept at
   their previous values.  They budget a snapshot transfer / catch-up round, which
   this pass did not re-measure, so inventing new numbers would be worse than
   saying so.  What was wrong and is now fixed is the RELATIONSHIP: every one of
   them is at least several probe budgets, and the shortest one (400 ms) is at
   least one full probe. */
/* Monotonic-clock poll deadline: bounds the WALL time a poll loop may run,
   independent of how long each probe blocks (a fixed-iteration loop overshoots
   when a probe's own timeout dominates). */
static k_u64 k_deadline_us(unsigned int timeout_ms){
  k_u64 d=0;
  k_monotonic_us(&d);
  return d+(k_u64)timeout_ms*1000u;
}
static int k_deadline_passed(k_u64 deadline){
  k_u64 now=0;
  return k_monotonic_us(&now)!=0||now>=deadline;
}
static int k_wait_leader(const k_cluster *cluster){
  k_response_data response;
  k_u64 deadline;
  int node,leader_index=-1;
  /* Leader detection via the REDIRECT business mechanism, not raft_inspect: a
     WRITE (DEL of a reserved never-present key) is processed (K_STATUS_OK) only
     by the leader; a follower answers K_STATUS_REDIRECT.  Accepting ONLY the OK
     avoids trusting a stale REDIRECT leader_id during a leader-change window
     (a follower can briefly still report the removed old leader).  The DEL is a
     no-op apply (key never exists) - it only advances the log by one. */
  static const k_u8 probe[8]={0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
  deadline=k_deadline_us(5000u);
  while(leader_index<0&&!k_deadline_passed(deadline)){
    for(node=0;node<cluster->count&&leader_index<0;node++){
      memset(&response,0,sizeof(response));
      if(k_sync_call(cluster->nodes[node].host,cluster->nodes[node].client_port,K_REQ_DEL,probe,8u,0,0,SELFTEST_PROBE_MS,&response)==0&&response.status==K_STATUS_OK) leader_index=node;
      k_response_data_free(&response);
    }
    if(leader_index<0) runtime_msleep(50u);
  }
  return leader_index;
}
/* Remove every on-disk artifact a selftest node left behind (snapshot, WAL
   segments, WAL meta, cfg), by base URI.  Shared by the membership selftests'
   node teardown; one place to fix if the disk-format cleanup ever grows. */
/* Upper bound for the snapshot-index sweep in k_selftest_node_cleanup: these selftests write a few
   hundred entries at most, so any snapshot they can leave behind has an index well below this. */
#define K_SELFTEST_SNAP_SWEEP 4096
/* Unlink every snapshot this base could own, by bounded index sweep, and RETRY while an unlink
   fails.  Two reasons the earlier metadata-driven deletes left kdb-selftest-*.snap.* files in the
   working directory: a snapshot whose index is recorded nowhere in the stored metadata is
   undiscoverable by a read, and a server that was stopped moments ago may still hold the file for
   a little longer - an unlink of an open file fails silently on Windows.  vfs_unlink reports 0 for
   a missing file, so the sweep is idempotent and cheap (a few thousand calls). */
static void k_selftest_snap_sweep(const char *base){
  char path[K_URI_MAX];
  int attempt,idx,failed;
  for(attempt=0;attempt<8;attempt++){
    failed=0;
    for(idx=0;idx<K_SELFTEST_SNAP_SWEEP;idx++){
      if(k_path_snapshot(path,base,(k_i64)idx)!=0) continue;
      if(vfs_unlink(path)!=0) failed++;
    }
    if(!failed) return;
    runtime_msleep(20u);
  }
}
static void k_selftest_node_cleanup(const char *base){
  k_restore restore;
  k_wal_meta_state meta;
  char path[K_URI_MAX];
  int wal_exists;
  int seg;
  if(k_state_load(base,&restore,&meta,&wal_exists)>0){
    if(restore.persist.last_included_index>0&&k_path_snapshot(path,base,restore.persist.last_included_index)==0) vfs_unlink(path);
    k_restore_free(&restore);
  }
  if(k_wal_meta_load(base,&meta)==1){
    seg=meta.record.segment>meta.next.segment?(int)meta.record.segment:(int)meta.next.segment;
    for(;seg>=0;seg--){
      if(k_path_wal_segment(path,base,seg)==0) vfs_unlink(path);
    }
  }
  if(k_path_suffix(path,base,".wal.meta")==0) vfs_unlink(path);
  if(k_path_suffix(path,base,".cfg")==0) vfs_unlink(path);
  k_selftest_snap_sweep(base);
}
/* Start a self-only bootstrap node at base_port+3 (client) / base_port+13
   (peer), with a per-tag on-disk base, so each membership selftest gets an
   isolated node without repeating the runtime/worker bootstrap.  On success
   returns 1 with server4 started and *rt4 holding the runtime; on failure
   tears everything down and returns 0. */
static int k_selftest_boot_node(k_server *server4,k_cluster *cluster4,runtime_ctx **rt4,char *base4,const char *tag,int new_id,unsigned short base_port){
  memset(cluster4,0,sizeof(*cluster4));
  cluster4->count=1;
  cluster4->nodes[0].id=new_id;
  strcpy(cluster4->nodes[0].host,"127.0.0.1");
  cluster4->nodes[0].client_port=(unsigned short)(base_port+3u);
  cluster4->nodes[0].peer_port=(unsigned short)(base_port+13u);
  sprintf(base4,"disk://kdb-selftest-%s-%u-n%d",tag,k_process_id(),new_id);
  k_server_init(server4,new_id,cluster4->nodes[0].client_port,cluster4->nodes[0].peer_port,base4,cluster4);
  server4->bootstrap=1;
  *rt4=runtime_create("thread",1,k_server_worker,0);
  if(!*rt4){ k_server_release(server4); return 0; }
  if(runtime_task_post(*rt4,0,server4)!=0){ runtime_destroy(*rt4); k_server_release(server4); return 0; }
  runtime_wait_workers_ready(*rt4);
  if(server4->started!=1){
    printf("selftest: %s node start failed\n",tag);
    runtime_stop(*rt4);
    runtime_wait_workers_exit(*rt4);
    runtime_destroy(*rt4);
    k_server_release(server4);
    return 0;
  }
  return 1;
}
/* bootstrap: a brand-new node with an empty-peer cluster spec joins the running
   3-node cluster by address (MEMBER ADD id@host:cp:pp), Sec 4.4 */
static int k_selftest_bootstrap(const k_cluster *cluster,int leader_index,unsigned short base_port,int new_id){
  k_cluster cluster4;
  k_server server4;
  runtime_ctx *rt4;
  k_response_data response;
  char base4[K_URI_MAX];
  int ids4[1];
  char hosts4[1][K_HOST_MAX];
  unsigned short cp4[1],pp4[1];
  int got4,rc=-1;
  k_i64 snap_idx=0,task_idx=0;
  char path[K_URI_MAX];
  k_u64 deadline;
  /* no pre-listed bootstrap source: the new node starts with only itself and
     must learn the cluster from whichever live member contacts it (Sec 4.4
     dynamic membership - a static single-source config is the anti-pattern) */
  if(!k_selftest_boot_node(&server4,&cluster4,&rt4,base4,"boot",new_id,base_port)) return -1;
  ids4[0]=new_id;
  strcpy(hosts4[0],"127.0.0.1");
  cp4[0]=cluster4.nodes[0].client_port;
  pp4[0]=cluster4.nodes[0].peer_port;
  memset(&response,0,sizeof(response));
  if(k_follow_member(cluster,leader_index,K_MEMBER_ADD,ids4,1,hosts4,cp4,pp4,&response)!=0||response.status!=K_STATUS_OK){
    printf("selftest: bootstrap ADD failed\n");
    k_response_data_free(&response); goto done; }
  k_response_data_free(&response);
  /* node 4 is now a voter: it must serve the durable key it caught up */
  got4=0;
  deadline=k_deadline_us(SELFTEST_REACT_MS);
  for(;;){
    memset(&response,0,sizeof(response));
    if(k_sync_call(cluster4.nodes[0].host,cluster4.nodes[0].client_port,K_REQ_GET,"durable",7u,0,0,SELFTEST_PROBE_MS,&response)==0&&response.status==K_STATUS_OK&&response.body_size==5u&&memcmp(response.body,"value",K_LEN("value"))==0) got4=1;
    k_response_data_free(&response);
    if(got4) break;
    if(k_deadline_passed(deadline)) break;
    runtime_msleep(50u);
  }
  if(!got4){ printf("selftest: bootstrap poll timeout\n"); goto done; }
  rc=0;
  memset(&response,0,sizeof(response));
  k_sync_call(cluster4.nodes[0].host,cluster4.nodes[0].client_port,K_REQ_SHUTDOWN,0,0,0,0,2000u,&response);
  k_response_data_free(&response);
done:
  runtime_stop(rt4);
  runtime_wait_workers_exit(rt4);
  runtime_destroy(rt4);
  /* Capture the snapshot indexes BEFORE the release clears them, unlink AFTER it has closed the
     node's file handles (an unlink of a still-open file fails on Windows), then run the by-base
     sweep for the WAL/meta/cfg artifacts. */
  snap_idx=server4.snapshot.index;
  task_idx=server4.last_snapshot_task_index;
  k_server_release(&server4);
  k_selftest_node_cleanup(base4);
  if(snap_idx>0&&k_path_snapshot(path,base4,snap_idx)==0) vfs_unlink(path);
  if(task_idx>0&&k_path_snapshot(path,base4,task_idx)==0) vfs_unlink(path);
  return rc;
}
/* strtou64 unit test: the hand-written strtoull shim must match the standard
   semantics (whitespace, sign, base, overflow saturation, endptr) since the
   FCALL transfer command's amount parsing depends on it. */
static int k_selftest_strtou64(void){
  static const struct{ const char *s; int base; k_u64 expect; } cases[]={
    {"123",10,123u},{"  123",10,123u},{"+123",10,123u},
    {"-1",10,~((k_u64)0)},{"0x1F",0,31u},{"010",0,8u},{"1F",16,31u},
    {"18446744073709551616",10,~((k_u64)0)},{"abc",10,0u}
  };
  char buf[8];
  char *end;
  int i;
  k_u64 got;
  for(i=0;i<(int)(sizeof(cases)/sizeof(cases[0]));i++){
    got=k_strtou64(cases[i].s,&end,cases[i].base);
    if(got!=cases[i].expect){ printf("selftest: strtou64 value mismatch (%s)\n",cases[i].s); return -1; }
  }
  memcpy(buf,"123abc",7); buf[7]=0;
  got=k_strtou64(buf,&end,10);
  if(got!=123u||strcmp(end,"abc")!=0){ printf("selftest: strtou64 endptr mismatch\n"); return -1; }
  memcpy(buf,"xyz",4);
  end=0;
  if(k_strtou64(buf,&end,10)!=0u||end!=buf){ printf("selftest: strtou64 nodigits mismatch\n"); return -1; }
  return 0;
}
/* Silent-standby predicate unit test: the bootstrap-vs-removed distinction.
   A node being ADDED (bootstrap) replays the leader's pre-ADD log, so its
   raft config_new temporarily excludes self with config_joint==0 - it must NOT
   be read as removed (dropping the config_joint gate would misread it). */
static int k_selftest_silent_standby(void){
  k_server server;
  memset(&server,0,sizeof(server));
  server.id=1;
  server.voter_count=1;
  server.voters[0]=1;            /* applied membership still contains self */
  /* The membership facts below arrive through raft_ready (issue #14, C6).  The predicate is unchanged;
     what changed is where its inputs come from - this test now drives exactly the inputs the server
     caches from the ready bundle, instead of reaching into raft_ctx's config fields the way it used to. */
  /* bootstrap being ADDED: not joint, and the new configuration excludes self */
  server.cfg_joint=0; server.self_is_voter=0; server.self_is_learner=0;
  if(k_server_silent_standby(&server)!=0){ printf("selftest: silent-standby bootstrap misread\n"); return -1; }
  /* removed node mid-joint: joint, and the new configuration excludes self */
  server.cfg_joint=1;
  if(k_server_silent_standby(&server)!=1){ printf("selftest: silent-standby removed missed\n"); return -1; }
  /* learner: excluded from the voters but present in the learners list */
  server.self_is_learner=1;
  if(k_server_silent_standby(&server)!=0){ printf("selftest: silent-standby learner misread\n"); return -1; }
  /* normal member: present in the voters */
  server.self_is_learner=0; server.self_is_voter=1;
  if(k_server_silent_standby(&server)!=0){ printf("selftest: silent-standby member misread\n"); return -1; }
  return 0;
}
/* Sec 4.4 "any server can be added": a brand-new node with an id LOWER than every
   existing member must still join. The static "lower dials higher" dial-up rule
   would orphan it (nobody dials a lower id); the leader dials it as a pending
   catch-up target instead, and the node accepts the down-dial. */
static int k_selftest_bootstrap_low(void){
  k_cluster cluster;
  k_server servers[3];
  runtime_ctx *runtime;
  k_response_data response;
  unsigned short base_port=(unsigned short)(15000u+(k_process_id()%5000u));
  int leader_index,rc=-1;
  k_embedded_cluster(&cluster,base_port,3);   /* ids {3,4,5} */
  k_embedded_bases(servers,&cluster,"selftest-low",3);
  k_embedded_clean(servers);
  runtime=k_embedded_start(servers);
  if(!runtime){ printf("selftest: low bootstrap cluster start failed\n"); k_embedded_clean(servers); return -1; }
  leader_index=k_wait_leader(&cluster);
  if(leader_index<0){ printf("selftest: low bootstrap election failed\n"); goto stop; }
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_SET,"durable",7u,"value",5u,&response)!=0||response.status!=K_STATUS_OK){ printf("selftest: low bootstrap PUT failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  if(k_selftest_bootstrap(&cluster,leader_index,base_port,1)!=0){ printf("selftest: low-id bootstrap failed\n");goto stop; }
  rc=0;
stop:
  if(runtime){
    if(k_embedded_shutdown(&cluster)!=0){
      printf("selftest: low bootstrap shutdown failed\n");
      rc=-1;
      runtime_stop(runtime);
    }
    runtime_wait_workers_exit(runtime);
    runtime_destroy(runtime);
  }
  k_embedded_clean(servers);
  return rc;
}
static int k_selftest_member_failover(void){
  k_cluster cluster;
  k_cluster cluster4;
  k_server servers[3];
  k_server server4;
  runtime_ctx *runtime;
  runtime_ctx *rt4;
  k_response_data response;
  unsigned short base_port=(unsigned short)(21000u+(k_process_id()%4000u));
  int leader_index,rc=-1;
  k_u64 deadline;
  int ids4[1];
  char hosts4[1][K_HOST_MAX];
  unsigned short cp4[1],pp4[1];
  char base4[K_URI_MAX];
  k_embedded_cluster(&cluster,base_port,1);
  k_embedded_bases(servers,&cluster,"selftest-failover",1);
  k_embedded_clean(servers);
  runtime=k_embedded_start(servers);
  if(!runtime){ printf("selftest: failover cluster start failed\n"); k_embedded_clean(servers); return -1; }
  leader_index=k_wait_leader(&cluster);
  if(leader_index<0){ printf("selftest: failover election failed\n"); goto stop; }
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_SET,"k",1u,"v",1u,&response)!=0||response.status!=K_STATUS_OK){ printf("selftest: failover SET failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  if(!k_selftest_boot_node(&server4,&cluster4,&rt4,base4,"failover",4,base_port)) goto stop;
  ids4[0]=4;
  strcpy(hosts4[0],"127.0.0.1");
  cp4[0]=cluster4.nodes[0].client_port;
  pp4[0]=cluster4.nodes[0].peer_port;
  memset(&response,0,sizeof(response));
  if(k_follow_member(&cluster,leader_index,K_MEMBER_ADD,ids4,1,hosts4,cp4,pp4,&response)!=0||response.status!=K_STATUS_OK){ printf("selftest: failover ADD failed (status=%u)\n",(unsigned)response.status);k_response_data_free(&response);goto stop4; }
  k_response_data_free(&response);
  memset(&response,0,sizeof(response));
  k_sync_call(cluster.nodes[leader_index].host,cluster.nodes[leader_index].client_port,K_REQ_SHUTDOWN,0,0,0,0,2000u,&response);
  k_response_data_free(&response);
  /* verify failover: the dynamically-added node must serve the durable key once
     a surviving node wins the election (it reaches the new leader via the
     replicated address book). */
  rc=-1;
  deadline=k_deadline_us(4000u);
  for(;;){
    memset(&response,0,sizeof(response));
    if(k_sync_call(cluster4.nodes[0].host,cluster4.nodes[0].client_port,K_REQ_GET,"k",1u,0,0,SELFTEST_PROBE_MS,&response)==0&&response.status==K_STATUS_OK&&response.body_size==1u&&memcmp(response.body,"v",K_LEN("v"))==0){ rc=0; k_response_data_free(&response); break; }
    k_response_data_free(&response);
    if(k_deadline_passed(deadline)) break;
    runtime_msleep(50u);
  }
  if(rc!=0) printf("selftest: failover read failed\n");
stop4:
  memset(&response,0,sizeof(response));
  k_sync_call(cluster4.nodes[0].host,cluster4.nodes[0].client_port,K_REQ_SHUTDOWN,0,0,0,0,2000u,&response);
  k_response_data_free(&response);
  runtime_stop(rt4);
  runtime_wait_workers_exit(rt4);
  runtime_destroy(rt4);
  k_server_release(&server4);
  /* remove node 4's files */
  k_selftest_node_cleanup(base4);
stop:
  if(runtime){
    k_embedded_shutdown(&cluster);
    runtime_stop(runtime);
    runtime_wait_workers_exit(runtime);
    runtime_destroy(runtime);
  }
  k_embedded_clean(servers);
  return rc;
}
/* 1 when the TOPOLOGY body lists node [id] with role=voter.  Entries are comma
   separated "id@host:client:peer role=<role>". */
static int k_selftest_topology_has_voter(const void *body,k_u32 size,int id){
  char buf[512];
  char pat[16];
  char *entry,*next;
  int n;
  if(!body||size==0||size>=(k_u32)sizeof(buf)) return 0;
  memcpy(buf,body,(size_t)size);
  buf[size]=0;
  n=sprintf(pat,"%d@",id);
  entry=buf;
  while(entry&&*entry){
    next=strchr(entry,',');
    if(next) *next=0;
    if(strncmp(entry,pat,(size_t)n)==0&&strstr(entry,"role=voter")) return 1;
    entry=next?next+1:0;
  }
  return 0;
}
/* Sec 4.4 auto-replacement: the leader detects a failed FOLLOWER via per-peer
   liveness (missed_rounds) and replaces it add-before-remove with a bootstrap
   node 4.  Verifies the replacement joins the voter set (serves the durable key)
   AND that the failed voter is removed (the REMOVE half of add-before-remove). */
static int k_selftest_auto_replace(void){
  k_cluster cluster;
  k_cluster cluster4;
  k_server servers[3];
  k_server server4;
  runtime_ctx *runtime;
  runtime_ctx *rt4;
  k_response_data response;
  unsigned short base_port=(unsigned short)(26000u+(k_process_id()%3000u));
  int leader_index,victim,rc=-1,i,got=0,removed=0;
  k_u64 deadline;
  char base4[K_URI_MAX];
  k_embedded_cluster(&cluster,base_port,1);   /* ids {1,2,3} */
  k_embedded_bases(servers,&cluster,"selftest-autorep",1);
  for(i=0;i<3;i++){
    servers[i].auto_replace=1;
    servers[i].auto_replace_new_id=4;
    strcpy(servers[i].auto_replace_new_host,"127.0.0.1");
    servers[i].auto_replace_new_client_port=(unsigned short)(base_port+3u);
    servers[i].auto_replace_new_peer_port=(unsigned short)(base_port+13u);
    servers[i].auto_replace_threshold=3u;
  }
  k_embedded_clean(servers);
  runtime=k_embedded_start(servers);
  if(!runtime){ printf("selftest: auto-replace cluster start failed\n"); k_embedded_clean(servers); return -1; }
  leader_index=k_wait_leader(&cluster);
  if(leader_index<0){ printf("selftest: auto-replace election failed\n"); goto stop; }
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_SET,"durable",7u,"value",5u,&response)!=0||response.status!=K_STATUS_OK){ printf("selftest: auto-replace SET failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  /* start the replacement node 4 (bootstrap, empty config) */
  if(!k_selftest_boot_node(&server4,&cluster4,&rt4,base4,"auto-replace",4,base_port)) goto stop;
  /* kill a FOLLOWER (not the leader): the leader detects it and auto-replaces */
  victim=(leader_index+1)%3;
  memset(&response,0,sizeof(response));
  k_sync_call(cluster.nodes[victim].host,cluster.nodes[victim].client_port,K_REQ_SHUTDOWN,0,0,0,0,2000u,&response);
  k_response_data_free(&response);
  /* node 4 should join via add-before-remove and serve the durable key */
  deadline=k_deadline_us(15000u);
  for(;;){
    memset(&response,0,sizeof(response));
    if(k_sync_call(cluster4.nodes[0].host,cluster4.nodes[0].client_port,K_REQ_GET,"durable",7u,0,0,SELFTEST_PROBE_MS,&response)==0&&response.status==K_STATUS_OK&&response.body_size==5u&&memcmp(response.body,"value",K_LEN("value"))==0) got=1;
    k_response_data_free(&response);
    if(got) break;
    if(k_deadline_passed(deadline)) break;
    runtime_msleep(50u);
  }
  if(!got) printf("selftest: auto-replace failed (node 4 did not join)\n");
  /* The join check above only covers the ADD half.  add-BEFORE-remove requires the
     leader to remove the failed voter next: without it the cluster keeps a dead
     voter forever, and (because the phase only clears when the REMOVE commits)
     the replacer stays parked and never reacts to another failure. */
  deadline=k_deadline_us(15000u);
  for(;;){
    memset(&response,0,sizeof(response));
    if(k_follow_call(&cluster,leader_index,K_REQ_TOPOLOGY,0,0,0,0,&response)==0&&response.status==K_STATUS_OK
       &&!k_selftest_topology_has_voter(response.body,response.body_size,cluster.nodes[victim].id)) removed=1;
    k_response_data_free(&response);
    if(removed||k_deadline_passed(deadline)) break;
    runtime_msleep(50u);
  }
  if(!removed) printf("selftest: auto-replace never removed the failed voter (REMOVE phase did not run)\n");
  rc=(got&&removed)?0:-1;
  /* cleanup node 4 */
  memset(&response,0,sizeof(response));
  k_sync_call(cluster4.nodes[0].host,cluster4.nodes[0].client_port,K_REQ_SHUTDOWN,0,0,0,0,2000u,&response);
  k_response_data_free(&response);
  runtime_stop(rt4);
  runtime_wait_workers_exit(rt4);
  runtime_destroy(rt4);
  k_server_release(&server4);
  k_selftest_node_cleanup(base4);
stop:
  if(runtime){
    k_embedded_shutdown(&cluster);
    runtime_stop(runtime);
    runtime_wait_workers_exit(runtime);
    runtime_destroy(runtime);
  }
  k_embedded_clean(servers);
  return rc;
}
/* Sec 4.4 removed-node silent standby: after MEMBER REMOVE a still-running node
   stops dialing (goes idle) instead of churning its peer connections; a later
   MEMBER ADD re-adopts it without a restart.  Verifies both directions. */
static int k_selftest_remove_readd(void){
  k_cluster cluster;
  k_cluster cluster4;
  k_server servers[3];
  k_server server4;
  runtime_ctx *runtime;
  runtime_ctx *rt4;
  k_response_data response;
  unsigned short base_port=(unsigned short)(30000u+(k_process_id()%3000u));
  int leader_index,victim,rc=-1,j,dialing,ok;
  k_conn *pc;
  k_u64 deadline;
  int ids3[1],ids4[1];
  char hosts3[1][K_HOST_MAX],hosts4[1][K_HOST_MAX];
  unsigned short cp3[1],pp3[1],cp4[1],pp4[1];
  char base4[K_URI_MAX];
  k_embedded_cluster(&cluster,base_port,1);   /* ids {1,2,3} */
  k_embedded_bases(servers,&cluster,"selftest-removereadd",1);
  k_embedded_clean(servers);
  runtime=k_embedded_start(servers);
  if(!runtime){ printf("selftest: remove-readd cluster start failed\n"); k_embedded_clean(servers); return -1; }
  leader_index=k_wait_leader(&cluster);
  if(leader_index<0){ printf("selftest: remove-readd election failed\n"); goto stop; }
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_SET,"durable",7u,"value",5u,&response)!=0||response.status!=K_STATUS_OK){ printf("selftest: remove-readd SET failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  /* start node 4 (bootstrap, self-only) */
  if(!k_selftest_boot_node(&server4,&cluster4,&rt4,base4,"remove-readd",4,base_port)) goto stop;
  ids4[0]=4;
  strcpy(hosts4[0],"127.0.0.1");
  cp4[0]=cluster4.nodes[0].client_port;
  pp4[0]=cluster4.nodes[0].peer_port;
  /* ADD node 4 (first join) */
  memset(&response,0,sizeof(response));
  if(k_follow_member(&cluster,leader_index,K_MEMBER_ADD,ids4,1,hosts4,cp4,pp4,&response)!=0||response.status!=K_STATUS_OK){ printf("selftest: remove-readd ADD failed\n");k_response_data_free(&response);goto stop4; }
  k_response_data_free(&response);
  ok=0;
  deadline=k_deadline_us(4000u);
  for(;;){
    memset(&response,0,sizeof(response));
    if(k_sync_call(cluster4.nodes[0].host,cluster4.nodes[0].client_port,K_REQ_GET,"durable",7u,0,0,SELFTEST_PROBE_MS,&response)==0&&response.status==K_STATUS_OK&&response.body_size==5u&&memcmp(response.body,"value",K_LEN("value"))==0){ ok=1; k_response_data_free(&response); break; }
    k_response_data_free(&response);
    if(k_deadline_passed(deadline)) break;
    runtime_msleep(50u);
  }
  if(!ok){ printf("selftest: remove-readd node4 join failed\n"); goto stop4; }
  /* REMOVE a non-leader node (victim): it would keep dialing a higher-id
     member; the silent-standby fix makes it stop initiating dials (idle) while
     keeping its listener + raft state.  Removing a follower also avoids the
     leader-change that would make the RE-ADD below hit a stale leader. */
  victim=(leader_index+1)%3;
  ids3[0]=cluster.nodes[victim].id;
  memset(&response,0,sizeof(response));
  if(k_follow_member(&cluster,leader_index,K_MEMBER_REMOVE,ids3,1,0,0,0,&response)!=0||response.status!=K_STATUS_OK){ printf("selftest: remove-readd REMOVE failed\n");k_response_data_free(&response);goto stop4; }
  k_response_data_free(&response);
  /* verify the removed node stopped dialing: poll until it holds no outbound
     peer connections */
  rc=-1;
  deadline=k_deadline_us(3000u);
  for(;;){
    dialing=0;
    for(j=0;j<servers[victim].cluster.count;j++){
      pc=servers[victim].peer_conns[j];
      if(pc&&pc->outbound){ dialing=1; break; }
    }
    if(!dialing){ rc=0; break; }
    if(k_deadline_passed(deadline)) break;
    runtime_msleep(50u);
  }
  if(rc!=0){ printf("selftest: remove-readd node%d still dialing\n",ids3[0]); goto stop4; }
  rc=-1;
  /* RE-ADD the removed node (no restart): it must rejoin via the bootstrap path */
  strcpy(hosts3[0],cluster.nodes[victim].host);
  cp3[0]=cluster.nodes[victim].client_port;
  pp3[0]=cluster.nodes[victim].peer_port;
  memset(&response,0,sizeof(response));
  if(k_follow_member(&cluster,leader_index,K_MEMBER_ADD,ids3,1,hosts3,cp3,pp3,&response)!=0||response.status!=K_STATUS_OK){ printf("selftest: remove-readd RE-ADD failed (status=%u)\n",(unsigned)response.status);k_response_data_free(&response);goto stop4; }
  k_response_data_free(&response);
  rc=-1;
  deadline=k_deadline_us(4000u);
  for(;;){
    memset(&response,0,sizeof(response));
    if(k_sync_call(cluster.nodes[victim].host,cluster.nodes[victim].client_port,K_REQ_GET,"durable",7u,0,0,SELFTEST_PROBE_MS,&response)==0&&response.status==K_STATUS_OK&&response.body_size==5u&&memcmp(response.body,"value",K_LEN("value"))==0){ rc=0; k_response_data_free(&response); break; }
    k_response_data_free(&response);
    if(k_deadline_passed(deadline)) break;
    runtime_msleep(50u);
  }
  if(rc!=0) printf("selftest: remove-readd node%d re-add failed\n",ids3[0]);
stop4:
  memset(&response,0,sizeof(response));
  k_sync_call(cluster4.nodes[0].host,cluster4.nodes[0].client_port,K_REQ_SHUTDOWN,0,0,0,0,2000u,&response);
  k_response_data_free(&response);
  runtime_stop(rt4);
  runtime_wait_workers_exit(rt4);
  runtime_destroy(rt4);
  k_server_release(&server4);
  k_selftest_node_cleanup(base4);
stop:
  if(runtime){
    k_embedded_shutdown(&cluster);
    runtime_stop(runtime);
    runtime_wait_workers_exit(runtime);
    runtime_destroy(runtime);
  }
  k_embedded_clean(servers);
  return rc;
}
/* Sec 4.4 removed-leader re-add: removing the LEADER self-removes it (step-down
   to silent standby), the survivors elect a new leader, and MEMBER ADD via that
   new leader re-adopts the removed leader without a restart. */
static int k_selftest_remove_leader_readd(void){
  k_cluster cluster;
  k_cluster cluster4;
  k_cluster full;
  k_server servers[3];
  k_server server4;
  runtime_ctx *runtime;
  runtime_ctx *rt4;
  k_response_data response;
  unsigned short base_port=(unsigned short)(32000u+(k_process_id()%3000u));
  int leader_index,new_leader,rc=-1;
  k_u64 deadline;
  int ids1[1],ids4[1];
  char hosts1[1][K_HOST_MAX],hosts4[1][K_HOST_MAX];
  unsigned short cp1[1],pp1[1],cp4[1],pp4[1];
  char base4[K_URI_MAX];
  k_embedded_cluster(&cluster,base_port,1);   /* ids {1,2,3} */
  k_embedded_bases(servers,&cluster,"selftest-rmleader",1);
  k_embedded_clean(servers);
  runtime=k_embedded_start(servers);
  if(!runtime){ printf("selftest: remove-leader cluster start failed\n"); k_embedded_clean(servers); return -1; }
  leader_index=k_wait_leader(&cluster);
  if(leader_index<0){ printf("selftest: rmleader election failed\n"); goto stop; }
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_SET,"durable",7u,"value",5u,&response)!=0||response.status!=K_STATUS_OK){ printf("selftest: rmleader SET failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  /* bootstrap node 4 (self-only) */
  if(!k_selftest_boot_node(&server4,&cluster4,&rt4,base4,"remove-leader",4,base_port)) goto stop;
  ids4[0]=4;
  strcpy(hosts4[0],"127.0.0.1");
  cp4[0]=cluster4.nodes[0].client_port;
  pp4[0]=cluster4.nodes[0].peer_port;
  memset(&response,0,sizeof(response));
  if(k_follow_member(&cluster,leader_index,K_MEMBER_ADD,ids4,1,hosts4,cp4,pp4,&response)!=0||response.status!=K_STATUS_OK){ printf("selftest: rmleader ADD failed\n");k_response_data_free(&response);goto stop4; }
  k_response_data_free(&response);
  /* REMOVE the leader: it self-removes (joint -> C_new without itself), steps
     down to silent standby, and the survivors must elect a new leader */
  ids1[0]=cluster.nodes[leader_index].id;
  memset(&response,0,sizeof(response));
  if(k_follow_member(&cluster,leader_index,K_MEMBER_REMOVE,ids1,1,0,0,0,&response)!=0||response.status!=K_STATUS_OK){ printf("selftest: rmleader REMOVE failed\n");k_response_data_free(&response);goto stop4; }
  k_response_data_free(&response);
  /* the new leader may be the bootstrap node 4 (not in the static {1,2,3}
     cluster): probe the full 4-node set */
  full=cluster;
  full.nodes[3]=cluster4.nodes[0];
  full.count=4;
  new_leader=k_wait_leader(&full);
  if(new_leader<0){ printf("selftest: rmleader no new leader\n"); goto stop4; }
  /* RE-ADD the removed leader via the NEW leader */
  strcpy(hosts1[0],cluster.nodes[leader_index].host);
  cp1[0]=cluster.nodes[leader_index].client_port;
  pp1[0]=cluster.nodes[leader_index].peer_port;
  memset(&response,0,sizeof(response));
  if(k_follow_member(&full,new_leader,K_MEMBER_ADD,ids1,1,hosts1,cp1,pp1,&response)!=0||response.status!=K_STATUS_OK){ printf("selftest: rmleader RE-ADD failed (status=%u)\n",(unsigned)response.status);k_response_data_free(&response);goto stop4; }
  k_response_data_free(&response);
  /* verify the removed leader re-joins and serves the durable key */
  rc=-1;
  deadline=k_deadline_us(4000u);
  for(;;){
    memset(&response,0,sizeof(response));
    if(k_sync_call(cluster.nodes[leader_index].host,cluster.nodes[leader_index].client_port,K_REQ_GET,"durable",7u,0,0,SELFTEST_PROBE_MS,&response)==0&&response.status==K_STATUS_OK&&response.body_size==5u&&memcmp(response.body,"value",K_LEN("value"))==0){ rc=0; k_response_data_free(&response); break; }
    k_response_data_free(&response);
    if(k_deadline_passed(deadline)) break;
    runtime_msleep(50u);
  }
  if(rc!=0) printf("selftest: rmleader re-add failed\n");
stop4:
  memset(&response,0,sizeof(response));
  k_sync_call(cluster4.nodes[0].host,cluster4.nodes[0].client_port,K_REQ_SHUTDOWN,0,0,0,0,2000u,&response);
  k_response_data_free(&response);
  runtime_stop(rt4);
  runtime_wait_workers_exit(rt4);
  runtime_destroy(rt4);
  k_server_release(&server4);
  k_selftest_node_cleanup(base4);
stop:
  if(runtime){
    k_embedded_shutdown(&cluster);
    runtime_stop(runtime);
    runtime_wait_workers_exit(runtime);
    runtime_destroy(runtime);
  }
  k_embedded_clean(servers);
  return rc;
}


/* ================= Test: selftest ================= */
typedef struct{
  unsigned char *data;
  unsigned int len;
  unsigned int off;
} k_membuf;
static int k_membuf_write(void *ud,const unsigned char *d,unsigned int size){
  k_membuf *m=(k_membuf *)ud;
  if(!m||!d||m->len+size>4096u) return -1;
  memcpy(m->data+m->len,d,size);
  m->len+=size;
  return 0;
}
static int k_membuf_read(void *ud,unsigned char *d,unsigned int size){
  k_membuf *m=(k_membuf *)ud;
  if(!m||!d||m->off+size>m->len) return -1;
  memcpy(d,m->data+m->off,size);
  m->off+=size;
  return 0;
}
/* Verify the order-statistic treap primitives (rank/count_range/select/
   min_range/max_range) against a known key set, including size maintenance
   through set/delete/delete_range and save/load. */
static int k_selftest_treap_stats(void){
  treap *t,*t2;
  const unsigned char *key,*value;
  unsigned int key_len,value_len;
  unsigned char keybuf[2],bb[1],ee[1];
  unsigned char store[4096];
  k_membuf m;
  treap_u64 i;
  t=treap_create(12345u);
  if(!t) return -1;
  for(i=0;i<26u;i++){
    keybuf[0]=(unsigned char)('a'+(int)i);
    keybuf[1]=(unsigned char)('0'+(int)(i%10u));
    if(treap_set(t,keybuf,1u,keybuf+1,1u)!=0) goto fail;
  }
  if(treap_count(t)!=26u) goto fail;
  keybuf[0]='a'; if(treap_rank(t,keybuf,1u)!=0u) goto fail;
  keybuf[0]='m'; if(treap_rank(t,keybuf,1u)!=12u) goto fail;
  keybuf[0]='z'; if(treap_rank(t,keybuf,1u)!=25u) goto fail;
  bb[0]='a'; ee[0]='m'; if(treap_count_range(t,bb,1u,ee,1u)!=12u) goto fail;
  ee[0]='f'; if(treap_count_range(t,0,0,ee,1u)!=5u) goto fail;
  bb[0]='g'; if(treap_count_range(t,bb,1u,0,0)!=20u) goto fail;
  for(i=0;i<26u;i++){
    if(treap_select(t,i,&key,&key_len,&value,&value_len)!=1) goto fail;
    if(key_len!=1u||key[0]!=(unsigned char)('a'+(int)i)) goto fail;
  }
  if(treap_select(t,26u,&key,&key_len,&value,&value_len)!=0) goto fail;
  bb[0]='m'; ee[0]='z';
  if(treap_min_range(t,bb,1u,ee,1u,&key,&key_len,&value,&value_len)!=1||key_len!=1u||key[0]!='m') goto fail;
  if(treap_max_range(t,bb,1u,ee,1u,&key,&key_len,&value,&value_len)!=1||key_len!=1u||key[0]!='y') goto fail;
  /* size maintenance through mutation */
  keybuf[0]='m';
  if(treap_delete(t,keybuf,1u)!=1) goto fail;
  if(treap_count(t)!=25u) goto fail;
  keybuf[0]='n'; if(treap_rank(t,keybuf,1u)!=12u) goto fail;
  bb[0]='a'; ee[0]='f';
  if(treap_delete_range(t,bb,1u,ee,1u)!=5u) goto fail;
  if(treap_count(t)!=20u) goto fail;
  bb[0]='g'; ee[0]='n';
  if(treap_count_range(t,bb,1u,ee,1u)!=6u) goto fail;
  /* save/load round-trip preserves sizes (f,g..l,n..z) */
  treap_capture(t);
  m.data=store; m.len=0; m.off=0;
  if(treap_save(t,k_membuf_write,&m)!=0) goto fail;
  t2=treap_create(12345u);
  if(!t2) goto fail;
  m.off=0;
  if(treap_load(t2,k_membuf_read,&m)!=0){ treap_free(t2); goto fail; }
  if(treap_count(t2)!=20u){ treap_free(t2); goto fail; }
  keybuf[0]='g'; if(treap_rank(t2,keybuf,1u)!=1u){ treap_free(t2); goto fail; }
  keybuf[0]='m'; if(treap_rank(t2,keybuf,1u)!=7u){ treap_free(t2); goto fail; }
  if(treap_select(t2,0u,&key,&key_len,&value,&value_len)!=1||key[0]!='f'){ treap_free(t2); goto fail; }
  treap_free(t2);
  treap_free(t);
  return 0;
fail:
  treap_free(t);
  return -1;
}
static int k_run_selftest(void){
  k_cluster cluster;
  k_server servers[3];
  runtime_ctx *runtime=0;
  k_response_data response;
  char bases[3][K_URI_MAX];
  char key[32],value[32];
  char expect[256];
  k_client_app ca;
  k_buf args;
  const char *marker;
  k_i64 snapshot_index;
  unsigned short base_port=(unsigned short)(35000u+(k_process_id()%10000u));
  int leader_index,i,snapshot_seen,rc=-1;
  k_u64 deadline;
  k_u8 *scan_cursor;
  k_u32 scan_cursor_len;
  int scan_total,scan_has_more,scan_rounds;
  int ids3[1];
  k_embedded_cluster(&cluster,base_port,1);
  k_embedded_bases(servers,&cluster,"selftest",1);
  for(i=0;i<3;i++) memcpy(bases[i],servers[i].base,strlen(servers[i].base)+1u);
  k_embedded_clean(servers);
  runtime=k_embedded_start(servers);
  if(!runtime){
    printf("selftest: cluster startup failed\n");
    return -1;
  }
  leader_index=k_wait_leader(&cluster);
  if(leader_index<0){
    printf("selftest: leader election failed\n");
    goto stop;
  }
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_SET,"alpha",5u,"one",3u,&response)!=0||response.status!=K_STATUS_OK){ printf("selftest: PUT failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_GET,"alpha",5u,0,0,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=3u||memcmp(response.body,"one",K_LEN("one"))!=0){ printf("selftest: GET failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_COUNT,0,0,0,0,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=1u||memcmp(response.body,"1",K_LEN("1"))!=0){ printf("selftest: COUNT failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_MIN,0,0,0,0,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=K_LEN("alpha=one")||memcmp(response.body,"alpha=one",K_LEN("alpha=one"))!=0){ printf("selftest: MIN failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_MAX,0,0,0,0,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=K_LEN("alpha=one")||memcmp(response.body,"alpha=one",K_LEN("alpha=one"))!=0){ printf("selftest: MAX failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  /* MSET: batch set beta=two, gamma=three -> affected=2 */
  memset(&args,0,sizeof(args));
  k_buf_u32(&args,2u);
  k_buf_u32(&args,4u); k_buf_bytes(&args,(const k_u8*)"beta",4u);
  k_buf_u32(&args,3u); k_buf_bytes(&args,(const k_u8*)"two",3u);
  k_buf_u32(&args,5u); k_buf_bytes(&args,(const k_u8*)"gamma",5u);
  k_buf_u32(&args,5u); k_buf_bytes(&args,(const k_u8*)"three",5u);
  memset(&response,0,sizeof(response));
  if(args.err||k_follow_call(&cluster,leader_index,K_REQ_MSET,args.data,args.len,0,0,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=1u||memcmp(response.body,"2",K_LEN("2"))!=0){ printf("selftest: MSET failed\n");k_response_data_free(&response);k_buf_free(&args);goto stop; }
  k_response_data_free(&response);
  k_buf_free(&args);
  /* MGET: batch read alpha,beta,gamma,delta -> delta missing (skipped) */
  memset(&args,0,sizeof(args));
  k_buf_u32(&args,4u);
  k_buf_u32(&args,5u); k_buf_bytes(&args,(const k_u8*)"alpha",5u);
  k_buf_u32(&args,4u); k_buf_bytes(&args,(const k_u8*)"beta",4u);
  k_buf_u32(&args,5u); k_buf_bytes(&args,(const k_u8*)"gamma",5u);
  k_buf_u32(&args,5u); k_buf_bytes(&args,(const k_u8*)"delta",5u);
  memset(&response,0,sizeof(response));
  if(args.err||k_follow_call(&cluster,leader_index,K_REQ_MGET,args.data,args.len,0,0,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=K_LEN("alpha=one\nbeta=two\ngamma=three\n")||memcmp(response.body,"alpha=one\nbeta=two\ngamma=three\n",K_LEN("alpha=one\nbeta=two\ngamma=three\n"))!=0){ printf("selftest: MGET failed\n");k_response_data_free(&response);k_buf_free(&args);goto stop; }
  k_response_data_free(&response);
  k_buf_free(&args);
  /* RSET: overwrite [beta,zzzz) -> "X" (beta,gamma overwritten; alpha untouched) */
  memset(&args,0,sizeof(args));
  k_buf_u32(&args,4u); k_buf_bytes(&args,(const k_u8*)"beta",4u);
  k_buf_u32(&args,4u); k_buf_bytes(&args,(const k_u8*)"zzzz",4u);
  memset(&response,0,sizeof(response));
  if(args.err||k_follow_call(&cluster,leader_index,K_REQ_RSET,args.data,args.len,"X",1u,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=1u||memcmp(response.body,"2",K_LEN("2"))!=0){ printf("selftest: RSET failed\n");k_response_data_free(&response);k_buf_free(&args);goto stop; }
  k_response_data_free(&response);
  k_buf_free(&args);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_GET,"beta",4u,0,0,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=1u||memcmp(response.body,"X",K_LEN("X"))!=0){ printf("selftest: RSET overwrite failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_GET,"alpha",5u,0,0,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=3u||memcmp(response.body,"one",K_LEN("one"))!=0){ printf("selftest: RSET range bound failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  /* MDEL: batch delete beta,gamma -> affected=2; COUNT back to 1 */
  memset(&args,0,sizeof(args));
  k_buf_u32(&args,2u);
  k_buf_u32(&args,4u); k_buf_bytes(&args,(const k_u8*)"beta",4u);
  k_buf_u32(&args,5u); k_buf_bytes(&args,(const k_u8*)"gamma",5u);
  memset(&response,0,sizeof(response));
  if(args.err||k_follow_call(&cluster,leader_index,K_REQ_MDEL,args.data,args.len,0,0,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=1u||memcmp(response.body,"2",K_LEN("2"))!=0){ printf("selftest: MDEL failed\n");k_response_data_free(&response);k_buf_free(&args);goto stop; }
  k_response_data_free(&response);
  k_buf_free(&args);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_COUNT,0,0,0,0,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=1u||memcmp(response.body,"1",K_LEN("1"))!=0){ printf("selftest: COUNT after MDEL failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  /* MEMBERS: the Sec 6.1 inclusive directory, one id@host:client_port:peer_port per member */
  memset(&response,0,sizeof(response));
  i=sprintf(expect,"1@127.0.0.1:%u:%u,2@127.0.0.1:%u:%u,3@127.0.0.1:%u:%u",(unsigned)base_port,(unsigned)(base_port+10u),(unsigned)(base_port+1u),(unsigned)(base_port+11u),(unsigned)(base_port+2u),(unsigned)(base_port+12u));
  if(k_follow_call(&cluster,leader_index,K_REQ_MEMBERS,0,0,0,0,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=(k_u32)i||memcmp(response.body,expect,(size_t)i)!=0){ printf("selftest: MEMBERS failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  /* client-side discovery cache: seed parse + MEMBERS apply */
  memset(&ca,0,sizeof(ca));
  if(k_client_seed_parse(&ca,"127.0.0.1:1111,127.0.0.1:2222")!=0||ca.host_count!=2||ca.seed_count!=2||strcmp(ca.hosts[0],"127.0.0.1")!=0||ca.ports[0]!=1111||strcmp(ca.hosts[1],"127.0.0.1")!=0||ca.ports[1]!=2222){ printf("selftest: client seed parse failed\n");goto stop; }
  k_client_apply_members(&ca,"1@127.0.0.1:3333:3343,2@127.0.0.1:4444:4454");
  if(ca.host_count!=4||strcmp(ca.hosts[2],"127.0.0.1")!=0||ca.ports[2]!=3333||strcmp(ca.hosts[3],"127.0.0.1")!=0||ca.ports[3]!=4444){ printf("selftest: client members apply failed\n");goto stop; }
  memset(&response,0,sizeof(response));
  if(k_sync_rget_call(cluster.nodes[leader_index].host,cluster.nodes[leader_index].client_port,0,0,0,0,K_SCAN_ASC,0u,1000u,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=K_LEN("alpha=one")||memcmp(response.body,"alpha=one",K_LEN("alpha=one"))!=0){ printf("selftest: RGET failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  memset(&response,0,sizeof(response));
  if(k_sync_call(cluster.nodes[leader_index].host,cluster.nodes[leader_index].client_port,K_REQ_STATS,0,0,0,0,1000u,&response)!=0||response.status!=K_STATUS_OK||!response.body||!strstr((const char *)response.body,"count=1")||!strstr((const char *)response.body,"height=")||!strstr((const char *)response.body,"bytes=")||!strstr((const char *)response.body,"pending_frees=")||!strstr((const char *)response.body,"leader=")||!strstr((const char *)response.body,"term=")||!strstr((const char *)response.body,"commit=")||!strstr((const char *)response.body,"applied=")||!strstr((const char *)response.body,"snapshot=")||!strstr((const char *)response.body,"log=")||!strstr((const char *)response.body,"wal_segment=")||!strstr((const char *)response.body,"wal_next_offset=")||!strstr((const char *)response.body,"wal_pending=")){ printf("selftest: STATS failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_DEL,"alpha",5u,0,0,&response)!=0||response.status!=K_STATUS_OK){ printf("selftest: DEL failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_GET,"alpha",5u,0,0,&response)!=0||response.status!=K_STATUS_NOT_FOUND){ printf("selftest: deleted key still present\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  /* EXEC: transfer success */
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_SET,"a",1u,"100",3u,&response)!=0||response.status!=K_STATUS_OK){ printf("selftest: EXEC PUT a failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_SET,"b",1u,"200",3u,&response)!=0||response.status!=K_STATUS_OK){ printf("selftest: EXEC PUT b failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  memset(&args,0,sizeof(args));
  k_buf_u32(&args,1u); k_buf_bytes(&args,"a",1u);
  k_buf_u32(&args,1u); k_buf_bytes(&args,"b",1u);
  k_buf_u32(&args,2u); k_buf_bytes(&args,"50",2u);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_FCALL,"transfer",8u,args.data,args.len,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=2u||memcmp(response.body,"ok",K_LEN("ok"))!=0){ printf("selftest: EXEC transfer failed\n");k_response_data_free(&response);k_buf_free(&args);goto stop; }
  k_response_data_free(&response);
  k_buf_free(&args);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_GET,"a",1u,0,0,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=2u||memcmp(response.body,"50",K_LEN("50"))!=0){ printf("selftest: EXEC transfer GET a failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_GET,"b",1u,0,0,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=3u||memcmp(response.body,"250",K_LEN("250"))!=0){ printf("selftest: EXEC transfer GET b failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  /* EXEC: transfer insufficient balance (whole transaction rolls back) */
  memset(&args,0,sizeof(args));
  k_buf_u32(&args,1u); k_buf_bytes(&args,"a",1u);
  k_buf_u32(&args,1u); k_buf_bytes(&args,"b",1u);
  k_buf_u32(&args,3u); k_buf_bytes(&args,"999",3u);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_FCALL,"transfer",8u,args.data,args.len,&response)!=0||response.status!=K_STATUS_ERROR){ printf("selftest: EXEC transfer overflow status\n");k_response_data_free(&response);k_buf_free(&args);goto stop; }
  k_response_data_free(&response);
  k_buf_free(&args);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_GET,"a",1u,0,0,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=2u||memcmp(response.body,"50",K_LEN("50"))!=0){ printf("selftest: EXEC transfer overflow rollback failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_GET,"b",1u,0,0,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=3u||memcmp(response.body,"250",K_LEN("250"))!=0){ printf("selftest: EXEC transfer overflow rollback b failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  /* EXEC: consecutive transfer (serial: the second reads the first's committed result) */
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_SET,"c",1u,"50",2u,&response)!=0||response.status!=K_STATUS_OK){ printf("selftest: EXEC serial setup c failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_SET,"d",1u,"0",1u,&response)!=0||response.status!=K_STATUS_OK){ printf("selftest: EXEC serial setup d failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  memset(&args,0,sizeof(args));
  k_buf_u32(&args,1u); k_buf_bytes(&args,"c",1u);
  k_buf_u32(&args,1u); k_buf_bytes(&args,"d",1u);
  k_buf_u32(&args,2u); k_buf_bytes(&args,"20",2u);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_FCALL,"transfer",8u,args.data,args.len,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=2u||memcmp(response.body,"ok",K_LEN("ok"))!=0){ printf("selftest: EXEC transfer serial 1 failed\n");k_response_data_free(&response);k_buf_free(&args);goto stop; }
  k_response_data_free(&response);
  k_buf_free(&args);
  memset(&args,0,sizeof(args));
  k_buf_u32(&args,1u); k_buf_bytes(&args,"c",1u);
  k_buf_u32(&args,1u); k_buf_bytes(&args,"d",1u);
  k_buf_u32(&args,2u); k_buf_bytes(&args,"10",2u);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_FCALL,"transfer",8u,args.data,args.len,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=2u||memcmp(response.body,"ok",K_LEN("ok"))!=0){ printf("selftest: EXEC transfer serial 2 failed\n");k_response_data_free(&response);k_buf_free(&args);goto stop; }
  k_response_data_free(&response);
  k_buf_free(&args);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_GET,"c",1u,0,0,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=2u||memcmp(response.body,"20",K_LEN("20"))!=0){ printf("selftest: EXEC transfer serial GET c failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  /* CAS success: PUT lock=0, CAS lock 0->1, returns old value 0, GET lock==1 */
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_SET,"lock",4u,"0",1u,&response)!=0||response.status!=K_STATUS_OK){ printf("selftest: CAS setup PUT failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  memset(&args,0,sizeof(args));
  k_buf_u8(&args,(k_u8)K_CAS_CMP);
  k_buf_u32(&args,1u); k_buf_bytes(&args,"1",1u);
  k_buf_u32(&args,1u); k_buf_bytes(&args,"0",1u);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_CAS,"lock",4u,args.data,args.len,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=1u||memcmp(response.body,"0",K_LEN("0"))!=0){ printf("selftest: CAS success failed\n");k_response_data_free(&response);k_buf_free(&args);goto stop; }
  k_response_data_free(&response);
  k_buf_free(&args);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_GET,"lock",4u,0,0,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=1u||memcmp(response.body,"1",K_LEN("1"))!=0){ printf("selftest: CAS success GET failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  /* CAS conflict: CAS lock new=2 old=0 (current 1), returns CONFLICT + current 1, GET lock still 1 */
  memset(&args,0,sizeof(args));
  k_buf_u8(&args,(k_u8)K_CAS_CMP);
  k_buf_u32(&args,1u); k_buf_bytes(&args,"2",1u);
  k_buf_u32(&args,1u); k_buf_bytes(&args,"0",1u);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_CAS,"lock",4u,args.data,args.len,&response)!=0||response.status!=K_STATUS_CONFLICT||response.body_size!=1u||memcmp(response.body,"1",K_LEN("1"))!=0){ printf("selftest: CAS conflict failed\n");k_response_data_free(&response);k_buf_free(&args);goto stop; }
  k_response_data_free(&response);
  k_buf_free(&args);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_GET,"lock",4u,0,0,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=1u||memcmp(response.body,"1",K_LEN("1"))!=0){ printf("selftest: CAS conflict GET failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  /* SETNX: CAS k 9 (no old) -> set if not exists; retry conflicts.  Key k sorts
     before lock so the later MIN/MAX ordering checks still see lock as the max. */
  memset(&args,0,sizeof(args));
  k_buf_u8(&args,(k_u8)K_CAS_SETNX);
  k_buf_u32(&args,1u); k_buf_bytes(&args,"9",1u);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_CAS,"k",1u,args.data,args.len,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=0u){ printf("selftest: CAS SETNX success failed\n");k_response_data_free(&response);k_buf_free(&args);goto stop; }
  k_response_data_free(&response);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_CAS,"k",1u,args.data,args.len,&response)!=0||response.status!=K_STATUS_CONFLICT||response.body_size!=1u||memcmp(response.body,"9",K_LEN("9"))!=0){ printf("selftest: CAS SETNX conflict failed\n");k_response_data_free(&response);k_buf_free(&args);goto stop; }
  k_response_data_free(&response);
  k_buf_free(&args);
  memset(&response,0,sizeof(response));
  if(k_sync_call(cluster.nodes[leader_index].host,cluster.nodes[leader_index].client_port,K_REQ_INFO,0,0,0,0,1000u,&response)!=0||response.status!=K_STATUS_OK){ printf("selftest: INFO failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  for(i=0;i<70;i++){
    sprintf(key,"bulk-%03d",i);
    sprintf(value,"value-%03d",i);
    memset(&response,0,sizeof(response));
    if(k_follow_call(&cluster,leader_index,K_REQ_SET,key,(k_u32)strlen(key),value,(k_u32)strlen(value),&response)!=0||response.status!=K_STATUS_OK){ printf("selftest: bulk PUT failed at %d\n",i);k_response_data_free(&response);goto stop; }
    k_response_data_free(&response);
  }
  /* RGET pagination: limit=10, use the next_key cursor to accumulate all 70 bulk keys with no gaps */
  scan_cursor=(k_u8 *)K_MALLOC(5u);
  if(!scan_cursor){ printf("selftest: oom\n");goto stop; }
  memcpy(scan_cursor,"bulk-",5u);
  scan_cursor_len=5u;
  scan_total=0;
  scan_has_more=1;
  for(scan_rounds=0;scan_has_more&&scan_rounds<20;scan_rounds++){
    k_reader sr;
    k_u32 page_count=0,j;
    memset(&response,0,sizeof(response));
    if(k_sync_rget_call(cluster.nodes[leader_index].host,cluster.nodes[leader_index].client_port,scan_cursor,scan_cursor_len,"bulk-:",6u,K_SCAN_ASC,10u,1000u,&response)!=0||response.status!=K_STATUS_OK){ printf("selftest: RGET page failed\n");k_response_data_free(&response);K_FREE(scan_cursor);goto stop; }
    memset(&sr,0,sizeof(sr));
    sr.data=response.body;
    sr.len=response.body_size;
    scan_has_more=(int)k_reader_u8(&sr);
    if(scan_has_more){
      k_u32 nk=k_reader_u32(&sr);
      const k_u8 *nkp=k_reader_bytes(&sr,nk);
      K_FREE(scan_cursor);
      scan_cursor=(k_u8 *)K_MALLOC(nk?nk:1u);
      if(!scan_cursor){ k_response_data_free(&response);goto stop; }
      memcpy(scan_cursor,nkp,nk);
      scan_cursor_len=nk;
    }else{
      K_FREE(scan_cursor);
      scan_cursor=0;
      scan_cursor_len=0;
    }
    for(j=sr.off;j<sr.len;j++) if(sr.data[j]==(k_u8)'\n') page_count++;
    if(sr.len>sr.off) page_count++;
    scan_total+=(int)page_count;
    k_response_data_free(&response);
  }
  K_FREE(scan_cursor);
  if(scan_has_more){ printf("selftest: RGET pagination incomplete\n");goto stop; }
  if(scan_total!=70){ printf("selftest: RGET pagination count=%d\n",scan_total);goto stop; }
  /* MIN/MAX over multiple keys: MIN = lexicographically smallest present key ("a"), MAX = largest
     ("lock") - proves they return the true extremes (the single-key test earlier had min==max). */
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_MIN,0,0,0,0,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=K_LEN("a=50")||memcmp(response.body,"a=50",K_LEN("a=50"))!=0){ printf("selftest: MIN ordering failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_MAX,0,0,0,0,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=K_LEN("lock=1")||memcmp(response.body,"lock=1",K_LEN("lock=1"))!=0){ printf("selftest: MAX ordering failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  /* RGET bounded range [bulk-010, bulk-020): exactly bulk-010..019 in ascending byte order (half-open end) */
  memset(&response,0,sizeof(response));
  if(k_sync_rget_call(cluster.nodes[leader_index].host,cluster.nodes[leader_index].client_port,"bulk-010",8u,"bulk-020",8u,K_SCAN_ASC,0u,1000u,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=189u||memcmp(response.body,"bulk-010=value-010",K_LEN("bulk-010=value-010"))!=0||memcmp(response.body+171u,"bulk-019=value-019",K_LEN("bulk-019=value-019"))!=0){ printf("selftest: RGET range failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  /* RGET DESC: reverse byte order over the full bulk prefix - bulk-069 first, bulk-000 last */
  memset(&response,0,sizeof(response));
  if(k_sync_rget_call(cluster.nodes[leader_index].host,cluster.nodes[leader_index].client_port,"bulk-",5u,"bulk-:",6u,K_SCAN_DESC,0u,1000u,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=1329u||memcmp(response.body,"bulk-069=value-069",K_LEN("bulk-069=value-069"))!=0||memcmp(response.body+1311u,"bulk-000=value-000",K_LEN("bulk-000=value-000"))!=0){ printf("selftest: RGET desc failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  /* RDEL: delete bulk-000..bulk-029 (30 keys), response body = deleted count "30" */
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_RDEL,"bulk-000",8u,"bulk-030",8u,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=2u||memcmp(response.body,"30",K_LEN("30"))!=0){ printf("selftest: RDEL failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  for(i=0;i<70;i++){
    sprintf(key,"bulk-%03d",i);
    memset(&response,0,sizeof(response));
    if(k_follow_call(&cluster,leader_index,K_REQ_GET,key,(k_u32)strlen(key),0,0,&response)!=0){ printf("selftest: RDEL get check failed\n");k_response_data_free(&response);goto stop; }
    if((i<30&&response.status!=K_STATUS_NOT_FOUND)||(i>=30&&response.status!=K_STATUS_OK)){ printf("selftest: RDEL bulk-%03d wrong\n",i);k_response_data_free(&response);goto stop; }
    k_response_data_free(&response);
  }
  snapshot_seen=0;
  deadline=k_deadline_us(2000u);
  for(;;){
    memset(&response,0,sizeof(response));
    if(k_sync_call(cluster.nodes[leader_index].host,cluster.nodes[leader_index].client_port,K_REQ_INFO,0,0,0,0,1000u,&response)==0&&response.status==K_STATUS_OK&&response.body){
      marker=strstr((const char *)response.body,"snapshot=");
      snapshot_index=0;
      if(marker&&sscanf(marker,"snapshot=%" K_I64_FMT,&snapshot_index)==1&&snapshot_index>0) snapshot_seen=1;
    }
    k_response_data_free(&response);
    if(snapshot_seen) break;
    if(k_deadline_passed(deadline)) break;
    runtime_msleep(20u);
  }
  if(!snapshot_seen){ printf("selftest: snapshot threshold did not produce a snapshot\n");goto stop; }
  /* MEMBER: remove node 3 from the voter set, then add it back */
  ids3[0]=3;
  memset(&response,0,sizeof(response));
  if(k_follow_member(&cluster,leader_index,K_MEMBER_REMOVE,ids3,1,0,0,0,&response)!=0||response.status!=K_STATUS_OK){ printf("selftest: MEMBER REMOVE failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  /* self-removal steps the old leader down with leader_id=0 until the next
     heartbeat: re-locate the new leader before ADD */
  leader_index=k_wait_leader(&cluster);
  if(leader_index<0){ printf("selftest: leader re-election after self-removal failed\n");goto stop; }
  memset(&response,0,sizeof(response));
  if(k_follow_member(&cluster,leader_index,K_MEMBER_ADD,ids3,1,0,0,0,&response)!=0||response.status!=K_STATUS_OK){ printf("selftest: MEMBER ADD failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_SET,"durable",7u,"value",5u,&response)!=0||response.status!=K_STATUS_OK){ printf("selftest: durable PUT failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  if(k_embedded_shutdown(&cluster)!=0){ printf("selftest: first network shutdown failed\n");goto stop; }
  runtime_wait_workers_exit(runtime);
  runtime_destroy(runtime);
  runtime=0;
  for(i=0;i<3;i++) if(!k_wal_files_valid(bases[i])){ printf("selftest: WAL segment/meta validation failed for node %d\n",i+1);goto stop; }
  for(i=0;i<3;i++) k_server_init(&servers[i],i+1,cluster.nodes[i].client_port,cluster.nodes[i].peer_port,bases[i],&cluster);
  runtime=k_embedded_start(servers);
  if(!runtime){ printf("selftest: restart failed\n");goto stop; }
  leader_index=k_wait_leader(&cluster);
  if(leader_index<0){ printf("selftest: post-restart election failed\n");goto stop; }
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_GET,"durable",7u,0,0,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=5u||memcmp(response.body,"value",K_LEN("value"))!=0){ printf("selftest: recovery GET failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_GET,"a",1u,0,0,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=2u||memcmp(response.body,"50",K_LEN("50"))!=0){ printf("selftest: recovery EXEC a failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  memset(&response,0,sizeof(response));
  if(k_follow_call(&cluster,leader_index,K_REQ_GET,"b",1u,0,0,&response)!=0||response.status!=K_STATUS_OK||response.body_size!=3u||memcmp(response.body,"250",K_LEN("250"))!=0){ printf("selftest: recovery EXEC b failed\n");k_response_data_free(&response);goto stop; }
  k_response_data_free(&response);
  if(k_selftest_bootstrap(&cluster,leader_index,base_port,4)!=0){ printf("selftest: bootstrap failed\n");goto stop; }
  if(k_selftest_bootstrap_low()!=0){ printf("selftest: low-id bootstrap failed\n");goto stop; }
  if(k_selftest_member_failover()!=0){ printf("selftest: member failover failed\n");goto stop; }
  if(k_selftest_auto_replace()!=0){ printf("selftest: auto-replace failed\n");goto stop; }
  if(k_selftest_remove_readd()!=0){ printf("selftest: remove-readd failed\n");goto stop; }
  if(k_selftest_remove_leader_readd()!=0){ printf("selftest: remove-leader-readd failed\n");goto stop; }
  if(k_selftest_treap_stats()!=0){ printf("selftest: treap stats failed\n");goto stop; }
  if(k_selftest_silent_standby()!=0){ printf("selftest: silent-standby failed\n");goto stop; }
  if(k_selftest_strtou64()!=0){ printf("selftest: strtou64 failed\n");goto stop; }
  rc=0;
stop:
  if(runtime){
    if(k_embedded_shutdown(&cluster)!=0){
      printf("selftest: network shutdown failed\n");
      rc=-1;
      runtime_stop(runtime);
    }
    runtime_wait_workers_exit(runtime);
    runtime_destroy(runtime);
  }
  k_embedded_clean(servers);
  if(rc==0) printf("selftest: PASS\n");
  return rc;
}


int main(void){
  return k_run_selftest()==0?EXIT_SUCCESS:EXIT_FAILURE;
}
