/* ============================================================================
   raft_cluster_fuzz.c -- deterministic MULTI-NODE cluster fuzz driver for raft.h

   Complements the single-node driver (raft_fuzz.c, which covers API-level
   crashes and OOM rollback).  This driver runs an ACTUAL N-node Raft cluster
   (3 or 5 nodes) through the real pull-based message-passing harness:

     - every node is driven by raft_advance();
     - outbound raft_peer_message values are deep-copied, perturbed
       (drop / duplicate / reorder-delay / random multi-node partition) and
       delivered to their target via raft_recvfrom_peer();
     - each node's application layer persists, applies and serves snapshots;
     - nodes CRASH at random (raft_destroy) and RESTART from a deep-copied
       durable disk image rebuilt via cfg.restore (the crash/restart path);
     - the LIBRARY's allocations are single-point OOM-injectable (chaos
       phase only; never in the fault-free liveness tail).

   The point of this driver is the continuously-checked SAFETY ORACLE:

     (1) ELECTION SAFETY:   at most one leader per term.
     (2) LOG MATCHING / STATE MACHINE SAFETY: no two nodes ever apply
         different entries at the same index (same index+term => same data).
     (3) LEADER COMPLETENESS: a newly elected leader must contain every
         entry that has been applied anywhere in the cluster.
     (4) SNAPSHOT CONSISTENCY: an installed snapshot boundary must agree
         with the committed term at that index (when known).
     (5) SNAPSHOT BYTES: an installed snapshot image must equal the
         canonical applied-state image at that index (same index => same
         bytes), catching wrong-copy / offset / truncation corruption.

   The oracle is BLACK-BOX: it reads only the public Ready output (persist
   view, apply entries, is_leader, client results) and never calls raft_inspect.
   One deliberate exception: check_config_consistency (the P1 config-immediate
   check) is gray-box -- it reads live config_old/config_new/config_learners/
   config_joint (not the Ready output) to cross-check the LOG-derived config
   (Sec. 4.1), mirroring what dump_logs would show.  The five SAFETY oracles stay
   fully black-box via Ready; the gray-box check asserts a config-immediate
   INTERNAL invariant (live config == log-derived), not an externally-observable
   violation.

   After the random-fault phase, a clean quiescence tail heals the network,
   restarts every dead node, and checks LIVENESS: a leader must be elected and
   a marker command must commit and be applied.

   Any violation aborts with the message, and the offending seed is the last
   "seed N" line on stderr, so it is reproducible via:

     raft_cluster_fuzz <seed> 1

   No third-party dependencies, C89, deterministic (splitmix64 PRNG).

   Build (Windows/MinGW, C89):
     gcc -std=c89 -O2 -Wall -Wextra tests/raft_cluster_fuzz.c -o raft_cluster_fuzz
   Build (Linux/clang, recommended, with sanitizers):
     clang -std=c89 -O1 -g -fsanitize=address,undefined tests/raft_cluster_fuzz.c -o raft_cluster_fuzz

   Usage:
     raft_cluster_fuzz [start_seed [count [persist_delay]]]

   persist_delay (default 0) models an asynchronous fsync: when > 0, each
   non-snapshot-dirty persist lands after 0..persist_delay random steps, during
   which the node keeps advancing -- so the leader can commit before its own
   disk write lands (raft.h Sec. 10.2.1).  A crash inside the window drops the
   un-landed persist, exactly like a slow fsync that never completed.
   ============================================================================ */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <signal.h>

/* MSVC 6.0 provides _vsnprintf, not C99 vsnprintf */
#if defined(_MSC_VER)
#define vsnprintf _vsnprintf
#endif

/* ---- OOM-injectable allocators for the LIBRARY only (the harness's own
   malloc/calloc/realloc stay real).  Defined BEFORE RAFT_IMPLEMENTATION so the
   library's RAFT_MALLOC/CALLOC/REALLOC resolve to these.  fuzz_oom_at==0
   disables; otherwise the (fuzz_oom_at)-th library allocation returns NULL
   exactly once (then clears), and maybe_oom() re-arms it during the chaos
   phase only - never in the fault-free tail, so liveness can't be sabotaged. */
static unsigned int fuzz_oom_at=0;
static unsigned int fuzz_alloc_calls=0;
static unsigned int g_oom_count=0; /* OOM fires this run (volatile-cookie drop confounder) */
/* Per-node OOM attribution (P2-1): every library entry point sets g_cur_node_idx
   first; an OOM that fires is charged to that node.  A cookie's volatile
   storage lives on its ACCEPTING node only, so a later OOM on a DIFFERENT node
   can never drop it - the P0-1 exemption keys on this per-node count instead of
   the over-broad global counter.  Literal 5 == MAX_NODES (defined below). */
static int g_cur_node_idx=-1;
static unsigned int g_oom_node[5];
static void oom_fire(void){
  g_oom_count++;
  if(g_cur_node_idx>=0&&g_cur_node_idx<5) g_oom_node[g_cur_node_idx]++;
}
static void *fuzz_malloc(size_t n){
  fuzz_alloc_calls++;
  if(fuzz_oom_at!=0&&fuzz_alloc_calls==fuzz_oom_at){ fuzz_oom_at=0; oom_fire(); return 0; }
  return malloc(n);
}
static void *fuzz_calloc(size_t nm,size_t sz){
  fuzz_alloc_calls++;
  if(fuzz_oom_at!=0&&fuzz_alloc_calls==fuzz_oom_at){ fuzz_oom_at=0; oom_fire(); return 0; }
  return calloc(nm,sz);
}
static void *fuzz_realloc(void *p,size_t n){
  fuzz_alloc_calls++;
  if(fuzz_oom_at!=0&&fuzz_alloc_calls==fuzz_oom_at){ fuzz_oom_at=0; oom_fire(); return 0; }
  return realloc(p,n);
}
#define RAFT_STATIC
#define RAFT_MALLOC fuzz_malloc
#define RAFT_CALLOC fuzz_calloc
#define RAFT_REALLOC fuzz_realloc
#define RAFT_IMPLEMENTATION
#include "../code/raft.h"
/* MSVC 6 has no `long long`, no ULL/LL literals and no %llu, while MinGW-w64 accepts all of them - which
   is why this class of defect only ever surfaced on the legacy guest.  This driver includes only
   code/raft.h (which pulls in stdlib/string), so it carries its own small layer; a test that already
   includes code/kbase.h uses the project's k_u64/K_U64_FMT instead. */
#if defined(_MSC_VER)
typedef __int64 fuzz_i64;
typedef unsigned __int64 fuzz_u64;
#define FUZZ_I64_FMT "I64d"
#define FUZZ_U64_FMT "I64u"
#define FUZZ_U64_C(x) x##ui64
#else
typedef long long fuzz_i64;
typedef unsigned long long fuzz_u64;
#define FUZZ_I64_FMT "lld"
#define FUZZ_U64_FMT "llu"
#define FUZZ_U64_C(x) x##ULL
#endif

/* ---- tuning ---- */
#define MAX_NODES    5
#define MAX_LOG      8192    /* absolute log index range covered by the oracle */
#define MAX_SNAP     8192    /* snapshot byte buffer cap (>= snapshot_chunk_size) */
#define MAX_PENDING  1024    /* in-flight perturbed message queue cap */
#define MAX_LEADERS  1024    /* distinct (term -> leader) records tracked */
#define MAX_STEPS    1500    /* harness steps per cluster run */

/* ---- deterministic PRNG: splitmix64 ---- */
static fuzz_u64 fuzz_state;

static fuzz_u64 fuzz_next_u64(void){
  fuzz_u64 z;
  fuzz_state += FUZZ_U64_C(0x9E3779B97F4A7C15);
  z = fuzz_state;
  z = (z ^ (z >> 30)) * FUZZ_U64_C(0xBF58476D1CE4E5B9);
  z = (z ^ (z >> 27)) * FUZZ_U64_C(0x94D049BB133111EB);
  z = z ^ (z >> 31);
  return z;
}
static unsigned int fuzz_rand(void){
  return (unsigned int)(fuzz_next_u64() & 0xFFFFFFFFu);
}
static unsigned int fuzz_rand_below(unsigned int n){
  if(n<=1) return 0;
  return fuzz_rand() % n;
}
/* 1 = fault-free quiescence tail: message delivery is unperturbed (no
   drop/delay/duplicate) AND time advances in small steps only.  Forward-
   declared here because advance_elapsed() is defined before the cluster
   live-state block below (where the flag is set/cleared); single storage. */
static int g_clean;
/* Set by each clean-tail step loop: the single elapsed every node advances by
   that step (see process_ready).  0 outside the tail. */
static unsigned int g_step_elapsed;
/* ---- persist-delay: async-persist timing knob ($10.2.1).  0 (default) keeps
   the synchronous-fsync model exactly as before; a value N > 0 makes each
   non-snapshot-dirty persist land after 0..N random steps, during which the
   node keeps advancing.  Set via the optional third CLI argument. ---- */
static unsigned int g_persist_delay;
/* elapsed per advance: normally 1..80ms (fine-grained interleaving), but
   occasionally a large tick (1..700ms) to exercise deadline-overshoot branches
   (an election/heartbeat timeout exceeded by a wide margin within ONE tick;
   election_max reaches ~598, so 700 fully covers the overshoot)
   that the small-tick distribution never reaches. */
static unsigned int advance_elapsed(void){
  /* Sec. 3.9 (broadcastTime << electionTimeout): the fault-free tail must drive
     time in SMALL steps.  A single 1..700ms tick exceeds the 150..598ms
     election timeout, so in the tail it trips every follower's deadline in one
     advance and the cluster re-elects a new leader every term (leader churn).
     That churn is CORRECT Raft behaviour - safety never depends on timing,
     availability does - but it defeats the Sec. 3.10 TRANSFER CONVERGENCE liveness
     check: the transfer target cannot out-campaign the churn, so the assertion
     fires spuriously (seed 2444).  The overshoot stressor belongs to the
     chaos phase only. */
  if(g_clean) return 1u+fuzz_rand_below(80u);
  if(fuzz_rand_below(24)==0) return 1u+fuzz_rand_below(700u);
  return 1u+fuzz_rand_below(80u);
}

/* ---- failure reporting ---- */
static void ev_dump(void);
static void dump_logs(void);
static void fail(const char *fmt,...){
  char buf[512];
  va_list ap;
  va_start(ap,fmt);
  vsnprintf(buf,sizeof(buf),fmt,ap);
  va_end(ap);
  fprintf(stderr,"FAIL: %s\n",buf);
  ev_dump();
  dump_logs();
  exit(1);
}

/* ---- content hash (FNV-1a 64-bit) over the applied command bytes ---- */
static fuzz_u64 hash_bytes(const void *p,unsigned int n){
  const unsigned char *b=(const unsigned char*)p;
  fuzz_u64 h=FUZZ_U64_C(14695981039346656037);
  unsigned int i;
  for(i=0;i<n;i++){ h ^= b[i]; h *= FUZZ_U64_C(1099511628211); }
  return h;
}
/* ---- config-content hash (P0-1 config log-matching + P1-3 snapshot config
   consistency).  cfg ids are sorted (raft_make_sorted_ids), so the byte stream
   is canonical across nodes; a length tag between the three masks removes
   set-boundary ambiguity.  A NULL ids pointer (mask-less CONFIG entry) hashes
   as an empty mask, matching the library's no-op treatment. ---- */
static fuzz_u64 cfg_hash(raft_mask old,raft_mask new_,raft_mask learn){
  fuzz_u64 h=FUZZ_U64_C(14695981039346656037);
  int i,n;
  n=(old.ids&&old.id_count>0)?old.id_count:0;
  for(i=0;i<n;i++){ h ^= (fuzz_u64)old.ids[i]; h *= FUZZ_U64_C(1099511628211); }
  h ^= (fuzz_u64)n; h *= FUZZ_U64_C(1099511628211);
  n=(new_.ids&&new_.id_count>0)?new_.id_count:0;
  for(i=0;i<n;i++){ h ^= (fuzz_u64)new_.ids[i]; h *= FUZZ_U64_C(1099511628211); }
  h ^= (fuzz_u64)n; h *= FUZZ_U64_C(1099511628211);
  n=(learn.ids&&learn.id_count>0)?learn.id_count:0;
  for(i=0;i<n;i++){ h ^= (fuzz_u64)learn.ids[i]; h *= FUZZ_U64_C(1099511628211); }
  h ^= (fuzz_u64)n; h *= FUZZ_U64_C(1099511628211);
  return h;
}

/* ---- diagnostic event ring buffer (dumped on failure) ---- */
#define EV_MAX 8192
static char ev_buf[EV_MAX][112];
static int ev_count;
static void ev(const char *fmt,...){
  char buf[112];
  va_list ap;
  va_start(ap,fmt);
  vsnprintf(buf,sizeof(buf),fmt,ap);
  va_end(ap);
  strcpy(ev_buf[ev_count%EV_MAX],buf);
  ev_count++;
}
static void ev_dump(void){
  int i,start=ev_count>EV_MAX?ev_count-EV_MAX:0;
  fprintf(stderr,"--- last %d events ---\n",ev_count-start);
  for(i=start;i<ev_count;i++) fprintf(stderr,"%s\n",ev_buf[i%EV_MAX]);
}
/* hard-crash dump: flush the event trace + log dump, then re-raise so the
   process still dies with the right signal (last "seed N" line = repro) */
static void crash_dump(int sig){
  fprintf(stderr,"\n=== CRASH signal %d ===\n",sig);
  ev_dump();
  dump_logs();
  fflush(stderr);
  signal(sig,SIG_DFL);
  raise(sig);
}

/* ---- deep-copy of an outbound raft_peer_message ----
   The Ready's message pointers alias the SENDER's internal buffers, which are
   only valid until raft_ready_consumed / the next advance.  Because messages
   are delayed and reordered by the network model, every pointed-to region is
   copied into one malloc'd payload block. */
typedef struct queued_msg{
  struct queued_msg *next;
  int deliver_at;
  raft_peer_message m;
  unsigned char *payload;
} queued_msg;

/* size of a single mask's ids array (0 if empty) */
static size_t mask_size(raft_mask m){
  return (m.id_count>0&&m.ids)?sizeof(int)*(size_t)m.id_count:0;
}
/* round n up to 8-byte alignment.  The packed payload places typed pointers at
   arbitrary offsets; raft_i64 elements and the 64-bit pointer member of
   raft_mask require 8-byte alignment, so every typed field must be re-aligned
   (measure_msg sizes and fill_msg offsets must agree, or the copy overruns). */
static size_t align8(size_t n){ return (n + 7u) & ~((size_t)7u); }
/* aligned size of one mask's ids (no padding when empty) */
static size_t mask_pad_size(size_t base,raft_mask m){
  if(m.id_count>0&&m.ids) return align8(base)+mask_size(m);
  return base;
}
/* aligned size of a mask ARRAY: aligned struct block, then each ids aligned */
static size_t mask_array_size(size_t base,const raft_mask *arr,int ec){
  size_t n;
  int i;
  if(!arr||ec<=0) return base;
  n=align8(base);
  n+=sizeof(raft_mask)*(size_t)ec;
  for(i=0;i<ec;i++) n=mask_pad_size(n,arr[i]);
  return n;
}
/* total payload bytes needed to deep-copy src's pointed-to regions */
static size_t measure_msg(const raft_peer_message *src){
  size_t n=0;
  switch(src->type){
    case RAFT_MSG_APPEND:
      {
        const raft_append_entries *a=&src->append_entries;
        int ec=a->entry_count,i;
        if(ec>0){
          n+=sizeof(raft_i64)*(size_t)ec;      /* entry_terms (off 0, aligned) */
          n+=(size_t)ec;                       /* entry_kinds (bytes) */
          n=align8(n); n+=sizeof(unsigned int)*(size_t)ec;  /* entry_data_sizes */
          for(i=0;i<ec;i++) n+=a->entry_data_sizes[i];      /* entry_data (bytes) */
          if(a->entry_cfg_old)      n=mask_array_size(n,a->entry_cfg_old,ec);
          if(a->entry_cfg_new)      n=mask_array_size(n,a->entry_cfg_new,ec);
          if(a->entry_cfg_learners) n=mask_array_size(n,a->entry_cfg_learners,ec);
        }
      }
      break;
    case RAFT_MSG_INSTALL_SNAPSHOT:
      {
        const raft_install_snapshot *is=&src->install_snapshot;
        n+=(is->snapshot_chunk_size>0)?(size_t)is->snapshot_chunk_size:0;
        n=mask_pad_size(n,is->snapshot_cfg_old);
        n=mask_pad_size(n,is->snapshot_cfg_new);
        n=mask_pad_size(n,is->snapshot_cfg_learners);
      }
      break;
    default:
      break; /* value-only messages */
  }
  return n;
}
/* copy one mask's ids into payload at off; returns new offset */
static size_t fill_mask(unsigned char *p,size_t off,raft_mask *dst,raft_mask src){
  if(src.id_count>0&&src.ids){
    off=align8(off);
    dst->ids=(const int*)(p+off);
    memcpy(p+off,src.ids,sizeof(int)*(size_t)src.id_count);
    off+=sizeof(int)*(size_t)src.id_count;
    dst->id_count=src.id_count;
  }else{
    dst->ids=0;
    dst->id_count=0;
  }
  return off;
}
/* copy a mask array (ec elements) into payload at off; returns new offset */
static size_t fill_mask_array(unsigned char *p,size_t off,const raft_mask **dst,
                              const raft_mask *src_arr,int ec){
  raft_mask *arr;
  int i;
  if(ec<=0||!src_arr){ *dst=0; return off; }
  off=align8(off);
  arr=(raft_mask*)(p+off);
  memcpy(p+off,src_arr,sizeof(raft_mask)*(size_t)ec);
  off+=sizeof(raft_mask)*(size_t)ec;
  for(i=0;i<ec;i++) off=fill_mask(p,off,&arr[i],src_arr[i]);
  *dst=arr;
  return off;
}
/* fill q->payload (allocated to exactly measure_msg(src) bytes) and fix q->m's
   pointers into it.  Single allocation -> no realloc pointer invalidation. */
