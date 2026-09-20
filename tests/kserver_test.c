/* kserver_test.c -- black-box deterministic tests for the kynovo replicated
 * storage engine core (kserver.h).  Mirrors raft_test.c: injects a capture
 * transport + the deterministic "sync" runtime backend + a "mem:" disk, drives
 * the server by hand (k_server_advance + runtime_drain), and asserts content-
 * level state (treap contents) plus captured outbound frames.
 *
 * This is NOT a full replication cluster (that is kserver_cluster_fuzz.c's
 * job); it pins the single-node engine path: open -> elect -> apply -> read,
 * proving the three-injection-seam core (transport / time / runtime backend)
 * compiles and runs deterministically with zero cemon / cli.
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

/* ---- capture transport (records outbound frames; feeds nothing itself) ---- */
static struct{
  int send_count;
  k_u32 last_magic;
  k_u8 last_type;
  k_u8 last_payload[2048];
  k_u32 last_size;
  int fail_send;
  int recv_calls;
  int close_calls;
  int dial_calls;
} gcap;

static void cap_reset(void){ memset(&gcap,0,sizeof(gcap)); }

static int cap_send_frame(void *sock,k_u32 magic,k_u8 type,const void *payload,k_u32 size,int control){
  (void)sock;(void)control;
  if(gcap.fail_send) return -1;
  gcap.send_count++;
  gcap.last_magic=magic;
  gcap.last_type=type;
  gcap.last_size=size;
  if(size&&payload) memcpy(gcap.last_payload,payload,(size<sizeof(gcap.last_payload))?size:sizeof(gcap.last_payload));
  return 0;
}
static void cap_close(void *sock){ (void)sock; gcap.close_calls++; }
static int cap_recv(void *sock){ (void)sock; gcap.recv_calls++; return 0; }
static void *cap_dial(k_server *server,const k_node_spec *node){ (void)server;(void)node; gcap.dial_calls++; return (void*)(size_t)1; }
static void cap_setud(void *sock,void *ud){ (void)sock;(void)ud; }
static const k_server_transport cap_transport={"cap",cap_send_frame,cap_close,cap_recv,cap_dial,cap_setud};

/* ---- single-node setup (deterministic: sync runtime, mem: disk, capture I/O) ---- */
static void setup(k_server *s,int id,const char *base){
  k_cluster c;
  memset(s,0,sizeof(*s));
  memset(&c,0,sizeof(c));
  c.count=1;
  c.nodes[0].id=id;
  c.nodes[0].client_port=7000;
  c.nodes[0].peer_port=7001;
  strcpy(c.nodes[0].host,"127.0.0.1");
  k_server_init(s,id,c.nodes[0].client_port,c.nodes[0].peer_port,base,&c);
  s->runtime_backend="sync";
  s->transport=&cap_transport;
  s->admission=1;
  cap_reset();
}

/* one deterministic turn: advance the raft clock, then drain both sync workers */
static void turn(k_server *s,unsigned int ms){
  k_server_advance(s,ms);
  if(s->wal_rt) runtime_drain(s->wal_rt);
  if(s->snapshot_rt) runtime_drain(s->snapshot_rt);
}

/* drive until the single node self-elects (election_min_max 250..500 ms) */
static int elect(k_server *s){
  int i;
  for(i=0;i<100&&!s->is_leader;i++) turn(s,50u);
  return s->is_leader?0:-1;
}

/* build a full client request frame (header + payload) into `out`, return total */
static k_u32 make_client_frame(k_u8 *out,k_u32 cap,k_u8 type,k_u32 request_id,const void *key,k_u32 key_len,const void *value,k_u32 value_len){
  k_buf b;
  k_u32 total;
  memset(&b,0,sizeof(b));
  if(k_request_payload(&b,request_id,type,key,key_len,value,value_len)!=0){ k_buf_free(&b); return 0; }
  total=K_FRAME_HEADER+b.len;
  if(total>cap){ k_buf_free(&b); return 0; }
  k_frame_header_build(out,K_CLIENT_MAGIC,type,b.len);
  if(b.len) memcpy(out+K_FRAME_HEADER,b.data,b.len);
  k_buf_free(&b);
  return total;
}

/* A client request that arrives in the SAME drive cycle as its connection accept must
   still be answered.  That is exactly what the CLI does: its transport sends the first
   (discovery) request from inside the connect completion, so the server sees the accept
   and the frame in one poll, before any other drive has run.  Written because the live CLI
   hung right after CONNECT with its discovery reply never delivered. */
static unsigned int g_sameframe_seq;
static void test_client_request_in_accept_cycle_is_answered(void){
  k_server s;
  char base[64];
  k_u8 frame[K_FRAME_HEADER+64];
  k_u32 total;
  int i,sends_before,closes_before;
  TEST_BEGIN("first client request in the accept cycle is answered (CLI send-from-connect)");
  sprintf(base,"mem://kstest-sameframe-%u",g_sameframe_seq++);
  setup(&s,1,base);
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  sends_before=gcap.send_count;
  closes_before=gcap.close_calls;
  k_server_client_accepted(&s,(void*)(size_t)1);
  total=make_client_frame(frame,sizeof(frame),K_REQ_MEMBERS,1u,0,0,0,0);
  TEST_ASSERT(total>0,"MEMBERS frame");
  k_server_client_received(s.connections,frame,total);   /* same cycle as the accept */
  for(i=0;i<200;i++) turn(&s,20u);
  TEST_ASSERT(gcap.send_count>sends_before,"server answered the first request");
  TEST_ASSERT(gcap.last_type==K_RESPONSE,"the reply is a response frame");
  TEST_ASSERT(gcap.close_calls==closes_before,"connection was not closed");
  k_server_release(&s);
}


static void apply_until(k_server *s,raft_i64 target){
  int i;
  for(i=0;i<200&&s->last_applied<target;i++) turn(s,50u);
}

/* ---- WAL recovery after the meta slot was NOT fsync'ed ----
   The record carries its own header/CRC, so the meta slot is only a hint that is
   fsync'ed on rotation and every K_WAL_META_FSYNC_EVERY records.  If the slot's
   writes are lost in a crash (modelled here by putting the older slot bytes back),
   recovery must still find the NEWEST durable record by scanning the segment. */
static unsigned int g_walscan_seq;
static void test_wal_recovery_scan_ignores_stale_meta(void){
  k_server s;
  char base[64];
  char path[K_URI_MAX];
  k_u8 slot_bytes[K_WAL_META_SIZE];
  const unsigned char *val=0;
  unsigned int vlen=0;
  k_u8 frame[K_FRAME_HEADER+64];
  k_u32 total;
  vfs_file *meta;
  raft_i64 bl;
  int i;
  TEST_BEGIN("server WAL recovery scans past a stale (un-fsynced) meta slot");
  sprintf(base,"mem://kstest-walscan-%u",g_walscan_seq++);
  setup(&s,1,base);
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  bl=s.last_applied;
  k_server_client_accepted(&s,(void*)(size_t)1);
  total=make_client_frame(frame,sizeof(frame),K_REQ_SET,1u,"k",1,"a",1);
  TEST_ASSERT(total>0,"SET a frame");
  k_server_client_received(s.connections,frame,total);
  apply_until(&s,bl+1);
  for(i=0;i<40;i++) turn(&s,20u);          /* first record durable */
  /* remember the meta slot state from BEFORE the second write */
  TEST_ASSERT(k_path_suffix(path,base,".wal.meta")==0,"meta path");
  meta=vfs_open(path);
  TEST_ASSERT(meta!=0,"meta file open");
  TEST_ASSERT(vfs_read(meta,0,slot_bytes,sizeof(slot_bytes))==0,"meta read");
  vfs_close(meta);
  bl=s.last_applied;
  k_server_client_accepted(&s,(void*)(size_t)1);
  total=make_client_frame(frame,sizeof(frame),K_REQ_SET,2u,"k",1,"b",1);
  TEST_ASSERT(total>0,"SET b frame");
  k_server_client_received(s.connections,frame,total);
  apply_until(&s,bl+1);
  for(i=0;i<40;i++) turn(&s,20u);          /* second record durable */
  /* simulate a crash that lost the meta slot's recent writes */
  meta=vfs_open(path);
  TEST_ASSERT(meta!=0,"meta reopen");
  TEST_ASSERT(vfs_write(meta,0,slot_bytes,sizeof(slot_bytes))==0,"meta restore");
  vfs_close(meta);
  k_server_release(&s);
  setup(&s,1,base);
  TEST_ASSERT(k_server_open(&s)==0,"reopen (scan recovery)");
  TEST_ASSERT(elect(&s)==0,"re-elect");
  for(i=0;i<300;i++) turn(&s,20u);
  TEST_ASSERT(treap_get(s.tree,(const unsigned char*)"k",1,&val,&vlen)==1,"key recovered");
  TEST_ASSERT(vlen==1&&val&&val[0]=='b',"recovery used the NEWEST record, not the slot's record");
  k_server_release(&s);
  TEST_END();
}

/* ---- driver clock: the sub-millisecond remainder must be carried ----
   The production loop samples the clock once per drained network completion, so
   it can run thousands of iterations per second.  Rounding each sub-millisecond
   iteration up to 1 ms made the logical clock run several times faster than wall
   time (Raft election/heartbeat and the flush window fired early). */
static void test_elapsed_step_carries_sub_ms(void){
  k_u64 now=1000000u,last=1000000u;   /* the origin is already established */
  unsigned int total=0,ms,i;
  TEST_BEGIN("server: elapsed step carries the sub-ms remainder");
  for(i=0;i<5000;i++){
    now+=200u;                       /* 200 us per iteration -> 1 s of wall time */
    total+=k_server_elapsed_step(now,&last,1000u);
  }
  TEST_ASSERT(total==1000u,"logical time equals wall time exactly (no per-loop rounding)");
  TEST_ASSERT_I64_EQ((raft_i64)last,2000000,"the origin moved exactly 1 s of wall time");
  /* A single sub-millisecond sample must advance NOTHING and must not move the
     origin: moving it (or rounding the step up to 1 ms) is exactly what made the
     old driver's logical clock run several times faster than wall time. */
  now=3000000u; last=3000000u;
  TEST_ASSERT(k_server_elapsed_step(now+200u,&last,1000u)==0u,"a 200 us step advances nothing");
  TEST_ASSERT_I64_EQ((raft_i64)last,3000000,"a 200 us step does not move the origin");
  TEST_ASSERT(k_server_elapsed_step(now+900u,&last,1000u)==0u,"still nothing at 900 us");
  TEST_ASSERT(k_server_elapsed_step(now+1000u,&last,1000u)==1u,"one whole millisecond advances 1 ms");
  now+=5000000u;                     /* a debugger-class stall */
  ms=k_server_elapsed_step(now,&last,1000u);
  TEST_ASSERT(ms==1000u,"a long stall advances exactly the cap");
  now+=1500u;
  ms=k_server_elapsed_step(now,&last,1000u);
  TEST_ASSERT(ms==1u,"time continues right after a resync");
  TEST_END();
}

