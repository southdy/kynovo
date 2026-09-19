/* ============================================================================
   raft_fuzz.c -- standalone deterministic fuzz driver for raft.h

   No third-party dependencies.  Generates random-but-reproducible action
   sequences (peer messages, client requests, advances, persist/apply/snapshot
   notifications) for a single node, plus single-point OOM injection, to find
   crashes, undefined behaviour and OOM-rollback defects in the Raft library.

   Build (Windows/MinGW, C89):
     gcc -std=c89 -O2 -Wall -Wextra tests/raft_fuzz.c -o raft_fuzz
   Build (Linux/clang, recommended, with sanitizers):
     clang -std=c89 -O1 -g -fsanitize=address,undefined tests/raft_fuzz.c -o raft_fuzz

   Usage:
     raft_fuzz [start_seed [count]]
       start_seed : first seed (default 1)
       count      : number of iterations (default 100000)

   Each iteration prints its seed to stderr (flushed) BEFORE running, so the
   last printed seed reproduces a crash:
     raft_fuzz <seed> 1

   The OOM injector fails the Nth allocation CALL (N drawn from the PRNG for
   ~1/3 of iterations), exercising the library's rollback paths that are not
   test-injectable in the deterministic suite.
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

/* ---- action trace ring buffer + crash dump (crash self-localization) ----
   On SIGSEGV/SIGILL/SIGABRT the handler dumps the last actions to stderr and
   re-raises, so a crash is self-localizing: the last "seed N" line gives the
   repro, and the action trace shows what the node was doing right before. */
#define ACT_MAX 64
static char act_buf[ACT_MAX][128];
static int act_count;
static void act_log(const char *fmt,...){
  char buf[128];
  va_list ap;
  va_start(ap,fmt);
  vsnprintf(buf,sizeof(buf),fmt,ap);
  va_end(ap);
  buf[sizeof(buf)-1]=0;
  strcpy(act_buf[act_count%ACT_MAX],buf);
  act_count++;
}
static void act_dump(void){
  int i,start=act_count>ACT_MAX?act_count-ACT_MAX:0;
  fprintf(stderr,"--- last %d actions ---\n",act_count-start);
  for(i=start;i<act_count;i++) fprintf(stderr,"%s\n",act_buf[i%ACT_MAX]);
}
static void crash_dump(int sig){
  fprintf(stderr,"\n=== CRASH signal %d ===\n",sig);
  act_dump();
  fflush(stderr);
  signal(sig,SIG_DFL);
  raise(sig);
}


/* ---- OOM-injectable allocators (MUST be defined BEFORE the implementation).
   fuzz_oom_at: 0 = disabled; otherwise the Nth allocation call returns NULL
   (single-point injection).  The library's rollback paths free already-made
   allocations, so returning NULL is sufficient. */
static unsigned int fuzz_oom_at = 0;
static unsigned int fuzz_alloc_calls = 0;

static void *fuzz_malloc(size_t n){
  fuzz_alloc_calls++;
  if(fuzz_oom_at!=0 && fuzz_alloc_calls==fuzz_oom_at) return 0;
  return malloc(n);
}
static void *fuzz_calloc(size_t nm,size_t sz){
  fuzz_alloc_calls++;
  if(fuzz_oom_at!=0 && fuzz_alloc_calls==fuzz_oom_at) return 0;
  return calloc(nm,sz);
}
static void *fuzz_realloc(void *p,size_t n){
  fuzz_alloc_calls++;
  if(fuzz_oom_at!=0 && fuzz_alloc_calls==fuzz_oom_at) return 0;
  return realloc(p,n);
}

#define RAFT_STATIC
#define RAFT_MALLOC fuzz_malloc
#define RAFT_CALLOC fuzz_calloc
#define RAFT_REALLOC fuzz_realloc
#define RAFT_IMPLEMENTATION
#include "../code/raft.h"

/* ---- deterministic PRNG: splitmix64 (self-contained, no stdint.h) ---- */
static unsigned long long fuzz_state;