static void fill_msg(const raft_peer_message *src,queued_msg *q){
  unsigned char *p=q->payload;
  size_t off=0;
  switch(src->type){
    case RAFT_MSG_APPEND:
      {
        raft_append_entries *a=&q->m.append_entries;
        const raft_i64 *src_terms=a->entry_terms;
        const unsigned char *src_kinds=a->entry_kinds;
        const unsigned int *src_sizes=a->entry_data_sizes;
        const void *src_data=a->entry_data;
        const raft_mask *src_old=a->entry_cfg_old;
        const raft_mask *src_new=a->entry_cfg_new;
        const raft_mask *src_learn=a->entry_cfg_learners;
        int ec=a->entry_count,i;
        size_t total=0;
        if(ec<=0){
          a->entry_terms=0; a->entry_kinds=0; a->entry_data=0; a->entry_data_sizes=0;
          a->entry_cfg_old=0; a->entry_cfg_new=0; a->entry_cfg_learners=0;
          return;
        }
        a->entry_terms=(const raft_i64*)(p+off);
        memcpy(p+off,src_terms,sizeof(raft_i64)*(size_t)ec);
        off+=sizeof(raft_i64)*(size_t)ec;
        a->entry_kinds=p+off;
        memcpy(p+off,src_kinds,(size_t)ec);
        off+=(size_t)ec;
        off=align8(off);
        a->entry_data_sizes=(const unsigned int*)(p+off);
        memcpy(p+off,src_sizes,sizeof(unsigned int)*(size_t)ec);
        off+=sizeof(unsigned int)*(size_t)ec;
        for(i=0;i<ec;i++) total+=src_sizes[i];
        if(total){
          a->entry_data=p+off;
          memcpy(p+off,src_data,total);
          off+=total;
        }else a->entry_data=0;
        off=fill_mask_array(p,off,&a->entry_cfg_old,src_old,ec);
        off=fill_mask_array(p,off,&a->entry_cfg_new,src_new,ec);
        off=fill_mask_array(p,off,&a->entry_cfg_learners,src_learn,ec);
      }
      break;
    case RAFT_MSG_INSTALL_SNAPSHOT:
      {
        raft_install_snapshot *is=&q->m.install_snapshot;
        const void *src_data=is->snapshot_data;
        size_t n=(is->snapshot_chunk_size>0)?(size_t)is->snapshot_chunk_size:0;
        if(n){
          is->snapshot_data=p+off;
          memcpy(p+off,src_data,n);
          off+=n;
        }else is->snapshot_data=0;
        off=fill_mask(p,off,&is->snapshot_cfg_old,is->snapshot_cfg_old);
        off=fill_mask(p,off,&is->snapshot_cfg_new,is->snapshot_cfg_new);
        off=fill_mask(p,off,&is->snapshot_cfg_learners,is->snapshot_cfg_learners);
      }
      break;
    default:
      break;
  }
}
static queued_msg *msg_clone(const raft_peer_message *src,int deliver_at){
  queued_msg *q=(queued_msg*)malloc(sizeof(queued_msg));
  size_t total;
  if(!q) return 0;
  q->next=0;
  q->deliver_at=deliver_at;
  q->m=*src;
  q->payload=0;
  total=measure_msg(src);
  if(total){
    q->payload=(unsigned char*)malloc(total);
    if(!q->payload){ free(q); return 0; }
    fill_msg(src,q);
  }
  return q;
}
static void msg_free(queued_msg *q){
  if(!q) return;
  free(q->payload);
  free(q);
}

/* ---- asynchronous-persist in-flight copy (the raft.h Sec. 10.2.1 timing model).
   When g_persist_delay > 0, a NON-snapshot-dirty persist view is deep-copied
   here and only written to the disk image + reported via raft_persist_complete
   when its due step arrives (persist_copy_flush).  The node KEEPS ADVANCING
   meanwhile, so the leader can receive a follower's durable ACK and commit
   BEFORE its own fsync completes - the exact "committing before the leader's
   own disk write" window raft.h documents as its load-bearing Sec. 10.2.1
   assumption.  A crash before the due step drops this in-flight copy (the disk
   image is untouched): a slow fsync whose data never landed.  snapshot_dirty
   persists are NOT delayed (snapshot publish timing stays on the synchronous
   path), so this copy never carries snapshot bytes or snapshot metadata. */
typedef struct persist_copy{
  raft_i64 term;
  int voted_for;
  raft_i64 lii;                 /* last_included_index */
  raft_i64 lit;                 /* last_included_term */
  int log_count;
  raft_persist_entry entries[MAX_LOG];
  unsigned char data[MAX_LOG][32];
  int old_ids[MAX_LOG][MAX_NODES], new_ids[MAX_LOG][MAX_NODES], learn_ids[MAX_LOG][MAX_NODES];
} persist_copy;

/* ---- per-node harness state ---- */
typedef struct{
  raft_ctx *r;
  int id;
  int alive;                        /* 1=running, 0=crashed (r is NULL) */
  raft_i64 cur_term;                /* current term (from Ready.persist.term) */
  raft_i64 term_at[MAX_LOG];        /* term of the entry at absolute index i */
  unsigned char term_known[MAX_LOG];/* term_at[i] valid? */
  raft_i64 last_applied;
  raft_i64 commit_index;            /* black-box commit index (for the committed-term oracle) */
  raft_i64 last_index;              /* last live log index (included + count) */
  raft_i64 last_included_index;
  raft_i64 last_included_term;
  int last_included_term_known;
  unsigned char snap[MAX_SNAP];     /* this node's snapshot bytes (app-owned) */
  unsigned int snap_size;
  raft_i64 snap_last_index;
  raft_i64 snap_last_term;
  /* pending LOCAL snapshot: filled into snap_tmp, published to snap only after
     raft_persist_complete finalizes it (otherwise a leader would stream the
     still-current snapshot metadata against the new bytes - size mismatch).
     snap_tmp_index records WHICH index the pending snapshot is for: two
     snapshots at different indices can have the SAME byte size, so the publish
     check must match the index, not just the size. */
  unsigned char snap_tmp[MAX_SNAP];
  unsigned int snap_tmp_size;
  raft_i64 snap_tmp_index;
  int snap_tmp_pending;
  /* INCOMING snapshot (written by deliver_one as chunks arrive); published into
     snap only after raft_apply_complete installs it. */
  unsigned char snap_recv[MAX_SNAP];
  unsigned int snap_recv_size;
  /* stable config (reused verbatim on restart so timing stays identical) */
  unsigned int cfg_heartbeat, cfg_e_min, cfg_e_max;
  unsigned int cfg_seed, cfg_snap_chunk;
  int cfg_log_chunk;
  /* ---- durable disk image (deep-copied on every persist; the crash/restart
     source of truth).  Only entries AFTER last_included_index are stored,
     exactly like the persist view.  Default model is a SYNCHRONOUS fsync: the
     image is written on each persist view and crashes only fire after
     process_ready returns (persist_complete already issued).  With
     g_persist_delay > 0 the harness instead models an ASYNCHRONOUS fsync
     (raft.h Sec. 10.2.1): a non-snapshot-dirty persist view is captured into
     `inflight`, the image is written only when its due step arrives, and a
     crash in between drops it - so a leader can commit before its own disk
     write lands, exercising the durable-majority commit contract at the
     cluster level (not just via the unit test). ---- */
  int disk_dirty;
  raft_i64 disk_term;
  int disk_voted_for;
  raft_i64 disk_lii;                /* last_included_index (committed snapshot) */
  raft_i64 disk_lit;                /* last_included_term */
  raft_i64 disk_snap_size;          /* snapshot_size (0 = none) */
  unsigned char disk_snap[MAX_SNAP];
  int disk_snap_old_n, disk_snap_new_n, disk_snap_learn_n;
  int disk_snap_old[MAX_NODES], disk_snap_new[MAX_NODES], disk_snap_learn[MAX_NODES];
  /* PROPOSED snapshot (from a snapshot_dirty persist).  It becomes the committed
     snapshot only once the library PUBLISHES it (a later non-dirty persist whose
     last_included_index reached it).  The library may abort a pending snapshot
     on step-down / install, so the committed image must never be overwritten by
     an un-published snapshot (else disk_lii and disk_snap_size diverge and a
     corrupted snapshot is restored and streamed out). */
  int disk_pending_dirty;
  raft_i64 disk_pending_lii, disk_pending_lit;
  unsigned int disk_pending_size;
  unsigned char disk_snap_pending[MAX_SNAP];
  int disk_pending_old_n, disk_pending_new_n, disk_pending_learn_n;
  int disk_pending_old[MAX_NODES], disk_pending_new[MAX_NODES], disk_pending_learn[MAX_NODES];
  int disk_pending_log_count;
  raft_i64 disk_pending_index[MAX_LOG];
  raft_i64 disk_pending_term_i[MAX_LOG];
  unsigned char disk_pending_kind[MAX_LOG];
  unsigned int disk_pending_dsize[MAX_LOG];
  unsigned char disk_pending_data[MAX_LOG][32];
  int disk_pending_oldn[MAX_LOG], disk_pending_newn[MAX_LOG], disk_pending_learnn[MAX_LOG];
  int disk_pending_oldcfg[MAX_LOG][MAX_NODES], disk_pending_newcfg[MAX_LOG][MAX_NODES], disk_pending_learncfg[MAX_LOG][MAX_NODES];
  int disk_log_count;
  raft_i64 disk_index[MAX_LOG];
  raft_i64 disk_term_i[MAX_LOG];
  unsigned char disk_kind[MAX_LOG];
  unsigned int disk_size[MAX_LOG];
  unsigned char disk_data[MAX_LOG][32];
  int disk_old_n[MAX_LOG], disk_new_n[MAX_LOG], disk_learn_n[MAX_LOG];
  int disk_old[MAX_LOG][MAX_NODES], disk_new[MAX_LOG][MAX_NODES], disk_learn[MAX_LOG][MAX_NODES];
  /* ---- asynchronous-persist in-flight state (see persist_copy above) ---- */
  persist_copy inflight;
  int persist_inflight;             /* 1 = an in-flight persist is waiting to land */
  int persist_due;                  /* step at which it lands (persist_copy_flush) */
  raft_i64 inflight_durable;        /* durable_index to report at flush time */
} node;

/* ---- global safety oracle ---- */
static raft_i64 g_term[MAX_LOG];            /* committed term at index (0=unset) */
static unsigned char g_term_set[MAX_LOG];
static fuzz_u64 g_hash[MAX_LOG];  /* committed command hash at index */
static unsigned int g_size[MAX_LOG];        /* committed command byte length */
static int g_first_node[MAX_LOG];           /* node that first applied the index */
static fuzz_u64 g_state[MAX_LOG]; /* canonical applied-state hash at index */
static raft_i64 g_leader_term[MAX_LEADERS];
static int g_leader_id[MAX_LEADERS];
static int g_leader_count;
/* committed term at index (P1-2: recorded on COMMIT, not just apply, so the
   leader-completeness oracle also covers entries committed but not yet applied) */
static raft_i64 g_cterm[MAX_LOG];
static unsigned char g_cterm_set[MAX_LOG];
static raft_i64 g_cterm_max;   /* max committed index seen (bounds the completeness scan) */
static raft_i64 g_commit_max;   /* max commit_index observed across nodes */
/* config-content oracles (P0-1 / P1-3): hash of the membership carried by a
   CONFIG log entry / snapshot at an absolute index.  First-writer-wins; a
   differing second writer is a config divergence (two nodes holding different
   memberships at the same index). */
static fuzz_u64 g_cfg_hash[MAX_LOG];
static unsigned char g_cfg_set[MAX_LOG];
static fuzz_u64 g_snap_cfg_hash[MAX_LOG];
static unsigned char g_snap_cfg_set[MAX_LOG];

/* ---- outstanding client requests (P0-1 result liveness + P0-2 Sec. 6.4 reads) ---- */
#define MAX_OUTSTANDING 512
struct outstanding{
  const void *cookie;
  int node_idx;             /* node index (id-1) that accepted the request */
  int kind;                 /* RAFT_CLIENT_* request type */
  int transfer_target;      /* transfer target id, or 0 */
  raft_i64 submit_commit;   /* g_commit_max at submission (barrier lower bound) */
  unsigned int submit_oom;      /* g_oom_count at submission (an OOM later may drop the volatile cookie) */
  unsigned int submit_oom_node; /* per-node g_oom_node[node_idx] at submission (P2-1) */
};
static struct outstanding g_out[MAX_OUTSTANDING];
static int g_out_count;
/* P0-3 ($3.10) DETERMINISTIC transfer test: submitted in the STABLE tail only.
   g_xfer_target/g_xfer_resolved/g_xfer_status track the single tail transfer;
   after it COMMITS, the target must actually become leader (g_leader). */
#define XFER_COOKIE ((const void*)(size_t)0x7FFF0003)
static int g_xfer_resolved;
static int g_xfer_status;
static int g_xfer_target;
static void out_add(const void *cookie,int node_idx,int kind,int transfer_target){
  int i;
  for(i=0;i<g_out_count;i++)
    if(g_out[i].cookie==cookie&&g_out[i].node_idx==node_idx) return; /* dedupe */
  if(g_out_count>=MAX_OUTSTANDING){
    /* P0-1 coverage would silently degrade: make the loss visible instead of
       dropping this request's tracking without a trace. */
    ev("OUTSTANDING OVERFLOW: untracked cookie %p node %d kind %d (count=%d)",
       cookie,node_idx+1,kind,g_out_count);
    return;
  }
  g_out[g_out_count].cookie=cookie;
  g_out[g_out_count].node_idx=node_idx;
  g_out[g_out_count].kind=kind;
  g_out[g_out_count].transfer_target=transfer_target;
  g_out[g_out_count].submit_commit=g_commit_max;
  g_out[g_out_count].submit_oom=g_oom_count;
  g_out[g_out_count].submit_oom_node=g_oom_node[node_idx];
  g_out_count++;
}
static int out_find(const void *cookie,int node_idx){
  int i;
  for(i=0;i<g_out_count;i++)
    if(g_out[i].cookie==cookie&&g_out[i].node_idx==node_idx) return i;
  return -1;
}
static void out_resolve(const void *cookie,int node_idx){
  int i=out_find(cookie,node_idx);
  if(i<0) return;
  memmove(&g_out[i],&g_out[i+1],(unsigned int)(g_out_count-i-1)*sizeof(g_out[0]));
  g_out_count--;
}
static void out_abandon_node(int node_idx){
  int i=0;
  while(i<g_out_count){
    if(g_out[i].node_idx==node_idx){
      memmove(&g_out[i],&g_out[i+1],(unsigned int)(g_out_count-i-1)*sizeof(g_out[0]));
      g_out_count--;
    }else i++;
  }
}

/* ---- canonical snapshot content (byte-level snapshot oracle) ----
   The applied state at index i is unique (oracle 2 forces every node to apply
   identical (index,term,data)), so a snapshot at index i has a unique canonical
   byte image derived from the running applied-state hash.  Local snapshots are
   filled with this image and installed snapshots must match it, catching any
   byte-copy / offset / truncation corruption in streaming or install. */
static fuzz_u64 state_mix(fuzz_u64 prev,raft_i64 idx,
                                    raft_i64 term,fuzz_u64 data_hash){
  fuzz_u64 h=prev ^ (fuzz_u64)idx;
  h ^= (fuzz_u64)term;
  h ^= data_hash;
  h ^= h>>32;
  h *= FUZZ_U64_C(1099511628211);
  h ^= h>>32;
  return h;
}
static unsigned int snap_size_for(raft_i64 index){
  fuzz_u64 st=(index>=0&&index<MAX_LOG)?g_state[index]:0;
  return 64u+(unsigned int)(st%1024u);
}
static unsigned char snap_byte_for(raft_i64 index,unsigned int i){
  fuzz_u64 st=(index>=0&&index<MAX_LOG)?g_state[index]:0;
  return (unsigned char)((st>>((i&7u)*8u)) ^ (i*131u));
}
static fuzz_u64 canonical_snap_hash(raft_i64 index,unsigned int size){
  fuzz_u64 h=FUZZ_U64_C(14695981039346656037);
  unsigned int i;
  for(i=0;i<size;i++){ unsigned char b=snap_byte_for(index,i); h ^= b; h *= FUZZ_U64_C(1099511628211); }
  return h;
}

/* ---- cluster live state ---- */
#define MARKER_COOKIE ((const void*)(size_t)0x7FFF0001)
#define DRAIN_COOKIE  ((const void*)(size_t)0x7FFF0002)
static int g_ids[MAX_NODES]={1,2,3,4,5};
static node nodes[MAX_NODES];
static int n_nodes;
static queued_msg *pending[MAX_PENDING];
static int pending_count;
static int part_mask;   /* bitmask (bit id-1) of isolated nodes; 0 = no partition */
static int part_until;
static int part_mask2;  /* second independent partition (multi-way split); 0 = off */
static int part_until2;
static int g_leader;    /* 1-based id of the current leader, 0 = none (tail) */
static int g_marker;    /* 1 when the tail marker command COMMITTED */
/* g_clean is forward-declared near advance_elapsed() (single storage there). */

/* ---- targeted fault-injection (deterministic scenarios) ----
   The chaotic phase samples low-probability fault x extension SUPERPOSITIONS so
   sparsely that deep extension paths (joint x crash, snapshot-stream x drop,
   disruptive pre-vote, partitioned-leader read) are almost never hit.  These
   scenarios instead drive the cluster to a target state with the PUBLIC API,
   observe the Ready message stream for the critical event, and inject ONE
   deterministic fault at that point -- turning probabilistic coverage into
   per-run coverage.  The interception points read only public message fields
   (never raft_ctx internals): pure black-box. */
#define TGT_NONE        0
#define TGT_SNAP_DROP   1  /* drop one InstallSnapshot chunk to tgt_target mid-stream */
#define TGT_JOINT_CRASH 2  /* crash the leader mid joint-config-entry propagation */
#define TGT_READ_INDEX  3  /* a partitioned leader must not service a linearizable read */
#define TGT_READ_COOKIE ((const void*)(size_t)0x72000001u)
#define TGT_PRE_VOTE    4  /* a removed node's disruptive pre-vote must be suppressed */
#define TGT_TRANSFER_CRASH 5 /* crash the leader right after it hands off via TimeoutNow */
static int tgt_kind;       /* active scenario (TGT_*); TGT_NONE = off */
static int tgt_target;     /* 1-based node id the scenario aims at */
static int tgt_fired;      /* the targeted fault was injected */
static int tgt_crash_id;   /* joint scenario: leader to crash (set by interception) */
static int g_tgt_hits;     /* scenarios that actually fired their fault this seed (coverage) */
static int g_elect_fail;   /* tgt_elect_leader returned 0 before a scenario (no leader electable) */
static int g_queue_full;   /* times the perturbed-message queue hit MAX_PENDING (silent drop) */
/* Diagnostic-only back-off meter (failure dump): how far does a REJECTED
   AppendEntries move the leader's next_index for the replying peer?  The Sec
   5.3 conflict optimization jumps next_index to the follower's conflict index
   (one round-trip per conflicting TERM); a plain decrement of one entry per
   round-trip cannot repair a deeply divergent log inside one election timeout,
   which is how a quiescent cluster ends up re-electing forever instead of
   committing.  Global counters, printed by dump_logs. */
