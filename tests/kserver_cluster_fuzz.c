/* kserver_cluster_fuzz.c -- deterministic multi-node harness for the kynovo
 * replicated engine core (kserver.h).  Raises raft_cluster_fuzz.c to the server
 * layer: N k_server nodes, each driven by k_server_advance + runtime_drain
 * (deterministic "sync" runtime backend), wired through a virtual-network
 * transport that captures outbound frames and delivers them (re-framed with the
 * header the receiver's k_rx_feed expects) to the target peer_received -- the
 * FDB Flow deterministic simulation.
 *
 * Content-level safety oracle (the hard principle): every node must apply the
 * SAME byte image for the same log index.  Concretely, after a SET commits the
 * leader applies it and every follower applies an identical copy, so all nodes
 * read the same value back -- no per-node recompute, no divergence.
 *
 * Deterministic (splitmix64), no sockets, no wall clock, no cemon/cli.
 *
 * Build: gcc -std=c89 -O2 -Wall -Wextra -Wno-unused-function \
 *           tests/kserver_cluster_fuzz.c -o build/kserver_cluster_fuzz.exe
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>

/* ---- OOM fault injection: a single transient allocation fails every
   g_oom_interval allocations while g_oom_interval>0 (chaos only), so most
   allocations succeed and the system must retry past the occasional failure --
   the realistic "transient memory pressure" model, not a whole-step freeze.
   Three headers each expose injectable allocator macros (K_/RAFT_/TREAP_), and
   the macros MUST be defined before the first include of each header, else the
   header's own default (bare malloc) wins and the `#ifndef` guard swallows the
   override.  One counter thus drives every allocation site in server/raft/treap.
   The harness's own vnet plumbing uses bare malloc and is NEVER faulted. ---- */
static int g_oom_interval=0;  /* >0: fail every Nth allocation (chaos only) */
static int g_oom_count=0;
static int km_oom_hit(void){
  if(!g_oom_interval) return 0;
  return (++g_oom_count % g_oom_interval)==0;
}
static void *km_malloc(size_t n){ return km_oom_hit()?0:malloc(n); }
static void *km_calloc(size_t n,size_t s){ return km_oom_hit()?0:calloc(n,s); }
static void *km_realloc(void *p,size_t n){ return km_oom_hit()?0:realloc(p,n); }
#define K_MALLOC km_malloc
#define K_CALLOC km_calloc
#define K_REALLOC km_realloc
#define RAFT_MALLOC km_malloc
#define RAFT_CALLOC km_calloc
#define RAFT_REALLOC km_realloc
#define TREAP_MALLOC km_malloc
#define TREAP_CALLOC km_calloc
#define TREAP_REALLOC km_realloc

#define VFS_STATIC
#define VFS_IMPLEMENTATION
#include "../code/vfs.h"
#define TREAP_STATIC
#define TREAP_IMPLEMENTATION
#include "../code/treap.h"
#define RUNTIME_STATIC
#define RUNTIME_IMPLEMENTATION
#include "../code/runtime.h"
#define RAFT_STATIC
#define RAFT_IMPLEMENTATION
#include "../code/raft.h"
#include "../code/kbase.h"
#include "../code/kproto.h"
#include "../code/kserver.h"
#include "lincheck.h"

/* This block needs code/kbase.h, which the include block above has now brought in.  It used to sit
   before the includes and spell C99 types directly; the allocator macros below it must stay above
   the project headers, so the block moved down instead of the include moving up. */
/* Seed of the cluster currently running: printed BEFORE each run (below), and
   repeated by the crash handler, so a segfault or a hang is reproducible from
   the last line of output instead of being unattributable. */
static k_u64 g_current_seed;
static void kscf_crash_dump(int sig){
  fprintf(stderr,"\n=== CRASH signal %d during seed %" K_U64_FMT " ===\n",sig,g_current_seed);
  fflush(stderr);
  signal(sig,SIG_DFL);
  raise(sig);
}

#define NNODE 3
#define MAXSTEP 4000

/* ---- deterministic PRNG (splitmix64) ---- */
static k_u64 rng_state;
static k_u64 rng_u64(void){
  k_u64 z = (rng_state += K_U64_C(0x9E3779B97F4A7C15));
  z = (z ^ (z >> 30)) * K_U64_C(0xBF58476D1CE4E5B9);
  z = (z ^ (z >> 27)) * K_U64_C(0x94D049BB133111EB);
  return z ^ (z >> 31);
}

/* ---- fault injection: per-run random drop/dup percentages ---- */
static int g_drop_pct;   /* % of peer frames dropped */
static int g_dup_pct;    /* % of peer frames duplicated */
static int g_chaos;      /* 1 = chaos phase (faults on), 0 = healed liveness tail */
static int g_partition_on;      /* current partition active */
static int g_part[NNODE];       /* per-node partition group (0/1) */
static int g_part_churn_pct;    /* % chance per step to re-partition */
static int g_oom_clusters;      /* runs where >=1 node OOM-stopped (proves injection active) */
#define CHAOS_STEP 1500
#define HEAL_STEP  1500

/* ---- content-level oracle: hash the whole state machine (ordered) ---- */
typedef struct{ k_u64 h; } hashctx;
static int hash_visit(const unsigned char *key,unsigned int key_len,
                      const unsigned char *value,unsigned int value_len,void *ud){
  hashctx *c=(hashctx *)ud;
  unsigned int i;
  for(i=0;i<key_len;i++){ c->h ^= (k_u64)key[i]; c->h *= K_U64_C(1099511628211); }
  c->h ^= K_U64_C(0xFFFFFFFF00000000) ^ (k_u64)key_len;  /* key length boundary */
  for(i=0;i<value_len;i++){ c->h ^= (k_u64)value[i]; c->h *= K_U64_C(1099511628211); }
  c->h ^= K_U64_C(0x00000000FFFFFFFF) ^ (k_u64)value_len;
  return 0;
}
static k_u64 state_hash(const treap *t){
  hashctx c; c.h = K_U64_C(1469598103934665603);
  if(t) treap_scan(t,0,0,0,0,TREAP_ASC,hash_visit,&c);
  return c.h;
}

