/* RAFT.H -- Poll-based Raft consensus library (single-header C89)
   ================================================================
   NOT THREAD-SAFE: all calls must be serialized by the caller.
   The library maintains internal mutable state with no locks.

   Zero-callback, pull-driven design. A single advancement primitive
   collects all pending work into a Ready structure; the caller
   fulfills each category externally (persist, apply, send, deliver).

   Lifecycle: READY -> RUNNING -> STOPPING -> DRAINING -> STOPPED.
   raft_create returns a node already in READY (no UNINITIALIZED state).

   Zero-copy contract: all pointers in Ready (messages[].entry_data,
   apply_entries[].command, etc.) point into internal raft_ctx buffers.
   They are valid only until the next library call that may modify
   state.  The caller MUST call raft_ready_consumed() to release the
   Ready buffers only after it has finished processing all pointers.

   Accumulation: raft_advance does not clear prior unconsumed output;
   repeated calls accumulate their results, and the latest Ready is the
   complete union of all unconsumed work.  raft_ready_consumed resets
   the accumulation.  Pointers from an EARLIER Ready are not valid
   across a later advance (internal buffers may be reallocated).

   Client semantics: commands are at-least-once.  A retried command may
   be appended and applied more than once.  Exactly-once (deduplication)
   is the application's responsibility: embed a unique client_id and
   sequence number inside the command and maintain a session
   (serial -> response) in the replicated state machine; the response
   must be persisted atomically with the command's effect to survive
   failover.  The cookie field is a volatile correlation tag, not a
   deduplication key: it is echoed back in the terminal result
   (COMMITTED / REDIRECT / FAILED), and for ADD_LEARNER and a deferred
   reconfig the catch-up progress is additionally reported as
   CATCHUP_READY / CATCHUP_FAILED carrying the same caller cookie.

   Commitment and durability: commit_index advances when a majority of
   the current configuration has ACKed an entry from the current term.
   A follower's success ACK is released only after the caller reports
   durability via raft_persist_complete, so the caller MUST invoke
   raft_persist_complete only after entries up to durable_index (plus the
   term/voted_for shown in Ready.persist) are fsync'ed to stable storage.
   The ACK advertises the CONFIRMED durable frontier - the highest index
   the caller has reported durable, capped by the log tip at that report
   and cut down by every truncation / log replacement - never the raw log
   tip, and never an index a cut has invalidated.  This is the
   load-bearing assumption behind committing before the
   leader's own disk write (dissertation $10.2.1).  In a cluster with
   voter peers, the leader's own entry counts toward the commit majority
   ONLY once it is durable (durable_index reaches it); "committed" then
   always means "safe to apply" (a majority holds it durably).  A node
   with no voter peers (a single-node cluster, or a sole voter plus
   learners) is its own whole quorum, so commit_index advances on the
   leader's next tick without waiting for durability; there durability is
   entirely the caller's responsibility via raft_persist_complete /
   durable_index.

   Return values: raft_advance returns 1 (Ready has work), 0 (no work),
   or -1 (error).  raft_recvfrom_peer, raft_recvfrom_client and the
   mutation APIs return 0 on acceptance or safe ignoring, and -1 on
   rejection or resource failure (state is unchanged on -1; an OOM
   failure leaves the node consistent and retryable).  Client request
   outcomes are delivered asynchronously through Ready.client_results,
   not through return codes.

   API summary:
     raft_create / raft_destroy     lifecycle
     raft_advance                   single advancement primitive
     raft_stop / raft_should_stop   graceful shutdown
     raft_recvfrom_peer             protocol message reception (single unified entry)
     raft_recvfrom_client           client request (single unified entry)
     raft_snapshot                  initiate snapshot; compaction applies at raft_persist_complete
     raft_persist_complete          cumulative durable index notification
     raft_apply_complete            cumulative applied index notification
     raft_snapshot_data_provided    leader snapshot chunk read result
     raft_snapshot_data_ready       leader snapshot data file durable (total_size)
     raft_ready_consumed            release live buffers and reset accumulated Ready state
     raft_inspect                   diagnostic read-only snapshot
*/
#ifndef RAFT_H
#define RAFT_H
#ifdef __cplusplus
extern "C" {
#endif
#ifndef RAFT_DEF
#ifdef RAFT_STATIC
#define RAFT_DEF static
#else
#define RAFT_DEF extern
#endif
#endif
#if defined(_MSC_VER)
typedef unsigned __int64 raft_u64;
typedef __int64 raft_i64;
#define RAFT_U64_C(x) x##ui64
#define RAFT_I64_C(x) x##i64
/* printf format macros for raft_i64/raft_u64 (mirrors kbase.h's K_I64_FMT):
   MSVC 6.0 has no `long long`, so its printf spells a 64-bit conversion
   "I64d"; gcc spells it "lld".  Never write %lld literally - it is a C99
   conversion that MSVC 6.0's runtime does not accept. */
#define RAFT_I64_FMT "I64d"
#define RAFT_U64_FMT "I64u"
#else
typedef unsigned long long raft_u64;
typedef long long raft_i64;
#define RAFT_U64_C(x) x##ULL
#define RAFT_I64_C(x) x##LL
#define RAFT_I64_FMT "lld"
#define RAFT_U64_FMT "llu"
#endif
/* ---- node state ---- */
enum{
  RAFT_FOLLOWER=1,
  RAFT_CANDIDATE=2,
  RAFT_LEADER=3
};
/* ---- lifecycle phase ---- */
enum{
  RAFT_PHASE_READY=1,
  RAFT_PHASE_RUNNING=2,
  RAFT_PHASE_STOPPING=3,
  RAFT_PHASE_DRAINING=4,
  RAFT_PHASE_STOPPED=5
};
/* ---- client result status ---- */
enum{
  RAFT_CLIENT_REDIRECT=1,
  RAFT_CLIENT_COMMITTED=2,
  RAFT_CLIENT_READY=3,
  RAFT_CLIENT_FAILED=4,
  RAFT_CLIENT_CATCHUP_READY=5,
  RAFT_CLIENT_CATCHUP_FAILED=6
};
/* ---- entry kind ---- */
enum{
  RAFT_ENTRY_COMMAND=0,
  RAFT_ENTRY_NOOP=1,
  RAFT_ENTRY_CONFIG=2
};
/* ---- configuration mask ---- */
typedef struct raft_mask{
  const int *ids;
  int id_count;
} raft_mask;
/* ---- client command (single or batch submit) ----
   cookie: volatile correlation tag for the COMMITTED/REDIRECT result;
   it is not a deduplication key (see "Client semantics" above). */
typedef struct raft_command{
  const void *cookie;
  const void *command;
  unsigned int command_size;
} raft_command;
/* ---- protocol message types ---- */
typedef struct raft_request_vote{
  raft_i64 term;
  raft_i64 last_log_index;
  raft_i64 last_log_term;
  int candidate_id;
  int pre_vote;
} raft_request_vote;
typedef struct raft_request_vote_result{
  raft_i64 term;
  int vote_granted;
  int pre_vote;
} raft_request_vote_result;
typedef struct raft_append_entries{
  raft_i64 term;
  raft_i64 prev_log_index;
  raft_i64 prev_log_term;
  raft_i64 leader_commit;
  raft_u64 read_context;
  const raft_i64 *entry_terms;
  const unsigned char *entry_kinds;
  const void *entry_data;
  const unsigned int *entry_data_sizes;
  const raft_mask *entry_cfg_old;
  const raft_mask *entry_cfg_new;
  const raft_mask *entry_cfg_learners;
  int leader_id;
  int entry_count;
} raft_append_entries;
typedef struct raft_append_entries_result{
  raft_i64 term;
  raft_i64 rejected;
  raft_i64 last_log_index;
  raft_i64 conflict_term;
  raft_i64 conflict_first_index;
  raft_u64 read_context;
  int success;
} raft_append_entries_result;
typedef struct raft_install_snapshot{
  raft_i64 term;
  raft_i64 snapshot_last_index;
  raft_i64 snapshot_last_term;
  raft_i64 snapshot_offset;
  raft_i64 snapshot_data_size;
  raft_i64 snapshot_chunk_size;
  raft_mask snapshot_cfg_old;
  raft_mask snapshot_cfg_new;
  raft_mask snapshot_cfg_learners;
  const void *snapshot_data;
  int snapshot_done;
  int leader_id;
} raft_install_snapshot;
typedef struct raft_install_snapshot_result{
  raft_i64 term;
  raft_i64 last_included_index; /* snapshot the follower ACTUALLY installed */
} raft_install_snapshot_result;
typedef struct raft_timeout_now{
  raft_i64 term;
  raft_i64 last_log_index;
  raft_i64 last_log_term;
  int leader_id;
} raft_timeout_now;
typedef struct raft_read_index_req{
  raft_i64 term;
  /* Round identity (Sec. 6.4).  The leader echoes it in the result so the
     follower only consumes the answer to the round it is currently waiting
     for: a DELAYED or DUPLICATED result from an earlier round otherwise
     resolves a later batch of barriers with the older read index, which
     serves a stale read even though every entry up to that older index was
     committed.  (kdb cluster fuzz seed 7429: a duplicate result resolved a
     fresh barrier at applied 19 while the submitted commit frontier was 21.) */
  raft_u64 context;
} raft_read_index_req;
typedef struct raft_read_index_result{
  raft_i64 term;
  raft_i64 read_index;
  raft_u64 context;   /* echo of the request's round identity; 0 = legacy/broadcast */
} raft_read_index_result;
/* ---- protocol message in Ready (tagged union) ---- */
enum{
  RAFT_MSG_REQUEST_VOTE=1,
  RAFT_MSG_REQUEST_VOTE_RESULT=2,
  RAFT_MSG_APPEND=3,
  RAFT_MSG_APPEND_RESULT=4,
  RAFT_MSG_INSTALL_SNAPSHOT=5,
  RAFT_MSG_INSTALL_SNAPSHOT_RESULT=6,
  RAFT_MSG_READ_INDEX=7,
  RAFT_MSG_READ_INDEX_RESULT=8,
  RAFT_MSG_TIMEOUT_NOW=9
};
typedef struct raft_peer_message{
  int type;
  int from;
  int to;
  raft_i64 term;
  union{
    raft_request_vote request_vote;
    raft_request_vote_result request_vote_result;
    raft_append_entries append_entries;
    raft_append_entries_result append_entries_result;
    raft_install_snapshot install_snapshot;
    raft_install_snapshot_result install_snapshot_result;
    raft_timeout_now timeout_now;
    raft_read_index_req read_index_req;
    raft_read_index_result read_index_result;
  };
} raft_peer_message;
/* ---- client request (tagged union, dispatched by raft_recvfrom_client) ---- */
enum{
  RAFT_CLIENT_SUBMIT=1,
  RAFT_CLIENT_BARRIER=2,
  RAFT_CLIENT_RECONFIG=3,
  RAFT_CLIENT_ADD_LEARNER=4,
  RAFT_CLIENT_REMOVE_LEARNER=5,
  RAFT_CLIENT_TRANSFER=6
};
typedef struct raft_client_message{
  int type;
  const void *cookie; /* barrier/reconfig/add/remove/transfer use; submit ignores */
  union{
    struct{
      const raft_command *commands;
      int count;
    } submit;
    /* barrier: no extra fields (cookie only) */
    struct{
      const int *ids;
      int id_count;
    } reconfig;
    struct{
      int learner_id;
    } learner; /* ADD_LEARNER / REMOVE_LEARNER */
    struct{
      int target_id;
    } transfer;
  };
} raft_client_message;
/* ---- Ready: application entry ---- */
typedef struct raft_apply_entry{
  raft_i64 index;
  raft_i64 term;
  int kind;                    /* RAFT_ENTRY_COMMAND / RAFT_ENTRY_NOOP / RAFT_ENTRY_CONFIG */
  const void *command;
  unsigned int command_size;
  const void *cookie;          /* client_pending cookie for this entry (0 if none) */
  void *response;              /* reserved (unused) */
  unsigned int response_capacity; /* reserved (unused) */
  const raft_mask *cfg_old;      /* RAFT_ENTRY_CONFIG only: old voter set (else 0) */
  const raft_mask *cfg_new;      /* RAFT_ENTRY_CONFIG only: new voter set (else 0) */
  const raft_mask *cfg_learners; /* RAFT_ENTRY_CONFIG only: learner set (else 0) */
} raft_apply_entry;
/* ---- Ready: client result ---- */
typedef struct raft_client_result{
  const void *cookie;
  const void *response;        /* reserved (unused): result lives in the app state machine */
  unsigned int response_size;  /* reserved (unused) */
  int status;
  int leader_id;
} raft_client_result;
/* ---- Ready: snapshot read request ---- */
typedef struct raft_snapshot_read_req{
  int follower_id;
  raft_i64 last_included_index;
  raft_i64 byte_offset;
  unsigned int byte_count;
} raft_snapshot_read_req;
/* ---- Persistence: durable log entry view ---- */
typedef struct raft_persist_entry{
  raft_i64 index;
  raft_i64 term;
  int kind;
  const void *data;
  unsigned int data_size;
  raft_mask cfg_old;
  raft_mask cfg_new;
  raft_mask cfg_learners;
} raft_persist_entry;
/* ---- Persistence output (value struct; internal pointers share Ready lifetime) ---- */
typedef struct raft_persist{
  raft_i64 term;
  int voted_for;
  raft_i64 last_included_index;
  raft_i64 last_included_term;
  int snapshot_dirty;
  raft_i64 snapshot_size;
  raft_mask snapshot_cfg_old;
  raft_mask snapshot_cfg_new;
  raft_mask snapshot_cfg_learners;
  const raft_persist_entry *log_entries;
  int log_entry_count;
} raft_persist;
/* ---- Ready structure (all output in one place) ---- */
typedef struct raft_peer_health{
  int id;
  raft_i64 match_index;
  raft_i64 next_index;
  unsigned int missed_rounds; /* consecutive heartbeat rounds with no response (failure signal) */
} raft_peer_health;
typedef struct raft_ready{
  int has_work;
  int phase_stopped;
  raft_persist persist;
  int persist_needed;
  const raft_apply_entry *apply_entries;
  int apply_count;
  const raft_client_result *client_results;
  int client_result_count;
  const raft_peer_message *messages;
  int message_count;
  int snapshot_install_needed;
  raft_i64 snapshot_last_index;
  raft_i64 snapshot_last_term;
  raft_i64 snapshot_size;
  raft_i64 commit_index;
  int leader_change;
  int is_leader;
  /* Membership as FACTS the application needs for its own policy (issue #14, C6): the application used to
     read raft_ctx's config_joint/config_new/config_learners directly, which is policy reaching into
     mechanism.  raft.h states what is true; what to DO about it stays in the application. */
  int config_joint;      /* a joint (C_old,C_new) configuration is currently in force */
  int self_is_voter;     /* this node is in the current configuration's voters */
  int self_is_learner;   /* this node is in the learners list */
  int leader_id;
  const raft_peer_health *peer_health; /* per-peer liveness (leader only), valid until next raft_advance */
  int peer_health_count;
  const raft_snapshot_read_req *snapshot_reads;
  int snapshot_read_count;
} raft_ready;
/* ---- Configuration (zero callbacks) ---- */
typedef struct raft_config{
  int id;
  const int *peers;
  int peer_count;
  int bootstrap;               /* peer_count==0 && bootstrap: empty config (non-voting new node, Sec 4.4) */
  unsigned int heartbeat_ms;
  unsigned int election_min_ms;
  unsigned int election_max_ms;
  unsigned int seed;
  unsigned int snapshot_chunk_size;
  int log_chunk_size;
  const raft_persist *restore;
} raft_config;
/* ---- Diagnostic info ---- */
typedef struct raft_peer_info{
  int id;
  int is_learner;
  raft_i64 match_index;
  raft_i64 next_index;
  int in_old_config;
  int in_new_config;
  int catchup_round;
  unsigned int catchup_round_elapsed;
} raft_peer_info;
typedef struct raft_info{
  int id;
  int state;
  int leader_id;
  int voted_for;
  int phase;
  raft_i64 term;
  raft_i64 log_entry_count;  /* entries after last_included_index; total log = last_included_index + log_entry_count */
  raft_i64 commit_index;
  raft_i64 last_applied;
  raft_i64 last_included_index;
  raft_i64 last_included_term;
  int peer_count;
  unsigned int election_elapsed;
  unsigned int election_deadline;
  int in_pre_vote;
  int config_joint;
  int leadership_confirmed;
  int quorum_acked;
  int snapshot_install_pending;
  /* peer info: set to caller buffer before call to fill peer_count entries, NULL to skip */
  raft_peer_info *peers;
} raft_info;
/* ---- Opaque context ---- */
typedef struct raft_ctx raft_ctx;
/* ---- Lifecycle ---- */
RAFT_DEF raft_ctx *raft_create(const raft_config *cfg);
RAFT_DEF void raft_destroy(raft_ctx *r);
/* ---- Advancement (sole driver of all output and phase transitions) ---- */
RAFT_DEF int raft_advance(raft_ctx *r,unsigned int elapsed_ms,raft_ready *ready);
/* ---- Shutdown ---- */
RAFT_DEF void raft_stop(raft_ctx *r);
RAFT_DEF int raft_should_stop(const raft_ctx *r);
/* ---- Protocol message reception (single unified entry) ---- */
RAFT_DEF int raft_recvfrom_peer(raft_ctx *r,const raft_peer_message *msg);
/* ---- Client request (single unified entry) ---- */
RAFT_DEF int raft_recvfrom_client(raft_ctx *r,const raft_client_message *msg);
/* ---- Compaction ---- */
RAFT_DEF int raft_snapshot(raft_ctx *r);
/* ---- Completion notifications (cumulative) ---- */
RAFT_DEF int raft_persist_complete(raft_ctx *r,raft_i64 durable_index);
RAFT_DEF int raft_snapshot_persist_complete(raft_ctx *r,raft_i64 durable_index);
RAFT_DEF int raft_apply_complete(raft_ctx *r,raft_i64 applied_index);
/* ---- Snapshot data flow (leader streaming path) ---- */
RAFT_DEF int raft_snapshot_data_provided(raft_ctx *r,int follower_id,raft_i64 byte_offset,const void *data,unsigned int size);
RAFT_DEF int raft_snapshot_data_ready(raft_ctx *r,raft_i64 total_size);
/* ---- Ready consumption: release live buffers and reset accumulated state ---- */
RAFT_DEF void raft_ready_consumed(raft_ctx *r);
/* ---- Diagnostic ---- */
RAFT_DEF int raft_inspect(const raft_ctx *r,raft_info *out);
#ifdef __cplusplus
}
#endif
#endif
#if defined(RAFT_IMPLEMENTATION)&&!defined(RAFT_IMPLEMENTATION_ONCE)
#define RAFT_IMPLEMENTATION_ONCE
#include <stdlib.h>
#include <string.h>
#ifndef RAFT_MALLOC
#define RAFT_MALLOC malloc
#endif
#ifndef RAFT_FREE
#define RAFT_FREE free
#endif
#ifndef RAFT_CALLOC
#define RAFT_CALLOC calloc
#endif
#ifndef RAFT_REALLOC
#define RAFT_REALLOC realloc
#endif
/* ---- term safety cap (prevents INT64 overflow in term++ operations) ---- */
#define RAFT_TERM_MAX RAFT_I64_C(9223372036854775806)  /* INT64_MAX-1: highest term value the library will ever hold. INT64_MAX is the out-of-range sentinel rejected by every RPC guard; raft_become_candidate blocks term++ once current_term reaches this, so term++ can never produce INT64_MAX. */
/* ---- learner catchup round limit ($4.2.1, shared by learner and deferred-reconfig paths) ---- */
#define RAFT_CATCHUP_ROUNDS 10
/* ---- internal types ---- */
typedef struct raft_set{
  int *ids;
  int id_count;
} raft_set;
typedef struct raft_peer{
  int id;
  int is_learner;
  raft_i64 next_index;
  raft_i64 match_index;
  raft_i64 pending_snapshot_offset;
  raft_i64 pending_snapshot_last_index; /* last_index of the snapshot being streamed to this peer */
  unsigned char vote;
  unsigned char pre_vote_acked;
  unsigned char in_old_config;
  unsigned char in_new_config;
  unsigned char read_index_acked; /* dedup per-round ReadIndex ACK */
  unsigned char quorum_acked;     /* dedup per-window checkQuorum contact */
  unsigned int missed_rounds;     /* consecutive heartbeat rounds with no response */
  /* learner catchup ($4.2.1 round-based algorithm) */
  int catchup_round;
  raft_i64 catchup_round_start_index;
  unsigned int catchup_round_elapsed;
  const void *catchup_cookie; /* ADD_LEARNER caller cookie for CATCHUP_READY/FAILED */
} raft_peer;
typedef struct raft_log_chunk{
  raft_i64 *terms;
  unsigned char *kinds;
  raft_mask *cfg_old;
  raft_mask *cfg_new;
  raft_mask *cfg_learners;
  raft_i64 *data_offsets;
} raft_log_chunk;
typedef struct raft_log{
  raft_log_chunk *chunks;
  int num_chunks;
  int chunk_capacity;
  int chunk_bits;
  int chunk_mask;
  int chunk_size;
  unsigned char *data;
  raft_u64 data_capacity;
  raft_u64 data_size;
  raft_i64 count;
  raft_i64 last_included_index;
  raft_i64 last_included_term;
} raft_log;
typedef struct raft_snapshot_meta{
  raft_i64 last_index;
  raft_i64 last_term;
  raft_i64 size;
  raft_set cfg_old;
  raft_set cfg_new;
  raft_set cfg_learners;
} raft_snapshot_meta;
typedef struct raft_read_barrier{
  const void *cookie;
  raft_i64 target_index;
  unsigned int needs_gen; /* must wait for read_barrier_gen >= this */
} raft_read_barrier;
typedef struct raft_client_pending{
  const void *cookie;
  raft_i64 log_index;
} raft_client_pending;
typedef struct raft_read_index_pending{
  int from;
  raft_i64 index; /* captured at receipt; 0 = capture at flush (leader not yet confirmed) */
  raft_u64 context; /* requesting follower's round identity, echoed back at flush */
} raft_read_index_pending;
struct raft_ctx{
  raft_config cfg;
  int prev_leader_state;
  int phase;
  int state;
  int voted_for;
  int leader_id;
  /* treap-style accumulated Ready state (OR semantics, cleared by ready_consumed) */
  int ready_persist_pending;
  int ready_leader_change;
  raft_i64 current_term;
  raft_i64 commit_index;
  raft_i64 last_applied;
  raft_i64 apply_offered;
  unsigned int rng;
  unsigned int heartbeat_elapsed;
  unsigned int election_elapsed;
  unsigned int election_deadline;
  unsigned int leader_contact_elapsed;
  int in_pre_vote;
  int transfer_target;
  unsigned int transfer_elapsed;
  const void *transfer_cookie;   /* cookie for leadership transfer result notification */
  int persist_needed;
  int peer_count;
  raft_peer *peers;
  raft_log log;
  raft_set config_old;
  raft_set config_new;
  raft_set config_learners;
  raft_set config_bootstrap; /* bootstrap voter set: the base config at last_included_index==0 */
  int config_joint;
  raft_set config_pending_old;
  raft_set config_pending_new;
  raft_set config_pending_learners;
  int config_pending;
  raft_i64 config_index;
  raft_i64 config_applied_index;
  int config_apply_stale; /* a config-immediate apply failed (OOM): the live
      config may not match the log's latest config entry; must be re-applied
      before campaigning / on the next AppendEntries */
  raft_snapshot_meta snapshot;
  raft_snapshot_meta snapshot_pending;
  int snapshot_pending_dirty;
  int snapshot_data_ready_flag;
  raft_snapshot_meta snapshot_recv;
  raft_i64 snapshot_recv_expected_offset;
  int snapshot_install_pending;
  int deferred_vote_response;
  int deferred_vote_to;
  unsigned int deferred_vote_gen;
  int deferred_vote_request;       /* candidate: real RequestVote broadcast pending term/vote persist */
  unsigned int deferred_vote_request_gen; /* persist_gen that must be reached before broadcasting */
  int deferred_append_response;
  int deferred_append_to;
  /* A DEFERRED REJECTION (issue #13): the reply carries our current term, so it must not leave before
     that term is durable (Sec. 3.8).  It keeps its OWN latch on purpose - an ACK that is already pending
     must not be overwritten by a rejection (and vice versa): converting one into the other loses the
     match progress the leader needs and stalls its read barrier (raft_cluster_fuzz seed 869). */
  int deferred_reject_pending;
  unsigned int deferred_reject_gen;             /* persist_gen that must be reached before it is sent */
  raft_i64 deferred_reject_hint;                /* the rejection's "rejected" value */
  raft_i64 deferred_reject_to;                  /* leader it is addressed to */
  raft_i64 deferred_reject_last_index;          /* captured at rejection time, as the immediate path does */
  raft_i64 deferred_reject_conflict_term;
  raft_i64 deferred_reject_conflict_first_index;
  raft_u64 deferred_reject_read_context;
  raft_i64 deferred_append_last_index; /* highest confirmed frontier already reported to the leader */
  raft_i64 deferred_append_confirmed;  /* largest prev+entry_count this follower has accepted since the
      latch was taken: the range an AppendEntries ACK may advertise (paper TLA+
      msuccess: mmatchIndex = mprevLogIndex + Len(mentries)), never the log tip -
      entries above it were not verified against the leader by that RPC and
      counting them lets the leader commit a "phantom majority" */
  raft_u64 deferred_append_read_context; /* latest valid AppendEntries round echoed by the durable ACK */
  unsigned int deferred_append_gen;   /* persist_gen that must be reached before the ACK is released */
  unsigned int persist_gen;  /* bumped by every persist completion */
  /* The generation that will carry the CURRENT term/vote to disk (Sec. 3.8).  A reply that advertises
     our term may only leave once persist_gen has reached it - the vote path and the successful-AE ACK
     already work this way, and issue #13's rejection path now does too.  Narrow on purpose: an
     unpersisted log APPEND must not delay a rejection whose term is already durable. */
  unsigned int term_dirty_gen;
  raft_i64 durable_index;
  /* Highest log index the CALLER has actually written to its WAL (the reported value
     clamped to the log tip at report time, and lowered by any truncation).  The persist
     view only carries entries above it, so a record is a DELTA instead of a re-write of
     everything since the snapshot base - which is what made each record cost ~3x the new
     bytes (megabytes at 64 KiB values). */
  raft_i64 persist_synced_index; /* latest durable index reported by raft_persist_complete */
  raft_i64 durable_confirm; /* highest index the caller has confirmed durable for the
      CURRENT log prefix: min(durable_index, log tip at that report), monotone while
      the prefix is unchanged.  A follower's success ACK may never claim more than
      this - match index means "durably written to this server's disk" (dissertation
      10.2.1), never "in memory".  Cut down at every truncation / log replacement. */
  raft_read_barrier *read_barriers;
  int read_barrier_count;
  int read_barrier_capacity;
  int leadership_confirmed;
  unsigned int read_barrier_gen; /* incremented on heartbeat quorum; leader barriers wait for gen > needs_gen */
  void **follower_read_pending; /* cookies awaiting a ReadIndex result (FIFO) */
  int follower_read_pending_count;
  int follower_read_pending_capacity;
  int follower_read_frozen; /* leading cookies already sent in the in-flight ReadIndex */
  int follower_read_waiting; /* a ReadIndex request is queued to be emitted at next advance */
  raft_u64 follower_read_context; /* round identity of the in-flight ReadIndex (0 = none/first) */
  unsigned int follower_read_elapsed;
  unsigned int follower_read_barrier_elapsed; /* Sec. 6.4: non-leader read_barriers stalled below target */
  raft_i64 follower_read_barrier_last_applied; /* last_applied at the last progress sample */
  int read_index_pending_count;
  raft_read_index_pending *read_index_pending;
  int read_index_pending_capacity;
  raft_u64 read_context; /* unique round echoed by AppendEntriesResult */
  unsigned char read_index_heartbeat_sent; /* gate: count ReadIndex acks only after a fresh heartbeat */
  raft_client_pending *client_pending;
  int client_pending_count;        /* live entries: [client_pending_head, +count) */
  int client_pending_capacity;
  /* Entries are ordered by log_index and retired from the FRONT as they are applied,
     so completed ones are a prefix: a head cursor turns retirement into an index bump
     instead of a memmove per completed request (which made a deep pipeline quadratic
     in the number of in-flight client requests). */
  int client_pending_head;
  raft_u64 result_dropped;         /* results lost to allocation failure (never truncated) */
  raft_peer_message *msg_buf;
  int msg_count;
  int msg_capacity;
  raft_apply_entry *apply_buf;
  int apply_count;
  int apply_capacity;
  raft_peer_health *peer_health_buf;
  int peer_health_capacity;
  raft_client_result *result_buf;
  int result_count;
  int result_capacity;
  raft_snapshot_read_req *snap_read_buf;
  int snap_read_count;
  int snap_read_capacity;
  raft_persist_entry *persist_entry_buf;
  int persist_entry_capacity;
  unsigned int *entry_size_buf;
  int entry_size_cap;
  /* deferred reconfig: catch-up before joint creation ($4.1 single-server add
     via $4.3 joint consensus; round-based catch-up per $4.2.1) */
  raft_set config_catchup_new;
  const void *config_catchup_cookie;
  int config_catchup_active;
  int config_catchup_round;           /* 0 = not started; 1..RAFT_CATCHUP_ROUNDS */
  raft_i64 config_catchup_start_index; /* log-tip snapshot at the start of the current round */
  unsigned int config_catchup_elapsed; /* time in the CURRENT round (last round < election_min) */
  unsigned int config_catchup_total_elapsed; /* total since deferral (never-catch-up abort) */
  unsigned int joint_elapsed; /* time an uncommitted joint config has been stuck */
};
static int raft_id_valid(int id){
  return id>0;
}
static raft_mask raft_mask_zero(void){
  raft_mask m;
  m.ids=0;
  m.id_count=0;
  return m;
}
static int raft_mask_any(raft_mask m){
  return m.id_count>0;
}
static int raft_mask_eq(raft_mask a,raft_mask b){
  if(a.id_count!=b.id_count) return 0;
  if(a.id_count<=0) return 1;
  return memcmp(a.ids,b.ids,sizeof(int)*(unsigned int)a.id_count)==0;
}
static int raft_mask_has(raft_mask m,int id){
  int lo,hi,mid;
  if(!raft_id_valid(id)) return 0;
  lo=0;
  hi=m.id_count-1;
  while(lo<=hi){
    mid=lo+((hi-lo)/2);
    if(m.ids[mid]==id) return 1;
    if(m.ids[mid]<id) lo=mid+1;
    else hi=mid-1;
  }
  return 0;
}
static void raft_set_clear(raft_set *set){
  if(!set) return;
  if(set->ids) RAFT_FREE(set->ids);
  set->ids=0;
  set->id_count=0;
}
static raft_mask raft_set_view(const raft_set *set){
  raft_mask m;
  if(!set){
    m.ids=0;
    m.id_count=0;
    return m;
  }
  /* Copy field-by-field instead of type-punning the raft_set* as a raft_mask*:
     the two struct types are layout-compatible but distinct, and the cast
     violates strict aliasing (gcc -O2 may then reorder/cache the reads and
     observe a stale config). */
  m.ids=set->ids;
  m.id_count=set->id_count;
  return m;
}
static int raft_make_sorted_ids(const int *ids,int id_count,int **out){
  int i,j,v,*copy;
  if(!out) return -1;
  *out=0;
  if(id_count<0||(id_count>0&&!ids)) return -1;
  if(id_count<=0) return 0;
  copy=(int *)RAFT_MALLOC(sizeof(int)*(unsigned int)id_count);
  if(!copy) return -1;
  memcpy(copy,ids,sizeof(int)*(unsigned int)id_count);
  for(i=1;i<id_count;i++){
    v=copy[i];
    j=i-1;
    while(j>=0&&copy[j]>v){
      copy[j+1]=copy[j];
      j--;
    }
    copy[j+1]=v;
  }
  for(i=0;i<id_count;i++){
    if(!raft_id_valid(copy[i])||(i>0&&copy[i]==copy[i-1])){
      RAFT_FREE(copy);
      return -1;
    }
  }
  *out=copy;
  return 0;
}
static int raft_set_assign(raft_set *set,const int *ids,int id_count){
  int *copy;
  if(!set||raft_make_sorted_ids(ids,id_count,&copy)<0) return -1;
  if(set->ids) RAFT_FREE(set->ids);
  set->ids=copy;
  set->id_count=id_count;
  return 0;
}
static int raft_set_copy(raft_set *dst,const raft_set *src){
  if(!dst||!src) return -1;
  return raft_set_assign(dst,src->ids,src->id_count);
}
static int raft_phase_ge(const raft_ctx *r,int p){
  return r!=0&&r->phase>=p;
}
static int raft_set_has(const raft_set *set,int id){
  int i;
  if(!set||!set->ids||set->id_count<=0) return 0;
  for(i=0;i<set->id_count;i++) if(set->ids[i]==id) return 1;
  return 0;
}
static int raft_is_learner(const raft_set *learners,int id){
  int i;
  if(!learners||learners->id_count<=0) return 0;
  for(i=0;i<learners->id_count;i++){
    if(learners->ids[i]==id) return 1;
  }
  return 0;
}
/* saturating elapsed-time accumulator: wraps to a stuck-at-max value instead
   of 0, so a timeout comparison can never fire early after ~49 days of uptime
   (unsigned int milliseconds). */