static int g_rej_count;           /* rejections observed */
static raft_i64 g_rej_dec_total;  /* summed next_index decrement */
static raft_i64 g_rej_dec_max;    /* largest single decrement (=1 => never jumps) */
/* Tail-scoped message counters: how much replication traffic does the FAULT-FREE
   tail actually carry?  A leader whose peers never answer walks its
   election_elapsed to the deadline and steps down (checkQuorum), which is
   indistinguishable from a follower timeout in a post-mortem unless the reply
   traffic is counted here. */
static raft_i64 g_tail_ae;     /* AppendEntries delivered during the tail */
static raft_i64 g_tail_ares;   /* AppendEntries RESULTs delivered during the tail */
static raft_i64 g_tail_rej;    /* ... of which rejected */
static raft_i64 g_tail_leader_ae_entries; /* tail: leader AEs carrying entries */
static raft_i64 g_tail_fol_acks;          /* tail: ACKs emitted by a non-leader */
static raft_i64 g_tail_fol_acks_ok;       /* ... of which success */
static raft_i64 g_tail_ares_leader;       /* tail: ACKs delivered to a node that IS leader */
static raft_i64 g_tail_ares_leader_intem; /* ... and in that leader's current term */
static raft_i64 g_tail_ack_to[16];        /* per-target in-term ACK deliveries (by node id) */
/* Churn meter for the fault-free tail: a quiet, fully connected cluster must
   settle on one leader.  If the term keeps advancing in the tail, the log
   divergence is being re-created faster than replication can repair it, and no
   liveness assertion about that tail can ever pass - so the failure message
   must carry this number, not just "the marker did not commit". */
static int g_tail_leader_changes;  /* leadership changes seen in the tail */
static int g_tail_prev_leader;     /* last leader id observed in the tail */
static raft_i64 g_tail_term_first; /* leader term at the first tail observation */
static raft_i64 g_tail_term_last;  /* leader term at the last tail observation */

static void dump_logs(void){
  int ni;
  fprintf(stderr,"NX BACKOFF: rejections=%d total_decrement=%" RAFT_I64_FMT " max_single_step=%" RAFT_I64_FMT "\n",
          g_rej_count,g_rej_dec_total,g_rej_dec_max);
  fprintf(stderr,"TAIL MSGS: append=%" RAFT_I64_FMT " append_result=%" RAFT_I64_FMT " (rejected=%" RAFT_I64_FMT ")\n",
          g_tail_ae,g_tail_ares,g_tail_rej);
  fprintf(stderr,"TAIL ACKS: leader_ae_with_entries=%" RAFT_I64_FMT " follower_acks=%" RAFT_I64_FMT " (success=%" RAFT_I64_FMT ")\n",
          g_tail_leader_ae_entries,g_tail_fol_acks,g_tail_fol_acks_ok);
  fprintf(stderr,"TAIL CONTACT: ack_to_leader=%" RAFT_I64_FMT " in_term=%" RAFT_I64_FMT "\n",
          g_tail_ares_leader,g_tail_ares_leader_intem);
  { int ai; for(ai=1;ai<=n_nodes;ai++)
      fprintf(stderr,"  in-term ACKs to node %d: %" RAFT_I64_FMT "\n",ai,g_tail_ack_to[ai]); }
  for(ni=0;ni<n_nodes;ni++){
    node *n=&nodes[ni];
    raft_log *L;
    raft_i64 idx;
    int pi;
    if(!n->r){ fprintf(stderr,"LOG node %d: CRASHED\n",n->id); continue; }
    L=&n->r->log;
    fprintf(stderr,"LOG node %d: term=%" RAFT_I64_FMT " state=%d commit=%" RAFT_I64_FMT " applied=%" RAFT_I64_FMT " LII=%" RAFT_I64_FMT " LIT=%" RAFT_I64_FMT " count=%" RAFT_I64_FMT " snap=%" RAFT_I64_FMT " el=%u elmax=%u pv=%d lc=%u defer=%d dlast=%" RAFT_I64_FMT " pgen=%u dur=%" RAFT_I64_FMT " dconf=%" RAFT_I64_FMT " cfg_old={",
            n->id,n->r->current_term,(int)n->r->state,
            n->r->commit_index,n->r->last_applied,
            L->last_included_index,L->last_included_term,L->count,
            n->r->snapshot.size,
            (unsigned)n->r->election_elapsed,(unsigned)n->r->election_deadline,(int)n->r->in_pre_vote,
            (unsigned)n->r->leader_contact_elapsed,
            (int)n->r->deferred_append_response,n->r->deferred_append_last_index,
            (unsigned)n->r->persist_gen,n->r->durable_index,n->r->durable_confirm);
    { int i; for(i=0;i<n->r->config_old.id_count;i++) fprintf(stderr,"%s%d",i?",":"",n->r->config_old.ids[i]); }
    fprintf(stderr,"} cfg_new={");
    { int i; for(i=0;i<n->r->config_new.id_count;i++) fprintf(stderr,"%s%d",i?",":"",n->r->config_new.ids[i]); }
    fprintf(stderr,"} joint=%d peers:",n->r->config_joint);
    for(pi=0;pi<n->r->peer_count;pi++){
      /* nx = next_index, the replication back-off cursor.  Diagnostics only:
         dump_logs already reads internals on the failure path.  nx is what
         distinguishes "replication converged" from "the leader is still
         walking next_index down" when a liveness check fires. */
      fprintf(stderr," %d(%s%s,m=%" RAFT_I64_FMT ",nx=%" RAFT_I64_FMT ",mr=%u,v=%d)",n->r->peers[pi].id,
              n->r->peers[pi].in_old_config?"O":"",n->r->peers[pi].in_new_config?"N":"",
              n->r->peers[pi].match_index,n->r->peers[pi].next_index,
              (unsigned)n->r->peers[pi].missed_rounds,
              (int)n->r->peers[pi].vote);
    }
    fprintf(stderr,"\n");
    for(idx=L->last_included_index+1;idx<=L->last_included_index+L->count;idx++){
      raft_i64 off=idx-L->last_included_index-1;
      int ci=(int)(off>>L->chunk_bits);
      int co=(int)(off&L->chunk_mask);
      if(ci>=L->num_chunks||!L->chunks[ci].terms) break;
      fprintf(stderr,"  [%" FUZZ_I64_FMT "] term=%" FUZZ_I64_FMT " kind=%d\n",(fuzz_i64)idx,
              (fuzz_i64)L->chunks[ci].terms[co],(int)L->chunks[ci].kinds[co]);
    }
  }
}

/* ---- oracle checks ---- */
static void cfgfmt(node *n,char *out){
  char *w=out;
  int i;
  *w++='{';
  for(i=0;i<n->r->config_new.id_count;i++){
    if(i>0) *w++=',';
    w+=sprintf(w,"%d",n->r->config_new.ids[i]);
  }
  *w++='}';
  *w=0;
}
static void check_apply(node *n,raft_i64 index,raft_i64 term,
                        const void *data,unsigned int size){
  fuzz_u64 h;
  char cb[32];
  if(index<1) return;
  if(index>=MAX_LOG)
    fail("APPLY: index %" FUZZ_I64_FMT " out of oracle range (MAX_LOG=%d)",index,MAX_LOG);
  cfgfmt(n,cb);
  ev("APPLY node %d idx %" FUZZ_I64_FMT " term %" FUZZ_I64_FMT " size %u commit %" FUZZ_I64_FMT " cfg%s",
     n->id,index,term,size,(fuzz_i64)n->r->commit_index,cb);
  h=hash_bytes(data,size);
  if(g_term_set[index]){
    if(g_term[index]!=term)
      fail("LOG MATCHING: index %" FUZZ_I64_FMT " applied with term %" FUZZ_I64_FMT " but term %" FUZZ_I64_FMT " was applied elsewhere (node %d)",
           index,term,g_term[index],n->id);
    if(g_hash[index]!=h)
      fail("STATE MACHINE SAFETY: index %" FUZZ_I64_FMT " term %" FUZZ_I64_FMT " data differs: node %d size %u hash %" FUZZ_U64_FMT " vs node %d size %u hash %" FUZZ_U64_FMT "",
           index,term,n->id,size,h,g_first_node[index],g_size[index],g_hash[index]);
  }else{
    g_term[index]=term;
    g_term_set[index]=1;
    g_hash[index]=h;
    g_size[index]=size;
    g_first_node[index]=n->id;
    g_state[index]=state_mix(g_state[index-1],index,term,h);
  }
}
static void check_leader(node *n,raft_i64 term){
  int i;
  char cb[64];
  int w=0,j;
  w+=sprintf(cb+w,"{");
  for(j=0;j<n->r->config_old.id_count;j++) w+=sprintf(cb+w,"%s%d",j?",":"",n->r->config_old.ids[j]);
  w+=sprintf(cb+w,"}/{");
  for(j=0;j<n->r->config_new.id_count;j++) w+=sprintf(cb+w,"%s%d",j?",":"",n->r->config_new.ids[j]);
  sprintf(cb+w,"} j=%d",n->r->config_joint);
  ev("LEADER node %d term %" FUZZ_I64_FMT " cfg %s",n->id,term,cb);
  for(i=0;i<g_leader_count;i++){
    if(g_leader_term[i]==term&&g_leader_id[i]!=n->id)
      fail("ELECTION SAFETY: two leaders in term %" FUZZ_I64_FMT " (nodes %d and %d)",
           term,g_leader_id[i],n->id);
  }
  if(g_leader_count>=MAX_LEADERS)
    fail("ELECTION SAFETY COVERAGE: term %" FUZZ_I64_FMT " exceeds MAX_LEADERS=%d (leader tracking overflow)",term,MAX_LEADERS);
  g_leader_term[g_leader_count]=term;
  g_leader_id[g_leader_count]=n->id;
  g_leader_count++;
  /* Leader's log-tip view (last_included_index / last_included_term /
     last_index / term_at[]) is maintained black-box from the Ready.persist
     output in process_ready, which runs before this check on the SAME advance
     (the leader's NOOP append always sets persist_needed).  No raft_inspect. */
  for(i=1;i<=g_cterm_max;i++){
    if(!g_cterm_set[i]) continue;
    /* Leader Completeness only binds a leader elected at term T to entries
       committed in EARLIER terms (< T).  An entry whose committed term is
       >= T may be committed later (in this or a future term), so a leader at
       term T is not required to already hold it.  (With a delayed/reordered
       network a term-8 vote can be granted before a term-9 commit yet be
       PROCESSED after it; skipping term_i >= T avoids that false positive.) */
    if(g_cterm[i]>=term) continue;
    if(i<=n->last_included_index){
      if(i==n->last_included_index&&n->last_included_term_known
         &&n->last_included_term!=g_cterm[i])
        fail("LEADER COMPLETENESS: leader %d term %" FUZZ_I64_FMT " has snapshot boundary term %" FUZZ_I64_FMT " at index %d, committed term is %" FUZZ_I64_FMT "",
             n->id,term,n->last_included_term,i,g_cterm[i]);
      continue; /* snapshot prefix: correct by induction, not directly verifiable */
    }
    if(i>n->last_index)
      fail("LEADER COMPLETENESS: leader %d term %" FUZZ_I64_FMT " is missing committed entry %d (log tip %" FUZZ_I64_FMT ")",
           n->id,term,i,n->last_index);
    else if(n->term_known[i]&&n->term_at[i]!=g_cterm[i])
      fail("LEADER COMPLETENESS: leader %d term %" FUZZ_I64_FMT " has term %" FUZZ_I64_FMT " at committed index %d, expected %" FUZZ_I64_FMT "",
           n->id,term,n->term_at[i],i,g_cterm[i]);
  }
}

/* ---- crash/restart: durable disk image + node lifecycle ---- */
/* Copy the persist view's log tail into either the committed or the pending
   disk image.  Entries are relative to that image's own last_included_index. */
static void disk_store_log(node *n,const raft_persist *p,int to_pending){
  int i,j;
  if(p->log_entry_count>MAX_LOG) fail("disk log overflow");
  if(to_pending) n->disk_pending_log_count=p->log_entry_count;
  else n->disk_log_count=p->log_entry_count;
  for(i=0;i<p->log_entry_count;i++){
    const raft_persist_entry *e=&p->log_entries[i];
    if(e->data_size>32) fail("disk entry data overflow");
    if(to_pending){
      n->disk_pending_index[i]=e->index;
      n->disk_pending_term_i[i]=e->term;
      n->disk_pending_kind[i]=(unsigned char)e->kind;
      n->disk_pending_dsize[i]=e->data_size;
      if(e->data_size>0&&e->data) memcpy(n->disk_pending_data[i],e->data,e->data_size);
      n->disk_pending_oldn[i]=e->cfg_old.id_count;
      for(j=0;j<e->cfg_old.id_count&&j<MAX_NODES;j++) n->disk_pending_oldcfg[i][j]=e->cfg_old.ids[j];
      n->disk_pending_newn[i]=e->cfg_new.id_count;
      for(j=0;j<e->cfg_new.id_count&&j<MAX_NODES;j++) n->disk_pending_newcfg[i][j]=e->cfg_new.ids[j];
      n->disk_pending_learnn[i]=e->cfg_learners.id_count;
      for(j=0;j<e->cfg_learners.id_count&&j<MAX_NODES;j++) n->disk_pending_learncfg[i][j]=e->cfg_learners.ids[j];
    }else{
      n->disk_index[i]=e->index;
      n->disk_term_i[i]=e->term;
      n->disk_kind[i]=(unsigned char)e->kind;
      n->disk_size[i]=e->data_size;
      if(e->data_size>0&&e->data) memcpy(n->disk_data[i],e->data,e->data_size);
      n->disk_old_n[i]=e->cfg_old.id_count;
      for(j=0;j<e->cfg_old.id_count&&j<MAX_NODES;j++) n->disk_old[i][j]=e->cfg_old.ids[j];
      n->disk_new_n[i]=e->cfg_new.id_count;
      for(j=0;j<e->cfg_new.id_count&&j<MAX_NODES;j++) n->disk_new[i][j]=e->cfg_new.ids[j];
      n->disk_learn_n[i]=e->cfg_learners.id_count;
      for(j=0;j<e->cfg_learners.id_count&&j<MAX_NODES;j++) n->disk_learn[i][j]=e->cfg_learners.ids[j];
    }
  }
}
/* THE single place that enforces the durable image's invariant: a CONTIGUOUS PREFIX starting at
   disk_lii+1.  Every path that mutates the image ends with this call, so an illegal image (a gap
   above the boundary, or entries at/below it) cannot escape - raft_create refuses such a log, and
   a refused restore in the fault-free tail leaves the node dead, kills the majority and shows up
   as a bogus LIVENESS failure. */
static void disk_image_normalize(node *n){
  int i,w=0;
  raft_i64 expect=n->disk_lii+1;
  for(i=0;i<n->disk_log_count;i++){
    if(n->disk_index[i]<=n->disk_lii) continue;
    if(n->disk_index[i]!=expect) break;      /* gap: everything after it is not restorable */
    if(w!=i){
      n->disk_index[w]=n->disk_index[i];
      n->disk_term_i[w]=n->disk_term_i[i];
      n->disk_kind[w]=n->disk_kind[i];
      n->disk_size[w]=n->disk_size[i];
      if(n->disk_size[i]>0) memcpy(n->disk_data[w],n->disk_data[i],(size_t)n->disk_size[i]);
      n->disk_old_n[w]=n->disk_old_n[i];
      n->disk_new_n[w]=n->disk_new_n[i];
      n->disk_learn_n[w]=n->disk_learn_n[i];
    }
    expect++; w++;
  }
  n->disk_log_count=w;
}
/* Merge the persist view's log entries into the COMMITTED disk log, deduped by
   absolute index.  Used by the snapshot_dirty path: a snapshot_dirty persist
   carries REAL durable log entries (the tail after the PROPOSED lii) in the
   same view as the snapshot proposal.  Those entries must land in the
   COMMITTED image even if the snapshot itself is never published (e.g. its
   publish hits an OOM and the node crashes before a retry); otherwise an entry
   that was already applied is silently lost on restart. */
static void disk_merge_log(node *n,const raft_persist *p){
  int i,j;
  for(i=0;i<p->log_entry_count;i++){
    const raft_persist_entry *e=&p->log_entries[i];
    int pos=-1;
    if(e->data_size>32) fail("disk entry data overflow");
    for(j=0;j<n->disk_log_count;j++) if(n->disk_index[j]==e->index){ pos=j; break; }
    if(pos<0){
      if(n->disk_log_count>=MAX_LOG) fail("disk log overflow");
      pos=n->disk_log_count;
      n->disk_log_count++;
    }
    n->disk_index[pos]=e->index;
    n->disk_term_i[pos]=e->term;
    n->disk_kind[pos]=(unsigned char)e->kind;
    n->disk_size[pos]=e->data_size;
    if(e->data_size>0&&e->data) memcpy(n->disk_data[pos],e->data,e->data_size);
    n->disk_old_n[pos]=e->cfg_old.id_count;
    for(j=0;j<e->cfg_old.id_count&&j<MAX_NODES;j++) n->disk_old[pos][j]=e->cfg_old.ids[j];
    n->disk_new_n[pos]=e->cfg_new.id_count;
    for(j=0;j<e->cfg_new.id_count&&j<MAX_NODES;j++) n->disk_new[pos][j]=e->cfg_new.ids[j];
    n->disk_learn_n[pos]=e->cfg_learners.id_count;
    for(j=0;j<e->cfg_learners.id_count&&j<MAX_NODES;j++) n->disk_learn[pos][j]=e->cfg_learners.ids[j];
  }
  /* TRUNCATE to the LIBRARY'S OWN LOG TIP, which is the only authority on how long the log is.
     Neither "the view does not carry entry X" (a plain persist view is a DELTA, an empty one is a
     heartbeat - that reading wiped durable config entries and produced the section F violations)
     nor "the view has a different term at index X" (an image entry can simply be STALE relative to
     the view - that reading discarded the committed index-4/term-9 entry and produced the seed-690
     LOG MATCHING failure) is sound evidence of a cut.  The library's log tip is: entries the
     library no longer has must go, entries it still has must stay, and merging replaces the term
     at each index. */
  {
    raft_i64 tip=n->r->log.last_included_index+(raft_i64)n->r->log.count;
    int w2=0;
    for(j=0;j<n->disk_log_count;j++){
      if((raft_i64)n->disk_index[j]>tip) continue;
      if(w2!=j){
        n->disk_index[w2]=n->disk_index[j];
        n->disk_term_i[w2]=n->disk_term_i[j];
        n->disk_kind[w2]=n->disk_kind[j];
        n->disk_size[w2]=n->disk_size[j];
        if(n->disk_size[j]>0) memcpy(n->disk_data[w2],n->disk_data[j],(size_t)n->disk_size[j]);
        n->disk_old_n[w2]=n->disk_old_n[j];
        n->disk_new_n[w2]=n->disk_new_n[j];
        n->disk_learn_n[w2]=n->disk_learn_n[j];
      }
      w2++;
    }
    n->disk_log_count=w2;
  }
}
/* Commit a PROPOSED snapshot (pending image) into the committed disk image.
   Only the SNAPSHOT (lii/lit/bytes/configs) comes from the pending buffer.
   The log tail is NOT copied from the pending capture - that capture is from
   snapshot_dirty time and misses entries appended between then and the publish
   (those entries are already in the committed disk log, written by the
   snapshot_dirty persist itself and any subsequent plain persists).  Instead
   the committed log tail is COMPACTED to the new boundary: entries at or
   before the new lii are covered by the snapshot and dropped, entries after it
   are preserved. */