/* ---- virtual network: queues of dials and in-flight frames ---- */
typedef struct vmsg{
  struct vmsg *next;
  int from, to;
  k_u32 magic;
  k_u8 type;
  k_u8 *payload;      /* frame body (no header); delivered with a rebuilt header */
  k_u32 size;
} vmsg;
typedef struct vdial{
  struct vdial *next;
  int from, to;
} vdial;
static vmsg  *vmsg_q;
static vdial *vdial_q;

/* ---- client-response capture (enabled only during the linearizability phase,
   so the existing peer-frame path stays byte-identical otherwise) ---- */
typedef struct cresp{
  struct cresp *next;
  k_u32 request_id;
  k_u8 status;
  k_u8 *body;
  k_u32 body_size;
} cresp;
static cresp *cresp_q;
static int g_capture_resp;
static void cresp_clear(void){
  cresp *c,*n;
  for(c=cresp_q;c;c=n){ n=c->next; free(c->body); free(c); }
  cresp_q=0;
}

#define SOCK_ENC(f,t) ((void*)(size_t)((f)*100+(t)))
#define SOCK_FROM(s)  ((int)(((size_t)(s))/100))
#define SOCK_TO(s)    ((int)(((size_t)(s))%100))

static void *vnet_dial(k_server *server,const k_node_spec *node){
  vdial *d=(vdial *)malloc(sizeof(*d));
  if(!d) return 0;
  d->from=server->id;
  d->to=node->id;
  d->next=vdial_q; vdial_q=d;
  return SOCK_ENC(server->id,node->id);
}
static int vnet_send(void *sock,k_u32 magic,k_u8 type,const void *payload,k_u32 size,int control){
  vmsg *m;
  (void)control;
  if(g_capture_resp && magic==K_CLIENT_MAGIC && type==K_RESPONSE){
    /* capture the client response for the linearizability oracle */
    k_response_data r;
    cresp *c;
    if(k_response_decode(&r,(const k_u8*)payload,size)==0){
      c=(cresp *)malloc(sizeof(*c));
      if(c){
        c->request_id=r.request_id;
        c->status=r.status;
        c->body=r.body;          /* transfer ownership of the decode's copy */
        c->body_size=r.body_size;
        c->next=cresp_q; cresp_q=c;
      }else{
        k_response_data_free(&r);
      }
    }
    return 0;   /* client response: not a peer frame */
  }
  m=(vmsg *)malloc(sizeof(*m));
  if(!m) return -1;
  m->from=SOCK_FROM(sock);
  m->to=SOCK_TO(sock);
  m->magic=magic;
  m->type=type;
  m->size=size;
  m->payload=0;
  if(size){
    m->payload=(k_u8 *)malloc(size);
    if(!m->payload){ free(m); return -1; }
    memcpy(m->payload,payload,size);
  }
  m->next=vmsg_q; vmsg_q=m;
  return 0;
}
static void vnet_close(void *sock){ (void)sock; }
static int  vnet_recv(void *sock){ (void)sock; return 0; }
static void vnet_setud(void *sock,void *ud){ (void)sock;(void)ud; }
static const k_server_transport vnet_transport={"vnet",vnet_send,vnet_close,vnet_recv,vnet_dial,vnet_setud};

/* ---- node registry ---- */
static k_server nodes[NNODE];
static int nnode;

static k_server *node_by_id(int id){
  int i;
  for(i=0;i<nnode;i++) if(nodes[i].id==id) return &nodes[i];
  return 0;
}
/* find a node's connection by its local socket handle (covers inbound conns
   still unclaimed until their HELLO arrives) */
static k_conn *find_conn_by_sock(k_server *s,void *sock){
  k_conn *c;
  for(c=s->connections;c;c=c->next) if(c->sock==sock) return c;
  return 0;
}

/* deliver one peer frame to its target (re-framed so the receiver's k_rx_feed
   parses it), subject to fault injection (drop / duplicate) */
static void deliver_peer_frame(int to,int from,k_u32 magic,k_u8 type,const k_u8 *payload,k_u32 size){
  k_server *dst=node_by_id(to);
  k_conn *c=dst?find_conn_by_sock(dst,SOCK_ENC(to,from)):0;
  k_u8 *frame;
  if(!c) return;
  frame=(k_u8 *)malloc((size_t)K_FRAME_HEADER+size);
  if(!frame) return;
  k_frame_header_build(frame,magic,type,size);
  if(size) memcpy(frame+K_FRAME_HEADER,payload,size);
  k_server_peer_received(c,frame,K_FRAME_HEADER+size);
  free(frame);
}
static int all_hash_same(void){
  k_u64 h0=state_hash(nodes[0].tree);
  int i;
  for(i=1;i<nnode;i++) if(state_hash(nodes[i].tree)!=h0) return 0;
  return 1;
}
static void free_queues(void){
  vmsg *m,*n;
  vdial *d,*dn;
  for(m=vmsg_q;m;m=n){ n=m->next; free(m->payload); free(m); }
  vmsg_q=0;
  for(d=vdial_q;d;d=dn){ dn=d->next; free(d); }
  vdial_q=0;
}

static int init_cluster(int n){
  k_cluster c;
  int i;
  nnode=n;
  memset(&c,0,sizeof(c));
  c.count=n;
  for(i=0;i<n;i++){
    c.nodes[i].id=i+1;
    strcpy(c.nodes[i].host,"127.0.0.1");
    c.nodes[i].client_port=7000+(unsigned short)i;
    c.nodes[i].peer_port=7100+(unsigned short)i;
  }
  for(i=0;i<n;i++){
    char base[64];
    k_server *s=&nodes[i];
    memset(s,0,sizeof(*s));
    sprintf(base,"mem://kcluster-%u-%d",(unsigned)rng_u64(),i+1);
    k_server_init(s,i+1,c.nodes[i].client_port,c.nodes[i].peer_port,base,&c);
    s->runtime_backend="sync";
    s->transport=&vnet_transport;
    s->admission=1;
    if(k_server_open(s)!=0) return -1;
  }
  return 0;
}
static void release_cluster(void){
  int i;
  for(i=0;i<nnode;i++) k_server_release(&nodes[i]);
  nnode=0;
  free_queues();
}
/* crash + restart one node: release frees the in-memory raft/treap/runtime but
   leaves the mem: disk (WAL/snapshot/cfg) intact.  A TRUE reboot must also
   zero the run-state flags (stopped/fatal/...): release clears memory but not
   them, so a node that OOM-stopped would reopen still marked stopped and never
   re-enter service.  Save the init fields, release, memset the whole struct,
   then init + open exactly as a fresh process would. */