static unsigned long long fuzz_next_u64(void){
  unsigned long long z;
  fuzz_state += 0x9E3779B97F4A7C15ULL;
  z = fuzz_state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  z = z ^ (z >> 31);
  return z;
}
static unsigned int fuzz_rand(void){
  return (unsigned int)(fuzz_next_u64() & 0xFFFFFFFFu);
}
static unsigned int fuzz_rand_mod(unsigned int n){
  if(n<=1) return 0;
  return fuzz_rand() % n;
}

/* ---- single-node invariant oracle (content-level state-machine invariants).
   raft_fuzz otherwise only detects a crash; these four bounds catch "not a
   crash but wrong": OOM-rollback state corruption, apply replay/reorder, term
   or commit regression.  Every check is a SINGLE-node invariant that holds
   under ANY (even adversarial) message sequence, snapshot install, raft_stop,
   and OOM injection, so a hit is always a real defect (zero false-positive
   surface).  They deliberately do NOT require apply continuity (+1): a snapshot
   install legitimately jumps the index -- only strict increase is demanded. */
static raft_i64 g_last_term   = RAFT_I64_C(-1);
static raft_i64 g_last_commit = RAFT_I64_C(-1);
static raft_i64 g_last_apply  = RAFT_I64_C(-1);

static void invariant_fail(const char *what,raft_i64 got,raft_i64 bound){
  fprintf(stderr,"\nINVARIANT VIOLATION: %s (got %lld, bound %lld)\n",
          what,(long long)got,(long long)bound);
  act_dump();
  fflush(stderr);
  exit(1);
}

static void check_invariants(const raft_ready *rd){
  int i;
  /* Once stopped, raft.h resets its live output (commit/apply views return to
     0) as part of teardown; monotonicity binds a RUNNING node only.  Reset the
     tracked bounds and skip until the next raft_stop-free run. */
  if(rd->phase_stopped){
    g_last_term   = RAFT_I64_C(-1);
    g_last_commit = RAFT_I64_C(-1);
    g_last_apply  = RAFT_I64_C(-1);
    return;
  }
  /* term: sampled ONLY when a persist is needed, because ready.persist.term is
     the durable-term view and may LAG the live current_term between persists
     (raft_cluster_fuzz notes the same: cur_term refreshes only from the persist
     view).  A term bump always sets persist_needed, so sampling here loses no
     coverage while eliminating the false "term regressed" on a stale view. */
  if(rd->persist_needed){
    if(g_last_term>=0 && rd->persist.term<g_last_term)
      invariant_fail("term regressed",rd->persist.term,g_last_term);
    g_last_term=rd->persist.term;
  }
  /* commit index never decreases (ready.commit_index is a live scalar) */
  if(g_last_commit>=0 && rd->commit_index<g_last_commit)
    invariant_fail("commit_index regressed",rd->commit_index,g_last_commit);
  g_last_commit=rd->commit_index;
  /* apply entries: strictly increasing index, never beyond commit */
  for(i=0;i<rd->apply_count;i++){
    raft_i64 idx=rd->apply_entries[i].index;
    if(idx<=g_last_apply)
      invariant_fail("apply index replayed/reordered",idx,g_last_apply);
    if(idx>rd->commit_index)
      invariant_fail("apply index beyond commit",idx,rd->commit_index);
    g_last_apply=idx;
  }
}