static unsigned int raft_elapsed_add(unsigned int cur,unsigned int add){
  unsigned int next=cur+add;
  if(next<cur) next=~0U;
  return next;
}
/* ---- log operations ---- */
static raft_i64 raft_log_last_index(const raft_log *log){
  return log->last_included_index+log->count;
}
static raft_i64 raft_log_term_at(const raft_log *log,raft_i64 idx){
  int ci;
  raft_i64 off;
  if(idx<=log->last_included_index) return log->last_included_term;
  if(idx>raft_log_last_index(log)) return RAFT_I64_C(0);
  off=idx-log->last_included_index-1;
  ci=(int)(off>>log->chunk_bits);
  return log->chunks[ci].terms[(int)(off&log->chunk_mask)];
}
static int raft_log_ensure_chunk(raft_log *log,int ci){
  int nc,i;
  raft_log_chunk *nc_arr;
  if(ci<log->num_chunks) return 0;
  if(ci>=log->chunk_capacity){
    nc=log->chunk_capacity>0?log->chunk_capacity*2:4;
    if(nc<=ci) nc=ci+1;
    nc_arr=(raft_log_chunk *)RAFT_REALLOC(log->chunks,(unsigned int)nc*sizeof(raft_log_chunk));
    if(!nc_arr) return -1;
    log->chunks=nc_arr;
    log->chunk_capacity=nc;
  }
  for(i=log->num_chunks;i<=ci;i++) memset(&log->chunks[i],0,sizeof(raft_log_chunk));
  log->num_chunks=ci+1;
  return 0;
}
/* allocate the term/kind/data-offset arrays for one log chunk (caller already
   called raft_log_ensure_chunk); rolls back on OOM. */
static int raft_log_alloc_chunk(raft_log *log,int ci){
  unsigned int sz;
  if(log->chunks[ci].terms) return 0;
  sz=(unsigned int)log->chunk_size;
  log->chunks[ci].terms=(raft_i64 *)RAFT_CALLOC(sz,sizeof(raft_i64));
  log->chunks[ci].kinds=(unsigned char *)RAFT_CALLOC(sz,1);
  log->chunks[ci].data_offsets=(raft_i64 *)RAFT_CALLOC(sz,sizeof(raft_i64));
  if(!log->chunks[ci].terms||!log->chunks[ci].kinds||!log->chunks[ci].data_offsets){
    if(log->chunks[ci].terms) RAFT_FREE(log->chunks[ci].terms);
    if(log->chunks[ci].kinds) RAFT_FREE(log->chunks[ci].kinds);
    if(log->chunks[ci].data_offsets) RAFT_FREE(log->chunks[ci].data_offsets);
    log->chunks[ci].terms=0;
    log->chunks[ci].kinds=0;
    log->chunks[ci].data_offsets=0;
    return -1;
  }
  return 0;
}
/* grow the log data buffer by at least extra bytes (doubling, 4KB minimum);
   no-op if already large enough. */
static int raft_log_reserve_data(raft_log *log,raft_u64 extra){
  raft_u64 need,nc;
  unsigned char *nd;
  if(extra<=0) return 0;
  need=log->data_size+extra;
  if(need<=log->data_capacity) return 0;
  nc=log->data_capacity>0?log->data_capacity*2:4096;
  if(nc<need) nc=need;
  nd=(unsigned char *)RAFT_REALLOC(log->data,nc);
  if(!nd) return -1;
  log->data=nd;
  log->data_capacity=nc;
  return 0;
}
static int raft_log_append(raft_log *log,raft_i64 term,int kind,const void *data,unsigned int data_size,raft_mask cfg_old,raft_mask cfg_new,raft_mask cfg_learners){
  int ci,*cp=0,*cp2=0;
  raft_i64 off,idx,*terms,*offsets;
  unsigned char *kinds;
  unsigned int mi;
  raft_u64 saved_size;
  idx=raft_log_last_index(log)+1;
  off=idx-log->last_included_index-1;
  ci=(int)(off>>log->chunk_bits);
  if(raft_log_ensure_chunk(log,ci)<0) return -1;
  if(raft_log_alloc_chunk(log,ci)<0) return -1;
  terms=log->chunks[ci].terms;
  kinds=log->chunks[ci].kinds;
  offsets=log->chunks[ci].data_offsets;
  terms[(int)(off&log->chunk_mask)]=term;
  kinds[(int)(off&log->chunk_mask)]=(unsigned char)kind;
  offsets[(int)(off&log->chunk_mask)]=(raft_i64)log->data_size;
  saved_size=log->data_size; /* pre-copy size: full rollback target for config-mask OOM below */
  if(data_size>0&&data){
    if(raft_log_reserve_data(log,(raft_u64)data_size)<0) return -1;
    memcpy(log->data+log->data_size,data,data_size);
    log->data_size+=(raft_u64)data_size;
  }
  if(kind==RAFT_ENTRY_CONFIG&&raft_mask_any(cfg_old)){
    mi=(unsigned int)(off&log->chunk_mask);
    if(!log->chunks[ci].cfg_old){
      unsigned int sz2=(unsigned int)log->chunk_size;
      log->chunks[ci].cfg_old=(raft_mask *)RAFT_CALLOC(sz2,sizeof(raft_mask));
      log->chunks[ci].cfg_new=(raft_mask *)RAFT_CALLOC(sz2,sizeof(raft_mask));
      log->chunks[ci].cfg_learners=(raft_mask *)RAFT_CALLOC(sz2,sizeof(raft_mask));
      /* if any allocation failed, clean up all three so the chunk isn't corrupted */
      if(!log->chunks[ci].cfg_old||!log->chunks[ci].cfg_new||!log->chunks[ci].cfg_learners){
        if(log->chunks[ci].cfg_old) RAFT_FREE(log->chunks[ci].cfg_old);
        if(log->chunks[ci].cfg_new) RAFT_FREE(log->chunks[ci].cfg_new);
        if(log->chunks[ci].cfg_learners) RAFT_FREE(log->chunks[ci].cfg_learners);
        log->chunks[ci].cfg_old=0;
        log->chunks[ci].cfg_new=0;
        log->chunks[ci].cfg_learners=0;
        log->data_size=saved_size;
        return -1;
      }
    }
    if(cfg_old.id_count>0){
      cp=(int *)RAFT_MALLOC(sizeof(int)*(unsigned int)cfg_old.id_count);
      if(!cp){
        log->data_size=saved_size;
        return -1;
      }
      memcpy(cp,cfg_old.ids,sizeof(int)*(unsigned int)cfg_old.id_count);
      cfg_old.ids=cp;
    }
    if(cfg_new.id_count>0){
      cp2=(int *)RAFT_MALLOC(sizeof(int)*(unsigned int)cfg_new.id_count);
      if(!cp2){
        if(cp) RAFT_FREE(cp);
        log->data_size=saved_size;
        return -1;
      }
      memcpy(cp2,cfg_new.ids,sizeof(int)*(unsigned int)cfg_new.id_count);
      cfg_new.ids=cp2;
    }
    /* free old config mask ids before overwriting (truncated entry slots) */
    if(log->chunks[ci].cfg_old[mi].ids) RAFT_FREE((void*)log->chunks[ci].cfg_old[mi].ids);
    if(log->chunks[ci].cfg_new[mi].ids) RAFT_FREE((void*)log->chunks[ci].cfg_new[mi].ids);
    if(log->chunks[ci].cfg_learners&&log->chunks[ci].cfg_learners[mi].ids) RAFT_FREE((void*)log->chunks[ci].cfg_learners[mi].ids);
    log->chunks[ci].cfg_old[mi]=cfg_old;
    log->chunks[ci].cfg_new[mi]=cfg_new;
    if(raft_mask_any(cfg_learners)){
      int *cp3=(int *)RAFT_MALLOC(sizeof(int)*(unsigned int)cfg_learners.id_count);
      if(!cp3){
        /* rollback: zero chunk slots that reference cp/cp2, then free */
        log->chunks[ci].cfg_old[mi]=raft_mask_zero();
        log->chunks[ci].cfg_new[mi]=raft_mask_zero();
        log->chunks[ci].cfg_learners[mi]=raft_mask_zero();
        if(cp) RAFT_FREE(cp);
        if(cp2) RAFT_FREE(cp2);
        log->data_size=saved_size;
        return -1;
      }
      memcpy(cp3,cfg_learners.ids,sizeof(int)*(unsigned int)cfg_learners.id_count);
      log->chunks[ci].cfg_learners[mi].ids=cp3;
      log->chunks[ci].cfg_learners[mi].id_count=cfg_learners.id_count;
    }
  }
  log->count++;
  return 0;
}
/* ---- true batch append for client commands (no config masks) ---- */
static int raft_log_append_commands(raft_log *log,raft_i64 term,const raft_command *commands,int count){
  int i,ci_first,ci_last;
  raft_i64 off_first;
  raft_u64 total_data;
  if(count<=0) return 0;
  total_data=0;
  for(i=0;i<count;i++){
    if(commands[i].command_size>0&&commands[i].command) total_data+=(raft_u64)commands[i].command_size;
  }
  off_first=raft_log_last_index(log)+1-log->last_included_index-1;
  ci_first=(int)(off_first>>log->chunk_bits);
  ci_last=(int)((off_first+count-1)>>log->chunk_bits);
  if(raft_log_ensure_chunk(log,ci_last)<0) return -1;
  for(i=ci_first;i<=ci_last;i++){
    if(raft_log_alloc_chunk(log,i)<0) return -1;
  }
  if(raft_log_reserve_data(log,total_data)<0) return -1;
  for(i=0;i<count;i++){
    raft_i64 off=off_first+(raft_i64)i;
    int ci=(int)(off>>log->chunk_bits);
    int co=(int)(off&log->chunk_mask);
    log->chunks[ci].terms[co]=term;
    log->chunks[ci].kinds[co]=(unsigned char)RAFT_ENTRY_COMMAND;
    log->chunks[ci].data_offsets[co]=(raft_i64)log->data_size;
    if(commands[i].command_size>0&&commands[i].command){
      memcpy(log->data+log->data_size,commands[i].command,commands[i].command_size);
      log->data_size+=(raft_u64)commands[i].command_size;
    }
  }
  log->count+=(raft_i64)count;
  return 0;
}
/* ---- config mask memory ownership ---- */
static void raft_free_chunk_cfg(raft_log_chunk *c,int chunk_size){
  int j;
  if(c->cfg_old){
    for(j=0;j<chunk_size;j++){
      if(c->cfg_old[j].ids) RAFT_FREE((void*)c->cfg_old[j].ids);
    }
    RAFT_FREE(c->cfg_old);
    c->cfg_old=0;
  }
  if(c->cfg_new){
    for(j=0;j<chunk_size;j++){
      if(c->cfg_new[j].ids) RAFT_FREE((void*)c->cfg_new[j].ids);
    }
    RAFT_FREE(c->cfg_new);
    c->cfg_new=0;
  }
  if(c->cfg_learners){
    for(j=0;j<chunk_size;j++){
      if(c->cfg_learners[j].ids) RAFT_FREE((void*)c->cfg_learners[j].ids);
    }
    RAFT_FREE(c->cfg_learners);
    c->cfg_learners=0;
  }
}
static void raft_log_chunk_free(raft_log_chunk *c,int chunk_size){
  if(c->terms) RAFT_FREE(c->terms);
  if(c->kinds) RAFT_FREE(c->kinds);
  if(c->data_offsets) RAFT_FREE(c->data_offsets);
  raft_free_chunk_cfg(c,chunk_size);
}
static int raft_log_compact_prefix(raft_log *log,raft_i64 idx,raft_i64 term){
  raft_log next;
  raft_i64 entry,last,off,data_off,data_end;
  int ci,co;
  if(!log||idx<log->last_included_index||idx>raft_log_last_index(log)) return -1;
  memset(&next,0,sizeof(next));
  next.chunk_size=log->chunk_size;
  next.chunk_bits=log->chunk_bits;
  next.chunk_mask=log->chunk_mask;
  next.last_included_index=idx;
  next.last_included_term=term;
  last=raft_log_last_index(log);
  for(entry=idx+1;entry<=last;entry++){
    int kind;
    raft_mask cfg_old=raft_mask_zero(),cfg_new=raft_mask_zero(),cfg_learners=raft_mask_zero();
    off=entry-log->last_included_index-1;
    ci=(int)(off>>log->chunk_bits);
    co=(int)(off&log->chunk_mask);
    kind=(int)log->chunks[ci].kinds[co];
    data_off=log->chunks[ci].data_offsets[co];
    if(entry<last){
      int nci=ci,nco=co+1;
      if(nco>=log->chunk_size){
        nci++;
        nco=0;
      }
      data_end=log->chunks[nci].data_offsets[nco];
    }else data_end=(raft_i64)log->data_size;
    if(kind==RAFT_ENTRY_CONFIG&&log->chunks[ci].cfg_old){
      cfg_old=log->chunks[ci].cfg_old[co];
      cfg_new=log->chunks[ci].cfg_new[co];
      if(log->chunks[ci].cfg_learners) cfg_learners=log->chunks[ci].cfg_learners[co];
    }
    if(raft_log_append(&next,log->chunks[ci].terms[co],kind,data_end>data_off?log->data+data_off:0,(unsigned int)(data_end>data_off?data_end-data_off:0),cfg_old,cfg_new,cfg_learners)<0){
      int i;
      for(i=0;i<next.num_chunks;i++) raft_log_chunk_free(&next.chunks[i],next.chunk_size);
      if(next.chunks) RAFT_FREE(next.chunks);
      if(next.data) RAFT_FREE(next.data);
      return -1;
    }
  }
  for(ci=0;ci<log->num_chunks;ci++) raft_log_chunk_free(&log->chunks[ci],log->chunk_size);
  if(log->chunks) RAFT_FREE(log->chunks);
  if(log->data) RAFT_FREE(log->data);
  *log=next;
  return 0;
}
static int raft_log_truncate(raft_log *log,raft_i64 idx){
  raft_i64 new_count;
  int ci,lc;
  if(idx<log->last_included_index) return -1;
  /* Truncating to (or past) the very end is a no-op.  Without the >= guard,
     idx==last would read data_offsets of the first entry PAST the log: either
     out-of-bounds (when new_count is a chunk_size multiple) or a zeroed unused
     slot (-> data_size=0, silently corrupting subsequent appends). */
  if(idx>=raft_log_last_index(log)) return 0;
  new_count=idx-log->last_included_index;
  /* adjust data_size to first truncated entry's offset */
  if(new_count<=0) log->data_size=0;
  else{
    int dc=(int)(new_count>>log->chunk_bits);
    int dco=(int)(new_count&log->chunk_mask);
    log->data_size=log->chunks[dc].data_offsets[dco];
  }
  ci=(int)((new_count+log->chunk_size-1)>>log->chunk_bits);
  log->count=new_count;
  while(log->num_chunks>ci){
    lc=log->num_chunks-1;
    raft_log_chunk_free(&log->chunks[lc],log->chunk_size);
    memset(&log->chunks[lc],0,sizeof(raft_log_chunk));
    log->num_chunks--;
  }
  /* free config masks in truncated slots of the last kept chunk */
  if(ci>0&&log->chunks[ci-1].cfg_old){
    int j,dco=(int)(new_count&log->chunk_mask);
    if(dco>0){
      for(j=dco;j<log->chunk_size;j++){
        if(log->chunks[ci-1].cfg_old[j].ids){
          RAFT_FREE((void*)log->chunks[ci-1].cfg_old[j].ids);
          log->chunks[ci-1].cfg_old[j].ids=0;
          log->chunks[ci-1].cfg_old[j].id_count=0;
        }
        if(log->chunks[ci-1].cfg_new[j].ids){
          RAFT_FREE((void*)log->chunks[ci-1].cfg_new[j].ids);
          log->chunks[ci-1].cfg_new[j].ids=0;
          log->chunks[ci-1].cfg_new[j].id_count=0;
        }
        if(log->chunks[ci-1].cfg_learners&&log->chunks[ci-1].cfg_learners[j].ids){
          RAFT_FREE((void*)log->chunks[ci-1].cfg_learners[j].ids);
          log->chunks[ci-1].cfg_learners[j].ids=0;
          log->chunks[ci-1].cfg_learners[j].id_count=0;
        }
      }
    }
  }
  return 0;
}
/* Durability frontier invalidation.
   durable_confirm is only meaningful for the CURRENT log prefix: after a cut the
   entries past it are gone, and whatever is written later at those indexes is NOT
   covered by an earlier persist report.  Every site that cuts or replaces the
   prefix must lower the frontier to the highest index still valid, otherwise a
   follower could ACK an index that was never fsync'ed and the leader would count
   it towards a commit (State Machine Safety rests on match index meaning "durably
   written to this server's disk", dissertation 10.2.1). */
static void raft_durable_clamp(raft_ctx *r,raft_i64 highest_valid){
  if(r->durable_confirm>highest_valid) r->durable_confirm=highest_valid;
  /* A cut rewrites entries at indices the caller may already have written: those records
     are stale from that index on, so the delta must start there again. */
  if(r->persist_synced_index>highest_valid) r->persist_synced_index=highest_valid;
  /* A report already superseded by the cut must not block a later, fresher ACK:
     keep the reported high-water inside the surviving prefix as well.  Reporting
     a lower index is always safe (the leader only ever raises its match index). */
  if(r->deferred_append_last_index>highest_valid) r->deferred_append_last_index=highest_valid;
}
/* Log cut that also invalidates the frontier: one entry point, so no truncation
   site can forget the clamp. */
static int raft_log_cut(raft_ctx *r,raft_i64 idx){
  int rc=raft_log_truncate(&r->log,idx);
  if(rc==0) raft_durable_clamp(r,idx);
  return rc;
}
/* ---- peer helpers ---- */
static raft_peer *raft_find_peer(raft_ctx *r,int id){
  int i;
  for(i=0;i<r->peer_count;i++){
    if(r->peers[i].id==id) return &r->peers[i];
  }
  return 0;
}
static int raft_quorum_size_of(const raft_ctx *r,int old_config_only){
  int i,n=0,self_in;
  for(i=0;i<r->peer_count;i++){
    if(r->peers[i].is_learner) continue;
    if(old_config_only&&!r->peers[i].in_old_config) continue;
    if(!old_config_only&&!r->peers[i].in_new_config) continue;
    n++;
  }
  /* A removed member does not count toward quorum ($4.2.2): include self
     only while it is still a voting member of the config in question. */
  self_in=old_config_only?raft_mask_has(raft_set_view(&r->config_old),r->cfg.id):raft_mask_has(raft_set_view(&r->config_new),r->cfg.id);
  if(self_in) n++;
  return n/2+1;
}
static int raft_result_ensure(raft_ctx *r,int need);
static void raft_result_emit(raft_ctx *r,const void *cookie,int status){
  /* Defensive allocation on the emit path itself: every caller is supposed to
     pre-ensure room, and getting that protocol wrong silently ran off the end of
     result_buf (it corrupted results once).  A result that cannot be recorded is
     counted and dropped - never truncated over neighbouring memory. */
  if(r->result_count>=r->result_capacity&&raft_result_ensure(r,r->result_count+1)!=0){
    r->result_dropped++;
    return;
  }
  r->result_buf[r->result_count].cookie=cookie;
  r->result_buf[r->result_count].response=0;
  r->result_buf[r->result_count].response_size=0;
  r->result_buf[r->result_count].status=status;
  r->result_buf[r->result_count].leader_id=r->state==RAFT_LEADER?r->cfg.id:r->leader_id;
  r->result_count++;
}
static int raft_buf_ensure(void **buf,int *cap,int need,int elem_size,int init_cap){
  int nc;
  void *nb;
  if(*cap>=need) return 0;
  nc=*cap>0?*cap*2:init_cap;
  if(nc<need) nc=need;
  nb=RAFT_REALLOC(*buf,(unsigned int)nc*(unsigned int)elem_size);
  if(!nb) return -1;
  *buf=nb;
  *cap=nc;
  return 0;
}
static int raft_result_ensure(raft_ctx *r,int need){
  return raft_buf_ensure((void**)&r->result_buf,&r->result_capacity,need,sizeof(raft_client_result),8);
}
static int raft_msg_ensure(raft_ctx *r,int need){
  return raft_buf_ensure((void**)&r->msg_buf,&r->msg_capacity,need,sizeof(raft_peer_message),16);
}
static int raft_apply_ensure(raft_ctx *r,int need){
  return raft_buf_ensure((void**)&r->apply_buf,&r->apply_capacity,need,sizeof(raft_apply_entry),8);
}
static int raft_peer_health_ensure(raft_ctx *r,int need){
  return raft_buf_ensure((void**)&r->peer_health_buf,&r->peer_health_capacity,need,sizeof(raft_peer_health),8);
}
static int raft_persist_entry_ensure(raft_ctx *r,int need){
  return raft_buf_ensure((void**)&r->persist_entry_buf,&r->persist_entry_capacity,need,sizeof(raft_persist_entry),16);
}
static int raft_client_pending_ensure(raft_ctx *r,int need){
  return raft_buf_ensure((void**)&r->client_pending,&r->client_pending_capacity,need,sizeof(raft_client_pending),8);
}
/* Reclaim the retired prefix of client_pending.  Compaction is amortized: it only runs
   when the head has grown to the whole live set or past a small bound, so a stream of
   completions costs O(1) each with an occasional bulk move (instead of a move per
   completion). */
static void raft_client_pending_compact(raft_ctx *r){
  if(!r||r->client_pending_head<=0) return;
  if(r->client_pending_head<64&&r->client_pending_head<r->client_pending_count) return;
  if(r->client_pending_count>0){
    memmove(r->client_pending,&r->client_pending[r->client_pending_head],
            (unsigned int)r->client_pending_count*sizeof(raft_client_pending));
  }
  r->client_pending_head=0;
}
static int raft_read_barrier_ensure(raft_ctx *r,int need){
  return raft_buf_ensure((void**)&r->read_barriers,&r->read_barrier_capacity,need,sizeof(raft_read_barrier),8);
}
static int raft_snap_read_ensure(raft_ctx *r,int need){
  return raft_buf_ensure((void**)&r->snap_read_buf,&r->snap_read_capacity,need,sizeof(raft_snapshot_read_req),4);
}
static int raft_read_index_pending_ensure(raft_ctx *r,int need){
  return raft_buf_ensure((void**)&r->read_index_pending,&r->read_index_pending_capacity,need,sizeof(raft_read_index_pending),8);
}
static int raft_follower_read_ensure(raft_ctx *r,int need){
  return raft_buf_ensure((void**)&r->follower_read_pending,&r->follower_read_pending_capacity,need,sizeof(void*),8);
}
static void raft_config_pending_only_clear(raft_ctx *r){
  raft_set_clear(&r->config_pending_old);
  raft_set_clear(&r->config_pending_new);
  raft_set_clear(&r->config_pending_learners);
  r->config_pending=0;
  r->config_index=0;
}
static void raft_config_pending_clear(raft_ctx *r){
  raft_config_pending_only_clear(r);
  /* also abort any deferred reconfig catch-up */
  raft_set_clear(&r->config_catchup_new);
  r->config_catchup_cookie=0;
  r->config_catchup_active=0;
  r->config_catchup_round=0;
  r->config_catchup_start_index=0;
  r->config_catchup_elapsed=0;
  r->config_catchup_total_elapsed=0;
}
/* Retire a "zombie" pending config: the entry at config_index has ALREADY
   committed (commit_index >= config_index), so the change is complete ($4.1
   "complete once C_new committed") but config_pending was never retired.  The
   normal retire site (raft_apply_complete's commit-time scan) only re-visits
   config entries NEWLY applied since config_applied_index, so a config entry
   committed/finalized in an earlier interval leaves a zombie that would reject
   every later membership change forever -- the $4.2.1 "make no more progress"
   trap.  Clear ONLY the pending tracking (a possibly-independent deferred
   catch-up must survive, mirroring the seed-1830 exemption elsewhere). */
static void raft_config_zombie_clear(raft_ctx *r){
  if(r->config_pending&&r->config_index>0&&r->commit_index>=r->config_index) raft_config_pending_only_clear(r);
}
/* Flush the three pending-client lists (read barriers, pending client
   commands, follower ReadIndex barriers) with one terminal status, then clear
   them.  Used by raft_step_down (REDIRECT) and the shutdown flush (FAILED). */
static void raft_flush_client_results(raft_ctx *r,int status){
  int i;
  if(raft_result_ensure(r,r->result_count+r->read_barrier_count+r->client_pending_count+r->follower_read_pending_count)!=0){
    /* Cannot report them RIGHT NOW: keep every pending entry and try again on the next
       step-down / shutdown instead of dropping them.  Clearing the lists here (as this
       used to do, unconditionally, right after a guarded emit) discards the outcome of
       requests that are still waiting: the client waits forever and the server never
       answers, i.e. state leaves raft.h without ever being reported through ready.  A
       leader barrier kept across a step-down is still resolved - it is failed by the
       stalled-barrier bound below, which reports each cookie individually. */
    return;
  }
  for(i=0;i<r->read_barrier_count;i++) raft_result_emit(r,r->read_barriers[i].cookie,status);
  for(i=0;i<r->client_pending_count;i++) raft_result_emit(r,r->client_pending[r->client_pending_head+i].cookie,status);
  for(i=0;i<r->follower_read_pending_count;i++) raft_result_emit(r,r->follower_read_pending[i],status);
  r->read_barrier_count=0;
  r->client_pending_count=0;
  r->client_pending_head=0;
  r->follower_read_pending_count=0;
  r->follower_read_frozen=0;
  r->follower_read_waiting=0;
  r->follower_read_context=0;
}
static void raft_step_down(raft_ctx *r,raft_i64 term,int new_leader_id){
  int i;
  const void *tc,*cc;
  if(term>=RAFT_TERM_MAX+1) return; /* cap: INT64_MAX-1 */
  r->current_term=term;
  r->persist_needed=1; /* Sec. 3.8: a term change (and the voted_for clear below) must be persisted */
  r->term_dirty_gen=r->persist_gen+1;  /* the generation that will make this term durable */
  r->state=RAFT_FOLLOWER;
  r->voted_for=0;
  r->leader_id=new_leader_id;
  r->ready_leader_change=1;
  r->in_pre_vote=0;
  r->transfer_target=0;
  r->transfer_elapsed=0;
  /* save transfer_cookie for REDIRECT delivery below; clear now so it won't dangle */
  tc=r->transfer_cookie;
  r->transfer_cookie=0;
  /* capture the deferred-reconfig cookie before config_pending_clear drops it,
     so the caller still receives a terminal REDIRECT on step-down (guarded by
     config_catchup_active: after successful catch-up the cookie is already in
     client_pending and must not be notified twice). */
  cc=r->config_catchup_active?r->config_catchup_cookie:0;
  r->leader_contact_elapsed=0;
  r->leadership_confirmed=0;
  raft_config_pending_clear(r);
  r->deferred_vote_response=0;
  r->deferred_vote_request=0;
  r->deferred_append_response=0;
  r->snapshot_pending_dirty=0;
  r->snapshot_data_ready_flag=0;
  r->snapshot_install_pending=0;
  /* Abort any in-flight snapshot receive: a new leader restarts the stream
     from offset 0, so the previous expected offset must not reject it ($7.4). */
  r->snapshot_recv_expected_offset=0;
  r->snapshot_recv.last_index=0;
  r->follower_read_elapsed=0;
  r->read_index_pending_count=0;
  r->read_index_heartbeat_sent=0;
  r->election_elapsed=0;
  r->joint_elapsed=0;
  for(i=0;i<r->peer_count;i++){
    r->peers[i].vote=0;
    r->peers[i].quorum_acked=0;
    r->peers[i].read_index_acked=0;
  }
  /* notify pending clients, barriers, and follower ReadIndex requests with
     redirect so callers are not left hanging (DRAINING uses FAILED instead) */
  raft_flush_client_results(r,RAFT_CLIENT_REDIRECT);
  r->read_barrier_gen=0;
  /* notify transfer/reconfig initiators with redirect on step-down */
  if(raft_result_ensure(r,r->result_count+(tc?1:0)+(cc?1:0))==0){
    if(tc) raft_result_emit(r,tc,RAFT_CLIENT_REDIRECT);
    if(cc) raft_result_emit(r,cc,RAFT_CLIENT_REDIRECT);
  }
}
/* ---- election ---- */
/* xorshift32: deterministic (seedable) with far better statistical uniformity
   than an LCG, so the Sec. 9.2 uniform-random timeout assumption actually holds;
   the node id is mixed in so equal-seed nodes draw distinct deadlines. */