static int crash_restart_node(int idx){
  k_server *s=&nodes[idx];
  int victim_id=s->id;
  unsigned short cp=s->client_port,pp=s->peer_port;
  k_cluster saved_cluster=s->cluster;
  char saved_base[K_URI_MAX];
  int j;
  strcpy(saved_base,s->base);
  /* Model the peers noticing the crash: each peer's half-open link to the
     crashed node is torn down via k_conn_closed (which clears that node's
     peer_socks/peer_conns), so it re-dials on the next reconnect -- otherwise
     the leader still "sees" a live link to a restarted node whose own side is
     gone, and the restart can never catch up. */
  for(j=0;j<nnode;j++){
    k_server *other;
    int vi;
    if(j==idx) continue;
    other=&nodes[j];
    vi=k_cluster_index(&other->cluster,victim_id);
    if(vi>=0&&other->peer_conns[vi]) k_conn_closed(other->peer_conns[vi]);
  }
  k_server_release(s);
  memset(s,0,sizeof(*s));
  k_server_init(s,victim_id,cp,pp,saved_base,&saved_cluster);
  s->transport=&vnet_transport;
  s->runtime_backend="sync";
  s->admission=1;
  return k_server_open(s);
}

/* one deterministic step: advance all nodes, establish + deliver */
static void step(unsigned int ms){
  int i;
  /* chaos only: occasionally tear a new random partition (some leader terms
     later the partition heals on its own when re-partition lands on one group) */
  if(g_chaos && g_part_churn_pct>0 && (int)(rng_u64()%100u)<g_part_churn_pct){
    g_partition_on=1;
    for(i=0;i<nnode;i++) g_part[i]=(int)(rng_u64()%2u);
  }else if(g_chaos && (int)(rng_u64()%100u)<g_part_churn_pct/2){
    g_partition_on=0;   /* heal the partition */
  }
  for(i=0;i<nnode;i++){
    k_server_advance(&nodes[i],ms);
    if(nodes[i].wal_rt) runtime_drain(nodes[i].wal_rt);
    if(nodes[i].snapshot_rt) runtime_drain(nodes[i].snapshot_rt);
  }
  { /* complete dials: dialer peer_dialed (HELLO out), target peer_accepted */
    vdial *d;
    for(d=vdial_q;d;d=d->next){
      k_server *src=node_by_id(d->from);
      k_server *dst=node_by_id(d->to);
      k_conn *c;
      if(!src||!dst) continue;
      c=find_conn_by_sock(src,SOCK_ENC(d->from,d->to));
      if(c) k_server_peer_dialed(c);
      k_server_peer_accepted(dst,SOCK_ENC(d->to,d->from));
    }
    { vdial *dd,*dn; for(dd=vdial_q;dd;dd=dn){ dn=dd->next; free(dd); } vdial_q=0; }
  }
  { /* deliver peer frames: HELLO first (claims the peer), then RAFT */
    int pass;
    for(pass=0;pass<2;pass++){
      vmsg **pp=&vmsg_q;
      while(*pp){
        vmsg *m=*pp;
        if(m->magic==K_PEER_MAGIC && m->type==(pass==0?K_PEER_HELLO:K_PEER_RAFT)){
          /* Fault injection applies to the Raft data plane only (AppendEntries /
             votes are idempotent under dup, retried under drop); the HELLO
             handshake must stay reliable -- a duplicate HELLO is treated as a
             protocol error by the peer (it closes the link), and dropping it
             strands an unclaimed half-open connection. */
          int times=1;
          if(pass==1&&g_chaos){
            int cross=(g_partition_on)&&(g_part[m->from-1]!=g_part[m->to-1]);
            unsigned int rr=(unsigned int)(rng_u64()%100u);
            times=cross?0:(rr<(unsigned int)g_drop_pct)?0:((rr<(unsigned int)(g_drop_pct+g_dup_pct))?2:1);
          }
          { int k; for(k=0;k<times;k++) deliver_peer_frame(m->to,m->from,m->magic,m->type,m->payload,m->size); }
          *pp=m->next;
          free(m->payload); free(m);
        }else pp=&m->next;
      }
    }
    { vmsg *m; while(vmsg_q){ m=vmsg_q; vmsg_q=m->next; free(m->payload); free(m); } }
  }
}

static int elect_any(void){
  int i,iter;
  for(iter=0;iter<MAXSTEP;iter++){
    step(50u);
    for(i=0;i<nnode;i++) if(nodes[i].is_leader) return i;
  }
  return -1;
}

static k_u32 make_set_frame(k_u8 *out,k_u32 cap,k_u32 rid,const char *key,const char *value){
  k_buf b; k_u32 total;
  memset(&b,0,sizeof(b));
  if(k_request_payload(&b,rid,K_REQ_SET,key,(k_u32)strlen(key),value,(k_u32)strlen(value))!=0){ k_buf_free(&b); return 0; }
  total=K_FRAME_HEADER+b.len;
  if(total>cap){ k_buf_free(&b); return 0; }
  k_frame_header_build(out,K_CLIENT_MAGIC,K_REQ_SET,b.len);
  if(b.len) memcpy(out+K_FRAME_HEADER,b.data,b.len);
  k_buf_free(&b);
  return total;
}

/* generic client-request frame builder: any K_REQ_* type with already-packed
   key/value (MSET carries the batch list as "key"; CAS carries
   [mode][new][old] as "value"; FCALL carries command-name as "key" + args as
   "value"). */
static k_u32 make_req_frame(k_u8 *out,k_u32 cap,k_u32 rid,k_u8 type,const void *key,k_u32 key_len,const void *value,k_u32 value_len){
  k_buf b; k_u32 total;
  memset(&b,0,sizeof(b));
  if(k_request_payload(&b,rid,type,key,key_len,value,value_len)!=0){ k_buf_free(&b); return 0; }
  total=K_FRAME_HEADER+b.len;
  if(total>cap){ k_buf_free(&b); return 0; }
  k_frame_header_build(out,K_CLIENT_MAGIC,type,b.len);
  if(b.len) memcpy(out+K_FRAME_HEADER,b.data,b.len);
  k_buf_free(&b);
  return total;
}