static void disk_commit_pending(node *n){
  int i,j,k,changed,ci;
  raft_mask so,sn_,sl;
  so=raft_set_view(&n->r->snapshot.cfg_old);
  sn_=raft_set_view(&n->r->snapshot.cfg_new);
  sl=raft_set_view(&n->r->snapshot.cfg_learners);
  /* Detect a real publish: the library's committed snapshot differs from the
     committed disk image.  Drive off r->snapshot, NOT disk_pending_dirty: under
     async persist a delayed flush's raft_persist_complete can publish the
     snapshot BEFORE the matching snapshot_dirty persist view is emitted, so the
     pending buffer (and its dirty flag) is neither a reliable trigger nor a
     reliable source.  r->snapshot is authoritative at publish time. */
  changed=(n->r->snapshot.last_index!=n->disk_lii)
       ||(n->r->snapshot.last_term!=n->disk_lit)
       ||(so.id_count!=n->disk_snap_old_n)
       ||(sn_.id_count!=n->disk_snap_new_n)
       ||(sl.id_count!=n->disk_snap_learn_n);
  if(!changed){
    for(ci=0;ci<so.id_count&&ci<MAX_NODES;ci++)
      if(so.ids[ci]!=n->disk_snap_old[ci]){ changed=1; break; }
  }
  if(!changed){ n->disk_pending_dirty=0; return; }
  n->disk_lii=n->r->snapshot.last_index;
  n->disk_lit=n->r->snapshot.last_term;
  n->disk_snap_size=n->r->snapshot.size;
  /* Bytes: the library's freshly published snapshot image.  Under async
     persist the publish can be driven by a delayed flush's persist_complete
     BEFORE the snapshot_dirty persist view is emitted and BEFORE snap_tmp is
     copied into n->snap, so try sources in freshness order: the pending
     snap_tmp buffer (filled by fill_snap_tmp at raft_snapshot time), then the
     pending disk capture, then the already-published n->snap image, then the
     install-receive buffer snap_recv (an INSTALL may publish on a later
     advance than the one that recorded install_to, so the snap_recv->n->snap
     copy in phase 2 can be missed - snap_recv still holds the exact shipped
     bytes here, seed 148271). */
  if(n->snap_tmp_pending&&n->snap_tmp_size>0&&n->snap_tmp_size<=MAX_SNAP
     &&n->snap_tmp_size==(unsigned int)n->r->snapshot.size)
    memcpy(n->disk_snap,n->snap_tmp,(size_t)n->snap_tmp_size);
  else if(n->disk_pending_dirty&&n->disk_pending_size==(unsigned int)n->r->snapshot.size
          &&n->disk_pending_size>0&&n->disk_pending_size<=MAX_SNAP)
    memcpy(n->disk_snap,n->disk_snap_pending,(size_t)n->disk_pending_size);
  else if(n->r->snapshot.size>0&&n->r->snapshot.size<=MAX_SNAP
          &&n->snap_size==(unsigned int)n->r->snapshot.size)
    memcpy(n->disk_snap,n->snap,(size_t)n->r->snapshot.size);
  else if(n->snap_recv_size>0&&n->snap_recv_size<=MAX_SNAP
          &&n->snap_recv_size==(unsigned int)n->r->snapshot.size)
    memcpy(n->disk_snap,n->snap_recv,(size_t)n->snap_recv_size);
  else if(n->r->snapshot.size>0&&n->r->snapshot.size<=MAX_SNAP)
    fail("SNAPSHOT BYTES: publish source mismatch (size %" FUZZ_I64_FMT " not found in any app image)",
         (fuzz_i64)n->r->snapshot.size);
  n->disk_snap_old_n=so.id_count;
  for(i=0;i<so.id_count&&i<MAX_NODES;i++) n->disk_snap_old[i]=so.ids[i];
  n->disk_snap_new_n=sn_.id_count;
  for(i=0;i<sn_.id_count&&i<MAX_NODES;i++) n->disk_snap_new[i]=sn_.ids[i];
  n->disk_snap_learn_n=sl.id_count;
  for(i=0;i<sl.id_count&&i<MAX_NODES;i++) n->disk_snap_learn[i]=sl.ids[i];
  /* compact the committed log tail to entries strictly after the new lii */
  k=0;
  for(i=0;i<n->disk_log_count;i++){
    if(n->disk_index[i]<=n->disk_lii) continue;
    n->disk_index[k]=n->disk_index[i];
    n->disk_term_i[k]=n->disk_term_i[i];
    n->disk_kind[k]=n->disk_kind[i];
    n->disk_size[k]=n->disk_size[i];
    if(n->disk_size[i]>0) memcpy(n->disk_data[k],n->disk_data[i],(size_t)n->disk_size[i]);
    n->disk_old_n[k]=n->disk_old_n[i];
    for(j=0;j<n->disk_old_n[i]&&j<MAX_NODES;j++) n->disk_old[k][j]=n->disk_old[i][j];
    n->disk_new_n[k]=n->disk_new_n[i];
    for(j=0;j<n->disk_new_n[i]&&j<MAX_NODES;j++) n->disk_new[k][j]=n->disk_new[i][j];
    n->disk_learn_n[k]=n->disk_learn_n[i];
    for(j=0;j<n->disk_learn_n[i]&&j<MAX_NODES;j++) n->disk_learn[k][j]=n->disk_learn[i][j];
    k++;
  }
  n->disk_log_count=k;
  n->disk_pending_dirty=0;
  disk_image_normalize(n);
}
static void disk_persist(node *n,const raft_persist *p){
  int i;
  const unsigned char *src;
  n->disk_term=p->term;
  n->disk_voted_for=p->voted_for;
  if(p->snapshot_dirty){
    /* Before recording the NEW snapshot, land any async-deferred in-flight
       persist first: its entries (e.g. 5,6 below the snapshot boundary) are as
       durable as this snapshot's own commit, and skipping them would leave a
       non-contiguous log gap (disk 1..4 + tail[7]) that raft_create's restore
       rejects (raft.h:2692) - the node could never revive (seed 1700).  Merge
       the in-flight LOG ONLY (no raft_persist_complete: that would mutate
       raft's log and invalidate the still-unread ready.persist pointers). */
    if(n->persist_inflight){
      raft_persist tmp;
      memset(&tmp,0,sizeof(tmp));
      tmp.term=n->inflight.term;
      tmp.voted_for=n->inflight.voted_for;
      tmp.last_included_index=n->inflight.lii;
      tmp.last_included_term=n->inflight.lit;
      tmp.log_entries=n->inflight.entries;
      tmp.log_entry_count=n->inflight.log_count;
      disk_merge_log(n,&tmp);
      n->persist_inflight=0;
      n->inflight.log_count=0;
    }
    /* A NEW snapshot is being proposed (a local pending snapshot or a received
       install).  Record the FULL proposed image in the PENDING buffer; it is
       promoted to the committed image only once the library PUBLISHES it.  The
       library may abort a pending snapshot (e.g. on step-down), so the
       committed image must never be overwritten by an un-published snapshot -
       otherwise disk_lii and disk_snap_size diverge and a corrupted snapshot
       is restored and streamed out. */
    src=(n->r&&n->r->snapshot_install_pending)
        ?n->snap_recv
        :(n->snap_tmp_pending?n->snap_tmp:n->snap);
    n->disk_pending_lii=p->last_included_index;
    n->disk_pending_lit=p->last_included_term;
    n->disk_pending_size=p->snapshot_size;
    if(p->snapshot_size>0&&p->snapshot_size<=MAX_SNAP)
      memcpy(n->disk_snap_pending,src,(size_t)p->snapshot_size);
    n->disk_pending_old_n=p->snapshot_cfg_old.id_count;
    for(i=0;i<n->disk_pending_old_n&&i<MAX_NODES;i++) n->disk_pending_old[i]=p->snapshot_cfg_old.ids[i];
    n->disk_pending_new_n=p->snapshot_cfg_new.id_count;
    for(i=0;i<n->disk_pending_new_n&&i<MAX_NODES;i++) n->disk_pending_new[i]=p->snapshot_cfg_new.ids[i];
    n->disk_pending_learn_n=p->snapshot_cfg_learners.id_count;
    for(i=0;i<n->disk_pending_learn_n&&i<MAX_NODES;i++) n->disk_pending_learn[i]=p->snapshot_cfg_learners.ids[i];
    disk_store_log(n,p,1);
    /* The log entries in this snapshot_dirty view are REAL durable entries and
       must survive independently of whether the snapshot is ever published:
       merge them into the committed log too (the pending capture is discarded
       on crash). */
    disk_merge_log(n,p);
    n->disk_pending_dirty=1;
  }else{
    /* No new snapshot.  Promote any published pending snapshot, then refresh the
       committed image from the confirmed persist view. */
    disk_commit_pending(n);
    n->disk_lii=p->last_included_index;
    n->disk_lit=p->last_included_term;
    /* MERGE, never replace: a persist view may be a DELTA (e.g. only index 6 while 5 is already
       durable), and a heartbeat view carries nothing at all - replacing the image with the view
       therefore drops entries AND the membership they carry.  T2 (per-advance three-track probe)
       located the first divergence at the reconfig step: the library moved (3,3) -> (3,2) -> (2,2)
       while the image stayed (3,3), because disk_commit_pending only tracks SNAPSHOT membership;
       a pure log-entry config change never reached the image, so a crash restored the older
       membership and the node could win an election excluding the nodes with the committed log.
       disk_merge_log already merges by absolute index (entries carry their own cfg masks). */
    disk_merge_log(n,p);

  }
  disk_image_normalize(n);
  n->disk_dirty=1;
}
/* deep-copy a NON-snapshot-dirty persist view into an in-flight copy.  The
   copy's entries carry their own data + cfg-id storage (entries[].data and
   entries[].cfg_*.ids point INTO pc), so it outlives raft_ready_consumed. */
static void persist_copy_capture(persist_copy *pc,const raft_persist *p){
  int i,j;
  pc->term=p->term;
  pc->voted_for=p->voted_for;
  pc->lii=p->last_included_index;
  pc->lit=p->last_included_term;
  pc->log_count=p->log_entry_count;
  for(i=0;i<p->log_entry_count;i++){
    const raft_persist_entry *e=&p->log_entries[i];
    pc->entries[i].index=e->index;
    pc->entries[i].term=e->term;
    pc->entries[i].kind=e->kind;
    pc->entries[i].data_size=e->data_size;
    pc->entries[i].data=(e->data_size>0&&e->data)?pc->data[i]:0;
    if(e->data_size>0&&e->data) memcpy(pc->data[i],e->data,(size_t)e->data_size);
    pc->entries[i].cfg_old.ids=pc->old_ids[i];
    pc->entries[i].cfg_old.id_count=e->cfg_old.id_count;
    for(j=0;j<e->cfg_old.id_count&&j<MAX_NODES;j++) pc->old_ids[i][j]=e->cfg_old.ids[j];
    pc->entries[i].cfg_new.ids=pc->new_ids[i];
    pc->entries[i].cfg_new.id_count=e->cfg_new.id_count;
    for(j=0;j<e->cfg_new.id_count&&j<MAX_NODES;j++) pc->new_ids[i][j]=e->cfg_new.ids[j];
    pc->entries[i].cfg_learners.ids=pc->learn_ids[i];
    pc->entries[i].cfg_learners.id_count=e->cfg_learners.id_count;
    for(j=0;j<e->cfg_learners.id_count&&j<MAX_NODES;j++) pc->learn_ids[i][j]=e->cfg_learners.ids[j];
  }
}
/* Land a due in-flight persist: write it into the disk image AND report it via
   raft_persist_complete in one place, so the crash/restart source of truth and
   the library's durable_index never diverge (the fix for the earlier split
   "immediate disk_persist + delayed persist_complete" false-positive model). */
static void persist_copy_flush(node *n){
  raft_persist tmp;
  memset(&tmp,0,sizeof(tmp));
  tmp.term=n->inflight.term;
  tmp.voted_for=n->inflight.voted_for;
  tmp.last_included_index=n->inflight.lii;
  tmp.last_included_term=n->inflight.lit;
  tmp.log_entries=n->inflight.entries;
  tmp.log_entry_count=n->inflight.log_count;
  disk_persist(n,&tmp);
  n->inflight_durable=n->disk_lii+n->disk_log_count;   /* what the flush actually stored */
  (void)raft_persist_complete(n->r,n->inflight_durable);
  disk_commit_pending(n);
  n->persist_inflight=0;
}
static void cfg_for(raft_config *c,const node *n){
  memset(c,0,sizeof(*c));
  c->id=n->id;
  c->peers=g_ids;
  c->peer_count=n_nodes;
  c->heartbeat_ms=n->cfg_heartbeat;
  c->election_min_ms=n->cfg_e_min;
  c->election_max_ms=n->cfg_e_max;
  c->seed=n->cfg_seed;
  c->snapshot_chunk_size=n->cfg_snap_chunk;
  c->log_chunk_size=n->cfg_log_chunk;
}
static int node_create(node *n){
  raft_config cfg;
  cfg_for(&cfg,n);
  g_cur_node_idx=(int)(n-nodes);
  n->r=raft_create(&cfg);
  if(!n->r) return 0; /* OOM (injectable): leave the node uninitialized */
  n->alive=1;
  n->disk_dirty=0;
  n->disk_log_count=0;
  n->disk_pending_dirty=0;
  n->persist_inflight=0;
  n->last_applied=0;
  n->commit_index=0;
  n->last_index=0;
  n->last_included_index=0;
  n->last_included_term_known=0;
  n->cur_term=0;
  return 1;
}
static void node_crash(node *n){
  if(!n->alive||!n->r) return;
  out_abandon_node((int)(n-nodes)); /* a crash silently drops the node's pending requests */
  g_cur_node_idx=(int)(n-nodes);
  raft_destroy(n->r);
  n->r=0;
  n->alive=0;
  n->persist_inflight=0;   /* a crash drops the un-landed in-flight persist (slow fsync never landed) */
  ev("CRASH node %d",n->id);
}
static void node_restore(node *n){
  static raft_persist_entry re[MAX_LOG];
  static raft_persist rp;
  raft_config cfg;
  int i;
  if(n->alive) return;
  if(!n->disk_dirty){ (void)node_create(n); return; } /* never persisted: fresh node */
  memset(&rp,0,sizeof(rp));
  memset(re,0,sizeof(re));
  rp.term=n->disk_term;
  rp.voted_for=n->disk_voted_for;
  rp.last_included_index=n->disk_lii;
  rp.last_included_term=n->disk_lit;
  rp.snapshot_size=n->disk_snap_size;
  rp.snapshot_cfg_old.ids=(n->disk_snap_old_n>0)?n->disk_snap_old:0;
  rp.snapshot_cfg_old.id_count=n->disk_snap_old_n;
  rp.snapshot_cfg_new.ids=(n->disk_snap_new_n>0)?n->disk_snap_new:0;
  rp.snapshot_cfg_new.id_count=n->disk_snap_new_n;
  rp.snapshot_cfg_learners.ids=(n->disk_snap_learn_n>0)?n->disk_snap_learn:0;
  rp.snapshot_cfg_learners.id_count=n->disk_snap_learn_n;
  rp.log_entries=re;
  rp.log_entry_count=n->disk_log_count;
  for(i=0;i<n->disk_log_count;i++){
    re[i].index=n->disk_index[i];
    re[i].term=n->disk_term_i[i];
    re[i].kind=n->disk_kind[i];
    re[i].data_size=n->disk_size[i];
    re[i].data=(n->disk_size[i]>0)?n->disk_data[i]:0;
    re[i].cfg_old.ids=(n->disk_old_n[i]>0)?n->disk_old[i]:0;
    re[i].cfg_old.id_count=n->disk_old_n[i];
    re[i].cfg_new.ids=(n->disk_new_n[i]>0)?n->disk_new[i]:0;
    re[i].cfg_new.id_count=n->disk_new_n[i];
    re[i].cfg_learners.ids=(n->disk_learn_n[i]>0)?n->disk_learn[i]:0;
    re[i].cfg_learners.id_count=n->disk_learn_n[i];
  }
  disk_image_normalize(n);
  cfg_for(&cfg,n);
  cfg.restore=&rp;
  g_cur_node_idx=(int)(n-nodes);
  n->r=raft_create(&cfg);
  if(!n->r){
    return; /* OOM (injectable) or a refused image: stay dead, retry on a later restart */
  }
  n->alive=1;
  n->persist_inflight=0;   /* restart from the disk image: nothing is in-flight */
  ev("RESTART node %d term %" FUZZ_I64_FMT " lii %" FUZZ_I64_FMT " count %d",n->id,(fuzz_i64)n->disk_term,
     (fuzz_i64)n->disk_lii,n->disk_log_count);
  /* a local snapshot pending at crash time never finalized: drop it */
  n->snap_tmp_pending=0;
  n->snap_tmp_size=0;
  n->snap_tmp_index=0;
  n->snap_recv_size=0;
  /* restore app-owned snapshot bytes + the black-box log view */
  n->snap_size=0;
  if(n->disk_snap_size>0&&n->disk_snap_size<=MAX_SNAP){
    n->snap_size=(unsigned int)n->disk_snap_size;
    memcpy(n->snap,n->disk_snap,(size_t)n->snap_size);
  }
  n->snap_last_index=n->disk_lii;
  n->snap_last_term=n->disk_lit;
  n->last_applied=n->disk_lii;
  n->commit_index=n->disk_lii;
  n->last_included_index=n->disk_lii;
  n->last_included_term=n->disk_lit;
  n->last_included_term_known=1;
  n->cur_term=n->disk_term;
  /* A restart rebuilds the black-box term view from the disk image ONLY.
     Entries appended before the crash but never fsync'ed (async-fsync
     in-flight, dropped on crash) must NOT survive as term_known: otherwise an
     entry that was never committed is remembered at its old term and the
     g_cterm oracle mis-records it as committed (seed 9296).  Clear the whole
     view, then rebuild lii + the committed disk tail below. */
  memset(n->term_known,0,sizeof(n->term_known));
  memset(n->term_at,0,sizeof(n->term_at));
  n->last_index=n->disk_lii+n->disk_log_count;
  if(n->disk_lii>=1&&n->disk_lii<MAX_LOG){ n->term_at[n->disk_lii]=n->disk_lit; n->term_known[n->disk_lii]=1; }
  for(i=0;i<n->disk_log_count;i++){
    raft_i64 idx=n->disk_index[i];
    if(idx>=1&&idx<MAX_LOG){ n->term_at[idx]=n->disk_term_i[i]; n->term_known[idx]=1; }
  }
}
static void maybe_faults(void){
  /* crash a random live node */
  if(fuzz_rand_below(150)==0){
    int k=(int)fuzz_rand_below((unsigned int)n_nodes);
    node_crash(&nodes[k]);
  }
  /* restart a random dead node from its disk image */
  if(fuzz_rand_below(60)==0){
    int k=(int)fuzz_rand_below((unsigned int)n_nodes);
    if(!nodes[k].alive) node_restore(&nodes[k]);
  }
  /* rare full-cluster power loss (Sec. 8 persistence / crash consistency): crash
     EVERY node, then let the existing restart path (and the fault-free tail's
     restore-all) bring them back from their disk images.  This tests that the
     SET of images captured at DIFFERENT commit points still re-forms a
     recoverable, non-divergent cluster - the classic "whole datacenter lost
     power" scenario the single-node crash/restart model never hits. */
  if(fuzz_rand_below(2000)==0){
    int k;
    for(k=0;k<n_nodes;k++) node_crash(&nodes[k]);
    ev("POWER LOSS: all %d nodes crashed",n_nodes);
  }
}
static void maybe_oom(void){
  /* single-point OOM: fail exactly one of the next 64 library allocations */
  if(fuzz_oom_at==0&&fuzz_rand_below(300)==0)
    fuzz_oom_at=fuzz_alloc_calls+1+fuzz_rand_below(64);
}