/* ---- a fresh batch's age must not include the poll wait before its arrival ---- */
static void test_group_commit_batch_age_starts_at_arrival(void){
  k_server s;
  k_u8 frame[K_FRAME_HEADER+64];
  k_u32 total;
  raft_i64 bl;
  int i;
  TEST_BEGIN("server: fresh batch age starts at its own arrival");
  setup(&s,1,"mem://kstest-batchage");
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  bl=s.last_applied;
  k_server_client_accepted(&s,(void*)(size_t)1);
  total=make_client_frame(frame,sizeof(frame),K_REQ_SET,1u,"k",1,"v",1);
  TEST_ASSERT(total>0,"SET frame");
  k_server_client_received(s.connections,frame,total);
  TEST_ASSERT(s.write_count==1,"write queued");
  /* PRODUCTION ORDER (kdbsvr.c): after the poll, the empty->non-empty transition
     resets the batch age AND the same round is advanced with batch_ms=0, so the poll
     wait that delivered this write cannot count as its age. */
  s.write_elapsed=99u;                 /* whatever the previous batch left behind */
  k_server_batch_start(&s);
  TEST_ASSERT(s.write_elapsed==0,"batch age reset at arrival");
  k_server_advance_at(&s,8u,0u);       /* an 8 ms poll delivered this write */
  TEST_ASSERT(s.write_count==1,"a poll wait BEFORE the arrival does not expire the window");
  k_server_advance_at(&s,1u,1u);
  TEST_ASSERT(s.write_count==1,"not submitted 1 ms into a 3 ms window");
  k_server_advance_at(&s,2u,2u);
  TEST_ASSERT(s.write_count==0,"submitted when the batch's own age really expired");
  apply_until(&s,bl+1);
  for(i=0;i<20;i++) turn(&s,20u);
  TEST_ASSERT(s.flush_by_window>0,"the window (not the poll wait) triggered the submit");
  TEST_ASSERT(s.wal_records>=s.flush_batches,"records >= submit groups (one advance can merge)");
  TEST_ASSERT(s.wal_records>0,"the WAL record counter advances");
  /* Same poll, second case: a partial batch is already queued, the poll reaches the
     item target mid-round (that batch is submitted) and writes keep arriving, so a
     RESIDUAL batch is left behind.  The residual batch is new: it must not inherit the
     age the submitted batch had accumulated. */
  {
    k_u64 flushes_before;
    k_u32 pending_before;
    unsigned int bms;
    int j;
    flushes_before=s.flush_batches;
    pending_before=s.write_count;
    for(j=0;j<20;j++){
      k_u8 f3[K_FRAME_HEADER+64];
      k_u32 t3;
      k_server_client_accepted(&s,(void*)(size_t)(size_t)(j+1));
      t3=make_client_frame(f3,sizeof(f3),K_REQ_SET,(k_u32)(100u+(k_u32)j),"kr",2,"v",1);
      TEST_ASSERT(t3>0,"SET frame");
      k_server_client_received(s.connections,f3,t3);
    }
    TEST_ASSERT(s.write_count==20u+pending_before,"partial batch queued");
    for(j=0;j<(int)(s.cfg.flush_item_limit-(20u+pending_before)+5u);j++){
      k_u8 f4[K_FRAME_HEADER+64];
      k_u32 t4;
      k_server_client_accepted(&s,(void*)(size_t)(size_t)(200+j));
      t4=make_client_frame(f4,sizeof(f4),K_REQ_SET,(k_u32)(200u+(k_u32)j),"kr",2,"v",1);
      k_server_client_received(s.connections,f4,t4);
    }
    TEST_ASSERT(s.flush_batches>flushes_before,"the item target submitted mid-round");
    TEST_ASSERT(s.write_count>0,"a residual batch was left behind");
    bms=k_server_batch_elapsed_ms(&s,8u,pending_before,flushes_before);
    TEST_ASSERT(bms==0u,"the residual batch's age starts at its own arrival");
    k_server_advance_at(&s,8u,bms);
    TEST_ASSERT(s.write_count>0,"the residual batch was NOT flushed by an inherited age");
  }
  k_server_release(&s);
  TEST_END();
}

/* ---- the durable meta slot is only ever written when it is fsynced ----
   An earlier version wrote the (unsynced) slot on every batch, overwriting the last
   durable copy in the page cache: a power loss could then leave BOTH slots stale or
   torn, leaving k_wal_meta_load with no usable scan start even though every record
   was intact.  Now the durable copies advance only at a checkpoint / rotation, and
   between them the position lives in memory. */
static unsigned int g_walmeta_seq;
static void test_wal_meta_slot_only_written_when_durable(void){
  k_server s;
  char base[64];
  k_wal_meta_state before,after;
  k_u8 frame[K_FRAME_HEADER+64];
  k_u32 total;
  raft_i64 bl;
  int i;
  TEST_BEGIN("server WAL durable meta slot only advances at a checkpoint");
  sprintf(base,"mem://kstest-walmeta-%u",g_walmeta_seq++);
  setup(&s,1,base);
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  bl=s.last_applied;
  k_server_client_accepted(&s,(void*)(size_t)1);
  total=make_client_frame(frame,sizeof(frame),K_REQ_SET,1u,"k",1,"a",1);
  TEST_ASSERT(total>0,"SET frame");
  k_server_client_received(s.connections,frame,total);
  apply_until(&s,bl+1);
  for(i=0;i<40;i++) turn(&s,20u);
  TEST_ASSERT(k_wal_meta_load(base,&before)==1,"durable slot readable");
  TEST_ASSERT(before.record_size>0,"the durable slot names a record");
  for(i=0;i<5;i++){
    k_u8 f2[K_FRAME_HEADER+64];
    k_u32 t2;
    char val[4];
    sprintf(val,"%d",i);
    bl=s.last_applied;
    k_server_client_accepted(&s,(void*)(size_t)1);
    t2=make_client_frame(f2,sizeof(f2),K_REQ_SET,(k_u32)(i+2u),"k",1,val,(k_u32)strlen(val));
    TEST_ASSERT(t2>0,"SET frame");
    k_server_client_received(s.connections,f2,t2);
    apply_until(&s,bl+1);
    for(bl=0;bl<30;bl++) turn(&s,20u);
  }
  TEST_ASSERT(k_wal_meta_load(base,&after)==1,"durable slot still readable");
  TEST_ASSERT_I64_EQ((raft_i64)after.generation,(raft_i64)before.generation,
                     "a non-checkpoint batch does NOT overwrite the durable slot");
  TEST_ASSERT_I64_EQ((raft_i64)after.record.offset,(raft_i64)before.record.offset,
                     "the durable slot still names the same record");
  TEST_ASSERT(s.wal_meta.generation>before.generation,
              "the in-memory position does advance per batch");
  k_server_release(&s);
  TEST_END();
}

/* ---- BOTH slots invalid (a power loss that tore both copies) must fail-stop ----
   With the durable copies only replaced one at a time at a checkpoint, an intact
   fsynced slot always remains; both being unreadable therefore means real corruption
   of the meta file, and guessing a scan start could silently drop acked writes. */
static unsigned int g_walmetabad_seq;
static void test_wal_meta_both_slots_invalid_fail_stop(void){
  k_server s;
  char base[64];
  char path[K_URI_MAX];
  k_u8 frame[K_FRAME_HEADER+64];
  k_u8 bad[2];
  k_u32 total;
  vfs_file *meta;
  raft_i64 bl;
  int i;
  TEST_BEGIN("server WAL meta with both slots torn refuses to start (fail-stop)");
  sprintf(base,"mem://kstest-walmetabad-%u",g_walmetabad_seq++);
  setup(&s,1,base);
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  bl=s.last_applied;
  k_server_client_accepted(&s,(void*)(size_t)1);
  total=make_client_frame(frame,sizeof(frame),K_REQ_SET,1u,"k",1,"a",1);
  TEST_ASSERT(total>0,"SET frame");
  k_server_client_received(s.connections,frame,total);
  apply_until(&s,bl+1);
  for(i=0;i<40;i++) turn(&s,20u);
  k_server_release(&s);
  TEST_ASSERT(k_path_suffix(path,base,".wal.meta")==0,"meta path");
  meta=vfs_open(path);
  TEST_ASSERT(meta!=0,"meta open");
  bad[0]=0x5a; bad[1]=0xa5;
  TEST_ASSERT(vfs_write(meta,2u,bad,1u)==0,"tear slot 0");
  TEST_ASSERT(vfs_write(meta,(k_u64)K_WAL_META_SLOT_GAP+2u,bad+1,1u)==0,"tear slot 1");
  vfs_close(meta);
  setup(&s,1,base);
  TEST_ASSERT(k_server_open(&s)!=0,"recovery REFUSES to start without a usable meta slot");
  TEST_END();
}

/* ---- a torn NEWER checkpoint slot must fall back to the older one ----
   The declared scenario is not a torn idle slot but a crash that dies while writing the
   NEXT checkpoint copy: the alternate slot then looks newer yet fails its CRC, and
   recovery must (a) select the surviving older slot and (b) scan FORWARD from it to the
   newest record, which was fsynced before the checkpoint copy was attempted. */
static unsigned int g_walmetatorn_seq;
static void test_wal_meta_one_slot_torn_still_starts(void){
  k_server s;
  char base[64];
  char meta_path[K_URI_MAX];
  k_wal_meta_state durable;
  k_u8 frame[K_FRAME_HEADER+64];
  k_u8 slot[K_WAL_META_SLOT_SIZE];
  k_wal_meta_state promoted;
  k_u32 total;
  vfs_file *meta;
  raft_i64 bl;
  const unsigned char *val=0;
  unsigned int vlen=0;
  int i,other;
  TEST_BEGIN("server WAL meta with a torn NEWER slot recovers from the older slot");
  sprintf(base,"mem://kstest-walmetatorn-%u",g_walmetatorn_seq++);
  setup(&s,1,base);
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  for(i=0;i<3;i++){
    char v[2];
    v[0]=(char)('a'+i); v[1]='\0';
    bl=s.last_applied;
    k_server_client_accepted(&s,(void*)(size_t)1);
    total=make_client_frame(frame,sizeof(frame),K_REQ_SET,(k_u32)(i+1),"k",1,v,1);
    TEST_ASSERT(total>0,"SET frame");
    k_server_client_received(s.connections,frame,total);
    apply_until(&s,bl+1);
    for(bl=0;bl<30;bl++) turn(&s,20u);
  }
  TEST_ASSERT(k_wal_meta_load(base,&durable)==1,"a durable slot is readable");
  TEST_ASSERT(durable.generation<s.wal_meta.generation,"the durable slot lags the newest record");
  k_server_release(&s);
  /* write the NEXT checkpoint copy into the other slot and tear it: the slot ends up
     newer than the surviving one with an invalid CRC */
  promoted=s.wal_meta;
  other=(durable.slot==0)?1:0;
  k_wal_meta_slot_encode(slot,&promoted);
  slot[6]^=0x5a;                             /* tear the encoded copy */
  TEST_ASSERT(k_path_suffix(meta_path,base,".wal.meta")==0,"meta path");
  meta=vfs_open(meta_path);
  TEST_ASSERT(meta!=0,"meta open");
  TEST_ASSERT(vfs_write(meta,(k_u64)other*(k_u64)K_WAL_META_SLOT_GAP,slot,sizeof(slot))==0,"write the torn slot");
  TEST_ASSERT(vfs_sync(meta)==0,"fsync the torn slot");
  vfs_close(meta);
  setup(&s,1,base);
  TEST_ASSERT(k_server_open(&s)==0,"recovery falls back to the older, valid slot");
  TEST_ASSERT(elect(&s)==0,"re-elect");
  for(i=0;i<300;i++) turn(&s,20u);
  TEST_ASSERT(treap_get(s.tree,(const unsigned char*)"k",1,&val,&vlen)==1,"key recovered");
  TEST_ASSERT(vlen==1&&val&&val[0]=='c',"scanning forward from the older slot reached the newest record");
  k_server_release(&s);
  TEST_END();
}


/* ---- a torn tail record is skipped ----
   A crash in the middle of a record write leaves a PARTIAL record after the last
   complete one (header written, payload missing).  Those bytes were never fsynced and
   never acked, so recovery must ignore them and use the last complete record. */