/* content-level oracle: every node must read the SAME value for a committed key */
static int all_read_same(const char *key,const char *expected){
  int i;
  for(i=0;i<nnode;i++){
    const unsigned char *v=0; unsigned int vl=0;
    if(treap_get(nodes[i].tree,(const unsigned char*)key,(unsigned int)strlen(key),&v,&vl)!=1) return 0;
    if(vl!=strlen(expected)||memcmp(v,expected,vl)!=0) return 0;
  }
  return 1;
}

static const char *final_check(int *leaders_out){
  int i,leaders=0;
  for(i=0;i<nnode;i++) if(nodes[i].is_leader) leaders++;
  *leaders_out=leaders;
  if(leaders!=1) return "leader count";
  if(!all_hash_same()) return "state hash divergence";
  if(!all_read_same("k","v42")) return "key k missing/divergent";
  if(!all_read_same("k2","v84")) return "key k2 missing/divergent";
  return 0;
}

/* ---- concurrent-client register linearizability oracle ----
   NCLIENTS logical clients interleave SET/GET/DEL against NCKEYS keys while
   drop/dup faults churn the leader.  Each completed op records (is_write, val,
   inv, resp) with inv/resp as logical step counters; the phase then checks each
   key's history is linearizable (lin_linearizable).  This catches a deposed
   leader serving a stale read -- invisible to the convergence oracle.
   Deterministic: every scheduling decision goes through rng_u64(); inv/resp are
   step counters, never wall clock.  Returns 0 on pass, -1 on violation. */
#define NCLIENTS 4
#define NCKEYS 4
#define PHASE_STEPS 1500
typedef struct lcli{ int active; int retry_pending; k_u32 rid; int key; int is_read; int write_val; long inv; int retries; } lcli;

/* What the linearizability checker actually DECIDED.  The verdict line used to report only
   "clusters consistent", so a run in which the checker recorded or decided nothing printed exactly
   the same success line as a run that checked dozens of histories - "0 failures" indistinguishable
   from "0 data" (issue #16).  These counters make the check self-certifying, and a run that decided
   no history at all is a failure, not a pass. */
