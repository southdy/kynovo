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
/* Real threads for the concurrency case at the bottom of this file, and the runtime's gate for a
   synchronised start so the two threads cannot simply miss each other. */
#define RUNTIME_STATIC
#define RUNTIME_IMPLEMENTATION
#include "../code/runtime.h"

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

/* ---- the mem backend under two threads, on two paths that share ONE hash bucket ----
   The in-memory backend keeps one process-global table, and vfs_mem_open's bucket insert / refcount++ used to
   race with vfs_mem_unlink's unlink / refcount test / free: two threads inside those calls could lose an
   update, which shows up as a file that is not there any more (or as a double free).  It serialises that table
   with a spinlock now, and this is the case that would notice if the lock ever went away.

   The two paths are asserted to collide, so the test cannot quietly stop testing what it is named after.  The
   backend is driven directly - the wrapper this file installs under the "mem" scheme is not part of what is
   being checked.  What is asserted is per-thread content: each thread opens its own path, writes, reads back,
   closes and unlinks it, and counts anything that did not behave.  A lost bucket update makes one of those
   steps fail; the double-free shape crashes here instead of passing silently. */
typedef struct memlock_arg{
  const char *path;
  runtime_gate *gate;
  int iterations;
  int opens;
  int failures;
} memlock_arg;

static void memlock_thread(runtime_ctx *rt,void *arg){
  memlock_arg *a=(memlock_arg *)arg;
  k_u8 payload[32],back[32];
  int i,j;
  runtime_worker_ready(rt);
  runtime_gate_arrive(a->gate);
  for(i=0;i<a->iterations;i++){
    vfs_file *f;
    for(j=0;j<32;j++) payload[j]=(k_u8)(i+j);
    f=vfs_mem_open(&vfs_mem.base,a->path);
    if(!f){ a->failures++; continue; }
    a->opens++;
    if(vfs_write(f,0,payload,32u)!=0) a->failures++;
    memset(back,0,sizeof(back));
    if(vfs_read(f,0,back,32u)!=0) a->failures++;
    else if(memcmp(back,payload,32u)!=0) a->failures++;
    vfs_close(f);
    if(vfs_mem_unlink(&vfs_mem.base,a->path)!=0) a->failures++;   /* only this thread touches this path */
  }
  runtime_worker_exit(rt);
}

static void test_mem_backend_two_threads_one_bucket(void){
  runtime_ctx *rt_a,*rt_b;
  runtime_gate *gate;
  memlock_arg a,b;
  char path_a[64],path_b[64];
  TEST_BEGIN("vfs mem backend: two threads, two paths in one hash bucket");
  strcpy(path_a,"kstest-memlock-a");
  strcpy(path_b,"kstest-memlock-b597");
  /* the collision is the point of the test, so it is asserted and not assumed */
  TEST_ASSERT(vfs_hash(path_a,strlen(path_a))%VFS_MEM_HASH_BUCKETS==
              vfs_hash(path_b,strlen(path_b))%VFS_MEM_HASH_BUCKETS,
              "the two paths hash to the same bucket");
  memset(&a,0,sizeof(a));
  memset(&b,0,sizeof(b));
  a.path=path_a; b.path=path_b;
  a.iterations=20000; b.iterations=20000;
  gate=runtime_gate_create(2);
  TEST_ASSERT(gate!=0,"gate");
  a.gate=gate; b.gate=gate;
  rt_a=runtime_create("thread",1,memlock_thread,&a);
  rt_b=runtime_create("thread",1,memlock_thread,&b);
  TEST_ASSERT(rt_a!=0&&rt_b!=0,"two worker threads");
  runtime_wait_workers_ready(rt_a);
  runtime_wait_workers_ready(rt_b);
  runtime_gate_wait(gate);                 /* both threads are at the start line */
  runtime_gate_open(gate);                 /* ... go */
  runtime_wait_workers_exit(rt_a);
  runtime_wait_workers_exit(rt_b);
  TEST_ASSERT_I64_EQ(a.opens,a.iterations,"thread A ran its whole loop");
  TEST_ASSERT_I64_EQ(b.opens,b.iterations,"thread B ran its whole loop");
  TEST_ASSERT_I64_EQ(a.failures,0,"thread A saw no failed open/write/read/unlink");
  TEST_ASSERT_I64_EQ(b.failures,0,"thread B saw no failed open/write/read/unlink");
  TEST_ASSERT(vfs_mem_unlink(&vfs_mem.base,path_a)==-1,"nothing left behind under A's path");
  TEST_ASSERT(vfs_mem_unlink(&vfs_mem.base,path_b)==-1,"nothing left behind under B's path");
  runtime_gate_destroy(gate);
  runtime_destroy(rt_a);
  runtime_destroy(rt_b);
  TEST_END();
}

int main(void){
  TEST_PLAN(5);
  test_seam_transparent();
  test_injected_sync_failure();
  test_injected_write_failure();
  test_fault_during_open();
  test_mem_backend_two_threads_one_bucket();
  TEST_SUMMARY();
  return TEST_EXIT_CODE();
}