static unsigned int g_waltorn_seq;
static void test_wal_recovery_skips_torn_tail(void){
  k_server s;
  char base[64];
  char path[K_URI_MAX];
  const unsigned char *val=0;
  unsigned int vlen=0;
  k_u8 frame[K_FRAME_HEADER+64];
  k_u8 hdr[K_WAL_HEADER_SIZE];
  k_u32 total;
  vfs_file *seg;
  raft_i64 bl,tail;
  int i;
  TEST_BEGIN("server WAL recovery skips a torn (partial) tail record");
  sprintf(base,"mem://kstest-waltorn-%u",g_waltorn_seq++);
  setup(&s,1,base);
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  bl=s.last_applied;
  k_server_client_accepted(&s,(void*)(size_t)1);
  total=make_client_frame(frame,sizeof(frame),K_REQ_SET,1u,"k",1,"a",1);
  TEST_ASSERT(total>0,"SET a frame");
  k_server_client_received(s.connections,frame,total);
  apply_until(&s,bl+1);
  for(i=0;i<40;i++) turn(&s,20u);
  bl=s.last_applied;
  k_server_client_accepted(&s,(void*)(size_t)1);
  total=make_client_frame(frame,sizeof(frame),K_REQ_SET,2u,"k",1,"b",1);
  TEST_ASSERT(total>0,"SET b frame");
  k_server_client_received(s.connections,frame,total);
  apply_until(&s,bl+1);
  for(i=0;i<40;i++) turn(&s,20u);
  /* a RECORD HEADER claiming a 4 KiB payload, at the tail: exactly what a crash in the
     middle of a record write leaves behind */
  tail=(raft_i64)s.wal_meta.next.offset;
  TEST_ASSERT(tail>0,"tail position known");
  memset(hdr,0,sizeof(hdr));
  k_write_u32(hdr,K_WAL_MAGIC);
  k_write_u32(hdr+4,K_WAL_VERSION);
  k_write_u64(hdr+8,s.wal_meta.generation+1u);
  k_write_u32(hdr+16,4096u);
  k_write_u32(hdr+20,0u);                /* CRC over a payload that is not there */
  TEST_ASSERT(k_path_wal_segment(path,base,s.wal_meta.next.segment)==0,"segment path");
  seg=vfs_open(path);
  TEST_ASSERT(seg!=0,"segment open");
  TEST_ASSERT(vfs_write(seg,(k_u64)tail,hdr,sizeof(hdr))==0,"write partial tail record");
  vfs_close(seg);
  k_server_release(&s);
  setup(&s,1,base);
  TEST_ASSERT(k_server_open(&s)==0,"reopen: a partial tail is ignored, not fatal");
  TEST_ASSERT(elect(&s)==0,"re-elect");
  for(i=0;i<300;i++) turn(&s,20u);
  TEST_ASSERT(treap_get(s.tree,(const unsigned char*)"k",1,&val,&vlen)==1,"key recovered");
  TEST_ASSERT(vlen==1&&val&&val[0]=='b',"recovered the last COMPLETE record");
  k_server_release(&s);
  TEST_END();
}

/* ---- corruption of an ACKED record must fail-stop ----
   Only the record named by a DURABLE slot is provably acked (the slot is written and
   fsynced only at a checkpoint / rotation).  Do exactly what a checkpoint does - write
   the OTHER slot with the in-memory position and fsync it - so the durable hint names
   the newest record while the older, still valid record stays in the segment: corrupting
   the hinted record must fail-stop instead of falling back to the older one. */
static unsigned int g_walcorrupt_seq;
static void test_wal_recovery_rejects_corrupt_acked_record(void){
  k_server s;
  char base[64];
  char path[K_URI_MAX];
  char meta_path[K_URI_MAX];
  k_wal_meta_state durable,promoted;
  k_u8 slot[K_WAL_META_SLOT_SIZE];
  k_u8 frame[K_FRAME_HEADER+64];
  k_u8 garbage[4];
  k_u32 total;
  vfs_file *seg,*mf;
  raft_i64 bl,last_off;
  int i,other;
  TEST_BEGIN("server WAL recovery refuses a corrupt ACKED record (fail-stop)");
  sprintf(base,"mem://kstest-walcorrupt-%u",g_walcorrupt_seq++);
  setup(&s,1,base);
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  bl=s.last_applied;
  k_server_client_accepted(&s,(void*)(size_t)1);
  total=make_client_frame(frame,sizeof(frame),K_REQ_SET,1u,"k",1,"a",1);
  TEST_ASSERT(total>0,"SET a frame");
  k_server_client_received(s.connections,frame,total);
  apply_until(&s,bl+1);
  for(i=0;i<40;i++) turn(&s,20u);
  bl=s.last_applied;
  k_server_client_accepted(&s,(void*)(size_t)1);
  total=make_client_frame(frame,sizeof(frame),K_REQ_SET,2u,"k",1,"b",1);
  TEST_ASSERT(total>0,"SET b frame");
  k_server_client_received(s.connections,frame,total);
  apply_until(&s,bl+1);
  for(i=0;i<40;i++) turn(&s,20u);
  TEST_ASSERT(k_wal_meta_load(base,&durable)==1,"a durable slot is readable");
  promoted=s.wal_meta;                        /* the newest record, now checkpointed */
  other=(promoted.slot==0)?1:0;
  k_wal_meta_slot_encode(slot,&promoted);
  TEST_ASSERT(k_path_suffix(meta_path,base,".wal.meta")==0,"meta path");
  mf=vfs_open(meta_path);
  TEST_ASSERT(mf!=0,"meta open");
  TEST_ASSERT(vfs_write(mf,(k_u64)other*(k_u64)K_WAL_META_SLOT_GAP,slot,sizeof(slot))==0,"checkpoint slot");
  TEST_ASSERT(vfs_sync(mf)==0,"checkpoint fsync");
  vfs_close(mf);
  TEST_ASSERT(k_wal_meta_load(base,&durable)==1,"the promoted slot is the newest");
  TEST_ASSERT_I64_EQ((raft_i64)durable.record.offset,(raft_i64)s.wal_meta.record.offset,
                     "the durable slot now names the newest record");
  last_off=(raft_i64)durable.record.offset;
  TEST_ASSERT(last_off>0,"a later record exists (so the older one is still in the segment)");
  TEST_ASSERT(k_path_wal_segment(path,base,durable.record.segment)==0,"segment path");
  memset(garbage,0x5a,sizeof(garbage));
  seg=vfs_open(path);
  TEST_ASSERT(seg!=0,"segment open");
  TEST_ASSERT(vfs_write(seg,(k_u64)last_off+(k_u64)K_WAL_HEADER_SIZE,garbage,sizeof(garbage))==0,"corrupt the acked record");
  vfs_close(seg);
  k_server_release(&s);
  setup(&s,1,base);
  TEST_ASSERT(k_server_open(&s)!=0,"recovery REFUSES to open on a corrupt acked record");
  TEST_END();
}

/* ---- a corrupt record AFTER the checkpoint must fail-stop ----
   Every record is fsynced before it can be acked, so a record after the durable slot
   may still have been acknowledged: the slot only lags its checkpoint.  Reading the
   payload completely but failing the CRC is NOT a torn write (a torn write cannot be
   read to its end) - it is corruption of an acked record. */
static unsigned int g_walafter_seq;
static void test_wal_recovery_rejects_corrupt_record_after_checkpoint(void){
  k_server s;
  char base[64];
  char path[K_URI_MAX];
  k_wal_meta_state durable;
  k_u8 frame[K_FRAME_HEADER+64];
  k_u8 garbage[4];
  k_u32 total;
  vfs_file *seg;
  raft_i64 bl,victim;
  int i;
  TEST_BEGIN("server WAL recovery refuses a corrupt ACKED record after the checkpoint");
  sprintf(base,"mem://kstest-walafter-%u",g_walafter_seq++);
  setup(&s,1,base);
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  bl=s.last_applied;
  k_server_client_accepted(&s,(void*)(size_t)1);
  total=make_client_frame(frame,sizeof(frame),K_REQ_SET,1u,"k",1,"a",1);
  TEST_ASSERT(total>0,"SET a frame");
  k_server_client_received(s.connections,frame,total);
  apply_until(&s,bl+1);
  for(i=0;i<40;i++) turn(&s,20u);
  TEST_ASSERT(k_wal_meta_load(base,&durable)==1,"durable slot readable");
  TEST_ASSERT(durable.record_size>0,"the durable slot names a record");
  bl=s.last_applied;
  k_server_client_accepted(&s,(void*)(size_t)1);
  total=make_client_frame(frame,sizeof(frame),K_REQ_SET,2u,"k",1,"b",1);
  TEST_ASSERT(total>0,"SET b frame");
  k_server_client_received(s.connections,frame,total);
  apply_until(&s,bl+1);
  for(i=0;i<40;i++) turn(&s,20u);
  victim=(raft_i64)(durable.record.offset+durable.record_size);
  TEST_ASSERT(victim>(raft_i64)durable.record.offset,"a later record exists after the durable slot");
  TEST_ASSERT(k_path_wal_segment(path,base,durable.record.segment)==0,"segment path");
  memset(garbage,0x5a,sizeof(garbage));
  seg=vfs_open(path);
  TEST_ASSERT(seg!=0,"segment open");
  TEST_ASSERT(vfs_write(seg,(k_u64)victim+(k_u64)K_WAL_HEADER_SIZE,garbage,sizeof(garbage))==0,"corrupt the acked record after the checkpoint");
  vfs_close(seg);
  k_server_release(&s);
  setup(&s,1,base);
  TEST_ASSERT(k_server_open(&s)!=0,"a corrupt ACKED record after the checkpoint fails recovery");
  TEST_END();
}

/* ---- a skipped WAL generation is not a torn tail ----
   The WAL worker assigns exactly one generation per appended record, in order, so a
   record whose generation is not the previous one plus one means a record is missing.
   The copied record below keeps its (valid) payload CRC, which covers only the payload:
   the chain rule is what catches it. */
static unsigned int g_walgap_seq;
static void test_wal_recovery_rejects_missing_generation(void){
  k_server s;
  char base[64];
  char path[K_URI_MAX];
  k_wal_meta_state durable;
  k_u8 frame[K_FRAME_HEADER+64];
  k_u8 header[K_WAL_HEADER_SIZE];
  k_u8 *copy;
  k_u32 total;
  vfs_file *seg;
  raft_i64 bl,second;
  int i;
  TEST_BEGIN("server WAL recovery refuses a skipped generation");
  sprintf(base,"mem://kstest-walgap-%u",g_walgap_seq++);
  setup(&s,1,base);
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  bl=s.last_applied;
  k_server_client_accepted(&s,(void*)(size_t)1);
  total=make_client_frame(frame,sizeof(frame),K_REQ_SET,1u,"k",1,"a",1);
  TEST_ASSERT(total>0,"SET a frame");
  k_server_client_received(s.connections,frame,total);
  apply_until(&s,bl+1);
  for(i=0;i<40;i++) turn(&s,20u);
  TEST_ASSERT(k_wal_meta_load(base,&durable)==1,"durable slot readable");
  bl=s.last_applied;
  k_server_client_accepted(&s,(void*)(size_t)1);
  total=make_client_frame(frame,sizeof(frame),K_REQ_SET,2u,"k",1,"b",1);
  TEST_ASSERT(total>0,"SET b frame");
  k_server_client_received(s.connections,frame,total);
  apply_until(&s,bl+1);
  for(i=0;i<40;i++) turn(&s,20u);
  second=(raft_i64)(durable.record.offset+durable.record_size);
  TEST_ASSERT(k_path_wal_segment(path,base,durable.record.segment)==0,"segment path");
  seg=vfs_open(path);
  TEST_ASSERT(seg!=0,"segment open");
  {
    k_u64 size;
    TEST_ASSERT(vfs_read(seg,(k_u64)second,header,sizeof(header))==0,"read the second record header");
    size=(k_u64)K_WAL_HEADER_SIZE+(k_u64)k_read_u32(header+16);
    copy=(k_u8 *)malloc((size_t)size);
    TEST_ASSERT(copy!=0,"buffer");
    TEST_ASSERT(vfs_read(seg,(k_u64)second,copy,(k_u32)size)==0,"read the second record");
    k_write_u64(copy+8,k_read_u64(header+8)+5u);     /* skip five generations */
    TEST_ASSERT(vfs_write(seg,(k_u64)second+size,copy,(k_u32)size)==0,"append the tampered record");
    free(copy);
  }
  vfs_close(seg);
  k_server_release(&s);
  setup(&s,1,base);
  TEST_ASSERT(k_server_open(&s)!=0,"a skipped generation fails recovery");
  TEST_END();
}