static unsigned int raft_rng_step(raft_ctx *r){
  unsigned int x=r->rng;
  if(x==0u) x=2463534242u; /* xorshift32 fixed point at 0: re-seed */
  x^=x<<13u;
  x^=x>>17u;
  x^=x<<5u;
  r->rng=x;
  return x;
}
static void raft_reset_election_deadline(raft_ctx *r){
  unsigned int range,x;
  if(r->cfg.election_max_ms<=r->cfg.election_min_ms) range=0;
  else range=r->cfg.election_max_ms-r->cfg.election_min_ms;
  x=raft_rng_step(r);
  x^=(unsigned int)r->cfg.id*2654435761u;
  /* range+1 can wrap to 0 when range==UINT_MAX (min==0, max==UINT_MAX), making
     x%0 undefined; promote the modulus to 64-bit so it never wraps. */
  r->election_deadline=r->cfg.election_min_ms+(unsigned int)(x%((raft_u64)range+1u));
}
static int raft_broadcast_vote(raft_ctx *r,int pre_vote){
  int i,saved_msg;
  raft_i64 li;
  raft_peer_message *m;
  saved_msg=r->msg_count;
  for(i=0;i<r->peer_count;i++){
    if(r->peers[i].is_learner) continue;
    if(raft_msg_ensure(r,r->msg_count+1)<0){
      r->msg_count=saved_msg;
      return -1;
    }
    m=&r->msg_buf[r->msg_count];
    m->type=RAFT_MSG_REQUEST_VOTE;
    m->from=r->cfg.id;
    m->to=r->peers[i].id;
    m->term=r->current_term;
    m->request_vote.term=r->current_term;
    li=raft_log_last_index(&r->log);
    m->request_vote.last_log_index=li;
    m->request_vote.last_log_term=raft_log_term_at(&r->log,li);
    m->request_vote.candidate_id=r->cfg.id;
    m->request_vote.pre_vote=pre_vote;
    r->msg_count++;
  }
  return 0;
}
/* true if any peer is a VOTER (not a learner).  A node whose peers are all
   learners (e.g. a single-voter config plus catch-up learners) has no one to
   ask for votes, so it must self-elect exactly like a 1-node cluster. */
static int raft_has_voter_peer(const raft_ctx *r){
  int i;
  for(i=0;i<r->peer_count;i++) if(!r->peers[i].is_learner) return 1;
  return 0;
}
static int raft_become_candidate(raft_ctx *r){
  int i;
  /* Sec. 4.2.1: a non-voting member (learner) must never become a leader - it does not count
     itself in any quorum, so campaigning can only steal an election window from a real voter.
     The transfer path already refuses to hand leadership to a learner; this closes the same
     hole on the election path. */
  if(raft_is_learner(&r->config_learners,r->cfg.id)) return -1;
  if(r->current_term>=RAFT_TERM_MAX) return -1; /* cap: term++ would reach INT64_MAX (invalid); stay follower */
  r->state=RAFT_CANDIDATE;
  r->current_term++;
  r->voted_for=r->cfg.id;
  /* Cancel a pending deferred vote response: it recorded a vote for a
     DIFFERENT candidate at the OLD term.  If left set, the response would be
     sent later with r->current_term (now the NEW term), turning a vote
     granted at term T into a term T+1 vote - two leaders in one term (seed
     2226).  Symmetric with raft_step_down, which also clears it. */
  r->deferred_vote_response=0;
  r->deferred_vote_to=0;
  r->persist_needed=1; /* Sec. 3.8: persist the term bump + self-vote BEFORE requesting votes */
  r->term_dirty_gen=r->persist_gen+1;
  r->leader_id=0;
  r->ready_leader_change=1;
  r->in_pre_vote=0;
  r->election_elapsed=0;
  raft_reset_election_deadline(r);
  for(i=0;i<r->peer_count;i++){
    r->peers[i].vote=0;
    r->peers[i].pre_vote_acked=0;
  }
  if(!raft_has_voter_peer(r)){
    if(raft_log_append(&r->log,r->current_term,RAFT_ENTRY_NOOP,0,0,raft_mask_zero(),raft_mask_zero(),raft_mask_zero())<0){
      /* rollback: revert term and state on allocation failure */
      r->current_term--;
      r->voted_for=0;
      r->state=RAFT_FOLLOWER;
      r->ready_leader_change=1;
      return -1;
    }
    r->state=RAFT_LEADER;
    r->leader_id=r->cfg.id;
    r->heartbeat_elapsed=0;
    r->leader_contact_elapsed=0; /* leader is always in its own Sec. 4.2.3 grace period */
    r->persist_needed=1;
    for(i=0;i<r->peer_count;i++){
      r->peers[i].next_index=raft_log_last_index(&r->log)+1;
      r->peers[i].match_index=0;
      r->peers[i].missed_rounds=0;
    }
    return 0;
  }
  /* Sec. 3.8: defer the REAL RequestVote broadcast until the term bump + self-vote
     are persisted (persist_gen reaches deferred_vote_request_gen).  Symmetric
     with the voter-side deferred_vote_response gate: a candidate must not send
     RequestVote RPCs carrying a term whose self-vote is not yet durable, or a
     crash could let it cast a second vote in the same term after restart
     (one-vote-per-term, Sec. 3.4). */
  r->deferred_vote_request=1;
  r->deferred_vote_request_gen=r->persist_gen+1;
  return 0;
}
static int raft_count_votes(const raft_ctx *r,int old_config_only,int pre_vote){
  int i,v=0;
  for(i=0;i<r->peer_count;i++){
    if(r->peers[i].is_learner) continue;
    if(old_config_only){
      if(!r->peers[i].in_old_config) continue;
    }else{
      if(!r->peers[i].in_new_config) continue;
    }
    if(pre_vote){
      if(r->peers[i].pre_vote_acked) v++;
    }else{
      if(r->peers[i].vote) v++;
    }
  }
  return v;
}
static int raft_start_pre_vote(raft_ctx *r){
  int i;
  r->in_pre_vote=1;
  r->election_elapsed=0;
  raft_reset_election_deadline(r);
  r->deferred_vote_request=0; /* a new pre-vote round supersedes any pending real-vote broadcast */
  for(i=0;i<r->peer_count;i++) r->peers[i].pre_vote_acked=0;
  if(!raft_has_voter_peer(r)){
    /* single-node (self in config): self-elect exactly like a 1-node cluster.
       EMPTY config (bootstrap new node, Sec 4.4): no quorum of its own - stay a
       follower and never campaign, so its term stays put until the cluster
       leader's AppendEntries arrives. */
    if(raft_mask_has(raft_set_view(&r->config_old),r->cfg.id)) return raft_become_candidate(r);
    return 0;
  }
  /* OOM: clear in_pre_vote and return success; election_elapsed/deadline were
     already reset above, so the pre-vote is simply retried on the next deadline. */
  if(raft_broadcast_vote(r,1)<0) r->in_pre_vote=0;
  return 0;
}
/* ---- peer rebuild (joint consensus aware) ---- */
static int raft_find_peer_in(const raft_peer *peers,int count,int id){
  int i;
  for(i=0;i<count;i++){
    if(peers[i].id==id) return i;
  }
  return -1;
}
/* carry replication progress (and learner catch-up state) from an old peer
   entry with the same id into a freshly rebuilt peer slot. require_learner=1
   only matches old learner entries (used for learner-only additions). */
static void raft_peer_carry(raft_peer *dst,const raft_peer *old,int old_count,int pid,int require_learner){
  int j;
  for(j=0;j<old_count;j++){
    if(old[j].id==pid&&(!require_learner||old[j].is_learner)){
      dst->next_index=old[j].next_index;
      dst->match_index=old[j].match_index;
      dst->pending_snapshot_offset=old[j].pending_snapshot_offset;
      dst->pending_snapshot_last_index=old[j].pending_snapshot_last_index;
      if(dst->is_learner){
        dst->catchup_round=old[j].catchup_round;
        dst->catchup_round_start_index=old[j].catchup_round_start_index;
        dst->catchup_round_elapsed=old[j].catchup_round_elapsed;
        dst->catchup_cookie=old[j].catchup_cookie;
      }
      break;
    }
  }
}
static int raft_rebuild_peers_normal(raft_ctx *r){
  int i,pi,pid,self_id,new_count,old_count,learn_count;
  raft_peer *np;
  old_count=r->peer_count;
  new_count=r->config_new.id_count;
  learn_count=r->config_learners.id_count;
  self_id=r->cfg.id;
  np=(raft_peer *)RAFT_CALLOC((unsigned int)(new_count+learn_count),sizeof(raft_peer));
  if(!np) return -1;
  pi=0;
  for(i=0;i<new_count;i++){
    pid=r->config_new.ids[i];
    if(pid==self_id) continue;
    np[pi].id=pid;
    np[pi].in_old_config=1;
    np[pi].in_new_config=1;
    np[pi].is_learner=0; /* a peer in a voter config is never a learner ($4.2.1) */
    np[pi].next_index=raft_log_last_index(&r->log)+1;
    raft_peer_carry(&np[pi],r->peers,old_count,pid,0);
    pi++;
  }
  /* add learner-only peers (not already voters) */
  for(i=0;i<learn_count;i++){
    pid=r->config_learners.ids[i];
    if(pid==self_id) continue;
    if(raft_find_peer_in(np,pi,pid)>=0) continue;
    np[pi].id=pid;
    np[pi].is_learner=1;
    np[pi].in_old_config=0;
    np[pi].in_new_config=0;
    np[pi].next_index=raft_log_last_index(&r->log)+1;
    raft_peer_carry(&np[pi],r->peers,old_count,pid,1);
    pi++;
  }
  if(r->peers) RAFT_FREE(r->peers);
  r->peers=np;
  r->peer_count=pi;
  return 0;
}
static int raft_rebuild_peers_joint(raft_ctx *r){
  int i,j,pi,pid,self_id,old_count,total,learn_count;
  raft_peer *np;
  old_count=r->peer_count;
  self_id=r->cfg.id;
  learn_count=r->config_learners.id_count;
  total=r->config_old.id_count+r->config_new.id_count;
  np=(raft_peer *)RAFT_CALLOC((unsigned int)(total+learn_count),sizeof(raft_peer));
  if(!np) return -1;
  pi=0;
  /* add all peers in C_old (mark in_old_config=1) */
  for(i=0;i<r->config_old.id_count;i++){
    pid=r->config_old.ids[i];
    if(pid==self_id) continue;
    j=raft_find_peer_in(np,pi,pid);
    if(j>=0){
      np[j].in_old_config=1;
      continue;
    }
    np[pi].id=pid;
    np[pi].in_old_config=1;
    np[pi].in_new_config=0;
    np[pi].is_learner=0; /* a peer in a voter config is never a learner ($4.2.1) */
    np[pi].next_index=raft_log_last_index(&r->log)+1;
    raft_peer_carry(&np[pi],r->peers,old_count,pid,0);
    pi++;
  }
  /* add all peers in C_new (mark in_new_config=1) */
  for(i=0;i<r->config_new.id_count;i++){
    pid=r->config_new.ids[i];
    if(pid==self_id) continue;
    j=raft_find_peer_in(np,pi,pid);
    if(j>=0){
      np[j].in_new_config=1;
      continue;
    }
    np[pi].id=pid;
    np[pi].in_old_config=0;
    np[pi].in_new_config=1;
    np[pi].is_learner=0; /* a peer in a voter config is never a learner ($4.2.1) */
    np[pi].next_index=raft_log_last_index(&r->log)+1;
    raft_peer_carry(&np[pi],r->peers,old_count,pid,0);
    pi++;
  }
  /* add learner-only peers (not already in C_old or C_new) */
  for(i=0;i<learn_count;i++){
    pid=r->config_learners.ids[i];
    if(pid==self_id) continue;
    if(raft_find_peer_in(np,pi,pid)>=0) continue;
    np[pi].id=pid;
    np[pi].is_learner=1;
    np[pi].in_old_config=0;
    np[pi].in_new_config=0;
    np[pi].next_index=raft_log_last_index(&r->log)+1;
    raft_peer_carry(&np[pi],r->peers,old_count,pid,1);
    pi++;
  }
  if(r->peers) RAFT_FREE(r->peers);
  r->peers=np;
  r->peer_count=pi;
  return 0;
}
static raft_i64 raft_find_existing_final_config(const raft_ctx *r,raft_i64 after_idx){
  /* scan log entries after after_idx for a CONFIG entry whose cfg_old==cfg_new.
     returns the index if found, 0 otherwise. */
  raft_i64 idx,last,off;
  int ci,co,kind;
  raft_mask cm_old,cm_new;
  last=raft_log_last_index(&r->log);
  for(idx=after_idx+1;idx<=last;idx++){
    /* Entries at or before last_included_index live in the compacted snapshot
       prefix, not in the live chunk arrays: skipping them keeps the chunk
       offset non-negative (a caller passing after_idx=0 on a compacted log
       would otherwise compute off = 1-last_included_index-1 < 0 and index
       chunks[-1]). */
    if(idx<=r->log.last_included_index) continue;
    off=idx-r->log.last_included_index-1;
    ci=(int)(off>>r->log.chunk_bits);
    co=(int)(off&r->log.chunk_mask);
    if(ci>=r->log.num_chunks) break;
    kind=(int)r->log.chunks[ci].kinds[co];
    if(kind!=RAFT_ENTRY_CONFIG) continue;
    if(!r->log.chunks[ci].cfg_old) continue;
    cm_old=r->log.chunks[ci].cfg_old[co];
    cm_new=r->log.chunks[ci].cfg_new[co];
    if(raft_mask_eq(cm_old,cm_new)) return idx;
  }
  return RAFT_I64_C(0);
}
/* ---- apply latest config from log (config-immediately, $4.1) ---- */
/* Apply a single configuration (old/new/learners) to the live config with
   OOM-safe rollback.  Shared by raft_apply_config_from_log for both the
   "latest config entry" and "revert to base config" cases. */
/* assign r->config_learners to cm_learners with any id already in the voter
   mask cm_voters removed.  A peer in the voter configuration is never a
   learner ($4.2.1): raft_add_learner already rejects voters, so promotion via
   raft_reconfig must drop the promoted id from the learner set here. */
static int raft_assign_learners_filtered(raft_ctx *r,raft_mask cm_learners,raft_mask cm_voters){
  int n=0,i,k=0;
  int *ids;
  for(i=0;i<cm_learners.id_count;i++){
    if(!raft_mask_has(cm_voters,cm_learners.ids[i])) n++;
  }
  if(n<=0){
    raft_set_clear(&r->config_learners);
    return 0;
  }
  ids=(int*)RAFT_MALLOC(sizeof(int)*(unsigned int)n);
  if(!ids) return -1;
  for(i=0;i<cm_learners.id_count;i++){
    if(!raft_mask_has(cm_voters,cm_learners.ids[i])) ids[k++]=cm_learners.ids[i];
  }
  raft_set_clear(&r->config_learners);
  r->config_learners.ids=ids;
  r->config_learners.id_count=n;
  return 0;
}
static int raft_apply_config_masks(raft_ctx *r,raft_mask cm_old,raft_mask cm_new,raft_mask cm_learners){
  int rc=0;
  /* save state for OOM rollback */
  raft_set saved_old,saved_new,saved_learners;
  int saved_joint;
  saved_old.ids=0;
  saved_old.id_count=0;
  saved_new.ids=0;
  saved_new.id_count=0;
  saved_learners.ids=0;
  saved_learners.id_count=0;
  if(raft_set_copy(&saved_old,&r->config_old)<0||raft_set_copy(&saved_new,&r->config_new)<0||raft_set_copy(&saved_learners,&r->config_learners)<0){
    /* OOM: abort mutation, keep current config intact.  Mark the live config
       stale so it is re-applied later (leader-completeness safety: a node must
       never campaign under a config older than its log's latest config entry). */
    r->config_apply_stale=1;
    raft_set_clear(&saved_old);
    raft_set_clear(&saved_new);
    raft_set_clear(&saved_learners);
    return -1;
  }
  saved_joint=r->config_joint;
  if(!raft_mask_eq(cm_old,cm_new)){
    if(raft_set_assign(&r->config_old,cm_old.ids,cm_old.id_count)<0) rc=-1;
    if(rc==0&&raft_set_assign(&r->config_new,cm_new.ids,cm_new.id_count)<0) rc=-1;
    /* apply learner config BEFORE peer rebuild so rebuild is the last mutation;
       if rebuild succeeds and a later step fails, peers were never touched */
    if(rc==0) rc=raft_assign_learners_filtered(r,cm_learners,cm_new);
    if(rc==0){
      r->config_joint=1;
      rc=raft_rebuild_peers_joint(r);
    }
  }else{
    if(raft_set_assign(&r->config_old,cm_new.ids,cm_new.id_count)<0) rc=-1;
    if(rc==0&&raft_set_assign(&r->config_new,cm_new.ids,cm_new.id_count)<0) rc=-1;
    if(rc==0) rc=raft_assign_learners_filtered(r,cm_learners,cm_new);
    if(rc==0){
      r->config_joint=0;
      rc=raft_rebuild_peers_normal(r);
    }
  }
  if(rc<0){
    /* OOM: restore saved config; peer list was never modified */
    r->config_apply_stale=1;
    raft_set_assign(&r->config_old,saved_old.ids,saved_old.id_count);
    raft_set_assign(&r->config_new,saved_new.ids,saved_new.id_count);
    raft_set_assign(&r->config_learners,saved_learners.ids,saved_learners.id_count);
    r->config_joint=saved_joint;
    raft_set_clear(&saved_old);
    raft_set_clear(&saved_new);
    raft_set_clear(&saved_learners);
    return -1;
  }
  raft_set_clear(&saved_old);
  raft_set_clear(&saved_new);
  raft_set_clear(&saved_learners);
  r->config_apply_stale=0;
  return 0;
}
/* ---- apply latest config from log (config-immediately, $4.1) ---- */
static int raft_apply_config_from_log(raft_ctx *r){
  raft_i64 idx,last,off;
  int ci,co,kind;
  raft_mask cm_old,cm_new,cm_learners,b_old,b_new,b_learn;
  last=raft_log_last_index(&r->log);
  for(idx=last;idx>r->log.last_included_index;idx--){
    off=idx-r->log.last_included_index-1;
    ci=(int)(off>>r->log.chunk_bits);
    co=(int)(off&r->log.chunk_mask);
    if(ci>=r->log.num_chunks) continue;
    kind=(int)r->log.chunks[ci].kinds[co];
    if(kind!=RAFT_ENTRY_CONFIG) continue;
    if(!r->log.chunks[ci].cfg_old) continue;
    cm_old=r->log.chunks[ci].cfg_old[co];
    cm_new=r->log.chunks[ci].cfg_new[co];
    cm_learners=raft_mask_zero();
    if(r->log.chunks[ci].cfg_learners) cm_learners=r->log.chunks[ci].cfg_learners[co];
    return raft_apply_config_masks(r,cm_old,cm_new,cm_learners);
  }
  /* No config entry remains in the log (e.g. an uncommitted config entry was
     just truncated).  Revert the live config to the base configuration at
     last_included_index so a stale config_old/config_new/config_joint cannot
     survive and be wrongly finalized later ($4.1 config-immediate). */
  if(r->log.last_included_index>0&&raft_mask_any(raft_set_view(&r->snapshot.cfg_old))){
    b_old=raft_set_view(&r->snapshot.cfg_old);
    b_new=raft_set_view(&r->snapshot.cfg_new);
    b_learn=raft_set_view(&r->snapshot.cfg_learners);
  }else{
    b_old=raft_set_view(&r->config_bootstrap);
    b_new=b_old;
    b_learn=raft_mask_zero();
  }
  /* Skip the allocating rebuild when the config already equals the base:
     avoids churn on every AppendEntries carrying no config entry.  Do NOT
     skip while config_apply_stale is set: a stale flag means the live config
     and the PEER LIST may be inconsistent (e.g. a snapshot-install peer
     rebuild hit an OOM and left old peers under the new config), so the
     rebuild must be re-run even though the config masks already match. */
  if(!r->config_apply_stale&&raft_mask_eq(raft_set_view(&r->config_old),b_old)&&raft_mask_eq(raft_set_view(&r->config_new),b_new)&&raft_mask_eq(raft_set_view(&r->config_learners),b_learn)&&r->config_joint==!raft_mask_eq(b_old,b_new)){
    r->config_apply_stale=0;
    return 0;
  }
  return raft_apply_config_masks(r,b_old,b_new,b_learn);
}
/* Sec. 4.3: append the final C_new entry for a committed joint entry and repoint
   the reconfig cookie at it.  Idempotent: if a C_new entry already exists
   after joint_idx (appended earlier by this or a previous leader), just track
   it. */
static int raft_finalize_joint(raft_ctx *r,raft_i64 joint_idx,raft_mask cm_new){
  int i;
  raft_i64 existing=raft_find_existing_final_config(r,joint_idx);
  if(existing>0){
    r->config_pending=1;
    r->config_index=existing;
    return raft_apply_config_from_log(r);
  }
  r->config_pending=1;
  r->config_index=raft_log_last_index(&r->log)+1;
  if(raft_log_append(&r->log,r->current_term,RAFT_ENTRY_CONFIG,0,0,cm_new,cm_new,raft_set_view(&r->config_learners))<0){
    r->config_pending=0;
    r->config_index=0;
    return -1;
  }
  if(raft_apply_config_from_log(r)<0){
    raft_log_cut(r,r->config_index-1);
    r->config_pending=0;
    r->config_index=0;
    return -1;
  }
  r->persist_needed=1;
  /* repoint the reconfig cookie from the joint entry to the final entry */
  for(i=0;i<r->client_pending_count;i++){
    if(r->client_pending[r->client_pending_head+i].log_index==joint_idx){
      r->client_pending[r->client_pending_head+i].log_index=r->config_index;
      break;
    }
  }
  return 0;
}
/* Sec. 4.3: once the joint entry is committed (commit_index >= config_index), the
   leader immediately appends C_new rather than waiting for the joint to be
   APPLIED.  No-op when not leader / no joint / not yet committed. */
static int raft_maybe_finalize_joint(raft_ctx *r){
  raft_i64 off;
  raft_mask cm_old,cm_new;
  int ci,co;
  if(r->state!=RAFT_LEADER) return 0;
  if(!r->config_joint||!r->config_pending||r->config_index<=0) return 0;
  if(r->commit_index<r->config_index) return 0;
  off=r->config_index-r->log.last_included_index-1;
  ci=(int)(off>>r->log.chunk_bits);
  co=(int)(off&r->log.chunk_mask);
  if(ci>=r->log.num_chunks||!r->log.chunks[ci].cfg_old) return 0;
  if(r->log.chunks[ci].kinds[co]!=RAFT_ENTRY_CONFIG) return 0;
  cm_old=r->log.chunks[ci].cfg_old[co];
  cm_new=r->log.chunks[ci].cfg_new[co];
  if(raft_mask_eq(cm_old,cm_new)) return 0; /* not actually a joint entry */
  return raft_finalize_joint(r,r->config_index,cm_new);
}
/* ---- transition to leader (shared by election paths) ---- */
static int raft_become_leader(raft_ctx *r){
  int i;
  raft_i64 li;
  if(raft_log_append(&r->log,r->current_term,RAFT_ENTRY_NOOP,0,0,raft_mask_zero(),raft_mask_zero(),raft_mask_zero())<0){
    /* rollback: revert to follower (term stays - already sent RequestVote).
       Also clear the vote tally: a stale vote result delivered later must not
       re-elect this reverted follower on votes cast in the aborted candidacy
       (seed 8063: a follower re-elected after stepping down and granting its
       vote to another candidate -> two leaders in one term). */
    r->state=RAFT_FOLLOWER;
    r->ready_leader_change=1;
    r->voted_for=0;
    r->in_pre_vote=0;
    for(i=0;i<r->peer_count;i++){
      r->peers[i].vote=0;
      r->peers[i].pre_vote_acked=0;
    }
    return -1;
  }
  r->state=RAFT_LEADER;
  r->leader_id=r->cfg.id;
  r->ready_leader_change=1;
  r->heartbeat_elapsed=0;
  r->election_elapsed=0;
  r->leader_contact_elapsed=0; /* leader is always in its own Sec. 4.2.3 grace period */
  r->leadership_confirmed=0;
  /* A candidate that re-entered pre-vote (its election timer fired again while
     awaiting real votes, $9.6) has in_pre_vote=1 when it WINS the real
     election here.  If not cleared, a late pre-vote result would pass the
     raft_recv_request_vote_result guard and make this LEADER become a
     candidate again - leaking its leader-queued read barriers into non-leader
     state (stale READY, seed 4138).  A leader never pre-votes, so the flag
     must be 0. */
  r->in_pre_vote=0;
  r->persist_needed=1;
  for(i=0;i<r->peer_count;i++){
    r->peers[i].next_index=raft_log_last_index(&r->log)+1;
    r->peers[i].match_index=0;
    r->peers[i].missed_rounds=0;
    r->peers[i].pending_snapshot_offset=0;
    r->peers[i].pending_snapshot_last_index=0;
    r->peers[i].catchup_round=0;
    r->peers[i].catchup_round_elapsed=0;
  }
  /* If a joint config (C_old,new) was committed but not yet finalized by
     a previous leader, the new leader must append C_new immediately ($4.3).
     Without this, a node that already applied the joint entry before
     becoming leader would never emit the final C_new entry. */
  li=raft_log_last_index(&r->log);
  if(r->config_joint){
    raft_i64 idx,last_joint=0;
    int ci,co,kind;
    for(idx=li;idx>r->log.last_included_index;idx--){
      raft_i64 off=idx-r->log.last_included_index-1;
      ci=(int)(off>>r->log.chunk_bits);
      co=(int)(off&r->log.chunk_mask);
      if(ci>=r->log.num_chunks||!r->log.chunks[ci].cfg_old) continue;
      kind=(int)r->log.chunks[ci].kinds[co];
      if(kind==RAFT_ENTRY_CONFIG){
        raft_mask cm_old=r->log.chunks[ci].cfg_old[co];
        raft_mask cm_new=r->log.chunks[ci].cfg_new[co];
        if(!raft_mask_eq(cm_old,cm_new)){
          last_joint=idx;
          break;
        }
      }
    }
    if(last_joint<=0&&r->log.last_included_index>0&&!raft_mask_eq(raft_set_view(&r->snapshot.cfg_old),raft_set_view(&r->snapshot.cfg_new))){
      /* joint from snapshot (the snapshot itself holds a joint config): already
         committed, safe to finalize if no C_new exists.  A live joint config with
         no joint entry in the log and a NON-joint snapshot (or no snapshot) is
         stale (from a truncated uncommitted entry) and must NOT be finalized. */
      if(raft_find_existing_final_config(r,0)==0){
        /* No C_new entry: append one now */
        r->config_pending=1;
        r->config_index=raft_log_last_index(&r->log)+1;
        if(raft_log_append(&r->log,r->current_term,RAFT_ENTRY_CONFIG,0,0,raft_set_view(&r->config_new),raft_set_view(&r->config_new),raft_set_view(&r->config_learners))<0){
          /* OOM: don't leave a phantom config_pending/config_index with no log entry */
          r->config_pending=0;
          r->config_index=0;
        }else if(raft_apply_config_from_log(r)<0){
          raft_log_cut(r,r->config_index-1);
          r->config_pending=0;
          r->config_index=0;
        }else{
          r->persist_needed=1;
        }
      }
    }else if(last_joint>0&&r->commit_index>=last_joint&&raft_find_existing_final_config(r,last_joint)==0){
      /* joint committed in log, no C_new entry after it: finalize.
         Derive C_new from the C_new field of the joint log entry ($4.3). */
      raft_i64 joff=last_joint-r->log.last_included_index-1;
      int jci=(int)(joff>>r->log.chunk_bits);
      int jco=(int)(joff&r->log.chunk_mask);
      raft_mask cm_new_target=r->log.chunks[jci].cfg_new[jco];
      r->config_pending=1;
      r->config_index=raft_log_last_index(&r->log)+1;
      if(raft_log_append(&r->log,r->current_term,RAFT_ENTRY_CONFIG,0,0,cm_new_target,cm_new_target,raft_set_view(&r->config_learners))<0){
        /* OOM: don't leave a phantom config_pending/config_index with no log entry */
        r->config_pending=0;
        r->config_index=0;
      }else if(raft_apply_config_from_log(r)<0){
        raft_log_cut(r,r->config_index-1);
        r->config_pending=0;
        r->config_index=0;
      }else{
        r->persist_needed=1;
      }
    }else if(last_joint>0&&r->commit_index<last_joint&&!r->config_pending){
      /* An UNCOMMITTED joint entry survives in the log (a previous leader aborted
         a replicated stuck joint by truncating its own copy and stepping down, or
         crashed before committing it).  Re-arm the joint timeout so THIS leader
         can also abort safely instead of sitting on an uncommittable joint with
         config_pending cleared. */
      r->config_pending=1;
      r->config_index=last_joint;
      r->joint_elapsed=0;
    }
  }
  return 0;
}
/* ---- commit advancement ---- */
static int raft_count_match(const raft_ctx *r,raft_i64 idx,int old_config_only){
  int i,c=0;
  for(i=0;i<r->peer_count;i++){
    if(r->peers[i].is_learner) continue;
    if(old_config_only&&!r->peers[i].in_old_config) continue;
    if(!old_config_only&&!r->peers[i].in_new_config) continue;
    if(r->peers[i].match_index>=idx) c++;
  }
  return c;
}
static void raft_advance_commit(raft_ctx *r){
  raft_i64 idx,last,et;
  int ok_old,ok_new,si_old,si_new,require_durable;
  last=raft_log_last_index(&r->log);
  /* $10.2.1: the leader's own entry counts toward the commit majority only
     once it is durable (durable_index>=idx).  A node with no voter peers
     (single-node, or a sole voter plus learners) is its own whole quorum, so
     it self-commits without waiting for durability; there durability is
     entirely the caller's responsibility via raft_persist_complete. */
  require_durable=raft_has_voter_peer(r);
  for(idx=r->commit_index+1;idx<=last;idx++){
    et=raft_log_term_at(&r->log,idx);
    if(et!=r->current_term) continue;
    si_old=raft_mask_has(raft_set_view(&r->config_old),r->cfg.id);
    si_new=raft_mask_has(raft_set_view(&r->config_new),r->cfg.id);
    if(require_durable){
      if(si_old&&r->durable_index<idx) si_old=0;
      if(si_new&&r->durable_index<idx) si_new=0;
    }
    if(r->config_joint){
      ok_old=(si_old+raft_count_match(r,idx,1))>=raft_quorum_size_of(r,1);
      ok_new=(si_new+raft_count_match(r,idx,0))>=raft_quorum_size_of(r,0);
      if(ok_old&&ok_new){
        r->commit_index=idx;
        r->leadership_confirmed=1;
      }else break;
    }else{
      if(si_new+raft_count_match(r,idx,0)>=raft_quorum_size_of(r,0)){
        r->commit_index=idx;
        r->leadership_confirmed=1;
      }else break;
    }
  }
}
static int raft_commit_config_entry(raft_ctx *r,const void *cookie){
  int i,pid;
  raft_peer *lp;
  raft_client_pending_compact(r);
  if(raft_client_pending_ensure(r,r->client_pending_head+r->client_pending_count+1)<0){
    /* Clear ONLY the pending config (the joint entry was never appended).
       The deferred-reconfig catch-up state (cookie + target config) must
       SURVIVE: the caller (raft_maybe_advance_reconfig) will retry, and a
       full raft_config_pending_clear here would drop the accepted reconfig's
       cookie, leaving the client without a terminal result. */
    raft_config_pending_only_clear(r);
    return -1;
  }
  r->config_index=raft_log_last_index(&r->log)+1;
  if(raft_log_append(&r->log,r->current_term,RAFT_ENTRY_CONFIG,0,0,raft_set_view(&r->config_pending_old),raft_set_view(&r->config_pending_new),raft_set_view(&r->config_pending_learners))<0){
    /* Same rule: raft_log_append is atomic, so the catch-up cookie survives. */
    raft_config_pending_only_clear(r);
    return -1;
  }
  /* A learner promoted by this change (now a voter in the new config) completes
     its catch-up: emit CATCHUP_READY for its ADD_LEARNER cookie BEFORE the
     config apply rebuilds the peer as a voter and drops the cookie, so the
     caller always gets a terminal result even when it reconfigures before
     waiting for the learner's own CATCHUP_READY ($4.2.1). */
  for(i=0;i<r->config_pending_new.id_count;i++){
    pid=r->config_pending_new.ids[i];
    if(pid==r->cfg.id) continue;
    lp=raft_find_peer(r,pid);
    if(lp&&lp->is_learner&&lp->catchup_cookie&&lp->catchup_round>=0){
      if(raft_result_ensure(r,r->result_count+1)<0) break;
      raft_result_emit(r,lp->catchup_cookie,RAFT_CLIENT_CATCHUP_READY);
      lp->catchup_round=-1; /* catch-up complete: result emitted, loop skips it */
    }
  }
  if(raft_apply_config_from_log(r)<0){
    raft_log_cut(r,r->config_index-1);
    /* The joint entry is rolled back; the catch-up cookie must survive so the
       deferred-reconfig client is retried rather than silently dropped. */
    raft_config_pending_only_clear(r);
    return -1;
  }
  r->persist_needed=1;
  r->client_pending[r->client_pending_head+r->client_pending_count].cookie=cookie;
  r->client_pending[r->client_pending_head+r->client_pending_count].log_index=r->config_index;
  r->client_pending_count++;
  return 0;
}
/* ---- deferred reconfig: catch-up before joint creation ($4.1 single-server) ---- */
static int raft_new_servers_caught_up(raft_ctx *r,raft_mask old_mask,raft_mask new_mask,raft_i64 target_index){
  int i,pid;
  raft_peer *pr;
  if(!raft_mask_any(old_mask)||!raft_mask_any(new_mask)) return 0;
  for(i=0;i<new_mask.id_count;i++){
    pid=new_mask.ids[i];
    if(raft_mask_has(old_mask,pid)) continue;
    if(pid==r->cfg.id) continue;
    pr=raft_find_peer(r,pid);
    /* A new server that is not yet a peer must be caught up before the joint
       entry is created ($4.2.1).  This holds for EVERY cluster size, including
       a single-node bootstrap expanding to its first followers ($4.4): the new
       servers catch up to the LEADER's log, not to "existing peers". */
    if(!pr) return 0;
    /* A learner being promoted must ALSO be caught up first ($4.2.1): the
       reconfiguration proceeds only once every new voter has reached the
       leader's log tip, learner or not. */
    if(pr->match_index<target_index) return 0;
  }
  return 1;
}
static int raft_ensure_catchup_peers(raft_ctx *r,raft_mask new_mask){
  int i,pid,old_count,need=0;
  raft_peer *np;
  old_count=r->peer_count;
  for(i=0;i<new_mask.id_count;i++){
    pid=new_mask.ids[i];
    if(pid==r->cfg.id) continue;
    if(raft_find_peer(r,pid)) continue;
    need++;
  }
  if(need<=0) return 0;
  np=(raft_peer *)RAFT_REALLOC(r->peers,(unsigned int)(old_count+need)*sizeof(raft_peer));
  if(!np) return -1;
  r->peers=np;
  memset(&r->peers[old_count],0,(unsigned int)need*sizeof(raft_peer));
  for(i=0;i<new_mask.id_count;i++){
    pid=new_mask.ids[i];
    if(pid==r->cfg.id) continue;
    if(raft_find_peer_in(r->peers,old_count,pid)>=0) continue;
    r->peers[r->peer_count].id=pid;
    r->peers[r->peer_count].is_learner=0;
    r->peers[r->peer_count].next_index=raft_log_last_index(&r->log)+1;
    r->peers[r->peer_count].match_index=0;
    r->peers[r->peer_count].missed_rounds=0;
    r->peers[r->peer_count].in_old_config=0;
    r->peers[r->peer_count].in_new_config=0;
    r->peer_count++;
  }
  return 0;
}
/* Abort a deferred reconfig catch-up with a terminal CATCHUP_FAILED result
   ($4.2.1: the new server is too slow / never catches up).  Clears all
   catch-up state so a later reconfig can start a fresh cycle. */