/* ---- edge-biased value generators ---- */
static raft_i64 fuzz_term(void){
  switch(fuzz_rand_mod(9)){
    case 0: return RAFT_I64_C(0);
    case 1: return RAFT_I64_C(1);
    case 2: return RAFT_I64_C(-1);
    case 3: return RAFT_TERM_MAX;
    case 4: return RAFT_TERM_MAX + RAFT_I64_C(1);   /* INT64_MAX */
    case 5: return -RAFT_TERM_MAX;
    case 6: return -RAFT_TERM_MAX - RAFT_I64_C(2);  /* INT64_MIN */
    default: return (raft_i64)fuzz_rand_mod(64) - RAFT_I64_C(2);
  }
}
static raft_i64 fuzz_index(void){
  switch(fuzz_rand_mod(7)){
    case 0: return RAFT_I64_C(0);
    case 1: return RAFT_I64_C(1);
    case 2: return RAFT_I64_C(-1);
    case 3: return RAFT_I64_C(1000000);
    case 4: return RAFT_I64_C(-1000000);
    default: return (raft_i64)fuzz_rand_mod(16) - RAFT_I64_C(1);
  }
}
static int fuzz_id(void){
  switch(fuzz_rand_mod(8)){
    case 0: return 0;          /* invalid */
    case 1: return 1;
    case 2: return 2;
    case 3: return 3;
    case 4: return 4;
    case 5: return -1;
    case 6: return 2147483647;
    default: return (int)fuzz_rand_mod(8) - 2;
  }
}
/* valid node id (id>0) for CONFIG/RESTORE fields, where raft.h requires id>0
   (raft_id_valid) and rejects duplicates.  fuzz_id() above stays adversarial for
   MESSAGE fields, where id<=0 exercises the API's invalid-id handling. */
static int fuzz_id_valid(void){
  return (int)fuzz_rand_mod(16) + 1;   /* 1..16 */
}

/* ---- static arena for pointer-bearing fields (the library COPIES the input
   data on receipt, so the arena can be reused across actions) ---- */
static unsigned char fuzz_arena[512];
static raft_i64      fuzz_terms[8];
static unsigned char fuzz_kinds[8];
static unsigned int  fuzz_sizes[8];
static int           fuzz_ids[16];
static raft_mask     fuzz_masks[8];
static raft_command  fuzz_cmds[4];

static void fuzz_fill_arena(void){
  int i;
  for(i=0;i<(int)sizeof(fuzz_arena);i++) fuzz_arena[i]=(unsigned char)fuzz_rand();
  for(i=0;i<8;i++){
    fuzz_terms[i]=fuzz_term();
    fuzz_kinds[i]=(unsigned char)fuzz_rand_mod(3);     /* entry kind 0..2 */
    fuzz_sizes[i]=fuzz_rand_mod(32);
  }
  for(i=0;i<16;i++) fuzz_ids[i]=fuzz_id();
  for(i=0;i<8;i++){
    fuzz_masks[i].ids = fuzz_ids;
    fuzz_masks[i].id_count = (int)fuzz_rand_mod(9);    /* 0..8 ids */
  }
}

/* a config mask with random (possibly unsorted/duplicate/invalid) ids */
static raft_mask fuzz_mask(void){
  raft_mask m;
  int n=(int)fuzz_rand_mod(9);
  m.ids = (n>0) ? fuzz_ids : 0;
  m.id_count = n;
  return m;
}