/* ---- recovery across a real segment rotation ----
   Rotation fsyncs the slot naming the NEW segment, so recovery must start in that
   segment and still find the newest record. */
static unsigned int g_walrot_seq;
static void test_wal_recovery_across_segment_rotation(void){
  k_server s;
  char base[64];
  const unsigned char *val=0;
  unsigned int vlen=0;
  k_u8 frame[K_FRAME_HEADER+64];
  k_u32 total;
  raft_i64 bl;
  int i;
  TEST_BEGIN("server WAL recovery across a real segment rotation");
  sprintf(base,"mem://kstest-walrot-%u",g_walrot_seq++);
  setup(&s,1,base);
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  s.cfg.wal_seg_size=900u;               /* rotate every few records */
  for(i=0;i<40;i++){
    char key[8];
    sprintf(key,"k%d",i);
    bl=s.last_applied;
    k_server_client_accepted(&s,(void*)(size_t)1);
    total=make_client_frame(frame,sizeof(frame),K_REQ_SET,1u,key,(k_u32)strlen(key),"v",1);
    TEST_ASSERT(total>0,"SET frame");
    k_server_client_received(s.connections,frame,total);
    apply_until(&s,bl+1);
    for(bl=0;bl<3;bl++) turn(&s,20u);
  }
  for(i=0;i<40;i++) turn(&s,20u);
  TEST_ASSERT(s.wal_meta.next.segment>0,"the WAL actually rotated segments");
  k_server_release(&s);
  setup(&s,1,base);
  TEST_ASSERT(k_server_open(&s)==0,"reopen: recover across segments");
  TEST_ASSERT(elect(&s)==0,"re-elect");
  for(i=0;i<300;i++) turn(&s,20u);
  /* WAL records carry only the DELTA now, so recovery has to stitch many records (and
     several segments) back together: every key of the run must be there, not just the
     newest one. */
  {
    int missing=0;
    for(i=0;i<40;i++){
      char key[8];
      sprintf(key,"k%d",i);
      if(treap_get(s.tree,(const unsigned char*)key,(unsigned int)strlen(key),&val,&vlen)!=1) missing++;
    }
    TEST_ASSERT(missing==0,"every key replayed across records/segments");
  }
  TEST_ASSERT(treap_get(s.tree,(const unsigned char*)"k39",3,&val,&vlen)==1,"newest key recovered");
  TEST_ASSERT(treap_get(s.tree,(const unsigned char*)"k0",2,&val,&vlen)==1,"first key recovered (log replayed)");
  k_server_release(&s);
  TEST_END();
}

/* ---- the newest segment that holds acknowledged records is gone ----
   Records are fsynced as they are written, but the metadata slot only every 64 records (and at a segment
   switch), so the slot cannot witness everything: the segments are the only authority on what was acked.  If
   the segment holding the newest acknowledged record is missing, recovery must REFUSE - continuing on top of
   this store would write over, and silently outlive, whatever that segment held (review 2.2).  Before the fix
   this recovered the older records, pulled the write position back and carried on. */
static unsigned int g_walmiss_seq;
static unsigned int g_walceil_seq;
/* ---- the scan ceiling must never turn a healthy store into an empty one (fourth-round review A1)
   Snapshot cleanup unlinks whole segments below the snapshot base, so a long-lived store can start with a released
   prefix.  The walk needs a ceiling, but reaching it while nothing was read and the metadata IS present means the
   store has acknowledged records: the old code returned success there, so such a store came back as an EMPTY state
   machine - silently, with its metadata present, and again on every later restart.  The scan now jumps to the tail
   around the write position, and if that finds nothing it refuses loudly.  This case releases the prefix by hand
   (exactly what cleanup does) and insists the store does not come back empty. */
/* ---- a cut in the MIDDLE of the WAL must be fail-stop, not a silent truncation (fourth-round review A2/A3)
   The auditors argued that a missing segment with records after it slips past the acknowledged-record check,
   because that check compares against a metadata slot that is only fsynced every K_WAL_META_FSYNC_EVERY records.
   Tracing says otherwise, and this case pins it: the slot is ALSO synced on every segment change, so its record
   always sits in the newest segment and any cut before that is caught.  Unlink one middle segment here (both sides
   keep their records) and the store must refuse, naming both positions - a silent truncation would drop every
   record after the cut.  The property, not a fix: this case is green before and after any change. */
/* ---- an install must replace a stale longer file, not inherit its tail (fourth-round review A6)
   The install writes straight to the versioned file, so a previous, longer incarnation of the same index would
   otherwise survive past the new end and be loaded later as part of the new snapshot - the save path already
   probes for exactly that.  Here a 128-byte stale file is created first, a 20-byte install runs over it, and
   nothing may be readable at offset 20 afterwards. */
static void test_install_snapshot_discards_a_stale_longer_file(void){
  k_server s;
  char base[64],path[K_URI_MAX];
  char filler[128];
  k_u8 payload[20],probe;
  int rc;
  raft_install_snapshot ins;
  vfs_file *f;
  TEST_BEGIN("server InstallSnapshot replaces a stale longer file instead of keeping its tail");
  sprintf(base,"mem://kstest-snapinst-%u",g_walceil_seq++);
  setup(&s,1,base);
  TEST_ASSERT(k_server_open(&s)==0,"open");
  memset(filler,'x',sizeof(filler));
  TEST_ASSERT(k_path_snapshot(path,base,7)==0,"snapshot path");
  f=vfs_open(path);
  TEST_ASSERT(f!=0,"create the stale file");
  TEST_ASSERT(vfs_write(f,0,filler,sizeof(filler))==0,"write 128 stale bytes");
  vfs_sync(f);
  vfs_close(f);
  memset(&ins,0,sizeof(ins));
  memset(payload,'y',sizeof(payload));
  ins.snapshot_last_index=7;
  ins.snapshot_offset=0;
  ins.snapshot_data=payload;
  ins.snapshot_chunk_size=(k_u32)sizeof(payload);
  ins.snapshot_done=1;
  /* The stale file is LONGER than the install, so the install must not adopt its tail: it refuses and discards
     the file (a mixed file may never survive to be loaded later).  Either way, no byte of the old incarnation
     may remain. */
  rc=k_server_write_inbound_snapshot(&s,&ins);
  TEST_ASSERT(rc!=0,"an install shorter than the file already there is refused, not silently trimmed");
  f=vfs_open(path);
  TEST_ASSERT(f!=0,"reopen the path");
  TEST_ASSERT(vfs_read(f,0,&probe,1u)!=0,"the stale incarnation is gone, not left behind as a tail");
  vfs_close(f);
  k_server_release(&s);
  TEST_END();
}

static void test_wal_recovery_refuses_a_middle_segment_cut(void){
  k_server s;
  char base[64],path[K_URI_MAX],key[16];
  k_u8 frame[K_FRAME_HEADER+64];
  k_u32 total;
  raft_i64 bl;
  k_u64 cut;
  int i,rc;
  TEST_BEGIN("server WAL recovery refuses a cut in the middle of the log (fail-stop)");
  sprintf(base,"mem://kstest-midcut-%u",g_walceil_seq++);
  setup(&s,1,base);
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  s.cfg.wal_seg_size=900u;                      /* rotate every few records */
  for(i=0;i<60;i++){
    sprintf(key,"k%d",i);
    bl=s.last_applied;
    k_server_client_accepted(&s,(void*)(size_t)1);
    total=make_client_frame(frame,sizeof(frame),K_REQ_SET,1u,key,(k_u32)strlen(key),"v",1);
    TEST_ASSERT(total>0,"SET frame");
    k_server_client_received(s.connections,frame,total);
    apply_until(&s,bl+1);
    for(bl=0;bl<3;bl++) turn(&s,20u);
  }
  TEST_ASSERT(s.wal_meta.next.segment>3u,"several segments were written");
  cut=s.wal_meta.next.segment/2u;              /* a middle one: records exist on both sides */
  k_server_release(&s);
  TEST_ASSERT(k_path_wal_segment(path,base,cut)==0,"cut path");
  TEST_ASSERT(vfs_unlink(path)==0,"remove one middle segment");
  setup(&s,1,base);
  rc=k_server_open(&s);
  if(rc==0) k_server_release(&s);
  TEST_ASSERT(rc!=0,"a middle-segment cut must fail-stop instead of silently truncating the log");
  TEST_END();
}

static void test_wal_recovery_refuses_a_prefix_past_the_scan_ceiling(void){
  k_server s;
  char base[64],path[K_URI_MAX],key[16];
  const unsigned char *val=0;
  unsigned int vlen=0;
  k_u8 frame[K_FRAME_HEADER+64];
  k_u32 total;
  raft_i64 bl;
  k_u64 seg;
  unsigned int released=0u;
  int i,rc,ok=0;
  TEST_BEGIN("server WAL recovery refuses to start empty behind a released prefix past the scan ceiling");
  sprintf(base,"mem://kstest-walceil-%u",g_walceil_seq++);
  setup(&s,1,base);
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  s.cfg.wal_seg_size=900u;                        /* rotate every few records */
  for(i=0;i<4000&&s.wal_meta.next.segment<=K_WAL_SCAN_EMPTY_PREFIX_MAX+1u;i++){
    sprintf(key,"k%d",i);
    bl=s.last_applied;
    k_server_client_accepted(&s,(void*)(size_t)1);
    total=make_client_frame(frame,sizeof(frame),K_REQ_SET,1u,key,(k_u32)strlen(key),"v",1);
    TEST_ASSERT(total>0,"SET frame");
    k_server_client_received(s.connections,frame,total);
    apply_until(&s,bl+1);
    for(bl=0;bl<3;bl++) turn(&s,20u);
  }
  for(bl=0;bl<40;bl++) turn(&s,20u);
  TEST_ASSERT(s.wal_meta.next.segment>K_WAL_SCAN_EMPTY_PREFIX_MAX,"the WAL really passed the scan ceiling");
  k_server_release(&s);
  for(seg=0;seg<=K_WAL_SCAN_EMPTY_PREFIX_MAX;seg++){
    if(k_path_wal_segment(path,base,seg)==0&&vfs_unlink(path)==0) released++;
  }
  TEST_ASSERT(released>0,"the prefix really was released");
  setup(&s,1,base);
  rc=k_server_open(&s);
  if(rc==0){
    /* Recovering is fine; coming back EMPTY is the defect.  The newest key is the one the tail scan would hold. */
    ok=treap_get(s.tree,(const unsigned char*)key,(k_u32)strlen(key),&val,&vlen)==1;
    k_server_release(&s);
  }
  TEST_ASSERT(rc!=0||ok,"a released prefix past the ceiling must not produce a silent empty store");
  TEST_END();
}

static void test_wal_recovery_refuses_missing_newest_segment(void){
  k_server s;
  char base[64],path[K_URI_MAX];
  const unsigned char *val=0;
  unsigned int vlen=0;
  k_u8 frame[K_FRAME_HEADER+64];
  k_u32 total;
  raft_i64 bl;
  k_u64 newest;
  int i;
  TEST_BEGIN("server WAL recovery refuses a deleted newest segment (fail-stop)");
  sprintf(base,"mem://kstest-walmiss-%u",g_walmiss_seq++);
  setup(&s,1,base);
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  s.cfg.wal_seg_size=900u;                 /* rotate every few records */
  for(i=0;i<40;i++){
    char key[8];
    sprintf(key,"k%d",i);
    bl=s.last_applied;
    k_server_client_accepted(&s,(void*)(size_t)1);
    total=make_client_frame(frame,sizeof(frame),K_REQ_SET,1u,key,(k_u32)strlen(key),"v",1);
    TEST_ASSERT(total>0,"SET frame");
    k_server_client_received(s.connections,frame,total);
    apply_until(&s,bl+1);
    for(bl=0;bl<3;bl++) turn(&s,20u);
  }
  for(i=0;i<40;i++) turn(&s,20u);
  TEST_ASSERT(s.wal_meta.next.segment>1,"several segments were written");
  TEST_ASSERT(treap_get(s.tree,(const unsigned char*)"k0",2,&val,&vlen)==1,"the run really landed in the tree");
  TEST_ASSERT(treap_get(s.tree,(const unsigned char*)"k39",3,&val,&vlen)==1,"the newest key landed in the tree");
  newest=(s.wal_meta.next.offset>0)?s.wal_meta.next.segment:(s.wal_meta.next.segment-1u);
  k_server_release(&s);
  TEST_ASSERT(k_path_wal_segment(path,base,newest)==0,"segment path");
  TEST_ASSERT(vfs_unlink(path)==0,"remove the newest segment that holds records");
  setup(&s,1,base);
  TEST_ASSERT(k_server_open(&s)!=0,"a store whose newest acknowledged segment is gone refuses to open");
  TEST_END();
}