static void raft_deferred_catchup_abort(raft_ctx *r){
  if(raft_result_ensure(r,r->result_count+1)==0) raft_result_emit(r,r->config_catchup_cookie,RAFT_CLIENT_CATCHUP_FAILED);
  raft_set_clear(&r->config_catchup_new);
  r->config_catchup_cookie=0;
  r->config_catchup_active=0;
  r->config_catchup_round=0;
  r->config_catchup_start_index=0;
  r->config_catchup_elapsed=0;
  r->config_catchup_total_elapsed=0;
}
/* Create the joint entry for a deferred reconfig whose catch-up round has
   already completed (the caller, raft_leader_tick, has verified every new
   server reached the round's snapshot).  Moves the cookie into client_pending
   for the terminal COMMITTED result. */
static int raft_maybe_advance_reconfig(raft_ctx *r){
  int *sorted;
  const void *cc;
  if(!r||r->state!=RAFT_LEADER) return 0;
  if(!r->config_catchup_active) return 0;
  /* new servers caught up (verified by caller): create joint entry */
  if(raft_make_sorted_ids(r->config_catchup_new.ids,r->config_catchup_new.id_count,&sorted)<0) return -1;
  if(raft_set_copy(&r->config_pending_old,&r->config_old)<0){
    RAFT_FREE(sorted);
    return -1;
  }
  r->config_pending=1;
  r->config_pending_new.ids=sorted;
  r->config_pending_new.id_count=r->config_catchup_new.id_count;
  if(raft_set_copy(&r->config_pending_learners,&r->config_learners)<0){
    /* OOM: keep the catch-up active (retry next tick) and clean up the partial
       pending config.  Do NOT clear config_catchup_cookie/config_catchup_active,
       or the deferred-reconfig client would be left without a terminal result. */
    raft_set_clear(&r->config_pending_old);
    raft_set_clear(&r->config_pending_new);
    r->config_pending=0;
    return -1;
  }
  cc=r->config_catchup_cookie;
  /* Keep the deferred-reconfig catch-up state alive until
     raft_commit_config_entry SUCCEEDS.  Its OOM paths clear only the pending
     config (see raft_commit_config_entry), so if it fails we must NOT have
     already surrendered the cookie - otherwise the accepted reconfig hangs
     forever with no terminal result. */
  if(raft_commit_config_entry(r,cc)<0) return -1;
  raft_set_clear(&r->config_catchup_new);
  r->config_catchup_active=0;
  r->config_catchup_cookie=0; /* ownership moved to client_pending */
  r->config_catchup_round=0;
  r->config_catchup_start_index=0;
  r->config_catchup_elapsed=0;
  r->config_catchup_total_elapsed=0;
  return 0;
}
static int raft_reconfig(raft_ctx *r,const void *cookie,const int *ids,int id_count){
  int *sorted;
  raft_mask new_cfg_mask;
  if(!r||!cookie)return -1;
  if(r->phase!=RAFT_PHASE_RUNNING) return -1;
  if(r->state!=RAFT_LEADER) return -1;
  if(raft_id_valid(r->transfer_target)) return -1; /* Sec. 3.10 step 1: no new client requests during transfer */
  if(r->config_joint) return -1; /* at most one uncommitted config ($4.1) */
  if(r->config_catchup_active) return -1; /* deferred reconfig in progress */
  if(raft_make_sorted_ids(ids,id_count,&sorted)<0) return -1;
  new_cfg_mask.ids=sorted;
  new_cfg_mask.id_count=id_count;
  /* no-op: already at target config ($4.1) */
  if(raft_mask_eq(new_cfg_mask,raft_set_view(&r->config_old))){
    RAFT_FREE(sorted);
    /* The target membership is already in effect, so the change is trivially
       committed: emit COMMITTED immediately so the caller's cookie still gets
       a terminal result (it is never queued via client_pending).  If the
       result buffer cannot grow (OOM), REJECT (-1) instead of returning 0
       with no terminal result: accepting a request we cannot confirm would
       hang the caller forever (seed 6852).  The client retries and, once
       memory is available, gets COMMITTED. */
    if(raft_result_ensure(r,r->result_count+1)<0) return -1;
    raft_result_emit(r,cookie,RAFT_CLIENT_COMMITTED);
    return 0;
  }
  /* if no new servers need catch-up, proceed with immediate joint */
  if(raft_new_servers_caught_up(r,raft_set_view(&r->config_old),new_cfg_mask,raft_log_last_index(&r->log))){
    /* if pending not yet appended, cancel and replace */
    if(r->config_pending&&r->config_index==0) raft_config_pending_clear(r);
    raft_config_zombie_clear(r);
    if(r->config_pending&&r->config_index>0){
      RAFT_FREE(sorted);
      return -1;
    }
    if(raft_set_copy(&r->config_pending_old,&r->config_old)<0){
      RAFT_FREE(sorted);
      return -1;
    }
    r->config_pending=1;
    r->config_pending_new.ids=sorted;
    r->config_pending_new.id_count=id_count;
    if(raft_set_copy(&r->config_pending_learners,&r->config_learners)<0){
      raft_config_pending_clear(r);
      return -1;
    }
    return raft_commit_config_entry(r,cookie);
  }
  /* new servers need catch-up: defer joint creation */
  /* if pending not yet appended, cancel and replace */
  if(r->config_pending&&r->config_index==0) raft_config_pending_clear(r);
  raft_config_zombie_clear(r);
  if(r->config_pending&&r->config_index>0){
    RAFT_FREE(sorted);
    return -1;
  }
  if(raft_set_assign(&r->config_catchup_new,ids,id_count)<0){
    RAFT_FREE(sorted);
    return -1;
  }
  RAFT_FREE(sorted);
  r->config_catchup_cookie=cookie;
  if(raft_ensure_catchup_peers(r,raft_set_view(&r->config_catchup_new))<0){
    raft_set_clear(&r->config_catchup_new);
    r->config_catchup_cookie=0;
    return -1;
  }
  r->config_catchup_active=1;
  r->config_catchup_round=0;
  r->config_catchup_start_index=0;
  r->config_catchup_elapsed=0;
  r->config_catchup_total_elapsed=0;
  r->heartbeat_elapsed=r->cfg.heartbeat_ms; /* force AE broadcast next tick ($4.1 catch-up) */
  return 0; /* accepted, deferred until catch-up completes */
}
/* ---- append entries queueing ---- */
static void raft_queue_append_entries(raft_ctx *r,int pi,int *offset){
  raft_peer *pr=&r->peers[pi];
  raft_i64 prev_idx=pr->next_index-1;
  raft_i64 last=raft_log_last_index(&r->log);
  raft_i64 prev_term,off,cnt;
  raft_peer_message *m;
  int count,ci,co,mc,si;
  if(pr->next_index<=r->log.last_included_index){
    /* follower needs snapshot: only emit read if a snapshot exists */
    if(r->snapshot.size<=0) return;
    /* A previous stream reached the end (pending_snapshot_offset >= size) but
       the install result has not advanced next_index past the snapshot yet:
       the follower may have dropped the final chunk(s), restarted mid-stream,
       or installed an OLDER snapshot at the same index (different bytes), so
       restart from offset 0 instead of re-sending the final zero-byte chunk
       forever.  Re-streaming is idempotent: the follower's offset check drops
       chunks of a stream it already completed, and the install result advances
       match_index/next_index to break out of the snapshot path.  (The previous
       match_index<last_index guard was insufficient: match_index tracks the
       LOG boundary, not WHICH snapshot bytes the follower installed, so a
       replaced same-index snapshot left the leader stuck at the end offset.) */
    if(pr->pending_snapshot_offset>=r->snapshot.size) pr->pending_snapshot_offset=0;
    /* record the snapshot actually streamed, so the install result uses it even
       if the leader's snapshot is replaced mid-stream */
    pr->pending_snapshot_last_index=r->snapshot.last_index;
    /* dedup: update existing snap_read for this follower if present */
    for(si=0;si<r->snap_read_count;si++){
      if(r->snap_read_buf[si].follower_id==pr->id){
        r->snap_read_buf[si].last_included_index=r->log.last_included_index;
        r->snap_read_buf[si].byte_offset=pr->pending_snapshot_offset;
        r->snap_read_buf[si].byte_count=r->cfg.snapshot_chunk_size;
        return;
      }
    }
    if(raft_snap_read_ensure(r,r->snap_read_count+1)<0) return;
    r->snap_read_buf[r->snap_read_count].follower_id=pr->id;
    r->snap_read_buf[r->snap_read_count].last_included_index=r->log.last_included_index;
    r->snap_read_buf[r->snap_read_count].byte_offset=pr->pending_snapshot_offset;
    r->snap_read_buf[r->snap_read_count].byte_count=r->cfg.snapshot_chunk_size;
    r->snap_read_count++;
    return;
  }
  /* AppendEntries path: the follower's log overlaps our live log, so any
     in-progress snapshot stream for it is obsolete.  Clear the stale stream
     progress: a snapshot stream can be abandoned when the follower ACKs past
     our last_included_index (an append success that advances next_index) while
     its install result never arrives (e.g. it re-elected at a higher term and
     ignored the chunks).  A leftover pending_snapshot_offset>0 would otherwise
     make checkQuorum count this peer as "actively streaming" forever and a
     removed leader would never step down (liveness). */
  if(pr->pending_snapshot_offset>0) pr->pending_snapshot_offset=0;
  if(raft_msg_ensure(r,r->msg_count+1)<0) return;
  m=&r->msg_buf[r->msg_count];
  m->type=RAFT_MSG_APPEND;
  m->from=r->cfg.id;
  m->to=pr->id;
  m->term=r->current_term;
  prev_term=raft_log_term_at(&r->log,prev_idx);
  m->append_entries.term=r->current_term;
  m->append_entries.prev_log_index=prev_idx;
  m->append_entries.prev_log_term=prev_term;
  m->append_entries.leader_commit=r->commit_index;
  m->append_entries.read_context=r->read_context;
  m->append_entries.leader_id=r->cfg.id;
  off=pr->next_index-r->log.last_included_index-1;
  co=(int)(off&r->log.chunk_mask);
  cnt=last-pr->next_index+1;
  mc=r->log.chunk_size-co;
  if(cnt>mc) cnt=mc;
  if(cnt<0) cnt=0;
  count=(int)cnt;
  /* OOM guard: if entry_size_buf is unallocated OR its capacity cannot hold this
     peer's region [*offset, *offset+count), degrade to heartbeat-only this round.
     The previous guard only caught the never-allocated case (cap<=0); a failed
     REALLOC growth in raft_leader_tick leaves a STALE smaller cap, so we must
     bound the write by the actual region size, or entry_size_buf[base+j] writes
     past the buffer. */
  if(count>0&&(!r->entry_size_buf||r->entry_size_cap<*offset+count)) count=0;
  m->append_entries.entry_count=count;
  m->append_entries.entry_terms=0;
  m->append_entries.entry_kinds=0;
  m->append_entries.entry_data=0;
  m->append_entries.entry_data_sizes=0;
  m->append_entries.entry_cfg_old=0;
  m->append_entries.entry_cfg_new=0;
  m->append_entries.entry_cfg_learners=0;
  if(count>0){
    raft_i64 first_off;
    int j,base=*offset;
    ci=(int)(off>>r->log.chunk_bits);
    m->append_entries.entry_terms=&r->log.chunks[ci].terms[co];
    m->append_entries.entry_kinds=&r->log.chunks[ci].kinds[co];
    first_off=r->log.chunks[ci].data_offsets[co];
    m->append_entries.entry_data=r->log.data+first_off;
    m->append_entries.entry_cfg_old=r->log.chunks[ci].cfg_old?&r->log.chunks[ci].cfg_old[co]:0;
    m->append_entries.entry_cfg_new=r->log.chunks[ci].cfg_new?&r->log.chunks[ci].cfg_new[co]:0;
    m->append_entries.entry_cfg_learners=r->log.chunks[ci].cfg_learners?&r->log.chunks[ci].cfg_learners[co]:0;
    for(j=0;j<count;j++){
      raft_i64 cur_off,next_off;
      int cj=(int)((off+j)>>r->log.chunk_bits);
      int ck=(int)((off+j)&r->log.chunk_mask);
      cur_off=r->log.chunks[cj].data_offsets[ck];
      if(pr->next_index+j==last){
        next_off=(raft_i64)r->log.data_size;
      }else if(cj<r->log.num_chunks-1||ck+1<r->log.chunk_size){
        int cj2=cj,ck2=ck+1;
        if(ck2>=r->log.chunk_size){
          cj2++;
          ck2=0;
        }
        if(cj2<r->log.num_chunks&&r->log.chunks[cj2].data_offsets) next_off=r->log.chunks[cj2].data_offsets[ck2];
        else next_off=(raft_i64)r->log.data_size;
      }else{
        next_off=(raft_i64)r->log.data_size;
      }
      r->entry_size_buf[base+j]=(unsigned int)(next_off>cur_off?next_off-cur_off:0);
    }
    m->append_entries.entry_data_sizes=r->entry_size_buf+base;
    *offset=base+count;
  }
  r->msg_count++;
}
/* ---- heartbeat / replication triggers ---- */
static void raft_leader_tick(raft_ctx *r,unsigned int elapsed_ms){
  int i,needs_heartbeat,si_old,si_new,active_new,active_old,q_old,q_new,reached=0;
  /* advance deferred reconfig catch-up ($4.1 single-server add, $4.2.1
     round-based algorithm, mirroring the learner path).  Each round snapshots
     the log tip; the round completes once every new server's match_index
     reaches that snapshot.  The change is applied after RAFT_CATCHUP_ROUNDS
     rounds whose final round finished within an election minimum; a separate
     total liveness deadline aborts a new server that never catches up at all. */
  if(r->config_catchup_active){
    r->config_catchup_total_elapsed=raft_elapsed_add(r->config_catchup_total_elapsed,elapsed_ms);
    if(r->config_catchup_round==0){
      r->config_catchup_round=1;
      r->config_catchup_start_index=raft_log_last_index(&r->log);
      r->config_catchup_elapsed=0;
    }else if(raft_new_servers_caught_up(r,raft_set_view(&r->config_old),raft_set_view(&r->config_catchup_new),r->config_catchup_start_index)){
      if(r->config_catchup_round>=RAFT_CATCHUP_ROUNDS){
        if(r->config_catchup_elapsed<r->cfg.election_min_ms){
          if(raft_maybe_advance_reconfig(r)<0) return;
        }else{
          raft_deferred_catchup_abort(r);
        }
      }else{
        r->config_catchup_round++;
        r->config_catchup_start_index=raft_log_last_index(&r->log);
        r->config_catchup_elapsed=0;
      }
    }else{
      r->config_catchup_elapsed=raft_elapsed_add(r->config_catchup_elapsed,elapsed_ms);
    }
    /* total liveness deadline: a new server that never catches up aborts
       ($4.2.1 "drowned sailors") */
    if(r->config_catchup_active&&(raft_u64)r->config_catchup_total_elapsed>=(raft_u64)r->cfg.election_min_ms*10u) raft_deferred_catchup_abort(r);
  }
  /* joint config liveness: an uncommitted joint entry that cannot be committed
     within a bounded time aborts the stuck membership change (revert to the
     previous config) instead of churning leaders forever. */
  if(r->config_joint&&r->config_pending&&r->commit_index<r->config_index){
    r->joint_elapsed=raft_elapsed_add(r->joint_elapsed,elapsed_ms);
    if((raft_u64)r->joint_elapsed>=(raft_u64)r->cfg.election_min_ms*10u){
      raft_i64 joint_idx=r->config_index;
      int bi,replicated=0;
      /* A joint entry that a peer already ACKed (match_index >= joint_idx) is
         kept: the leader keeps trying to commit it (a lagging C_new voter can
         still be caught up, e.g. via snapshot); a replicated joint that truly
         can never commit is the standard joint-consensus liveness limit
         ($4.2.2: the operator restores the voter).  A joint that no peer ACKed
         is aborted below - but never by reusing the same (index, term), because
         a follower may have received it without ACKing; the abort instead bumps
         the term and writes a NOOP so any such stale copy is overwritten. */
      for(i=0;i<r->peer_count;i++) if(r->peers[i].match_index>=joint_idx) replicated=1;
      /* Only abort a joint THIS leader appended (its term == current_term):
         match_index is a CURRENT-TERM replication record, so it proves the
         joint never left this leader only for a joint written in this term.
         A joint INHERITED from a previous term must never be truncated here -
         after a restart commit_index is reset to the snapshot boundary, so an
         inherited joint may in fact have been COMMITTED (a majority holds it)
         even though this leader's fresh match_index says "unreplicated";
         truncating it would overwrite a committed entry at the same index
         (leader-completeness violation, seed 119744).  An inherited joint is
         instead kept and committed/finalized once a quorum is reachable. */
      if(!replicated&&raft_log_term_at(&r->log,joint_idx)==r->current_term){
        /* notify the reconfig cookie (and any commands appended after the joint,
           now truncated) with a terminal result */
        /* Ordered by log_index, so the entries at/after joint_idx are a SUFFIX: notify
           them and truncate the tail (no per-entry memmove). */
        {
          int cut=r->client_pending_count;
          for(bi=0;bi<r->client_pending_count;bi++){
            if(r->client_pending[r->client_pending_head+bi].log_index>=joint_idx){
              cut=bi;
              break;
            }
          }
          for(bi=cut;bi<r->client_pending_count;bi++){
            raft_result_emit(r,r->client_pending[r->client_pending_head+bi].cookie,RAFT_CLIENT_FAILED);
          }
          r->client_pending_count=cut;
        }
        /* truncate the uncommitted joint entry and everything after it (safe:
           no peer ACKed it) */
        raft_log_cut(r,joint_idx-1);
        r->config_pending=0;
        r->config_index=0;
        raft_set_clear(&r->config_catchup_new);
        r->config_catchup_cookie=0;
        r->config_catchup_active=0;
        r->config_catchup_round=0;
        r->config_catchup_start_index=0;
        r->config_catchup_elapsed=0;
        r->config_catchup_total_elapsed=0;
        r->joint_elapsed=0;
        /* SAFE revert: a follower may have RECEIVED (but not yet ACKed) the
           joint and the entries after it, so reusing the same (index, term)
           for a later entry would leave that follower's stale copy to be
           committed later (a log-matching violation / split brain).  Bump the
           term and step down WITHOUT appending anything: the next leader is
           elected at a higher term, so any entry it writes at (or after) the
           truncated index carries a new term and forces followers holding the
           stale joint to truncate.  (Appending a NOOP here would be written by
           a node that is not that term's leader, breaking the log-matching
           theorem: a later real leader could write a DIFFERENT entry at the
           same index and term, which a follower's matching-prev check would
           then fail to reconcile.)

           The term bump + step-down MUST happen UNCONDITIONALLY after the
           truncation: the truncation has already retired the (index, term)
           slot, so skipping the bump (e.g. if the config revert below OOMs)
           would leave the leader at the SAME term free to reuse that slot,
           while a follower that received (but never ACKed) the joint keeps its
           stale copy -> log matching (seed 3593).  Do the bump + step-down
           FIRST; the config revert below is best-effort and self-heals via the
           config_apply_stale backstop on OOM. */
        r->persist_needed=1;
        if(r->current_term<RAFT_TERM_MAX) r->current_term++;
        r->voted_for=0;
        raft_step_down(r,r->current_term,0);
        raft_apply_config_from_log(r);
        return;
      }else{
        /* replicated joint: keep it, re-arm the timeout and keep trying */
        r->joint_elapsed=0;
      }
    }
  }else{
    r->joint_elapsed=0;
  }
  /* checkQuorum: if the last heartbeat cycle reached quorum, reset the
     election timer.  Self is counted only while still a voting member of
     each config ($4.2.2); each peer contributes at most once (either its
     per-window quorum_acked flag or an active snapshot stream), and the
     in_old_config/in_new_config flags exclude peers outside the respective
     config (learners, in neither, never count). */
  si_old=raft_mask_has(raft_set_view(&r->config_old),r->cfg.id);
  si_new=raft_mask_has(raft_set_view(&r->config_new),r->cfg.id);
  /* A peer mid snapshot-stream (pending_snapshot_offset>0) is being actively
     caught up: the leader is sending it data, so it counts as leader CONTACT
     for checkQuorum even though the InstallSnapshot RESULT only arrives after
     the last chunk.  Without this, a leader whose C_new voter is far behind
     would exhaust its election deadline long before a (multi-chunk) snapshot
     stream completes, step down, and restart the stream from scratch on every
     re-election - a livelock.  Safety-neutral: commit still requires real
     ACKs (match_index), so this only prevents premature step-down. */
  active_new=si_new;
  active_old=si_old;
  for(i=0;i<r->peer_count;i++){
    if(r->peers[i].quorum_acked||r->peers[i].pending_snapshot_offset>0){
      if(r->peers[i].in_old_config) active_old++;
      if(r->peers[i].in_new_config) active_new++;
    }
  }
  if(r->config_joint){
    q_old=raft_quorum_size_of(r,1);
    q_new=raft_quorum_size_of(r,0);
    reached=(active_old>=q_old&&active_new>=q_new);
  }else{
    reached=(active_new>=raft_quorum_size_of(r,0));
  }
  if(reached) r->election_elapsed=0;
  /* per-peer quorum_acked flags accumulate across ticks (rolling window); they
     reset on heartbeat emission below, not every tick.  Resetting here (once per
     tick) would shrink the contact window to a single poll interval and, in a
     5-node cluster, two followers ACKing in ADJACENT ticks would never count
     together - the leader would falsely step down on network jitter (9.7). */
  /* Leader read barrier: advance the generation only on a FRESH heartbeat
     quorum (read_index_acked from the current round).  Contact alone is not
     enough - a delayed stale ACK from a peer that has since moved to a higher
     term would otherwise resolve the barrier READY at a stale commit index
     (seed 1460).  Mirrors the follower ReadIndex flush's fresh-ack quorum. */
  if(r->read_barrier_count>0){
    int fa_old=si_old,fa_new=si_new;
    for(i=0;i<r->peer_count;i++){
      if(r->peers[i].read_index_acked&&!r->peers[i].is_learner){
        if(r->peers[i].in_old_config) fa_old++;
        if(r->peers[i].in_new_config) fa_new++;
      }
    }
    if(r->config_joint){
      if(fa_old>=raft_quorum_size_of(r,1)&&fa_new>=raft_quorum_size_of(r,0)) r->read_barrier_gen++;
    }else{
      if(fa_new>=raft_quorum_size_of(r,0)) r->read_barrier_gen++;
    }
  }
  if(r->election_elapsed>=r->election_deadline){
    raft_step_down(r,r->current_term,0);
    return;
  }
  /* Advance commit and finalize a newly-committed joint BEFORE the heartbeat:
     ACKs were already folded into match_index by the recv entry points, and
     appending C_new here (rather than after the heartbeat) means the heartbeat
     below replicates it in the SAME tick.  Finalizing after the heartbeat left
     a crash window where C_new was in the leader's log but not the C_new
     voter's - that voter then had a SHORTER log than a removed node holding the
     uncommitted C_new entry and could never be elected (leader-completeness
     deadlock). */
  raft_advance_commit(r);
  if(raft_maybe_finalize_joint(r)<0) return;
  needs_heartbeat=(r->heartbeat_elapsed>=r->cfg.heartbeat_ms);
  if(r->read_barrier_count>0&&!r->leadership_confirmed) needs_heartbeat=1;
  if(needs_heartbeat){
    raft_i64 total_count=0;
    int wp=0,k,running_off=0;
    int old_msg_count=r->msg_count;
    r->heartbeat_elapsed=0;
    /* start a fresh checkQuorum contact window on each heartbeat: per-peer
       quorum_acked flags from the just-sent round accumulate across ticks
       until the next heartbeat resets them. */
    for(i=0;i<r->peer_count;i++){
      if(r->peers[i].quorum_acked) r->peers[i].missed_rounds=0; else r->peers[i].missed_rounds++;
      r->peers[i].quorum_acked=0;
    }
    /* pass 1: compute total entry-size count across all peers */
    for(i=0;i<r->peer_count;i++){
      raft_peer *pr2=&r->peers[i];
      int co2,mc2;
      if(pr2->next_index<=r->log.last_included_index) continue;
      co2=(int)((pr2->next_index-r->log.last_included_index-1)&r->log.chunk_mask);
      mc2=r->log.chunk_size-co2;
      total_count+=mc2;
    }
    if(total_count>r->entry_size_cap&&total_count<=RAFT_I64_C(2147483647)){
      /* grow (or OOM): keep old buffer on failure; transient degrade to
         heartbeat-only this round (realloc leaves memory intact). */
      unsigned int *nb3=(unsigned int*)RAFT_REALLOC(r->entry_size_buf,(unsigned int)total_count*sizeof(unsigned int));
      if(nb3){
        r->entry_size_buf=nb3;
        r->entry_size_cap=(int)total_count;
      }
    }
    /* pass 2: generate messages, each with its own entry_size_buf region */
    for(i=0;i<r->peer_count;i++) raft_queue_append_entries(r,i,&running_off);
    /* compact: new AppendEntries supersede old ones per peer.
       Keep non-APPEND messages (votes, read_index, etc.) and
       only the latest APPEND messages generated by this heartbeat. */
    for(k=0;k<r->msg_count;k++){
      if(r->msg_buf[k].type==RAFT_MSG_APPEND&&k<old_msg_count) continue;
      if(wp!=k) r->msg_buf[wp]=r->msg_buf[k];
      wp++;
    }
    r->msg_count=wp;
    r->read_index_heartbeat_sent=1; /* fresh heartbeat sent: eligible to collect ReadIndex acks */
  }
  /* flush deferred ReadIndex responses once quorum peers have ACKed */
  if(r->read_index_pending_count>0){
    int pi,ri,ack_old=0,ack_new=0,flush_ok=0;
    if(raft_mask_has(raft_set_view(&r->config_old),r->cfg.id)) ack_old=1;
    if(raft_mask_has(raft_set_view(&r->config_new),r->cfg.id)) ack_new=1;
    for(pi=0;pi<r->peer_count;pi++){
      if(r->peers[pi].read_index_acked&&!r->peers[pi].is_learner){
        if(r->peers[pi].in_old_config) ack_old++;
        if(r->peers[pi].in_new_config) ack_new++;
      }
    }
    if(r->config_joint) flush_ok=(ack_old>=raft_quorum_size_of(r,1)&&ack_new>=raft_quorum_size_of(r,0));
    else flush_ok=(ack_new>=raft_quorum_size_of(r,0));
    /* Sec. 6.4: the contact quorum above only proves this server is STILL the
       leader (no newer leader elected).  Before leadership_confirmed, the
       leader's commit_index can lag the true committed prefix (its current-
       term NOOP is not yet committed), so a request captured with index 0
       would re-read a stale commit_index and leak a stale read to the
       follower.  Flush only once the NOOP is committed - leadership_confirmed
       is set by raft_advance_commit exactly when commit_index advances past
       the current-term entry, at which point commit_index covers every
       previously committed entry. */
    if(flush_ok&&r->leadership_confirmed){
      if(raft_msg_ensure(r,r->msg_count+r->read_index_pending_count)==0){
        for(ri=0;ri<r->read_index_pending_count;ri++){
          /* Sec. 6.4 step 3: capture the commit index NOW - the fresh heartbeat
             quorum (flush_ok) just re-confirmed leadership and, together with
             the leadership_confirmed gate, guarantees this commit_index covers
             every previously committed entry.  Never use a receipt-time
             capture: it is stale if the flush was delayed. */
          raft_i64 ri_idx=r->commit_index;
          r->msg_buf[r->msg_count].type=RAFT_MSG_READ_INDEX_RESULT;
          r->msg_buf[r->msg_count].from=r->cfg.id;
          r->msg_buf[r->msg_count].to=r->read_index_pending[ri].from;
          r->msg_buf[r->msg_count].term=r->current_term;
          r->msg_buf[r->msg_count].read_index_result.term=r->current_term;
          r->msg_buf[r->msg_count].read_index_result.read_index=ri_idx;
          r->msg_buf[r->msg_count].read_index_result.context=r->read_index_pending[ri].context;
          r->msg_count++;
        }
        r->read_index_pending_count=0;
        r->read_index_heartbeat_sent=0;
      }
    }
  }
  /* learner catchup round tracking ($4.2.1) */
  for(i=0;i<r->peer_count;i++){
    raft_peer *pr=&r->peers[i];
    if(!pr->is_learner||pr->catchup_round<0) continue;
    if(pr->catchup_round==0){
      pr->catchup_round=1;
      pr->catchup_round_start_index=raft_log_last_index(&r->log);
      pr->catchup_round_elapsed=0;
    }else if(pr->match_index>=pr->catchup_round_start_index){
      if(pr->catchup_round>=RAFT_CATCHUP_ROUNDS){
        if(pr->catchup_round_elapsed<r->cfg.election_min_ms){
          if(raft_result_ensure(r,r->result_count+1)<0) break;
          raft_result_emit(r,pr->catchup_cookie,RAFT_CLIENT_CATCHUP_READY);
          pr->catchup_round=-1;
        }else{
          if(raft_result_ensure(r,r->result_count+1)<0) break;
          raft_result_emit(r,pr->catchup_cookie,RAFT_CLIENT_CATCHUP_FAILED);
          pr->catchup_round=-2;
        }
      }else{
        pr->catchup_round++;
        pr->catchup_round_start_index=raft_log_last_index(&r->log);
        pr->catchup_round_elapsed=0;
      }
    }else{
      pr->catchup_round_elapsed=raft_elapsed_add(pr->catchup_round_elapsed,elapsed_ms);
    }
  }
}
/* Look up the client_pending cookie for a log index (0 if none).  client_pending
   is ordered by log_index and small (in-flight client requests), so a linear
   scan is fine.  Called from raft_enqueue_apply to tag each offered entry with
   its submitter so the app can correlate an apply outcome back to the caller. */