/* ---- P1 config-immediate consistency oracle (gray-box, mirrors dump_logs).
   raft.h derives config_old/config_new/config_learners/config_joint from the
   LOG (raft_apply_config_from_log: the last CONFIG entry, else the snapshot
   base, else the bootstrap) at append/install/truncate time; a live config
   that disagrees with that derivation is a Sec. 4.1 config-immediate bug (e.g. a
   truncation that failed to revert the config, seed 3593 class).  The OOM
   rollback window (config_apply_stale) is skipped.  This check is a DIAGNOSTIC
   coupling to raft.h's config-derivation internals (it asserts an INTERNAL
   invariant -- live config == log-derived -- not an externally-observable
   safety violation), so it localizes an early break but would false-positive if
   raft.h ever legally changed its derivation while keeping external safety. ---- */
static void check_config_consistency(node *n){
  raft_log *L=&n->r->log;
  raft_mask e_old,e_new,e_learn;
  int e_joint,i,lc;
  raft_i64 idx;
  static int learn_buf[MAX_NODES];
  /* cheap invariants (valid even while config_apply_stale): sorted, in-range */
  for(i=1;i<n->r->config_old.id_count;i++)
    if(n->r->config_old.ids[i]<=n->r->config_old.ids[i-1])
      fail("CONFIG ORDER: node %d config_old ids not ascending",n->id);
  for(i=1;i<n->r->config_new.id_count;i++)
    if(n->r->config_new.ids[i]<=n->r->config_new.ids[i-1])
      fail("CONFIG ORDER: node %d config_new ids not ascending",n->id);
  for(i=0;i<n->r->config_old.id_count;i++)
    if(n->r->config_old.ids[i]<1||n->r->config_old.ids[i]>n_nodes)
      fail("CONFIG BOUNDS: node %d config_old id %d outside [1,%d]",n->id,n->r->config_old.ids[i],n_nodes);
  for(i=0;i<n->r->config_new.id_count;i++)
    if(n->r->config_new.ids[i]<1||n->r->config_new.ids[i]>n_nodes)
      fail("CONFIG BOUNDS: node %d config_new id %d outside [1,%d]",n->id,n->r->config_new.ids[i],n_nodes);
  /* strong check: live config == log-derived config */
  if(n->r->config_apply_stale) return;
  if(L->last_included_index>0&&raft_mask_any(raft_set_view(&n->r->snapshot.cfg_old))){
    e_old=raft_set_view(&n->r->snapshot.cfg_old);
    e_new=raft_set_view(&n->r->snapshot.cfg_new);
    e_learn=raft_set_view(&n->r->snapshot.cfg_learners);
  }else{
    e_old=raft_set_view(&n->r->config_bootstrap);
    e_new=e_old;
    e_learn=raft_mask_zero();
  }
  for(idx=L->last_included_index+L->count;idx>L->last_included_index;idx--){
    raft_i64 off=idx-L->last_included_index-1;
    int ci=(int)(off>>L->chunk_bits);
    int co=(int)(off&L->chunk_mask);
    if(ci>=L->num_chunks||!L->chunks[ci].terms) continue;
    if(L->chunks[ci].kinds[co]!=RAFT_ENTRY_CONFIG) continue;
    if(!L->chunks[ci].cfg_old) continue; /* mask-less: no-op */
    e_old=L->chunks[ci].cfg_old[co];
    e_new=L->chunks[ci].cfg_new[co];
    e_learn=raft_mask_zero();
    if(L->chunks[ci].cfg_learners) e_learn=L->chunks[ci].cfg_learners[co];
    break;
  }
  /* replicate raft_assign_learners_filtered: drop learners promoted to voters */
  lc=0;
  for(i=0;i<e_learn.id_count;i++) if(!raft_mask_has(e_new,e_learn.ids[i])) learn_buf[lc++]=e_learn.ids[i];
  e_learn.ids=lc>0?learn_buf:0;
  e_learn.id_count=lc;
  e_joint=!raft_mask_eq(e_old,e_new);
  if(!raft_mask_eq(raft_set_view(&n->r->config_old),e_old)
     ||!raft_mask_eq(raft_set_view(&n->r->config_new),e_new)
     ||!raft_mask_eq(raft_set_view(&n->r->config_learners),e_learn)
     ||n->r->config_joint!=e_joint)
    fail("CONFIG DIVERGENCE: node %d live config (joint=%d) != log-derived (joint=%d)",
         n->id,(int)n->r->config_joint,e_joint);
}

/* ---- one node: advance + full application-layer processing ---- */
static void enqueue(const raft_peer_message *m,int step);
static void process_ready(node *n,int step){
  raft_ready ready;
  int i;
  raft_i64 durable_index=0;   /* index reported to raft_persist_complete */
  raft_i64 apply_to=-1;       /* last apply index reported to raft_apply_complete */
  raft_i64 install_to=-1;     /* install index, or -1 */
  int persist_pending=0;
  int snapshot_persist_pending=0;  /* this persist carried a snapshot (snapshot_dirty) */
  raft_i64 cidx,nc;

  if(!n->alive||!n->r) return;
  g_cur_node_idx=(int)(n-nodes);   /* attribute library OOMs to this node (P2-1) */
  /* land a due asynchronous persist BEFORE advancing, so this step's commit
     computation sees the freshly-reported durable_index (the Sec. 10.2.1 window). */
  if(n->persist_inflight&&n->persist_due<=step) persist_copy_flush(n);
  /* In the fault-free tail every node advances by the SAME elapsed per step
     (g_step_elapsed): per-node independent random elapsed is unbounded clock
     DRIFT, and Raft's liveness ("broadcastTime << electionTimeout") assumes
     bounded drift - with independent random clocks a follower legitimately
     times out against a healthy leader forever, so a liveness assertion in
     that tail can never hold.  The chaos phase keeps the independent clocks
     (safety must hold under ANY timing). */
  if(g_clean&&g_step_elapsed) (void)raft_advance(n->r,g_step_elapsed,&ready);
  else (void)raft_advance(n->r,advance_elapsed(),&ready);
  if(ready.is_leader) g_leader=n->id;

  /* ---- phase 1: read ALL zero-copy Ready data BEFORE any state-mutating
     call.  raft_persist_complete can compact the log (freeing apply_entries'
     command storage) and raft_apply_complete can install a snapshot (freeing
     the persist view), so persist/apply/messages are all consumed first. ---- */

  /* persist view: record terms + snapshot metadata, and capture the durable
     disk image (deep copy, survives a crash/restart). */
  if(ready.persist_needed){
    raft_persist *p=&ready.persist;
    raft_i64 live_last=p->last_included_index;
    raft_i64 j;
    int k;
    n->cur_term=p->term;
    if(p->log_entry_count>0) live_last=p->log_entries[p->log_entry_count-1].index;
    if(p->last_included_index>=1&&p->last_included_index<MAX_LOG){
      n->term_at[p->last_included_index]=p->last_included_term;
      n->term_known[p->last_included_index]=1;
      n->last_included_index=p->last_included_index;
      n->last_included_term=p->last_included_term;
      n->last_included_term_known=1;
    }
    for(k=0;k<p->log_entry_count;k++){
      raft_i64 idx=p->log_entries[k].index;
      if(idx>=1&&idx<MAX_LOG){ n->term_at[idx]=p->log_entries[k].term; n->term_known[idx]=1; }
    }
    /* P0-1 config log-matching: a CONFIG entry's payload is the membership
       change.  check_apply cannot see it (apply entries carry kind but no cfg
       masks, and the command payload is empty), so hash it here from the
       persist view and require every node COMMITTING the same index to agree.
       Only COMMITTED entries (idx <= ready.commit_index) are recorded: an
       uncommitted config entry may be legitimately truncated and replaced at
       the same index (no committed state is overwritten), which would
       otherwise read as a phantom divergence. */
    for(k=0;k<p->log_entry_count;k++){
      const raft_persist_entry *e=&p->log_entries[k];
      raft_i64 idx=e->index;
      if(e->kind!=RAFT_ENTRY_CONFIG) continue;
      if(idx<1||idx>=MAX_LOG) continue;
      if(idx>ready.commit_index) continue;   /* uncommitted: mutable, skip */
      if(g_cfg_set[idx]){
        if(g_cfg_hash[idx]!=cfg_hash(e->cfg_old,e->cfg_new,e->cfg_learners))
          fail("CONFIG LOG MATCHING: index %" FUZZ_I64_FMT " has different config entries on different nodes",(fuzz_i64)idx);
      }else{
        g_cfg_hash[idx]=cfg_hash(e->cfg_old,e->cfg_new,e->cfg_learners);
        g_cfg_set[idx]=1;
      }
    }
    /* P1-3 snapshot config consistency: two nodes snapshotting the same
       boundary index must carry identical membership metadata (the byte-level
       snapshot oracle covers command bytes only, not config). */
    if(p->snapshot_dirty&&p->last_included_index>=1&&p->last_included_index<MAX_LOG){
      fuzz_u64 h=cfg_hash(p->snapshot_cfg_old,p->snapshot_cfg_new,p->snapshot_cfg_learners);
      raft_i64 li=p->last_included_index;
      if(g_snap_cfg_set[li]){
        if(g_snap_cfg_hash[li]!=h)
          fail("SNAPSHOT CONFIG: index %" FUZZ_I64_FMT " snapshot config differs between nodes",(fuzz_i64)li);
      }else{
        g_snap_cfg_hash[li]=h;
        g_snap_cfg_set[li]=1;
      }
    }
    /* terms beyond the live suffix were truncated since the last persist */
    for(j=live_last+1;j<=n->last_index&&j<MAX_LOG;j++) n->term_known[j]=0;
    /* The black-box log tip must come from the LIBRARY'S LOG, not from this persist view: a plain
       persist view is a delta and an empty one is a heartbeat, so view-derived tips regress to the
       snapshot boundary (observed: a restarted leader's tip read 12 while its log held 13/14, and
       the leader-completeness oracle then reported a missing committed entry - seed 914). */
    n->last_index=n->r->log.last_included_index+(raft_i64)n->r->log.count;
    if(p->snapshot_dirty){
      n->snap_last_index=p->last_included_index;
      n->snap_last_term=p->last_included_term;
      if(p->snapshot_size>0&&p->snapshot_size<=MAX_SNAP)
        n->snap_size=(unsigned int)p->snapshot_size;
    }
    /* Delay the fsync only in the CHAOS phase, only for a node WITH voter
       peers, and only when the TERM is unchanged.  Sec. 10.2.1 allows deferring
       the LOG entries; but Sec. 3.8 requires a term/voted_for change to be
       persisted immediately (a node that votes and then crashes before the
       term lands must NOT "forget" its vote, or two leaders could be elected
       in the same term - cluster-fuzz seed 344).  So a term bump lands
       synchronously; a same-term log append may land late. */
    if(!p->snapshot_dirty&&g_persist_delay>0&&raft_has_voter_peer(n->r)&&!g_clean
       &&p->term==n->disk_term){
      unsigned int d=fuzz_rand_below(g_persist_delay+1);
      if(d>0){
        /* delay the fsync: capture and land it later; keep advancing. */
        persist_copy_capture(&n->inflight,p);
        n->inflight_durable=live_last;
        n->persist_inflight=1;
        n->persist_due=step+d;
        /* Nothing has landed yet: the durable prefix is whatever the image holds now. */
        n->inflight_durable=n->disk_lii+n->disk_log_count;
      }else{
        disk_persist(n,p);
        /* Report the NORMALISED image prefix, never the live log tip: the leader must not count
           an entry the app has not actually stored. */
        durable_index=n->disk_lii+n->disk_log_count;
        persist_pending=1;
        n->persist_inflight=0;   /* an immediate land supersedes any in-flight view */
      }
    }else{
      disk_persist(n,p);
      /* A snapshot_dirty persist durables only its SNAPSHOT boundary, not the
         log tail beyond it: the tail is durable only once the snapshot itself
         is published (a contiguous-prefix guarantee - reporting the tail tip
         here would let a later leader commit entries past a snapshot boundary
         that never landed, the fragmented-log seed 2233).  raft.h publishes
         the snapshot exactly when durable_index >= snapshot_pending.last_index,
         so report the proposed lii and let the tail's durability catch up via
         the next non-snapshot persist. */
      durable_index=(p->snapshot_dirty?p->last_included_index:(n->disk_lii+n->disk_log_count));
      snapshot_persist_pending=p->snapshot_dirty;
      persist_pending=1;
      n->persist_inflight=0;
    }
  }
  /* apply entries: hash into the oracle while their storage is still valid */
  if(ready.apply_count>0){
    raft_i64 prev=n->last_applied;
    for(i=0;i<ready.apply_count;i++){
      const raft_apply_entry *e=&ready.apply_entries[i];
      if(e->index!=prev+1)
        fail("APPLY ORDER: node %d applied index %" FUZZ_I64_FMT " after %" FUZZ_I64_FMT " (gap/overlap)",
             n->id,e->index,prev);
      check_apply(n,e->index,e->term,e->command,e->command_size);
      prev=e->index;
    }
    apply_to=ready.apply_entries[ready.apply_count-1].index;
  }

  /* serve snapshot-stream reads (emits InstallSnapshot messages; read-only on
     the log, so apply pointers above remain valid) */
  for(i=0;i<ready.snapshot_read_count;i++){
    const raft_snapshot_read_req *sr=&ready.snapshot_reads[i];
    raft_i64 off=sr->byte_offset;
    unsigned int cnt=sr->byte_count;
    if(off>=0&&off<=(raft_i64)n->snap_size){
      unsigned int avail=n->snap_size-(unsigned int)off;
      /* Sec. 7 / Sec. 5.1: a snapshot read references the library's CURRENT committed
         snapshot (sr->last_included_index).  n->snap holds the bytes for
         n->snap_last_index.  Under async persist the library can publish a NEW
         snapshot while the app's byte image has not yet been copied (publish
         lag): serving bytes for a different index would stream a mislabeled
         snapshot (cluster-fuzz seed 101, SNAPSHOT BYTES mismatch).  Drop the
         stale read; the library re-issues it against the new index once the
         app's image catches up. */
      if(sr->last_included_index!=n->snap_last_index) continue;
      if(cnt>avail) cnt=avail;
      (void)raft_snapshot_data_provided(n->r,sr->follower_id,off,n->snap+off,cnt);
    }
  }

  /* record a pending snapshot install + verify its boundary term */
  if(ready.snapshot_install_needed){
    raft_i64 li=ready.snapshot_last_index;
    ev("INSTALL node %d idx %" FUZZ_I64_FMT " term %" FUZZ_I64_FMT "",n->id,li,ready.snapshot_last_term);
    if(li>=1&&li<MAX_LOG&&g_term_set[li]&&g_term[li]!=ready.snapshot_last_term)
      fail("SNAPSHOT CONSISTENCY: install boundary index %" FUZZ_I64_FMT " term %" FUZZ_I64_FMT " vs committed term %" FUZZ_I64_FMT "",
           li,ready.snapshot_last_term,g_term[li]);
    /* byte-level snapshot oracle: the installed image must equal the canonical
       applied-state image at li (catches wrong-copy/offset/truncation).  The
       app copies only chunks the library accepted, so snap_recv is never a mix
       of rejected streams. */
    if(li>=0&&li<MAX_LOG&&n->snap_recv_size>0){
      unsigned int want=snap_size_for(li);
      fuzz_u64 got=hash_bytes(n->snap_recv,n->snap_recv_size);
      fuzz_u64 wh=canonical_snap_hash(li,want);
      if(n->snap_recv_size!=want||got!=wh)
        fail("SNAPSHOT BYTES: node %d installed %u bytes at index %" FUZZ_I64_FMT " (canonical %u bytes, hash %" FUZZ_U64_FMT " vs %" FUZZ_U64_FMT ")",
             n->id,n->snap_recv_size,li,want,got,wh);
    }
    install_to=li;
  }

  /* leadership transitions: check Election Safety + Leader Completeness.
     Use the library's ACTUAL current_term, not the persist-derived n->cur_term:
     cur_term is refreshed only from the persist view, which can lag the live
     term (persist is consumed asynchronously across advances), so it would
     mis-record a leader at the PREVIOUS term and false-positive "two leaders
     in one term".  current_term is always authoritative. */
  if(ready.leader_change&&ready.is_leader) check_leader(n,n->r->current_term);

  /* tail-liveness marker detection (black-box, via client results) */
  for(i=0;i<ready.client_result_count;i++){
    const raft_client_result *cr=&ready.client_results[i];
    int oi;
    if(cr->cookie==MARKER_COOKIE
       &&cr->status==RAFT_CLIENT_COMMITTED) g_marker=1;
    /* P0-3 tail-transfer result detection (deterministic, submitted only in
       the stable tail by run_cluster after the marker commits). */
    if(cr->cookie==XFER_COOKIE){
      g_xfer_resolved=1;
      g_xfer_status=(int)cr->status;
    }
    /* TGT_READ_INDEX: a partitioned leader MUST NOT service a linearizable read
       (Sec. 6.4).  READY requires a FRESH heartbeat quorum (read_barrier_gen must
       advance past the barrier's needs_gen), which an isolated leader cannot
       obtain -- so a barrier READY in the window is a stale-read violation the
       positive-space oracles (SM safety / P0-2) do not see. */
    if(tgt_kind==TGT_READ_INDEX&&cr->cookie==TGT_READ_COOKIE
       &&cr->status==RAFT_CLIENT_READY)
      fail("LINEARIZABLE READ: partitioned leader %d served a barrier READY (Sec. 6.4 violation)",
           n->id);
    /* P0-1: every ACCEPTED request must eventually get a terminal result
       (COMMITTED/REDIRECT/FAILED/READY/CATCHUP_*).  A request whose node
       crashes is abandoned by node_crash. */
    oi=out_find(cr->cookie,(int)(n-nodes));
    if(oi>=0){
      /* P0-2 (Sec. 6.4): a barrier READY result must reflect every entry
         committed before submission - the state machine must have applied
         at least the submission-time commit lower bound. */
      if(g_out[oi].kind==RAFT_CLIENT_BARRIER&&cr->status==RAFT_CLIENT_READY){
        if(n->last_applied<g_out[oi].submit_commit)
          fail("READ LINEARIZABILITY: node %d barrier READY at last_applied %" FUZZ_I64_FMT " below submission commit %" FUZZ_I64_FMT "",
               n->id,n->last_applied,g_out[oi].submit_commit);
      }
      out_resolve(cr->cookie,(int)(n-nodes));
      ev("RESOLVE node %d cookie %p status %d",n->id,cr->cookie,(int)cr->status);
    }
  }

  /* deep-copy outbound messages.  Use the LIVE r->msg_count (not the stale
     ready.message_count): raft_snapshot_data_provided above appends
     InstallSnapshot chunks to r->msg_buf AFTER raft_advance assembled the
     Ready view, so the old count would drop every outgoing chunk. */
  for(i=0;i<n->r->msg_count;i++){
    const raft_peer_message *tm=&n->r->msg_buf[i];
    /* Tail liveness meters (Ready output only): how many entry-bearing
       AppendEntries does the leader emit, and how many ACKs do the followers
       answer with?  A follower that keeps receiving entries but never ACKs
       starves the leader's quorum contact, so the leader walks its
       election_elapsed to the deadline and steps down (per-term churn). */
    if(g_clean&&ready.is_leader&&tm->type==RAFT_MSG_APPEND&&tm->append_entries.entry_count>0)
      g_tail_leader_ae_entries++;
    if(g_clean&&!ready.is_leader&&tm->type==RAFT_MSG_APPEND_RESULT){
      g_tail_fol_acks++;
      if(tm->append_entries_result.success) g_tail_fol_acks_ok++;
    }
    /* targeted fault: drop a mid-stream InstallSnapshot chunk to the armed
       follower.  Prefer offset>0 (a chunk the follower has not yet acked) so the
       drop exercises raft.h's offset-recovery / restart-stream path; a
       single-chunk snapshot is still dropped and re-streamed whole. */
    if(tgt_kind==TGT_SNAP_DROP&&!tgt_fired
       &&tm->type==RAFT_MSG_INSTALL_SNAPSHOT&&tm->to==tgt_target){
      const raft_install_snapshot *is=&tm->install_snapshot;
      if(is->snapshot_offset>0||is->snapshot_done){
        tgt_fired=1;
        ev("TGT DROP SNAPSHOT off=%" FUZZ_I64_FMT " done=%d to node %d",
           (fuzz_i64)is->snapshot_offset,(int)is->snapshot_done,tgt_target);
        continue;   /* drop: skip enqueue */
      }
    }
    /* targeted fault: crash the leader the moment it broadcasts a JOINT config
       entry (an AppendEntries whose CONFIG entry carries C_old != C_new).  Drop
       this one broadcast so the joint entry reaches only SOME old-config
       followers, then the scenario crashes the sender -- the exact Sec. 4.3 window
       joint consensus exists to make safe. */
    if(tgt_kind==TGT_JOINT_CRASH&&!tgt_fired
       &&tm->type==RAFT_MSG_APPEND&&tm->from==tgt_target){
      const raft_append_entries *ae=&tm->append_entries;
      int j;
      for(j=0;j<ae->entry_count;j++){
        if(ae->entry_kinds[j]==RAFT_ENTRY_CONFIG
           &&ae->entry_cfg_old&&ae->entry_cfg_new
           &&!raft_mask_eq(ae->entry_cfg_old[j],ae->entry_cfg_new[j])){
          tgt_fired=1;
          tgt_crash_id=tm->from;
          ev("TGT JOINT CRASH: drop joint entry (slot %d) from %d to %d",
             j,tm->from,tm->to);
          break;
        }
      }
      if(tgt_fired) continue;   /* drop this joint-entry broadcast */
    }
    /* TGT_PRE_VOTE: a removed node may pre-vote (expected; this is the fired
       signal), but it must never escalate to a REAL vote (pre_vote==0): the
       voters' heartbeat grace period (Sec. 4.2.3, leader_contact_elapsed <
       election_min) must keep suppressing it.  A real vote here means the
       suppression failed. */
    if(tgt_kind==TGT_PRE_VOTE&&tm->type==RAFT_MSG_REQUEST_VOTE
       &&tm->from==tgt_target){
      if(!tm->request_vote.pre_vote)
        fail("PRE-VOTE: removed node %d escalated to a real vote (Sec. 4.2.3 grace-period failure)",
             tgt_target);
      tgt_fired=1;   /* the removed node pre-voted: scenario reached its state */
    }
    /* TGT_TRANSFER_CRASH: crash the leader right after it hands leadership off
       via TimeoutNow (Sec. 3.10).  The target has already caught up (match_index >=
       last), so it can campaign at once; the in-flight TimeoutNow still reaches
       it.  Exercising this handoff's crash tolerance: the old leader dies after
       yielding, and the target must still take over cleanly. */
    if(tgt_kind==TGT_TRANSFER_CRASH&&!tgt_fired
       &&tm->type==RAFT_MSG_TIMEOUT_NOW&&tm->to==tgt_target){
      tgt_fired=1;
      tgt_crash_id=tm->from;
      ev("TGT TRANSFER CRASH: TimeoutNow from %d to %d",tm->from,tm->to);
      /* do NOT drop: let the TimeoutNow reach the target so it campaigns */
    }
    enqueue(&n->r->msg_buf[i],step);
  }
  /* ---- phase 2: completion callbacks (may free/realloc internal buffers).
     raft_ready_consumed runs FIRST: it only resets counts, and results emitted
     by raft_apply_complete below must SURVIVE to the next advance (otherwise
     ready_consumed would wipe result_count and the COMMITTED result is lost -
     the same advance->consume->apply->advance discipline the test suite uses). ---- */
  raft_ready_consumed(n->r);
  if(persist_pending){
    (void)raft_persist_complete(n->r,durable_index);

    if(snapshot_persist_pending){
      (void)raft_snapshot_persist_complete(n->r,durable_index);
      snapshot_persist_pending=0;
    }
    /* Promote a pending disk snapshot the moment the library publishes it
       (compacts the log to its index), closing the crash window between the
       snapshot_dirty persist and the next plain persist. */
    disk_commit_pending(n);
  }
  if(n->snap_tmp_pending){
    /* publish only once the library finalized the pending snapshot (its index
       AND size now match); otherwise retry on a later persist.  The index check
       is load-bearing: two snapshots at different indices can share a byte
       size, and matching size alone would publish the NEW bytes against the
       OLD (still-current) snapshot index and stream them out mislabeled. */
    if(n->r->snapshot.last_index==n->snap_tmp_index
       &&n->r->snapshot.size==(raft_i64)n->snap_tmp_size){
      n->snap_size=n->snap_tmp_size;
      memcpy(n->snap,n->snap_tmp,(size_t)n->snap_size);
      n->snap_tmp_pending=0;
      /* The published boundary must track the library's snapshot even when the
         publish arrived via a delayed-flush persist_complete (no snapshot_dirty
         persist reached the app first): otherwise sr->last_included_index never
         equals snap_last_index, every snapshot read is dropped, and a lagging
         C_new voter can never be caught up (joint-config livelock, seed 71548). */
      n->snap_last_index=n->r->snapshot.last_index;
      n->snap_last_term=n->r->snapshot.last_term;
    }
  }
  if(apply_to>=0){
    n->last_applied=apply_to;
    (void)raft_apply_complete(n->r,apply_to);
  }
  if(install_to>=0){
    (void)raft_apply_complete(n->r,install_to);
    /* Publish the installed bytes + advance last_applied only when the library
       ACTUALLY completed the install: its committed snapshot boundary reached
       install_to (r->snapshot.last_index == install_to).  "Completed" means the
       compaction succeeded and r->snapshot was replaced by the installed image.
       Two wrong signals were rejected:
         - snapshot.size == snap_recv_size: sizes can match by coincidence for a
           FAILED install (OOM deferred it), mis-tagging it complete and making
           the next apply look like a duplicate (seed 14190, "N after N");
         - r->last_applied >= install_to: a STALE install (install_to < current
           boundary) is skipped by raft.h, so last_applied is already > install_to
           even though nothing was installed -- advancing n->last_applied to
           install_to here would REWIND it (seed 265, "3 after 1").
       Keep snap_recv + last_applied for the retry when the install is deferred. */
    if(n->r->snapshot.last_index==install_to){
      if(n->snap_recv_size>0){
        if(n->last_applied<install_to) n->last_applied=install_to;
        n->snap_size=n->snap_recv_size;
        memcpy(n->snap,n->snap_recv,(size_t)n->snap_size);
        n->snap_recv_size=0;
        n->snap_last_index=n->r->snapshot.last_index;
        n->snap_last_term=n->r->snapshot.last_term;
      }
    }
    /* Promote a pending disk snapshot the moment the install PUBLISHES it
       (r->snapshot.last_index reaches the pending lii), mirroring the
       persist_complete path for LOCAL snapshots.  The install's snapshot_dirty
       persist is recorded in the pending buffer, and raft_persist_complete does
       NOT publish installs - only raft_apply_complete(install_to) does.  Without
       this the pending snapshot is never promoted and a crash before the next
       plain persist restores the OLD snapshot + a stale log tail (the entries
       the install's coalesced persist should have retired, e.g. a truncated
       entry that was overwritten at a higher term). */
    disk_commit_pending(n);
  }
  /* P1-2: record the COMMITTED term (not just applied) so the leader-
     completeness oracle also covers entries committed but not yet applied.
     A node's commit_index advances only after the entry is persisted, so
     term_at[] (maintained from the persist view above) is authoritative. */
  nc=n->r->commit_index;
  for(cidx=n->commit_index+1;cidx<=nc&&cidx<MAX_LOG;cidx++){
    if(n->term_known[cidx]&&!g_cterm_set[cidx]){
      g_cterm[cidx]=n->term_at[cidx];
      g_cterm_set[cidx]=1;
      if(cidx>g_cterm_max) g_cterm_max=cidx;
    }
  }
  n->commit_index=nc;
  if(nc>g_commit_max) g_commit_max=nc;
  /* NOTE (removed-away leader-membership oracle): "leader must be a voter in
     its committed config" is NOT a Raft safety invariant and is deliberately
     NOT checked.  Sec. 4.2.4/Sec. 4.3 allow a removed server whose log is up-to-date
     to win an election and lead (and a C_old-only candidate to win a joint
     election); the library relies on self-removal step-down at APPLY time and
     checkQuorum, and a removed leader's commits are still quorum-valid.  The
     membership-safety checks live in P0-1 (config log-matching) and P1-3
     (snapshot config consistency) instead. */
  /* P1: config-immediate consistency (gray-box, Sec. 4.1) */
  check_config_consistency(n);
}