/* ---- snapshot file validation and retention ----
   A snapshot is written straight to its versioned file and verified by streaming CRC
   before anything is trusted or unlinked (no temp file + rename): a torn, truncated or
   bit-rotted file must be rejected, and the previous snapshot must survive a newer one
   so it can still be used as a rollback target. */
static unsigned int g_snapver_seq;
static void test_snapshot_file_verify_and_retention(void){
  char base[64];
  k_u8 buf[64];
  k_u32 crc;
  vfs_file *f;
  TEST_BEGIN("server snapshot: file verification rejects damage, keeps the previous base");
  sprintf(base,"mem://kstest-snapver-%u",g_snapver_seq++);
  /* a well-formed snapshot image: 12 bytes of payload + CRC trailer */
  memset(buf,0x11,12u);
  k_crc32(buf,12u,&crc);
  k_write_u32(buf+12,crc);
  {
    char spath[K_URI_MAX];
    TEST_ASSERT(k_path_snapshot(spath,base,7)==0,"snapshot path");
    f=vfs_open(spath);
    TEST_ASSERT(f!=0,"create snapshot file");
    TEST_ASSERT(vfs_write(f,0,buf,16u)==0,"write image");
    vfs_close(f);
    TEST_ASSERT(k_snapshot_verify_file(base,7)==1,"a well-formed image verifies");
    /* one flipped byte in the payload must be rejected */
    f=vfs_open(spath);
    buf[3]^=0x40;
    TEST_ASSERT(vfs_write(f,3,buf+3,1u)==0,"flip a payload byte");
    vfs_close(f);
    TEST_ASSERT(k_snapshot_verify_file(base,7)==0,"a damaged image is rejected");
    /* a truncated image (trailer replaced by zeros) must be rejected too */
    f=vfs_open(spath);
    memset(buf,0,4u);
    TEST_ASSERT(vfs_write(f,12u,buf,4u)==0,"zero the trailer");
    vfs_close(f);
    TEST_ASSERT(k_snapshot_verify_file(base,7)==0,"a truncated image is rejected");
    /* an absent file is simply "not usable" (never an error) */
    TEST_ASSERT(k_snapshot_verify_file(base,8)==0,"an absent snapshot is not usable");
    TEST_ASSERT(k_snapshot_verify_file(base,0)==1,"a zero base needs no snapshot");
  }
  TEST_END();
}
/* The write gate is held closed by an in-flight FCALL.  If that request dies without a
   terminal result - the client that owned it disconnected, the connection was reaped - the
   gate must reopen, otherwise every later write from every client queues forever with no
   answer and no redirect (the client then waits on a request nobody will ever complete). */
static void test_fcall_gate_reopens_when_its_request_dies(void){
  k_server s;
  k_request *request;
  TEST_BEGIN("server: freeing the FCALL that holds the write gate reopens it");
  setup(&s,1,"mem://kstest-gate-reopen");
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  request=k_request_create_server(&s,0,77u,K_REQ_FCALL,(const k_u8 *)"f",1u,0,0,1u);
  TEST_ASSERT(request!=0,"fcall request created");
  s.gate_closed=1;                      /* the FCALL is between fork and commit */
  k_request_free(&s,request);           /* ... and its client is gone */
  TEST_ASSERT_I64_EQ(s.gate_closed,0,"gate reopened when the FCALL died");
  k_server_release(&s);
  TEST_END();
}
static void test_single_node_elect(void){
  k_server s;
  TEST_BEGIN("server single-node self-elect (sync runtime)");
  setup(&s,1,"mem://kstest-elect");
  TEST_ASSERT(k_server_open(&s)==0,"open ok");
  TEST_ASSERT(elect(&s)==0,"became leader");
  TEST_ASSERT_I64_EQ(s.is_leader,1,"is_leader set");
  TEST_ASSERT_I64_EQ(s.leader_id,1,"leader_id is self");
  k_server_release(&s);
  TEST_END();
}

static void test_single_node_set_apply(void){
  k_server s;
  const unsigned char *val=0;
  unsigned int vlen=0;
  k_u8 frame[K_FRAME_HEADER+64];
  k_u32 total;
  k_conn *conn;
  raft_i64 baseline;
  TEST_BEGIN("server single-node SET applies to state machine");
  setup(&s,1,"mem://kstest-set");
  TEST_ASSERT(k_server_open(&s)==0,"open ok");
  TEST_ASSERT(elect(&s)==0,"leader");
  baseline=s.last_applied;  /* the leader's own NOOP already applied */
  /* admit a client connection and feed a SET frame */
  k_server_client_accepted(&s,(void*)(size_t)1);
  conn=s.connections;
  TEST_ASSERT(conn!=0,"client conn admitted");
  total=make_client_frame(frame,sizeof(frame),K_REQ_SET,1u,"k",1,"v",1);
  TEST_ASSERT(total>0,"frame built");
  gcap.send_count=0;
  k_server_client_received(conn,frame,total);
  apply_until(&s,baseline+1);
  /* the COMMITTED client result flushes one tick after the apply */
  turn(&s,50u);
  /* content-level oracle: the state machine holds the write */
  TEST_ASSERT(treap_get(s.tree,(const unsigned char*)"k",1,&val,&vlen)==1,"key present");
  TEST_ASSERT(vlen==1&&val&&val[0]=='v',"value == v");
  /* the leader replied with an OK response frame (captured on transport) */
  TEST_ASSERT(gcap.send_count>=1,"an outbound frame was sent");
  TEST_ASSERT_I64_EQ(gcap.last_magic,K_CLIENT_MAGIC,"response magic");
  TEST_ASSERT_I64_EQ(gcap.last_type,K_RESPONSE,"response type");
  k_server_release(&s);
  TEST_END();
}

static void test_single_node_get_reads_back(void){
  k_server s;
  k_u8 frame[K_FRAME_HEADER+256];
  k_u32 total;
  k_conn *conn;
  raft_i64 baseline;
  k_response_data resp;
  int i;
  TEST_BEGIN("server single-node GET reads back the value");
  setup(&s,1,"mem://kstest-get");
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  baseline=s.last_applied;
  k_server_client_accepted(&s,(void*)(size_t)1);
  conn=s.connections;
  /* SET k=v, flush it */
  total=make_client_frame(frame,sizeof(frame),K_REQ_SET,1u,"k",1,"v",1);
  TEST_ASSERT(total>0,"SET frame");
  k_server_client_received(conn,frame,total);
  apply_until(&s,baseline+1);
  turn(&s,50u);
  /* GET k -> response body must be the value bytes */
  gcap.send_count=0;
  total=make_client_frame(frame,sizeof(frame),K_REQ_GET,2u,"k",1,0,0);
  TEST_ASSERT(total>0,"GET frame");
  k_server_client_received(conn,frame,total);
  for(i=0;i<100&&gcap.send_count==0;i++) turn(&s,50u);
  TEST_ASSERT(gcap.send_count>=1,"GET response sent");
  TEST_ASSERT(k_response_decode(&resp,gcap.last_payload,gcap.last_size)==0,"response decodes");
  TEST_ASSERT_I64_EQ(resp.status,K_STATUS_OK,"status ok");
  TEST_ASSERT(resp.body_size==1&&resp.body&&resp.body[0]=='v',"body == v");
  k_response_data_free(&resp);
  k_server_release(&s);
  TEST_END();
}

static unsigned int g_crash_seq;
static void test_single_node_crash_restart(void){
  k_server s;
  const unsigned char *val=0; unsigned int vlen=0;
  char base[64];
  k_u8 frame[K_FRAME_HEADER+64]; k_u32 total;
  raft_i64 bl; int i;
  TEST_BEGIN("server single-node crash/restart recovers state");
  sprintf(base,"mem://kstest-crash-%u",g_crash_seq++);
  setup(&s,1,base);
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  bl=s.last_applied;
  k_server_client_accepted(&s,(void*)(size_t)1);
  total=make_client_frame(frame,sizeof(frame),K_REQ_SET,1u,"k",1,"v",1);
  TEST_ASSERT(total>0,"SET frame");
  k_server_client_received(s.connections,frame,total);
  apply_until(&s,bl+1);
  turn(&s,50u);
  for(i=0;i<40;i++) turn(&s,20u);  /* let the WAL settle durably */
  k_server_release(&s);            /* crash */
  /* reboot == a fresh process: zero the WHOLE struct (clears run-state flags
     like stopped/fatal that release does NOT reset) + init + re-inject seams.
     release alone + re-set fields is only safe for a HEALTHY node; a node that
     OOM-stopped would reopen still marked stopped. */
  setup(&s,1,base);
  TEST_ASSERT(k_server_open(&s)==0,"reopen (recover)");
  TEST_ASSERT(elect(&s)==0,"re-elect");
  for(i=0;i<300;i++) turn(&s,20u);  /* replay the WAL back into the tree */
  TEST_ASSERT(treap_get(s.tree,(const unsigned char*)"k",1,&val,&vlen)==1,"key recovered after restart");
  TEST_ASSERT(vlen==1&&val&&val[0]=='v',"value recovered after restart");
  k_server_release(&s);
  TEST_END();
}

/* ---- disk:// durability: real files, must be cleaned up ---- */
static unsigned int g_disk_seq;
/* Disk-backed tests must not reuse a basename across RUNS: the counter restarts at
   0 in every process and the vfs disk backend opens with OPEN_ALWAYS (no
   truncate), so a run killed mid-test would leave files the next run silently
   recovers from.  Tag every basename with the process start time. */
static k_u64 g_run_tag;
static void disk_cleanup(const char *base){
  char path[K_URI_MAX];
  k_u64 seg;
  if(k_path_suffix(path,base,".cfg")==0) vfs_unlink(path);
  if(k_path_suffix(path,base,".wal.meta")==0) vfs_unlink(path);
  /* EVERY segment, not just segment 0: a store that rotated (small wal_seg_size, or many
     records) leaves 1, 2, ... behind, and those used to survive the test - nine of them were
     committed by `git add -A` for exactly this reason.  The bound is the same ceiling recovery
     uses for a released prefix, which is far above anything a test writes. */
  for(seg=0;seg<K_WAL_SCAN_EMPTY_PREFIX_MAX;seg++){
    if(k_path_wal_segment(path,base,seg)!=0) break;
    vfs_unlink(path);
  }
}

static void test_single_node_get_missing(void){
  k_server s;
  k_u8 frame[K_FRAME_HEADER+64];
  k_u32 total;
  k_conn *conn;
  k_response_data resp;
  int i;
  TEST_BEGIN("server GET missing key -> NOT_FOUND");
  setup(&s,1,"mem://kstest-missing");
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  k_server_client_accepted(&s,(void*)(size_t)1);
  conn=s.connections;
  gcap.send_count=0;
  total=make_client_frame(frame,sizeof(frame),K_REQ_GET,1u,"nope",4,0,0);
  TEST_ASSERT(total>0,"GET frame");
  k_server_client_received(conn,frame,total);
  for(i=0;i<100&&gcap.send_count==0;i++) turn(&s,50u);
  TEST_ASSERT(gcap.send_count>=1,"GET response sent");
  TEST_ASSERT(k_response_decode(&resp,gcap.last_payload,gcap.last_size)==0,"response decodes");
  TEST_ASSERT_I64_EQ(resp.status,K_STATUS_NOT_FOUND,"status not-found");
  k_response_data_free(&resp);
  k_server_release(&s);
  TEST_END();
}