static const void *raft_pending_cookie(const raft_ctx *r,raft_i64 idx){
  int i;
  for(i=0;i<r->client_pending_count;i++){
    if(r->client_pending[r->client_pending_head+i].log_index==idx) return r->client_pending[r->client_pending_head+i].cookie;
  }
  return 0;
}
/* ---- enqueue committed entries for application ---- */
static void raft_enqueue_apply(raft_ctx *r){
  raft_i64 idx,data_off,off,next_off;
  int ci,co;
  while(r->apply_offered<r->commit_index){
    idx=r->apply_offered+1;
    /* A pending snapshot install may DISCARD entries at/after its boundary
       (mismatched boundary term, Sec. 7.3): do not offer apply entries past it,
       or the app would apply entries that are about to be dropped, leaving
       last_applied past the boundary while the install resets the log to it
       (seed 4652).  The kept tail (if any) is offered after the install. */
    if(r->snapshot_install_pending&&idx>r->snapshot_recv.last_index) break;
    if(raft_apply_ensure(r,r->apply_count+1)<0) return;
    r->apply_buf[r->apply_count].index=idx;
    r->apply_buf[r->apply_count].term=raft_log_term_at(&r->log,idx);
    data_off=idx-r->log.last_included_index-1;
    ci=(int)(data_off>>r->log.chunk_bits);
    co=(int)(data_off&r->log.chunk_mask);
    r->apply_buf[r->apply_count].kind=RAFT_ENTRY_COMMAND;
    r->apply_buf[r->apply_count].command=0;
    r->apply_buf[r->apply_count].command_size=0;
    if(r->log.chunks&&ci<r->log.num_chunks&&r->log.chunks[ci].data_offsets){
      if(r->log.chunks[ci].kinds) r->apply_buf[r->apply_count].kind=(int)r->log.chunks[ci].kinds[co];
      off=r->log.chunks[ci].data_offsets[co];
      r->apply_buf[r->apply_count].command=r->log.data+off;
      if(idx<=raft_log_last_index(&r->log)){
        if(idx<raft_log_last_index(&r->log)){
          if(co+1<r->log.chunk_size) next_off=r->log.chunks[ci].data_offsets[co+1];
          else if(ci+1<r->log.num_chunks&&r->log.chunks[ci+1].data_offsets) next_off=r->log.chunks[ci+1].data_offsets[0];
          else next_off=(raft_i64)r->log.data_size;
        }else{
          next_off=(raft_i64)r->log.data_size;
        }
        if(next_off>off) r->apply_buf[r->apply_count].command_size=(unsigned int)(next_off-off);
      }
    }
    r->apply_buf[r->apply_count].cookie=raft_pending_cookie(r,idx);
    r->apply_buf[r->apply_count].response=0;
    r->apply_buf[r->apply_count].response_capacity=0;
    /* CONFIG entries carry no data: expose the new member set from the log's
       parallel cfg masks so the app can learn it from Ready (never raft_inspect).
       Non-config entries (and chunks with no config entry) leave these 0. */
    r->apply_buf[r->apply_count].cfg_old=0;
    r->apply_buf[r->apply_count].cfg_new=0;
    r->apply_buf[r->apply_count].cfg_learners=0;
    if(r->apply_buf[r->apply_count].kind==RAFT_ENTRY_CONFIG&&r->log.chunks&&ci<r->log.num_chunks&&r->log.chunks[ci].cfg_old){
      r->apply_buf[r->apply_count].cfg_old=&r->log.chunks[ci].cfg_old[co];
      r->apply_buf[r->apply_count].cfg_new=&r->log.chunks[ci].cfg_new[co];
      r->apply_buf[r->apply_count].cfg_learners=&r->log.chunks[ci].cfg_learners[co];
    }
    r->apply_count++;
    r->apply_offered=idx;
  }
}
/* materialize a queued follower ReadIndex request and freeze the whole
   accumulated batch: barriers arriving after emission join the next round, so
   each batch's readIndex is never older than its own invocations (Sec. 6.4) */
