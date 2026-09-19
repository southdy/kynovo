/* ============================================================================
 * kdbsvr.c -- the kynovo server application (production deploy).
 *
 *   kdbsvr init <base-uri>
 *   kdbsvr server <id> <client-port> <peer-port> <base-uri> <cluster-spec>
 *                 [--auto-replace <id@ipv4:client-port:peer-port>]
 *                 [--auto-replace-threshold <N>]
 *
 *   build: ./build.sh kdbsvr
 * ============================================================================
 */
#define VFS_IMPLEMENTATION
#include "vfs.h"
#define TREAP_IMPLEMENTATION
#include "treap.h"
#define RUNTIME_IMPLEMENTATION
#include "runtime.h"
#define CEMON_IMPLEMENTATION
#include "cemon.h"
#define RAFT_IMPLEMENTATION
#include "raft.h"
#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#else
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <time.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include "kbase.h"
#include "kproto.h"
#include "kserver.h"

/* Process-shutdown flag: set by a POSIX signal handler or the Windows console
   control handler; polled by k_server_run so shutdown is graceful (the handler
   itself only sets a flag -- nothing heavier is async-signal-safe). */
static volatile sig_atomic_t g_stop=0;

static void k_on_signal(int sig){
  (void)sig;
  g_stop=1;
}

#if defined(_WIN32)
static BOOL WINAPI k_on_console_ctrl(DWORD ctrl){
  (void)ctrl;
  g_stop=1;
  return TRUE;
}
#endif

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
    /* cemon frees the socket object when the connection dies, so the handle must be cleared
       BEFORE the close and never used afterwards (cemon_close dereferences the socket, so a
       second close of the same handle is a use-after-free, not an error return). */
    void *dead=conn->sock;
    conn->sock=0;
    if(dead) conn->server->transport->close(dead);
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
    /* cemon frees the socket object when the connection dies, so the handle must be cleared
       BEFORE the close and never used afterwards (cemon_close dereferences the socket, so a
       second close of the same handle is a use-after-free, not an error return). */
    void *dead=conn->sock;
    conn->sock=0;
    if(dead) conn->server->transport->close(dead);
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
/* Real elapsed milliseconds since the previous advance, with the sub-millisecond
   remainder carried (k_server_elapsed_step).  This is the driver's ONLY clock
   source: the periodic tick timer it replaced made the flush window coarse (both
   the timer period and the poll timeout were poll_ms, so a queued write could
   wait a full poll_ms even though flush_timeout_ms was 3), and two clocks
   advancing the same timers would double-count election time.
   The step helper deliberately does NOT round a sub-millisecond loop up to 1 ms:
   this loop runs once per drained network completion (thousands of times per
   second under load), so rounding up would make the logical clock run several
   times faster than wall time and fire election/heartbeat/flush early. */
static unsigned int k_server_elapsed_ms(k_server *server){
  k_u64 now_us;
  if(!server) return 0;
  if(k_monotonic_us(&now_us)!=0) return 0;   /* no clock: advance nothing rather than invent time */
  return k_server_elapsed_step(now_us,&server->last_tick_us,1000u);
}
/* App-layer cemon attachment (stays in the app, not kserver.h): bind the event
   loop, listen on the client/peer ports, then admit and run the initial
   reconnect.  k_server_open above is pure init (no link I/O). */