static void test_malformed_frame_closes(void){
  k_server s;
  k_conn *conn;
  k_u8 bad[K_FRAME_HEADER];
  TEST_BEGIN("server malformed frame (wrong magic) closes connection");
  setup(&s,1,"mem://kstest-malformed");
  TEST_ASSERT(k_server_open(&s)==0,"open");
  k_server_client_accepted(&s,(void*)(size_t)1);
  conn=s.connections;
  gcap.close_calls=0;
  memset(bad,0,sizeof(bad));
  k_frame_header_build(bad,K_PEER_MAGIC,K_REQ_GET,0u);   /* client expects K_CLIENT_MAGIC */
  k_server_client_received(conn,bad,K_FRAME_HEADER);
  TEST_ASSERT_I64_EQ(gcap.close_calls,1,"connection closed on bad magic");
  k_server_release(&s);
  TEST_END();
}

/* A batch list (MSET/MDEL/MGET) that does not parse - bogus count, truncated
   item, over-long key - must be rejected at the FRAME layer.  It used to be
   length-checked only ("the list has >= 4 bytes"), so a count of 0xffffffff
   replicated as a write and answered OK, then failed to apply and flagged the
   node fatal: a malformed frame from an unauthenticated client could take the
   node down.  A well-formed batch must still be accepted. */
static void test_malformed_batch_frame_rejected(void){
  k_server s;
  k_conn *conn;
  k_u8 frame[K_FRAME_HEADER+64];
  k_buf b;
  k_u32 total;
  k_response_data resp;
  int i;
  TEST_BEGIN("server malformed batch frame (bogus count) rejected");
  setup(&s,1,"mem://kstest-badb");
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  k_server_client_accepted(&s,(void*)(size_t)1);
  conn=s.connections;
  TEST_ASSERT(conn!=0,"client conn admitted");
  /* count = 0xffffffff with no items at all */
  memset(&b,0,sizeof(b));
  k_buf_u32(&b,0xFFFFFFFFu);
  total=make_client_frame(frame,sizeof(frame),K_REQ_MSET,1u,b.data,b.len,0,0);
  k_buf_free(&b);
  TEST_ASSERT(total>0,"MSET frame built");
  gcap.send_count=0;
  k_server_client_received(conn,frame,total);
  TEST_ASSERT_I64_EQ(gcap.send_count,0,"no response for a rejected batch frame");
  TEST_ASSERT_I64_EQ(s.fatal,0,"server did not go fatal");
  /* a valid batch on a FRESH connection must still be accepted (the rejected frame
     closed its connection, like the bad-magic case) */
  k_server_client_accepted(&s,(void*)(size_t)2);
  conn=s.connections;
  TEST_ASSERT(conn!=0,"second client conn admitted");
  memset(&b,0,sizeof(b));
  k_buf_u32(&b,1u);                              /* count */
  k_buf_u32(&b,1u); k_buf_u8(&b,(k_u8)'m');      /* key "m" */
  k_buf_u32(&b,1u); k_buf_u8(&b,(k_u8)'v');      /* value "v" */
  total=make_client_frame(frame,sizeof(frame),K_REQ_MSET,2u,b.data,b.len,0,0);
  k_buf_free(&b);
  TEST_ASSERT(total>0,"well-formed MSET frame built");
  gcap.send_count=0;
  k_server_client_received(conn,frame,total);
  for(i=0;i<50&&gcap.send_count==0;i++) turn(&s,50u);
  TEST_ASSERT(gcap.send_count>=1,"well-formed batch answered");
  TEST_ASSERT(k_response_decode(&resp,gcap.last_payload,gcap.last_size)==0,"response decodes");
  TEST_ASSERT_I64_EQ(resp.status,K_STATUS_OK,"batch status ok");
  TEST_ASSERT(resp.body_size==1&&resp.body[0]=='1',"batch reports 1 affected key");
  k_response_data_free(&resp);
  TEST_ASSERT_I64_EQ(s.fatal,0,"still not fatal");
  k_server_release(&s);
  TEST_END();
}

static void test_admission_gate_rejects(void){
  k_server s;
  k_u8 frame[K_FRAME_HEADER+64];
  k_u32 total;
  k_conn *conn;
  k_response_data resp;
  TEST_BEGIN("server admission gate rejects writes when stopping");
  setup(&s,1,"mem://kstest-admission");
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  k_server_client_accepted(&s,(void*)(size_t)1);
  conn=s.connections;
  s.admission=0;   /* begin stop: non-INFO/STATS/HELP requests are rejected */
  gcap.send_count=0;
  total=make_client_frame(frame,sizeof(frame),K_REQ_SET,1u,"k",1,"v",1);
  TEST_ASSERT(total>0,"SET frame");
  k_server_client_received(conn,frame,total);
  TEST_ASSERT(gcap.send_count>=1,"rejection response sent synchronously");
  TEST_ASSERT(k_response_decode(&resp,gcap.last_payload,gcap.last_size)==0,"response decodes");
  TEST_ASSERT_I64_EQ(resp.status,K_STATUS_ERROR,"status error");
  k_response_data_free(&resp);
  k_server_release(&s);
  TEST_END();
}

static void test_disk_durability(void){
  k_server s;
  const unsigned char *val=0; unsigned int vlen=0;
  char base[64];
  k_u8 frame[K_FRAME_HEADER+64]; k_u32 total;
  raft_i64 bl; int i;
  TEST_BEGIN("server disk:// crash/restart recovers from real disk");
  sprintf(base,"disk://kstest-disk-%" K_U64_FMT "-%u",g_run_tag,g_disk_seq++);
  disk_cleanup(base);              /* pre-clean: never inherit a previous run's files */
  setup(&s,1,base);
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  bl=s.last_applied;
  k_server_client_accepted(&s,(void*)(size_t)1);
  total=make_client_frame(frame,sizeof(frame),K_REQ_SET,1u,"k",1,"v",1);
  TEST_ASSERT(total>0,"SET frame");
  k_server_client_received(s.connections,frame,total);
  apply_until(&s,bl+1);
  turn(&s,50u);
  for(i=0;i<40;i++) turn(&s,20u);   /* let the WAL settle durably to disk */
  /* The WAL worker holds its segment + meta handles across batches (opening and
     closing them per batch cost as much as the fsyncs).  Assert the cache is live
     and that it is released on stop - the second assertion is the leak test. */
  TEST_ASSERT(s.wal_worker.open_files>0,"WAL worker holds its file handles across batches");
  k_server_release(&s);             /* crash */
  TEST_ASSERT_I64_EQ(s.wal_worker.open_files,0,"WAL handles released when the worker stops");
  setup(&s,1,base);                 /* reboot == fresh process */
  TEST_ASSERT(k_server_open(&s)==0,"reopen (recover from disk)");
  TEST_ASSERT(elect(&s)==0,"re-elect");
  for(i=0;i<300;i++) turn(&s,20u);  /* replay the WAL back from disk */
  TEST_ASSERT(treap_get(s.tree,(const unsigned char*)"k",1,&val,&vlen)==1,"key recovered from disk");
  TEST_ASSERT(vlen==1&&val&&val[0]=='v',"value recovered from disk");
  k_server_release(&s);
  disk_cleanup(base);
  TEST_END();
}

/* Peer frames that cannot be sent used to disappear silently: the transport result
   was ignored and a missing peer socket returned early, so a half-open or saturated
   link surfaced only as a follower timing out much later.  The drop is counted now. */
/* Structural group-commit policy: a sustained stream accumulates up to the batch
   target instead of being flushed every event round, and a round that saw no new
   input submits whatever is pending - so a lone write is never delayed to fill a
   batch, and batch size stays observable via flush_batches/flush_writes_total. */
static void test_group_commit_policy(void){
  k_server s;
  k_u8 frame[K_FRAME_HEADER+64];
  k_u32 total;
  k_conn *conn;
  int i;
  TEST_BEGIN("server: group-commit submits at the target or when the round drains");
  setup(&s,1,"mem://kstest-groupcommit");
  TEST_ASSERT(k_server_open(&s)==0,"open ok");
  TEST_ASSERT(elect(&s)==0,"leader");
  k_server_client_accepted(&s,(void*)(size_t)1);
  conn=s.connections;
  TEST_ASSERT(conn!=0,"client conn admitted");
  s.cfg.flush_item_limit=4;   /* small target: keeps the test deterministic */
  for(i=0;i<3;i++){
    total=make_client_frame(frame,sizeof(frame),K_REQ_SET,(k_u32)(1+i),"k",1,"v",1);
    TEST_ASSERT(total>0,"frame built");
    k_server_client_received(conn,frame,total);
  }
  TEST_ASSERT_I64_EQ(s.write_count,3,"three writes batched, none submitted yet");
  TEST_ASSERT_I64_EQ(s.flush_batches,0,"nothing submitted below the target");
  TEST_ASSERT_I64_EQ(k_server_flush_if_ready(&s,1),0,"below target with new input: hold the batch");
  TEST_ASSERT_I64_EQ(s.write_count,3,"batch still pending");
  TEST_ASSERT_I64_EQ(k_server_flush_if_ready(&s,0),1,"drained round: submit the pending batch");
  TEST_ASSERT_I64_EQ(s.write_count,0,"batch handed to the WAL");
  TEST_ASSERT_I64_EQ(s.flush_batches,1,"exactly one batch");
  TEST_ASSERT_I64_EQ(s.flush_writes_total,3,"carrying all three writes");
  for(i=0;i<4;i++){   /* the 4th pending write reaches the target on its own */
    total=make_client_frame(frame,sizeof(frame),K_REQ_SET,(k_u32)(20+i),"k",1,"v",1);
    TEST_ASSERT(total>0,"frame built");
    k_server_client_received(conn,frame,total);
  }
  TEST_ASSERT_I64_EQ(s.flush_batches,2,"reaching the target submits without waiting for a drain");
  TEST_ASSERT_I64_EQ(s.flush_writes_total,7,"4+3 writes across two batches");
  k_server_release(&s);
  TEST_END();
}

static void test_peer_send_drop_counted(void){
  k_server s;
  k_ready_message msg;
  static k_u8 payload[8];
  TEST_BEGIN("server: an undeliverable peer frame is counted, not silently dropped");
  setup(&s,1,"mem://kstest-senddrop");
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  TEST_ASSERT_I64_EQ(s.peer_send_drops,0,"no drops before the test");
  memset(&msg,0,sizeof(msg));
  msg.to=2;                                  /* not a member of this 1-node cluster */
  msg.control=1;
  msg.payload=payload;
  msg.size=(k_u32)sizeof(payload);
  TEST_ASSERT_I64_EQ(k_server_send_ready_message(&s,&msg),-1,"frame to a non-member reports failure");
  TEST_ASSERT_I64_EQ(s.peer_send_drops,1,"the drop is counted");
  /* a MEMBER whose peer socket is missing is the realistic case (a half-open link:
     dialed but the peer's HELLO never arrived, so peer_socks is still 0) */
  msg.to=1;
  TEST_ASSERT_I64_EQ(k_server_send_ready_message(&s,&msg),-1,"frame to an unconnected member reports failure");
  TEST_ASSERT_I64_EQ(s.peer_send_drops,2,"both drop paths are counted");
  k_server_release(&s);
  TEST_END();
}