static void raft_follower_read_materialize(raft_ctx *r){
  if(raft_msg_ensure(r,r->msg_count+1)<0) return; /* OOM: retry next advance */
  r->msg_buf[r->msg_count].type=RAFT_MSG_READ_INDEX;
  r->msg_buf[r->msg_count].from=r->cfg.id;
  r->msg_buf[r->msg_count].to=r->leader_id;
  r->msg_buf[r->msg_count].term=r->current_term;
  r->msg_buf[r->msg_count].read_index_req.term=r->current_term;
  /* Monotonic round identity, never 0 (0 is reserved for "no round"), so a
     result can only match the round that is actually in flight. */
  r->follower_read_context++;
  if(r->follower_read_context==0) r->follower_read_context=1;
  r->msg_buf[r->msg_count].read_index_req.context=r->follower_read_context;
  r->msg_count++;
  r->follower_read_frozen=r->follower_read_pending_count;
  r->follower_read_elapsed=0;
  r->follower_read_waiting=0;
}
/* ---- read barrier check ---- */
static void raft_check_barriers(raft_ctx *r){
  int i;
  for(i=0;i<r->read_barrier_count;){
    int ready;
    if(r->state==RAFT_LEADER){
      raft_i64 effective_target=r->read_barriers[i].target_index;
      if(r->commit_index>effective_target) effective_target=r->commit_index;
      ready=r->leadership_confirmed&&r->read_barrier_gen>=r->read_barriers[i].needs_gen&&r->last_applied>=effective_target; /* Sec. 6.4: fresh heartbeat quorum */
    }else{
      ready=r->last_applied>=r->read_barriers[i].target_index;
    }
    if(ready){
      if(raft_result_ensure(r,r->result_count+1)<0) break;
      raft_result_emit(r,r->read_barriers[i].cookie,RAFT_CLIENT_READY);
      memmove(&r->read_barriers[i],&r->read_barriers[i+1],(unsigned int)(r->read_barrier_count-i-1)*sizeof(raft_read_barrier));
      r->read_barrier_count--;
    }else i++;
  }
}
/* ---- advance main logic ---- */
static int raft_advance_inner(raft_ctx *r,unsigned int elapsed_ms){
  /* Restore the config-immediate invariant on EVERY advance, in EVERY state
     (leader included): a transient OOM in a previous config apply - or in the
     config REVERT after an aborted/truncated joint entry - leaves the live
     config stale.  A leader with a stale config can commit entries under the
     WRONG configuration (e.g. a joint config whose entry was truncated), and a
     candidate can campaign under an older config and overwrite committed
     entries (leader completeness).  On OOM here the re-apply is retried on the
     next advance. */
  if(r->config_apply_stale&&raft_apply_config_from_log(r)<0) return -1;
  if(r->state==RAFT_FOLLOWER||r->state==RAFT_CANDIDATE){
    /* Sec. 9.6: BOTH followers and candidates only pre-vote on timeout; a term
      increment happens solely via a pre-vote majority in
      raft_recv_request_vote_result.  A losing/partitioned candidate must
      not escalate on its own and disrupt a valid leader. */
    /* Sec. 4.2.2 (Figure 4.6): a server removed from its latest configuration
       must STILL be able to campaign - until C_new is committed it may be
       needed to lead.  Disruption by a removed server is prevented by the
       heartbeat grace period + the voter's log up-to-date check, not by
       barring it from campaigning. */
    if(r->election_elapsed>=r->election_deadline&&raft_start_pre_vote(r)<0) return -1;
  }
  if(r->state==RAFT_LEADER) raft_leader_tick(r,elapsed_ms);
  raft_enqueue_apply(r);
  raft_check_barriers(r);
  return 0;
}
/* ---- public API ---- */
RAFT_DEF raft_ctx *raft_create(const raft_config *cfg){
  raft_ctx *r;
  int i,pi;
  const raft_persist *p;
  if(!cfg) return 0;
  r=(raft_ctx *)RAFT_CALLOC(1,sizeof(raft_ctx));
  if(!r) return 0;
  /* reject nonsensical timing config up front (the root cause of pathological
     election-deadline / timeout arithmetic downstream) */
  if(cfg->heartbeat_ms==0||cfg->election_min_ms==0) goto fail;
  r->cfg=*cfg;
  r->phase=RAFT_PHASE_READY;
  r->state=RAFT_FOLLOWER;
  r->leader_id=0;
  r->current_term=0;
  r->commit_index=0;
  r->last_applied=0;
  r->apply_offered=0;
  r->config_joint=0;
  r->config_applied_index=0;
  r->config_apply_stale=0;
  r->config_learners.ids=0;
  r->config_learners.id_count=0;
  r->rng=cfg->seed;
  raft_reset_election_deadline(r);
  r->log.chunk_size=cfg->log_chunk_size>0?cfg->log_chunk_size:256;
  /* cap before rounding so the power-of-two loop below can never reach 1<<31
     (signed shift UB) or loop forever on a pathological size */
  if(r->log.chunk_size>(1<<20)) r->log.chunk_size=(1<<20);
  r->log.chunk_bits=0;
  while((1<<r->log.chunk_bits)<r->log.chunk_size) r->log.chunk_bits++;
  /* Round the chunk size up to a power of two so chunk_mask indexes exactly
     chunk_size slots: a non-power-of-two size would otherwise let
     (off & chunk_mask) reach past the allocated chunk arrays (heap overflow). */
  r->log.chunk_size=(1<<r->log.chunk_bits);
  r->log.chunk_mask=r->log.chunk_size-1;
  if(cfg->peer_count>0){
    if(raft_set_assign(&r->config_old,cfg->peers,cfg->peer_count)<0) goto fail;
    if(raft_set_copy(&r->config_new,&r->config_old)<0) goto fail;
    /* $4.4: the bootstrap peer list is the FULL initial configuration and must
       include this node itself.  A config that excludes self would place the
       node outside every quorum (its own vote is never counted), silently. */
    if(!raft_mask_has(raft_set_view(&r->config_old),cfg->id)) goto fail;
    r->peer_count=cfg->peer_count-raft_mask_has(raft_set_view(&r->config_old),cfg->id);
    if(r->peer_count>0) r->peers=(raft_peer *)RAFT_CALLOC((unsigned int)r->peer_count,sizeof(raft_peer));
    if(r->peer_count>0&&!r->peers) goto fail;
    pi=0;
    for(i=0;i<cfg->peer_count;i++){
      if(cfg->peers[i]==cfg->id) continue;
      r->peers[pi].id=cfg->peers[i];
      r->peers[pi].is_learner=0;
      r->peers[pi].next_index=r->log.last_included_index+1;
      r->peers[pi].in_old_config=1;
      r->peers[pi].in_new_config=1;
      pi++;
    }
  }else if(cfg->bootstrap){
    /* empty config: a brand-new node that joins via membership change (Sec 4.4).
       config_old/config_new stay EMPTY, so it has no quorum of its own: it
       never campaigns (pre-vote finds no peers), never advances its term, and
       simply accepts AppendEntries from the cluster leader once added. */
  }else{
    /* single-node: config includes self */
    if(raft_set_assign(&r->config_old,&cfg->id,1)<0) goto fail;
    if(raft_set_copy(&r->config_new,&r->config_old)<0) goto fail;
  }
  /* retain the bootstrap config: the base to revert to when the log has no
     config entry (config-immediate must revert a truncated config entry, $4.1) */
  if(raft_set_copy(&r->config_bootstrap,&r->config_old)<0) goto fail;
  if(cfg->restore){
    p=cfg->restore;
    if(p->term<0||p->term>=RAFT_TERM_MAX+1) goto fail; /* cap: restored term out of range */
    if(p->last_included_index<0) goto fail;
    if(p->last_included_term<0||p->last_included_term>=RAFT_TERM_MAX+1) goto fail;
    if(p->snapshot_size<0) goto fail;
    r->current_term=p->term;
    r->voted_for=p->voted_for;
    r->commit_index=p->last_included_index;
    r->last_applied=p->last_included_index;
    r->apply_offered=p->last_included_index;
    r->log.last_included_index=p->last_included_index;
    r->log.last_included_term=p->last_included_term;
    /* Rebuild snapshot metadata so a restored leader can stream the snapshot to
       lagging followers ($5.1).  size==0 (no durable snapshot file) disables
       streaming, matching the live-node behavior. */
    r->snapshot.last_index=p->last_included_index;
    r->snapshot.last_term=p->last_included_term;
    r->snapshot.size=p->snapshot_size;
    if(raft_mask_any(p->snapshot_cfg_old)){
      if(raft_set_assign(&r->config_old,p->snapshot_cfg_old.ids,p->snapshot_cfg_old.id_count)<0) goto fail;
      if(raft_set_assign(&r->config_new,p->snapshot_cfg_new.ids,p->snapshot_cfg_new.id_count)<0) goto fail;
      if(raft_set_assign(&r->snapshot.cfg_old,p->snapshot_cfg_old.ids,p->snapshot_cfg_old.id_count)<0) goto fail;
      if(raft_set_assign(&r->snapshot.cfg_new,p->snapshot_cfg_new.ids,p->snapshot_cfg_new.id_count)<0) goto fail;
      if(!raft_mask_eq(p->snapshot_cfg_old,p->snapshot_cfg_new)) r->config_joint=1;
    }
    if(raft_mask_any(p->snapshot_cfg_learners)){
      if(raft_set_assign(&r->config_learners,p->snapshot_cfg_learners.ids,p->snapshot_cfg_learners.id_count)<0) goto fail;
      if(raft_set_assign(&r->snapshot.cfg_learners,p->snapshot_cfg_learners.ids,p->snapshot_cfg_learners.id_count)<0) goto fail;
    }
    if(p->log_entry_count<0||(p->log_entry_count>0&&!p->log_entries)) goto fail;
    for(i=0;i<p->log_entry_count;i++){
      const raft_persist_entry *entry=&p->log_entries[i];
      if(entry->index!=raft_log_last_index(&r->log)+1) goto fail;
      if(entry->term<0||entry->term>=RAFT_TERM_MAX+1||entry->kind<RAFT_ENTRY_COMMAND||entry->kind>RAFT_ENTRY_CONFIG) goto fail;
      if(entry->data_size>0&&!entry->data) goto fail;
      if(raft_log_append(&r->log,entry->term,entry->kind,entry->data,entry->data_size,entry->cfg_old,entry->cfg_new,entry->cfg_learners)<0) goto fail;
    }
    /* The restored log is durable by definition: it was fsync'ed before being
       reported via raft_persist_complete, so durable_index starts at the
       restored log tip.  Without this a restored node answers with
       durable_index=0 until the caller re-reports it, and a restored leader
       cannot count its own entries toward the commit majority ($10.2.1). */
    r->durable_index=raft_log_last_index(&r->log);
    r->durable_confirm=r->durable_index;
    r->persist_synced_index=r->durable_index;
    if(raft_apply_config_from_log(r)<0) goto fail;
  }
  /* rebuild peers from restored config (overrides bootstrap peers) */
  if(cfg->restore){
    int rr2;
    if(r->config_joint) rr2=raft_rebuild_peers_joint(r);
    else rr2=raft_rebuild_peers_normal(r);
    if(rr2<0) goto fail;
  }
  r->config_applied_index=r->last_applied;
  r->cfg.peers=0; /* clear shallow copy; config is in r->config_old/config_new */
  return r;
fail:
  raft_destroy(r);
  return 0;
}
RAFT_DEF void raft_destroy(raft_ctx *r){
  int i;
  if(!r) return;
  raft_set_clear(&r->config_old);
  raft_set_clear(&r->config_new);
  raft_set_clear(&r->config_learners);
  raft_set_clear(&r->config_bootstrap);
  raft_set_clear(&r->config_pending_old);
  raft_set_clear(&r->config_pending_new);
  raft_set_clear(&r->config_pending_learners);
  raft_set_clear(&r->snapshot.cfg_old);
  raft_set_clear(&r->snapshot.cfg_new);
  raft_set_clear(&r->snapshot.cfg_learners);
  raft_set_clear(&r->snapshot_pending.cfg_old);
  raft_set_clear(&r->snapshot_pending.cfg_new);
  raft_set_clear(&r->snapshot_pending.cfg_learners);
  raft_set_clear(&r->snapshot_recv.cfg_old);
  raft_set_clear(&r->snapshot_recv.cfg_new);
  raft_set_clear(&r->snapshot_recv.cfg_learners);
  raft_set_clear(&r->config_catchup_new);
  if(r->peers) RAFT_FREE(r->peers);
  if(r->log.chunks){
    for(i=0;i<r->log.num_chunks;i++) raft_log_chunk_free(&r->log.chunks[i],r->log.chunk_size);
    RAFT_FREE(r->log.chunks);
  }
  if(r->log.data) RAFT_FREE(r->log.data);
  if(r->read_barriers) RAFT_FREE(r->read_barriers);
  if(r->read_index_pending) RAFT_FREE(r->read_index_pending);
  if(r->follower_read_pending) RAFT_FREE(r->follower_read_pending);
  if(r->client_pending) RAFT_FREE(r->client_pending);
  if(r->msg_buf) RAFT_FREE(r->msg_buf);
  if(r->apply_buf) RAFT_FREE(r->apply_buf);
  if(r->peer_health_buf) RAFT_FREE(r->peer_health_buf);
  if(r->result_buf) RAFT_FREE(r->result_buf);
  if(r->snap_read_buf) RAFT_FREE(r->snap_read_buf);
  if(r->persist_entry_buf) RAFT_FREE(r->persist_entry_buf);
  if(r->entry_size_buf) RAFT_FREE(r->entry_size_buf);
  RAFT_FREE(r);
}
RAFT_DEF int raft_advance(raft_ctx *r,unsigned int elapsed_ms,raft_ready *ready){
  int drain_pending,lc,is_ldr,ldr_id,frd_i,frd_n,rbi,rbj,rbn,rb_pending,i;
  raft_i64 li;
  /* ZERO THE BUNDLE BEFORE THE REMAINING GUARDS.  An early return used to leave the caller's struct
     exactly as it found it, so a failed raft_advance left whatever the stack held in every field and a
     caller that read one (the tests did) read uninitialized memory - which is what GCC 16.2 reports as
     17 `ready.* may be used uninitialized` sites in tests/raft_test.c (issue #6, gcc 15.2.0 does not see
     it).  A failed advance must leave a well-defined EMPTY bundle instead: the return code still says
     the call failed, and no field is ever undefined. */
  if(!ready) return -1;
  memset(ready,0,sizeof(raft_ready));
  if(!r) return -1;
  if(!raft_phase_ge(r,RAFT_PHASE_READY)) return -1;
  r->prev_leader_state=r->state;
  if(r->phase==RAFT_PHASE_STOPPING){
    r->state=RAFT_FOLLOWER;
    r->leader_id=0;
    r->snapshot_data_ready_flag=0;
    r->snapshot_pending_dirty=0;
    r->phase=RAFT_PHASE_DRAINING;
  }
  if(r->phase==RAFT_PHASE_READY) r->phase=RAFT_PHASE_RUNNING;
  if(r->phase==RAFT_PHASE_DRAINING){
    r->heartbeat_elapsed=raft_elapsed_add(r->heartbeat_elapsed,elapsed_ms);
    r->election_elapsed=raft_elapsed_add(r->election_elapsed,elapsed_ms);
    r->leader_contact_elapsed=raft_elapsed_add(r->leader_contact_elapsed,elapsed_ms);
    raft_enqueue_apply(r);
    raft_check_barriers(r);
    /* A pending read barrier is NOT drainable work: resolvable barriers were
       already emitted as results by raft_check_barriers (result_count>0), and
       the rest are dead ends (target_index > last_applied, with no further
       applies coming).  Counting read_barrier_count here would keep
       drain_pending true forever and make the flush-and-stop below unreachable
       -- a shutdown deadlock.  Omit it so unresolvable barriers are flushed as
       FAILED and the node reaches STOPPED. */
    drain_pending=r->apply_count>0||r->result_count>0||r->msg_count>0||r->snap_read_count>0||r->persist_needed||r->snapshot_install_pending;
    if(!drain_pending){
      /* flush remaining barriers/client_pending/follower read barriers as
         failed before stopping (symmetric to raft_step_down) */
      raft_flush_client_results(r,RAFT_CLIENT_FAILED);
      /* drop unanswered leader-side ReadIndex requests (mirrors raft_step_down):
         the requesting followers fail via their own follower_read timeout. */
      r->read_index_pending_count=0;
      r->read_index_heartbeat_sent=0;
      /* flush a deferred reconfig catch-up as failed (symmetric to client_pending) */
      if(r->config_catchup_active&&r->config_catchup_cookie){
        if(raft_result_ensure(r,r->result_count+1)==0) raft_result_emit(r,r->config_catchup_cookie,RAFT_CLIENT_FAILED);
        r->config_catchup_cookie=0;
        r->config_catchup_active=0;
        r->config_catchup_round=0;
        r->config_catchup_start_index=0;
        r->config_catchup_elapsed=0;
        r->config_catchup_total_elapsed=0;
        raft_set_clear(&r->config_catchup_new); /* symmetric with step_down/timeout paths */
      }
      /* flush an in-progress leadership transfer as failed (symmetric to the
         other client cookies; step_down emits REDIRECT instead, but shutdown
         must not leave the transfer initiator hanging) */
      if(r->transfer_cookie){
        if(raft_result_ensure(r,r->result_count+1)==0) raft_result_emit(r,r->transfer_cookie,RAFT_CLIENT_FAILED);
        r->transfer_cookie=0;
      }
      r->transfer_target=0;
      r->transfer_elapsed=0;
      ready->phase_stopped=1;
      r->phase=RAFT_PHASE_STOPPED;
    }
    ready->has_work=1;
    goto output;
  }
  if(r->phase==RAFT_PHASE_STOPPED){
    ready->phase_stopped=1;
    return 0;
  }
  r->heartbeat_elapsed=raft_elapsed_add(r->heartbeat_elapsed,elapsed_ms);
  r->election_elapsed=raft_elapsed_add(r->election_elapsed,elapsed_ms);
  /* A leader does NOT accumulate leader-contact time: it *is* the leader, so its
     Sec. 4.2.3 pre-vote grace period must stay permanently active (leader_contact_elapsed
     is pinned to 0, reset in raft_become_leader).  If it grew here, a long-lived
     leader would "lose" its grace period and start GRANTING disruptive pre-votes to
     stale/removed nodes, letting them escalate to a real election against a healthy
     leader (raft_cluster_fuzz seed 310288: per-term leader churn). */
  if(r->state!=RAFT_LEADER) r->leader_contact_elapsed=raft_elapsed_add(r->leader_contact_elapsed,elapsed_ms);
  if(r->follower_read_frozen>0){
    r->follower_read_elapsed=raft_elapsed_add(r->follower_read_elapsed,elapsed_ms);
    if((raft_u64)r->follower_read_elapsed>(raft_u64)r->cfg.election_max_ms*2u){
      frd_n=r->follower_read_frozen;
      if(raft_result_ensure(r,r->result_count+frd_n)==0){
        for(frd_i=0;frd_i<frd_n;frd_i++) raft_result_emit(r,r->follower_read_pending[frd_i],RAFT_CLIENT_FAILED);
      }
      if(frd_n<r->follower_read_pending_count) memmove(r->follower_read_pending,r->follower_read_pending+frd_n,(unsigned int)(r->follower_read_pending_count-frd_n)*sizeof(void*));
      r->follower_read_pending_count-=frd_n;
      r->follower_read_frozen=0;
      r->follower_read_elapsed=0;
      if(r->follower_read_pending_count>0) r->follower_read_waiting=1;
    }
  }
  /* Sec. 6.4: a non-leader ReadIndex barrier whose state machine never reaches its
     read index (the follower was removed from the configuration, or is
     partitioned without ever stepping down) would otherwise wait forever.
     Fail such stalled barriers after the same bound as the frozen phase, while
     resetting the timer whenever last_applied advances so a follower that is
     still catching up (e.g. via snapshot) is never failed. */
  rb_pending=0;
  if(r->state!=RAFT_LEADER&&r->read_barrier_count>0){
    for(rbi=0;rbi<r->read_barrier_count;rbi++){
      if(r->read_barriers[rbi].target_index>r->last_applied){
        rb_pending=1;
        break;
      }
    }
  }
  if(rb_pending){
    if(r->follower_read_barrier_last_applied!=r->last_applied){
      r->follower_read_barrier_elapsed=0;
      r->follower_read_barrier_last_applied=r->last_applied;
    }else{
      r->follower_read_barrier_elapsed=raft_elapsed_add(r->follower_read_barrier_elapsed,elapsed_ms);
    }
    if((raft_u64)r->follower_read_barrier_elapsed>(raft_u64)r->cfg.election_max_ms*2u){
      rbn=0;
      for(rbj=0;rbj<r->read_barrier_count;rbj++){
        if(r->read_barriers[rbj].target_index>r->last_applied) rbn++;
      }
      if(raft_result_ensure(r,r->result_count+rbn)==0){
        for(rbj=0;rbj<r->read_barrier_count;rbj++){
          if(r->read_barriers[rbj].target_index>r->last_applied) raft_result_emit(r,r->read_barriers[rbj].cookie,RAFT_CLIENT_FAILED);
        }
      }
      for(rbj=0;rbj<r->read_barrier_count;){
        if(r->read_barriers[rbj].target_index>r->last_applied){
          memmove(&r->read_barriers[rbj],&r->read_barriers[rbj+1],(unsigned int)(r->read_barrier_count-rbj-1)*sizeof(raft_read_barrier));
          r->read_barrier_count--;
        }else rbj++;
      }
      r->follower_read_barrier_elapsed=0;
      r->follower_read_barrier_last_applied=r->last_applied;
    }
  }else{
    r->follower_read_barrier_elapsed=0;
    r->follower_read_barrier_last_applied=r->last_applied;
  }
  /* materialize a queued follower ReadIndex at emission time, freezing the
     whole accumulated batch (Sec. 6.4 amortization) */
  if(r->follower_read_waiting) raft_follower_read_materialize(r);
  if(raft_id_valid(r->transfer_target)){
    raft_peer *tp=raft_find_peer(r,r->transfer_target);
    /* Complete the handoff only when the target can still become leader: it
       must remain a voter in the LATEST config (in_new_config).  A target that
       was removed by a concurrent reconfig - left in C_old only, or dropped
       from the peer list - can never win an election, so reporting COMMITTED
       here would be a false success (seed 3089); it falls through to the
       timeout/FAILED path instead. */
    if(tp&&!tp->is_learner&&tp->in_new_config&&tp->match_index>=raft_log_last_index(&r->log)){
      raft_peer_message *tm;
      if(raft_msg_ensure(r,r->msg_count+1)==0){
        tm=&r->msg_buf[r->msg_count];
        tm->type=RAFT_MSG_TIMEOUT_NOW;
        tm->from=r->cfg.id;
        tm->to=r->transfer_target;
        tm->term=r->current_term;
        tm->timeout_now.term=r->current_term;
        li=raft_log_last_index(&r->log);
        tm->timeout_now.last_log_index=li;
        tm->timeout_now.last_log_term=raft_log_term_at(&r->log,li);
        tm->timeout_now.leader_id=r->cfg.id;
        r->msg_count++;
      }
      r->transfer_target=0;
      r->transfer_elapsed=0;
      /* notify caller: leadership transfer initiated ($3.10) */
      if(r->transfer_cookie){
        if(raft_result_ensure(r,r->result_count+1)==0) raft_result_emit(r,r->transfer_cookie,RAFT_CLIENT_COMMITTED);
        r->transfer_cookie=0;
      }
    }else{
      r->transfer_elapsed=raft_elapsed_add(r->transfer_elapsed,elapsed_ms);
      if(r->transfer_elapsed>=r->cfg.election_min_ms){
        r->transfer_target=0;
        r->transfer_elapsed=0;
        /* notify caller: leadership transfer timed out ($3.10) */
        if(r->transfer_cookie){
          if(raft_result_ensure(r,r->result_count+1)==0) raft_result_emit(r,r->transfer_cookie,RAFT_CLIENT_FAILED);
          r->transfer_cookie=0;
        }
      }
    }
  }
  if(r->deferred_vote_response&&r->persist_gen>=r->deferred_vote_gen){
    raft_peer_message *m3;
    if(raft_msg_ensure(r,r->msg_count+1)==0){
      m3=&r->msg_buf[r->msg_count];
      m3->type=RAFT_MSG_REQUEST_VOTE_RESULT;
      m3->from=r->cfg.id;
      m3->to=r->deferred_vote_to;
      m3->term=r->current_term;
      m3->request_vote_result.term=r->current_term;
      m3->request_vote_result.vote_granted=1;
      m3->request_vote_result.pre_vote=0;
      r->msg_count++;
    }
    r->deferred_vote_response=0;
  }
  if(r->deferred_append_response&&r->persist_gen>=r->deferred_append_gen){
    raft_i64 tip2,report;
    tip2=raft_log_last_index(&r->log);
    /* Advertise the range the AppendEntries confirmed (deferred_append_confirmed
       = max prev+entry_count since the latch), capped by the durable frontier and
       by the current log tip - never the durable tip itself, which can sit above
       what was verified and let the leader commit a phantom majority. */
    report=r->durable_confirm<r->deferred_append_confirmed?r->durable_confirm:r->deferred_append_confirmed;
    if(report>tip2) report=tip2;
    /* ACK the CONFIRMED durable frontier - never above it, never above the log
       tip.  Match index means "durably written to this server's disk"
       (dissertation 10.2.1), so an ACK may only advertise what the caller has
       fsync'ed; but it must NOT wait for a persist round to complete after the
       append.  The old gate (persist_gen advanced past the arm) was evaluated at
       the top of an advance, i.e. before the caller's persist of that same round
       finished, so when entry-bearing AppendEntries arrived at least as fast as
       persists completed - a caller that persists promptly, exactly what the
       durability contract asks for - the gate was re-armed every round and could
       never be observed satisfied: followers stopped ACKing, the leader's
       missed_rounds climbed past the election deadline, it stepped down every
       term and reset next_index to its log tip, so a divergent log never
       converged (raft_cluster_fuzz liveness tail, deterministic on every seed).
       Reporting the durable frontier keeps the same safety and always admits an
       ACK.  While the frontier lags, the latch stays set and the next advance
       re-reports it once it has advanced (idempotent for the leader). */
    if(report>r->deferred_append_last_index){
      if(raft_msg_ensure(r,r->msg_count+1)==0){
        raft_peer_message *m4=&r->msg_buf[r->msg_count];
        m4->type=RAFT_MSG_APPEND_RESULT;
        m4->from=r->cfg.id;
        m4->to=r->deferred_append_to;
        m4->term=r->current_term;
        m4->append_entries_result.term=r->current_term;
        m4->append_entries_result.success=1;
        m4->append_entries_result.rejected=0;
        m4->append_entries_result.last_log_index=report;
        m4->append_entries_result.conflict_term=0;
        m4->append_entries_result.conflict_first_index=0;
        m4->append_entries_result.read_context=r->deferred_append_read_context;
        r->msg_count++;
        r->deferred_append_last_index=report;
      }
    }
    /* Released once the confirmed range has been advertised durably; until then
       the latch survives so the ACK is not lost (and is re-reported as the
       durable frontier advances). */
    if(r->durable_confirm>=r->deferred_append_confirmed) r->deferred_append_response=0;
  }
  /* Sec. 3.8 release for a deferred REJECTION (issue #13): it goes out once the term it advertises is
     durable.  Its own latch, so a pending ACK travels independently - overwriting one with the other is
     what stalled the leader's read barrier at raft_cluster_fuzz seed 869. */
  if(r->deferred_reject_pending&&r->persist_gen>=r->deferred_reject_gen){
    if(raft_msg_ensure(r,r->msg_count+1)==0){
      raft_peer_message *m6=&r->msg_buf[r->msg_count];
      m6->type=RAFT_MSG_APPEND_RESULT;
      m6->from=r->cfg.id;
      m6->to=r->deferred_reject_to;
      m6->term=r->current_term;
      m6->append_entries_result.term=r->current_term;
      m6->append_entries_result.success=0;
      m6->append_entries_result.rejected=r->deferred_reject_hint;
      m6->append_entries_result.last_log_index=r->deferred_reject_last_index;
      m6->append_entries_result.conflict_term=r->deferred_reject_conflict_term;
      m6->append_entries_result.conflict_first_index=r->deferred_reject_conflict_first_index;
      m6->append_entries_result.read_context=r->deferred_reject_read_context;
      r->msg_count++;
    }
    r->deferred_reject_pending=0;   /* sent once, not re-reported */
  }
  /* Sec. 3.8: release the candidate's real RequestVote broadcast once the term
     bump + self-vote have been persisted (symmetric with the two deferred
     response blocks above).  On OOM keep the flag and retry next advance. */
  if(r->deferred_vote_request&&r->persist_gen>=r->deferred_vote_request_gen&&r->state==RAFT_CANDIDATE){
    if(raft_broadcast_vote(r,0)==0) r->deferred_vote_request=0;
  }
  /* Election/pre-vote OOM or term cap: inner functions already
      rolled back state (term/voted_for/state).  Continue to output:
      so other accumulated work (apply entries, messages, results)
      is still delivered.  Election retried next advance cycle. */
  if(raft_advance_inner(r,elapsed_ms)<0) goto output;
output:
  lc=(r->prev_leader_state!=r->state);
  is_ldr=(r->state==RAFT_LEADER);
  ldr_id=is_ldr?r->cfg.id:r->leader_id;
  /* accumulate metadata: OR semantics so unconsumed work is not lost */
  if(lc) r->ready_leader_change=1;
  if(r->persist_needed) r->ready_persist_pending=1;
  ready->has_work=(r->apply_count>0||r->result_count>0||r->msg_count>0||r->snap_read_count>0||r->ready_persist_pending||r->ready_leader_change||(r->snapshot_pending_dirty&&r->snapshot_data_ready_flag)||r->snapshot_install_pending||ready->phase_stopped);
  /* Membership FACTS, reported on EVERY advance (issue #14, C6).  They must not sit beside the
     leader-change event flags above: those are only filled when that event fires, and the application's
     policy needs these continuously - a stale copy reads as "no joint configuration", which silently
     disables the silent-standby rule (selftest caught exactly that). */
  ready->config_joint=r->config_joint;
  ready->self_is_voter=raft_set_has(&r->config_new,r->cfg.id);
  ready->self_is_learner=raft_set_has(&r->config_learners,r->cfg.id);
  /* ---- view assembly: pointer assignments to internal buffers ---- */
  if(r->apply_count>0){
    ready->apply_entries=r->apply_buf;
    ready->apply_count=r->apply_count;
  }
  if(r->result_count>0){
    ready->client_results=r->result_buf;
    ready->client_result_count=r->result_count;
  }
  if(r->msg_count>0){
    ready->messages=r->msg_buf;
    ready->message_count=r->msg_count;
  }
  if(r->snap_read_count>0){
    ready->snapshot_reads=r->snap_read_buf;
    ready->snapshot_read_count=r->snap_read_count;
  }
  if(r->ready_persist_pending){
    raft_i64 idx,off,data_off,next_off,last_idx,start_idx,entry_count;
    int ei,ci,co;
    ready->persist_needed=1;
    ready->persist.term=r->current_term;
    ready->persist.voted_for=r->voted_for;
    ready->persist.last_included_index=r->log.last_included_index;
    ready->persist.last_included_term=r->log.last_included_term;
    if(r->snapshot_install_pending){
      ready->persist.snapshot_dirty=1;
      ready->persist.snapshot_size=r->snapshot_recv.size;
      ready->persist.last_included_index=r->snapshot_recv.last_index;
      ready->persist.last_included_term=r->snapshot_recv.last_term;
      ready->persist.snapshot_cfg_old=raft_set_view(&r->snapshot_recv.cfg_old);
      ready->persist.snapshot_cfg_new=raft_set_view(&r->snapshot_recv.cfg_new);
      ready->persist.snapshot_cfg_learners=raft_set_view(&r->snapshot_recv.cfg_learners);
    }else if(r->snapshot_pending_dirty&&r->snapshot_data_ready_flag){
      ready->persist.snapshot_dirty=1;
      ready->persist.snapshot_size=r->snapshot_pending.size;
      ready->persist.last_included_index=r->snapshot_pending.last_index;
      ready->persist.last_included_term=r->snapshot_pending.last_term;
      ready->persist.snapshot_cfg_old=raft_set_view(&r->snapshot_pending.cfg_old);
      ready->persist.snapshot_cfg_new=raft_set_view(&r->snapshot_pending.cfg_new);
      ready->persist.snapshot_cfg_learners=raft_set_view(&r->snapshot_pending.cfg_learners);
    }
    last_idx=raft_log_last_index(&r->log);
    start_idx=ready->persist.last_included_index+1;
    if(start_idx<r->log.last_included_index+1) start_idx=r->log.last_included_index+1;
    /* Everything up to durable_index is ALREADY on disk (the caller reports it durable
       only after the record was fsync'ed), so the persist view carries just the delta.
       Before this, every round re-offered the whole retained log, so each WAL record
       rewrote all entries since the snapshot base: measured ~3x the new bytes at 4 KiB
       values and megabytes per record at 64 KiB, and that record - not the tree copy -
       dominated the write path.  Recovery concatenates records, so a delta is enough;
       a re-offered range is harmless because the reader keeps the LAST version of each
       log index (which is also exactly what a log truncation needs). */
    if(r->persist_synced_index+1>start_idx) start_idx=r->persist_synced_index+1;
    entry_count=start_idx<=last_idx?(int)(last_idx-start_idx+1):0;
    if(entry_count>0&&raft_persist_entry_ensure(r,entry_count)<0){
      /* OOM: defer persist to next round.  Clear ALL persist fields
         including snapshot_dirty so the caller sees a clean slate. */
      ready->persist_needed=0;
      ready->persist.snapshot_dirty=0;
      ready->persist.log_entries=0;
      ready->persist.log_entry_count=0;
    }else{
      for(idx=start_idx,ei=0;idx<=last_idx;idx++,ei++){
        raft_persist_entry *entry=&r->persist_entry_buf[ei];
        off=idx-r->log.last_included_index-1;
        ci=(int)(off>>r->log.chunk_bits);
        co=(int)(off&r->log.chunk_mask);
        memset(entry,0,sizeof(*entry));
        entry->index=idx;
        entry->term=r->log.chunks[ci].terms[co];
        entry->kind=(int)r->log.chunks[ci].kinds[co];
        data_off=r->log.chunks[ci].data_offsets[co];
        if(idx<last_idx){
          int nci=ci,nco=co+1;
          if(nco>=r->log.chunk_size){
            nci++;
            nco=0;
          }
          next_off=r->log.chunks[nci].data_offsets[nco];
        }else next_off=(raft_i64)r->log.data_size;
        if(next_off>data_off){
          entry->data=r->log.data+data_off;
          entry->data_size=(unsigned int)(next_off-data_off);
        }
        if(entry->kind==RAFT_ENTRY_CONFIG&&r->log.chunks[ci].cfg_old){
          entry->cfg_old=r->log.chunks[ci].cfg_old[co];
          entry->cfg_new=r->log.chunks[ci].cfg_new[co];
          if(r->log.chunks[ci].cfg_learners) entry->cfg_learners=r->log.chunks[ci].cfg_learners[co];
        }
      }
      ready->persist.log_entries=r->persist_entry_buf;
      ready->persist.log_entry_count=entry_count;
      r->persist_needed=0;
    }
  }
  /* snapshot_install: blocked while persist is deferred (ensure OOM above).
     Once persist succeeds in a future round, install proceeds. */
  if(r->snapshot_install_pending&&!r->persist_needed){
    ready->snapshot_install_needed=1;
    ready->snapshot_last_index=r->snapshot_recv.last_index;
    ready->snapshot_last_term=r->snapshot_recv.last_term;
    ready->snapshot_size=r->snapshot_recv.size;
  }
  ready->commit_index=r->commit_index;
  ready->is_leader=(r->state==RAFT_LEADER);
  ready->leader_id=ldr_id;
  ready->leader_change=r->ready_leader_change;
  /* per-peer liveness (leader only): expose match/next/missed for the Sec 4.4 replacement policy */
  if(r->state==RAFT_LEADER){
    if(raft_peer_health_ensure(r,r->peer_count)<0){
      ready->peer_health=0;
      ready->peer_health_count=0;
    }else{
      for(i=0;i<r->peer_count;i++){
        r->peer_health_buf[i].id=r->peers[i].id;
        r->peer_health_buf[i].match_index=r->peers[i].match_index;
        r->peer_health_buf[i].next_index=r->peers[i].next_index;
        r->peer_health_buf[i].missed_rounds=r->peers[i].missed_rounds;
      }
      ready->peer_health=r->peer_health_buf;
      ready->peer_health_count=r->peer_count;
    }
  }else{
    ready->peer_health=0;
    ready->peer_health_count=0;
  }
  return ready->has_work?1:0;
}
RAFT_DEF void raft_stop(raft_ctx *r){
  if(!r) return;
  if(r->phase==RAFT_PHASE_READY||r->phase==RAFT_PHASE_RUNNING) r->phase=RAFT_PHASE_STOPPING;
}
RAFT_DEF int raft_should_stop(const raft_ctx *r){
  return r!=0&&r->phase>=RAFT_PHASE_STOPPING;
}
static int raft_recv_request_vote(raft_ctx *r,int from,const raft_request_vote *rpc){
  raft_i64 last_idx,last_term;
  int log_ok;
  raft_peer_message *m;
  if(!r||!rpc) return -1;
  if(!raft_phase_ge(r,RAFT_PHASE_READY)) return -1;
  if(!raft_id_valid(from)||!raft_id_valid(rpc->candidate_id)) return 0;
  if(rpc->term>=RAFT_TERM_MAX+1) return 0; /* cap: term >= INT64_MAX is out of range */
  /* pre-vote: check the leader-contact grace period BEFORE any term update, so
     a disruptive pre-vote cannot bump the receiver's term ($4.2.3) */
  if(rpc->pre_vote&&r->leader_contact_elapsed<r->cfg.election_min_ms) return 0;
  if(rpc->term>r->current_term) raft_step_down(r,rpc->term,0);
  if(rpc->term<r->current_term) return 0;
  /* voted_for only restricts real votes, not pre-votes ($4.2.3) */
  if(!rpc->pre_vote&&r->voted_for!=0&&r->voted_for!=rpc->candidate_id) return 0;
  /* Sec. 4.2.4: votes are granted solely on log up-to-date-ness (plus the $4.2.3
     heartbeat grace period above); a removed server may still be elected if
     its log is current, so no candidate-membership gate is applied here. */
  last_idx=raft_log_last_index(&r->log);
  last_term=raft_log_term_at(&r->log,last_idx);
  log_ok=(rpc->last_log_term>last_term)||(rpc->last_log_term==last_term&&rpc->last_log_index>=last_idx);
  if(!log_ok) return 0;
  if(rpc->pre_vote){
    /* pre-vote: grant without persisting vote */
        if(raft_msg_ensure(r,r->msg_count+1)<0) return -1;
    m=&r->msg_buf[r->msg_count];
    m->type=RAFT_MSG_REQUEST_VOTE_RESULT;
    m->from=r->cfg.id;
    m->to=from;
    m->term=r->current_term;
    m->request_vote_result.term=r->current_term;
    m->request_vote_result.vote_granted=1;
    m->request_vote_result.pre_vote=1;
    r->msg_count++;
    return 0;
  }
  r->voted_for=rpc->candidate_id;
  r->election_elapsed=0;
  r->persist_needed=1;
  /* defer vote response until voted_for is persisted */
  r->deferred_vote_response=1;
  r->deferred_vote_to=from;
  r->deferred_vote_gen=r->persist_gen+1;
  return 0;
}
static int raft_recv_request_vote_result(raft_ctx *r,int from,const raft_request_vote_result *result){
  raft_peer *pr;
  int s_old,s_new;
  if(!r||!result) return -1;
  if(!raft_id_valid(from)) return 0;
  if(!r->in_pre_vote&&r->state!=RAFT_CANDIDATE) return 0;
  if(result->term>r->current_term){
    raft_step_down(r,result->term,0);
    return 0;
  }
  if(result->term<r->current_term) return 0;
  pr=raft_find_peer(r,from);
  if(!pr) return 0;
  if(result->pre_vote){
    /* Only meaningful during a live pre-vote round: a candidate that has
       already escalated (in_pre_vote=0) must ignore a late ack from the
       finished round, and a plain follower never requested a pre-vote. */
    if(!r->in_pre_vote) return 0;
    if(result->vote_granted) pr->pre_vote_acked=1;
    if(r->config_joint){
      s_old=raft_mask_has(raft_set_view(&r->config_old),r->cfg.id);
      s_new=raft_mask_has(raft_set_view(&r->config_new),r->cfg.id);
      /* Sec. 4.3: a candidate may be in C_old only (being removed) and still
         escalate if it holds a pre-vote majority of BOTH C_old and C_new -
         s_old/s_new are the candidate's own (self) votes in each config. */
      if(s_old+raft_count_votes(r,1,1)>=raft_quorum_size_of(r,1)&&s_new+raft_count_votes(r,0,1)>=raft_quorum_size_of(r,0)){
        r->in_pre_vote=0;
        raft_become_candidate(r);
      }
    }else{
      int si=raft_mask_has(raft_set_view(&r->config_new),r->cfg.id);
      if(si+raft_count_votes(r,0,1)>=raft_quorum_size_of(r,0)){
        r->in_pre_vote=0;
        raft_become_candidate(r);
      }
    }
    return 0;
  }
  /* A REAL vote result is only meaningful while this node is a CANDIDATE
     awaiting real votes.  A follower - even one re-running a pre-vote round
     (in_pre_vote=1) - must ignore a stale real-vote result from its own
     earlier candidacy: the deferred-vote-response delay can deliver it after
     the node has stepped down and granted its vote elsewhere, and honoring it
     would re-elect the node on a tally it no longer holds - two leaders in
     one term (seed 8063). */
  if(r->state!=RAFT_CANDIDATE) return 0;
  if(result->vote_granted) pr->vote=1;
  if(r->config_joint){
    s_old=raft_mask_has(raft_set_view(&r->config_old),r->cfg.id);
    s_new=raft_mask_has(raft_set_view(&r->config_new),r->cfg.id);
    /* Sec. 4.3: a candidate in C_old only (being removed) may still WIN a joint
       election when it holds a majority of BOTH C_old and C_new.  Requiring
       s_new would deadlock when the joint entry only reached the C_old voters
       and the sole C_new voter is still behind (it cannot be elected and the
       C_old holders are barred from leading). */
    if(s_old+raft_count_votes(r,1,0)>=raft_quorum_size_of(r,1)&&s_new+raft_count_votes(r,0,0)>=raft_quorum_size_of(r,0)) raft_become_leader(r);
  }else{
    int si=raft_mask_has(raft_set_view(&r->config_new),r->cfg.id);
    if(si+raft_count_votes(r,0,0)>=raft_quorum_size_of(r,0)) raft_become_leader(r);
  }
  return 0;
}
static int raft_recv_append_entries(raft_ctx *r,int from,const raft_append_entries *rpc){
  raft_i64 last_idx,pt,last,eidx,eterm,reject_hint=0,cap;
  int i,kind,log_changed=0,append_oom=0;
  raft_mask cm_old,cm_new,cm_learners;
  raft_peer_message *rm;
  const void *edata;
  unsigned int esize;
  const unsigned char *ecursor;
  if(!r||!rpc) return -1;
  if(!raft_phase_ge(r,RAFT_PHASE_READY)) return -1;
  if(!raft_id_valid(from)||!raft_id_valid(rpc->leader_id)) return 0;
  if(rpc->term>=RAFT_TERM_MAX+1) return 0; /* cap: term >= INT64_MAX is out of range */
  if(rpc->term>r->current_term) raft_step_down(r,rpc->term,rpc->leader_id);
  if(rpc->term<r->current_term) return 0;
  /* Figure 2 converts a server to follower only on a HIGHER term; an equal-term
     AppendEntries from a peer claiming leadership is anomalous (two leaders in
     one term), so a leader ignores it instead of stepping down. */
  if(r->state==RAFT_LEADER) return 0;
  /* Sec. 3.4: an equal-term AE reverts a candidate to follower, but voted_for is
     NOT cleared here - the candidate already voted for itself this term and
     must not cast a second vote in the same term (Sec. 3.4 one vote per term,
     Sec. 3.8 persist voted_for).  voted_for is cleared only by raft_step_down on a
     term change (symmetric with raft_recv_install_snapshot). */
  /* A valid AppendEntries from a current leader aborts any in-progress pre-vote,
     even for a follower ($4.2.3 / $9.6): otherwise late pre-vote acks could
     still escalate this node into an unnecessary real election. */
  r->in_pre_vote=0;
  if(r->leader_id!=rpc->leader_id) r->ready_leader_change=1;
  r->leader_id=rpc->leader_id;
  r->leader_contact_elapsed=0;
  r->election_elapsed=0;
  if(r->state!=RAFT_FOLLOWER){
    r->state=RAFT_FOLLOWER;
    r->ready_leader_change=1;
  }
  if(rpc->entry_count<0) return -1;
  last_idx=raft_log_last_index(&r->log);
  if(rpc->prev_log_index>0){
    if(rpc->prev_log_index>last_idx) goto reply_fail;
    if(rpc->prev_log_index<r->log.last_included_index){
      /* The requested prev index is inside our COMPACTED prefix: we cannot
         verify its term (raft_log_term_at returns last_included_term for ANY
         index <= LII, so the leader's prev_log_term would be compared against
         the boundary entry's term, not the entry at prev_log_index).  Echoing
         prev_log_index would push the leader's next_index to/at its own
         snapshot boundary, making it stream a snapshot we then IGNORE as stale
         (snapshot_last_index < our LII) when our snapshot is AHEAD of the
         leader's - a permanent replication deadlock.  Ask the leader to resume
         at LII+1 instead: if it holds those entries it sends them directly,
         otherwise it re-snapshots from the correct point. */
      reject_hint=r->log.last_included_index+1;
      goto reply_fail;
    }
    pt=raft_log_term_at(&r->log,rpc->prev_log_index);
    if(pt!=rpc->prev_log_term) goto reply_fail;
  }
  ecursor=(const unsigned char *)rpc->entry_data;
  for(i=0;i<rpc->entry_count;i++){
    eidx=rpc->prev_log_index+1+(raft_i64)i;
    eterm=rpc->entry_terms?rpc->entry_terms[i]:rpc->term;
    kind=RAFT_ENTRY_COMMAND;
    if(rpc->entry_kinds) kind=(int)rpc->entry_kinds[i];
    if(kind<0||kind>2) return -1;
    if(rpc->entry_terms&&rpc->entry_terms[i]>=RAFT_TERM_MAX+1) return -1; /* cap: entry term >= INT64_MAX is out of range */
    cm_old=raft_mask_zero();
    cm_new=raft_mask_zero();
    cm_learners=raft_mask_zero();
    if(rpc->entry_cfg_old) cm_old=rpc->entry_cfg_old[i];
    if(rpc->entry_cfg_new) cm_new=rpc->entry_cfg_new[i];
    if(rpc->entry_cfg_learners) cm_learners=rpc->entry_cfg_learners[i];
    edata=0;
    esize=0;
    if(ecursor&&rpc->entry_data_sizes){
      esize=rpc->entry_data_sizes[i];
      if(esize>0) edata=ecursor;
      ecursor+=esize;
    }
    /* The follower already holds this entry with the SAME term (the leader re-sent
       it, or a reordered AE caught up): keep it and skip.  Do NOT truncate here -
       a delayed heartbeat (entry_count==0) or a re-sent entry must never remove
       entries the follower already holds (log-matching safety). */
    if(eidx<=last_idx&&eidx>r->log.last_included_index&&raft_log_term_at(&r->log,eidx)==eterm) continue;
    /* Conflict at eidx (or a gap past our tip): delete [eidx .. tip] and append
       this entry and everything after it. */
    if(eidx<=last_idx){
      if(eidx-1<r->log.last_included_index){
        /* The leader wants to (over)write an entry at or before our compacted
           prefix: we cannot truncate before last_included_index.  Reject with a
           hint to skip past the snapshot (last_included_index+1); without it the
           leader loops on prev_log_index==0 and a stale same-index entry can
           survive to be committed (log-matching violation). */
        reject_hint=r->log.last_included_index+1;
        goto reply_fail;
      }
      raft_log_cut(r,eidx-1);
      log_changed=1;
    }
    for(;;){
      if(raft_log_append(&r->log,eterm,kind,edata,esize,cm_old,cm_new,cm_learners)<0){
        append_oom=1;
        goto append_done;
      }
      i++;
      if(i>=rpc->entry_count) break;
      eterm=rpc->entry_terms?rpc->entry_terms[i]:rpc->term;
      kind=RAFT_ENTRY_COMMAND;
      if(rpc->entry_kinds) kind=(int)rpc->entry_kinds[i];
      if(rpc->entry_terms&&rpc->entry_terms[i]>=RAFT_TERM_MAX+1) return -1;
      cm_old=raft_mask_zero();
      cm_new=raft_mask_zero();
      cm_learners=raft_mask_zero();
      if(rpc->entry_cfg_old) cm_old=rpc->entry_cfg_old[i];
      if(rpc->entry_cfg_new) cm_new=rpc->entry_cfg_new[i];
      if(rpc->entry_cfg_learners) cm_learners=rpc->entry_cfg_learners[i];
      edata=0;
      esize=0;
      if(ecursor&&rpc->entry_data_sizes){
        esize=rpc->entry_data_sizes[i];
        if(esize>0) edata=ecursor;
        ecursor+=esize;
      }
    }
    break;
  }
append_done:
  /* Reapply the latest config only when the log changed (entries appended or
     prefix truncated) OR a previous apply failed (config_apply_stale); a pure
     heartbeat with a healthy config leaves it unchanged, so re-scanning would
     only churn allocations.  This must ALSO run when an append OOM'd partway:
     the entries already appended may carry config entries (e.g. the final
     C_new that removes this server), and skipping the apply here would leave
     the live config stale - the node could later campaign under a config its
     own log already supersedes (leader-completeness violation). */
  if((rpc->entry_count>0||log_changed||r->config_apply_stale)&&raft_apply_config_from_log(r)<0) return -1;
  if(append_oom) return -1;
  if(rpc->leader_commit>r->commit_index){
    /* Sec. 5.4.2 / Figure 2: a follower may commit only up to the last entry THIS message
       established (prev_log_index + entry_count) - the tail beyond it is not validated by
       this RPC, so committing it can apply a divergent suffix (e.g. when a leader degraded
       to an empty heartbeat on allocation failure, or while it is still walking next_index
       back).  Capping at the local log end instead was wrong in exactly those windows. */
    cap=rpc->prev_log_index+(raft_i64)rpc->entry_count;
    last=raft_log_last_index(&r->log);
    if(cap>last) cap=last;
    if(rpc->leader_commit<cap) cap=rpc->leader_commit;
    if(cap>r->commit_index) r->commit_index=cap;
    /* $3.8: commit_index is volatile (safe to reinit on restart) and is not
       carried in raft_persist, so advancing it needs no persistence.  Setting
       persist_needed here forced a no-op persist cycle and deferred the
       heartbeat ACK, adding a disk round-trip to every committed entry. */
  }
  if(rpc->entry_count>0||log_changed) r->persist_needed=1;
  /* heartbeat: always acknowledge so leader can track quorum contact */
  if(rpc->entry_count==0&&!r->persist_needed){
    raft_peer_message *rm2;
    if(raft_msg_ensure(r,r->msg_count+1)==0){
      rm2=&r->msg_buf[r->msg_count];
      rm2->type=RAFT_MSG_APPEND_RESULT;
      rm2->from=r->cfg.id;
      rm2->to=r->leader_id;
      rm2->term=r->current_term;
      rm2->append_entries_result.term=r->current_term;
      rm2->append_entries_result.success=1;
      rm2->append_entries_result.rejected=0;
      /* Report what AppendEntries actually confirmed - the durable range up to
         prev_log_index (paper TLA+ msuccess: mmatchIndex = mprevLogIndex +
         Len(mentries) = prev here).  Reporting the log tip let the leader count
         unverified leftovers as replicated; the only reachable producer of
         "count==0 and prev < tip" is the entry-list OOM downgrade, and OOM is
         injectable, so this is not a theoretical path.  Clamped by the durable
         frontier (match index means "durably written to this server's disk",
         dissertation 10.2.1) and, for safety, by the current log tip. */
      rm2->append_entries_result.last_log_index=r->durable_confirm<rpc->prev_log_index?r->durable_confirm:rpc->prev_log_index;
      rm2->append_entries_result.conflict_term=0;
      rm2->append_entries_result.conflict_first_index=0;
      rm2->append_entries_result.read_context=rpc->read_context;
      r->msg_count++;
    }
    return 0;
  }
  if(r->persist_needed){
    if(!r->deferred_append_response){
      /* New latch.  The generation is taken ONCE, not re-armed on every
         AppendEntries: the release site runs at the top of an advance, i.e.
         before the caller's persist of that round completes, so a per-round arm
         left the gate permanently one generation short whenever entry-bearing
         AppendEntries arrived as fast as persists completed.  Sec. 3.8 still
         holds: no ACK before the caller has persisted what this AppendEntries
         changed (term/votedFor, and the appended entries). */
      r->deferred_append_gen=r->persist_gen+1;
      r->deferred_append_last_index=-1; /* nothing reported yet */
      r->deferred_append_confirmed=0;
    }
    /* The ACK may advertise at most what AppendEntries has verified so far:
       prev_log_index + entry_count (paper TLA+ msuccess).  Reporting the log tip
       instead counted the follower's own unverified leftovers as replicated. */
    if(rpc->prev_log_index+rpc->entry_count>r->deferred_append_confirmed)
      r->deferred_append_confirmed=rpc->prev_log_index+rpc->entry_count;
    r->deferred_append_response=1;
    r->deferred_append_to=r->leader_id;
    r->deferred_append_read_context=rpc->read_context;
  }
  return 0;
reply_fail:
  last=raft_log_last_index(&r->log);
  /* Sec. 3.8 ("each server persists its current term and vote ... to prevent the server from ... replacing
     log entries from a newer leader with those from a deposed leader"): a rejection carries our current
     term, and the receiving leader steps down on a higher one - so replying before the term is durable lets
     an unpersisted term depose a leader and then vanish on a restart.  The vote path and the successful-AE
     ACK are already gated this way; this path was not (issue #13).  Defer through the same latch while a
     persist is outstanding; reply immediately when nothing is pending (liveness unchanged). */
  /* Broad on purpose: `persist_needed` covers the term/vote change this reply advertises, and waiting one
     persist round keeps the leader's view consistent.  Narrowing it to the term's own generation passed the
     unit suite but broke liveness (raft_cluster_fuzz seed 869: a lagging match index stalled the leader's
     read barrier), so the wider gate is the one that holds on both. */
  if(r->persist_needed||r->persist_gen<r->term_dirty_gen){
    raft_i64 hint=reject_hint>0?reject_hint:(rpc->prev_log_index>last?last+1:rpc->prev_log_index);
    r->deferred_reject_gen=r->persist_gen+1;   /* released once the caller reports this persist */
    r->deferred_reject_last_index=last;        /* the immediate path reports the log tip at rejection time */
    r->deferred_reject_to=r->leader_id;
    r->deferred_reject_read_context=rpc->read_context;
    r->deferred_reject_hint=hint;
    /* Keep the conflict hints: they are what lets the leader skip a whole conflicting term, and a
       deferral must not quietly drop them. */
    r->deferred_reject_conflict_term=0;
    r->deferred_reject_conflict_first_index=0;
    if(rpc->prev_log_index>0&&rpc->prev_log_index>r->log.last_included_index&&rpc->prev_log_index<=last){
      raft_i64 ct=raft_log_term_at(&r->log,rpc->prev_log_index);
      raft_i64 cfi=rpc->prev_log_index;
      if(ct>0){
        while(cfi>r->log.last_included_index+1&&raft_log_term_at(&r->log,cfi-1)==ct) cfi--;
        r->deferred_reject_conflict_term=ct;
        r->deferred_reject_conflict_first_index=cfi;
      }
    }
    r->deferred_reject_pending=1;
    return 0;
  }
  if(raft_msg_ensure(r,r->msg_count+1)<0) return -1;
  rm=&r->msg_buf[r->msg_count];
  rm->type=RAFT_MSG_APPEND_RESULT;
  rm->from=r->cfg.id;
  rm->to=r->leader_id;
  rm->term=r->current_term;
  rm->append_entries_result.term=r->current_term;
  rm->append_entries_result.success=0;
  rm->append_entries_result.rejected=reject_hint>0?reject_hint:(rpc->prev_log_index>last?last+1:rpc->prev_log_index);
  rm->append_entries_result.last_log_index=last;
  rm->append_entries_result.conflict_term=0;
  rm->append_entries_result.conflict_first_index=0;
  rm->append_entries_result.read_context=rpc->read_context;
  if(rpc->prev_log_index>0&&rpc->prev_log_index>r->log.last_included_index&&rpc->prev_log_index<=last){
    raft_i64 ct=raft_log_term_at(&r->log,rpc->prev_log_index);
    raft_i64 cfi=rpc->prev_log_index;
    if(ct>0){
      while(cfi>r->log.last_included_index+1&&raft_log_term_at(&r->log,cfi-1)==ct) cfi--;
      rm->append_entries_result.conflict_term=ct;
      rm->append_entries_result.conflict_first_index=cfi;
    }
  }
  r->msg_count++;
  return 0;
}
static int raft_recv_append_entries_result(raft_ctx *r,int from,const raft_append_entries_result *result){
  raft_peer *pr;
  if(!r||!result) return -1;
  if(!raft_id_valid(from)) return 0;
  if(r->state!=RAFT_LEADER) return 0;
  if(result->term>=RAFT_TERM_MAX+1) return 0; /* cap: term >= INT64_MAX is out of range */
  if(result->term>r->current_term){
    raft_step_down(r,result->term,0);
    return 0;
  }
  if(result->term<r->current_term) return 0;
  pr=raft_find_peer(r,from);
  if(!pr) return 0;
  /* ReadIndex quorum counts a SUCCESSFUL in-term ACK -- the peer actually
     accepted this heartbeat's entries (its match_index advanced).  A rejection
     proves contact (see quorum_acked below) but NOT that this peer holds the
     leader's commit prefix, so it must not satisfy a linearizable-read barrier
     (dissertation Sec. 6.4 step 3: "acknowledgments from a majority"). */
  if(result->success&&result->read_context==r->read_context&&(r->read_index_pending_count>0||r->read_barrier_count>0)&&r->read_index_heartbeat_sent&&!pr->read_index_acked&&!pr->is_learner) pr->read_index_acked=1;
  /* checkQuorum counts CONTACT, not progress (Sec. 10.1.1/Sec. 4.2.4): any valid
     in-term response - success OR rejection - proves the follower is
     reachable, so the leader must not step down while converging a divergent
     log.  (Symmetric with the ReadIndex ack above, which also counts any
     response.) */
  if(!pr->is_learner){
    pr->quorum_acked=1;
    pr->missed_rounds=0;
  }
  if(result->success){
    /* Monotonicity guard: a stale/duplicate success ACK (last_log_index below
       match_index) must not regress progress, or the leader would re-send
       already-acked entries.  Clamp to the leader's OWN log tip: a follower
       may report a last_log_index BEYOND the leader's log (stale uncommitted
       entries from its own prior leadership that the leader has not seen), and
       counting those would let the leader "commit" entries the follower never
       actually replicated (leader-completeness violation). */
    raft_i64 leader_last=raft_log_last_index(&r->log);
    raft_i64 new_match=result->last_log_index<leader_last?result->last_log_index:leader_last;
    if(new_match>=pr->match_index){
      pr->match_index=new_match;
      pr->next_index=new_match+1;
    }
  }else{
    if(result->conflict_term>0&&result->conflict_first_index>0){
      /* Paper Sec. 5.3: the follower reports the term of its conflicting entry and
         the first index it stores for that term.  If the leader HAS entries of
         that term it resumes just past the LAST of them; otherwise it skips the
         follower's whole run for that term (conflict_first_index).  Terms are
         non-decreasing in a log, so a binary search finds the boundary, and
         unlike the previous forward scan from conflict_first_index it also works
         when the leader's entries of that term END BELOW that index - the common
         divergence case, where the scan immediately missed and fell through to
         "back off by one entry per round", so a log that diverged by N entries
         needed N round trips and no election survives long enough to finish. */
      raft_i64 t=result->conflict_term;
      raft_i64 lo=r->log.last_included_index+1;
      raft_i64 hi=raft_log_last_index(&r->log);
      raft_i64 a=lo,b=hi+1,mid,nx;
      while(a<b){
        mid=a+(b-a)/2;
        if(raft_log_term_at(&r->log,mid)>t) b=mid; else a=mid+1;
      }
      /* a = first index whose term exceeds t (hi+1 when the whole log is <= t) */
      if(a>lo&&raft_log_term_at(&r->log,a-1)==t) nx=a;   /* resume past our last entry of that term */
      else nx=result->conflict_first_index;              /* we hold none of that term */
      /* Deliberately NOT clamped up to lo: when the divergence sits at or below
         our snapshot boundary the follower cannot be repaired with log entries at
         all (we no longer hold that prefix), so next_index must stay <= the
         boundary and let raft_queue_append_entries route the peer to the
         InstallSnapshot stream (§7).  Clamping it to lo made the leader retry an
         AppendEntries whose prev term can never match, forever, and a joint
         config that needed that follower could then never commit
         (raft_cluster_fuzz seed 52). */
      if(nx>hi+1) nx=hi+1;
      pr->next_index=nx;
    }else if(result->rejected>0) pr->next_index=result->rejected;
    else if(pr->next_index>1) pr->next_index--;
  }
  if(!pr->is_learner) raft_advance_commit(r);
  return 0;
}
static int raft_recv_install_snapshot(raft_ctx *r,int from,const raft_install_snapshot *rpc){
  raft_peer *pr;
  if(!r||!rpc) return -1;
  if(!raft_phase_ge(r,RAFT_PHASE_READY)) return -1;
  if(!raft_id_valid(from)||!raft_id_valid(rpc->leader_id)) return 0;
  if(rpc->term>=RAFT_TERM_MAX+1) return 0; /* cap: term >= INT64_MAX is out of range */
  /* A snapshot we cannot accept - our term is HIGHER, or the snapshot's last
     index is behind our own compaction (Sec. 5.1/Sec. 7.4) - must not be dropped
     silently.  Tell the leader our term and ACTUAL last_included_index: if our
     term is higher it steps down (Sec. 3.3), otherwise it advances next_index past
     its stale snapshot and resumes AppendEntries at our boundary.  Silently
     ignoring would leave the leader re-streaming a snapshot it can never
     install, and - because a pending stream counts as checkQuorum "contact" -
     a removed leader would never step down (liveness deadlock). */
  if(rpc->term<r->current_term||rpc->snapshot_last_index<r->log.last_included_index){
    if(raft_msg_ensure(r,r->msg_count+1)==0){
      raft_peer_message *mr=&r->msg_buf[r->msg_count];
      mr->type=RAFT_MSG_INSTALL_SNAPSHOT_RESULT;
      mr->from=r->cfg.id;
      mr->to=from;
      mr->term=r->current_term;
      mr->install_snapshot_result.term=r->current_term;
      mr->install_snapshot_result.last_included_index=r->log.last_included_index;
      r->msg_count++;
    }
    return 0;
  }
  if(rpc->term>r->current_term) raft_step_down(r,rpc->term,rpc->leader_id);
  /* a leader ignores an equal-term InstallSnapshot from a peer claiming
     leadership (only a HIGHER term steps it down, per Figure 2). */
  if(r->state==RAFT_LEADER) return 0;
  if(r->leader_id!=rpc->leader_id) r->ready_leader_change=1;
  r->leader_id=rpc->leader_id;
  r->leader_contact_elapsed=0;
  r->election_elapsed=0;
  r->in_pre_vote=0; /* a valid leader snapshot aborts any in-progress pre-vote */
  if(r->state!=RAFT_FOLLOWER){
    r->state=RAFT_FOLLOWER;
    r->ready_leader_change=1;
  }
  pr=raft_find_peer(r,from);
  if(pr) pr->match_index=rpc->snapshot_last_index;
  /* new snapshot: reset tracking.  Compare BOTH last_index and byte size: a
     replacement snapshot taken at the SAME log index but with a different byte
     size (snapshot format/bytes changed) must also restart the stream, or the
     follower's stale expected_offset rejects every chunk of the new stream. */
  if(r->snapshot_recv.last_index!=rpc->snapshot_last_index||r->snapshot_recv.size!=rpc->snapshot_data_size){
    r->snapshot_recv.last_index=rpc->snapshot_last_index;
    r->snapshot_recv.last_term=rpc->snapshot_last_term;
    r->snapshot_recv.size=rpc->snapshot_data_size;
    r->snapshot_recv_expected_offset=0;
    r->snapshot_install_pending=0;
    raft_set_clear(&r->snapshot_recv.cfg_old);
    raft_set_clear(&r->snapshot_recv.cfg_new);
    raft_set_clear(&r->snapshot_recv.cfg_learners);
    if(raft_mask_any(rpc->snapshot_cfg_old)){
      if(raft_set_assign(&r->snapshot_recv.cfg_old,rpc->snapshot_cfg_old.ids,rpc->snapshot_cfg_old.id_count)<0||raft_set_assign(&r->snapshot_recv.cfg_new,rpc->snapshot_cfg_new.ids,rpc->snapshot_cfg_new.id_count)<0){
        raft_set_clear(&r->snapshot_recv.cfg_old);
        raft_set_clear(&r->snapshot_recv.cfg_new);
        r->snapshot_recv.last_index=0;
        return 0;
      }
    }
    if(raft_mask_any(rpc->snapshot_cfg_learners)){
      if(raft_set_assign(&r->snapshot_recv.cfg_learners,rpc->snapshot_cfg_learners.ids,rpc->snapshot_cfg_learners.id_count)<0){
        raft_set_clear(&r->snapshot_recv.cfg_learners);
        r->snapshot_recv.last_index=0;
        return 0;
      }
    }
  }
  if(rpc->snapshot_offset!=r->snapshot_recv_expected_offset) return 0;
  /* Validate the done boundary BEFORE advancing expected_offset: a rejected
     final chunk must not corrupt the offset so a correct retry is accepted. */
  if(rpc->snapshot_done&&rpc->snapshot_offset+rpc->snapshot_chunk_size!=rpc->snapshot_data_size) return -1;
  r->snapshot_recv_expected_offset=rpc->snapshot_offset+rpc->snapshot_chunk_size;
  if(rpc->snapshot_done){
    /* snapshot reception complete; caller must fsync file, then advance will emit persist+install */
    r->snapshot_recv.last_index=rpc->snapshot_last_index;
    r->snapshot_recv.last_term=rpc->snapshot_last_term;
    r->snapshot_recv.size=rpc->snapshot_data_size;
    r->snapshot_install_pending=1;
    r->persist_needed=1;
  }
  return 0;
}
static int raft_recv_install_snapshot_result(raft_ctx *r,int from,const raft_install_snapshot_result *result){
  raft_peer *pr;
  raft_i64 li;
  if(!r||!result) return -1;
  if(!raft_id_valid(from)) return 0;
  if(r->state!=RAFT_LEADER) return 0;
  if(result->term>=RAFT_TERM_MAX+1) return 0; /* cap: term >= INT64_MAX is out of range */
  if(result->term>r->current_term){
    raft_step_down(r,result->term,0);
    return 0;
  }
  if(result->term<r->current_term) return 0;
  pr=raft_find_peer(r,from);
  if(!pr) return 0;
  /* The installed snapshot's last_included_index is authoritative: the follower
     reports the boundary it ACTUALLY installed, so a stale pending stream
     (leader's snapshot advanced, or an older delayed stream completed) cannot
     inflate match_index above the follower's real log prefix ($7.4). */
  li=result->last_included_index;
  if(li<=0) li=pr->pending_snapshot_last_index; /* legacy result: fallback */
  if(li>=pr->match_index){
    pr->match_index=li;
    pr->next_index=li+1;
  }
  pr->pending_snapshot_offset=0;
  if(!pr->is_learner){
    /* A completed snapshot install is leader contact too: count it toward
       checkQuorum (mirrors raft_recv_append_entries_result), so a leader whose
       quorum member is only caught up via snapshot does not step down. */
    pr->quorum_acked=1;
    pr->missed_rounds=0;
    raft_advance_commit(r);
  }
  return 0;
}
static void raft_read_round_restart(raft_ctx *r){
  int pi;
  r->read_context++;
  if(!r->read_context) r->read_context=1;
  for(pi=0;pi<r->peer_count;pi++) r->peers[pi].read_index_acked=0;
  r->read_index_heartbeat_sent=0;
}
static int raft_recv_read_index(raft_ctx *r,int from,const raft_read_index_req *rpc){
  if(!r||!rpc) return -1;
  if(!raft_phase_ge(r,RAFT_PHASE_READY)) return -1;
  if(!raft_id_valid(from)) return 0;
  if(r->state!=RAFT_LEADER) return 0;
  if(rpc->term<r->current_term) return 0;
  if(rpc->term>r->current_term){
    if(rpc->term>=RAFT_TERM_MAX+1) return 0; /* cap: INT64_MAX-1 */
    raft_step_down(r,rpc->term,0);
    return 0;
  }
  /* Sec. 6.4 / Sec. 4.2.4: a spurious removed leader must not serve a follower's
     ReadIndex - its commit_index lags the real committed prefix and would leak
     a stale read.  Drop the request; the follower's barrier fails via its own
     timeout and the client retries.  (C_old-only joint leaders may serve.) */
  if(!raft_mask_has(raft_set_view(&r->config_old),r->cfg.id)&&!raft_mask_has(raft_set_view(&r->config_new),r->cfg.id)) return 0;
  if(raft_read_index_pending_ensure(r,r->read_index_pending_count+1)<0) return 0;
  r->read_index_pending[r->read_index_pending_count].from=from;
  /* Sec. 6.4 step 3: the read index is captured at FLUSH time (after the fresh
     heartbeat quorum re-confirms leadership), NOT at receipt.  A receipt-time
     capture goes stale when the flush is delayed: a ReadIndex processed while
     commit was low (e.g. a duplicated request) resolves the follower's batch
     with a stale index after commit has advanced (seed 1459).  Storing a
     sentinel 0 always defers the capture to the flush. */
  r->read_index_pending[r->read_index_pending_count].index=0;
  r->read_index_pending[r->read_index_pending_count].context=rpc->context;
  r->read_index_pending_count++;
  if(r->read_index_pending_count==1){
    raft_read_round_restart(r);
  }
  r->heartbeat_elapsed=r->cfg.heartbeat_ms;
  return 0;
}
static int raft_recv_read_index_result(raft_ctx *r,int from,const raft_read_index_result *result){
  int fri,n;
  if(!r||!result) return -1;
  if(!raft_phase_ge(r,RAFT_PHASE_READY)) return -1;
  if(!raft_id_valid(from)) return 0;
  if(r->follower_read_frozen==0) return 0;
  /* Only the answer to the round currently in flight may resolve barriers.
     A duplicated or delayed result from an earlier round carries an older
     read index; accepting it lets a fresh barrier (whose submission commit
     frontier is already higher) resolve READY at a stale applied index.  The
     dropped barriers stay at the front of the queue and are resolved by the
     next round's result - or failed by the frozen-phase timeout. */
  if(result->context!=r->follower_read_context) return 0;
  if(result->term>=RAFT_TERM_MAX+1) return 0; /* cap: term >= INT64_MAX is out of range */
  if(result->term>r->current_term){
    raft_step_down(r,result->term,0);
    return 0;
  }
  if(result->term<r->current_term) return 0;
  n=r->follower_read_frozen;
  if(raft_read_barrier_ensure(r,r->read_barrier_count+n)<0) return -1;
  for(fri=0;fri<n;fri++){
    r->read_barriers[r->read_barrier_count].cookie=r->follower_read_pending[fri];
    r->read_barriers[r->read_barrier_count].target_index=result->read_index;
    r->read_barriers[r->read_barrier_count].needs_gen=0;
    r->read_barrier_count++;
  }
  /* drop the resolved frozen batch from the front of the queue */
  if(n<r->follower_read_pending_count) memmove(r->follower_read_pending,r->follower_read_pending+n,(unsigned int)(r->follower_read_pending_count-n)*sizeof(void*));
  r->follower_read_pending_count-=n;
  r->follower_read_frozen=0;
  r->follower_read_elapsed=0;
  /* barriers queued during the flight start the next round at next advance */
  if(r->follower_read_pending_count>0) r->follower_read_waiting=1;
  return 0;
}
static int raft_recv_timeout_now(raft_ctx *r,int from,const raft_timeout_now *rpc){
  if(!r||!rpc) return -1;
  if(!raft_id_valid(from)) return 0;
  if(!raft_phase_ge(r,RAFT_PHASE_READY)) return -1;
  if(rpc->term>=RAFT_TERM_MAX+1) return 0; /* cap: term >= INT64_MAX is out of range */
  if(rpc->term<r->current_term) return 0;
  if(rpc->term>r->current_term) raft_step_down(r,rpc->term,rpc->leader_id);
  /* Only a follower is nudged into an immediate election ($3.10): a candidate
     is already campaigning, and a leader has already stepped down above. */
  /* $4.2.3: only the CURRENT leader holds "permission to disrupt" - a
     TimeoutNow from any other peer is ignored (defense in depth: a compromised
     peer must not be able to force an immediate election). */
  if(r->state==RAFT_FOLLOWER&&rpc->leader_id==r->leader_id){
    raft_i64 li=raft_log_last_index(&r->log);
    raft_i64 lt=raft_log_term_at(&r->log,li);
    /* $3.10: the leader syncs the target's log BEFORE sending TimeoutNow, so a
       transfer must only nudge an immediate election when the target's log tip
       exactly matches the leader's advertised tip (index AND term). A lagging
       target would otherwise campaign on a stale log; the voter-side $5.4.1
       freshness check would reject its pre-vote, so this guard merely keeps
       the transfer deterministic and avoids a wasted pre-vote round. */
    /* The target starts a REAL election (incrementing its term and becoming
        a candidate), NOT the follower pre-vote path: a pre-vote would be
        suppressed by the other followers' heartbeat grace period (Sec. 4.2.3),
        so the transfer would never complete. */
    if(li==rpc->last_log_index&&lt==rpc->last_log_term) return raft_become_candidate(r);
  }
  return 0;
}
/* Peer messages are accepted from READY onward (a fresh node may receive
   AppendEntries/snapshots to catch up before serving clients), whereas
   client requests require RUNNING (raft_submit & friends gate on it). */