static void k_server_on_stop_cemon(void *loop){
  cemon_stop((cemon *)loop);
}
static void k_server_wal_wake_cb(cemon *loop,void *ud){
  /* Wake-only: cemon_post itself wakes the event loop; the main loop drains the
     WAL result and advances in k_server_run right after cemon_poll returns. */
  (void)loop;
  (void)ud;
}
static void k_server_wal_wake(void *loop,void *ud){
  if(loop) cemon_post((cemon *)loop,k_server_wal_wake_cb,ud);
}
static int k_server_serve(k_server *server,cemon *loop){
  int local_index;
  local_index=k_cluster_index(&server->cluster,server->id);
  if(local_index<0) return -1;
  server->loop=loop;
  server->transport=&k_transport_cemon;
  server->on_stop=k_server_on_stop_cemon;
  server->on_wal_result=k_server_wal_wake;
  if(!cemon_tcp_listen(loop,k_numeric_host(server->cluster.nodes[local_index].host),server->peer_port,k_server_peer_io,server)) return -1;
  if(!cemon_tcp_listen(loop,k_numeric_host(server->cluster.nodes[local_index].host),server->client_port,k_server_client_io,server)) return -1;
  fprintf(stderr,"[cfg] poll_ms=%u flush_timeout_ms=%u flush_item_limit=%u flush_bytes_limit=%u (batch targets)\n",(unsigned)server->cfg.poll_ms,(unsigned)server->cfg.flush_timeout_ms,(unsigned)server->cfg.flush_item_limit,(unsigned)server->cfg.flush_bytes_limit);
  server->admission=1;
  if(k_monotonic_us(&server->last_tick_us)!=0) server->last_tick_us=0;
  k_server_reconnect(server);
  return 0;
}
static int k_server_run(k_server *server){
  int rc=0;
  for(;;){
    k_u64 arrived_before,flushes_before,round_t0,round_t1;
    k_u32 pending_before;
    int timeout;
    unsigned int elapsed_ms,batch_ms;
    arrived_before=server->writes_arrived;
    pending_before=server->write_count;
    flushes_before=server->flush_batches;
    /* Wake at the earliest of the poll period and the remaining flush deadline,
       so a queued write is submitted within flush_timeout_ms of its arrival even
       when the loop has no other input to wake it. */
    timeout=(int)server->cfg.poll_ms;
    { int left=k_server_flush_deadline_ms(server);
      if(left>=0&&left<timeout) timeout=left; }
    if(cemon_poll((cemon *)server->loop,timeout)!=0) break;
    if(server->fatal){
      /* Fail-stop: a fatal condition (apply could not be replayed, a snapshot
         load/copy failed, ...) means the state machine no longer matches the
         log, so stop serving instead of running on a broken tree.  The reason
         was printed where the flag was set; exit non-zero so a supervisor can
         restart the node. */
      printf("server %d: fatal condition, shutting down\n",server->id);
      break;
    }
    if(g_stop&&!server->stopping){
      printf("server %d: signal received, shutting down\n",server->id);
      k_server_begin_stop(server);
    }
    /* One clock source: the elapsed wall time since the previous iteration, measured
       here and handed to the state machine (k_server_advance_at), which then decides
       whether the queued writes form a batch worth submitting (k_server_flush_if_ready
       for the structural rules - item/byte target, input drained - and the elapsed
       age for the flush_timeout_ms window).  There is no periodic tick timer: the poll
       timeout is clamped to the remaining window, so the loop wakes exactly when the
       window expires.  Batch size and the submit reason are observable in STATS
       (flush_writes/flush_batches and flush_by_*). */
     /* DATA callbacks run inside cemon_poll, so compare the counter across the
       whole poll+drive round.  Sampling after poll would classify every busy
       round as drained and defeat structural group commit. */
    /* Wake, split from work: the worker stamped wal_post_us when it posted a finished bundle; this
       point is the START of the loop's next round, so the gap is how long the loop was not running.
       wake_us_* (below, after the round) adds that round's work on top. */
    if(server->wal_post_us&&server->wal_post_us!=server->wake_pre_seen){
      k_u64 wp=0;
      server->wake_pre_seen=server->wal_post_us;
      if(k_monotonic_us(&wp)==0&&wp>=server->wal_post_us){
        k_u64 pre=wp-server->wal_post_us;
        server->wake_pre_us_last=pre;
        if(pre>server->wake_pre_us_max) server->wake_pre_us_max=pre;
        if(pre>K_SLOW_WAKE_US) server->slow_wake_pres++;
        server->wake_pre_samples++;
      }
    }
    if(k_monotonic_us(&round_t0)!=0) round_t0=0;   /* round work starts here, after the poll's wait */
    elapsed_ms=k_server_elapsed_ms(server);
    /* Two quantities: Raft timers, reconnects and the snapshot policy measure real
       wall time (it really did pass), while the batch age only counts the time since
       the batch that is queued now was born (k_server_batch_elapsed_ms). */
    batch_ms=k_server_batch_elapsed_ms(server,elapsed_ms,pending_before,flushes_before);
    k_server_advance_at(server,elapsed_ms,batch_ms);
    /* The writes that arrived during this poll got here only now, so the wait
       before their arrival must not count as their batch age: otherwise a fresh
       batch already looks older than flush_timeout_ms and is flushed at once
       (one write per batch).  Advance the Raft clock first (it really did pass
       in wall time), then start the batch's own age. */
    k_server_flush_if_ready(server,server->writes_arrived>arrived_before);
    /* Wake handoff: the worker stamped wal_post_us when it posted a finished bundle; the round that
       just ended is the round that could have consumed it, so the gap is how long the loop took to be
       told.  Real clock on purpose - this is a diagnostic in the driver, not a core time input. */
    if(server->wal_post_us&&server->wal_post_us!=server->wake_us_seen){
      k_u64 wake_now=0;
      server->wake_us_seen=server->wal_post_us;
      if(k_monotonic_us(&wake_now)==0&&wake_now>=server->wal_post_us){
        k_u64 wake_us=wake_now-server->wal_post_us;
        server->wake_us_last=wake_us;
        if(wake_us>server->wake_us_max) server->wake_us_max=wake_us;
        if(wake_us>K_SLOW_WAKE_US) server->slow_wakes++;
        server->wake_samples++;   /* 0 samples means the measurement never ran, not that it was fast */
      }
    }
    if(round_t0&&k_monotonic_us(&round_t1)==0){
      k_u64 round_us=round_t1-round_t0;
      server->round_us_last=round_us;
      if(round_us>server->round_us_max) server->round_us_max=round_us;
      server->round_us_ewma=server->round_us_ewma?((server->round_us_ewma*7u+round_us)/8u):round_us;
      if(round_us>K_SLOW_ROUND_US) server->slow_rounds++;
    }
    /* Adaptive flush window, derived from MEASURED cost instead of a guessed constant.
       The window exists only so a batch can accumulate, so it need not exceed the cost
       of the sync the batch will trigger.  At low load the window dominates the latency
       (measured at K=1: p50 5.6 ms = 3 ms window + 1.9 ms record write+sync), while at
       high load the item target closes batches and the window is inert - so shrinking
       it buys latency and costs nothing.  The policy lives here in the driver; the core
       still reads cfg.flush_timeout_ms as an injected value. */
    {
      k_u64 sync_ewma=server->wal_worker.sync_us_ewma;
      if(sync_ewma>0){
        k_u32 want=(k_u32)((sync_ewma+999u)/1000u);
        if(want<1u) want=1u;
        if(want>server->flush_window_max_ms) want=server->flush_window_max_ms;
        if(want!=server->cfg.flush_timeout_ms) server->cfg.flush_timeout_ms=want;
      }
    }
  }
  if(!server->stopped||server->fatal) rc=-1;
  return rc;
}