/* ---- perturbed message queue ---- */
static void push_msg(const raft_peer_message *m,int deliver_at){
  queued_msg *q;
  if(pending_count>=MAX_PENDING){
    /* coverage visibility: a full in-flight queue silently drops this message
       (equivalent to the explicit 15% drop, but let the trace show it) */
    g_queue_full++;
    ev("QUEUE FULL: dropped msg type %d (pending=%d)",(int)m->type,pending_count);
    return;
  }
  q=msg_clone(m,deliver_at);
  if(!q) return;
  pending[pending_count++]=q;
}
static void enqueue(const raft_peer_message *m,int step){
  unsigned int r;
  if(g_clean){ push_msg(m,step); return; }   /* fault-free tail: deliver now */
  r=fuzz_rand_below(100);
  if(r<15) return;                                            /* 15% drop */
  push_msg(m,step+(r<28?(int)fuzz_rand_below(4):0));          /* ~13% delay 0..3 */
  if(r>=90) push_msg(m,step+1+(int)fuzz_rand_below(3));       /* ~10% duplicate */
}
/* Back-off meter (see g_rej_dec_max): compare the leader's next_index for the
   replying peer across one rejected AppendEntries delivery. */
static void note_reject_backoff(node *leader,int from_id,raft_i64 before){
  int pk;
  for(pk=0;pk<leader->r->peer_count;pk++){
    if(leader->r->peers[pk].id==from_id){
      raft_i64 dec=before-leader->r->peers[pk].next_index;
      g_rej_count++;
      if(dec>0) g_rej_dec_total+=dec;
      if(dec>g_rej_dec_max) g_rej_dec_max=dec;
      return;
    }
  }
}
static void deliver_one(queued_msg *q){
  int to=q->m.to;
  if(part_mask||part_mask2){
    int fa=(q->m.from>=1&&q->m.from<=n_nodes)&&((part_mask>>(q->m.from-1))&1);
    int ta=(q->m.to>=1&&q->m.to<=n_nodes)&&((part_mask>>(q->m.to-1))&1);
    int fa2=(q->m.from>=1&&q->m.from<=n_nodes)&&((part_mask2>>(q->m.from-1))&1);
    int ta2=(q->m.to>=1&&q->m.to<=n_nodes)&&((part_mask2>>(q->m.to-1))&1);
    if((part_mask&&fa!=ta)||(part_mask2&&fa2!=ta2)){ msg_free(q); return; } /* cross-partition: drop */
  }
  if(to>=1&&to<=n_nodes){
    node *t=&nodes[to-1];
    if(!t->alive||!t->r){ msg_free(q); return; }
    g_cur_node_idx=(int)(t-nodes);
    if(q->m.type==RAFT_MSG_INSTALL_SNAPSHOT){
      raft_install_snapshot *is=&q->m.install_snapshot;
      raft_i64 off=is->snapshot_offset;
      unsigned int n=(is->snapshot_chunk_size>0)?(unsigned int)is->snapshot_chunk_size:0;
      raft_i64 prev_lii=t->r->snapshot_recv.last_index;
      (void)raft_recvfrom_peer(t->r,&q->m);
      /* If the library switched to a DIFFERENT snapshot stream (a delayed /
         duplicated chunk from an older stream, or a replacement snapshot),
         discard the partially-built image: the new stream restarts at offset 0
         and rebuilds it.  Without this, a stale chunk accepted after a reset
         leaves bytes from the OLD stream mixed into snap_recv, and the
         byte-level oracle false-positives. */
      if(t->r->snapshot_recv.last_index!=prev_lii) t->snap_recv_size=0;
      /* copy ONLY the chunks the library actually accepted (its expected offset
         advanced to off+n), so the app's snap_recv never mixes bytes from a
         rejected/different stream - otherwise the byte-level snapshot oracle
         would false-positive on dropped/reordered chunks. */
      if(off>=0&&n>0&&(fuzz_u64)off+n<=MAX_SNAP
         &&t->r->snapshot_recv_expected_offset==off+(raft_i64)n){
        memcpy(t->snap_recv+off,is->snapshot_data,n);
        if(is->snapshot_done){
          if(is->snapshot_data_size>=0&&is->snapshot_data_size<=MAX_SNAP)
            t->snap_recv_size=(unsigned int)is->snapshot_data_size;
          t->snap_last_index=is->snapshot_last_index;
          t->snap_last_term=is->snapshot_last_term;
        }
      }
    }else{
      raft_i64 nx_before=-1;
      if(q->m.type==RAFT_MSG_APPEND&&g_clean) g_tail_ae++;
      if(q->m.type==RAFT_MSG_APPEND_RESULT&&g_clean){
        g_tail_ares++;
        if(!q->m.append_entries_result.success) g_tail_rej++;
        if(t->r->state==RAFT_LEADER){
          g_tail_ares_leader++;
          if(q->m.append_entries_result.term==t->r->current_term){
            g_tail_ares_leader_intem++;
            if(to>=1&&to<=16) g_tail_ack_to[to]++;
          }
        }
      }
      if(q->m.type==RAFT_MSG_APPEND_RESULT&&!q->m.append_entries_result.success&&t->r->state==RAFT_LEADER){
        int pk;
        for(pk=0;pk<t->r->peer_count;pk++){
          if(t->r->peers[pk].id==q->m.from){ nx_before=t->r->peers[pk].next_index; break; }
        }
      }
      (void)raft_recvfrom_peer(t->r,&q->m);
      if(nx_before>=0) note_reject_backoff(t,q->m.from,nx_before);
    }
  }
  msg_free(q);
}
static void deliver_due(int step){
  int i=0;
  while(i<pending_count){
    if(pending[i]->deliver_at<=step){
      queued_msg *q=pending[i];
      deliver_one(q);
      pending[i]=pending[--pending_count];
    }else i++;
  }
}