static int g_lin_histories;
static int g_lin_ops;
static int g_lin_inconclusive;
static int lincheck_phase(void){
  lin_op hist[NCKEYS][LIN_MAX];
  int hcount[NCKEYS];
  int fcount[NCKEYS];   /* fired (not recorded): cap firing so no completed op is ever dropped */
  lcli cl[NCLIENTS];
  k_u32 next_rid;
  long t;
  int i,c,steps,leader,val;
  k_u8 fbuf[K_FRAME_HEADER+64];
  k_u32 ftotal;
  char kbuf[3],vstr[2];
  cresp *cp;
  memset(hcount,0,sizeof(hcount));
  memset(fcount,0,sizeof(fcount));
  memset(cl,0,sizeof(cl));
  next_rid=10000u;
  /* establish each key at value 0 (the register init) */
  for(i=0;i<NCKEYS;i++){
    kbuf[0]='x'; kbuf[1]=(char)('0'+i); kbuf[2]=0;
    vstr[0]='0'; vstr[1]=0;
    leader=elect_any();
    if(leader<0) return -1;
    ftotal=make_req_frame(fbuf,sizeof(fbuf),next_rid++,K_REQ_SET,kbuf,2,vstr,1);
    if(!ftotal||!nodes[leader].connections) return -1;
    k_server_client_received(nodes[leader].connections,fbuf,ftotal);
    for(steps=0;steps<MAXSTEP;steps++){ step(50u); if(all_read_same(kbuf,"0")) break; }
    if(!all_read_same(kbuf,"0")) return -1;
  }
  /* concurrent phase: drop/dup + partition churn (leader changes exercise the
     read path), no OOM */
  g_chaos=1; g_oom_interval=0; g_part_churn_pct=12; g_partition_on=0;
  g_capture_resp=1;
  t=0;
  for(steps=0;steps<PHASE_STEPS;steps++){
    for(c=0;c<NCLIENTS;c++){
      if(cl[c].active) continue;
      leader=-1;
      for(i=0;i<nnode;i++) if(nodes[i].is_leader){ leader=i; break; }
      if(leader<0) continue;                    /* no leader this tick: retry */
      if(cl[c].retry_pending){
        /* re-fire the redirected write at the new leader, keeping original inv */
        kbuf[0]='x'; kbuf[1]=(char)('0'+cl[c].key); kbuf[2]=0;
        if(cl[c].write_val==LIN_ABSENT) ftotal=make_req_frame(fbuf,sizeof(fbuf),next_rid,K_REQ_DEL,kbuf,2,0,0);
        else{ vstr[0]=(char)('0'+cl[c].write_val); vstr[1]=0; ftotal=make_req_frame(fbuf,sizeof(fbuf),next_rid,K_REQ_SET,kbuf,2,vstr,1); }
        cl[c].rid=next_rid++;
        cl[c].retries++;
        cl[c].retry_pending=0;
        if(!ftotal||!nodes[leader].connections){ cl[c].active=0; continue; }
        k_server_client_received(nodes[leader].connections,fbuf,ftotal);
        cl[c].active=1;                          /* inv unchanged */
        continue;
      }
      if((int)(rng_u64()%100u)>=60) continue;   /* ~60% fire rate */
      cl[c].key=(int)(rng_u64()%(k_u64)NCKEYS);
      if(fcount[cl[c].key]>=LIN_MAX) continue;  /* key full: pick another next tick */
      cl[c].is_read=((int)(rng_u64()%3u)==0);   /* 1/3 reads */
      kbuf[0]='x'; kbuf[1]=(char)('0'+cl[c].key); kbuf[2]=0;
      if(cl[c].is_read){
        cl[c].write_val=LIN_ABSENT;
        ftotal=make_req_frame(fbuf,sizeof(fbuf),next_rid,K_REQ_GET,kbuf,2,0,0);
      }else if((int)(rng_u64()%2u)==0){
        cl[c].write_val=LIN_ABSENT;             /* DEL */
        ftotal=make_req_frame(fbuf,sizeof(fbuf),next_rid,K_REQ_DEL,kbuf,2,0,0);
      }else{
        cl[c].write_val=(int)(rng_u64()%3u);    /* SET 0/1/2 */
        vstr[0]=(char)('0'+cl[c].write_val); vstr[1]=0;
        ftotal=make_req_frame(fbuf,sizeof(fbuf),next_rid,K_REQ_SET,kbuf,2,vstr,1);
      }
      cl[c].rid=next_rid++;
      cl[c].inv=t;
      cl[c].retries=0;
      cl[c].retry_pending=0;
      if(!ftotal||!nodes[leader].connections){ cl[c].active=0; continue; }
      k_server_client_received(nodes[leader].connections,fbuf,ftotal);
      cl[c].active=1;
      fcount[cl[c].key]++;
    }
    step(50u);
    t++;
    for(cp=cresp_q;cp;cp=cp->next){
      for(c=0;c<NCLIENTS;c++) if(cl[c].active&&cl[c].rid==cp->request_id) break;
      if(c>=NCLIENTS) continue;
      if(cp->status==K_STATUS_REDIRECT){
        if(cl[c].is_read){
          cl[c].active=0;                       /* read: drop, re-fire fresh next tick */
        }else if(cl[c].retries<50){
          /* write: the deposed leader stepped down before committing, but the
             entry may still be committed later by the new leader's term-committed
             NOOP (Raft: committing the NOOP covers every lower entry).  Dropping
             the redirected write would then leave a later read observing an
             unrecorded write -> false stale read.  Mark for retry at the next
             leader, keeping the original inv (register value idempotent). */
          cl[c].active=0;
          cl[c].retry_pending=1;
        }else{
          cl[c].active=0;                       /* retry budget exhausted: drop */
          cl[c].retry_pending=0;
        }
      }else if(cl[c].is_read){
        val=(cp->status==K_STATUS_OK&&cp->body_size==1u)?(int)(cp->body[0]-'0'):LIN_ABSENT;
        if(hcount[cl[c].key]<LIN_MAX){
          hist[cl[c].key][hcount[cl[c].key]].is_write=0;
          hist[cl[c].key][hcount[cl[c].key]].val=val;
          hist[cl[c].key][hcount[cl[c].key]].inv=cl[c].inv;
          hist[cl[c].key][hcount[cl[c].key]].resp=t;
          hcount[cl[c].key]++;
        }
        cl[c].active=0;
      }else if(cp->status==K_STATUS_OK){
        if(hcount[cl[c].key]<LIN_MAX){
          hist[cl[c].key][hcount[cl[c].key]].is_write=1;
          hist[cl[c].key][hcount[cl[c].key]].val=cl[c].write_val;
          hist[cl[c].key][hcount[cl[c].key]].inv=cl[c].inv;
          /* A redirected (retried) write is at-least-once: its original entry
             (appended to a deposed leader) may be covered/committed by a later
             leader's term-committed NOOP at any time up to phase end, so its
             final effect time is unbounded.  Record resp at phase end rather
             than the retry commit time so the single recorded op can linearize
             at the cover point instead of fabricating a second (unrecorded)
             write.  A non-retried write commits exactly once, resp is exact. */
          hist[cl[c].key][hcount[cl[c].key]].resp = cl[c].retries>0 ? (PHASE_STEPS*2+2) : t;
          hcount[cl[c].key]++;
        }
        cl[c].active=0;
      }else{
        cl[c].active=0;                         /* write errored: drop */
      }
    }
    cresp_clear();
    t++;   /* responses of this iteration precede the next iteration's fires */
  }
  g_capture_resp=0;
  g_chaos=0;
  for(i=0;i<NCKEYS;i++){
    int lr=lin_linearizable(hist[i],hcount[i],0);
    /* A history longer than the op mask is NOT checked -- never report it as a
       violation (LIN_INCONCLUSIVE == -1); the per-key cap below keeps this
       unreachable here, but the checker must not turn "unchecked" into "failed". */
    if(lr==LIN_INCONCLUSIVE){ g_lin_inconclusive++; continue; }
    g_lin_histories++;
    g_lin_ops+=hcount[i];
    if(!lr){
      fprintf(stderr,"FAIL linearizability key x%d (ops=%d)\n",i,hcount[i]);
      lin_dump(hist[i],hcount[i]);
      return -1;
    }
  }
  return 0;
}