RAFT_DEF int raft_recvfrom_peer(raft_ctx *r,const raft_peer_message *msg){
  if(!r||!msg) return -1;
  if(r->phase==RAFT_PHASE_STOPPED) return 0;
  switch(msg->type){
    case RAFT_MSG_REQUEST_VOTE: return raft_recv_request_vote(r,msg->from,&msg->request_vote);
    case RAFT_MSG_REQUEST_VOTE_RESULT: return raft_recv_request_vote_result(r,msg->from,&msg->request_vote_result);
    case RAFT_MSG_APPEND: return raft_recv_append_entries(r,msg->from,&msg->append_entries);
    case RAFT_MSG_APPEND_RESULT: return raft_recv_append_entries_result(r,msg->from,&msg->append_entries_result);
    case RAFT_MSG_INSTALL_SNAPSHOT: return raft_recv_install_snapshot(r,msg->from,&msg->install_snapshot);
    case RAFT_MSG_INSTALL_SNAPSHOT_RESULT: return raft_recv_install_snapshot_result(r,msg->from,&msg->install_snapshot_result);
    case RAFT_MSG_READ_INDEX: return raft_recv_read_index(r,msg->from,&msg->read_index_req);
    case RAFT_MSG_READ_INDEX_RESULT: return raft_recv_read_index_result(r,msg->from,&msg->read_index_result);
    case RAFT_MSG_TIMEOUT_NOW: return raft_recv_timeout_now(r,msg->from,&msg->timeout_now);
    default: return -1;
  }
}
static int raft_submit(raft_ctx *r,const raft_command *commands,int count){
  int i;
  raft_i64 base_index;
  if(!r||count<0) return -1;
  if(count<=0) return 0;
  if(!commands) return -1;
  if(r->phase!=RAFT_PHASE_RUNNING) return -1;
  if(r->state!=RAFT_LEADER) return -1;
  if(raft_id_valid(r->transfer_target)) return -1;
  for(i=0;i<count;i++){
    if(!commands[i].cookie) return -1;
    if(commands[i].command_size>0&&!commands[i].command) return -1;
  }
  raft_client_pending_compact(r);
  if(raft_client_pending_ensure(r,r->client_pending_head+r->client_pending_count+count)<0) return -1;
  base_index=raft_log_last_index(&r->log)+1;
  if(raft_log_append_commands(&r->log,r->current_term,commands,count)<0) return -1;
  for(i=0;i<count;i++){
    r->client_pending[r->client_pending_head+r->client_pending_count].cookie=commands[i].cookie;
    r->client_pending[r->client_pending_head+r->client_pending_count].log_index=base_index+(raft_i64)i;
    r->client_pending_count++;
  }
  r->persist_needed=1;
  r->heartbeat_elapsed=r->cfg.heartbeat_ms; /* force AE broadcast next tick ($3.5: replicate immediately) */
  return 0;
}
static int raft_barrier(raft_ctx *r,const void *cookie){
  if(!r||!cookie) return -1;
  if(r->phase!=RAFT_PHASE_RUNNING) return -1;
  if(r->state==RAFT_LEADER){
    if(raft_id_valid(r->transfer_target)) return -1;
    /* Sec. 6.4 / Sec. 4.2.4: a leader that is no longer a voter in EITHER config (a
       spurious removed leader whose commit_index lags the real committed
       prefix) must not serve a read - it would return a STALE read index.
       Reject so the client retries at the real leader.  A C_old-only leader
       during joint is still a voter in C_old and may serve (Sec. 4.3). */
    if(!raft_mask_has(raft_set_view(&r->config_old),r->cfg.id)&&!raft_mask_has(raft_set_view(&r->config_new),r->cfg.id)) return -1;
    if(raft_read_barrier_ensure(r,r->read_barrier_count+1)<0) return -1;
    r->read_barriers[r->read_barrier_count].cookie=cookie;
    r->read_barriers[r->read_barrier_count].target_index=r->commit_index;
    r->read_barriers[r->read_barrier_count].needs_gen=r->read_barrier_gen+1; /* Sec. 6.4: wait for next heartbeat quorum */
    r->read_barrier_count++;
    if(r->read_barrier_count==1){
      /* First barrier of a batch: start a FRESH heartbeat round so the
         generation only advances on acks to THIS round - a delayed stale ACK
         from a peer that has since moved to a higher term must not resolve
         the barrier READY (seed 1460). */
      raft_read_round_restart(r);
    }
    r->heartbeat_elapsed=r->cfg.heartbeat_ms;
    return 0;
  }
  if(r->state==RAFT_FOLLOWER){
    if(!raft_id_valid(r->leader_id)) return -1;
    if(raft_follower_read_ensure(r,r->follower_read_pending_count+1)<0) return -1;
    r->follower_read_pending[r->follower_read_pending_count++]=(void*)cookie;
    /* amortize: a queued ReadIndex is materialized (and the batch frozen) at
       the next advance, so back-to-back barriers share one round */
    if(r->follower_read_frozen==0) r->follower_read_waiting=1;
    return 0;
  }
  return -1;
}
static int raft_add_learner(raft_ctx *r,const void *cookie,int learner_id){
  int *sorted_learners,lc,rc,i=0,j=0;
  if(!r||!cookie) return -1;
  if(r->phase!=RAFT_PHASE_RUNNING) return -1;
  if(r->state!=RAFT_LEADER) return -1;
  if(raft_id_valid(r->transfer_target)) return -1; /* Sec. 3.10 step 1 */
  if(r->config_joint) return -1; /* at most one uncommitted config ($4.1) */
  if(r->config_catchup_active) return -1;
  if(!raft_id_valid(learner_id)) return -1;
  if(raft_mask_has(raft_set_view(&r->config_old),learner_id)) return -1;
  if(raft_is_learner(&r->config_learners,learner_id)) return -1;
  /* only one pending config change at a time ($4.1) */
  raft_config_zombie_clear(r);
  if(r->config_pending&&r->config_index>0) return -1;
  if(r->config_pending&&r->config_index==0) raft_config_pending_clear(r);
  /* build new learner set: sorted insert (learner_id is not already present) */
  lc=r->config_learners.id_count+1;
  sorted_learners=(int*)RAFT_MALLOC(sizeof(int)*(unsigned int)lc);
  if(!sorted_learners) return -1;
  while(i<r->config_learners.id_count&&r->config_learners.ids[i]<learner_id) sorted_learners[j++]=r->config_learners.ids[i++];
  sorted_learners[j++]=learner_id;
  while(i<r->config_learners.id_count) sorted_learners[j++]=r->config_learners.ids[i++];
  /* track pending */
  if(raft_set_copy(&r->config_pending_old,&r->config_old)<0){
    RAFT_FREE(sorted_learners);
    return -1;
  }
  r->config_pending=1;
  r->config_pending_new.ids=0;
  r->config_pending_new.id_count=0;
  if(raft_set_copy(&r->config_pending_new,&r->config_old)<0){
    raft_set_clear(&r->config_pending_old);
    RAFT_FREE(sorted_learners);
    r->config_pending=0;
    return -1;
  }
  r->config_pending_learners.ids=sorted_learners;
  r->config_pending_learners.id_count=lc;
  rc=raft_commit_config_entry(r,cookie);
  if(rc==0){
    /* record the caller's cookie on the (now-created) learner peer so its
       terminal CATCHUP_READY/FAILED result carries the same correlation tag
       as the ADD_LEARNER request (consistent with raft_reconfig) */
    raft_peer *lp=raft_find_peer(r,learner_id);
    if(lp) lp->catchup_cookie=cookie;
  }
  return rc;
}
static int raft_remove_learner(raft_ctx *r,const void *cookie,int learner_id){
  int *sorted_learners=0,lc,i,j=0;
  if(!r||!cookie) return -1;
  if(r->phase!=RAFT_PHASE_RUNNING) return -1;
  if(r->state!=RAFT_LEADER) return -1;
  if(raft_id_valid(r->transfer_target)) return -1; /* Sec. 3.10 step 1 */
  if(r->config_joint) return -1; /* at most one uncommitted config ($4.1) */
  if(r->config_catchup_active) return -1;
  if(!raft_id_valid(learner_id)) return -1;
  if(!raft_is_learner(&r->config_learners,learner_id)) return -1;
  raft_config_zombie_clear(r);
  if(r->config_pending&&r->config_index>0) return -1;
  if(r->config_pending&&r->config_index==0) raft_config_pending_clear(r);
  lc=r->config_learners.id_count-1;
  if(lc>0){
    sorted_learners=(int*)RAFT_MALLOC(sizeof(int)*(unsigned int)lc);
    if(!sorted_learners) return -1;
    for(i=0;i<r->config_learners.id_count;i++){
      if(r->config_learners.ids[i]!=learner_id) sorted_learners[j++]=r->config_learners.ids[i];
    }
  }
  if(raft_set_copy(&r->config_pending_old,&r->config_old)<0){
    if(sorted_learners) RAFT_FREE(sorted_learners);
    return -1;
  }
  r->config_pending=1;
  r->config_pending_new.ids=0;
  r->config_pending_new.id_count=0;
  if(raft_set_copy(&r->config_pending_new,&r->config_old)<0){
    raft_set_clear(&r->config_pending_old);
    if(sorted_learners) RAFT_FREE(sorted_learners);
    r->config_pending=0;
    return -1;
  }
  r->config_pending_learners.ids=sorted_learners;
  r->config_pending_learners.id_count=lc;
  return raft_commit_config_entry(r,cookie);
}
static int raft_transfer(raft_ctx *r,const void *cookie,int target_id){
  raft_peer *tp;
  if(!r||!cookie) return -1;
  if(r->phase!=RAFT_PHASE_RUNNING) return -1;
  if(r->state!=RAFT_LEADER) return -1;
  /* Sec. 3.10 step 1: a transfer is already in progress - reject a second one.
     Without this the second request would OVERWRITE transfer_cookie, leaving
     the first caller with no terminal result (a hung cookie).  Symmetric with
     the transfer_target guards in raft_submit/reconfig/add_learner/remove_learner. */
  if(raft_id_valid(r->transfer_target)) return -1;
  if(!raft_id_valid(target_id)) return -1;
  tp=raft_find_peer(r,target_id);
  if(tp&&tp->is_learner) return -1; /* $4.2.1: a non-voting member cannot lead */
  r->transfer_target=target_id;
  r->transfer_elapsed=0;
  r->transfer_cookie=cookie;
  return 0;
}
RAFT_DEF int raft_recvfrom_client(raft_ctx *r,const raft_client_message *msg){
  if(!r||!msg) return -1;
  switch(msg->type){
    case RAFT_CLIENT_SUBMIT: return raft_submit(r,msg->submit.commands,msg->submit.count);
    case RAFT_CLIENT_BARRIER: return raft_barrier(r,msg->cookie);
    case RAFT_CLIENT_RECONFIG: return raft_reconfig(r,msg->cookie,msg->reconfig.ids,msg->reconfig.id_count);
    case RAFT_CLIENT_ADD_LEARNER: return raft_add_learner(r,msg->cookie,msg->learner.learner_id);
    case RAFT_CLIENT_REMOVE_LEARNER: return raft_remove_learner(r,msg->cookie,msg->learner.learner_id);
    case RAFT_CLIENT_TRANSFER: return raft_transfer(r,msg->cookie,msg->transfer.target_id);
    default: return -1;
  }
}
/* True if any config entry exists at index > applied_index.  The snapshot
   metadata copies config_old/config_new (config-immediate, $4.1), so such an
   entry would make the snapshot's membership reflect a change whose log entry
   is absent from the snapshot prefix ($5.1). */