/* ---- client traffic + compaction ---- */
static void fill_snap_tmp(node *n){
  unsigned int i;
  /* deterministic canonical image of the applied state at n->last_applied, so
     any two nodes snapshotting the same index produce identical bytes */
  for(i=0;i<n->snap_tmp_size;i++) n->snap_tmp[i]=snap_byte_for(n->last_applied,i);
}
static void client_op(node *n,int step){
  raft_client_message c;
  if(!n->alive||!n->r) return;
  g_cur_node_idx=(int)(n-nodes);
  memset(&c,0,sizeof(c));
  c.cookie=(const void*)(size_t)(step+1);
  switch(fuzz_rand_below(6)){
    case 0:
      {
        static raft_command cmds[3];
        static unsigned char cdata[3][32];
        int i,k=1+(int)fuzz_rand_below(3);
        for(i=0;i<k;i++){
          cmds[i].cookie=(const void*)(size_t)(0x50000000u+(unsigned)step*4u+(unsigned)(i+1));
          cmds[i].command=cdata[i];
          cmds[i].command_size=fuzz_rand_below(32);
        }
        c.type=RAFT_CLIENT_SUBMIT;
        c.submit.commands=cmds;
        c.submit.count=k;
      }
      break;
    case 1:
      c.type=RAFT_CLIENT_BARRIER;
      break;
    case 2:
      {
        static int ids[MAX_NODES];
        int pool[MAX_NODES];
        int i,cnt,tmp;
        for(i=0;i<n_nodes;i++) pool[i]=i+1;
        for(i=0;i<n_nodes;i++){
          int j=i+(int)fuzz_rand_below((unsigned int)(n_nodes-i));
          tmp=pool[i]; pool[i]=pool[j]; pool[j]=tmp;
        }
        cnt=1+(int)fuzz_rand_below((unsigned int)n_nodes);
        for(i=0;i<cnt;i++) ids[i]=pool[i];
        ev("RECONFIG node %d -> %d ids",n->id,cnt);
        c.type=RAFT_CLIENT_RECONFIG;
        c.reconfig.ids=ids;
        c.reconfig.id_count=cnt;
      }
      break;
    case 3:
      c.type=RAFT_CLIENT_ADD_LEARNER;
      c.learner.learner_id=1+(int)fuzz_rand_below((unsigned int)n_nodes);
      break;
    case 4:
      c.type=RAFT_CLIENT_REMOVE_LEARNER;
      c.learner.learner_id=1+(int)fuzz_rand_below((unsigned int)n_nodes);
      break;
    default:
      c.type=RAFT_CLIENT_TRANSFER;
      c.transfer.target_id=1+(int)fuzz_rand_below((unsigned int)n_nodes);
      break;
  }
  if(raft_recvfrom_client(n->r,&c)==0){
    if(c.type==RAFT_CLIENT_SUBMIT){
      /* raft.h resolves EACH submitted command with its OWN commands[i].cookie
         (one client_pending entry per command, each emitting its own result),
         so track every command's cookie -- not the outer submit cookie, which
         raft_submit ignores.  A single shared cookie would only track the
         first command and leave the rest of the batch silently unvalidated. */
      int i;
      for(i=0;i<c.submit.count;i++)
        out_add(c.submit.commands[i].cookie,(int)(n-nodes),RAFT_CLIENT_SUBMIT,0);
      ev("ACCEPT step %d node %d kind SUBMIT count %d",step,n->id,(int)c.submit.count);
    }else{
      out_add(c.cookie,(int)(n-nodes),(int)c.type,
              (c.type==RAFT_CLIENT_TRANSFER)?c.transfer.target_id:0);
      ev("ACCEPT step %d node %d kind %d cookie %p",step,n->id,(int)c.type,c.cookie);
    }
  }
}

/* ---- cluster lifecycle ---- */
static void init_nodes(void){
  int i;
  ev("CLUSTER %d nodes",n_nodes);
  for(i=0;i<n_nodes;i++){
    node *n=&nodes[i];
    n->id=g_ids[i];
    n->cfg_heartbeat=50;
    n->cfg_e_min=150u+fuzz_rand_below(200u);           /* 150..349 */
    n->cfg_e_max=n->cfg_e_min+50u+fuzz_rand_below(200u);
    n->cfg_seed=fuzz_rand();
    n->cfg_snap_chunk=256u+fuzz_rand_below(768u);
    n->cfg_log_chunk=(fuzz_rand_below(4)==0)?(4+(int)fuzz_rand_below(6)):0;
    if(!node_create(n)) fail("node_create failed"); /* OOM not armed during init */
  }
}
static void destroy_nodes(void){
  int i;
  for(i=0;i<n_nodes;i++){
    if(nodes[i].r) raft_destroy(nodes[i].r);
    nodes[i].r=0;
  }
  for(i=0;i<pending_count;i++) msg_free(pending[i]);
  pending_count=0;
}
static int live_node(void){
  /* pick a uniformly random LIVE node, or -1 if none are alive */
  int k=(int)fuzz_rand_below((unsigned int)n_nodes);
  int t=0;
  while(!nodes[k].alive&&t<n_nodes){ k=(k+1)%n_nodes; t++; }
  return nodes[k].alive?k:-1;
}
/* random non-trivial isolation subset: a bitmask with at least one bit set and
   at least one bit clear (never the empty set or the whole cluster). */
static int arm_partition(void){
  int pi,full=(1<<n_nodes)-1,mask=0;
  for(pi=0;pi<n_nodes;pi++) if(fuzz_rand_below(2)) mask|=(1<<pi);
  if(mask==0||mask==full) mask=1<<(int)fuzz_rand_below((unsigned int)n_nodes);
  return mask;
}
/* advance every live node for `steps` more steps; g_clean=1 makes delivery
   direct and timing small-step, so this is deterministic. */
static void tgt_drive(int *sp,int steps){
  int s,k;
  for(s=0;s<steps;s++){
    deliver_due(*sp);
    for(k=0;k<n_nodes;k++) if(nodes[k].alive) process_ready(&nodes[k],*sp);
    (*sp)++;
  }
}
/* submit `n` 16-byte writes to the leader.  Cookies are scenario-local (not
   out_add'ed, so P0-1 liveness does not track them; the scenario drives until
   they commit). */
static void tgt_submit_writes(node *L,int n){
  int i;
  for(i=0;i<n;i++){
    raft_client_message c;
    static raft_command cmd;
    static unsigned char data[16];
    memset(&data,0,sizeof(data));
    memset(&c,0,sizeof(c));
    cmd.cookie=(const void*)(size_t)(0x70000000u+(unsigned)i);
    cmd.command=data;
    cmd.command_size=sizeof(data);
    c.type=RAFT_CLIENT_SUBMIT;
    c.submit.commands=&cmd;
    c.submit.count=1;
    g_cur_node_idx=(int)(L-nodes);
    (void)raft_recvfrom_client(L->r,&c);
  }
}
/* drop in-flight messages + partitions so a scenario leaves no residue for the
   fault-free tail */
static void tgt_teardown(void){
  int k;
  for(k=0;k<pending_count;k++) msg_free(pending[k]);
  pending_count=0;
  part_mask=0; part_mask2=0;
}

/* Restore the membership to the full initial set {1..n_nodes} after the chaotic
   phase, so every targeted scenario starts from the SAME clean deterministic
   membership (tgt_elect_leader's documented intent) instead of whatever voter
   subset the chaos reconfig left behind.  A shrunken set (<3 voters) is exactly
   what makes joint/pre-vote/snap-drop silently skip -- the 34%-coverage defect.
   This restore is itself a lawful Sec. 4.4 membership change (re-add removed
   voters, which catch up via AppendEntries or a snapshot stream), so the chaos
   phase stays free to exercise arbitrary shrunken configs while the targeted
   layer gets a deterministic precondition. */
static void tgt_restore_full_config(int *step,int leader){
  node *L=&nodes[leader-1];
  raft_client_message c;
  static int ids[MAX_NODES];
  int i,need=0;
  if(!L->alive||!L->r) return;
  if(L->r->config_new.id_count!=n_nodes) need=1;
  else for(i=0;i<n_nodes;i++) if(L->r->config_new.ids[i]!=i+1){ need=1; break; }
  if(!need) return;
  for(i=0;i<n_nodes;i++) ids[i]=i+1;
  memset(&c,0,sizeof(c));
  c.cookie=(const void*)(size_t)0x75000000u;
  c.type=RAFT_CLIENT_RECONFIG;
  c.reconfig.ids=ids;
  c.reconfig.id_count=n_nodes;
  g_cur_node_idx=(int)(L-nodes);
  if(raft_recvfrom_client(L->r,&c)!=0) return;  /* rejected: stray pending reconfig */
  ev("TGT restore full config -> %d voters",n_nodes);
  tgt_drive(step,500);  /* let the joint commit + re-added voters catch up */
}

/* Heal every dead node, elect a stable leader, and quiesce.  Returns the 1-based
   leader id, or 0 if none was elected (scenario precondition unmet).  Each
   scenario calls this to start from a clean deterministic state, since a prior
   scenario (or the chaotic phase) leaves the cluster dirty (crashed nodes,
   in-flight messages, a half-applied reconfig). */
static int tgt_elect_leader(int *step){
  int k;
  g_clean=1;             /* direct delivery + small steps: deterministic */
  fuzz_oom_at=0;
  tgt_teardown();        /* drop chaos residue (delayed msgs, partitions) */
  for(k=0;k<n_nodes;k++) if(!nodes[k].alive) node_restore(&nodes[k]);
  g_leader=0;
  for(*step=0;*step<600&&g_leader<1;(*step)++){
    deliver_due(*step);
    for(k=0;k<n_nodes;k++) if(nodes[k].alive) process_ready(&nodes[k],*step);
  }
  if(g_leader<1) return 0;   /* could not elect: skip */
  tgt_drive(step,200);       /* quiesce: settle any in-flight reconfig + term */
  tgt_restore_full_config(step,g_leader);  /* full membership precondition */
  return (g_leader>=1)?g_leader:0;
}

/* ---- scenario TGT_SNAP_DROP: drop one mid-stream snapshot chunk ----
   Crash a follower, grow the leader's log past its prefix, snapshot, restore
   the follower, and drop one chunk of the InstallSnapshot stream that must now
   catch it up. */
static void tgt_snap_drop(int *step,int leader){
  node *L=&nodes[leader-1];
  int k,F=0;
  raft_i64 applied_before;
  for(k=0;k<n_nodes;k++) if(nodes[k].alive&&nodes[k].id!=leader){ F=nodes[k].id; break; }
  if(F==0){ tgt_teardown(); return; }
  applied_before=L->last_applied;
  node_crash(&nodes[F-1]);         /* F falls behind while down */
  tgt_submit_writes(L,32);         /* grow the leader's log past F's prefix */
  tgt_drive(step,200);             /* let the writes commit + apply */
  /* If crashing F stranded the leader without a quorum (a 3-node cluster with
     an unhealthy remaining follower), the writes never commit and F would not
     catch up via snapshot anyway: skip rather than fail. */
  if(L->last_applied<applied_before+16){ tgt_teardown(); return; }
  if(raft_snapshot(L->r)!=0){        /* precondition unmet (e.g. leftover config): skip */
    tgt_teardown();
    return;
  }
  L->snap_tmp_size=snap_size_for(L->last_applied);
  L->snap_tmp_index=L->last_applied;
  fill_snap_tmp(L);
  L->snap_tmp_pending=1;
  (void)raft_snapshot_data_ready(L->r,(raft_i64)L->snap_tmp_size);
  tgt_drive(step,60);                /* publish the local snapshot (compacts log) */
  node_restore(&nodes[F-1]);         /* F restarts far behind: needs InstallSnapshot */
  tgt_kind=TGT_SNAP_DROP; tgt_target=F; tgt_fired=0;
  tgt_drive(step,500);               /* drop fires; leader re-streams; F installs */
  if(tgt_fired){
    g_tgt_hits++;
    tgt_kind=TGT_NONE;
    tgt_teardown();
    return;
  }
  /* F was not caught up by InstallSnapshot this seed (e.g. the reconfig had not
     yet admitted it as a voter, or its prefix still overlaps the live log, so it
     took the AppendEntries path).  The scenario simply did not cover the drop
     path here -- this is NOT a safety assertion; SNAPSHOT BYTES / SM-safety still
     enforce snapshot correctness, and a genuinely stranded voter is caught by
     the tail's LIVENESS check. */
  ev("TGT skip: no InstallSnapshot stream to node %d",F);
  tgt_kind=TGT_NONE;
  tgt_teardown();
}

/* ---- scenario TGT_JOINT_CRASH: crash the leader mid joint-entry broadcast ----
   Raft membership change uses joint consensus (Sec. 4.3): one CONFIG entry carries
   C_old,new, which must commit through BOTH configs' majorities before the
   leader finalizes to C_new.  The dangerous window is a leader crash between
   appending the joint entry and committing it: some followers hold it, some
   don't.  Joint consensus is precisely what keeps that safe (single-step would
   not).  This scenario forces the window: shrink the voter set by one follower
   (an immediate joint, no catch-up), intercept the joint entry's broadcast, drop
   it to one follower, crash the leader, and let the survivors converge.  Safety
   of the recovery is asserted by the always-on oracles (config log-matching /
   SM safety / leader completeness); the scenario only makes the crash land in
   the joint window deterministically. */
static void tgt_joint_crash(int *step,int leader){
  node *L=&nodes[leader-1];
  static int ids[MAX_NODES];
  int ci,cnt,remove_id=0,vcount,k,s;
  raft_client_message c;
  if(!L->alive||!L->r){ tgt_teardown(); return; }
  /* Read the leader's live voter set to pick a follower to REMOVE (drives the
     reconfig target; not a safety assert -- recovery safety stays black-box).
     Removing one follower (new subset of old) never needs catch-up, so the joint entry
     is appended immediately rather than deferred (Sec. 4.1 / Sec. 4.2.1). */
  vcount=L->r->config_new.id_count;
  if(vcount<3) return;          /* shrinking below 2 voters strands the tail's transfer */
  for(ci=0;ci<vcount;ci++){
    int id=L->r->config_new.ids[ci];
    if(id!=leader){ remove_id=id; break; }
  }
  if(remove_id==0) return;
  cnt=0;
  for(ci=0;ci<vcount;ci++){
    int id=L->r->config_new.ids[ci];
    if(id!=remove_id) ids[cnt++]=id;
  }
  memset(&c,0,sizeof(c));
  c.cookie=(const void*)(size_t)0x71000000u;   /* scenario-local cookie (untracked) */
  c.type=RAFT_CLIENT_RECONFIG;
  c.reconfig.ids=ids;
  c.reconfig.id_count=cnt;
  g_cur_node_idx=(int)(L-nodes);
  if(raft_recvfrom_client(L->r,&c)!=0){ tgt_teardown(); return; } /* rejected: stray state */
  /* arm the interception and drive until the joint entry is (or is not) broadcast */
  tgt_kind=TGT_JOINT_CRASH; tgt_target=leader; tgt_fired=0; tgt_crash_id=0;
  for(s=0;s<400&&!tgt_fired;s++){
    deliver_due(*step);
    for(k=0;k<n_nodes;k++) if(nodes[k].alive) process_ready(&nodes[k],*step);
    (*step)++;
  }
  if(tgt_fired&&tgt_crash_id>0){
    g_tgt_hits++;
    /* Crash the joint-broadcasting leader NOW (process_ready has returned): the
       joint entry is in-flight to most followers, dropped to one, uncommitted.
       The survivors then re-elect and converge -- the oracles check safety. */
    tgt_kind=TGT_NONE;
    node_crash(&nodes[tgt_crash_id-1]);
    tgt_drive(step,600);           /* survivors re-elect + converge (oracles check) */
  }else{
    ev("TGT skip: joint entry never broadcast (no-op / deferred / rejected reconfig)");
  }
  tgt_kind=TGT_NONE;
  tgt_teardown();
}

/* ---- scenario TGT_READ_INDEX: a partitioned leader must not serve a read ----
   A linearizable read (Sec. 6.4) needs a FRESH heartbeat quorum: the leader records
   commit_index, confirms it is still leader in a NEW heartbeat round, then
   serves.  An ISOLATED leader cannot obtain that quorum and must hold the read
   (or step down).  This scenario partitions the leader, submits a barrier, and
   drives through the window -- any barrier READY in the window is a stale-read
   violation, caught by the TGT_READ_INDEX check in process_ready.  This is a
   NEGATIVE-SPACE assertion ("not READY while partitioned") that the positive-
   space oracles (SM safety / leader completeness / P0-2) cannot express. */
static void tgt_read_index(int *step,int leader){
  node *L=&nodes[leader-1];
  raft_client_message c;
  if(!L->alive||!L->r){ tgt_teardown(); return; }
  /* The negative assertion only holds when the leader is NOT alone: with a
     single-voter config the leader IS the whole quorum (quorum_size = n/2+1 =
     1), so a "partitioned" single-node leader legitimately serves READY
     immediately -- there is no follower to confirm against (Sec. 4.4).  The
     chaotic phase's shrink-reconfig can leave a 1-voter config, so skip those
     seeds rather than false-positive. */
  if(L->r->config_new.id_count<2) return;
  part_mask=1<<(leader-1);              /* isolate the leader from every peer */
  memset(&c,0,sizeof(c));
  c.cookie=TGT_READ_COOKIE;
  c.type=RAFT_CLIENT_BARRIER;
  g_cur_node_idx=(int)(L-nodes);
  if(raft_recvfrom_client(L->r,&c)!=0){ /* rejected: not leader / transfer / removed */
    part_mask=0; tgt_teardown(); return;
  }
  tgt_kind=TGT_READ_INDEX; /* scope gate for the READY check; tgt_fired unused here */
  tgt_drive(step,300);                  /* cover the partition + step-down window */
  g_tgt_hits++;                         /* partition established + barrier submitted: covered */
  tgt_kind=TGT_NONE;
  part_mask=0;
  tgt_teardown();
}

/* ---- scenario TGT_PRE_VOTE: a removed node's pre-vote must be suppressed ----
   A node removed from the voter set (Sec. 4.2.2 / Figure 4.6) keeps campaigning
   until C_new commits -- after that it elections-timeout and PRE-VOTES, but the
   surviving voters sit inside the heartbeat grace period (leader_contact_elapsed
   < election_min) and silently refuse (Sec. 4.2.3), so the removed node can never
   gather a pre-vote majority and never escalates to a real vote.  A real vote
   (pre_vote==0) from the removed node would mean that suppression failed -- the
   removal would let a stale node bump terms/disturb the leader.  This scenario
   removes one follower and watches for exactly that failure via the
   TGT_PRE_VOTE check in process_ready. */
static void tgt_pre_vote(int *step,int leader){
  node *L=&nodes[leader-1];
  static int ids[MAX_NODES];
  int ci,cnt,remove_id=0,vcount;
  raft_client_message c;
  if(!L->alive||!L->r){ tgt_teardown(); return; }
  vcount=L->r->config_new.id_count;
  if(vcount<3) return;               /* need >=2 voters left, plus the removed node */
  for(ci=0;ci<vcount;ci++){
    int id=L->r->config_new.ids[ci];
    if(id!=leader){ remove_id=id; break; }
  }
  if(remove_id==0) return;
  cnt=0;
  for(ci=0;ci<vcount;ci++){
    int id=L->r->config_new.ids[ci];
    if(id!=remove_id) ids[cnt++]=id;
  }
  memset(&c,0,sizeof(c));
  c.cookie=(const void*)(size_t)0x73000000u;   /* scenario-local cookie (untracked) */
  c.type=RAFT_CLIENT_RECONFIG;
  c.reconfig.ids=ids;
  c.reconfig.id_count=cnt;
  g_cur_node_idx=(int)(L-nodes);
  if(raft_recvfrom_client(L->r,&c)!=0){ tgt_teardown(); return; } /* rejected: stray state */
  tgt_drive(step,200);               /* let the shrink commit + finalize + F apply it */
  /* arm: F (now removed) will election-timeout and pre-vote; a real vote fails */
  tgt_kind=TGT_PRE_VOTE; tgt_target=remove_id; tgt_fired=0;
  tgt_drive(step,500);               /* cover several of F's election deadlines */
  tgt_kind=TGT_NONE;
  if(!tgt_fired) ev("TGT skip: removed node %d never pre-voted this seed",remove_id);
  else g_tgt_hits++;
  tgt_teardown();
}

