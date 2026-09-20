/* vfs_fault_test.c -- the fourth injection seam, used for real: a fault-injecting vfs backend.
 *
 * The server reaches its storage only through vfs.h's backend routing (vfs_list[] + vfs_route), and
 * THAT is the seam the project declared for storage faults.  It had no user at all: every test drove
 * the stock mem/disk backends, so "the seam exists and works" was never demonstrated, and a storage
 * failure path was only ever reasoned about (review A7).
 *
 * The wrapper here is installed under the same scheme name as the stock mem backend ("mem"), so disk
 * routing stays reachable.  It delegates every operation to mem and re-points each file it opens at
 * itself, so every later vfs_write/vfs_sync on that file comes back through the wrapper and can be
 * made to fail on demand.
 *
 * What is asserted is content, not chatter: an injected sync/write failure must set server->fatal
 * (fail-stop), while the no-fault run must complete with the same counters non-zero - a green result
 * from a wrapper that never ran would prove nothing.
 *
 * AI contract: BEGIN / PASS / SUMMARY prefixes, fully deterministic.
 */
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
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "../code/kbase.h"
#include "../code/kproto.h"
#include "../code/kserver.h"
#include "test.h"

/* ---- the fault wrapper ---- */
static vfs_backend g_fault;
static int g_fail_write_after=-1;   /* fail the (n+1)-th write; -1 = never */
static int g_fail_sync_after=-1;
static int g_opens,g_reads,g_writes,g_syncs,g_injected;

static vfs_file *fault_open(vfs_backend *be,const char *path){
  vfs_file *file=vfs_mem.base.open(&vfs_mem.base,path);
  (void)be;
  if(file) file->be=&g_fault;      /* from here on this file's operations are ours */
  g_opens++;
  return file;
}
static int fault_unlink(vfs_backend *be,const char *path){ (void)be; return vfs_mem.base.unlink(&vfs_mem.base,path); }
static int fault_read(vfs_file *file,vfs_u64 off,void *buf,unsigned int size){
  g_reads++;
  return vfs_mem.base.read(file,off,buf,size);
}
static int fault_write(vfs_file *file,vfs_u64 off,const void *buf,unsigned int size){
  g_writes++;
  if(g_fail_write_after>=0&&g_writes>g_fail_write_after){
    g_injected++;
    return -1;
  }
  return vfs_mem.base.write(file,off,buf,size);
}
static int fault_sync(vfs_file *file){
  g_syncs++;
  if(g_fail_sync_after>=0&&g_syncs>g_fail_sync_after){
    g_injected++;
    return -1;
  }
  return vfs_mem.base.sync(file);
}
static void fault_close(vfs_file *file){
  file->be=&vfs_mem.base;          /* hand the file back to its own backend before closing */
  vfs_mem.base.close(file);
}
static void fault_reset(void){
  g_fail_write_after=-1; g_fail_sync_after=-1;
  g_opens=g_reads=g_writes=g_syncs=g_injected=0;
}
static void fault_install(void){
  memset(&g_fault,0,sizeof(g_fault));
  g_fault.name="mem";
  g_fault.open=fault_open;
  g_fault.unlink=fault_unlink;
  g_fault.read=fault_read;
  g_fault.write=fault_write;
  g_fault.sync=fault_sync;
  g_fault.close=fault_close;
  fault_reset();
  vfs_list[1]=&g_fault;            /* slot 1 held the stock mem backend; "disk" stays reachable */
}

/* ---- capture transport (the server must be given one; nothing is fed here) ---- */
static int tr_send(void *sock,k_u32 magic,k_u8 type,const void *payload,k_u32 size,int control){
  (void)sock;(void)magic;(void)type;(void)payload;(void)size;(void)control;
  return 0;
}
static void tr_close(void *sock){ (void)sock; }
static int tr_recv(void *sock){ (void)sock; return 0; }
static void *tr_dial(k_server *server,const k_node_spec *node){ (void)server;(void)node; return (void*)(size_t)1; }
static void tr_setud(void *sock,void *ud){ (void)sock;(void)ud; }
static const k_server_transport g_transport={"capture",tr_send,tr_close,tr_recv,tr_dial,tr_setud};