/* ---- action: send one random peer message ---- */
static void fuzz_send_peer(raft_ctx *r){
  raft_peer_message m;
  memset(&m,0,sizeof(m));
  m.type = (int)fuzz_rand_mod(9) + 1;  /* RAFT_MSG_REQUEST_VOTE .. TIMEOUT_NOW */
  m.from = fuzz_id();
  m.to   = fuzz_id();
  m.term = fuzz_term();
  switch(m.type){
    case RAFT_MSG_REQUEST_VOTE:
      m.request_vote.term = fuzz_term();
      m.request_vote.last_log_index = fuzz_index();
      m.request_vote.last_log_term  = fuzz_term();
      m.request_vote.candidate_id   = fuzz_id();
      m.request_vote.pre_vote       = (int)fuzz_rand_mod(2);
      break;
    case RAFT_MSG_REQUEST_VOTE_RESULT:
      m.request_vote_result.term = fuzz_term();
      m.request_vote_result.vote_granted = (int)fuzz_rand_mod(2);
      m.request_vote_result.pre_vote     = (int)fuzz_rand_mod(2);
      break;
    case RAFT_MSG_APPEND:
      {
        int ec=(int)fuzz_rand_mod(5);   /* 0..4 entries */
        int i;
        m.append_entries.term = fuzz_term();
        m.append_entries.prev_log_index = fuzz_index();
        m.append_entries.prev_log_term  = fuzz_term();
        m.append_entries.leader_commit  = fuzz_index();
        m.append_entries.leader_id      = fuzz_id();
        m.append_entries.entry_count    = ec;
        if(ec>0){
          for(i=0;i<ec;i++) fuzz_sizes[i]=fuzz_rand_mod(16);  /* total <= 64 < arena */
          m.append_entries.entry_terms      = fuzz_terms;
          m.append_entries.entry_kinds      = fuzz_kinds;
          m.append_entries.entry_data       = fuzz_arena;
          m.append_entries.entry_data_sizes = fuzz_sizes;
          m.append_entries.entry_cfg_old      = fuzz_masks;
          m.append_entries.entry_cfg_new      = fuzz_masks;
          m.append_entries.entry_cfg_learners = fuzz_masks;
        }
      }
      break;
    case RAFT_MSG_APPEND_RESULT:
      m.append_entries_result.term = fuzz_term();
      m.append_entries_result.rejected = fuzz_index();
      m.append_entries_result.last_log_index = fuzz_index();
      m.append_entries_result.conflict_term = fuzz_term();
      m.append_entries_result.conflict_first_index = fuzz_index();
      m.append_entries_result.success = (int)fuzz_rand_mod(2);
      break;
    case RAFT_MSG_INSTALL_SNAPSHOT:
      m.install_snapshot.term = fuzz_term();
      m.install_snapshot.snapshot_last_index = fuzz_index();
      m.install_snapshot.snapshot_last_term  = fuzz_term();
      m.install_snapshot.snapshot_offset     = fuzz_index();
      m.install_snapshot.snapshot_data_size  = fuzz_index();
      m.install_snapshot.snapshot_chunk_size = fuzz_index();
      m.install_snapshot.snapshot_cfg_old      = fuzz_mask();
      m.install_snapshot.snapshot_cfg_new      = fuzz_mask();
      m.install_snapshot.snapshot_cfg_learners = fuzz_mask();
      m.install_snapshot.snapshot_data       = fuzz_arena;
      m.install_snapshot.snapshot_done       = (int)fuzz_rand_mod(2);
      m.install_snapshot.leader_id           = fuzz_id();
      break;
    case RAFT_MSG_INSTALL_SNAPSHOT_RESULT:
      m.install_snapshot_result.term = fuzz_term();
      m.install_snapshot_result.last_included_index = fuzz_index();
      break;
    case RAFT_MSG_READ_INDEX:
      m.read_index_req.term = fuzz_term();
      break;
    case RAFT_MSG_READ_INDEX_RESULT:
      m.read_index_result.term = fuzz_term();
      m.read_index_result.read_index = fuzz_index();
      break;
    case RAFT_MSG_TIMEOUT_NOW:
      m.timeout_now.term = fuzz_term();
      m.timeout_now.last_log_index = fuzz_index();
      m.timeout_now.last_log_term  = fuzz_term();
      m.timeout_now.leader_id      = fuzz_id();
      break;
    default:
      break;
  }
  act_log("peer type=%d from=%d to=%d",m.type,m.from,m.to);
  (void)raft_recvfrom_peer(r,&m);
}

/* ---- action: send one random client request ---- */
static void fuzz_send_client(raft_ctx *r){
  raft_client_message c;
  int i,n;
  memset(&c,0,sizeof(c));
  c.type = (int)fuzz_rand_mod(6) + 1;  /* RAFT_CLIENT_SUBMIT .. TRANSFER */
  c.cookie = (fuzz_rand_mod(4)==0) ? 0 : (const void*)1;  /* mostly valid cookie */
  switch(c.type){
    case RAFT_CLIENT_SUBMIT:
      n=(int)fuzz_rand_mod(5);          /* 0..4 commands */
      c.submit.commands = fuzz_cmds;
      c.submit.count = n;
      for(i=0;i<4;i++){
        fuzz_cmds[i].cookie = (const void*)1;
        fuzz_cmds[i].command = fuzz_arena + (i*16);
        fuzz_cmds[i].command_size = fuzz_rand_mod(64);
      }
      break;
    case RAFT_CLIENT_BARRIER:
      break;                            /* cookie only */
    case RAFT_CLIENT_RECONFIG:
      c.reconfig.ids = fuzz_ids;
      c.reconfig.id_count = (int)fuzz_rand_mod(9);
      break;
    case RAFT_CLIENT_ADD_LEARNER:
    case RAFT_CLIENT_REMOVE_LEARNER:
      c.learner.learner_id = fuzz_id();
      break;
    case RAFT_CLIENT_TRANSFER:
      c.transfer.target_id = fuzz_id();
      break;
    default:
      break;
  }
  act_log("client type=%d",c.type);
  (void)raft_recvfrom_client(r,&c);
}