static int run_one_cluster(k_u64 seed){
  k_u8 frame[K_FRAME_HEADER+256];
  k_u32 total;
  int leader,steps,i,leaders;
  const char *why;
  rng_state=seed;
  g_drop_pct=(int)(rng_u64()%25u);
  g_dup_pct=(int)(rng_u64()%25u);
  g_part_churn_pct=(int)(rng_u64()%9u);
  g_oom_interval=0;
  g_oom_count=0;
  g_partition_on=0;
  g_chaos=0;
  if(init_cluster(NNODE)!=0){ release_cluster(); return 0; }

  /* --- fault-free baseline: elect, submit k, converge --- */
  leader=elect_any();
  if(leader<0){ fprintf(stderr,"FAIL no leader (seed %" K_U64_FMT ")\n",(k_u64)seed); release_cluster(); return 0; }
  {
    k_server *L=&nodes[leader];
    k_server_client_accepted(L,(void*)(size_t)9000);
    total=make_set_frame(frame,sizeof(frame),1u,"k","v42");
    if(!total||!L->connections){ release_cluster(); return 0; }
    k_server_client_received(L->connections,frame,total);
  }
  for(steps=0;steps<MAXSTEP;steps++){ step(50u); if(all_read_same("k","v42")) break; }
  if(!all_read_same("k","v42")){
    fprintf(stderr,"FAIL baseline no agreement (seed %" K_U64_FMT ")\n",(k_u64)seed);
    release_cluster(); return 0;
  }

  /* --- chaos: drop + dup + random partitions + transient OOM -- committed k
     must survive.  OOM is per-allocation (every g_oom_interval-th alloc fails),
     so most allocations succeed and a failed one is retried next time. --- */
  g_chaos=1;
  g_oom_interval=(int)(30u+rng_u64()%270u);  /* 30..300 allocs between failures */
  g_oom_count=0;
  for(steps=0;steps<CHAOS_STEP;steps++) step(50u);

  /* --- healed liveness tail: faults off, re-elect, commit k2, converge --- */
  g_chaos=0;
  g_oom_interval=0;   /* stop faulting allocations */
  g_partition_on=0;
  { /* let the raft re-settle after partition healing before trusting is_leader */
    int settle;
    for(settle=0;settle<600;settle++) step(50u);
  }
  { /* crash + restart EVERY node that stopped (a transient allocation failure
       is fatal to raft: raft_stop via the drain path, so the node stops rather
       than risk a divergent apply), else one healthy follower for the plain
       recovery path.  A stopped node must reboot from its WAL/snapshot -- the
       OOM counterpart of crash/restart. */
    int vi,restarted=0;
    for(vi=0;vi<nnode;vi++){
      /* raft_stopped counts: a transient allocation failure stops raft through the drain path and the
         application records that in its own flag (kserver.h:4440).  A heal loop that only looks at
         stopped/fatal cannot see such a node, leaves it dead at its old commit index for the rest of the
         run, and then fails the snapshot check - which is exactly what seed 99 did. */
      /* stopping is the application's own shutdown flag and the one that matters here: k_server_begin_stop
         clears admission, and k_server_reconnect returns immediately without admission, so a node that
         stopped for ANY reason never dials again.  stop/fatal/raft_stopped alone miss that - raft_stopped
         only follows once a later ready bundle carries phase_stopped, which a stopped node never produces.
         Seed 99 left node 2 exactly there: alive, full membership, admission 0, stranded at commit 2. */
      if(!nodes[vi].stopped&&!nodes[vi].fatal&&!nodes[vi].raft_stopped&&!nodes[vi].stopping) continue;
      if(crash_restart_node(vi)!=0){
        fprintf(stderr,"FAIL restart stopped node %d (seed %" K_U64_FMT ")\n",vi+1,(k_u64)seed);
        release_cluster(); return 0;
      }
      restarted=1;
    }
    if(!restarted){
      int victim,f=0;
      for(vi=0;vi<nnode;vi++) if(!nodes[vi].is_leader){ victim=vi; f=1; break; }
      if(!f) victim=0;
      if(crash_restart_node(victim)!=0){
        fprintf(stderr,"FAIL restart node %d (seed %" K_U64_FMT ")\n",victim+1,(k_u64)seed);
        release_cluster(); return 0;
      }
    }else{
      g_oom_clusters++;   /* OOM actually stopped >=1 node this run */
    }
    for(steps=0;steps<600;steps++) step(50u);  /* rejoin + catch up */
  }
  leader=elect_any();
  if(leader<0){
    fprintf(stderr,"FAIL no leader after heal (seed %" K_U64_FMT ")\n",(k_u64)seed);
    for(i=0;i<nnode;i++) fprintf(stderr,"  n%d leader=%d fatal=%d stopped=%d la=%" K_I64_FMT " snap=%" K_I64_FMT "\n",
      nodes[i].id,nodes[i].is_leader,nodes[i].fatal,nodes[i].stopped,
      (k_i64)nodes[i].last_applied,(k_i64)nodes[i].snapshot.index);
    release_cluster(); return 0;
  }
  {
    k_server *L=&nodes[leader];
    total=make_set_frame(frame,sizeof(frame),2u,"k2","v84");
    if(!total||!L->connections){ release_cluster(); return 0; }
    k_server_client_received(L->connections,frame,total);
  }

  /* --- snapshot recovery oracle: commit enough sets (> snapshot_entries) to
     force every node to persist a snapshot, then crash/restart a follower so it
     must recover FROM THE SNAPSHOT BYTES (last_included_index>0 path, not a
     full WAL replay).  A corrupt / divergent snapshot image fails the reload or
     the state hash right here -- the byte-faithfulness check. --- */
  {
    k_server *L=&nodes[leader];
    k_u8 fbuf[K_FRAME_HEADER+256];
    char skey[16],sval[24];
    unsigned int nset=(unsigned int)nodes[leader].cfg.snapshot_entries+16u; /* exceed threshold */
    unsigned int j;
    int snap_ok;
    for(j=0;j<nset;j++){
      sprintf(skey,"sn%03u",j);
      sprintf(sval,"sv%u",j*7u+1u);
      total=make_set_frame(fbuf,sizeof(fbuf),1000u+j,skey,sval);
      if(!total||!L->connections){ release_cluster(); return 0; }
      k_server_client_received(L->connections,fbuf,total);
    }
    for(steps=0;steps<MAXSTEP;steps++){
      step(50u);
      snap_ok=1;
      for(i=0;i<nnode;i++) if(nodes[i].snapshot.index<=0){ snap_ok=0; break; }
      if(snap_ok&&all_hash_same()) break;
    }
    if(!snap_ok){
      int a,b;
      /* Peer-link map: whether each ordered pair still has a live link.  A restarted node whose peers
         dropped their side without re-dialling starves exactly like this, and that is a harness question;
         links present and the node still at commit 2 would be a product one. */
      for(a=0;a<nnode;a++){
        for(b=0;b<nnode;b++){
          int vi;
          if(a==b) continue;
          vi=k_cluster_index(&nodes[a].cluster,nodes[b].id);
          /* conn=link object exists, sock=HELLO handshake completed.  The application re-dials a
             peer_conns-only (half-open) link, so the two must be printed apart. */
          fprintf(stderr,"  link %d->%d vi=%d conn=%d sock=%d mem=%d\n",nodes[a].id,nodes[b].id,vi,
            (vi>=0&&nodes[a].peer_conns[vi])?1:0,
            (vi>=0&&nodes[a].peer_socks[vi])?1:0,
            k_membership_contains(&nodes[a],nodes[b].id)?1:0);
        }
      }
      fprintf(stderr,"FAIL snapshot never fired (seed %" K_U64_FMT " ent=%u)\n",
        (k_u64)seed,(unsigned)nodes[leader].cfg.snapshot_entries);
      /* Print the same fields as the no-leader dump above, plus leader/fatal/stopped: without them a
         restarting node that never catches up is indistinguishable from one that died again, and that
         distinction decides whether this is a harness/ injector matter or a product finding. */
      for(i=0;i<nnode;i++) fprintf(stderr,"  n%d leader=%d fatal=%d stopped=%d stopping=%d raft_stopped=%d voter=%d learner=%d joint=%d reconn=%u snap_idx%" K_I64_FMT " la%" K_I64_FMT " raft{state=%d commit=%" K_I64_FMT "}\n",
        nodes[i].id,nodes[i].is_leader,nodes[i].fatal,nodes[i].stopped,nodes[i].stopping,nodes[i].raft_stopped,
        nodes[i].self_is_voter,nodes[i].self_is_learner,nodes[i].cfg_joint,
        nodes[i].reconnect_elapsed,
        (k_i64)nodes[i].snapshot.index,(k_i64)nodes[i].last_applied,
        nodes[i].raft?nodes[i].raft->state:-1,
        (k_i64)(nodes[i].raft?nodes[i].raft->commit_index:-1));
      release_cluster(); return 0;
    }
    if(!all_hash_same()){
      fprintf(stderr,"FAIL snapshot divergence (seed %" K_U64_FMT ")\n",(k_u64)seed);
      release_cluster(); return 0;
    }
    if(!all_read_same("sn000","sv1")){
      fprintf(stderr,"FAIL snapshot key divergence (seed %" K_U64_FMT ")\n",(k_u64)seed);
      release_cluster(); return 0;
    }
    { /* crash/restart a follower: recovery must now come from the snapshot */
      int victim,vi;
      for(vi=0;vi<nnode;vi++) if(!nodes[vi].is_leader){ victim=vi; break; }
      if(vi>=nnode) victim=0;
      if(crash_restart_node(victim)!=0){
        fprintf(stderr,"FAIL snapshot restart (seed %" K_U64_FMT ")\n",(k_u64)seed);
        release_cluster(); return 0;
      }
      for(steps=0;steps<600;steps++) step(50u);
      if(!all_hash_same()){
        fprintf(stderr,"FAIL snapshot-recovery divergence (seed %" K_U64_FMT ")\n",(k_u64)seed);
        for(i=0;i<nnode;i++) fprintf(stderr,"  n%d hash%" K_U64_FMT "\n",nodes[i].id,
          (k_u64)state_hash(nodes[i].tree));
        release_cluster(); return 0;
      }
    }
  }

  { /* --- multi-op consistency oracle: beyond plain SET, exercise MSET (atomic
       batch: fork + N set + commit), CAS (framework conditional, etcd-txn
       class), and FCALL (USER command: the leader computes the write-set ONCE
       on a fork and the followers mechanically replay it -- user code never
       runs per-node).  These are the content-level-consistency paths most at
       risk of divergence, so state_hash must stay equal across all nodes. --- */
    k_server *L=&nodes[leader];
    k_u8 mbuf[K_FRAME_HEADER+512];
    k_u32 mtotal;
    k_buf tmp;
    int settled;
    k_server_client_accepted(L,(void*)(size_t)9001);
    if(!L->connections){ release_cluster(); return 0; }
    /* deterministic transfer staging: seed two account balances */
    mtotal=make_set_frame(mbuf,sizeof(mbuf),200u,"acct_a","100");
    if(!mtotal){ release_cluster(); return 0; }
    k_server_client_received(L->connections,mbuf,mtotal);
    mtotal=make_set_frame(mbuf,sizeof(mbuf),201u,"acct_b","50");
    if(!mtotal){ release_cluster(); return 0; }
    k_server_client_received(L->connections,mbuf,mtotal);
    for(steps=0;steps<MAXSTEP;steps++){ step(50u); if(all_read_same("acct_a","100")&&all_read_same("acct_b","50")) break; }
    if(!all_read_same("acct_a","100")){ fprintf(stderr,"FAIL multi-op staging (seed %" K_U64_FMT ")\n",(k_u64)seed); release_cluster(); return 0; }
    /* MSET: 3-key atomic batch */
    memset(&tmp,0,sizeof(tmp));
    k_buf_u32(&tmp,3u);
    k_buf_u32(&tmp,2u); k_buf_bytes(&tmp,"m1",2); k_buf_u32(&tmp,2u); k_buf_bytes(&tmp,"v1",2);
    k_buf_u32(&tmp,2u); k_buf_bytes(&tmp,"m2",2); k_buf_u32(&tmp,2u); k_buf_bytes(&tmp,"v2",2);
    k_buf_u32(&tmp,2u); k_buf_bytes(&tmp,"m3",2); k_buf_u32(&tmp,2u); k_buf_bytes(&tmp,"v3",2);
    mtotal=make_req_frame(mbuf,sizeof(mbuf),202u,K_REQ_MSET,tmp.data,tmp.len,0,0);
    k_buf_free(&tmp);
    if(!mtotal){ release_cluster(); return 0; }
    k_server_client_received(L->connections,mbuf,mtotal);
    /* CAS: seed cas_key=cv1, then compare-and-swap old=cv1 -> new=cv2 */
    mtotal=make_set_frame(mbuf,sizeof(mbuf),203u,"cas_key","cv1");
    if(!mtotal){ release_cluster(); return 0; }
    k_server_client_received(L->connections,mbuf,mtotal);
    for(steps=0;steps<200;steps++){ step(50u); if(all_read_same("cas_key","cv1")) break; }
    memset(&tmp,0,sizeof(tmp));
    k_buf_u8(&tmp,(k_u8)K_CAS_CMP); k_buf_u32(&tmp,3u); k_buf_bytes(&tmp,"cv2",3); k_buf_u32(&tmp,3u); k_buf_bytes(&tmp,"cv1",3);
    mtotal=make_req_frame(mbuf,sizeof(mbuf),204u,K_REQ_CAS,"cas_key",7,tmp.data,tmp.len);
    k_buf_free(&tmp);
    if(!mtotal){ release_cluster(); return 0; }
    k_server_client_received(L->connections,mbuf,mtotal);
    /* Back-to-back CAS then FCALL (NO settle wait): the FCALL barrier registers
       before the CAS commits, so the leader's forked read view omits the CAS
       result.  This locks the invariant that FCALL apply is op-replay (not
       swap-root): a swap-root of the stale fork would clobber cas_key back to
       cv1 and diverge the leader from the followers (settle check below). */
    /* FCALL transfer: acct_a -= 30, acct_b += 30 (args = [from][to][amount]) */
    memset(&tmp,0,sizeof(tmp));
    k_buf_u32(&tmp,6u); k_buf_bytes(&tmp,"acct_a",6);
    k_buf_u32(&tmp,6u); k_buf_bytes(&tmp,"acct_b",6);
    k_buf_u32(&tmp,2u); k_buf_bytes(&tmp,"30",2);
    mtotal=make_req_frame(mbuf,sizeof(mbuf),205u,K_REQ_FCALL,"transfer",8,tmp.data,tmp.len);
    k_buf_free(&tmp);
    if(!mtotal){ release_cluster(); return 0; }
    k_server_client_received(L->connections,mbuf,mtotal);
    /* converge + assert the writes landed identically on every node */
    settled=0;
    for(steps=0;steps<MAXSTEP;steps++){
      step(50u);
      if(all_hash_same()&&all_read_same("acct_a","70")&&all_read_same("acct_b","80")
         &&all_read_same("m1","v1")&&all_read_same("m3","v3")&&all_read_same("cas_key","cv2")){ settled=1; break; }
    }
    if(!settled){
      fprintf(stderr,"FAIL multi-op divergence (seed %" K_U64_FMT ")\n",(k_u64)seed);
      for(i=0;i<nnode;i++){
        const unsigned char *v=0; unsigned int vl=0;
        char aa[16]="-",ab[16]="-",m1[16]="-",ck[16]="-";
        if(treap_get(nodes[i].tree,(const unsigned char*)"acct_a",6,&v,&vl)==1&&vl<16){ memcpy(aa,v,vl); aa[vl]=0; }
        if(treap_get(nodes[i].tree,(const unsigned char*)"acct_b",6,&v,&vl)==1&&vl<16){ memcpy(ab,v,vl); ab[vl]=0; }
        if(treap_get(nodes[i].tree,(const unsigned char*)"m1",2,&v,&vl)==1&&vl<16){ memcpy(m1,v,vl); m1[vl]=0; }
        if(treap_get(nodes[i].tree,(const unsigned char*)"cas_key",7,&v,&vl)==1&&vl<16){ memcpy(ck,v,vl); ck[vl]=0; }
        fprintf(stderr,"  n%d L%d la%" K_I64_FMT " acct_a=%s acct_b=%s m1=%s cas_key=%s hash%" K_U64_FMT "\n",
          nodes[i].id,nodes[i].is_leader,(k_i64)nodes[i].last_applied,aa,ab,m1,ck,
          (k_u64)state_hash(nodes[i].tree));
      }
      release_cluster(); return 0;
    }
  }

  /* concurrent-client register linearizability oracle */
  if(lincheck_phase()!=0){ fprintf(stderr,"FAIL linearizability (seed %" K_U64_FMT ")\n",(k_u64)seed); release_cluster(); return 0; }
  for(steps=0;steps<MAXSTEP;steps++){
    step(50u);
    why=final_check(&leaders);
    if(!why) break;
  }
  why=final_check(&leaders);
  if(why){
    fprintf(stderr,"FAIL %s (seed %" K_U64_FMT " drop=%d dup=%d churn=%d leaders=%d)\n",
            why,(k_u64)seed,g_drop_pct,g_dup_pct,g_part_churn_pct,leaders);
    for(i=0;i<nnode;i++){
      const unsigned char *v=0; unsigned int vl=0; int r=0;
      r=treap_get(nodes[i].tree,(const unsigned char*)"k2",2,&v,&vl);
      fprintf(stderr,"  n%d L%d la%" K_I64_FMT " getk2=%d vl%u hash%" K_U64_FMT "\n",nodes[i].id,nodes[i].is_leader,
        (k_i64)nodes[i].last_applied,r,(unsigned)vl,(k_u64)state_hash(nodes[i].tree));
    }
    release_cluster(); return 0;
  }
  release_cluster();
  return 1;
}