/* ---- single-node setup, mirroring kserver_test.c ---- */
static void setup(k_server *s,const char *base){
  k_cluster c;
  memset(s,0,sizeof(*s));
  memset(&c,0,sizeof(c));
  c.count=1;
  c.nodes[0].id=1;
  c.nodes[0].client_port=7000;
  c.nodes[0].peer_port=7001;
  strcpy(c.nodes[0].host,"127.0.0.1");
  k_server_init(s,1,c.nodes[0].client_port,c.nodes[0].peer_port,base,&c);
  s->runtime_backend="sync";
  s->transport=&g_transport;
  s->admission=1;
}
static void turn(k_server *s,unsigned int ms){
  k_server_advance(s,ms);
  if(s->wal_rt) runtime_drain(s->wal_rt);
  if(s->snapshot_rt) runtime_drain(s->snapshot_rt);
}
static int elect(k_server *s){
  int i;
  for(i=0;i<100&&!s->is_leader;i++) turn(s,50u);
  return s->is_leader?0:-1;
}
static void settle(k_server *s){
  int i;
  for(i=0;i<40;i++) turn(s,20u);
}
/* one SET through the client path: accepted -> frame -> apply */
static int do_set(k_server *s,k_u32 request_id){
  k_buf b;
  k_u8 frame[256];
  k_u32 total;
  raft_i64 before=s->last_applied;
  int i;
  memset(&b,0,sizeof(b));
  if(k_request_payload(&b,request_id,K_REQ_SET,"k",1u,"v",1u)!=0){ k_buf_free(&b); return -1; }
  total=K_FRAME_HEADER+b.len;
  if(total>sizeof(frame)){ k_buf_free(&b); return -1; }
  k_frame_header_build(frame,K_CLIENT_MAGIC,K_REQ_SET,b.len);
  if(b.len) memcpy(frame+K_FRAME_HEADER,b.data,b.len);
  k_buf_free(&b);
  k_server_client_accepted(s,(void*)(size_t)1);
  k_server_client_received(s->connections,frame,total);
  for(i=0;i<200&&s->last_applied<=before;i++) turn(s,20u);
  return (s->last_applied>before)?0:-1;
}

static unsigned int g_seq;

/* 1. the seam is transparent and the wrapper is really in the path */
static void test_seam_transparent(void){
  k_server s;
  char base[64];
  TEST_BEGIN("vfs fault backend: no fault injected => the wrapper is in the path and harmless");
  fault_install();
  sprintf(base,"mem://vfault-clean-%u",g_seq++);
  setup(&s,base);
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  TEST_ASSERT(do_set(&s,1u)==0,"SET applies");
  settle(&s);
  printf("    backend: opens=%d writes=%d syncs=%d reads=%d injected=%d fatal=%d\n",
         g_opens,g_writes,g_syncs,g_reads,g_injected,s.fatal);
  TEST_ASSERT(g_opens>0&&g_writes>0&&g_syncs>0,"the wrapper was actually used (no 0-sample)");
  TEST_ASSERT(g_injected==0&&s.fatal==0,"no fault injected, no fail-stop");
  k_server_release(&s);
  TEST_END();
}

/* 2. an injected fsync failure must fail-stop, loudly */
static void test_injected_sync_failure(void){
  k_server s;
  char base[64];
  TEST_BEGIN("vfs fault backend: every fsync fails from now on => fail-stop");
  fault_install();
  sprintf(base,"mem://vfault-sync-%u",g_seq++);
  setup(&s,base);
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  /* Arm AFTER open on purpose: the config file is the first thing written, and a fault there makes
     open refuse to start (a different, equally loud path - see the pre-open test below).  Here the
     failure has to land on the WAL flush of a running server. */
  g_fail_sync_after=g_syncs;
  do_set(&s,1u);
  settle(&s);
  printf("    backend: writes=%d syncs=%d injected=%d fatal=%d\n",g_writes,g_syncs,g_injected,s.fatal);
  TEST_ASSERT(g_injected>0,"the injected fsync failure actually fired");
  TEST_ASSERT(s.fatal==1,"a storage failure the server cannot survive must fail-stop, not be ignored");
  k_server_release(&s);
  TEST_END();
}

/* 3. an injected write failure must fail-stop too (same class, different call) */
static void test_injected_write_failure(void){
  k_server s;
  char base[64];
  TEST_BEGIN("vfs fault backend: every write fails from now on => fail-stop");
  fault_install();
  sprintf(base,"mem://vfault-write-%u",g_seq++);
  setup(&s,base);
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  g_fail_write_after=g_writes;
  do_set(&s,1u);
  settle(&s);
  printf("    backend: writes=%d syncs=%d injected=%d fatal=%d\n",g_writes,g_syncs,g_injected,s.fatal);
  TEST_ASSERT(g_injected>0,"the injected write failure actually fired");
  TEST_ASSERT(s.fatal==1,"a failed WAL write must fail-stop, not be ignored");
  k_server_release(&s);
  TEST_END();
}

/* 4. a fault during open must be refused loudly, not survived */
static void test_fault_during_open(void){
  k_server s;
  char base[64];
  int rc;
  TEST_BEGIN("vfs fault backend: storage failure while opening => the server refuses to start");
  fault_install();
  g_fail_sync_after=0;             /* nothing can be persisted from the very first call */
  sprintf(base,"mem://vfault-open-%u",g_seq++);
  setup(&s,base);
  rc=k_server_open(&s);
  printf("    backend: opens=%d writes=%d syncs=%d injected=%d open_rc=%d\n",
         g_opens,g_writes,g_syncs,g_injected,rc);
  TEST_ASSERT(g_injected>0,"a storage fault fired while opening");
  TEST_ASSERT(rc!=0,"open must report the failure instead of starting anyway");
  /* No fatal flag is asserted here on purpose: a failed OPEN is reported to its caller (kdbsvr prints
     "fatal: cannot start server: ..." and exits 1), which is the loud signal for this stage.  Asserting
     a flag the product does not set would be testing the test. */
  k_server_release(&s);
  TEST_END();
}

int main(void){
  TEST_PLAN(4);
  test_seam_transparent();
  test_injected_sync_failure();
  test_injected_write_failure();
  test_fault_during_open();
  TEST_SUMMARY();
  return TEST_EXIT_CODE();
}