static int raft_has_config_beyond(const raft_ctx *r,raft_i64 applied_index){
  raft_i64 idx,last,start,off;
  int ci,co;
  last=raft_log_last_index(&r->log);
  start=applied_index+1;
  if(start<r->log.last_included_index+1) start=r->log.last_included_index+1;
  for(idx=start;idx<=last;idx++){
    off=idx-r->log.last_included_index-1;
    ci=(int)(off>>r->log.chunk_bits);
    co=(int)(off&r->log.chunk_mask);
    if(ci>=r->log.num_chunks) break;
    if(r->log.chunks[ci].kinds[co]==RAFT_ENTRY_CONFIG) return 1;
  }
  return 0;
}
RAFT_DEF int raft_snapshot(raft_ctx *r){
  if(!r) return -1;
  if(r->phase!=RAFT_PHASE_RUNNING) return -1;
  if(r->state!=RAFT_LEADER&&r->state!=RAFT_FOLLOWER) return -1;
  if(r->snapshot_pending_dirty) return -1;
  /* A stale apply flag means the live config (and peer list) is not yet
     consistent with the log (a config apply/revert failed partway, e.g. OOM,
     and must be re-run).  Capturing that inconsistent config into a snapshot
     would PERMANENTLY persist it: the log may no longer contain the config
     entry that produced it (an uncommitted joint entry can be truncated by a
     higher-term leader), so the "revert to snapshot base" path would later
     re-apply the stale joint and let it be finalized without the old
     quorum (membership-safety / leader-completeness violation).  Refuse to
     snapshot until the apply is retried successfully. */
  if(r->config_apply_stale) return -1;
  if(raft_has_config_beyond(r,r->last_applied)) return -1;
  r->snapshot_pending_dirty=1;
  r->snapshot_pending.last_index=r->last_applied;
  r->snapshot_pending.last_term=raft_log_term_at(&r->log,r->last_applied);
  r->snapshot_pending.size=0;
  raft_set_clear(&r->snapshot_pending.cfg_old);
  raft_set_clear(&r->snapshot_pending.cfg_new);
  raft_set_clear(&r->snapshot_pending.cfg_learners);
  if(raft_set_copy(&r->snapshot_pending.cfg_old,&r->config_old)<0||raft_set_copy(&r->snapshot_pending.cfg_new,&r->config_new)<0||raft_set_copy(&r->snapshot_pending.cfg_learners,&r->config_learners)<0){
    raft_set_clear(&r->snapshot_pending.cfg_old);
    raft_set_clear(&r->snapshot_pending.cfg_new);
    raft_set_clear(&r->snapshot_pending.cfg_learners);
    r->snapshot_pending_dirty=0;
    return -1;
  }
  return 0;
}
RAFT_DEF int raft_persist_complete(raft_ctx *r,raft_i64 durable_index){
  raft_i64 tip,conf;
  if(!r) return -1;
  if(r->phase!=RAFT_PHASE_RUNNING&&r->phase!=RAFT_PHASE_DRAINING) return -1;
  r->persist_gen++;
  if(durable_index>r->durable_index) r->durable_index=durable_index;
  /* A report covers the log as it stood when the caller started the persist, so
     cap it by the CURRENT tip: after a conflict cut an uncapped report is
     stale-high (a longer log was durable before the cut), and a follower ACK
     built on it would claim fsync'ed entries the node does not have.  This is the
     frontier a follower's success ACK may advertise. */
  tip=raft_log_last_index(&r->log);
  conf=durable_index<tip?durable_index:tip;
  if(conf>r->durable_confirm) r->durable_confirm=conf;
  if(conf>r->persist_synced_index) r->persist_synced_index=conf;   /* what is on disk */
  /* A leader whose own durability just advanced may now hold a durable
     majority for a current-term entry: re-evaluate commit immediately instead
     of waiting for the next leader tick (which runs raft_advance_commit).
     Followers advance commit only via leader_commit in AppendEntries, so this
     is leader-only. */
  if(r->state==RAFT_LEADER) raft_advance_commit(r);
  return 0;
}
RAFT_DEF int raft_snapshot_persist_complete(raft_ctx *r,raft_i64 durable_index){
  int i;
  if(!r) return -1;
  if(r->phase!=RAFT_PHASE_RUNNING&&r->phase!=RAFT_PHASE_DRAINING) return -1;
  /* The snapshot's OWN persist (carrying its size + metadata) is now durable:
     compact the covered prefix.  This is a separate completion from
     raft_persist_complete because a LOG-only persist can also report
     durable_index >= last_index (the log caught up past the snapshot boundary)
     before the snapshot's size has been written - compacting then would persist
     a 0-size snapshot that can never be streamed. */
  if(r->snapshot_pending_dirty&&r->snapshot_data_ready_flag&&durable_index>=r->snapshot_pending.last_index){
    raft_snapshot_meta tmp;
    /* build complete snapshot meta in temp; only mutates state on success */
    memset(&tmp,0,sizeof(tmp));
    tmp.last_index=r->snapshot_pending.last_index;
    tmp.last_term=r->snapshot_pending.last_term;
    tmp.size=r->snapshot_pending.size;
    if(raft_set_copy(&tmp.cfg_old,&r->snapshot_pending.cfg_old)<0||raft_set_copy(&tmp.cfg_new,&r->snapshot_pending.cfg_new)<0||raft_set_copy(&tmp.cfg_learners,&r->snapshot_pending.cfg_learners)<0){
      raft_set_clear(&tmp.cfg_old);
      raft_set_clear(&tmp.cfg_new);
      raft_set_clear(&tmp.cfg_learners);
      return -1;
    }
    /* config copies succeeded: compact covered prefix, preserving any tail */
    if(raft_log_compact_prefix(&r->log,r->snapshot_pending.last_index,r->snapshot_pending.last_term)<0){
      raft_set_clear(&tmp.cfg_old);
      raft_set_clear(&tmp.cfg_new);
      raft_set_clear(&tmp.cfg_learners);
      return -1;
    }
    raft_set_clear(&r->snapshot.cfg_old);
    raft_set_clear(&r->snapshot.cfg_new);
    raft_set_clear(&r->snapshot.cfg_learners);
    r->snapshot=tmp;
    /* The snapshot changed: invalidate in-flight streaming progress so the next
       snap_read restarts each follower from offset 0 of the new snapshot. */
    for(i=0;i<r->peer_count;i++) r->peers[i].pending_snapshot_offset=0;
    /* Do NOT recompute config_joint from the snapshot config: it may be stale
       if the live config advanced between raft_snapshot() and now; config_joint
       is already maintained by raft_apply_config_from_log. */
    r->snapshot_pending_dirty=0;
    r->snapshot_data_ready_flag=0;
    /* free snapshot_pending cfg now that ownership transferred to r->snapshot */
    raft_set_clear(&r->snapshot_pending.cfg_old);
    raft_set_clear(&r->snapshot_pending.cfg_new);
    raft_set_clear(&r->snapshot_pending.cfg_learners);
  }
  return 0;
}
RAFT_DEF int raft_apply_complete(raft_ctx *r,raft_i64 applied_index){
  int self_removed=0;
  raft_i64 idx;
  if(!r) return -1;
  if(r->phase!=RAFT_PHASE_RUNNING&&r->phase!=RAFT_PHASE_DRAINING) return -1;
  /* Advance last_applied for the NORMAL apply path.  A snapshot INSTALL
     advances last_applied inside its block AFTER the compaction succeeds: a
     failed (OOM) install must NOT leave last_applied beyond the log tip, or a
     later LOCAL snapshot would read a term past the tip (raft_log_term_at
     returns 0) and persist a snapshot with last_included_term==0 - corrupting
     leader completeness (seed 587). */
  if(!(r->snapshot_install_pending&&applied_index>=r->snapshot_recv.last_index)){
    if(applied_index>r->last_applied) r->last_applied=applied_index;
  }
  if(r->apply_offered<r->last_applied) r->apply_offered=r->last_applied;
  /* scan log for CONFIG entries in (config_applied_index, last_applied] */
  for(idx=r->config_applied_index+1;idx<=r->last_applied;idx++){
    raft_i64 off;
    int ci,co,kind;
    raft_mask cm_old,cm_new;
    if(idx<=r->log.last_included_index) continue;
    off=idx-r->log.last_included_index-1;
    ci=(int)(off>>r->log.chunk_bits);
    co=(int)(off&r->log.chunk_mask);
    if(ci>=r->log.num_chunks) break;
    /* A chunk slot whose arrays were never allocated (a failed raft_log_alloc_chunk
       OOM after raft_log_ensure_chunk advanced num_chunks) has NULL kinds; it is
       always the LAST chunk, so there are no entries beyond it ($3.8 OOM path). */
    if(!r->log.chunks[ci].kinds) break;
    kind=(int)r->log.chunks[ci].kinds[co];
    if(kind!=RAFT_ENTRY_CONFIG) continue;
    /* A mask-less CONFIG entry (malformed AppendEntries / restore) carries no
       config masks; treat it as a no-op, symmetric with the other config scan
       sites (raft_apply_config_from_log, raft_find_existing_final_config,
       raft_become_leader). */
    if(!r->log.chunks[ci].cfg_old) continue;
    cm_old=r->log.chunks[ci].cfg_old[co];
    cm_new=r->log.chunks[ci].cfg_new[co];
    /* config already applied at append-time ($4.1); handle commit-time side effects.
       Clear ONLY the pending-config tracking: the deferred-reconfig catch-up
       (config_catchup_*) is an independent, later reconfiguration and must survive.
       A stale config_pending/config_index from a previous term (a leader appended a
       config entry then stepped down before it committed) can still be retired here by
       the commit-time scan while a NEW deferred reconfig is mid-catch-up; a full
       raft_config_pending_clear would silently drop that catch-up cookie (seed 1830). */
    if(r->config_pending&&idx==r->config_index) raft_config_pending_only_clear(r);
    if(!raft_mask_eq(cm_old,cm_new)){
      /* Joint entry (C_old,new) committed: leader appends the final C_new entry.
         This normally happens at COMMIT time (raft_maybe_finalize_joint in
         raft_leader_tick); here it is the fallback for a joint committed before
         this node became leader or recovered by leader election. */
      if(r->state==RAFT_LEADER&&raft_finalize_joint(r,idx,cm_new)<0) return -1;
    }else{
      /* Final (C_new) or single-step entry committed: apply config and check self-removal */
      if(raft_apply_config_from_log(r)<0) return -1;
      /* Defer the step-down until AFTER the just-committed cookies (including
         this reconfig's own cookie) are delivered as COMMITTED below: stepping
         down here would blanket-redirect them and misreport a successful
         self-removal as REDIRECT. */
      if(r->state==RAFT_LEADER&&!raft_mask_has(cm_new,r->cfg.id)) self_removed=1;
    }
  }
  r->config_applied_index=r->last_applied;
  if(r->snapshot_install_pending&&applied_index>=r->snapshot_recv.last_index){
    int rr,saved_joint=0;
    raft_set saved_cfg_old,saved_cfg_new,saved_cfg_learners;
    raft_peer_message *m5;
    /* guard: snapshot older than current log prefix (Sec. 7.4).
       If the log was already compacted past this point there is
       nothing to install; discard the stale snapshot. */
    if(r->snapshot_recv.last_index<r->log.last_included_index) r->snapshot_install_pending=0;
    else{
      /* save old config for rollback (take ownership, detach from r->config) */
      saved_cfg_old.ids=r->config_old.ids;
      saved_cfg_old.id_count=r->config_old.id_count;
      r->config_old.ids=0;
      r->config_old.id_count=0;
      saved_cfg_new.ids=r->config_new.ids;
      saved_cfg_new.id_count=r->config_new.id_count;
      r->config_new.ids=0;
      r->config_new.id_count=0;
      saved_cfg_learners.ids=r->config_learners.ids;
      saved_cfg_learners.id_count=r->config_learners.id_count;
      r->config_learners.ids=0;
      r->config_learners.id_count=0;
      saved_joint=r->config_joint;
      /* apply snapshot config before mutating log; restore on failure */
      if(raft_mask_any(raft_set_view(&r->snapshot_recv.cfg_old))){
        if(raft_set_copy(&r->config_old,&r->snapshot_recv.cfg_old)<0||raft_set_copy(&r->config_new,&r->snapshot_recv.cfg_new)<0){
          /* free any config set already reallocated before restoring, so a
             partially-applied copy is not leaked on the OOM rollback */
          raft_set_clear(&r->config_old);
          raft_set_clear(&r->config_new);
          raft_set_clear(&r->config_learners);
          r->config_old=saved_cfg_old;
          r->config_new=saved_cfg_new;
          r->config_learners=saved_cfg_learners;
          r->config_joint=saved_joint;
          return -1;
        }
      }
      if(raft_mask_any(raft_set_view(&r->snapshot_recv.cfg_learners))){
        if(raft_set_copy(&r->config_learners,&r->snapshot_recv.cfg_learners)<0){
          raft_set_clear(&r->config_old);
          raft_set_clear(&r->config_new);
          raft_set_clear(&r->config_learners);
          r->config_old=saved_cfg_old;
          r->config_new=saved_cfg_new;
          r->config_learners=saved_cfg_learners;
          r->config_joint=saved_joint;
          return -1;
        }
      }else raft_set_clear(&r->config_learners);
      r->config_joint=!raft_mask_eq(raft_set_view(&r->config_old),raft_set_view(&r->config_new));
      /* compact log: preserve trailing entries ONLY when the follower already
         holds the snapshot boundary entry with the MATCHING term ($7.3: "if
         existing log entry has same index and term as snapshot's last included
         entry, retain log entries following it; otherwise discard the entire
         log").  A tail kept across a mismatched boundary carries entries the
         snapshot prefix contradicts (e.g. stale uncommitted entries from an old
         term), which would then corrupt the log-up-to-date vote check and let
         a stale voter grant votes to a candidate missing committed entries
         (leader-completeness violation). */
      if(raft_log_last_index(&r->log)>=r->snapshot_recv.last_index&&raft_log_term_at(&r->log,r->snapshot_recv.last_index)==r->snapshot_recv.last_term){
        if(raft_log_compact_prefix(&r->log,r->snapshot_recv.last_index,r->snapshot_recv.last_term)<0){
          raft_set_clear(&r->config_old);
          raft_set_clear(&r->config_new);
          raft_set_clear(&r->config_learners);
          r->config_old=saved_cfg_old;
          r->config_new=saved_cfg_new;
          r->config_learners=saved_cfg_learners;
          r->config_joint=saved_joint;
          return -1;
        }
      }else{
        int ci;
        r->log.last_included_index=r->snapshot_recv.last_index;
        r->log.last_included_term=r->snapshot_recv.last_term;
        r->log.count=0;
        /* The installed boundary content is durable (the caller fsync'ed the
           snapshot before the install), everything above it is not. */
        raft_durable_clamp(r,r->log.last_included_index);
        for(ci=0;ci<r->log.num_chunks;ci++) raft_log_chunk_free(&r->log.chunks[ci],r->log.chunk_size);
        r->log.num_chunks=0;
        if(r->log.data){
          RAFT_FREE(r->log.data);
          r->log.data=0;
          r->log.data_capacity=0;
        }
        r->log.data_size=0;
      }
      /* the install's compaction is committed: advance last_applied to the
         snapshot boundary here (NOT at the top of the function), so an OOM in
         any earlier install step left last_applied untouched and a later LOCAL
         snapshot cannot read a term past the log tip. */
      if(r->last_applied<r->snapshot_recv.last_index) r->last_applied=r->snapshot_recv.last_index;
      if(r->apply_offered<r->last_applied) r->apply_offered=r->last_applied;
      /* free saved old config (log and config are now committed to snapshot state) */
      raft_set_clear(&saved_cfg_old);
      raft_set_clear(&saved_cfg_new);
      raft_set_clear(&saved_cfg_learners);
      if(r->commit_index<r->log.last_included_index) r->commit_index=r->log.last_included_index;
      /* rebuild peer list from the installed config (OOM-safe: restore on failure) */
      if(r->config_joint) rr=raft_rebuild_peers_joint(r);
      else rr=raft_rebuild_peers_normal(r);
      /* OOM: the peer rebuild failed, so the peer list still reflects the
         PRE-install config while the live config (and the compacted log)
         already reflect the installed snapshot.  Do NOT restore the old
         config here (that would contradict the already-compacted log, whose
         LII now points at the installed snapshot).  Mark the config stale
         instead: raft_apply_config_from_log below and raft_advance_inner on
         the next advance re-apply the config, which re-runs the peer rebuild
         (the skip-if-unchanged fast path is disabled while the flag is set). */
      if(rr<0) r->config_apply_stale=1;
      /* Transfer the installed snapshot metadata BEFORE re-applying the log
         config: when the tail holds no config entry, raft_apply_config_from_log
         reverts to its "base" config (r->snapshot.cfg_*), which must be the NEW
         snapshot's config, not the stale old snapshot or the bootstrap config.
         Mark the install complete first so an OOM below cannot re-enter this
         block with a zeroed snapshot_recv. */
      raft_set_clear(&r->snapshot.cfg_old);
      raft_set_clear(&r->snapshot.cfg_new);
      raft_set_clear(&r->snapshot.cfg_learners);
      r->snapshot=r->snapshot_recv;
      memset(&r->snapshot_recv,0,sizeof(r->snapshot_recv));
      r->snapshot_install_pending=0;
      if(raft_apply_config_from_log(r)<0) return -1;
      /* Discard an in-progress LOCAL snapshot only when it is STALE: its
         boundary lies strictly BEHIND the installed snapshot.  A local snapshot
         at (or beyond) the installed boundary is equally (or more) up-to-date,
         so keep it - its own persist + compact path finalizes it and its size
         is preserved.  We deliberately do NOT clear it while merely RECEIVING
         chunks (raft_recv_install_snapshot), since a rejected or interrupted
         stream must not abort a valid local snapshot. */
      if(r->snapshot_pending.last_index<r->snapshot.last_index){
        r->snapshot_pending_dirty=0;
        r->snapshot_data_ready_flag=0;
        raft_set_clear(&r->snapshot_pending.cfg_old);
        raft_set_clear(&r->snapshot_pending.cfg_new);
        raft_set_clear(&r->snapshot_pending.cfg_learners);
      }
      /* notify leader that snapshot install completed */
      if(raft_id_valid(r->leader_id)&&raft_msg_ensure(r,r->msg_count+1)==0){
        m5=&r->msg_buf[r->msg_count];
        m5->type=RAFT_MSG_INSTALL_SNAPSHOT_RESULT;
        m5->from=r->cfg.id;
        m5->to=r->leader_id;
        m5->term=r->current_term;
        m5->install_snapshot_result.term=r->current_term;
        m5->install_snapshot_result.last_included_index=r->snapshot.last_index;
        r->msg_count++;
      }
    }
  }
  /* Deliver results for committed entries.  client_pending is ordered by log_index and
     retired from the front, so the completed requests are exactly a PREFIX: retiring them
     is an index bump, not a memmove per request (a deep pipeline used to cost O(N^2) per
     round).  raft_result_emit allocates on its own, so no pre-count/pre-ensure is needed
     here and no OOM path can skip the step-down below. */
  while(r->client_pending_count>0&&r->client_pending[r->client_pending_head].log_index<=r->last_applied){
    raft_result_emit(r,r->client_pending[r->client_pending_head].cookie,RAFT_CLIENT_COMMITTED);
    r->client_pending_head++;
    r->client_pending_count--;
  }
  if(r->client_pending_head>0&&r->client_pending_count==0) r->client_pending_head=0;
  if(r->client_pending_count==0) raft_client_pending_compact(r);
  /* A leader that committed its own removal steps down only after the
     just-committed cookies have been delivered as COMMITTED above. */
  if(self_removed&&r->state==RAFT_LEADER) raft_step_down(r,r->current_term,0);
  return 0;
}
RAFT_DEF int raft_snapshot_data_provided(raft_ctx *r,int follower_id,raft_i64 byte_offset,const void *data,unsigned int size){
  raft_peer_message *m;
  raft_peer *pr;
  raft_i64 remaining;
  if(!r) return -1;
  if(!raft_id_valid(follower_id)) return -1;
  if(r->phase!=RAFT_PHASE_RUNNING) return -1;
  if(r->state!=RAFT_LEADER) return -1;
  pr=raft_find_peer(r,follower_id);
  if(!pr) return -1;
  /* The leader's snapshot is authoritative: an InstallSnapshot chunk carries
     the identity of the snapshot actually being streamed (r->snapshot), never
     an application echo.  A snapshot replacement (raft_persist_complete) resets
     pending_snapshot_offset to 0, so an echo still describing the previous
     stream (stale byte_offset) is dropped here instead of being mislabeled
     and sent to a follower mid-stream of the old snapshot ($7.4 stale
     InstallSnapshot guard, enforced at the leader). */
  if(byte_offset!=pr->pending_snapshot_offset) return 0;
  if(raft_msg_ensure(r,r->msg_count+1)<0) return -1;
  m=&r->msg_buf[r->msg_count];
  m->type=RAFT_MSG_INSTALL_SNAPSHOT;
  m->from=r->cfg.id;
  m->to=follower_id;
  m->term=r->current_term;
  m->install_snapshot.term=r->current_term;
  m->install_snapshot.snapshot_last_index=r->snapshot.last_index;
  m->install_snapshot.snapshot_last_term=r->snapshot.last_term;
  m->install_snapshot.leader_id=r->cfg.id;
  m->install_snapshot.snapshot_data=data;
  m->install_snapshot.snapshot_offset=byte_offset;
  /* Clamp an oversized final chunk to the exact remaining bytes so the
     follower's strict "offset+chunk_size == snapshot_data_size" boundary
     check always holds on the done chunk ($5.1 InstallSnapshot streaming). */
  remaining=r->snapshot.size-byte_offset;
  if(remaining<0) remaining=0;
  if((raft_i64)size>remaining) size=(unsigned int)remaining;
  m->install_snapshot.snapshot_done=(byte_offset+(raft_i64)size==r->snapshot.size);
  m->install_snapshot.snapshot_data_size=r->snapshot.size;
  m->install_snapshot.snapshot_chunk_size=(raft_i64)size;
  m->install_snapshot.snapshot_cfg_old=raft_set_view(&r->snapshot.cfg_old);
  m->install_snapshot.snapshot_cfg_new=raft_set_view(&r->snapshot.cfg_new);
  m->install_snapshot.snapshot_cfg_learners=raft_set_view(&r->snapshot.cfg_learners);
  r->msg_count++;
  /* advance snapshot offset for next chunk */
  pr->pending_snapshot_offset=byte_offset+(raft_i64)size;
  return 0;
}
RAFT_DEF int raft_snapshot_data_ready(raft_ctx *r,raft_i64 total_size){
  if(!r) return -1;
  if(r->phase!=RAFT_PHASE_RUNNING) return -1;
  r->snapshot_pending.size=total_size;
  r->snapshot_data_ready_flag=1;
  r->persist_needed=1;  /* trigger persist output so snapshot metadata is emitted in Ready */
  return 0;
}
RAFT_DEF void raft_ready_consumed(raft_ctx *r){
  if(!r) return;
  /* retain buffers for next cycle; only reset counts.
     Caller MUST have finished processing all Ready pointers
     before calling this - see Zero-copy contract in header. */
  r->apply_count=0;
  r->result_count=0;
  r->msg_count=0;
  r->snap_read_count=0;
  r->ready_persist_pending=0;
  r->ready_leader_change=0;
}
RAFT_DEF int raft_inspect(const raft_ctx *r,raft_info *out){
  int qi;
  if(!r||!out) return -1;
  out->id=r->cfg.id;
  out->state=r->state;
  out->leader_id=r->leader_id;
  out->voted_for=r->voted_for;
  out->phase=r->phase;
  out->term=r->current_term;
  out->log_entry_count=r->log.count;
  out->commit_index=r->commit_index;
  out->last_applied=r->last_applied;
  out->last_included_index=r->log.last_included_index;
  out->last_included_term=r->log.last_included_term;
  out->peer_count=r->peer_count;
  out->election_elapsed=r->election_elapsed;
  out->election_deadline=r->election_deadline;
  out->in_pre_vote=r->in_pre_vote;
  out->config_joint=r->config_joint;
  out->leadership_confirmed=r->leadership_confirmed;
  out->quorum_acked=0;
  for(qi=0;qi<r->peer_count;qi++){
    if(r->peers[qi].quorum_acked&&!r->peers[qi].is_learner&&r->peers[qi].in_new_config) out->quorum_acked++;
  }
  out->snapshot_install_pending=r->snapshot_install_pending;
  if(out->peers){
    int pi;
    for(pi=0;pi<r->peer_count;pi++){
      out->peers[pi].id=r->peers[pi].id;
      out->peers[pi].is_learner=r->peers[pi].is_learner;
      out->peers[pi].match_index=r->peers[pi].match_index;
      out->peers[pi].next_index=r->peers[pi].next_index;
      out->peers[pi].in_old_config=r->peers[pi].in_old_config;
      out->peers[pi].in_new_config=r->peers[pi].in_new_config;
      out->peers[pi].catchup_round=r->peers[pi].catchup_round;
      out->peers[pi].catchup_round_elapsed=r->peers[pi].catchup_round_elapsed;
    }
  }
  return 0;
}
#endif