/* ---- action: advance and consume the Ready (zero-copy contract) ---- */
static void fuzz_advance(raft_ctx *r){
  raft_ready ready;
  unsigned int ms=fuzz_rand_mod(2000);
  act_log("advance %ums",ms);
  (void)raft_advance(r,ms,&ready);
  check_invariants(&ready);
  raft_ready_consumed(r);
}

/* ---- one node lifecycle driven by the PRNG ---- */
static void fuzz_one_node(unsigned long seed){
  raft_ctx *r;
  raft_config cfg;
  raft_persist rp;
  raft_persist_entry rpe[4];
  int i,n_actions,with_restore;

  fuzz_state = (unsigned long long)seed;
  fuzz_oom_at = 0;
  fuzz_alloc_calls = 0;
  act_count = 0;
  g_last_term = RAFT_I64_C(-1);
  g_last_commit = RAFT_I64_C(-1);
  g_last_apply = RAFT_I64_C(-1);

  /* ~1/3 of iterations inject a single-point OOM failure (1..128th alloc) */
  if(fuzz_rand_mod(3)==0) fuzz_oom_at = 1 + fuzz_rand_mod(128);

  /* random config: id always valid (1..5); peers/timing/chunk sizes adversarial */
  memset(&cfg,0,sizeof(cfg));
  cfg.id = (int)fuzz_rand_mod(5) + 1;
  {
    /* Valid, unique, self-included peer set: raft_create rejects id<=0 (invalid),
       duplicate ids, and a config that excludes self ($4.4).  Generate the list
       to SATISFY all three so raft_create accepts and the action loop runs; the
       adversarial fuzz_id() stays on the MESSAGE fields, not the config. */
    int pn=(int)fuzz_rand_mod(8);       /* 0..7 EXTRA peers */
    int j,k,pid,dup;
    fuzz_ids[0]=cfg.id;                 /* self is always the first member */
    for(j=1;j<=pn;j++){
      do{
        pid=fuzz_id_valid();
        dup=0;
        for(k=0;k<j;k++) if(fuzz_ids[k]==pid){ dup=1; break; }
      }while(dup);
      fuzz_ids[j]=pid;
    }
    cfg.peers = fuzz_ids;
    cfg.peer_count = pn + 1;
  }
  cfg.heartbeat_ms = fuzz_rand_mod(999) + 1;      /* >0 (0 is rejected at create, 2662) */
  cfg.election_min_ms = fuzz_rand_mod(999) + 1;   /* >0 (0 is rejected at create, 2662) */
  cfg.election_max_ms = fuzz_rand_mod(2000);      /* adversarial: max<=min is clamped (range=0) */
  cfg.seed = fuzz_rand();
  cfg.snapshot_chunk_size = fuzz_rand_mod(8192) + 1;
  cfg.log_chunk_size = (int)fuzz_rand_mod(4096);   /* 0 -> default, 6 -> non-pow2 */
  cfg.restore = 0;

  /* occasionally attach a VALID restore view (in-range terms/indexes, contiguous
     log entries, empty cfg masks) so raft_create accepts it and the action loop
     runs on a restored node.  The malformed-restore rejection path is covered by
     unit tests (see raft_test) instead of incidental fuzz rejection. */
  with_restore = (fuzz_rand_mod(4)==0);
  if(with_restore){
    int ec=(int)fuzz_rand_mod(5);       /* 0..4 restored log entries */
    int j;
    raft_i64 base=(raft_i64)fuzz_rand_mod(16);   /* last_included_index >= 0 */
    memset(&rp,0,sizeof(rp));
    rp.term = (raft_i64)fuzz_rand_mod(64);       /* valid: 0..63 < TERM_MAX+1 */
    rp.voted_for = fuzz_id();                    /* not validated on restore: keep adversarial */
    rp.last_included_index = base;
    rp.last_included_term = (raft_i64)fuzz_rand_mod(16);
    rp.snapshot_size = (raft_i64)fuzz_rand_mod(1024);
    rp.snapshot_cfg_old.ids = 0;      rp.snapshot_cfg_old.id_count = 0;
    rp.snapshot_cfg_new.ids = 0;      rp.snapshot_cfg_new.id_count = 0;
    rp.snapshot_cfg_learners.ids = 0; rp.snapshot_cfg_learners.id_count = 0;
    for(j=0;j<ec;j++){
      rpe[j].index = base + (raft_i64)(j + 1);   /* contiguous from base+1 */
      rpe[j].term = (raft_i64)fuzz_rand_mod(64); /* valid term */
      rpe[j].kind = (int)fuzz_rand_mod(2);       /* COMMAND(0) or NOOP(1) */
      rpe[j].data = fuzz_arena;
      rpe[j].data_size = fuzz_rand_mod(16);
      rpe[j].cfg_old.ids = 0;      rpe[j].cfg_old.id_count = 0;
      rpe[j].cfg_new.ids = 0;      rpe[j].cfg_new.id_count = 0;
      rpe[j].cfg_learners.ids = 0; rpe[j].cfg_learners.id_count = 0;
    }
    rp.log_entries = rpe;
    rp.log_entry_count = ec;
    cfg.restore = &rp;
  }

  r = raft_create(&cfg);

  n_actions = (int)fuzz_rand_mod(64) + 1;
  for(i=0;i<n_actions && r!=0;i++){
    int op=(int)fuzz_rand_mod(22);
    switch(op){
      case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8:
        fuzz_fill_arena();
        fuzz_send_peer(r);
        break;
      case 9: case 10: case 11: case 12: case 13:
        fuzz_fill_arena();
        fuzz_send_client(r);
        break;
      case 14: case 15:
        fuzz_advance(r);
        break;
      case 16:
        act_log("persist_complete + snapshot_persist_complete");
        (void)raft_persist_complete(r,fuzz_index());
        (void)raft_snapshot_persist_complete(r,fuzz_index());
        break;
      case 17:
        act_log("apply_complete");
        (void)raft_apply_complete(r,fuzz_index());
        break;
      case 18:
        act_log("snapshot");
        (void)raft_snapshot(r);
        break;
      case 19:
        act_log("snapshot_data_ready");
        (void)raft_snapshot_data_ready(r,fuzz_index());
        break;
      case 20:
        fuzz_fill_arena();   /* refresh the arena: op 20 reads fuzz_arena without a peer/client send */
        act_log("snapshot_data_provided");
        (void)raft_snapshot_data_provided(r,fuzz_id(),fuzz_index(),fuzz_arena,fuzz_rand_mod(64));
        break;
      default:
        act_log("stop");
        raft_stop(r);
        break;
    }
  }
  if(r) raft_destroy(r);
}

int main(int argc,char **argv){
  unsigned long seed = 1;
  unsigned long count = 100000;
  unsigned long i;
  if(argc>1) seed = (unsigned long)strtoul(argv[1],0,10);
  if(argc>2) count = (unsigned long)strtoul(argv[2],0,10);
  signal(SIGSEGV,crash_dump);
  signal(SIGILL,crash_dump);
  signal(SIGABRT,crash_dump);
  for(i=0;i<count;i++){
    /* print the seed BEFORE running so a crash is reproducible via the last line */
    fprintf(stderr,"seed %lu\n",seed + i);
    fflush(stderr);
    fuzz_one_node(seed + i);
  }
  fprintf(stderr,"done: %lu iterations\n",count);
  return 0;
}