/* MSVC 6 declares neither strtoull nor _strtoui64 (C4013 "undefined" here, then an unresolved
   __strtoui64 at link time), and the name differs across CRTs.  Only a decimal seed is ever read. */
static k_u64 kscf_parse_u64(const char *s){
  k_u64 v=0;
  if(s==0) return 0;
  while(*s==' '||*s=='\t') s++;
  while(*s>='0'&&*s<='9'){ v=v*10u+(k_u64)(*s-'0'); s++; }
  return v;
}
int main(int argc,char **argv){
  k_u64 seed=(argc>1)?kscf_parse_u64(argv[1]):1;
  int count=(argc>2)?atoi(argv[2]):1;
  int i,ok=0;
  if(lincheck_selftest()!=0){ fprintf(stderr,"lincheck selftest failed\n"); return 1; }
  signal(SIGSEGV,kscf_crash_dump);
  signal(SIGILL,kscf_crash_dump);
  signal(SIGABRT,kscf_crash_dump);
  for(i=0;i<count;i++){
    k_u64 s=seed+(k_u64)i;
    /* print the seed BEFORE running so a crash or hang is reproducible from the
       last line of output (the FAIL messages also carry it, but a crash prints
       nothing at all) */
    g_current_seed=s;
    fprintf(stderr,"seed %" K_U64_FMT "\n",(k_u64)s);
    fflush(stderr);
    if(run_one_cluster(s)) ok++;
  }
  printf("done: %d/%d clusters consistent (OOM stopped nodes in %d runs)\n",ok,count,g_oom_clusters);
  printf("linearizability: %d histories / %d ops decided by the checker, %d inconclusive\n",
         g_lin_histories,g_lin_ops,g_lin_inconclusive);
  if(g_lin_histories<=0){
    fprintf(stderr,"FAIL: the linearizability checker decided no history at all - a check that never ran is not a pass\n");
    return 1;
  }
  return ok==count?0:1;
}
