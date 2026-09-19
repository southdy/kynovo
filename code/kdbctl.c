/* ============================================================================
 * kdbctl.c -- the kynovo CLI application (ops: interactive client).
 *
 *   kdbctl <host:port[,host:port...]>
 *
 *   build: ./build.sh kdbctl
 * ============================================================================
 */
#define CLI_IMPLEMENTATION
#include "cli.h"
#define CEMON_IMPLEMENTATION
#include "cemon.h"
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
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include "kbase.h"
#include "kproto.h"
#include "kclient.h"

#define K_LEN(s) ((k_u32)(sizeof(s)-1u))
/* ================= Constants: client limits ================= */
/* client-side limits */
#define K_CLIENT_LINE_MAX 1024
#define K_CLIENT_ARG_MAX 16
#define K_CLIENT_HIST_MAX 32
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

/* ================= Client app layer: cemon adapter + CLI ================= */
/* cemon io callback: thin adapter that only dispatches to the pure handlers
   above and performs the continuation read. */
static void k_client_io(cemon_socket *sock,const cemon_event *event){
  k_client_app *app=(k_client_app *)cemon_getud(sock);
  if(!app||!event) return;
  /* Refresh the INJECTED clock here, per inbound event, before the state machine sees the frame.
     k_client_response_frame derives each request's latency from app->now_us, so a clock refreshed
     only once per loop turn made every completion delivered in the same turn read as 0 us. */
  if(k_monotonic_us(&app->now_us)!=0) app->now_us=0;
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
static int k_cli_member(cli_ctx *cli,int argc,char **argv){
  k_client_app *app=(k_client_app *)cli->ctx;
  int subcmd,id_count=0;
  int ids[K_MAX_NODES];
  char hosts[K_MAX_NODES][K_HOST_MAX];
  unsigned short client_ports[K_MAX_NODES];
  unsigned short peer_ports[K_MAX_NODES];
  if(argc<2||argc>3){ cli_print(cli,"usage: MEMBER ADD|REMOVE|RECONFIG <id,id,...>");return 0; }
  if(strcmp(argv[1],"ADD")==0) subcmd=K_MEMBER_ADD;
  else if(strcmp(argv[1],"REMOVE")==0) subcmd=K_MEMBER_REMOVE;
  else if(strcmp(argv[1],"RECONFIG")==0) subcmd=K_MEMBER_RECONFIG;
  else{ cli_print(cli,"usage: MEMBER ADD|REMOVE|RECONFIG <id,id,...>");return 0; }
  if(argc==3){
    const char *cursor=argv[2];
    while(*cursor&&id_count<K_MAX_NODES){
      const char *comma=strchr(cursor,',');
      char item[192];
      size_t len=comma?(size_t)(comma-cursor):strlen(cursor);
      if(len==0||len>=sizeof(item)){ id_count=0; break; }
      memcpy(item,cursor,len); item[len]='\0';
      if(subcmd==K_MEMBER_ADD){
        /* ADD item form: id@host:client_port:peer_port */
        char *at=strchr(item,'@');
        char *colon1,*colon2;
        int id,client_port,peer_port;
        size_t host_len;
        if(!at){ id_count=0; break; }
        *at='\0';
        colon1=strchr(at+1,':');
        if(!colon1){ id_count=0; break; }
        *colon1='\0';
        colon2=strchr(colon1+1,':');
        if(!colon2||strchr(colon2+1,':')){ id_count=0; break; }
        *colon2='\0';
        host_len=strlen(at+1);
        if(!host_len||host_len>=K_HOST_MAX){ id_count=0; break; }
        if(k_parse_uint(item,65535,&id)!=0||k_parse_uint(colon1+1,65535,&client_port)!=0||k_parse_uint(colon2+1,65535,&peer_port)!=0){ id_count=0; break; }
        ids[id_count]=id;
        memcpy(hosts[id_count],at+1,host_len+1u);
        client_ports[id_count]=(unsigned short)client_port;
        peer_ports[id_count]=(unsigned short)peer_port;
      }else{
        int id;
        if(k_parse_uint(item,65535,&id)!=0){ id_count=0; break; }
        ids[id_count]=id;
      }
      id_count++;
      if(!comma) break;
      cursor=comma+1;
    }
  }
  if(id_count<1){ cli_print(cli,"usage: MEMBER ADD|REMOVE|RECONFIG <id,id,...>");return 0; }
  if(k_client_queue_member(app,subcmd,ids,id_count,(const char (*)[K_HOST_MAX])hosts,client_ports,peer_ports)!=0) cli_print(cli,"error: could not queue request");
  return 0;
}
static int k_cli_help(cli_ctx *cli,int argc,char **argv){
  k_client_app *app=(k_client_app *)cli->ctx;
  (void)argv;
  if(argc!=1){ cli_print(cli,"usage: HELP");return 0; }
  if(k_client_queue(app,K_REQ_HELP,0,0,0,0)!=0) cli_print(cli,"error: could not queue request");
  return 0;
}
static int k_cli_set(cli_ctx *cli,int argc,char **argv){
  k_client_app *app=(k_client_app *)cli->ctx;
  if(argc!=3){ cli_print(cli,"usage: SET <key> <value>");return 0; }
  if(k_client_queue(app,K_REQ_SET,argv[1],(k_u32)strlen(argv[1]),argv[2],(k_u32)strlen(argv[2]))!=0) cli_print(cli,"error: could not queue request");
  return 0;
}
static int k_cli_get(cli_ctx *cli,int argc,char **argv){
  k_client_app *app=(k_client_app *)cli->ctx;
  if(argc!=2){ cli_print(cli,"usage: GET <key>");return 0; }
  if(k_client_queue(app,K_REQ_GET,argv[1],(k_u32)strlen(argv[1]),0,0)!=0) cli_print(cli,"error: could not queue request");
  return 0;
}
static int k_cli_del(cli_ctx *cli,int argc,char **argv){
  k_client_app *app=(k_client_app *)cli->ctx;
  if(argc!=2){ cli_print(cli,"usage: DEL <key>");return 0; }
  if(k_client_queue(app,K_REQ_DEL,argv[1],(k_u32)strlen(argv[1]),0,0)!=0) cli_print(cli,"error: could not queue request");
  return 0;
}
static int k_cli_count(cli_ctx *cli,int argc,char **argv){
  k_client_app *app=(k_client_app *)cli->ctx;
  const char *begin=0,*end=0;
  if(argc>3){ cli_print(cli,"usage: COUNT [begin [end]]");return 0; }
  if(argc>=2&&strcmp(argv[1],"-")!=0) begin=argv[1];
  if(argc>=3&&strcmp(argv[2],"-")!=0) end=argv[2];
  if(k_client_queue(app,K_REQ_COUNT,begin,begin?(k_u32)strlen(begin):0u,end,end?(k_u32)strlen(end):0u)!=0) cli_print(cli,"error: could not queue request");
  return 0;
}
static int k_cli_min(cli_ctx *cli,int argc,char **argv){
  k_client_app *app=(k_client_app *)cli->ctx;
  const char *begin=0,*end=0;
  if(argc>3){ cli_print(cli,"usage: MIN [begin [end]]");return 0; }
  if(argc>=2&&strcmp(argv[1],"-")!=0) begin=argv[1];
  if(argc>=3&&strcmp(argv[2],"-")!=0) end=argv[2];
  if(k_client_queue(app,K_REQ_MIN,begin,begin?(k_u32)strlen(begin):0u,end,end?(k_u32)strlen(end):0u)!=0) cli_print(cli,"error: could not queue request");
  return 0;
}
static int k_cli_max(cli_ctx *cli,int argc,char **argv){
  k_client_app *app=(k_client_app *)cli->ctx;
  const char *begin=0,*end=0;
  if(argc>3){ cli_print(cli,"usage: MAX [begin [end]]");return 0; }
  if(argc>=2&&strcmp(argv[1],"-")!=0) begin=argv[1];
  if(argc>=3&&strcmp(argv[2],"-")!=0) end=argv[2];
  if(k_client_queue(app,K_REQ_MAX,begin,begin?(k_u32)strlen(begin):0u,end,end?(k_u32)strlen(end):0u)!=0) cli_print(cli,"error: could not queue request");
  return 0;
}
static int k_cli_mset(cli_ctx *cli,int argc,char **argv){
  k_client_app *app=(k_client_app *)cli->ctx;
  k_buf list;
  int i;
  memset(&list,0,sizeof(list));
  if(argc<3||((argc-1)&1)!=0){ cli_print(cli,"usage: MSET <key> <value> ...");return 0; }
  k_buf_u32(&list,(k_u32)((argc-1)/2));
  for(i=1;i<argc;i+=2){
    k_u32 klen=(k_u32)strlen(argv[i]);
    k_u32 vlen=(k_u32)strlen(argv[i+1]);
    k_buf_u32(&list,klen);
    k_buf_bytes(&list,(const k_u8*)argv[i],klen);
    k_buf_u32(&list,vlen);
    k_buf_bytes(&list,(const k_u8*)argv[i+1],vlen);
  }
  if(list.err){ k_buf_free(&list); cli_print(cli,"error: out of memory"); return 0; }
  if(k_client_queue(app,K_REQ_MSET,list.data,list.len,0,0)!=0) cli_print(cli,"error: could not queue request");
  k_buf_free(&list);
  return 0;
}
static int k_cli_mdel(cli_ctx *cli,int argc,char **argv){
  k_client_app *app=(k_client_app *)cli->ctx;
  k_buf list;
  int i;
  memset(&list,0,sizeof(list));
  if(argc<2){ cli_print(cli,"usage: MDEL <key> ...");return 0; }
  k_buf_u32(&list,(k_u32)(argc-1));
  for(i=1;i<argc;i++){
    k_u32 klen=(k_u32)strlen(argv[i]);
    k_buf_u32(&list,klen);
    k_buf_bytes(&list,(const k_u8*)argv[i],klen);
  }
  if(list.err){ k_buf_free(&list); cli_print(cli,"error: out of memory"); return 0; }
  if(k_client_queue(app,K_REQ_MDEL,list.data,list.len,0,0)!=0) cli_print(cli,"error: could not queue request");
  k_buf_free(&list);
  return 0;
}
static int k_cli_mget(cli_ctx *cli,int argc,char **argv){
  k_client_app *app=(k_client_app *)cli->ctx;
  k_buf list;
  int i;
  memset(&list,0,sizeof(list));
  if(argc<2){ cli_print(cli,"usage: MGET <key> ...");return 0; }
  k_buf_u32(&list,(k_u32)(argc-1));
  for(i=1;i<argc;i++){
    k_u32 klen=(k_u32)strlen(argv[i]);
    k_buf_u32(&list,klen);
    k_buf_bytes(&list,(const k_u8*)argv[i],klen);
  }
  if(list.err){ k_buf_free(&list); cli_print(cli,"error: out of memory"); return 0; }
  if(k_client_queue(app,K_REQ_MGET,list.data,list.len,0,0)!=0) cli_print(cli,"error: could not queue request");
  k_buf_free(&list);
  return 0;
}
static int k_cli_rset(cli_ctx *cli,int argc,char **argv){
  k_client_app *app=(k_client_app *)cli->ctx;
  k_buf packed;
  if(argc!=4){ cli_print(cli,"usage: RSET <begin> <end> <value>");return 0; }
  memset(&packed,0,sizeof(packed));
  k_buf_u32(&packed,(k_u32)strlen(argv[1]));
  k_buf_bytes(&packed,(const k_u8*)argv[1],(k_u32)strlen(argv[1]));
  k_buf_u32(&packed,(k_u32)strlen(argv[2]));
  k_buf_bytes(&packed,(const k_u8*)argv[2],(k_u32)strlen(argv[2]));
  if(packed.err){ k_buf_free(&packed); cli_print(cli,"error: out of memory"); return 0; }
  if(k_client_queue(app,K_REQ_RSET,packed.data,packed.len,argv[3],(k_u32)strlen(argv[3]))!=0) cli_print(cli,"error: could not queue request");
  k_buf_free(&packed);
  return 0;
}
static int k_cli_members(cli_ctx *cli,int argc,char **argv){
  k_client_app *app=(k_client_app *)cli->ctx;
  (void)argv;
  if(argc!=1){ cli_print(cli,"usage: MEMBERS");return 0; }
  if(k_client_queue(app,K_REQ_MEMBERS,0,0,0,0)!=0) cli_print(cli,"error: could not queue request");
  return 0;
}
static int k_cli_topology(cli_ctx *cli,int argc,char **argv){
  k_client_app *app=(k_client_app *)cli->ctx;
  (void)argv;
  if(argc!=1){ cli_print(cli,"usage: TOPOLOGY");return 0; }
  if(k_client_queue(app,K_REQ_TOPOLOGY,0,0,0,0)!=0) cli_print(cli,"error: could not queue request");
  return 0;
}
static int k_cli_rget(cli_ctx *cli,int argc,char **argv){
  k_client_app *app=(k_client_app *)cli->ctx;
  const char *begin=0,*end=0;
  int direction=K_SCAN_ASC;
  k_u32 limit=0;
  if(argc<1||argc>6){ cli_print(cli,"usage: RGET [begin] [end] [asc|desc] [limit N]");return 0; }
  /* The limit needs its keyword: treating a bare trailing number as the limit made a numeric
     end key impossible to express ("RGET a 100" was read as begin=a limit=100). */
  if(argc>2&&strcmp(argv[argc-2],"limit")==0){
    limit=(k_u32)strtoul(argv[argc-1],0,10);
    argc-=2;
  }
  if(argc>1&&(strcmp(argv[argc-1],"asc")==0||strcmp(argv[argc-1],"desc")==0)){
    direction=strcmp(argv[argc-1],"desc")==0?K_SCAN_DESC:K_SCAN_ASC;
    argc--;
  }
  if(argc>3){ cli_print(cli,"usage: RGET [begin] [end] [asc|desc] [limit N]");return 0; }
  if(argc>=2&&strcmp(argv[1],"-")!=0) begin=argv[1];
  if(argc>=3&&strcmp(argv[2],"-")!=0) end=argv[2];
  if(k_client_queue_rget(app,begin,begin?(k_u32)strlen(begin):0u,end,end?(k_u32)strlen(end):0u,direction,limit)!=0) cli_print(cli,"error: could not queue request");
  return 0;
}
static int k_cli_info(cli_ctx *cli,int argc,char **argv){
  k_client_app *app=(k_client_app *)cli->ctx;
  (void)argv;
  if(argc!=1){ cli_print(cli,"usage: INFO");return 0; }
  if(k_client_queue(app,K_REQ_INFO,0,0,0,0)!=0) cli_print(cli,"error: could not queue request");
  return 0;
}
static int k_cli_stats(cli_ctx *cli,int argc,char **argv){
  k_client_app *app=(k_client_app *)cli->ctx;
  (void)argv;
  if(argc!=1){ cli_print(cli,"usage: STATS");return 0; }
  if(k_client_queue(app,K_REQ_STATS,0,0,0,0)!=0) cli_print(cli,"error: could not queue request");
  return 0;
}
static int k_cli_shutdown_server(cli_ctx *cli,int argc,char **argv){
  k_client_app *app=(k_client_app *)cli->ctx;
  (void)argv;
  if(argc!=1){ cli_print(cli,"usage: SHUTDOWN");return 0; }
  if(k_client_queue(app,K_REQ_SHUTDOWN,0,0,0,0)!=0) cli_print(cli,"error: could not queue request");
  return 0;
}
static int k_cli_exit(cli_ctx *cli,int argc,char **argv){
  k_client_app *app=(k_client_app *)cli->ctx;
  (void)argc;
  (void)argv;
  app->stopping=1;
  cli_stop(cli);
  return 1;
}
static int k_cli_cas(cli_ctx *cli,int argc,char **argv){
  k_client_app *app=(k_client_app *)cli->ctx;
  k_buf packed;
  k_u32 new_len,old_len;
  int mode;
  if(argc!=3&&argc!=4){ cli_print(cli,"usage: CAS <key> <new> [old]");return 0; }
  mode=(argc==4)?K_CAS_CMP:K_CAS_SETNX;
  new_len=(k_u32)strlen(argv[2]);
  old_len=(argc==4)?(k_u32)strlen(argv[3]):0u;
  memset(&packed,0,sizeof(packed));
  k_buf_u8(&packed,(k_u8)mode);
  k_buf_u32(&packed,new_len);
  k_buf_bytes(&packed,argv[2],new_len);
  if(argc==4){
    k_buf_u32(&packed,old_len);
    k_buf_bytes(&packed,argv[3],old_len);
  }
  if(packed.err){ k_buf_free(&packed);cli_print(cli,"error: CAS value too long");return 0; }
  if(k_client_queue(app,K_REQ_CAS,argv[1],(k_u32)strlen(argv[1]),packed.data,packed.len)!=0) cli_print(cli,"error: could not queue request");
  k_buf_free(&packed);
  return 0;
}
static int k_cli_rdel(cli_ctx *cli,int argc,char **argv){
  k_client_app *app=(k_client_app *)cli->ctx;
  const char *begin,*end;
  if(argc!=3){ cli_print(cli,"usage: RDEL <begin> <end>");return 0; }
  begin=strcmp(argv[1],"-")==0?0:argv[1];
  end=strcmp(argv[2],"-")==0?0:argv[2];
  if(k_client_queue(app,K_REQ_RDEL,begin,begin?(k_u32)strlen(begin):0u,end,end?(k_u32)strlen(end):0u)!=0) cli_print(cli,"error: could not queue request");
  return 0;
}
static int k_cli_fcall(cli_ctx *cli,int argc,char **argv){
  k_client_app *app=(k_client_app *)cli->ctx;
  k_buf args;
  int i;
  if(argc<2){ cli_print(cli,"usage: FCALL <name> [arg...]");return 0; }
  memset(&args,0,sizeof(args));
  for(i=2;i<argc;i++){
    k_u32 len=(k_u32)strlen(argv[i]);
    k_buf_u32(&args,len);
    k_buf_bytes(&args,argv[i],len);
  }
  if(args.err){ k_buf_free(&args);cli_print(cli,"error: FCALL args too long");return 0; }
  if(k_client_queue(app,K_REQ_FCALL,argv[1],(k_u32)strlen(argv[1]),args.data,args.len)!=0) cli_print(cli,"error: could not queue request");
  k_buf_free(&args);
  return 0;
}
/* ---- app-layer cemon transport backend (production k_client_transport) ---- */
static void *k_transport_cemon_client_connect(k_client_app *app,const char *host,unsigned short port){
  return cemon_tcp_connect((cemon *)app->loop,k_numeric_host(host),port,k_client_io,app);
}
static int k_transport_cemon_client_send(k_client_app *app,void *sock,k_u32 magic,k_u8 type,const void *payload,k_u32 size,int control){
  /* Exact send stamp: the core records pending->sent_us right after this returns. */
  if(app&&k_monotonic_us(&app->now_us)!=0) app->now_us=0;
  if(!sock) return -1;
  return k_send_frame((cemon_socket *)sock,magic,type,payload,size,control);
}
static int k_transport_cemon_client_recv(k_client_app *app,void *sock){
  (void)app;
  if(!sock) return -1;   /* never hand cemon a null/stale handle: it dereferences it */
  return cemon_recv((cemon_socket *)sock);
}
static void k_transport_cemon_client_close(k_client_app *app,void *sock){
  /* Clear the client's handle BEFORE the close.  cemon frees the socket, so a later use of the
     same handle (a queued send from the loop, a second close after the peer reset) dereferences
     freed memory instead of returning an error - the same use-after-free that killed the server. */
  if(app&&app->sock==sock) app->sock=0;
  if(!sock) return;
  cemon_close((cemon_socket *)sock);
}
static const k_client_transport k_transport_cemon_client={"cemon",k_transport_cemon_client_connect,k_transport_cemon_client_send,k_transport_cemon_client_recv,k_transport_cemon_client_close};
/* ---- app-layer output sink: forward a formatted line to cli_print ---- */
static void k_client_output_cli(void *ud,const char *fmt,...){
  cli_ctx *cli=(cli_ctx *)ud;
  va_list args;
  char line[CLI_PRINT_BUFSIZE];
  va_start(args,fmt);
  vsnprintf(line,sizeof(line),fmt,args);
  va_end(args);
  line[sizeof(line)-1]='\0';
  cli_print(cli,"%s",line);
}
static int g_cli_out_count;

/* ---- PIPE: pipelined load through the REAL client (kdbctl PIPE <K> <count> <SET|GET> <prefix> [value])
   This is the user-visible way to reach the store's real throughput: a synchronous client offers at
   most 1/RTT, which is far below the batch target, so the flush window never fills.  Here K requests
   are kept in flight on ONE connection and completions are matched by id (the client already did
   that).  Verification is built in: run PIPE SET, then PIPE GET over the same key range - the GET
   must report count OK plus one NOT_FOUND for a key that was never written (the negative control). */
#define K_PIPE_SAMPLES 8192u
static k_u32 g_pipe_lat[K_PIPE_SAMPLES];
static k_u32 g_pipe_lat_n;
static k_u32 g_pipe_first_id;   /* ids below this belong to the automatic discovery/handshake */
static k_u32 g_pipe_status_ok,g_pipe_status_nf,g_pipe_status_other;
static k_u64 g_pipe_us_sum;
static void k_cli_pipe_on_done(void *ud,k_u32 id,k_u8 status,k_u64 duration_us){
  (void)ud;(void)id;
  if(id<g_pipe_first_id) return;   /* the client's own discovery request, not part of the load */
  if(g_pipe_lat_n<K_PIPE_SAMPLES) g_pipe_lat[g_pipe_lat_n++]=(k_u32)(duration_us>4294967295u?4294967295u:duration_us);
  g_pipe_us_sum+=duration_us;
  if(status==K_STATUS_OK) g_pipe_status_ok++;
  else if(status==K_STATUS_NOT_FOUND) g_pipe_status_nf++;
  else g_pipe_status_other++;
}
static int k_pipe_cmp_u32(const void *a,const void *b){
  k_u32 x=*(const k_u32 *)a,y=*(const k_u32 *)b;
  return x<y?-1:(x>y?1:0);
}
/* Why the PIPE loop stopped.  A run cut short by the 60 s cap or by the stall detector used to
   print a verdict line identical in shape to a completed one, so a truncated run read as a slow run:
   k=1 n=4000 reported ok=3509, which is "as many as fitted in 60 s", not a throughput.  Say so. */
static const char *g_pipe_end="complete";
static void k_pipe_report(const char *mode,k_u32 k,k_u32 count,k_u64 wall_us,k_u32 done){
  k_u32 pct;
  printf("pipe mode=%s k=%u n=%u wall_us=%" K_U64_FMT " ops_per_s=%" K_U64_FMT " ok=%u not_found=%u other=%u end=%s\n",
         mode,(unsigned)k,(unsigned)count,(k_u64)wall_us,
         (k_u64)(wall_us?(k_u64)done*1000000u/(k_u64)wall_us:0u),
         (unsigned)g_pipe_status_ok,(unsigned)g_pipe_status_nf,(unsigned)g_pipe_status_other,g_pipe_end);
  if(g_pipe_lat_n){
    k_u32 *sorted=(k_u32 *)K_MALLOC(g_pipe_lat_n*sizeof(k_u32));
    k_u32 i;
    if(sorted){
      memcpy(sorted,g_pipe_lat,g_pipe_lat_n*sizeof(k_u32));
      qsort(sorted,g_pipe_lat_n,sizeof(k_u32),k_pipe_cmp_u32);
      printf("pipe latency_us: min=%u",(unsigned)sorted[0]);
      for(i=0;i<5u;i++){
        pct=(i==0)?50u:(i==1?90u:(i==2?99u:(i==3?999u:1000u)));
        printf(" %s=%u",pct==50u?"p50":(pct==90u?"p90":(pct==99u?"p99":(pct==999u?"p99.9":"max"))),
               (unsigned)sorted[(k_u32)(((k_u64)pct*(k_u64)(g_pipe_lat_n-1u))/1000u)]);
      }
      printf(" avg_us=%" K_U64_FMT "\n",(k_u64)(g_pipe_us_sum/g_pipe_lat_n));
      K_FREE(sorted);
    }
  }
}
static int k_cli_pipeline_run(k_client_app *app,cemon *loop,const char *line){
  char mode[8],prefix[K_HOST_MAX],value[K_CLIENT_LINE_MAX];
  k_u32 k=0,count=0,queued=0,failed=0;
  k_u64 t0=0,now=0,wall_us=0;
  k_u64 stall_t0=0,stall_now=0;
  k_u32 stall_queued=0,stall_done=0;
  int fields;
  memset(mode,0,sizeof(mode));
  memset(prefix,0,sizeof(prefix));
  memset(value,0,sizeof(value));
  fields=sscanf(line,"PIPE %u %u %7s %64s %255s",&k,&count,mode,prefix,value);
  if(fields<4||k<1u||k>100000u||count<1u||count>1000000u){
    printf("usage: PIPE <K> <count> <SET|GET> <prefix> [value]\n");
    return -1;
  }
  if(strcmp(mode,"SET")!=0&&strcmp(mode,"GET")!=0){
    printf("error: PIPE mode must be SET or GET\n");
    return -1;
  }
  g_pipe_lat_n=0;
  g_pipe_end="complete";
  g_pipe_status_ok=g_pipe_status_nf=g_pipe_status_other=0;
  g_pipe_us_sum=0;
  g_pipe_first_id=app->next_id+1u;   /* the first request THIS run will queue */
  app->inflight_limit=(int)k;
  app->on_done=k_cli_pipe_on_done;
  app->on_done_ud=0;
  if(k_monotonic_us(&t0)!=0) t0=0;
  for(;;){
    while(queued<count&&k_client_inflight_count(app)<k&&!app->stopping){
      char key[256];
      k_u32 before=k_client_inflight_count(app);
      snprintf(key,sizeof(key),"%s%u",prefix,(unsigned)queued);
      /* k_client_queue returns 0 both when it accepted the request AND when capacity/connect
         state made it refuse it, so count the request only if the in-flight depth actually grew. */
      if(k_client_queue(app,strcmp(mode,"SET")==0?K_REQ_SET:K_REQ_GET,key,(k_u32)strlen(key),
                        strcmp(mode,"SET")==0?value:0,strcmp(mode,"SET")==0?(k_u32)strlen(value):0u)!=0){ failed=1; break; }
      if(k_client_inflight_count(app)<=before) break;   /* refused (not connected / busy): retry next round */
      queued++;
    }
    if(cemon_poll(loop,10)!=0) break;
    if(k_monotonic_us(&app->now_us)!=0) break;
    k_client_poll(app);
    if(k_monotonic_us(&now)!=0) break;
    if(t0&&now-t0>60000000u){ printf("error: PIPE exceeded 60s\n"); g_pipe_end="cap60s"; break; }
    if(queued>=count&&k_client_inflight_count(app)==0) break;
    if(app->stopping) break;
    /* Stall detector: if neither the queued count nor the completion count moved for 5s, the run
       is wedged (connection closed under us, no leader, ...).  Report it instead of waiting out
       the cap, so a sweep of depths cannot silently eat its whole time budget. */
    if(queued!=stall_queued||app->done_count!=stall_done){
      stall_queued=queued;
      stall_done=app->done_count;
      stall_t0=now;
    }else if(stall_t0){
      if(k_monotonic_us(&stall_now)!=0) break;
      if(stall_now-stall_t0>5000000u){
        printf("pipe: stalled after %" K_U64_FMT " s (queued=%u done=%u in_flight=%u)\n",
               (k_u64)((now-t0)/1000000u),(unsigned)queued,(unsigned)app->done_count,
               (unsigned)k_client_inflight_count(app));
        g_pipe_end="stalled";
        break;
      }
    }
  }
  if(t0&&k_monotonic_us(&now)==0) wall_us=now-t0;
  k_pipe_report(mode,k,count,wall_us,app->done_count);
  if(queued<count||failed) printf("warning: only %u of %u requests were queued\n",(unsigned)queued,(unsigned)count);
  app->inflight_limit=0;
  app->on_done=0;
  return app->done_count?(g_pipe_status_other?1:0):-1;
}

static char g_cli_last[1024];
static unsigned int g_cli_out_truncated;   /* non-zero means a printed body did not fit */
#if defined(_MSC_VER)
#define k_bounded_vsnprintf _vsnprintf
#else
#define k_bounded_vsnprintf vsnprintf
#endif
/* Count the CLI's output so one-shot mode knows when its single command has finished, then
   forward to the normal sink (note: the sink itself is variadic). */
static void k_client_output_counted(void *ud,const char *fmt,...){
  /* 1024 was the third silent clamp of the same kind: a STATS line over 1 KB was cut here before it
     ever reached cli_print, so the "loud truncation" added there never fired.  4096 covers what this
     tool prints, and anything longer is marked and reported. */
  char text[4096];
  va_list ap;
  int need;
  g_cli_out_count++;
  va_start(ap,fmt);
  need=k_bounded_vsnprintf(text,sizeof(text),fmt,ap);
  va_end(ap);
  if(need<0||(size_t)need>=sizeof(text)){
    size_t keep=sizeof(text)-1u,mlen=(size_t)strlen(CLI_PRINT_TRUNC);
    if(keep>mlen) memcpy(text+keep-mlen,CLI_PRINT_TRUNC,mlen);
    text[sizeof(text)-1]='\0';
    g_cli_out_truncated++;
  }
  { /* Same rule as cli_print: keep what fits and SAY so, never truncate in silence. */
    size_t n=strlen(text),cap=sizeof(g_cli_last)-1u;
    int cut=(n>cap);
    if(cut) n=cap;
    memcpy(g_cli_last,text,n);
    if(cut&&n>15u) memcpy(g_cli_last+n-15u,"...[TRUNCATED]",15u);
    g_cli_last[n]='\0'; }
  k_client_output_cli(ud,"%s",text);
}
static int k_client_run(const char *seed_list,const char *one_shot){
  static const cli_cmd commands[]={
    {"HELP","show commands",k_cli_help},
    /* read: point / multi / range */
    {"GET","GET <key>",k_cli_get},
    {"MGET","MGET <key> ...",k_cli_mget},
    {"RGET","RGET [begin] [end] [asc|desc] [limit N]",k_cli_rget},
    /* write: point / multi / range */
    {"SET","SET <key> <value>",k_cli_set},
    {"MSET","MSET <key> <value> ...",k_cli_mset},
    {"RSET","RSET <begin> <end> <value>",k_cli_rset},
    /* delete: point / multi / range */
    {"DEL","DEL <key>",k_cli_del},
    {"MDEL","MDEL <key> ...",k_cli_mdel},
    {"RDEL","RDEL <begin> <end>",k_cli_rdel},
    /* aggregate */
    {"COUNT","COUNT [begin [end]]",k_cli_count},
    {"MIN","MIN [begin [end]]",k_cli_min},
    {"MAX","MAX [begin [end]]",k_cli_max},
    /* conditional / transaction */
    {"CAS","CAS <key> <new> [old]",k_cli_cas},
    {"FCALL","FCALL <name> [arg...]",k_cli_fcall},
    /* ops / admin */
    {"INFO","show server, Raft, and treap state",k_cli_info},
    {"STATS","show server, Raft, and treap diagnostics",k_cli_stats},
    {"MEMBER","MEMBER ADD|REMOVE|RECONFIG <id,...>",k_cli_member},
    {"MEMBERS","list cluster members",k_cli_members},
    {"TOPOLOGY","show cluster member roles",k_cli_topology},
    {"SHUTDOWN","stop the connected server",k_cli_shutdown_server},
    /* client */
    {"EXIT","exit the client",k_cli_exit},
    {"QUIT","exit the client",k_cli_exit},
    {0,0,0}
  };
  k_client_app app;
  cemon *loop;
  cli_ctx cli;
  char line[K_CLIENT_LINE_MAX];
  char *argv[K_CLIENT_ARG_MAX];
  char history[K_CLIENT_HIST_MAX*K_CLIENT_LINE_MAX];
  int rc=0;
  if(!seed_list||!seed_list[0]) return -1;
  memset(&app,0,sizeof(app));
  if(k_client_seed_parse(&app,seed_list)!=0) return -1;
  loop=cemon_create();
  if(!loop) return -1;
  cemon_bind_owner(loop);      /* the CLI thread polls and destroys this loop */
  app.loop=loop;
  app.transport=&k_transport_cemon_client;
  app.output=k_client_output_counted;
  app.output_ud=&cli;
  cli.hist_buf=history;
  cli.hist_max=K_CLIENT_HIST_MAX;
  cli.hist_len=K_CLIENT_LINE_MAX;
  if(cli_init(&cli,"kdb> ",commands,&app,0,line,sizeof(line),argv,K_CLIENT_ARG_MAX)!=0){
    cemon_destroy(loop);
    return -1;
  }
  if(k_client_connect(&app)!=0) cli_print(&cli,"connection attempt failed");
  if(one_shot){
    /* Scripted use (no TTY).  TWO phases, because the handshake itself produces output
       ("connected to ...") - counting that as the command's result used to close the
       connection with the request still pending:
         1. wait for the connection handshake to report in,
         2. then dispatch the command and wait for ITS output. */
    k_u64 t0=0,now=0,t_total=0,now2=0;
    int attempt,one_shot_ok=0,was_queued=0;
    k_u32 cmd_id=0;
    g_cli_out_count=0;
    if(k_monotonic_us(&t0)!=0) t0=0;
    while(g_cli_out_count==0&&!app.stopping){
      if(cemon_poll(loop,10)!=0) break;
      k_monotonic_us(&app.now_us);
      k_client_poll(&app);
      if(t0&&k_monotonic_us(&now)==0&&now-t0>5000000u) break;
    }
    g_cli_out_count=0;
    /* PIPE is a local load command (no wire type of its own): it drives the loop itself. */
    if(strncmp(one_shot,"PIPE",4)==0&&(one_shot[4]=='\0'||one_shot[4]==' ')){
      rc=k_cli_pipeline_run(&app,loop,one_shot);
      app.stopping=1;
    }else{
    t0=0;
    if(k_monotonic_us(&t0)!=0) t0=0;
    /* The client may still be busy with its own leader-discovery request, in which case the
       enqueue is refused ("wait for the current request"): retry until the command is
       actually accepted, then wait for its output. */
    if(k_monotonic_us(&t_total)!=0) t_total=0;
    for(attempt=0;attempt<20&&!app.stopping;attempt++){
      if(t_total&&k_monotonic_us(&now2)==0&&now2-t_total>10000000u){ break; }   /* 10s total */
      g_cli_last[0]='\0';
      g_cli_out_count=0;
      t0=0;
      if(k_monotonic_us(&t0)!=0) t0=0;
      /* Dispatch only when the client is idle, and judge completion STRUCTURALLY (output
         arrived and no request is in flight) instead of pattern-matching the notice text.
         Both mattered: matching "any output" made the loop exit before its first poll, and
         matching the notice string made correctness depend on user-visible wording. */
      if(k_client_busy(&app)){
        while(k_client_busy(&app)&&!app.stopping){
          if(cemon_poll(loop,10)!=0) break;
          k_monotonic_us(&app.now_us);
          k_client_poll(&app);
          if(t0&&k_monotonic_us(&now)==0&&now-t0>3000000u) break;
        }
        continue;
      }
      cli_exec_line(&cli,one_shot);
      /* A command only counts as dispatched when a request is actually in flight: a refused
         enqueue (not connected, client busy, no leader) prints a notice and leaves pending
         empty - and that notice is NOT a successful run. */
      was_queued=k_client_busy(&app);
      cmd_id=was_queued?app.pending->id:0u;
      while(!app.stopping){
        if(was_queued&&g_cli_out_count>0&&!k_client_busy(&app)) break;
        if(cemon_poll(loop,10)!=0) break;
        k_monotonic_us(&app.now_us);
        k_client_poll(&app);
        if(t0&&k_monotonic_us(&now)==0&&now-t0>3000000u) break;
      }
      /* Structural success test: the command's own request id received an OK response.
         "Some output appeared" was not enough - a connect failure prints a notice too. */
      /* NOT_FOUND is a legitimate answer (the key is simply absent); only a hard ERROR - or
         a request that never got a response at all - means the command did not execute. */
      if(cmd_id&&app.last_done_id==cmd_id&&!k_client_busy(&app)&&app.last_done_status!=K_STATUS_ERROR) one_shot_ok=1;
      break;
    }
    if(!one_shot_ok){
      /* One-shot mode must not report success when the command never produced output: a
         timeout, a refused command or an unreachable server all left rc==0 before, so scripts
         and CI read silence as success. */
      printf("error: no response from the server; the command was not executed\n");
      rc=-1;
    }
    app.stopping=1;
    }
  }
  while(!app.stopping){
    if(cli_poll(&cli)<0){
      app.stopping=1;
      break;
    }
    if(cemon_poll(loop,10)!=0){
      rc=-1;
      break;
    }
    k_monotonic_us(&app.now_us);
    k_client_poll(&app);
  }
  cli_shutdown(&cli);
  cemon_stop(loop);
  while(cemon_poll(loop,0)==0){}
  cemon_destroy(loop);
  k_pending_free(app.pending);
  k_rx_free(&app.rx);
  return rc;
}


int main(int argc,char **argv){
  char cmdline[1024];
  size_t used=0;
  int i,rc;
  if(argc<2){ printf("usage: kdbctl <host:port[,host:port...]> [command...]\n"); return EXIT_FAILURE; }
  /* kdbctl <host> GET <key>   /   kdbctl <host> SET <key> <value>   /   kdbctl <host> STATS */
  cmdline[0]='\0';
  for(i=2;i<argc;i++){
    size_t n=strlen(argv[i]);
    if(used+n+2u>=sizeof(cmdline)) break;
    if(used){ cmdline[used++]=' '; }
    memcpy(cmdline+used,argv[i],n);
    used+=n;
    cmdline[used]='\0';
  }
  rc=k_client_run(argv[1],argc>2?cmdline:0);
  return rc==0?EXIT_SUCCESS:EXIT_FAILURE;
}