static void test_peer_encode_splits_large_append(void){
  static k_u8 data[(K_VALUE_MAX+9u)*2u];
  raft_peer_message msg;
  raft_i64 terms[2];
  unsigned int sizes[2];
  k_decoded_peer decoded;
  k_buf encoded;
  int rc;
  TEST_BEGIN("server: oversized AppendEntries is split at a frame-safe entry boundary");
  memset(&msg,0,sizeof(msg));
  memset(data,0x5a,sizeof(data));
  terms[0]=1;
  terms[1]=1;
  sizes[0]=K_VALUE_MAX+9u;
  sizes[1]=K_VALUE_MAX+9u;
  msg.type=RAFT_MSG_APPEND;
  msg.from=1;
  msg.to=2;
  msg.term=1;
  msg.append_entries.term=1;
  msg.append_entries.leader_id=1;
  msg.append_entries.read_context=K_U64_C(0x123456789abcdef0);
  msg.append_entries.entry_count=2;
  msg.append_entries.entry_terms=terms;
  msg.append_entries.entry_data_sizes=sizes;
  msg.append_entries.entry_data=data;
  rc=k_peer_encode(&msg,&encoded);
  TEST_ASSERT_I64_EQ(rc,0,"encoder emits a bounded prefix instead of failing the Ready");
  if(rc==0){
    TEST_ASSERT(encoded.len<=K_FRAME_MAX,"encoded peer payload respects K_FRAME_MAX");
    TEST_ASSERT_I64_EQ(k_peer_decode(&decoded,encoded.data,encoded.len),0,"bounded prefix decodes");
    if(decoded.message.type==RAFT_MSG_APPEND){
      TEST_ASSERT_I64_EQ(decoded.message.append_entries.entry_count,1,"only the first fitting entry is emitted");
      TEST_ASSERT_U64_EQ(decoded.message.append_entries.read_context,K_U64_C(0x123456789abcdef0),"ReadIndex context survives the wire codec");
      k_decoded_peer_free(&decoded);
    }
    k_buf_free(&encoded);
  }
  TEST_END();
}

static void test_client_response_failure_closes_connection(void){
  k_server s;
  k_u8 frame[K_FRAME_HEADER+64];
  k_u32 total;
  raft_i64 baseline;
  TEST_BEGIN("server: failed committed response closes the client connection");
  setup(&s,1,"mem://kstest-response-fail");
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  baseline=s.last_applied;
  k_server_client_accepted(&s,(void*)(size_t)1);
  total=make_client_frame(frame,sizeof(frame),K_REQ_SET,1u,"k",1,"v",1);
  TEST_ASSERT(total>0,"SET frame");
  gcap.fail_send=1;
  k_server_client_received(s.connections,frame,total);
  apply_until(&s,baseline+1);
  turn(&s,50u);
  TEST_ASSERT(gcap.close_calls>0,"send failure closes the connection instead of stranding the client");
  k_server_release(&s);
  TEST_END();
}

/* The real transport's close emits CEMON_CLOSED inline, whose handler FREES the k_conn.  The capture
   transport used above never frees, so nothing in the deterministic suites could see what happens when a
   send failure closes the connection from inside a frame handler.  This test makes close behave like the
   real one and pins what used to go wrong: the connection must not be freed under the reader, and the rx
   accounting must run exactly once (the second decrement wrapped the counter, and a wrapped counter pauses
   every later client for good). */
static k_conn *g_close_target;
static void cap_fatal_close(void *sock){
  (void)sock;
  gcap.close_calls++;
  if(g_close_target) k_conn_closed(g_close_target);   /* what cemon_close + the app's CLOSED handler do */
}
static void test_close_during_send_is_deferred(void){
  k_server s;
  k_server_transport fatal;
  k_u8 frame[K_FRAME_HEADER+64];
  k_u32 one,total=0;
  TEST_BEGIN("server: a close from inside a frame handler defers the free and accounts the rx buffer once");
  setup(&s,1,"mem://kstest-close-defer");
  /* HELP is answered straight from the frame handler, so a send failure there closes the connection
     WHILE k_rx_feed is still walking the read - the real stack does exactly this (cemon emits
     CEMON_CLOSED inline and the app handler frees the k_conn). */
  fatal=cap_transport;
  fatal.close=cap_fatal_close;
  s.transport=&fatal;
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  k_server_client_accepted(&s,(void*)(size_t)1);
  one=make_client_frame(frame,sizeof(frame),K_REQ_HELP,1u,0,0,0,0);
  TEST_ASSERT(one>0,"first HELP frame");
  total=one;
  one=make_client_frame(frame+total,sizeof(frame)-total,K_REQ_HELP,2u,0,0,0,0);
  TEST_ASSERT(one>0,"second HELP frame in the same read");
  total+=one;
  g_close_target=s.connections;
  gcap.fail_send=1;
  k_server_client_received(s.connections,frame,total);
  gcap.fail_send=0;
  g_close_target=0;
  TEST_ASSERT(gcap.close_calls>0,"the failed response closed the connection");
  TEST_ASSERT(s.closing!=0,"the connection is queued for the reaper, not freed under the reader");
  TEST_ASSERT(s.rx_buffer_bytes<(k_u64)1u<<32,"the rx accounting ran once (a double decrement wrapped it)");
  turn(&s,50u);
  TEST_ASSERT(s.closing==0,"the reaper frees it at the top of the advance step");
  TEST_ASSERT(s.connections==0,"the dead connection is unlinked from the live list");
  k_server_release(&s);
  TEST_END();
}

static void test_single_voter_waits_for_local_wal(void){
  k_server s;
  const unsigned char *value=0;
  unsigned int value_len=0;
  k_u8 frame[K_FRAME_HEADER+64];
  k_u32 total;
  TEST_BEGIN("server: sole voter applies and acknowledges only after local WAL");
  setup(&s,1,"mem://kstest-local-durable");
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  k_server_client_accepted(&s,(void*)(size_t)1);
  total=make_client_frame(frame,sizeof(frame),K_REQ_SET,1u,"k",1,"v",1);
  TEST_ASSERT(total>0,"SET frame");
  gcap.send_count=0;
  k_server_client_received(s.connections,frame,total);
  k_server_advance(&s,50u); /* submit Ready, but deliberately do not drain wal_rt */
  TEST_ASSERT_I64_EQ(treap_get(s.tree,(const unsigned char*)"k",1,&value,&value_len),0,
    "state machine is unchanged before local WAL completion");
  TEST_ASSERT_I64_EQ(gcap.send_count,0,"no success response before local WAL completion");
  runtime_drain(s.wal_rt);
  k_server_advance(&s,0u);
  TEST_ASSERT(treap_get(s.tree,(const unsigned char*)"k",1,&value,&value_len)==1,
    "state machine applies after local WAL completion");
  TEST_ASSERT(value_len==1&&value&&value[0]=='v',"durable value is visible");
  TEST_ASSERT(gcap.send_count>0,"success response follows local WAL completion");
  k_server_release(&s);
  TEST_END();
}

static void test_wal_backpressure_pauses_client_receive(void){
  k_server s;
  k_conn *conn;
  k_u8 frame[K_FRAME_HEADER+32];
  k_u32 total;
  TEST_BEGIN("server: saturated WAL pauses and later resumes client receive");
  setup(&s,1,"mem://kstest-wal-backpressure");
  TEST_ASSERT(k_server_open(&s)==0,"open");
  TEST_ASSERT(elect(&s)==0,"leader");
  k_server_client_accepted(&s,(void*)(size_t)1);
  conn=s.connections;
  TEST_ASSERT_I64_EQ(gcap.recv_calls,1,"accept arms the first receive");
  s.wal_inflight_count=K_WAL_INFLIGHT_MAX;
  total=make_client_frame(frame,sizeof(frame),K_REQ_HELP,1u,0,0,0,0);
  TEST_ASSERT(total>0,"HELP frame");
  k_server_client_received(conn,frame,total);
  TEST_ASSERT_I64_EQ(gcap.recv_calls,1,"saturated WAL does not re-arm receive");
  s.wal_inflight_count=K_WAL_INFLIGHT_MAX-1;
  k_server_drive(&s,0u);
  TEST_ASSERT_I64_EQ(gcap.recv_calls,2,"receive resumes below the WAL high-water mark");
  k_server_release(&s);
  TEST_END();
}

static void test_global_client_admission_limits(void){
  k_server s;
  k_conn *conn;
  k_request *request;
  k_u8 frame[K_FRAME_HEADER+32];
  k_u32 total;
  TEST_BEGIN("server: global client connection and request-memory limits");
  setup(&s,1,"mem://kstest-admission-limits");
  TEST_ASSERT(k_server_open(&s)==0,"open");
  s.client_connection_count=K_CLIENT_CONNECTION_MAX;
  gcap.close_calls=0;
  k_server_client_accepted(&s,(void*)(size_t)1);
  TEST_ASSERT_I64_EQ(gcap.close_calls,1,"connection above the global cap is closed");
  TEST_ASSERT(s.connections==0,"rejected connection is not linked");
  s.client_connection_count=0;
  s.request_bytes=K_REQUEST_BYTES_MAX;
  request=k_request_create_server(&s,0,1u,K_REQ_GET,0,0,0,0,1u);
  TEST_ASSERT(request==0,"request above the byte cap is rejected");
  s.request_bytes=0;
  request=k_request_create_server(&s,0,2u,K_REQ_GET,0,0,0,0,16u);
  TEST_ASSERT(request!=0,"request below the cap is admitted");
  TEST_ASSERT_I64_EQ(s.request_count,1,"request count is charged");
  TEST_ASSERT(s.request_bytes>0,"request bytes are charged");
  k_request_free(&s,request);
  TEST_ASSERT_I64_EQ(s.request_count,0,"request count is released");
  TEST_ASSERT_U64_EQ(s.request_bytes,0,"request bytes are released");
  k_server_client_accepted(&s,(void*)(size_t)2);
  conn=s.connections;
  TEST_ASSERT_I64_EQ(gcap.recv_calls,1,"admitted connection arms receive");
  s.request_count=K_REQUEST_INFLIGHT_MAX;
  total=make_client_frame(frame,sizeof(frame),K_REQ_HELP,3u,0,0,0,0);
  TEST_ASSERT(total>0,"HELP frame");
  k_server_client_received(conn,frame,total);
  TEST_ASSERT_I64_EQ(gcap.recv_calls,1,"request high-water pauses receive");
  s.request_count=0;
  k_server_drive(&s,0u);
  TEST_ASSERT_I64_EQ(gcap.recv_calls,2,"receive resumes after request pressure clears");
  k_server_release(&s);
  TEST_END();
}

static void test_raft_command_size_limit(void){
  static k_u8 payload[K_RAFT_COMMAND_MAX];
  k_buf command;
  TEST_BEGIN("server: Raft command admission reserves peer-frame overhead");
  memset(payload,0x5a,sizeof(payload));
  memset(&command,0,sizeof(command));
  TEST_ASSERT_I64_EQ(k_server_build_write_command(&command,K_REQ_MSET,payload,K_RAFT_COMMAND_MAX-1u,0,0),0,
    "command exactly at the replication limit is accepted");
  TEST_ASSERT_U64_EQ(command.len,K_RAFT_COMMAND_MAX,"accepted command reaches the exact limit");
  k_buf_free(&command);
  memset(&command,0,sizeof(command));
  TEST_ASSERT(k_server_build_write_command(&command,K_REQ_MSET,payload,K_RAFT_COMMAND_MAX,0,0)!=0,
    "command above the replication limit is rejected before Raft append");
  k_buf_free(&command);
  TEST_END();
}

static void test_snapshot_wal_byte_accounting(void){
  k_server s;
  TEST_BEGIN("server: snapshot policy counts WAL bytes across segments");
  memset(&s,0,sizeof(s));
  s.cfg.wal_seg_size=1000u;
  s.snapshot_wal_segment=2u;
  s.snapshot_wal_offset=100u;
  s.wal_meta.next.segment=2u;
  s.wal_meta.next.offset=500u;
  TEST_ASSERT_U64_EQ(k_server_wal_bytes_since_snapshot(&s),400u,"same-segment delta");
  s.wal_meta.next.segment=3u;
  s.wal_meta.next.offset=50u;
  TEST_ASSERT_U64_EQ(k_server_wal_bytes_since_snapshot(&s),950u,"cross-segment tail plus head");
  s.wal_meta.next.segment=5u;
  s.wal_meta.next.offset=25u;
  TEST_ASSERT_U64_EQ(k_server_wal_bytes_since_snapshot(&s),2925u,"multiple full middle segments");
  TEST_END();
}