/* ---- scenario TGT_TRANSFER_CRASH: crash the leader right after handoff ----
   Sec. 3.10 leadership transfer: the old leader stops serving, replicates its log
   to the target, then emits TimeoutNow and yields.  The target (already caught
   up) campaigns immediately.  This scenario crashes the old leader the moment
   it emits TimeoutNow, so the handoff completes purely from the target's side
   (the in-flight TimeoutNow still arrives) -- the old leader dying after the
   yield must not strand leadership.  Safety (no two leaders, log completeness)
   is checked by the always-on oracles. */
static void tgt_transfer_crash(int *step,int leader){
  node *L=&nodes[leader-1];
  int target=0,ti,s,k;
  raft_client_message c;
  if(!L->alive||!L->r){ tgt_teardown(); return; }
  for(ti=0;ti<n_nodes;ti++){
    int id=ti+1;
    if(!nodes[ti].alive) continue;
    if(id==leader) continue;
    if(raft_mask_has(raft_set_view(&L->r->config_new),id)){ target=id; break; }
  }
  if(target==0) return;
  memset(&c,0,sizeof(c));
  c.cookie=(const void*)(size_t)0x74000000u;   /* scenario-local cookie (untracked) */
  c.type=RAFT_CLIENT_TRANSFER;
  c.transfer.target_id=target;
  g_cur_node_idx=(int)(L-nodes);
  if(raft_recvfrom_client(L->r,&c)!=0){ tgt_teardown(); return; } /* rejected: stray state */
  tgt_kind=TGT_TRANSFER_CRASH; tgt_target=target; tgt_fired=0; tgt_crash_id=0;
  for(s=0;s<400&&!tgt_fired;s++){
    deliver_due(*step);
    for(k=0;k<n_nodes;k++) if(nodes[k].alive) process_ready(&nodes[k],*step);
    (*step)++;
  }
  if(tgt_fired&&tgt_crash_id>0){
    g_tgt_hits++;
    tgt_kind=TGT_NONE;
    node_crash(&nodes[tgt_crash_id-1]);       /* old leader dies after yielding */
    tgt_drive(step,600);                      /* target campaigns + takes over */
    node_restore(&nodes[tgt_crash_id-1]);     /* heal the crashed node (no residue for tail) */
    tgt_drive(step,300);                      /* let it catch up + settle */
  }else{
    ev("TGT skip: transfer never reached TimeoutNow this seed");
  }
  tgt_kind=TGT_NONE;
  tgt_teardown();
}

/* Run deterministic targeted-fault scenarios after the chaotic phase and before
   the fault-free liveness tail.  Each scenario drives the cluster to a specific
   state via the PUBLIC API, then injects ONE fault at the critical Ready event
   (see the TGT_* interception in process_ready).  The existing black-box oracles
   (SNAPSHOT BYTES / SM safety / liveness) verify recovery; a scenario whose
   precondition is unmet is SKIPPED, not failed -- probabilistic coverage layered
   on top of the always-on safety oracle. */
static void run_targeted_scenarios(void){
  int step=0,leader;
  leader=tgt_elect_leader(&step);
  if(leader>=1) tgt_snap_drop(&step,leader); else g_elect_fail++;
  leader=tgt_elect_leader(&step);
  if(leader>=1) tgt_joint_crash(&step,leader); else g_elect_fail++;
  leader=tgt_elect_leader(&step);
  if(leader>=1) tgt_read_index(&step,leader); else g_elect_fail++;
  leader=tgt_elect_leader(&step);
  if(leader>=1) tgt_pre_vote(&step,leader); else g_elect_fail++;
  leader=tgt_elect_leader(&step);
  if(leader>=1) tgt_transfer_crash(&step,leader); else g_elect_fail++;
}

static void run_cluster(unsigned long seed){
  int step,k,i;
  fuzz_state=(fuzz_u64)seed;
  ev_count=0;
  ev("SEED %lu",(unsigned long)seed);
  memset(nodes,0,sizeof(nodes));
  memset(g_term,0,sizeof(g_term));
  memset(g_term_set,0,sizeof(g_term_set));
  memset(g_hash,0,sizeof(g_hash));
  memset(g_state,0,sizeof(g_state));
  memset(g_cterm,0,sizeof(g_cterm));
  memset(g_cterm_set,0,sizeof(g_cterm_set));
  g_cterm_max=0;
  g_commit_max=0;
  g_out_count=0;
  g_oom_count=0;
  memset(g_oom_node,0,sizeof(g_oom_node));
  g_cur_node_idx=-1;
  /* Also reset the OOM arming base: fuzz_oom_at is an ABSOLUTE library-allocation index, so
     without this the same seed fires its injected allocation failure at a different point
     depending on how many seeds ran in this invocation - which is why one seed's outcome
     changed between "3 1 0" (pass) and "1 2000 0" (fail). */
  fuzz_alloc_calls=0;
  fuzz_oom_at=0;
  memset(g_cfg_hash,0,sizeof(g_cfg_hash));
  memset(g_cfg_set,0,sizeof(g_cfg_set));
  memset(g_snap_cfg_hash,0,sizeof(g_snap_cfg_hash));
  memset(g_snap_cfg_set,0,sizeof(g_snap_cfg_set));
  g_xfer_resolved=0;
  g_xfer_status=0;
  g_xfer_target=0;
  g_state[0]=FUZZ_U64_C(14695981039346656037); /* base of the canonical applied-state chain */
  g_leader_count=0;
  g_leader=0; g_marker=0; g_clean=0;
  part_mask=0; part_until=0; part_mask2=0; part_until2=0;
  /* targeted-scenario state: reset unconditionally per seed.  Each scenario
     already ends with tgt_kind=TGT_NONE, but pinning the invariant here makes a
     future scenario's missed reset harmless instead of a cross-seed leak. */
  tgt_kind=TGT_NONE; tgt_target=0; tgt_fired=0; tgt_crash_id=0;
  g_tgt_hits=0; g_elect_fail=0; g_queue_full=0;
  n_nodes=(fuzz_rand_below(2)==0)?3:5;
  init_nodes();

  /* ---- random-fault phase: partitions + drops + crashes/restarts ---- */
  for(step=0;step<MAX_STEPS;step++){
    int extra;
    deliver_due(step);
    k=live_node();
    if(k>=0) process_ready(&nodes[k],step);

    maybe_faults();
    maybe_oom();

    extra=(int)fuzz_rand_below(10);
    if(extra==0){
      k=live_node();
      if(k>=0) client_op(&nodes[k],step);
    }else if(extra==1){
      k=live_node();
      if(k>=0){
        node *n=&nodes[k];
        g_cur_node_idx=k;
        if(raft_snapshot(n->r)==0){
          n->snap_tmp_size=snap_size_for(n->last_applied);
          n->snap_tmp_index=n->last_applied;
          fill_snap_tmp(n);
          n->snap_tmp_pending=1;
          (void)raft_snapshot_data_ready(n->r,(raft_i64)n->snap_tmp_size);
        }
      }
    }else if(extra==2){
      k=live_node();
      if(k>=0) process_ready(&nodes[k],step);
    }

    /* occasionally isolate a subset for a window (network partition).  TWO
       independent partitions may overlap, splitting the cluster into up to
       four groups (in-both / in-A / in-B / in-neither) - a multi-way split the
       single-partition model never produced. */
    if(part_mask==0&&fuzz_rand_below(300)==0){
      part_mask=arm_partition();
      part_until=step+20+(int)fuzz_rand_below(40);
      ev("PARTITION mask=%d",part_mask);
    }else if(part_mask&&step>=part_until){
      part_mask=0;
    }
    if(part_mask2==0&&fuzz_rand_below(500)==0){
      part_mask2=arm_partition();
      part_until2=step+20+(int)fuzz_rand_below(40);
      ev("PARTITION2 mask=%d",part_mask2);
    }else if(part_mask2&&step>=part_until2){
      part_mask2=0;
    }
  }

  /* ---- targeted fault-injection scenarios (deep extension x fault coverage) ---- */
  run_targeted_scenarios();

  /* ---- clean quiescence tail: heal, restart all, check LIVENESS ---- */
  part_mask=0; part_mask2=0;
  g_clean=1;
  fuzz_oom_at=0; /* no OOM injection in the fault-free liveness tail */
  for(k=0;k<n_nodes;k++) if(!nodes[k].alive) node_restore(&nodes[k]);
  g_leader=0;
  g_tail_leader_changes=0;
  g_tail_prev_leader=0;
  g_tail_term_first=0;
  g_tail_term_last=0;
  /* stabilize fault-free: a leader must be elected and stay */
  for(step=0;step<800;step++){
    g_step_elapsed=advance_elapsed();
    deliver_due(MAX_STEPS+step);
    for(k=0;k<n_nodes;k++) if(nodes[k].alive) process_ready(&nodes[k],MAX_STEPS+step);
    if(g_leader>0&&nodes[g_leader-1].r){
      if(g_leader!=g_tail_prev_leader){ g_tail_leader_changes++; g_tail_prev_leader=g_leader; }
      if(!g_tail_term_first) g_tail_term_first=nodes[g_leader-1].r->current_term;
      g_tail_term_last=nodes[g_leader-1].r->current_term;
    }
  }
  if(g_leader<1||g_leader>n_nodes||!nodes[g_leader-1].alive)
    fail("LIVENESS: no stable leader elected after heal (leader changes=%d, term %" RAFT_I64_FMT " -> %" RAFT_I64_FMT ")",
         g_tail_leader_changes,g_tail_term_first,g_tail_term_last);
  /* submit a marker command; keep ticking until it COMMITS (or timeout) */
  g_marker=0;
  for(step=0;step<1000&&!g_marker;step++){
    raft_client_message c;
    static raft_command cmd;
    static unsigned char mb[8]={0x5A,0xA5,0x5A,0xA5,0,0,0,0};
    g_step_elapsed=advance_elapsed();
    deliver_due(MAX_STEPS+800+step);
    for(k=0;k<n_nodes;k++) if(nodes[k].alive) process_ready(&nodes[k],MAX_STEPS+800+step);
    if(g_leader>0&&nodes[g_leader-1].r){
      if(g_leader!=g_tail_prev_leader){ g_tail_leader_changes++; g_tail_prev_leader=g_leader; }
      if(!g_tail_term_first) g_tail_term_first=nodes[g_leader-1].r->current_term;
      g_tail_term_last=nodes[g_leader-1].r->current_term;
    }
    if(!g_marker&&g_leader>0&&nodes[g_leader-1].alive){
      memset(&c,0,sizeof(c));
      cmd.cookie=MARKER_COOKIE;
      cmd.command=mb;
      cmd.command_size=8;
      c.type=RAFT_CLIENT_SUBMIT;
      c.submit.commands=&cmd;
      c.submit.count=1;
      g_cur_node_idx=g_leader-1;
      (void)raft_recvfrom_client(nodes[g_leader-1].r,&c);
    }
  }
  if(!g_marker) fail("LIVENESS: marker command did not commit in tail (leader changes in tail=%d, term %" RAFT_I64_FMT " -> %" RAFT_I64_FMT ", commit=%" RAFT_I64_FMT ")",
                     g_tail_leader_changes,g_tail_term_first,g_tail_term_last,
                     g_leader>0&&nodes[g_leader-1].r?nodes[g_leader-1].r->commit_index:RAFT_I64_C(-1));
  /* P0-3 ($3.10): DETERMINISTIC leadership-transfer convergence test in the
     STABLE tail.  First run a QUIESCENCE WINDOW so the stable leader
     heartbeats every follower and resets all election timers - otherwise a
     follower whose timer was about to fire (left over from the pre-marker
     election) campaigns concurrently with the transfer target and splits the
     vote, so the target legitimately loses (seeds 12625 / 1344).  The window
     must also be long enough to resettle a scenario-crashed node that the
     async-fsync model (persist_delay>0) leaves at a stale term through the
     chaos phase - it re-campaigns and splits the vote otherwise (seed 2213).
     Then submit a transfer from the stable leader to a live VOTER and tick
     until it resolves. */
  for(step=0;step<400;step++){
    g_step_elapsed=advance_elapsed();
    deliver_due(MAX_STEPS+1800+step);
    for(k=0;k<n_nodes;k++) if(nodes[k].alive) process_ready(&nodes[k],MAX_STEPS+1800+step);
  }
  {
    int target=0,ti,tries;
    for(ti=0;ti<n_nodes;ti++){
      int id=ti+1;
      if(!nodes[ti].alive) continue;
      if(id==g_leader) continue;
      if(nodes[g_leader-1].r
         &&raft_mask_has(raft_set_view(&nodes[g_leader-1].r->config_new),id)){
        target=id; break;
      }
    }
    if(target>0){
      raft_client_message c;
      g_xfer_target=target;
      g_xfer_resolved=0;
      g_xfer_status=0;
      memset(&c,0,sizeof(c));
      c.cookie=XFER_COOKIE;
      c.type=RAFT_CLIENT_TRANSFER;
      c.transfer.target_id=target;
      g_cur_node_idx=g_leader-1;
      if(raft_recvfrom_client(nodes[g_leader-1].r,&c)==0){
        out_add(c.cookie,(int)(g_leader-1),(int)c.type,target);
        for(tries=0;tries<1000&&!g_xfer_resolved;tries++){
          g_step_elapsed=advance_elapsed();
          deliver_due(MAX_STEPS+2000+tries);
          for(k=0;k<n_nodes;k++) if(nodes[k].alive) process_ready(&nodes[k],MAX_STEPS+2000+tries);
        }
        if(!g_xfer_resolved)
          fail("TRANSFER LIVENESS: transfer to node %d did not resolve in tail",target);
        if(g_xfer_status==RAFT_CLIENT_COMMITTED){
          /* Sec. 3.10 CONVERGENCE, aligned to the paper: the TimeoutNow was emitted
             and the target campaigned, but the paper promises only that the
             target is "highly likely" to win - a SPLIT VOTE with another fresh
             candidate is lawful in the async model (dissertation Sec. 3.10 "highly
             likely", and lines 4327-4329: no deterministic termination).  So assert
             LIVENESS - the cluster settles on SOME stable leader after the
             handoff - rather than that the target specifically wins. */
          int x=0,run=0,prev=0,settled=0,target_led=0;
          for(x=0;x<1500;x++){
            g_step_elapsed=advance_elapsed();
            deliver_due(MAX_STEPS+3000+x);
            for(k=0;k<n_nodes;k++) if(nodes[k].alive) process_ready(&nodes[k],MAX_STEPS+3000+x);
            if(g_leader==target) target_led=1;
            if(g_leader>0&&g_leader==prev) run++; else run=0;
            prev=g_leader;
            if(run>=100){ settled=1; break; }   /* same leader for a contiguous run = settled */
          }
          if(!settled)
            fail("TRANSFER CONVERGENCE: after a committed transfer the cluster did not settle on a stable leader in the fault-free tail (target %d won: %s)",target,target_led?"yes":"no (split vote)");
        }
      }
    }
  }
  /* P0-1: every accepted request must resolve with a terminal result.
     OOM EXEMPTION (per-node, P2-1): a cookie is exempt ONLY when at least one
     OOM fired ON ITS ACCEPTING NODE after submission - the cookie's volatile
     storage (client_pending / read barrier / follower ReadIndex / config
     catch-up / result buffer) lives on that node alone, so an OOM elsewhere
     cannot drop it.  This is strictly tighter than the old global counter,
     which exempted a cookie whenever ANY node OOM'd later and masked most
     early-accepted cookies.  Remaining asymmetry (unchanged): an OOM BEFORE
     submission that corrupts library state so a LATER request hangs is NOT
     exempted - that is a real bug (Round 59 seed 1830), and we WANT it. */
  for(i=0;i<g_out_count;i++){
    if(g_out[i].submit_oom_node==g_oom_node[g_out[i].node_idx])
      fail("CLIENT RESULT LIVENESS: accepted request never resolved (cookie %p node %d kind %d)",
           g_out[i].cookie,g_out[i].node_idx+1,g_out[i].kind);
  }
  /* P1-2 DRAINING (end-to-end): queue a barrier on the stable leader, stop it
     immediately (no advance in between, so the barrier is still pending), then
     tick the stopped node and require a terminal result during drain.  The unit
     tests cover per-request-type flushes; this covers raft_stop on a LIVE
     leader inside a real cluster (a path the cluster fuzzer otherwise never
     enters). */
  {
    node *L=&nodes[g_leader-1];
    if(L->alive&&L->r&&L->r->state==RAFT_LEADER){
      raft_client_message c;
      memset(&c,0,sizeof(c));
      c.type=RAFT_CLIENT_BARRIER;
      c.cookie=DRAIN_COOKIE;
      g_cur_node_idx=(int)(L-nodes);
      if(raft_recvfrom_client(L->r,&c)==0){
        int drained=0,di;
        raft_stop(L->r);
        for(di=0;di<200;di++){
          raft_ready r2;
          int ri;
          (void)raft_advance(L->r,10u,&r2);
          for(ri=0;ri<r2.client_result_count;ri++)
            if(r2.client_results[ri].cookie==DRAIN_COOKIE) drained=1;
          raft_ready_consumed(L->r);
          if(drained) break;
        }
        if(!drained)
          fail("DRAINING: pending barrier not flushed by raft_stop on leader %d",L->id);
      }
    }
  }
  /* Targeted-scenario coverage + harness-limit visibility: report when a seed
     under-covered (a skipped scenario or a dropped full queue), so probabilistic
     coverage never silently degrades.  5/5 + 0 drops = no output. */
  if(g_tgt_hits<5||g_queue_full>0)
    fprintf(stderr,"  tgt-hits %d/5  elect-fail %d  queue-full %d\n",
            g_tgt_hits,g_elect_fail,g_queue_full);
  destroy_nodes();
}

int main(int argc,char **argv){
  unsigned long seed=1;
  unsigned long count=10000;
  unsigned long i;
  if(argc>1) seed=(unsigned long)strtoul(argv[1],0,10);
  if(argc>2) count=(unsigned long)strtoul(argv[2],0,10);
  if(argc>3) g_persist_delay=(unsigned int)strtoul(argv[3],0,10);
  signal(SIGSEGV,crash_dump);
  signal(SIGILL,crash_dump);
  signal(SIGABRT,crash_dump);
  for(i=0;i<count;i++){
    fprintf(stderr,"seed %lu\n",seed+i);
    fflush(stderr);
    run_cluster(seed+i);
  }
  fprintf(stderr,"done: %lu iterations\n",count);
  return 0;
}