static void k_usage(void){
  printf("usage:\n");
  printf("  kdbsvr init <base-uri>\n");
  printf("  kdbsvr server <id> <client-port> <peer-port> <base-uri> <cluster-spec> [--auto-replace <id@ipv4:client-port:peer-port>] [--auto-replace-threshold <N>]\n");
  printf("cluster-spec: id@ipv4:client-port:peer-port[,id@ipv4:client-port:peer-port...]\n");
  printf("example: 1@127.0.0.1:7001:7101,2@127.0.0.1:7002:7102,3@127.0.0.1:7003:7103\n");
}

static int k_run_server_args(int argc,char **argv){
  k_cluster cluster;
  k_cluster replacement;
  k_server server;
  cemon *loop;
  int id,client_port,peer_port,node_index,rc;
  int threshold=3;
#ifdef _WIN32
  /* Raise the system timer resolution to 1ms for the life of the server.
     Windows defaults to a ~15.6ms tick, under which flush_timeout_ms (3ms) and
     poll_ms (10ms) would actually fire at ~16ms -- the batching window and the
     event poll would both be 5x coarser than configured. The setting is
     process-scoped and the OS restores it when the server exits. */
  if(timeBeginPeriod(1)!=TIMERR_NOERROR) fprintf(stderr,"warning: timeBeginPeriod(1) failed; poll and flush timing may be up to 5x coarser than configured\n");
#endif
  if(argc<7||k_parse_uint(argv[2],2147483647,&id)!=0||k_parse_uint(argv[3],65535,&client_port)!=0||k_parse_uint(argv[4],65535,&peer_port)!=0||strlen(argv[5])>=K_URI_MAX||strstr(argv[5],"://")==0){
    printf("kdbsvr: fatal: bad arguments for 'server' (need <id> <client-port> <peer-port> <base-uri> <cluster-spec>)\n");
    k_usage();
    return -1;
  }
  if(k_cluster_parse(&cluster,argv[6])!=0){
    printf("kdbsvr: fatal: cannot parse cluster spec '%s' (need id@ipv4:client-port:peer-port[,id@...] - comma separated)\n",argv[6]);
    return -1;
  }
  node_index=k_cluster_index(&cluster,id);
  if(node_index<0||cluster.nodes[node_index].client_port!=(unsigned short)client_port||cluster.nodes[node_index].peer_port!=(unsigned short)peer_port) return -1;
  k_server_init(&server,id,(unsigned short)client_port,(unsigned short)peer_port,argv[5],&cluster);
  /* optional --auto-replace <id@host:client_port:peer_port> [--auto-replace-threshold <N>] */
  if(argc>=9&&strcmp(argv[7],"--auto-replace")==0){
    if(k_cluster_parse(&replacement,argv[8])!=0||replacement.count!=1) return -1;
    server.auto_replace=1;
    server.auto_replace_new_id=replacement.nodes[0].id;
    strcpy(server.auto_replace_new_host,replacement.nodes[0].host);
    server.auto_replace_new_client_port=replacement.nodes[0].client_port;
    server.auto_replace_new_peer_port=replacement.nodes[0].peer_port;
    if(argc==11&&strcmp(argv[9],"--auto-replace-threshold")==0){
      if(k_parse_uint(argv[10],1000000,&threshold)!=0||threshold<1) return -1;
    }else if(argc!=9) return -1;
    server.auto_replace_threshold=(unsigned int)threshold;
  }else if(argc!=7) return -1;
  if(k_server_open(&server)!=0){ k_server_release(&server);printf("failed to start server %d\n",id);return -1; }
  loop=cemon_create();
  /* bind ownership explicitly: this thread drives (polls) and tears down the loop,
     so a helper thread can never capture ownership and lock it out */
  if(loop) cemon_bind_owner(loop);
  if(!loop||k_server_serve(&server,loop)!=0){
    if(loop) cemon_destroy(loop);
    k_server_release(&server);
    printf("failed to start server %d\n",id);
    return -1;
  }
  printf("server %d client=%s:%u peer=%s:%u\n",id,cluster.nodes[node_index].host,(unsigned)client_port,cluster.nodes[node_index].host,(unsigned)peer_port);
  if(server.auto_replace) printf("auto-replace enabled: replacement=%d@%s:%u:%u threshold=%u\n",server.auto_replace_new_id,server.auto_replace_new_host,(unsigned)server.auto_replace_new_client_port,(unsigned)server.auto_replace_new_peer_port,(unsigned)server.auto_replace_threshold);
#if defined(_WIN32)
  if(!SetConsoleCtrlHandler(k_on_console_ctrl,TRUE)) fprintf(stderr,"warning: no console control handler installed; Ctrl+C will not stop the server gracefully (use SHUTDOWN from a client)\n");
#else
  signal(SIGINT,k_on_signal);
  signal(SIGTERM,k_on_signal);
#endif
  rc=k_server_run(&server);
  if(rc!=0) printf("server %d exited: fatal=%d\n",id,server.fatal);
  else printf("server %d stopped\n",id);
  cemon_destroy(loop);
  k_server_release(&server);
  return rc;
}
static int k_run_init_args(int argc,char **argv){
  k_cfg cfg;
  unsigned int value;
  int i;
  if(argc<3||k_cfg_reset(argv[2])!=0||k_wal_meta_init(argv[2])!=0) return -1;
  if(argc==3){
    printf("initialized configuration for %s\n",argv[2]);
    return 0;
  }
  /* The batch target and the flush window are the two parameters that trade throughput against tail
     latency, so they are settable where the store is created instead of only being compile-time
     defaults.  The server prints them back at startup ([cfg] ...), which is the read-back that proves
     what was actually stored.  Unknown options fail loudly rather than being ignored. */
  if(k_cfg_load(argv[2],&cfg)!=0) return -1;
  for(i=3;i<argc;i++){
    if(i+1>=argc){
      printf("kdbsvr: fatal: option '%s' needs a value\n",argv[i]);
      return -1;
    }
    if(strcmp(argv[i],"--flush-items")==0){
      if(k_parse_uint(argv[i+1],65536,&value)!=0||value<1u){ printf("kdbsvr: fatal: --flush-items must be 1..65536\n"); return -1; }
      cfg.flush_item_limit=value;
    }else if(strcmp(argv[i],"--flush-window-ms")==0){
      if(k_parse_uint(argv[i+1],60000,&value)!=0||value<1u){ printf("kdbsvr: fatal: --flush-window-ms must be 1..60000\n"); return -1; }
      cfg.flush_timeout_ms=value;
    }else if(strcmp(argv[i],"--flush-bytes")==0){
      if(k_parse_uint(argv[i+1],268435456,&value)!=0||value<1u){ printf("kdbsvr: fatal: --flush-bytes must be 1..268435456\n"); return -1; }
      cfg.flush_bytes_limit=value;
    }else{
      printf("kdbsvr: fatal: unknown init option '%s'\n",argv[i]);
      return -1;
    }
    i++;
  }
  if(k_cfg_validate(&cfg)!=0){
    printf("kdbsvr: fatal: configuration rejected by validation\n");
    return -1;
  }
  if(k_cfg_store(argv[2],&cfg)!=0) return -1;
  printf("initialized configuration for %s (flush_items=%u flush_window_ms=%u flush_bytes=%u)\n",
         argv[2],(unsigned)cfg.flush_item_limit,(unsigned)cfg.flush_timeout_ms,(unsigned)cfg.flush_bytes_limit);
  return 0;
}


int main(int argc,char **argv){
  int rc;
  /* UNBUFFERED: an operator must see the startup banner and any fatal reason even when
     the log is redirected to a file, and a hard kill must not swallow them.  _IOLBF was
     not enough here (the redirected log stayed empty while the server was running), and
     the server's stdout volume is tiny, so correctness of visibility wins. */
  setvbuf(stdout,NULL,_IONBF,0);
  if(argc>=2&&strcmp(argv[1],"init")==0) rc=k_run_init_args(argc,argv);
  else if(argc>=2&&strcmp(argv[1],"server")==0) rc=k_run_server_args(argc,argv);
  else{ k_usage();return EXIT_FAILURE; }
  return rc==0?EXIT_SUCCESS:EXIT_FAILURE;
}