static void test_rx_buffer_admission_accounting(void){
  k_server s;
  k_conn *conn;
  k_u8 frame[K_FRAME_HEADER+16];
  k_u32 total;
  TEST_BEGIN("server: partial client frames obey the global RX budget");
  setup(&s,1,"mem://kstest-rx-budget");
  TEST_ASSERT(k_server_open(&s)==0,"open");
  k_server_client_accepted(&s,(void*)(size_t)1);
  conn=s.connections;
  total=make_client_frame(frame,sizeof(frame),K_REQ_HELP,1u,0,0,0,0);
  TEST_ASSERT(total>4u,"HELP frame");
  k_server_client_received(conn,frame,4u);
  TEST_ASSERT_U64_EQ(s.rx_buffer_bytes,4u,"partial frame bytes are charged globally");
  k_conn_closed(conn);
  TEST_ASSERT_U64_EQ(s.rx_buffer_bytes,0u,"closing the connection releases partial frame bytes");
  s.rx_buffer_bytes=K_RX_BYTES_MAX;
  gcap.recv_calls=0;
  k_server_client_accepted(&s,(void*)(size_t)2);
  conn=s.connections;
  TEST_ASSERT(conn&&conn->recv_paused,"RX high-water admits the connection but pauses reads");
  TEST_ASSERT_I64_EQ(gcap.recv_calls,0,"no receive is armed at the RX high-water mark");
  s.rx_buffer_bytes=0;
  k_server_drive(&s,0u);
  TEST_ASSERT_I64_EQ(gcap.recv_calls,1,"receive resumes after RX pressure clears");
  k_server_release(&s);
  TEST_END();
}


/* ---- targeted apply-path stress (no sockets, no event loop) ----
   Same path the server takes for a committed write (feed a client frame, advance until applied).
   Runs under Application Verifier Heaps to catch heap corruption in seconds rather than in tens of
   network soak rounds.  Usage: kserver_test.exe --apply-stress <ops> [keyspace] */
static int apply_stress(test_u64 ops,test_u64 keyspace,int threaded,int nosnap){
  k_server s;
  char base[64];
  k_u8 frame[K_FRAME_HEADER+128];
  k_u32 total,id;
  test_u64 i;
  if(!keyspace) keyspace=TEST_U64_C(4096);
  /* threaded=1 uses the REAL runtime backend (worker threads), which is the only configuration in
     which the WAL worker and the event loop touch a request's live payload concurrently.  Worker threads
     require a store whose backend is thread-safe, and mem:// is not (process-global, unlocked inode table),
     so the threaded run is disk-backed and the store is cleaned up on the way out. */
  if(threaded){
    sprintf(base,"disk://kstest-applystress-%u",(unsigned)(ops&0xffffu));
    disk_cleanup(base);
    setup(&s,1,base);
    s.runtime_backend=0;
  }else{
    sprintf(base,"mem://kstest-applystress-%u",(unsigned)(ops&0xffffu));
    setup(&s,1,base);
  }
  if(k_server_open(&s)!=0){ printf("apply-stress: open failed\n"); return 1; }
  if(elect(&s)!=0){ printf("apply-stress: no leader\n"); k_server_release(&s); return 1; }
  k_server_client_accepted(&s,(void*)(size_t)1);
  if(nosnap){ s.cfg.snapshot_entries=0xffffffffu; s.cfg.snapshot_segments=0xffffffffu; }
  printf("apply-stress: ops=%" TEST_U64_FMT " keyspace=%" TEST_U64_FMT " backend=%s snapshots=%s\n",ops,keyspace,threaded?"thread":"sync",nosnap?"disabled":"default");
  for(i=0;i<ops;i++){
    char key[32];
    unsigned int klen;
    klen=(unsigned int)sprintf(key,"ph_%" TEST_U64_FMT,(i%(keyspace?keyspace:TEST_U64_C(1))));
    id=(k_u32)(i+1u);
    total=make_client_frame(frame,sizeof(frame),K_REQ_SET,id,key,klen,"payload",7u);
    if(!total){ printf("apply-stress: frame build failed at %" TEST_U64_FMT "\n",i); break; }
    k_server_client_received(s.connections,frame,total);
    turn(&s,20u);
#ifdef K_ALLOC_DEBUG
    /* Between-operation validation: if a freed block was written, this names the VICTIM's
       allocation site and aborts here, so the failing operation index is the writer. */
    { char where[64]; sprintf(where,"post-free write after op %" TEST_U64_FMT,i); k_dbg_verify(where); }
#endif
    if((i%TEST_U64_C(50000))==TEST_U64_C(0)&&i) printf("apply-stress: %" TEST_U64_FMT " ops, count=%" TEST_U64_FMT "\n",i,(test_u64)treap_count(s.tree));
  }
  printf("apply-stress: done %" TEST_U64_FMT " ops\n",ops);
  k_server_release(&s);
  if(threaded) disk_cleanup(base);              /* the threaded run is disk-backed: take its store with us */
  return 0;
}

/* The membership-wait report (review: auto_replace's silent wait).  A catch-up wait is the one state an
   operator must ACT on - "start the replacement node" - and it used to be invisible outside TOPOLOGY.  The
   wait needs a cluster to stage, so the state is injected exactly as the ADDR apply would leave it and the
   deterministic clock is driven: the age must accumulate, the reminder must not flood, and both must reset
   when the CONFIG apply drains the wait. */
/* mem:// and the worker threads: this pair was refused while the in-memory backend's table was unlocked (two
   threads inside vfs_open/vfs_unlink could lose a bucket update).  The backend serialises that table with a
   spinlock now, so the combination must be accepted like any other.  That the concurrency is then CORRECT is
   not something this lifecycle check can show - tests/vfs_fault_test.c hammers two threads on two paths that
   share one hash bucket for that. */
static void test_mem_store_accepts_worker_threads(void){
  k_server s;
  TEST_BEGIN("server: mem:// starts with the real runtime backend (the table is locked now)");
  setup(&s,1,"mem://kstest-memthread-1");
  s.runtime_backend="thread";                 /* the default, spelled out: NOT the deterministic runtime */
  TEST_ASSERT(k_server_open(&s)==0,"a mem:// store starts with worker threads");
  k_server_release(&s);
  setup(&s,1,"mem://kstest-memthread-2");
  s.runtime_backend="sync";
  TEST_ASSERT(k_server_open(&s)==0,"and with the sync runtime, as before");
  k_server_release(&s);
  TEST_END();
}

static void test_membership_wait_reporting(void){
  k_server s;
  int i;
  TEST_BEGIN("membership wait: age accumulates, reminders are rate limited to one per 10s, both reset");
  setup(&s,1,"mem://kstest-mwait-1");
  TEST_ASSERT(k_server_open(&s)==0,"open");       /* advance() does nothing before the node is open */
  TEST_ASSERT(elect(&s)==0,"leader");
  s.pending[0]=2;                                 /* as the ADDR apply leaves it */
  s.pending_source[0]=1;                          /* and this node is the one that submitted it */
  s.pending_count=1;
  for(i=0;i<30;i++) turn(&s,1000u);      /* 30s of waiting, driven one second at a time */
  TEST_ASSERT_I64_EQ((raft_i64)s.membership_pending_ms,30000,"30s of wait accumulated");
  TEST_ASSERT_I64_EQ((raft_i64)s.membership_notice_count,4,"opening line + one reminder per 10s");
  TEST_ASSERT(s.voter_count==1,"the report changes nothing about the config");
  s.pending_count=0;                     /* the CONFIG apply graduated the target */
  turn(&s,1000u);
  TEST_ASSERT_I64_EQ((raft_i64)s.membership_pending_ms,0,"age resets when the wait ends");
  /* A pending entry this node did NOT submit (the replicated ADDR apply leaves source 0) is not reported and
     does not start an age: a follower cannot know whether that change committed or was refused, and the old
     server-wide source field made every later wait inherit whoever submitted last (review 3.3 / 3.5). */
  TEST_ASSERT(s.membership_pending_ms==0,"a foreign pending entry does not start an age after reset");
  s.pending[0]=3;
  s.pending_source[0]=0;                 /* learned from the ADDR apply: submitted elsewhere */
  s.pending_count=1;
  for(i=0;i<30;i++) turn(&s,1000u);
  TEST_ASSERT(s.membership_pending_ms==0,"a foreign pending entry never accumulates an age here");
  TEST_ASSERT_I64_EQ((raft_i64)s.membership_notice_count,0,"a foreign pending entry is not reported");
  s.pending_source[0]=1;                 /* the same entry, now submitted by this node */
  turn(&s,1000u);
  TEST_ASSERT_I64_EQ((raft_i64)s.membership_pending_ms,1000,"an own pending entry does accumulate");
  TEST_ASSERT_I64_EQ((raft_i64)s.membership_notice_count,1,"and it is reported");
  s.pending_count=0;
  turn(&s,1000u);
  TEST_ASSERT(s.membership_pending_ms==0,"and it resets again when the wait ends");
  TEST_ASSERT_I64_EQ((raft_i64)s.membership_notice_count,0,"the reminder budget resets with it");
  TEST_END();
}

int main(int argc,char **argv){
  if(argc>=3&&strcmp(argv[1],"--apply-stress")==0) return apply_stress(test_strtoull(argv[2]),argc>=4?test_strtoull(argv[3]):TEST_U64_C(4096),argc>=5&&strcmp(argv[4],"thread")==0,argc>=6&&strcmp(argv[5],"nosnap")==0);
  TEST_PLAN(40);
  g_run_tag=0;
  if(k_monotonic_us(&g_run_tag)!=0) g_run_tag=(k_u64)time(0);
  test_fcall_gate_reopens_when_its_request_dies();
  test_client_request_in_accept_cycle_is_answered();
  test_single_node_elect();
  test_single_node_set_apply();
  test_single_node_get_reads_back();
  test_single_node_crash_restart();
  test_elapsed_step_carries_sub_ms();
  test_group_commit_batch_age_starts_at_arrival();
  test_wal_recovery_scan_ignores_stale_meta();
  test_wal_recovery_skips_torn_tail();
  test_wal_recovery_rejects_corrupt_acked_record();
  test_wal_recovery_rejects_corrupt_record_after_checkpoint();
  test_wal_recovery_rejects_missing_generation();
  test_wal_recovery_across_segment_rotation();
  test_wal_recovery_refuses_missing_newest_segment();
  test_install_snapshot_discards_a_stale_longer_file();
  test_wal_recovery_refuses_a_middle_segment_cut();
  test_wal_recovery_refuses_a_prefix_past_the_scan_ceiling();
  test_wal_meta_slot_only_written_when_durable();
  test_wal_meta_both_slots_invalid_fail_stop();
  test_wal_meta_one_slot_torn_still_starts();
  test_snapshot_file_verify_and_retention();
  test_single_node_get_missing();
  test_malformed_frame_closes();
  test_malformed_batch_frame_rejected();
  test_admission_gate_rejects();
  test_disk_durability();
  test_group_commit_policy();
  test_peer_send_drop_counted();
  test_peer_encode_splits_large_append();
  test_client_response_failure_closes_connection();
  test_close_during_send_is_deferred();
  test_single_voter_waits_for_local_wal();
  test_wal_backpressure_pauses_client_receive();
  test_global_client_admission_limits();
  test_raft_command_size_limit();
  test_snapshot_wal_byte_accounting();
  test_rx_buffer_admission_accounting();
  test_membership_wait_reporting();
  test_mem_store_accepts_worker_threads();
  TEST_SUMMARY();
  return TEST_EXIT_CODE();
}
