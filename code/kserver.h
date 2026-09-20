/* kserver.h -- the kynovo replicated storage engine core.  Pure: depends
   only on kbase.h / kproto.h / raft.h / treap.h / vfs.h / runtime.h (never
   cemon/cli).  All link I/O goes through the injected k_server_transport
   vtable; time via k_server_advance(elapsed_ms); threads via the runtime
   backend.  Single translation unit use.
   Also owns the peer-side wire codec: raft-mask codec, peer
   message codec (raft_peer_message <-> wire), and response encode.

   NOT self-contained: the caller must include the dependencies FIRST, in this
   order (each with X_STATIC / X_IMPLEMENTATION as needed for single-TU use):
     vfs.h -> treap.h -> runtime.h -> raft.h -> kbase.h -> kproto.h -> kserver.h
   (kproto.h is self-contained and pulls in kbase.h + raft.h itself.) */
#ifndef KSERVER_H
#define KSERVER_H

/* ---- server constants (disk format / config / conn / timing) ---- */
/* disk format magics */
#define K_CFG_MAGIC 0x3147464bu
#define K_WAL_MAGIC 0x314c574bu
#define K_WAL_META_MAGIC 0x314d574bu
#define K_SNAPSHOT_MAGIC 0x4b444231u /* "KDB1": snapshot now leads with the address book */
/* disk format versions */
/* v2: added flush_bytes_limit (payload 36 -> 40 bytes).  Nothing is deployed
   yet, so there is no migration path: a v1 data directory is refused at open
   (fail-stop) and is simply re-initialized - note that `kdbsvr init` writes a
   fresh wal.meta too, so it discards the local data directory. */
#define K_CFG_VERSION 2u
#define K_WAL_VERSION 1u
/* 2: the two slots are 4 KiB apart (v1 had them adjacent in one 512 B region) */
#define K_WAL_META_VERSION 2u
/* on-disk sizes */
#define K_CFG_HEADER_SIZE 16u
#define K_CFG_PAYLOAD_SIZE 40u
#define K_CFG_SIZE (K_CFG_HEADER_SIZE+K_CFG_PAYLOAD_SIZE)
#define K_WAL_HEADER_SIZE 24u
#define K_WAL_META_SLOT_SIZE 128u
/* The two slots live 4 KiB apart: a power loss can tear the 512 B / 4 KiB region
   around the slot being written, and both copies must not share that region. */
#define K_WAL_META_SLOT_GAP 4096u
#define K_WAL_META_SIZE (K_WAL_META_SLOT_GAP*2u)
#define K_WAL_INFLIGHT_MAX 4
/* The WAL record carries its own header (magic/version/generation/size/CRC), so
   recovery can find the NEWEST record by scanning the segment the meta slot
   names.  The meta slot is therefore only a fast pointer, not the durability
   boundary: a slot is written AND fsync'ed only when the segment rotates or every
   K_WAL_META_FSYNC_EVERY records (writing it without syncing would overwrite the
   last durable copy in the page cache and could leave BOTH slots unusable after a
   power loss).  Between checkpoints the position lives in the worker's memory, so
   the steady-state cost is ONE fsync per record and the recovery scan stays bounded
   by K_WAL_META_FSYNC_EVERY records.  Recovery trusts the slot's own generation and
   size for the record it names, then walks forward with consecutive generations; a
   record that reads completely but fails its CRC is corruption (fail-stop), while a
   partially written record is the crash tail and is skipped. */
#define K_WAL_META_FSYNC_EVERY 64u
#ifndef K_CLIENT_CONNECTION_MAX
#define K_CLIENT_CONNECTION_MAX 4096u
#endif
#ifndef K_REQUEST_INFLIGHT_MAX
#define K_REQUEST_INFLIGHT_MAX 16384u
#endif
#ifndef K_REQUEST_BYTES_MAX
#define K_REQUEST_BYTES_MAX K_U64_C(268435456)
#endif
/* Cookie lookup index size.  A request's cookie IS its own address (see
   k_request_find), so the index hashes the pointer: the two hot lookups (one per
   applied entry, one per client result) otherwise scan a list that can hold
   K_REQUEST_INFLIGHT_MAX entries, which makes a batch completion O(N^2).  With
   this many buckets and at most K_REQUEST_INFLIGHT_MAX live requests, the chain
   length is bounded by 16 - O(1) with a small constant, 4 KiB of table. */
#define K_REQUEST_HASH_BUCKETS 1024u
#ifndef K_RX_BYTES_MAX
#define K_RX_BYTES_MAX K_U64_C(67108864)
#endif
#define K_SNAPSHOT_CHUNK 32768u
#define K_RAFT_COMMAND_MAX (K_FRAME_MAX-256u)
/* config defaults */
#define K_DEFAULT_SEED K_U64_C(0)
#define K_DEFAULT_WAL_SEG_SIZE K_U64_C(67108864)
#define K_DEFAULT_FLUSH_TIMEOUT_MS 3u
/* Batch target for the structural group-commit policy (kserver.h
   k_server_flush_if_ready): a sustained write stream is submitted once this many
   writes are pending, so this is the knob that trades write latency for throughput.
   It used to be a 1024-entry safety valve because the real batching was supposed to
   come from the 3 ms window - which the driver bypassed by flushing every round.
   Derived from a p99 target: batch ~= target_tps * accepted_delay. */
#define K_DEFAULT_FLUSH_ITEM_LIMIT 32u
/* Byte ceiling for one group-commit batch.  The item count alone is not a bound:
   32 x 512 KiB is a 16 MiB logical batch, and the same bytes are copied into the
   Raft log, the persist view and the WAL record.  Submitting on whichever of
   count/bytes/deadline trips first keeps a large-value workload from building a
   batch whose own serialization dominates the fsync it is amortizing. */
#define K_DEFAULT_FLUSH_BYTES_LIMIT 262144u
#define K_DEFAULT_SNAPSHOT_SEGMENTS 10u
#define K_DEFAULT_SNAPSHOT_ENTRIES 64u
#define K_DEFAULT_POLL_MS 10u
#define K_SNAPSHOT_WAL_RATIO 4u
/* connection / userdata types */
#define K_CONN_PEER 1
#define K_CONN_CLIENT 2
#define K_UD_SERVER 1
#define K_UD_CONN 2
/* timing (ms) */
#define K_RECONNECT_MS 100u
#define K_HEARTBEAT_MS 50u
#define K_ELECTION_MIN_MS 250u
#define K_ELECTION_MAX_MS 500u
/* snapshot task kinds */
#define K_SNAPSHOT_TASK_SAVE 1
#define K_SNAPSHOT_TASK_CLEANUP 2
/* peer message types */
#define K_PEER_HELLO 1u
#define K_PEER_RAFT 2u

/* op codes: data writes first, then the internal address-book op (wire values) */
#define K_OP_SET 1u
#define K_OP_MSET 2u
#define K_OP_RSET 3u
#define K_OP_DEL 4u
#define K_OP_MDEL 5u
#define K_OP_RDEL 6u
#define K_OP_CAS 7u
#define K_OP_FCALL 8u
#define K_OP_ADDR 9u
/* ================= Types: config ================= */
typedef struct k_cfg{
  k_u64 seed;
  k_u64 wal_seg_size;
  k_u32 flush_timeout_ms;
  k_u32 flush_window_max_ms;       /* the configured ceiling; the driver may shrink the window
                                      toward the measured sync cost but never past this */
  k_u32 flush_item_limit;   /* batch target: submit once this many writes are pending */
  k_u32 flush_bytes_limit;  /* byte target: submit once this many pending write bytes (0=off) */
  k_u32 snapshot_segments;
  k_u32 snapshot_entries;
  k_u32 poll_ms;
} k_cfg;

/* ================= Types: ready & server structures ================= */
typedef struct k_server k_server;
typedef struct k_conn k_conn;
typedef struct k_request k_request;
typedef struct k_snapshot_chunk k_snapshot_chunk;
typedef struct k_ready_bundle k_ready_bundle;
typedef struct k_snapshot_task k_snapshot_task;
/* Outbound/inbound transport seam (mirrors vfs.h backend routing): the server
   core performs ALL link I/O through this vtable and never calls cemon.  The
   default backend is cemon; a deterministic test harness swaps in a capture
   backend to record outbound frames and inject inbound frames. */
typedef struct k_server_transport{
  const char *name;
  int (*send_frame)(void *sock,k_u32 magic,k_u8 type,const void *payload,k_u32 size,int control);
  void (*close)(void *sock);
  int (*recv)(void *sock);
  void *(*dial)(k_server *server,const k_node_spec *node);
  void (*setud)(void *sock,void *ud);
} k_server_transport;
typedef struct k_file_io{
  vfs_file *file;
  k_u64 off;
  k_crc32_ctx crc;
} k_file_io;
typedef struct k_restore{
  raft_persist persist;
  raft_persist_entry *entries;
  int entry_capacity;
  /* Recovery concatenates the entries of MANY WAL records (each carries only a delta).
     The entry BYTES are copied into owned blocks whose addresses never move, so a record
     payload can be released as soon as it is decoded - which is what lets recovery run in
     a single pass over the WAL instead of reading it twice. */
  k_u8 **blocks;
  int block_count;
  int block_capacity;
  k_u32 block_size;
  k_u32 block_used;
  k_u64 generation;
} k_restore;
typedef struct k_snapshot_cache{
  raft_i64 index;
  raft_i64 term;
  raft_i64 size;
  int *old_ids;
  int old_count;
  int *new_ids;
  int new_count;
  int *learner_ids;
  int learner_count;
} k_snapshot_cache;
typedef struct k_wal_pos{
  k_u64 segment;
  k_u64 offset;
} k_wal_pos;
typedef struct k_wal_meta_state{
  int slot;
  k_u64 generation;
  k_wal_pos record;
  k_u64 record_size;
  k_wal_pos next;
} k_wal_meta_state;
typedef struct k_ready_message{
  int to;
  int control;
  k_u8 *payload;
  k_u32 size;
} k_ready_message;
typedef struct k_ready_apply{
  raft_i64 index;
  k_u8 *command;
  k_u32 command_size;
  const void *cookie;
  int kind;               /* RAFT_ENTRY_COMMAND / RAFT_ENTRY_NOOP / RAFT_ENTRY_CONFIG */
  int *cfg_old_ids;       /* CONFIG only: deep-copied old voter ids (joint keeps them) */
  int cfg_old_count;
  int *cfg_new_ids;       /* CONFIG only: deep-copied new voter ids */
  int cfg_new_count;
  int *cfg_learners_ids;  /* CONFIG only: deep-copied learner ids */
  int cfg_learners_count;
} k_ready_apply;
typedef struct k_ready_client{
  raft_client_result result;
  k_u8 *response;
} k_ready_client;
typedef struct k_wal_worker{
  k_u64 sync_us_ewma;              /* EWMA of one record's write+sync cost (observability +
                                      input to the driver's adaptive flush window) */
  k_u64 sync_us_max;               /* slowest single record write+sync seen.  The EWMA above is
                                      min-biased on purpose - it must track the fast end for the
                                      window policy - so it structurally hides outliers, and an
                                      occasional multi-millisecond sync is invisible without this. */
  k_u64 slow_syncs;                /* syncs slower than K_SLOW_SYNC_US */
  k_server *server;
  int meta_slot;
  k_u64 generation;
  k_wal_pos next;
  /* File handles are held across batches.  Opening/closing the segment and the meta
     file per batch costs about as much as the two fsyncs it accompanies (measured
     ~6 ms of ~12 ms per batch on this machine), and no correctness property needs a
     fresh handle: the worker already owns the WAL file state (the generation counter
     lives here for the same reason, see k_wal_append_bundle).  Durability is
     unchanged - every batch still ends in vfs_sync. */
  vfs_file *seg_file;
  k_u64 seg_file_segment;
  vfs_file *meta_file;
  int open_files;   /* held handles, 0..2: observability + leak assertion */
  int meta_since_sync;      /* records written since the meta slot was last fsync'ed */
  k_u64 meta_sync_segment;  /* segment the durable meta slot currently names */
} k_wal_worker;
struct k_ready_bundle{
  k_server *server;
  k_u8 *wal_payload;
  k_u32 wal_payload_size;
  k_u64 generation;
  raft_i64 durable_index;
  k_wal_meta_state wal_result;
  k_ready_message *messages;
  int message_count;
  k_ready_apply *applies;
  int apply_count;
  k_ready_client *clients;
  int client_count;
  raft_snapshot_read_req *snapshot_reads;
  int snapshot_read_count;
  k_snapshot_cache snapshot_after;
  raft_i64 old_snapshot_index;
  raft_i64 snapshot_install_index;
  int is_leader;
  int leader_id;
  int persist_needed;
  int live_deferred;
  int snapshot_dirty;
  int snapshot_install_needed;
  int phase_stopped;
  int wal_ok;
  k_u32 item_count;
};
struct k_snapshot_task{
  k_server *server;
  k_u64 size;              /* bytes written to the snapshot file (the file's length) */
  raft_i64 index;
  raft_i64 old_snapshot_index;
  k_u64 keep_segment;
  int kind;
  int serialized;
  int ok;
  int cleanup_failed;
  k_cluster cluster_copy; /* address book captured at snapshot time (main thread) */
};
struct k_conn{
  int ud_kind;
  k_server *server;
  void *sock;
  k_conn *next;
  k_request *requests;      /* in-flight requests belonging to THIS connection */
  k_rx rx;
  int kind;
  int outbound;
  int recv_paused;
  int node_index;
  int peer_id;
};
static k_u32 k_request_bucket(const void *cookie);
struct k_request{
  k_request *next;
  k_request *prev;         /* doubly-linked server->requests, so unlink is O(1) */
  k_request *hash_next;    /* chain in server->request_hash[k_request_bucket()] */
  k_request *hash_prev;    /* so bucket removal is O(1) too, not a chain scan */
  k_request *conn_next;    /* doubly-linked k_conn.requests, so closing a connection is
                              O(requests of THAT connection) instead of a scan of every
                              in-flight request on the server */
  k_request *conn_prev;
  int internal;            /* created by the server itself (no client to answer): the
                              result handler must not infer this from conn==0, which is
                              also what a closed client connection looks like */
  k_request *write_next;
  k_conn *conn;
  k_u32 id;
  int type;
  k_u8 *key;
  k_u32 key_len;
  k_u8 *end;
  k_u32 end_len;
  k_u8 *command;
  k_u32 command_size;
  treap *txn;      /* EXEC: COW working copy (published on apply) */
  k_buf writes;    /* EXEC: write-set buffer (put/del sequence) */
  k_buf result;    /* EXEC: command result returned to client */
  k_request *gate_next;  /* link in the FCALL gate queue */
  k_u64 tracked_bytes;
  int submitted;                 /* 1 = handed to raft and awaiting its terminal result; the request
                                    IS the raft cookie, so freeing it earlier leaves Raft's live
                                    payload pointing at freed memory (observed as a page-heap read
                                    fault in treap_blob_create). */
   /* conservative live-memory charge for admission control */
  int conflict;          /* CAS: 1 = condition not met (reply CONFLICT) */
  k_u64 admit_ms;        /* server-injected time at admission; the terminal result reports the age */
  k_u64 submit_ms;       /* when the request was handed to raft: splits the age into "waited for a batch
                            or a WAL slot" and "waiting for durable + the wake" */
};
static k_u32 k_request_bucket(const void *cookie){
  k_u64 v=(k_u64)(size_t)cookie;
  v^=v>>17;
  v*=K_U64_C(0x9e3779b97f4a7c15);
  v^=v>>29;
  return (k_u32)(v&(k_u64)(K_REQUEST_HASH_BUCKETS-1u));
}
struct k_snapshot_chunk{
  k_snapshot_chunk *next;
  int follower_id;
  raft_i64 offset;
  k_u8 *data;
  k_u32 size;
};
struct k_server{
  int ud_kind;
  int id;
  unsigned short client_port;
  unsigned short peer_port;
  char base[K_URI_MAX];
  k_cfg cfg;
  k_cluster cluster;
  /* dynamic membership snapshot (voters/learners), driven by CONFIG apply */
  int voters[K_MAX_NODES];
  int voter_count;
  /* Last membership FACTS raft reported in a ready bundle (issue #14): the application caches what it is
     told instead of reading raft_ctx's config fields itself. */
  int cfg_joint;
  int self_is_voter;
  int self_is_learner;
  int learners[K_MAX_NODES];
  int learner_count;
  int pending[K_MAX_NODES];  /* in-flight catch-up targets, not yet in config */
  int pending_count;
  const k_server_transport *transport;
  void *loop;                    /* opaque app event-loop handle (transport->dial only) */
  void (*on_stop)(void *loop);   /* app callback: fired once the server has stopped */
  void (*on_wal_result)(void *loop,void *ud); /* app callback: WAL write completed, wake the loop */
  const char *runtime_backend;   /* injected runtime backend: NULL="thread", "sync"=deterministic (test harness) */
  void *peer_socks[K_MAX_NODES];
  k_u64 peer_send_drops;        /* raft frames the peer transport refused/dropped */
  k_u64 flush_batches;          /* successful group-commit submissions */
  int releasing;                /* 1 = k_server_release is tearing down: freeing still-submitted
                                   requests there is by design, not a contract violation */
  k_u64 wal_post_failed;        /* WAL results that could not be handed back to the loop (fail-stop):
                                   a lost result can never release wal_inflight_count, so it is not
                                   a droppable event */
  k_u64 wal_blocked_warned;     /* 1 = the "submissions blocked" warning was already printed */
  k_u32 flush_window_max_ms;    /* the configured ceiling for cfg.flush_timeout_ms; the driver
                                   loop may shrink the window toward the measured sync cost
                                   but never grow it past what the operator configured */
  k_u64 wal_records;            /* WAL records actually appended (>= flush_batches:
                                   one Raft advance can merge several submissions) */
  k_u64 flush_writes_total;     /* writes carried by those submissions (mean batch = total/batches) */
  k_u64 writes_arrived;         /* monotonic: lets the driver tell "new input" from "drained" */
  /* Group-commit triggers, counted separately: which one actually fires is what
     decides the mean batch size (flush_writes_total/flush_batches). */
  k_u64 flush_by_target;
  k_u64 flush_by_drain;
  k_u64 flush_by_window;
  k_u64 flush_by_bytes;         /* batch submitted on the byte target */
#define K_SLOW_ROUND_US 5000u  /* a round whose work exceeds this is counted in slow_rounds */
#define K_SLOW_ACK_MS   5u     /* a request the server held longer than this is counted in slow_acks */
#define K_SLOW_WAKE_US  5000u  /* a WAL-post to loop-consumed handoff slower than this is counted */
#define K_SLOW_HANDOFF_US 5000u /* a loop-post to worker-started-write handoff slower than this */
#define K_SLOW_SYNC_US  5000u  /* a record write+sync slower than this is counted in slow_syncs */
  k_u64 flush_by_barrier;       /* submitted on demand: a read/FCALL needed the queue drained */
  k_u64 flush_by_stop;          /* submitted while stopping (the queue must not be stranded) */
  k_u64 write_bytes;            /* command bytes currently queued for the next submission */
  /* Ingest accounting: client requests admitted per event-loop round is THE ceiling
     for a multi-client high-TPS deployment.  requests/rounds is the number to watch. */
  k_u64 rounds;
  /* Round WORK accounting: how long the driver spent between the poll returning and the batch
     being submitted (the poll's own wait is not work and is excluded).  This is the only measure
     that can say whether a latency spike a client reports came from inside the loop: if no round
     here is slow, the delay was added outside the server.  Diagnostic only, never a decision.
     See doc/gaps-audit.md J-2. */
  k_u64 round_us_last;
  k_u64 round_us_max;
  k_u64 round_us_ewma;
  k_u64 slow_rounds;            /* rounds whose work exceeded K_SLOW_ROUND_US */
  /* Request-age accounting, in the server's own injected time: admission to terminal result - i.e.
     how long the server itself held a request.  This is the one number round accounting cannot give,
     because a loop blocked in its poll does no work at all: a latency spike a client reports is the
     server's only if this age is large.  Diagnostic only, never a decision.  doc/gaps-audit.md J-2. */
  k_u64 elapsed_total_ms;       /* accumulated injected time (never a substitute for the driver clock) */
  k_u64 req_age_ms_last;
  k_u64 req_age_ms_max;
  k_u64 slow_acks;              /* requests held longer than K_SLOW_ACK_MS */
  k_u64 req_wait_ms_last;       /* admission -> handed to raft (batch window, WAL slot, backpressure) */
  k_u64 req_wait_ms_max;
  k_u64 req_svc_ms_last;       /* handed to raft -> terminal result (durability plus the wake) */
  k_u64 req_svc_ms_max;
  /* Wake handoff, in real microseconds and diagnostics-only: the WAL worker stamps wal_post_us just
     before it posts the finished bundle, and the driver compares it with its own clock once the round
     that consumed it has finished.  This is the one segment the injected-time accounting cannot see -
     the loop does no work while it waits to be told - and it is where a cross-thread wake shows up. */
  k_u64 wal_post_us;
  k_u64 wake_us_last;
  k_u64 wake_us_max;
  k_u64 slow_wakes;             /* handoffs that took longer than K_SLOW_WAKE_US */
  k_u64 wake_us_seen;
  /* The other half of the same handoff: the loop stamps wal_task_post_us when it queues a bundle for
     the WAL worker, and the worker compares it with the clock reading it takes when it starts the
     write.  Together with wake_us_* this brackets the worker's own pickup.  Diagnostics only. */
  k_u64 wal_task_post_us;
  k_u64 handoff_us_last;
  k_u64 handoff_us_max;
  k_u64 slow_handoffs;
  k_u64 handoff_samples;        /* 0 means "never measured" - a max of 0 is not a fast handoff */
  k_u64 wake_samples;
  /* The wake measurement above runs to the END of the round that consumed the bundle, so it contains
     that round's own work.  wake_pre_* stops at the START of the loop's next round instead, which is
     the question the OS scheduler answers: post -> was the loop running again.  Together the pair
     says whether the delay is scheduling or the loop's own batch work. */
  k_u64 wake_pre_us_last;
  k_u64 wake_pre_us_max;
  k_u64 slow_wake_pres;
  k_u64 wake_pre_samples;
  k_u64 wake_pre_seen;
  /* Where a round's time goes, split at the poll boundary: receiving, decoding and SENDING all happen
     inside cemon callbacks, so round_us_* (measured after the poll returns) could only ever see half the
     work.  frames_* is how many client requests one round took in, which is the number that turns these
     microseconds into a per-request cost. */
  k_u64 poll_us_last;
  k_u64 poll_us_max;
  k_u64 poll_us_ewma;
  k_u64 frames_last;
  k_u64 frames_max;
  k_u64 poll_samples;
  k_u64 stats_truncated;   /* INFO/STATS replies that did not fit their buffer (must stay 0) */
  /* Backpressure depth: how many WAL writes may be in flight.  Defaults to the compile-time cap; with
     a latency budget configured the driver narrows it so that a request never queues behind more than
     budget/sync of durable work, and widens it again when syncs are fast.  Observable, and 0 means
     "not applied". */
  k_u32 wal_inflight_max;
  k_u32 latency_budget_us;
  k_u64 client_requests;
  k_u64 request_bytes;
  k_u64 rx_buffer_bytes;
  k_u32 request_count;
  k_u32 client_connection_count;
  k_conn *peer_conns[K_MAX_NODES];
  k_conn *connections;
  k_request *requests;
  k_request *request_hash[K_REQUEST_HASH_BUCKETS];   /* cookie index (see k_request_bucket) */
  k_request *write_head;
  k_request *write_tail;
  k_u32 write_count;
  k_request *gate_head;   /* writes queued while the FCALL gate is closed */
  k_request *gate_tail;
  int gate_closed;        /* FCALL in fork->commit window: queue all writes */
  unsigned int write_elapsed;
  k_snapshot_chunk *provided_chunks;
  raft_ctx *raft;
  treap *tree;
  runtime_ctx *runtime;
  runtime_ctx *wal_rt;
  runtime_ctx *snapshot_rt;
  k_wal_worker wal_worker;
  k_wal_meta_state wal_meta;
  int wal_inflight_count;
  k_snapshot_task *snapshot_task;
  k_snapshot_task *snapshot_retry;
  k_snapshot_cache snapshot;
  k_u64 persist_generation;
  k_u64 wal_pending_items;
  k_u64 wal_accumulated_events;
  k_u64 snapshot_wal_segment;
  k_u64 snapshot_wal_offset;
  /* A torn/corrupt newest snapshot must not brick startup, so the previous snapshot
     (and the WAL segments from its base onward) is kept as a rollback target: the
     history is two deep and only the older-than-previous one is ever unlinked. */
  raft_i64 snapshot_prev_index;
  raft_i64 snapshot_older_index;
  k_u64 snapshot_prev_wal_segment;
  k_u64 snapshot_failed_segment_baseline;
  raft_i64 snapshot_failed_apply_baseline;
  raft_i64 last_snapshot_task_index;
  k_u64 last_tick_us;
  unsigned int reconnect_elapsed;
  unsigned int wal_blocked_elapsed;
  int snapshot_inflight;
  int bootstrap;           /* start with config={self} (empty log), join via MEMBER ADD */
  int auto_replace;                 /* 1 = enable Sec 4.4 auto-replacement */
  int auto_replace_new_id;          /* replacement node id */
  char auto_replace_new_host[K_HOST_MAX];
  unsigned short auto_replace_new_client_port;
  unsigned short auto_replace_new_peer_port;
  unsigned int auto_replace_threshold; /* missed_rounds >= this -> replace */
  int auto_replace_phase;           /* 0=idle 1=adding 2=removing */
  int auto_replace_failed_id;       /* the failed voter being replaced */
  int snapshot_cleanup_busy;
  int snapshot_failed;
  int snapshot_cleanup_failed;
  int admission;
  int stopping;
  int stopped;
  int raft_stopped;
  int fatal;
  int started;
  int is_leader;
  int leader_id;
  raft_i64 last_applied;
};
/* ================= Server: snapshot cache & WAL metadata ================= */
/* ---- raft-mask codec ---- */
static void k_buf_mask(k_buf *b,raft_mask mask){
  int i;
  if(mask.id_count<0||mask.id_count>K_MAX_NODES||(mask.id_count&&!mask.ids)){
    b->err=1;
    return;
  }
  k_buf_u32(b,(k_u32)mask.id_count);
  for(i=0;i<mask.id_count;i++) k_buf_i32(b,(k_i32)mask.ids[i]);
}
static int k_reader_mask(k_reader *r,raft_mask *mask){
  k_u32 count,i;
  int *ids;
  if(!r||!mask) return -1;
  mask->ids=0;
  mask->id_count=0;
  count=k_reader_u32(r);
  if(r->err||count>K_MAX_NODES) return -1;
  if(!count) return 0;
  ids=(int *)K_MALLOC(sizeof(int)*count);
  if(!ids) return -1;
  for(i=0;i<count;i++) ids[i]=(int)k_reader_i32(r);
  if(r->err){
    K_FREE(ids);
    return -1;
  }
  mask->ids=ids;
  mask->id_count=(int)count;
  return 0;
}
static void k_mask_free(raft_mask *mask){
  if(!mask) return;
  K_FREE((void *)mask->ids);
  mask->ids=0;
  mask->id_count=0;
}
static int k_mask_copy(int **ids_out,int *count_out,raft_mask mask){
  int *ids=0;
  if(!ids_out||!count_out||mask.id_count<0||(mask.id_count&&!mask.ids)) return -1;
  if(mask.id_count){
    ids=(int *)K_MALLOC(sizeof(int)*(unsigned int)mask.id_count);
    if(!ids) return -1;
    memcpy(ids,mask.ids,sizeof(int)*(unsigned int)mask.id_count);
  }
  K_FREE(*ids_out);
  *ids_out=ids;
  *count_out=mask.id_count;
  return 0;
}
static raft_mask k_cache_mask(const int *ids,int count){
  raft_mask mask;
  mask.ids=ids;
  mask.id_count=count;
  return mask;
}

static int k_response_payload(k_buf *payload,k_u32 request_id,k_u8 status,int leader_id,const char *host,unsigned short port,const void *body,k_u32 body_size){
  k_u16 host_len=0;
  if(!payload||(body_size&&!body)) return -1;
  memset(payload,0,sizeof(*payload));
  if(host){
    size_t len=strlen(host);
    if(len>65535u) return -1;
    host_len=(k_u16)len;
  }
  k_buf_u32(payload,request_id);
  k_buf_u8(payload,status);
  k_buf_i32(payload,(k_i32)leader_id);
  k_buf_u16(payload,host_len);
  k_buf_bytes(payload,host,host_len);
  k_buf_u16(payload,(k_u16)port);
  k_buf_u32(payload,body_size);
  k_buf_bytes(payload,body,body_size);
  if(payload->err||payload->len>K_FRAME_MAX){
    k_buf_free(payload);
    return -1;
  }
  return 0;
}

/* ---- peer message codec (raft_peer_message <-> wire) ---- */
typedef struct k_decoded_peer{
  raft_peer_message message;
  raft_i64 *terms;
  unsigned char *kinds;
  unsigned int *sizes;
  raft_mask *old_masks;
  raft_mask *new_masks;
  raft_mask *learner_masks;
} k_decoded_peer;
static void k_decoded_peer_free(k_decoded_peer *decoded){
  int i,count=0;
  if(!decoded) return;
  if(decoded->message.type==RAFT_MSG_APPEND) count=decoded->message.append_entries.entry_count;
  for(i=0;i<count;i++){
    if(decoded->old_masks) k_mask_free(&decoded->old_masks[i]);
    if(decoded->new_masks) k_mask_free(&decoded->new_masks[i]);
    if(decoded->learner_masks) k_mask_free(&decoded->learner_masks[i]);
  }
  if(decoded->message.type==RAFT_MSG_INSTALL_SNAPSHOT){
    k_mask_free(&decoded->message.install_snapshot.snapshot_cfg_old);
    k_mask_free(&decoded->message.install_snapshot.snapshot_cfg_new);
    k_mask_free(&decoded->message.install_snapshot.snapshot_cfg_learners);
  }
  K_FREE(decoded->terms);
  K_FREE(decoded->kinds);
  K_FREE(decoded->sizes);
  K_FREE(decoded->old_masks);
  K_FREE(decoded->new_masks);
  K_FREE(decoded->learner_masks);
  memset(decoded,0,sizeof(*decoded));
}
static int k_peer_encode(const raft_peer_message *message,k_buf *out){
  int i,count,encoded_count;
  k_u32 count_offset;
  k_u64 data_size;
  const unsigned char *entry_data;
  if(!message||!out) return -1;
  memset(out,0,sizeof(*out));
  k_buf_u8(out,(k_u8)message->type);
  k_buf_i32(out,(k_i32)message->from);
  k_buf_i32(out,(k_i32)message->to);
  k_buf_i64(out,(k_i64)message->term);
  switch(message->type){
    case RAFT_MSG_REQUEST_VOTE:
      k_buf_i64(out,(k_i64)message->request_vote.term);
      k_buf_i64(out,(k_i64)message->request_vote.last_log_index);
      k_buf_i64(out,(k_i64)message->request_vote.last_log_term);
      k_buf_i32(out,(k_i32)message->request_vote.candidate_id);
      k_buf_u8(out,(k_u8)(message->request_vote.pre_vote!=0));
      break;
    case RAFT_MSG_REQUEST_VOTE_RESULT:
      k_buf_i64(out,(k_i64)message->request_vote_result.term);
      k_buf_u8(out,(k_u8)(message->request_vote_result.vote_granted!=0));
      k_buf_u8(out,(k_u8)(message->request_vote_result.pre_vote!=0));
      break;
    case RAFT_MSG_APPEND:
      count=message->append_entries.entry_count;
      if(count<0||count>65536||(count&&!message->append_entries.entry_terms)){
        out->err=1;
        break;
      }
      k_buf_i64(out,(k_i64)message->append_entries.term);
      k_buf_i64(out,(k_i64)message->append_entries.prev_log_index);
      k_buf_i64(out,(k_i64)message->append_entries.prev_log_term);
      k_buf_i64(out,(k_i64)message->append_entries.leader_commit);
      k_buf_i32(out,(k_i32)message->append_entries.leader_id);
      k_buf_i64(out,(k_i64)message->append_entries.read_context);
      count_offset=out->len;
      k_buf_u32(out,0u);
      encoded_count=0;
      data_size=0;
      for(i=0;i<count;i++){
        raft_mask empty={0};
        raft_mask old_mask=message->append_entries.entry_cfg_old?message->append_entries.entry_cfg_old[i]:empty;
        raft_mask new_mask=message->append_entries.entry_cfg_new?message->append_entries.entry_cfg_new[i]:empty;
        raft_mask learner_mask=message->append_entries.entry_cfg_learners?message->append_entries.entry_cfg_learners[i]:empty;
        k_u32 entry_size=message->append_entries.entry_data_sizes?message->append_entries.entry_data_sizes[i]:0u;
        k_u64 wire_size=13u;
        if(old_mask.id_count<0||old_mask.id_count>K_MAX_NODES||(old_mask.id_count&&!old_mask.ids)||
           new_mask.id_count<0||new_mask.id_count>K_MAX_NODES||(new_mask.id_count&&!new_mask.ids)||
           learner_mask.id_count<0||learner_mask.id_count>K_MAX_NODES||(learner_mask.id_count&&!learner_mask.ids)){
          out->err=1;
          break;
        }
        wire_size+=12u+4u*(k_u64)(old_mask.id_count+new_mask.id_count+learner_mask.id_count);
        if((k_u64)out->len+wire_size+data_size+(k_u64)entry_size>(k_u64)K_FRAME_MAX) break;
        k_buf_i64(out,(k_i64)message->append_entries.entry_terms[i]);
        k_buf_u8(out,message->append_entries.entry_kinds?message->append_entries.entry_kinds[i]:(k_u8)RAFT_ENTRY_COMMAND);
        k_buf_u32(out,entry_size);
        k_buf_mask(out,old_mask);
        k_buf_mask(out,new_mask);
        k_buf_mask(out,learner_mask);
        data_size+=(k_u64)entry_size;
        encoded_count++;
      }
      if(count>0&&encoded_count==0) out->err=1;
      if(!out->err) k_write_u32(out->data+count_offset,(k_u32)encoded_count);
      entry_data=(const unsigned char *)message->append_entries.entry_data;
      for(i=0;i<encoded_count;i++){
        k_u32 entry_size=message->append_entries.entry_data_sizes?message->append_entries.entry_data_sizes[i]:0u;
        if(entry_size&&!entry_data){ out->err=1; break; }
        k_buf_bytes(out,entry_data,entry_size);
        if(entry_size) entry_data+=entry_size;
      }
      break;
    case RAFT_MSG_APPEND_RESULT:
      k_buf_i64(out,(k_i64)message->append_entries_result.term);
      k_buf_i64(out,(k_i64)message->append_entries_result.rejected);
      k_buf_i64(out,(k_i64)message->append_entries_result.last_log_index);
      k_buf_i64(out,(k_i64)message->append_entries_result.conflict_term);
      k_buf_i64(out,(k_i64)message->append_entries_result.conflict_first_index);
      k_buf_i64(out,(k_i64)message->append_entries_result.read_context);
      k_buf_u8(out,(k_u8)(message->append_entries_result.success!=0));
      break;
    case RAFT_MSG_INSTALL_SNAPSHOT:
      k_buf_i64(out,(k_i64)message->install_snapshot.term);
      k_buf_i64(out,(k_i64)message->install_snapshot.snapshot_last_index);
      k_buf_i64(out,(k_i64)message->install_snapshot.snapshot_last_term);
      k_buf_i64(out,(k_i64)message->install_snapshot.snapshot_offset);
      k_buf_i64(out,(k_i64)message->install_snapshot.snapshot_data_size);
      k_buf_i64(out,(k_i64)message->install_snapshot.snapshot_chunk_size);
      k_buf_mask(out,message->install_snapshot.snapshot_cfg_old);
      k_buf_mask(out,message->install_snapshot.snapshot_cfg_new);
      k_buf_mask(out,message->install_snapshot.snapshot_cfg_learners);
      k_buf_u8(out,(k_u8)(message->install_snapshot.snapshot_done!=0));
      k_buf_i32(out,(k_i32)message->install_snapshot.leader_id);
      if(message->install_snapshot.snapshot_chunk_size<0||message->install_snapshot.snapshot_chunk_size>(raft_i64)K_FRAME_MAX) out->err=1;
      else k_buf_bytes(out,message->install_snapshot.snapshot_data,(k_u32)message->install_snapshot.snapshot_chunk_size);
      break;
    case RAFT_MSG_INSTALL_SNAPSHOT_RESULT:
      k_buf_i64(out,(k_i64)message->install_snapshot_result.term);
      k_buf_i64(out,(k_i64)message->install_snapshot_result.last_included_index);
      break;
    case RAFT_MSG_READ_INDEX:
      k_buf_i64(out,(k_i64)message->read_index_req.term);
      k_buf_i64(out,(k_i64)message->read_index_req.context);
      break;
    case RAFT_MSG_READ_INDEX_RESULT:
      k_buf_i64(out,(k_i64)message->read_index_result.term);
      k_buf_i64(out,(k_i64)message->read_index_result.read_index);
      k_buf_i64(out,(k_i64)message->read_index_result.context);
      break;
    case RAFT_MSG_TIMEOUT_NOW:
      k_buf_i64(out,(k_i64)message->timeout_now.term);
      k_buf_i64(out,(k_i64)message->timeout_now.last_log_index);
      k_buf_i64(out,(k_i64)message->timeout_now.last_log_term);
      k_buf_i32(out,(k_i32)message->timeout_now.leader_id);
      break;
    default:
      out->err=1;
      break;
  }
  if(out->err||out->len>K_FRAME_MAX){
    k_buf_free(out);
    return -1;
  }
  return 0;
}
static int k_peer_decode(k_decoded_peer *decoded,const k_u8 *data,k_u32 size){
  k_reader reader;
  raft_peer_message *message;
  k_u32 count,i,total_data=0;
  if(!decoded||!data) return -1;
  memset(decoded,0,sizeof(*decoded));
  memset(&reader,0,sizeof(reader));
  reader.data=data;
  reader.len=size;
  message=&decoded->message;
  message->type=(int)k_reader_u8(&reader);
  message->from=(int)k_reader_i32(&reader);
  message->to=(int)k_reader_i32(&reader);
  message->term=(raft_i64)k_reader_i64(&reader);
  switch(message->type){
    case RAFT_MSG_REQUEST_VOTE:
      message->request_vote.term=(raft_i64)k_reader_i64(&reader);
      message->request_vote.last_log_index=(raft_i64)k_reader_i64(&reader);
      message->request_vote.last_log_term=(raft_i64)k_reader_i64(&reader);
      message->request_vote.candidate_id=(int)k_reader_i32(&reader);
      message->request_vote.pre_vote=(int)k_reader_u8(&reader);
      break;
    case RAFT_MSG_REQUEST_VOTE_RESULT:
      message->request_vote_result.term=(raft_i64)k_reader_i64(&reader);
      message->request_vote_result.vote_granted=(int)k_reader_u8(&reader);
      message->request_vote_result.pre_vote=(int)k_reader_u8(&reader);
      break;
    case RAFT_MSG_APPEND:
      message->append_entries.term=(raft_i64)k_reader_i64(&reader);
      message->append_entries.prev_log_index=(raft_i64)k_reader_i64(&reader);
      message->append_entries.prev_log_term=(raft_i64)k_reader_i64(&reader);
      message->append_entries.leader_commit=(raft_i64)k_reader_i64(&reader);
      message->append_entries.leader_id=(int)k_reader_i32(&reader);
      message->append_entries.read_context=(raft_u64)k_reader_i64(&reader);
      count=k_reader_u32(&reader);
      if(reader.err||count>65536u) goto fail;
      message->append_entries.entry_count=(int)count;
      if(count){
        decoded->terms=(raft_i64 *)K_CALLOC(count,sizeof(raft_i64));
        decoded->kinds=(unsigned char *)K_CALLOC(count,1u);
        decoded->sizes=(unsigned int *)K_CALLOC(count,sizeof(unsigned int));
        decoded->old_masks=(raft_mask *)K_CALLOC(count,sizeof(raft_mask));
        decoded->new_masks=(raft_mask *)K_CALLOC(count,sizeof(raft_mask));
        decoded->learner_masks=(raft_mask *)K_CALLOC(count,sizeof(raft_mask));
        if(!decoded->terms||!decoded->kinds||!decoded->sizes||!decoded->old_masks||!decoded->new_masks||!decoded->learner_masks) goto fail;
      }
      for(i=0;i<count;i++){
        decoded->terms[i]=(raft_i64)k_reader_i64(&reader);
        decoded->kinds[i]=k_reader_u8(&reader);
        decoded->sizes[i]=k_reader_u32(&reader);
        if(decoded->sizes[i]>K_FRAME_MAX-total_data) goto fail;
        total_data+=decoded->sizes[i];
        if(k_reader_mask(&reader,&decoded->old_masks[i])!=0||k_reader_mask(&reader,&decoded->new_masks[i])!=0||k_reader_mask(&reader,&decoded->learner_masks[i])!=0) goto fail;
      }
      if(reader.err||total_data!=reader.len-reader.off) goto fail;
      message->append_entries.entry_terms=decoded->terms;
      message->append_entries.entry_kinds=decoded->kinds;
      message->append_entries.entry_data_sizes=decoded->sizes;
      message->append_entries.entry_cfg_old=decoded->old_masks;
      message->append_entries.entry_cfg_new=decoded->new_masks;
      message->append_entries.entry_cfg_learners=decoded->learner_masks;
      message->append_entries.entry_data=k_reader_bytes(&reader,total_data);
      break;
    case RAFT_MSG_APPEND_RESULT:
      message->append_entries_result.term=(raft_i64)k_reader_i64(&reader);
      message->append_entries_result.rejected=(raft_i64)k_reader_i64(&reader);
      message->append_entries_result.last_log_index=(raft_i64)k_reader_i64(&reader);
      message->append_entries_result.conflict_term=(raft_i64)k_reader_i64(&reader);
      message->append_entries_result.conflict_first_index=(raft_i64)k_reader_i64(&reader);
      message->append_entries_result.read_context=(raft_u64)k_reader_i64(&reader);
      message->append_entries_result.success=(int)k_reader_u8(&reader);
      break;
    case RAFT_MSG_INSTALL_SNAPSHOT:
      message->install_snapshot.term=(raft_i64)k_reader_i64(&reader);
      message->install_snapshot.snapshot_last_index=(raft_i64)k_reader_i64(&reader);
      message->install_snapshot.snapshot_last_term=(raft_i64)k_reader_i64(&reader);
      message->install_snapshot.snapshot_offset=(raft_i64)k_reader_i64(&reader);
      message->install_snapshot.snapshot_data_size=(raft_i64)k_reader_i64(&reader);
      message->install_snapshot.snapshot_chunk_size=(raft_i64)k_reader_i64(&reader);
      if(k_reader_mask(&reader,&message->install_snapshot.snapshot_cfg_old)!=0||k_reader_mask(&reader,&message->install_snapshot.snapshot_cfg_new)!=0||k_reader_mask(&reader,&message->install_snapshot.snapshot_cfg_learners)!=0) goto fail;
      message->install_snapshot.snapshot_done=(int)k_reader_u8(&reader);
      message->install_snapshot.leader_id=(int)k_reader_i32(&reader);
      if(message->install_snapshot.snapshot_chunk_size<0||(k_u64)message->install_snapshot.snapshot_chunk_size>(k_u64)(reader.len-reader.off)) goto fail;
      message->install_snapshot.snapshot_data=k_reader_bytes(&reader,(k_u32)message->install_snapshot.snapshot_chunk_size);
      break;
    case RAFT_MSG_INSTALL_SNAPSHOT_RESULT:
      message->install_snapshot_result.term=(raft_i64)k_reader_i64(&reader);
      message->install_snapshot_result.last_included_index=(raft_i64)k_reader_i64(&reader);
      break;
    case RAFT_MSG_READ_INDEX:
      message->read_index_req.term=(raft_i64)k_reader_i64(&reader);
      message->read_index_req.context=(raft_u64)k_reader_i64(&reader);
      break;
    case RAFT_MSG_READ_INDEX_RESULT:
      message->read_index_result.term=(raft_i64)k_reader_i64(&reader);
      message->read_index_result.read_index=(raft_i64)k_reader_i64(&reader);
      message->read_index_result.context=(raft_u64)k_reader_i64(&reader);
      break;
    case RAFT_MSG_TIMEOUT_NOW:
      message->timeout_now.term=(raft_i64)k_reader_i64(&reader);
      message->timeout_now.last_log_index=(raft_i64)k_reader_i64(&reader);
      message->timeout_now.last_log_term=(raft_i64)k_reader_i64(&reader);
      message->timeout_now.leader_id=(int)k_reader_i32(&reader);
      break;
    default:
      goto fail;
  }
  if(reader.err||reader.off!=reader.len||message->from<=0||message->to<=0) goto fail;
  return 0;
fail:
  k_decoded_peer_free(decoded);
  return -1;
}
static void k_snapshot_cache_free(k_snapshot_cache *cache){
  if(!cache) return;
  K_FREE(cache->old_ids);
  K_FREE(cache->new_ids);
  K_FREE(cache->learner_ids);
  memset(cache,0,sizeof(*cache));
}
static int k_snapshot_cache_set(k_snapshot_cache *cache,raft_i64 index,raft_i64 term,raft_i64 size,raft_mask old_mask,raft_mask new_mask,raft_mask learner_mask){
  int *old_ids=0,*new_ids=0,*learner_ids=0;
  int old_count=0,new_count=0,learner_count=0;
  if(!cache) return -1;
  if(k_mask_copy(&old_ids,&old_count,old_mask)!=0) goto fail;
  if(k_mask_copy(&new_ids,&new_count,new_mask)!=0) goto fail;
  if(k_mask_copy(&learner_ids,&learner_count,learner_mask)!=0) goto fail;
  K_FREE(cache->old_ids);
  K_FREE(cache->new_ids);
  K_FREE(cache->learner_ids);
  cache->old_ids=old_ids;
  cache->old_count=old_count;
  cache->new_ids=new_ids;
  cache->new_count=new_count;
  cache->learner_ids=learner_ids;
  cache->learner_count=learner_count;
  cache->index=index;
  cache->term=term;
  cache->size=size;
  return 0;
fail:
  K_FREE(old_ids);
  K_FREE(new_ids);
  K_FREE(learner_ids);
  return -1;
}
static int k_path_suffix(char *out,const char *base,const char *suffix){
  size_t base_len,suffix_len;
  if(!out||!base||!suffix) return -1;
  base_len=strlen(base);
  suffix_len=strlen(suffix);
  if(base_len+suffix_len+1u>K_URI_MAX) return -1;
  memcpy(out,base,base_len);
  memcpy(out+base_len,suffix,suffix_len+1u);
  return 0;
}
static int k_path_snapshot(char *out,const char *base,raft_i64 index){
  size_t base_len;
  if(!out||!base||index<0) return -1;
  base_len=strlen(base);
  if(base_len+40u>K_URI_MAX) return -1;
  memcpy(out,base,base_len);
  sprintf(out+base_len,".snap.%020" K_I64_FMT,(k_i64)index);
  return 0;
}
static int k_path_wal_segment(char *out,const char *base,k_u64 segment){
  size_t base_len;
  if(!out||!base) return -1;
  base_len=strlen(base);
  if(base_len+32u>K_URI_MAX) return -1;
  memcpy(out,base,base_len);
  sprintf(out+base_len,".wal.%020" K_U64_FMT,segment);
  return 0;
}
static int k_wal_pos_cmp(k_wal_pos a,k_wal_pos b){
  if(a.segment<b.segment) return -1;
  if(a.segment>b.segment) return 1;
  if(a.offset<b.offset) return -1;
  if(a.offset>b.offset) return 1;
  return 0;
}
static void k_wal_meta_slot_encode(k_u8 *slot,const k_wal_meta_state *state){
  k_u32 crc;
  memset(slot,0,K_WAL_META_SLOT_SIZE);
  k_write_u32(slot,K_WAL_META_MAGIC);
  k_write_u32(slot+4,K_WAL_META_VERSION);
  k_write_u64(slot+8,state->generation);
  k_write_u64(slot+16,state->record.segment);
  k_write_u64(slot+24,state->record.offset);
  k_write_u64(slot+32,state->record_size);
  k_write_u64(slot+40,state->next.segment);
  k_write_u64(slot+48,state->next.offset);
  k_crc32(slot,56u,&crc);
  k_write_u32(slot+56,crc);
}
static int k_wal_meta_slot_decode(const k_u8 *slot,k_wal_meta_state *state){
  k_u32 crc;
  if(!slot||!state||k_read_u32(slot)!=K_WAL_META_MAGIC||k_read_u32(slot+4)!=K_WAL_META_VERSION) return -1;
  k_crc32(slot,56u,&crc);
  if(crc!=k_read_u32(slot+56)) return -1;
  memset(state,0,sizeof(*state));
  state->generation=k_read_u64(slot+8);
  state->record.segment=k_read_u64(slot+16);
  state->record.offset=k_read_u64(slot+24);
  state->record_size=k_read_u64(slot+32);
  state->next.segment=k_read_u64(slot+40);
  state->next.offset=k_read_u64(slot+48);
  if(state->generation==0){
    if(state->record.segment||state->record.offset||state->record_size||state->next.segment||state->next.offset) return -1;
  }else if(state->record_size<K_WAL_HEADER_SIZE||state->record_size>(k_u64)K_STATE_MAX+K_WAL_HEADER_SIZE) return -1;
  return 0;
}
static int k_wal_meta_load(const char *base,k_wal_meta_state *state){
  char path[K_URI_MAX];
  k_u8 data[K_WAL_META_SIZE],probe;
  k_wal_meta_state a,b;
  vfs_file *file;
  int ok_a,ok_b,absent;
  if(!base||!state||k_path_suffix(path,base,".wal.meta")!=0) return -1;
  file=vfs_open(path);
  if(!file) return -1;
  if(vfs_read(file,0,data,sizeof(data))!=0){
    absent=vfs_read(file,0,&probe,1u)!=0;
    vfs_close(file);
    return absent?0:-1;
  }
  vfs_close(file);
  ok_a=k_wal_meta_slot_decode(data,&a)==0;
  ok_b=k_wal_meta_slot_decode(data+K_WAL_META_SLOT_GAP,&b)==0;
  if(!ok_a&&!ok_b) return -1;
  if(ok_a&&(!ok_b||a.generation>b.generation||(a.generation==b.generation&&k_wal_pos_cmp(a.next,b.next)>0))){
    *state=a;
    state->slot=0;
  }else{
    *state=b;
    state->slot=1;
  }
  return 1;
}
static int k_wal_meta_init(const char *base){
  char path[K_URI_MAX];
  k_u8 data[K_WAL_META_SIZE];
  k_wal_meta_state empty;
  vfs_file *file;
  if(!base||k_path_suffix(path,base,".wal.meta")!=0) return -1;
  memset(&empty,0,sizeof(empty));
  empty.slot=1;
  memset(data,0,sizeof(data));
  k_wal_meta_slot_encode(data,&empty);
  k_wal_meta_slot_encode(data+K_WAL_META_SLOT_GAP,&empty);
  file=vfs_open(path);
  if(!file) return -1;
  if(vfs_write(file,0,data,sizeof(data))!=0||vfs_sync(file)!=0){
    vfs_close(file);
    return -1;
  }
  vfs_close(file);
  return 0;
}
static void k_cfg_defaults(k_cfg *cfg){
  if(!cfg) return;
  cfg->seed=K_DEFAULT_SEED;
  cfg->wal_seg_size=K_DEFAULT_WAL_SEG_SIZE;
  cfg->flush_timeout_ms=K_DEFAULT_FLUSH_TIMEOUT_MS;
  cfg->flush_item_limit=K_DEFAULT_FLUSH_ITEM_LIMIT;
  cfg->flush_bytes_limit=K_DEFAULT_FLUSH_BYTES_LIMIT;
  cfg->snapshot_segments=K_DEFAULT_SNAPSHOT_SEGMENTS;
  cfg->snapshot_entries=K_DEFAULT_SNAPSHOT_ENTRIES;
  cfg->poll_ms=K_DEFAULT_POLL_MS;
}
static int k_cfg_validate(const k_cfg *cfg){
  if(!cfg||!cfg->wal_seg_size||!cfg->flush_timeout_ms||!cfg->flush_item_limit) return -1;
  if(cfg->flush_item_limit>65536u||cfg->flush_bytes_limit>268435456u||!cfg->snapshot_segments||!cfg->snapshot_entries||!cfg->poll_ms||cfg->poll_ms>60000u) return -1;
  return 0;
}
static int k_cfg_store(const char *base,const k_cfg *cfg){
  char path[K_URI_MAX];
  k_u8 data[K_CFG_SIZE];
  k_u8 *payload=data+K_CFG_HEADER_SIZE;
  k_u32 crc;
  vfs_file *file;
  if(!base||k_cfg_validate(cfg)!=0||k_path_suffix(path,base,".cfg")!=0) return -1;
  memset(data,0,sizeof(data));
  k_write_u64(payload,cfg->seed);
  k_write_u64(payload+8,cfg->wal_seg_size);
  k_write_u32(payload+16,cfg->flush_timeout_ms);
  k_write_u32(payload+20,cfg->flush_item_limit);
  k_write_u32(payload+36,cfg->flush_bytes_limit);
  k_write_u32(payload+24,cfg->snapshot_segments);
  k_write_u32(payload+28,cfg->snapshot_entries);
  k_write_u32(payload+32,cfg->poll_ms);
  k_crc32(payload,K_CFG_PAYLOAD_SIZE,&crc);
  k_write_u32(data,K_CFG_MAGIC);
  k_write_u32(data+4,K_CFG_VERSION);
  k_write_u32(data+8,K_CFG_PAYLOAD_SIZE);
  k_write_u32(data+12,crc);
  file=vfs_open(path);
  if(!file) return -1;
  if(vfs_write(file,0,data,sizeof(data))!=0||vfs_sync(file)!=0){
    vfs_close(file);
    return -1;
  }
  vfs_close(file);
  return 0;
}
static int k_cfg_load(const char *base,k_cfg *cfg){
  char path[K_URI_MAX];
  k_u8 data[K_CFG_SIZE],probe;
  const k_u8 *payload=data+K_CFG_HEADER_SIZE;
  k_u32 crc;
  vfs_file *file;
  if(!base||!cfg||k_path_suffix(path,base,".cfg")!=0) return -1;
  file=vfs_open(path);
  if(!file) return -1;
  if(vfs_read(file,0,data,sizeof(data))!=0){
    int absent=vfs_read(file,0,&probe,1u)!=0;
    vfs_close(file);
    return absent?1:-1;
  }
  vfs_close(file);
  if(k_read_u32(data)!=K_CFG_MAGIC||k_read_u32(data+4)!=K_CFG_VERSION||k_read_u32(data+8)!=K_CFG_PAYLOAD_SIZE) return -1;
  k_crc32(payload,K_CFG_PAYLOAD_SIZE,&crc);
  if(crc!=k_read_u32(data+12)) return -1;
  cfg->seed=k_read_u64(payload);
  cfg->wal_seg_size=k_read_u64(payload+8);
  cfg->flush_timeout_ms=k_read_u32(payload+16);
  cfg->flush_item_limit=k_read_u32(payload+20);
  cfg->flush_bytes_limit=k_read_u32(payload+36);
  cfg->snapshot_segments=k_read_u32(payload+24);
  cfg->snapshot_entries=k_read_u32(payload+28);
  cfg->poll_ms=k_read_u32(payload+32);
  return k_cfg_validate(cfg);
}
static int k_cfg_open(const char *base,k_cfg *cfg){
  int rc=k_cfg_load(base,cfg);
  if(rc==0) return 0;
  if(rc<0) return -1;
  k_cfg_defaults(cfg);
  return k_cfg_store(base,cfg);
}
static int k_cfg_reset(const char *base){
  k_cfg cfg;
  if(!base||strlen(base)>=K_URI_MAX||strstr(base,"://")==0) return -1;
  k_cfg_defaults(&cfg);
  return k_cfg_store(base,&cfg);
}
static int k_file_read(k_file_io *io,unsigned char *buf,unsigned int size){
  if(!io||!buf||vfs_read(io->file,io->off,buf,size)!=0) return -1;
  k_crc32_update(&io->crc,buf,size);
  io->off+=(k_u64)size;
  return 0;
}
static int k_file_read_cb(void *ud,unsigned char *buf,unsigned int size){
  return k_file_read((k_file_io *)ud,buf,size);
}
/* ================= Server: snapshot load & cluster membership ================= */
/* Full unsigned 64-bit string-to-integer (strtoull equivalent).  MSVC 6.0 has no
   _strtoui64 (only the signed, decimal, endptr-less _atoi64), so provide the
   complete semantics here: leading whitespace, optional sign, base 0 (auto) or
   2-36, saturating overflow, endptr.  errno is not set (callers here do not
   consult it). */
static k_u64 k_strtou64(const char *s,char **end,int base){
  const char *p;
  char c;
  k_u64 value,limit,cutoff,cutlim;
  unsigned int d;
  int any,neg,overflow;
  if(!s){ if(end) *end=0; return 0; }
  p=s;
  value=0; any=0; neg=0; overflow=0;
  while(*p==' '||*p=='\t'||*p=='\n'||*p=='\r'||*p=='\f'||*p=='\v') p++;
  if(*p=='+') p++;
  else if(*p=='-'){ neg=1; p++; }
  if(base<0||base==1||base>36){ if(end) *end=(char*)s; return 0; }
  if(base==0){
    if(*p=='0'){
      if(p[1]=='x'||p[1]=='X'){ base=16; p+=2; }
      else{ base=8; p++; }
    }else base=10;
  }else if(base==16&&*p=='0'&&(p[1]=='x'||p[1]=='X')){
    p+=2;
  }
  limit=~(k_u64)0;
  cutoff=limit/(k_u64)base;
  cutlim=limit%(k_u64)base;
  for(;;){
    c=*p;
    if(c>='0'&&c<='9') d=(unsigned int)(c-'0');
    else if(c>='a'&&c<='z') d=(unsigned int)(c-'a'+10);
    else if(c>='A'&&c<='Z') d=(unsigned int)(c-'A'+10);
    else break;
    if((int)d>=base) break;
    any=1;
    if(value>cutoff||(value==cutoff&&(k_u64)d>cutlim)) overflow=1;
    else value=value*(k_u64)base+(k_u64)d;
    p++;
  }
  if(end) *end=(char*)p;
  if(overflow) return limit;
  if(!any){ if(end) *end=(char*)s; return 0; }
  return neg?(k_u64)0-value:value;
}
/* Grow-or-update the address book: ADD carries a new node's network address, so
   the id need not be pre-listed (dynamic membership; dissertation Sec 4.4). */
static int k_server_add_address(k_server *server,int id,const char *host,k_u32 host_len,unsigned short client_port,unsigned short peer_port){
  int node_index,i;
  if(!server||id<=0||!host||host_len<1||host_len>=K_HOST_MAX) return -1;
  node_index=k_cluster_index(&server->cluster,id);
  if(node_index<0){
    /* reuse a slot freed by membership pruning before growing the book */
    for(i=0;i<server->cluster.count;i++){
      if(server->cluster.nodes[i].id==0){ node_index=i; break; }
    }
    if(node_index<0){
      if(server->cluster.count>=K_MAX_NODES) return -1;
      node_index=server->cluster.count++;
    }
  }
  server->cluster.nodes[node_index].id=id;
  memcpy(server->cluster.nodes[node_index].host,host,host_len);
  server->cluster.nodes[node_index].host[host_len]='\0';
  server->cluster.nodes[node_index].client_port=client_port;
  server->cluster.nodes[node_index].peer_port=peer_port;
  return 0;
}
static int k_snapshot_load_file(k_server *server,raft_i64 index,treap *tree){
  char path[K_URI_MAX];
  k_file_io io;
  vfs_file *file;
  k_u8 head[8];
  k_u8 host[K_HOST_MAX];
  k_u8 crc_bytes[4];
  k_u32 magic,count,host_len,i,stored_crc,computed_crc;
  int id,rc;
  if(!server||!tree||k_path_snapshot(path,server->base,index)!=0) return -1;
  file=vfs_open(path);
  if(!file) return -1;
  io.file=file;
  io.off=0;
  k_crc32_init(&io.crc);
  /* The snapshot leads with a small header: magic, then the address book
     (id/host/ports per node), then the treap bytes, then a CRC32 trailer
     covering everything before it.  The address book is routing state, but
     it must be restored so a restarted node can dial its peers; it is merged
     into the (static-spec) book via grow-or-update. */
  if(k_file_read(&io,head,8)!=0){ vfs_close(file); return -1; }
  magic=k_read_u32(head);
  count=k_read_u32(head+4);
  if(magic!=K_SNAPSHOT_MAGIC||count>K_MAX_NODES){ vfs_close(file); return -1; }
  for(i=0;i<count;i++){
    unsigned short client_port,peer_port;
    if(k_file_read(&io,head,8)!=0){ vfs_close(file); return -1; }
    id=(int)k_read_i32(head);
    host_len=k_read_u32(head+4);
    if(host_len<1u||host_len>=K_HOST_MAX){ vfs_close(file); return -1; }
    if(k_file_read(&io,host,host_len)!=0){ vfs_close(file); return -1; }
    if(k_file_read(&io,head,4)!=0){ vfs_close(file); return -1; }
    client_port=(unsigned short)k_read_u16(head);
    peer_port=(unsigned short)k_read_u16(head+2);
    if(k_server_add_address(server,id,(const char *)host,host_len,client_port,peer_port)!=0){ vfs_close(file); return -1; }
  }
  rc=treap_load(tree,k_file_read_cb,&io);
  if(rc!=0){ vfs_close(file); return -1; }
  k_crc32_final(&io.crc,&computed_crc);
  if(vfs_read(file,io.off,crc_bytes,4)!=0){ vfs_close(file); return -1; }
  stored_crc=k_read_u32(crc_bytes);
  vfs_close(file);
  return stored_crc==computed_crc?0:-1;
}
static int k_membership_has(const int *ids,int count,int id){
  int i;
  for(i=0;i<count;i++) if(ids[i]==id) return 1;
  return 0;
}
/* is `id` in the dynamic member set (voters or learners)? */
static int k_membership_contains(const k_server *server,int id){
  return k_membership_has(server->voters,server->voter_count,id)||k_membership_has(server->learners,server->learner_count,id)||k_membership_has(server->pending,server->pending_count,id);
}
/* update the membership snapshot from a CONFIG apply entry.  Voters are the
   UNION of old and new sets: during joint consensus both are still live voters
   (and must stay connected), and once C_new is applied old==new so the union
   collapses to the final config.  Learners are the entry's learner set. */
static void k_membership_update(k_server *server,const int *old_ids,int old_count,const int *new_ids,int new_count,const int *learner_ids,int learner_count){
  int voters[K_MAX_NODES];
  int count=0,i;
  if(!server) return;
  for(i=0;i<old_count&&count<K_MAX_NODES;i++)
    if(!k_membership_has(voters,count,old_ids[i])) voters[count++]=old_ids[i];
  for(i=0;i<new_count&&count<K_MAX_NODES;i++)
    if(!k_membership_has(voters,count,new_ids[i])) voters[count++]=new_ids[i];
  server->voter_count=count;
  for(i=0;i<count;i++) server->voters[i]=voters[i];
  server->learner_count=learner_count;
  for(i=0;i<learner_count;i++) server->learners[i]=learner_ids[i];
  /* drop any pending catch-up node that has graduated into the config */
  for(i=0;i<server->pending_count;){
    if(k_membership_has(voters,count,server->pending[i])||k_membership_has(learner_ids,learner_count,server->pending[i])){
      server->pending[i]=server->pending[server->pending_count-1];
      server->pending_count--;
    }else i++;
  }
}
/* ================= Server: snapshot read ================= */
static void k_restore_free(k_restore *restore){
  int i;
  if(!restore) return;
  k_mask_free(&restore->persist.snapshot_cfg_old);
  k_mask_free(&restore->persist.snapshot_cfg_new);
  k_mask_free(&restore->persist.snapshot_cfg_learners);
  for(i=0;i<restore->persist.log_entry_count;i++){
    k_mask_free(&restore->entries[i].cfg_old);
    k_mask_free(&restore->entries[i].cfg_new);
    k_mask_free(&restore->entries[i].cfg_learners);
  }
  K_FREE(restore->entries);
  for(i=0;i<restore->block_count;i++) K_FREE(restore->blocks[i]);
  K_FREE(restore->blocks);
  memset(restore,0,sizeof(*restore));
}
/* Copy entry bytes into an owned block and return the stable pointer the entry will
   hold.  Blocks are never reallocated (a new one is added when the current is full), so
   earlier entries keep valid pointers. */
static int k_restore_blocks_reserve(k_restore *restore){
  k_u8 **nb;
  int cap;
  if(restore->block_count<restore->block_capacity) return 0;
  cap=restore->block_capacity?restore->block_capacity*2:8;
  nb=(k_u8 **)K_MALLOC((k_u64)cap*sizeof(k_u8 *));
  if(!nb) return -1;
  memset(nb,0,(k_u64)cap*sizeof(k_u8 *));
  if(restore->block_count) memcpy(nb,restore->blocks,(k_u64)restore->block_count*sizeof(k_u8 *));
  K_FREE(restore->blocks);
  restore->blocks=nb;
  restore->block_capacity=cap;
  return 0;
}
static const k_u8 *k_restore_store(k_restore *restore,const k_u8 *data,k_u32 size){
  const k_u8 *out;
  if(!size) return 0;
  if(!data) return 0;
  if(!restore->block_size) restore->block_size=262144u;             /* 256 KiB blocks */
  if(!restore->blocks||restore->block_used+size>restore->block_size){
    if(k_restore_blocks_reserve(restore)!=0) return 0;
    {
      k_u32 want=size>restore->block_size?size:restore->block_size;
      k_u8 *blk=(k_u8 *)K_MALLOC((k_u64)want);
      if(!blk) return 0;
      restore->blocks[restore->block_count++]=blk;
      if(want>restore->block_size) restore->block_size=want;         /* oversized entry */
      restore->block_used=0;
    }
  }
  out=restore->blocks[restore->block_count-1]+restore->block_used;
  memcpy((void *)out,data,size);
  restore->block_used+=size;
  return out;
}
/* Insert one log entry into the reconstructed view.  Entries must end up ascending by
   index, and a log REWRITE (truncation) means a later record re-states an index that an
   earlier record already carried - the later version wins, which is why this is not a
   plain append. */
static int k_restore_put_entry(k_restore *restore,raft_i64 index,raft_i64 term,int kind,
                               raft_mask cfg_old,raft_mask cfg_new,raft_mask cfg_learners,
                               const k_u8 *data,k_u32 data_size,raft_i64 base_floor){
  raft_persist_entry *entry;
  int n,i;
  if(index<=base_floor){
    /* Already covered by the snapshot this recovery starts from. */
    k_mask_free(&cfg_old);
    k_mask_free(&cfg_new);
    k_mask_free(&cfg_learners);
    return 0;
  }
  n=restore->persist.log_entry_count;
  if(n>0&&restore->entries[n-1].index<index){
    /* common case: the log moves forward */
  }else{
    /* same index (rewrite) or a regression (truncation): drop everything from the
       insertion point on, keeping the LAST version of the index */
    i=n;
    while(i>0&&restore->entries[i-1].index>=index){
      i--;
      k_mask_free(&restore->entries[i].cfg_old);
      k_mask_free(&restore->entries[i].cfg_new);
      k_mask_free(&restore->entries[i].cfg_learners);
      memset(&restore->entries[i],0,sizeof(restore->entries[i]));
    }
    restore->persist.log_entry_count=i;
    n=i;
    if(n>0&&restore->entries[n-1].index==index) n--;   /* overwrite it in place */
  }
  if(n>=restore->entry_capacity){
    int cap=restore->entry_capacity?restore->entry_capacity*2:64;
    raft_persist_entry *ne;
    ne=(raft_persist_entry *)K_CALLOC((k_u64)cap,sizeof(raft_persist_entry));
    if(!ne){
      k_mask_free(&cfg_old);
      k_mask_free(&cfg_new);
      k_mask_free(&cfg_learners);
      return -1;
    }
    if(restore->persist.log_entry_count) memcpy(ne,restore->entries,(k_u64)restore->persist.log_entry_count*sizeof(raft_persist_entry));
    K_FREE(restore->entries);
    restore->entries=ne;
    restore->entry_capacity=cap;
  }
  entry=&restore->entries[n];
  entry->index=index;
  entry->term=term;
  entry->kind=kind;
  entry->cfg_old=cfg_old;
  entry->cfg_new=cfg_new;
  entry->cfg_learners=cfg_learners;
  entry->data=data;
  entry->data_size=data_size;
  if(restore->persist.log_entry_count<n+1) restore->persist.log_entry_count=n+1;
  return 0;
}
/* Decode ONE WAL record payload into the restore view.  header_mode selects which parts
   of the record header are adopted:
     0 = entries only,
     1 = term/voted_for (always taken from the NEWEST record: a term must never regress),
     2 = the snapshot base (last_included_index/term, snapshot size, config masks) - taken
         from a record that actually carries the base recovery is using. */
#define K_RESTORE_HDR_NONE 0
#define K_RESTORE_HDR_TERMVOTE 1
#define K_RESTORE_HDR_BASE 2
static int k_state_decode_record(k_restore *restore,k_u8 *payload,k_u32 payload_size,k_u64 generation,
                                 int header_mode,raft_i64 base_floor){
  k_reader reader;
  raft_mask cfg_old,cfg_new,cfg_learners;
  raft_i64 term,voted_for,last_included,last_included_term,snapshot_size,index,entry_term;
  k_u32 count,i,data_size;
  int kind;
  if(!restore||!payload||!payload_size) return -1;
  memset(&cfg_old,0,sizeof(cfg_old));
  memset(&cfg_new,0,sizeof(cfg_new));
  memset(&cfg_learners,0,sizeof(cfg_learners));
  memset(&reader,0,sizeof(reader));
  reader.data=payload;
  reader.len=payload_size;
  term=k_reader_i64(&reader);
  voted_for=(raft_i64)k_reader_i32(&reader);
  last_included=k_reader_i64(&reader);
  last_included_term=k_reader_i64(&reader);
  snapshot_size=k_reader_i64(&reader);
  if(k_reader_mask(&reader,&cfg_old)!=0||k_reader_mask(&reader,&cfg_new)!=0||k_reader_mask(&reader,&cfg_learners)!=0) goto fail;
  count=k_reader_u32(&reader);
  if(reader.err||count>2147483647u) goto fail;
  if(header_mode&K_RESTORE_HDR_TERMVOTE){
    restore->persist.term=term;
    restore->persist.voted_for=(int)voted_for;
  }
  if(header_mode&K_RESTORE_HDR_BASE){
    k_mask_free(&restore->persist.snapshot_cfg_old);
    k_mask_free(&restore->persist.snapshot_cfg_new);
    k_mask_free(&restore->persist.snapshot_cfg_learners);
    restore->persist.last_included_index=last_included;
    restore->persist.last_included_term=last_included_term;
    restore->persist.snapshot_size=snapshot_size;
    restore->persist.snapshot_cfg_old=cfg_old;
    restore->persist.snapshot_cfg_new=cfg_new;
    restore->persist.snapshot_cfg_learners=cfg_learners;
    memset(&cfg_old,0,sizeof(cfg_old));        /* ownership moved into the restore */
    memset(&cfg_new,0,sizeof(cfg_new));
    memset(&cfg_learners,0,sizeof(cfg_learners));
  }else{
    k_mask_free(&cfg_old);
    k_mask_free(&cfg_new);
    k_mask_free(&cfg_learners);
  }
  for(i=0;i<count;i++){
    memset(&cfg_old,0,sizeof(cfg_old));
    memset(&cfg_new,0,sizeof(cfg_new));
    memset(&cfg_learners,0,sizeof(cfg_learners));
    index=k_reader_i64(&reader);
    entry_term=k_reader_i64(&reader);
    kind=(int)k_reader_u8(&reader);
    data_size=k_reader_u32(&reader);
    if(k_reader_mask(&reader,&cfg_old)!=0||k_reader_mask(&reader,&cfg_new)!=0||k_reader_mask(&reader,&cfg_learners)!=0) goto fail;
    if(reader.err) goto fail;
    {
      const k_u8 *bytes=k_reader_bytes(&reader,data_size);
      if(reader.err) goto fail;
      if(data_size&&!bytes) goto fail;
      if(k_restore_put_entry(restore,index,entry_term,kind,cfg_old,cfg_new,cfg_learners,
                             k_restore_store(restore,bytes,data_size),data_size,base_floor)!=0) goto fail;
    }
  }
  if(reader.off!=reader.len) goto fail;
  if(generation>restore->generation) restore->generation=generation;
  restore->persist.log_entries=restore->entries;
  restore->persist.log_entry_count=restore->persist.log_entry_count;
  return 0;
fail:
  return -1;
}

/* Validate a snapshot FILE without touching live state: stream it through CRC32 and
   compare with the 4-byte trailer, so a torn/partial/corrupt snapshot is rejected
   BEFORE anything is loaded, trusted or deleted.  Returns 1 = usable, 0 = absent or
   invalid. */
static int k_snapshot_verify_file(const char *base,raft_i64 index){
  char path[K_URI_MAX];
  vfs_file *file;
  k_crc32_ctx ctx;
  k_u8 buf[4096],tail[4];
  k_u64 off;
  k_u32 crc,stored;
  unsigned int n,i,got;
  int ok=1;
  if(index<=0) return 1;                     /* a zero base needs no snapshot file */
  if(!base||k_path_snapshot(path,base,index)!=0) return 0;
  file=vfs_open(path);
  if(!file) return 0;
  k_crc32_init(&ctx);
  off=0;
  for(i=0;i<4u;i++) tail[i]=0;
  got=0;                                     /* bytes held back in the 4-byte window */
  for(;;){
    n=(unsigned int)sizeof(buf);
    if(vfs_read(file,off,buf,n)!=0){
      for(i=0;i<n;i++) if(vfs_read(file,off+i,buf+i,1u)!=0) break;
      n=i;
      if(!n) break;
    }
    for(i=0;i<n;i++){
      if(got<4u){
        tail[got++]=buf[i];
        continue;
      }
      k_crc32_update(&ctx,tail,1u);
      tail[0]=tail[1]; tail[1]=tail[2]; tail[2]=tail[3]; tail[3]=buf[i];
    }
    off+=n;
  }
  vfs_close(file);
  if(off<4u||got!=4u) return 0;
  k_crc32_final(&ctx,&crc);
  stored=(k_u32)tail[0]|((k_u32)tail[1]<<8)|((k_u32)tail[2]<<16)|((k_u32)tail[3]<<24);
  if(crc!=stored) ok=0;
  printf(ok?"snapshot: verified %" K_I64_FMT " (%" K_U64_FMT " bytes)\n"
           :"snapshot: %" K_I64_FMT " FAILED CRC validation (torn or corrupt)\n",(k_i64)index,off);
  return ok;
}
static int k_snapshot_verify_file(const char *base,raft_i64 index);
static int k_wal_state_load(const char *base,k_restore *restore,k_wal_meta_state *meta){
  char path[K_URI_MAX];
  k_u8 header[K_WAL_HEADER_SIZE];
  k_u8 head[24];
  k_u8 *payload;
  k_u32 payload_size,crc;
  k_u64 offset,gen,prev_gen,seg,last_seg;
  k_u64 prev_seg,prev_seg_gen;   /* cross-segment continuity (issue #15) */
  int prev_seg_clean;
  int clean_end;
  vfs_file *file;
  int meta_rc,have,pick_base;
  k_u64 n_gen,n_seg,n_off,n_size,bad_gen;
  k_u64 bseg,boff,bsize,vseg,voff,vsize;
  raft_i64 n_base,prev_base,base_of_use;
  int have_bad,records,kept,i;
  if(!base||!restore||!meta) return -1;
  memset(restore,0,sizeof(*restore));
  meta_rc=k_wal_meta_load(base,meta);
  if(meta_rc<=0) return meta_rc;
  if(meta->generation==0) return 1;
  /* Recovery = the newest usable snapshot + every WAL record after its base, concatenated.
     Records carry only the DELTA of the log above what was already durable, so recovery
     stitches them together in generation order; a later record re-stating an index (a log
     truncation) wins.  Entry bytes are copied into owned blocks, so the record payload can
     be released immediately and the whole WAL is read exactly ONCE. */
  last_seg=meta->next.segment;
  have_bad=0;
  bad_gen=0;
  records=0;
  n_gen=0; n_seg=0; n_off=0; n_size=0; n_base=0;
  bseg=0; boff=0; bsize=0;
  vseg=0; voff=0; vsize=0;
  prev_base=0;
  prev_seg=0; prev_seg_gen=0; prev_seg_clean=0;
  for(seg=0;seg<=last_seg;seg++){
    if(k_path_wal_segment(path,base,seg)!=0) return -1;
    file=vfs_open(path);
    if(!file) continue;                       /* segment released by snapshot cleanup */
    offset=0;
    have=0;
    prev_gen=0;
    clean_end=0;
    for(;;){
      /* Cross-segment continuity (issue #15).  The generation counter is global and increases by one
         per record, so the first complete record of a segment must continue the last complete record of
         the previous EXISTING segment.  This used to be unchecked: prev_gen was reset to 0 here, so a
         stale, foreign or mis-ordered segment was accepted and could be treated as the newest state.
         Only enforced when the previous segment ended cleanly - a torn tail means the last record was
         never acked and the next record's generation may legitimately skip, so insisting on +1 there
         would reject a healthy store after a crash. */
      if(vfs_read(file,offset,header,sizeof(header))!=0){
        k_u8 probe;
        if(vfs_read(file,offset,&probe,1u)!=0){ clean_end=1; break; }   /* clean end: no byte at all */
        printf("wal: segment %" K_U64_FMT " ends with an incomplete tail at offset %" K_U64_FMT
               " (that record was never completed, so it was never acked)\n",seg,offset);
        break;
      }
      if(k_read_u32(header)!=K_WAL_MAGIC||k_read_u32(header+4)!=K_WAL_VERSION) break;
      payload_size=k_read_u32(header+16);
      gen=k_read_u64(header+8);
      if(!payload_size||payload_size>K_STATE_MAX||gen==0) break;
      if(!have&&prev_seg_clean&&prev_seg_gen&&gen!=prev_seg_gen+1u){
        printf("wal: segment %" K_U64_FMT " starts at generation %" K_U64_FMT " but segment %" K_U64_FMT
               " ended at %" K_U64_FMT " (missing or foreign segment; ignoring this segment's records)\n",
               seg,gen,prev_seg,prev_seg_gen);
        if(!have_bad){ have_bad=1; bad_gen=prev_seg_gen+1u; }
        break;
      }
      if(have&&gen!=prev_gen+1u){
        if(!have_bad){ have_bad=1; bad_gen=prev_gen+1u; }    /* a record is missing */
        break;
      }
      payload=(k_u8 *)K_MALLOC(payload_size);
      if(!payload) { vfs_close(file); return -1; }
      if(vfs_read(file,offset+K_WAL_HEADER_SIZE,payload,payload_size)!=0){
        K_FREE(payload);
        break;                                               /* short payload: torn tail */
      }
      k_crc32(payload,payload_size,&crc);
      if(crc!=k_read_u32(header+20)){
        K_FREE(payload);
        if(!have_bad){ have_bad=1; bad_gen=gen; }             /* readable but corrupt */
        break;
      }
      records++;
      if(payload_size>=24u){
        memcpy(head,payload,24u);
        if(n_base!=(raft_i64)k_read_i64(head+12)){
          prev_base=n_base;                                   /* previous distinct base */
          vseg=bseg; voff=boff; vsize=bsize;                   /* ... and where it was */
          n_base=(raft_i64)k_read_i64(head+12);
        }
      }
      bseg=seg;
      boff=offset;
      bsize=(k_u64)K_WAL_HEADER_SIZE+(k_u64)payload_size;
      /* term/vote come from the NEWEST record (a term must never regress); the snapshot
         base metadata is filled in below, from the record that carries the base in use. */
      if(k_state_decode_record(restore,payload,payload_size,gen,K_RESTORE_HDR_TERMVOTE,0)!=0){
        K_FREE(payload);
        vfs_close(file);
        return -1;
      }
      K_FREE(payload);                                        /* bytes were copied */
      n_gen=gen;
      n_seg=seg;
      n_off=offset;
      n_size=(k_u64)K_WAL_HEADER_SIZE+(k_u64)payload_size;
      have=1;
      prev_gen=gen;
      offset+=n_size;
    }
    vfs_close(file);
    if(have){ prev_seg=seg; prev_seg_gen=prev_gen; prev_seg_clean=clean_end; }
  }
  if(!records){
    printf("wal: no usable record found in segments 0..%" K_U64_FMT ": refusing to recover\n",last_seg);
    return -1;
  }
  /* Bases only advance, so a record whose base goes BACKWARDS carries a stale header
     (observed after a restart: the newest record said 0 while its own entries started
     above the snapshot index).  Prefer the last consistent (higher) base. */
  if(n_base<prev_base) n_base=prev_base;
  base_of_use=n_base;
  pick_base=1;
  if(k_snapshot_verify_file(base,n_base)!=1){
    if(n_base>0&&k_snapshot_verify_file(base,prev_base)==1){
      printf("wal: snapshot for base %" K_I64_FMT " is not usable: rolling back to base %"
             K_I64_FMT "\n",(k_i64)n_base,(k_i64)prev_base);
      base_of_use=prev_base;
      pick_base=0;
    }else if(n_base>0){
      printf("wal: no usable snapshot for base %" K_I64_FMT " (nor for %" K_I64_FMT
             "): refusing to recover\n",(k_i64)n_base,(k_i64)prev_base);
      return -1;
    }
  }
  if(have_bad&&bad_gen>n_gen){
    printf("wal: corrupt record after the recoverable position (generation %" K_U64_FMT
           " > %" K_U64_FMT "): refusing to recover\n",bad_gen,n_gen);
    return -1;
  }
  /* Snapshot metadata (last included index/term, snapshot size, config masks) from the
     record that carries the base in use: one small read, not another full pass. */
  if(base_of_use>0){
    k_u64 use_seg=pick_base?bseg:vseg,use_off=pick_base?boff:voff,use_size=pick_base?bsize:vsize;
    if(!use_size){
      if(k_snapshot_verify_file(base,base_of_use)!=1) return -1;
      printf("wal: no record carries base %" K_I64_FMT ": refusing to recover\n",(k_i64)base_of_use);
      return -1;
    }
    if(k_path_wal_segment(path,base,use_seg)!=0) return -1;
    file=vfs_open(path);
    if(!file) return -1;
    payload=(k_u8 *)K_MALLOC((k_u32)(use_size-(k_u64)K_WAL_HEADER_SIZE));
    if(!payload){ vfs_close(file); return -1; }
    if(vfs_read(file,use_off+(k_u64)K_WAL_HEADER_SIZE,payload,
                (k_u32)(use_size-(k_u64)K_WAL_HEADER_SIZE))!=0){
      K_FREE(payload);
      vfs_close(file);
      return -1;
    }
    vfs_close(file);
    if(k_state_decode_record(restore,payload,(k_u32)(use_size-(k_u64)K_WAL_HEADER_SIZE),
                             n_gen,K_RESTORE_HDR_BASE,0)!=0){
      K_FREE(payload);
      return -1;
    }
    K_FREE(payload);
  }
  if(restore->persist.last_included_index!=base_of_use){
    printf("wal: base mismatch after decoding (entry says %" K_I64_FMT ", recovery uses %"
           K_I64_FMT "): refusing to recover\n",(k_i64)restore->persist.last_included_index,
           (k_i64)base_of_use);
    return -1;
  }
  /* Drop the entries the snapshot already covers and require a contiguous log. */
  kept=0;
  for(i=0;i<restore->persist.log_entry_count;i++){
    if(restore->entries[i].index>base_of_use){
      if(kept!=i) restore->entries[kept]=restore->entries[i];
      kept++;
    }else{
      k_mask_free(&restore->entries[i].cfg_old);
      k_mask_free(&restore->entries[i].cfg_new);
      k_mask_free(&restore->entries[i].cfg_learners);
    }
  }
  restore->persist.log_entry_count=kept;
  restore->persist.log_entries=restore->entries;
  {
    raft_i64 want=base_of_use+1;
    for(i=0;i<kept;i++){
      if(restore->entries[i].index!=want){
        printf("wal: replayed log has a hole at index %" K_I64_FMT " (expected %" K_I64_FMT
               "): refusing to recover\n",(k_i64)restore->entries[i].index,(k_i64)want);
        return -1;
      }
      want++;
    }
  }
  restore->generation=n_gen;
  meta->generation=n_gen;
  meta->record.segment=n_seg;
  meta->record.offset=n_off;
  meta->record_size=n_size;
  meta->next.segment=n_seg;
  meta->next.offset=n_off+n_size;
  return 2;
}

static int k_state_load(const char *base,k_restore *restore,k_wal_meta_state *meta,int *wal_exists){
  int wal_rc;
  if(!base||!restore||!meta||!wal_exists) return -1;
  wal_rc=k_wal_state_load(base,restore,meta);
  if(wal_rc<0) return -1;
  if(wal_rc>0){
    *wal_exists=1;
    return wal_rc==2?1:0;
  }
  *wal_exists=0;
  memset(meta,0,sizeof(*meta));
  return 0;
}
static int k_state_serialize(k_server *server,const raft_persist *persist,k_ready_bundle *bundle){
  k_buf payload;
  raft_mask old_mask,new_mask,learner_mask;
  raft_i64 snapshot_size,durable_index;
  int i;
  if(!server||!persist||!bundle||persist->log_entry_count<0||(persist->log_entry_count&&!persist->log_entries)) return -1;
  memset(&payload,0,sizeof(payload));
  old_mask=k_cache_mask(server->snapshot.old_ids,server->snapshot.old_count);
  new_mask=k_cache_mask(server->snapshot.new_ids,server->snapshot.new_count);
  learner_mask=k_cache_mask(server->snapshot.learner_ids,server->snapshot.learner_count);
  snapshot_size=server->snapshot.size;
  if(persist->snapshot_dirty){
    old_mask=persist->snapshot_cfg_old;
    new_mask=persist->snapshot_cfg_new;
    learner_mask=persist->snapshot_cfg_learners;
    snapshot_size=persist->snapshot_size;
  }
  k_buf_i64(&payload,(k_i64)persist->term);
  k_buf_i32(&payload,(k_i32)persist->voted_for);
  k_buf_i64(&payload,(k_i64)persist->last_included_index);
  k_buf_i64(&payload,(k_i64)persist->last_included_term);
  k_buf_i64(&payload,(k_i64)snapshot_size);
  k_buf_mask(&payload,old_mask);
  k_buf_mask(&payload,new_mask);
  k_buf_mask(&payload,learner_mask);
  k_buf_u32(&payload,(k_u32)persist->log_entry_count);
  durable_index=persist->last_included_index;
  for(i=0;i<persist->log_entry_count;i++){
    const raft_persist_entry *entry=&persist->log_entries[i];
    k_buf_i64(&payload,(k_i64)entry->index);
    k_buf_i64(&payload,(k_i64)entry->term);
    k_buf_u8(&payload,(k_u8)entry->kind);
    k_buf_u32(&payload,(k_u32)entry->data_size);
    k_buf_mask(&payload,entry->cfg_old);
    k_buf_mask(&payload,entry->cfg_new);
    k_buf_mask(&payload,entry->cfg_learners);
    k_buf_bytes(&payload,entry->data,(k_u32)entry->data_size);
    durable_index=entry->index;
  }
  if(payload.err||payload.len>K_STATE_MAX){
    k_buf_free(&payload);
    return -1;
  }
  bundle->wal_payload=payload.data;
  bundle->wal_payload_size=payload.len;
  bundle->generation=0;  /* assigned by the WAL worker in append order */
  bundle->durable_index=durable_index;
  bundle->persist_needed=1;
  bundle->item_count=(k_u32)persist->log_entry_count;
  if(persist->snapshot_dirty){
    bundle->snapshot_dirty=1;
    bundle->old_snapshot_index=server->snapshot.index;
    if(k_snapshot_cache_set(&bundle->snapshot_after,persist->last_included_index,persist->last_included_term,persist->snapshot_size,persist->snapshot_cfg_old,persist->snapshot_cfg_new,persist->snapshot_cfg_learners)!=0) return -1;
  }
  return 0;
}
static void k_wal_files_close(k_wal_worker *worker){
  if(!worker) return;
  if(worker->seg_file){
    vfs_close(worker->seg_file);
    worker->seg_file=0;
    if(worker->open_files>0) worker->open_files--;
  }
  if(worker->meta_file){
    vfs_close(worker->meta_file);
    worker->meta_file=0;
    if(worker->open_files>0) worker->open_files--;
  }
}
/* Cached handle for the CURRENT segment; a rotation (different segment number) or a
   previous error closes the old one first.  The disk backend opens with OPEN_ALWAYS
   and no truncation, so holding a handle does not change what a later reopen sees. */
static vfs_file *k_wal_seg_file(k_wal_worker *worker,const char *path,k_u64 segment){
  if(worker->seg_file&&worker->seg_file_segment==segment) return worker->seg_file;
  if(worker->seg_file){
    vfs_close(worker->seg_file);
    worker->seg_file=0;
    if(worker->open_files>0) worker->open_files--;
  }
  worker->seg_file=vfs_open(path);
  if(!worker->seg_file) return 0;
  worker->seg_file_segment=segment;
  worker->open_files++;
  return worker->seg_file;
}
static vfs_file *k_wal_meta_file(k_wal_worker *worker,const char *path){
  if(worker->meta_file) return worker->meta_file;
  worker->meta_file=vfs_open(path);
  if(worker->meta_file) worker->open_files++;
  return worker->meta_file;
}
static int k_wal_meta_store(k_wal_worker *worker,const k_wal_meta_state *state,int durable){
  char path[K_URI_MAX];
  k_u8 slot[K_WAL_META_SLOT_SIZE];
  vfs_file *file;
  int next_slot;
  if(!worker||!state||!worker->server||k_path_suffix(path,worker->server->base,".wal.meta")!=0) return -1;
  /* A slot is only ever written when it is fsynced in the same breath.  Writing
     the slot without syncing (as an earlier version did on every batch) overwrote
     the last DURABLE copy in the page cache, so a power loss could leave both
     slots stale or torn: k_wal_meta_load would then have no usable scan start even
     though every record is intact, and the node could not start.  Now the durable
     copies are only replaced, one at a time, at a checkpoint (every
     K_WAL_META_FSYNC_EVERY records) or on rotation - so at least one CRC-valid,
     fsynced slot always remains on disk.  Between checkpoints the position lives
     in memory only; recovery rebuilds it by scanning forward from the slot. */
  worker->generation=state->generation;
  worker->next=state->next;
  if(!durable) return 0;
  next_slot=worker->meta_slot==0?1:0;
  k_wal_meta_slot_encode(slot,state);
  file=k_wal_meta_file(worker,path);
  if(!file) return -1;
  if(vfs_write(file,(k_u64)next_slot*K_WAL_META_SLOT_GAP,slot,sizeof(slot))!=0||vfs_sync(file)!=0){
    k_wal_files_close(worker);
    return -1;
  }
  worker->meta_slot=next_slot;
  return 0;
}
/* The WAL worker keeps a min-biased EWMA of how long one record's write+sync costs
   (worker->sync_us_ewma).  It is both an observability field and the input to the
   driver's adaptive flush window. */
static k_u64 k_wal_sync_now(void){
  k_u64 us=0;
  if(k_monotonic_us(&us)!=0) return 0;   /* no clock: report 0 rather than invent time */
  return us;
}
static int k_wal_append_bundle(k_wal_worker *worker,k_ready_bundle *bundle){
  char path[K_URI_MAX];
  k_u8 header[K_WAL_HEADER_SIZE];
  k_wal_meta_state meta;
  k_wal_pos record,next;
  k_u64 record_size,segment_size;
  k_u32 crc;
  int durable;
  vfs_file *file;
  if(!worker||!bundle||!bundle->wal_payload||!bundle->wal_payload_size) return -1;
  /* The WAL worker owns the monotonic generation counter: assign it here (in
     append order, single worker thread) instead of on the main thread, so the
     main thread can enqueue writes ahead of the worker without racing on the
     counter (non-blocking pipeline). */
  if(worker->generation==K_U64_C(0xffffffffffffffff)) return -1;
  bundle->generation=worker->generation+1u;
  record_size=(k_u64)K_WAL_HEADER_SIZE+(k_u64)bundle->wal_payload_size;
  segment_size=worker->server->cfg.wal_seg_size;
  record=worker->next;
  if(record.offset>0&&(record_size>segment_size||record.offset>segment_size-record_size)){
    record.segment++;
    record.offset=0;
  }
  if(k_path_wal_segment(path,worker->server->base,record.segment)!=0) return -1;
  k_crc32(bundle->wal_payload,bundle->wal_payload_size,&crc);
  k_write_u32(header,K_WAL_MAGIC);
  k_write_u32(header+4,K_WAL_VERSION);
  k_write_u64(header+8,bundle->generation);
  k_write_u32(header+16,bundle->wal_payload_size);
  k_write_u32(header+20,crc);
  file=k_wal_seg_file(worker,path,record.segment);
  if(!file) return -1;
  if(vfs_write(file,record.offset,header,sizeof(header))!=0||vfs_write(file,record.offset+K_WAL_HEADER_SIZE,bundle->wal_payload,bundle->wal_payload_size)!=0||vfs_sync(file)!=0){
    k_wal_files_close(worker);   /* invalidate: the next batch reopens */
    return -1;
  }
  next=record;
  next.offset+=record_size;
  if(record_size>=segment_size||next.offset>=segment_size){
    next.segment++;
    next.offset=0;
  }
  memset(&meta,0,sizeof(meta));
  meta.generation=bundle->generation;
  meta.record=record;
  meta.record_size=record_size;
  meta.next=next;
  /* One fsync per record: the slot is only forced to disk when the segment
     changed (the durable meta must name the segment that holds the newest
     record) or every K_WAL_META_FSYNC_EVERY records (bounds the recovery scan).
     worker->meta_since_sync starts at the threshold, so the first record after
     startup always syncs the slot. */
  worker->meta_since_sync++;
  durable=(record.segment!=worker->meta_sync_segment)||(worker->meta_since_sync>=(int)K_WAL_META_FSYNC_EVERY);
  if(durable){
    worker->meta_since_sync=0;
    worker->meta_sync_segment=record.segment;
  }
  if(k_wal_meta_store(worker,&meta,durable)!=0) return -1;
  meta.slot=worker->meta_slot;
  bundle->wal_result=meta;
  return 0;
}
static k_conn *k_conn_create(k_server *server,void *sock,int kind,int outbound,int node_index){
  k_conn *conn;
  if(!server||!sock) return 0;
  conn=(k_conn *)K_CALLOC(1,sizeof(*conn));
  if(!conn) return 0;
  conn->ud_kind=K_UD_CONN;
  conn->server=server;
  conn->sock=sock;
  conn->kind=kind;
  conn->outbound=outbound;
  conn->node_index=node_index;
  conn->next=server->connections;
  server->connections=conn;
  if(kind==K_CONN_CLIENT) server->client_connection_count++;
  server->transport->setud(sock,conn);
  return conn;
}
static void k_request_unlink(k_server *server,k_request *request){
  k_request **link;
  if(!server||!request) return;
  if(request->prev) request->prev->next=request->next;
  else if(server->requests==request) server->requests=request->next;
  /* NOTE: no early return here.  The list unlink is only possible when the request really
     is reachable through prev/head, but the HASH unlink below must run either way: bailing
     out left the bucket pointing at freed memory, so a later client result matched a stale
     request, took the "no client to answer" path and silently dropped the response - the
     client then waited forever (kdbctl hung after CONNECT and refused every command). */
  if(request->next) request->next->prev=request->prev;
  link=&server->request_hash[k_request_bucket(request)];
  if(*link==request) *link=request->hash_next;
  if(request->hash_next) request->hash_next->hash_prev=request->hash_prev;
  if(request->hash_prev) request->hash_prev->hash_next=request->hash_next;
  if(request->conn){
    if(request->conn_prev) request->conn_prev->conn_next=request->conn_next;
    else if(request->conn->requests==request) request->conn->requests=request->conn_next;
    if(request->conn_next) request->conn_next->conn_prev=request->conn_prev;
  }
  request->prev=0;
  request->next=0;
  request->hash_next=0;
  request->hash_prev=0;
  request->conn_next=0;
  request->conn_prev=0;
}
static void k_server_open_gate(k_server *server);   /* defined below: freeing an FCALL reopens it */
/* True while the server is shutting down: runtime stop requested, release in progress, or the
   driver has been told to stop.  Used to tell an expected shutdown race from a live failure. */
static int server_is_stopping(const k_server *server){
  return !server||server->stopping||server->releasing||server->stopped;
}
static void k_request_free(k_server *server,k_request *request){
  if(!request) return;
  if(request->submitted&&(!server||!server->releasing)){
    /* Contract: a request handed to Raft IS its cookie and its live payload, so it may only be
       released after the terminal result.  Freeing it earlier leaves Raft pointing at freed memory
       (observed as a page-heap read fault inside treap_blob_create on the apply path). */
    fprintf(stderr,"warning: freeing request id=%u (type=%d) while Raft may still reference it: "
                   "it was submitted and no terminal result has arrived\n",(unsigned)request->id,(int)request->type);
  }
  k_request_unlink(server,request);
  /* An FCALL holds the write gate closed from fork until commit.  If that request dies for ANY
     reason - its client disconnected, the connection was reaped, a terminal failure path only
     freed it - nobody would ever reopen the gate: gate_closed stayed 1 forever and every later
     write from every client queued in gate_head with no answer and no redirect.  Reopening here
     covers all of those paths.  Recursion-safe: open_gate clears gate_closed before draining, so
     a nested free caused by the drain sees gate_closed==0 and does nothing. */
  if(server&&server->gate_closed&&request->type==K_REQ_FCALL) k_server_open_gate(server);
  if(server){
    if(server->request_count>0) server->request_count--;
    server->request_bytes=request->tracked_bytes<=server->request_bytes?server->request_bytes-request->tracked_bytes:0;
  }
  K_FREE(request->key);
  K_FREE(request->end);
  K_FREE(request->command);
  if(request->txn) treap_abort(request->txn);
  K_FREE(request->writes.data);
  K_FREE(request->result.data);
  K_FREE(request);
}
static k_request *k_request_find(k_server *server,const void *cookie){
  k_request *request;
  if(!server||!cookie) return 0;
  for(request=server->request_hash[k_request_bucket(cookie)];request;request=request->hash_next){
    if(request==(const k_request *)cookie) return request;
  }
  return 0;
}
/* ================= Custom commands (EXEC) ================= */
typedef struct k_cmd_ctx k_cmd_ctx;
typedef int (*k_cmd_fn)(k_cmd_ctx *ctx);
struct k_cmd_ctx{
  treap *txn;        /* COW working tree (read view, incl. this command's own writes) */
  k_buf *writes;     /* write-set output (put/del record sequence) */
  k_buf *result;     /* return-value output */
  const k_u8 *args;  /* command arguments */
  k_u32 args_len;
  k_u32 write_count; /* write-set record count */
};
typedef struct k_cmd{
  const char *name;
  k_cmd_fn fn;
} k_cmd;
typedef struct k_cmd_reg{
  k_cmd cmd;
  struct k_cmd_reg *next;
} k_cmd_reg;
static k_cmd_reg *g_cmd_regs=0;
static k_cmd_fn k_cmd_find(const k_u8 *name,k_u32 name_len){
  k_cmd_reg *r;
  for(r=g_cmd_regs;r;r=r->next){
    if(strlen(r->cmd.name)==name_len&&memcmp(r->cmd.name,name,name_len)==0) return r->cmd.fn;
  }
  return 0;
}
static int k_cmd_register(const char *name,k_cmd_fn fn){
  k_cmd_reg *r;
  if(!name||!fn) return -1;
  /* Idempotent: k_server_open registers the built-in command on every open,
     so skip the append if the name is already registered (first wins). */
  if(k_cmd_find((const k_u8 *)name,(k_u32)strlen(name))) return 0;
  r=(k_cmd_reg *)K_CALLOC(1,sizeof(*r));
  if(!r) return -1;
  r->cmd.name=name;
  r->cmd.fn=fn;
  r->next=g_cmd_regs;
  g_cmd_regs=r;
  return 0;
}
static int k_cmd_get(k_cmd_ctx *ctx,const k_u8 *key,k_u32 key_len,const k_u8 **value,k_u32 *value_len){
  return treap_get(ctx->txn,key,key_len,value,value_len);
}
static int k_cmd_put(k_cmd_ctx *ctx,const k_u8 *key,k_u32 key_len,const k_u8 *value,k_u32 value_len){
  int rc;
  /* Bound exactly what apply re-parses (K_KEY_MAX / K_VALUE_MAX) so an oversized
     write-set entry fails HERE as a command error instead of at apply, where a
     failed apply is a fail-stop (fatal).  The write-set is built from command
     arguments, i.e. from client input, so an unchecked entry let a client put a
     node into an unrecoverable state. */
  if(!ctx||!key||!key_len||key_len>K_KEY_MAX||(value_len&&!value)||value_len>K_VALUE_MAX) return -1;
  rc=treap_set(ctx->txn,key,key_len,value,value_len);
  if(rc<0) return rc;
  k_buf_u8(ctx->writes,K_OP_SET);
  k_buf_u32(ctx->writes,key_len);
  k_buf_bytes(ctx->writes,key,key_len);
  k_buf_u32(ctx->writes,value_len);
  k_buf_bytes(ctx->writes,value,value_len);
  ctx->write_count++;
  return 0;
}
/* Demo command: transfer <from> <to> <amount> - an atomic two-account transfer.
   args=[from_len][from][to_len][to][amount_len][amount].
   Shows FCALL's unique value: a multi-key atomic update no single SET/CAS/DEL
   can express.  NOT idempotent: an ack-lost retry transfers twice, so real
   business use must add re-entrancy safety (idempotency key / request dedup /
   GET+CAS guard) inside the command itself. */
static int k_cmd_transfer(k_cmd_ctx *ctx){
  k_reader reader;
  k_u32 from_len,to_len,amt_len;
  const k_u8 *from,*to,*amt,*fv,*tv;
  k_u32 fvl,tvl;
  char fbuf[32],tbuf[32],abuf[32];
  k_u64 fval,tval,amount;
  int fl,tl;
  if(!ctx||!ctx->args||ctx->args_len<12) return -1;
  memset(&reader,0,sizeof(reader));
  reader.data=ctx->args;
  reader.len=ctx->args_len;
  from_len=k_reader_u32(&reader);
  if(!from_len||from_len>K_KEY_MAX) return -1;
  from=k_reader_bytes(&reader,from_len);
  to_len=k_reader_u32(&reader);
  if(!to_len||to_len>K_KEY_MAX) return -1;
  to=k_reader_bytes(&reader,to_len);
  amt_len=k_reader_u32(&reader);
  if(!amt_len||amt_len>=sizeof(abuf)) return -1;
  amt=k_reader_bytes(&reader,amt_len);
  if(reader.err||reader.off!=reader.len) return -1;
  memcpy(abuf,amt,amt_len); abuf[amt_len]=0;
  amount=k_strtou64(abuf,0,10);
  if(k_cmd_get(ctx,from,from_len,&fv,&fvl)!=1) return -1;
  if(fvl>=sizeof(fbuf)) return -1;
  memcpy(fbuf,fv,fvl); fbuf[fvl]=0;
  fval=k_strtou64(fbuf,0,10);
  if(k_cmd_get(ctx,to,to_len,&tv,&tvl)!=1) return -1;
  if(tvl>=sizeof(tbuf)) return -1;
  memcpy(tbuf,tv,tvl); tbuf[tvl]=0;
  tval=k_strtou64(tbuf,0,10);
  if(fval<amount||tval>~(k_u64)0-amount) return -1;
  fl=sprintf(fbuf,"%" K_U64_FMT,(k_u64)(fval-amount));
  tl=sprintf(tbuf,"%" K_U64_FMT,(k_u64)(tval+amount));
  if(k_cmd_put(ctx,from,from_len,(const k_u8*)fbuf,(k_u32)fl)!=0) return -1;
  if(k_cmd_put(ctx,to,to_len,(const k_u8*)tbuf,(k_u32)tl)!=0) return -1;
  k_buf_bytes(ctx->result,(const k_u8*)"ok",2);
  return ctx->result->err?-1:0;
}
/* ================= Server: request lifecycle & response ================= */
static k_request *k_request_create_server(k_server *server,k_conn *conn,k_u32 id,int type,const k_u8 *key,k_u32 key_len,const k_u8 *end,k_u32 end_len,k_u64 payload_size){
  k_request *request;
  k_u64 charge;
  if(!server||(key_len&&!key)||(end_len&&!end)) return 0;
  if(payload_size>(K_REQUEST_BYTES_MAX-(k_u64)sizeof(*request))/2u) return 0;
  charge=(k_u64)sizeof(*request)+payload_size*2u;
  if(server->request_count>=K_REQUEST_INFLIGHT_MAX||server->request_bytes>K_REQUEST_BYTES_MAX-charge) return 0;
  server->client_requests++;
  request=(k_request *)K_CALLOC(1,sizeof(*request));
  if(!request) return 0;
  request->admit_ms=server->elapsed_total_ms;
  if(key_len){
    request->key=(k_u8 *)K_MALLOC(key_len);
    if(!request->key){
      K_FREE(request);
      return 0;
    }
    memcpy(request->key,key,key_len);
  }
  if(end_len){
    request->end=(k_u8 *)K_MALLOC(end_len);
    if(!request->end){
      K_FREE(request->key);
      K_FREE(request);
      return 0;
    }
    memcpy(request->end,end,end_len);
  }
  request->conn=conn;
  if(conn){
    request->conn_next=conn->requests;
    request->conn_prev=0;
    if(conn->requests) conn->requests->conn_prev=request;
    conn->requests=request;
  }
  request->id=id;
  request->type=type;
  request->key_len=key_len;
  request->end_len=end_len;
  request->tracked_bytes=charge;
  request->next=server->requests;
  request->prev=0;
  if(server->requests) server->requests->prev=request;
  server->requests=request;
  {
    k_u32 bucket=k_request_bucket(request);
    request->hash_next=server->request_hash[bucket];
    request->hash_prev=0;
    if(server->request_hash[bucket]) server->request_hash[bucket]->hash_prev=request;
    server->request_hash[bucket]=request;
  }
  server->request_count++;
  server->request_bytes+=charge;
  return request;
}
static void k_conn_closed(k_conn *conn){
  k_server *server;
  k_conn **link;
  k_request *request;
  int i;
  if(!conn) return;
  server=conn->server;
  /* The socket object is gone (cemon frees it when the connection dies): clear our handle so no
     later send/close can touch freed memory.  cemon_close on a released socket dereferences it
     (cemon_socket_admit reads sock->loop), so a stale handle is a hard crash, not an error code. */
  conn->sock=0;
  if(server){
    for(i=0;i<server->cluster.count;i++){
      if(server->peer_conns[i]==conn){
        server->peer_conns[i]=0;
        server->peer_socks[i]=0;
      }
    }
    /* Only this connection's requests can be affected, and the list makes that O(k)
       instead of O(every in-flight request on the server).  Detaching them (rather than
       letting the sweep find them later) also means the result path can tell a torn-down
       client from a server-created request without guessing from conn==0. */
    while((request=conn->requests)!=0){
      conn->requests=request->conn_next;
      request->conn_next=0;
      request->conn_prev=0;
      request->conn=0;
    }
    for(link=&server->connections;*link;link=&(*link)->next){
      if(*link==conn){
        *link=conn->next;
        break;
      }
    }
    if(conn->kind==K_CONN_CLIENT)
      server->rx_buffer_bytes=conn->rx.len<=server->rx_buffer_bytes?server->rx_buffer_bytes-(k_u64)conn->rx.len:0;
    if(conn->kind==K_CONN_CLIENT&&server->client_connection_count>0) server->client_connection_count--;
  }
  k_rx_free(&conn->rx);
  K_FREE(conn);
}
static int k_server_send_response(k_server *server,k_conn *conn,k_u32 request_id,k_u8 status,int leader_id,const void *body,k_u32 body_size){
  const char *host=0;
  unsigned short port=0;
  int node_index;
  k_buf payload;
  int rc;
  if(!server||!conn||!conn->sock) return -1;
  node_index=k_cluster_index(&server->cluster,leader_id);
  if(status==K_STATUS_REDIRECT&&node_index>=0){
    host=server->cluster.nodes[node_index].host;
    port=server->cluster.nodes[node_index].client_port;
  }
  if(k_response_payload(&payload,request_id,status,leader_id,host,port,body,body_size)!=0) return -1;
  rc=server->transport->send_frame(conn->sock,K_CLIENT_MAGIC,K_RESPONSE,payload.data,payload.len,0);
  k_buf_free(&payload);
    if(rc!=0){
      void *dead=conn->sock;
      conn->sock=0;                      /* clear BEFORE closing: the handle is invalid either way */
      server->transport->close(dead);
    }
  return rc;
}
static int k_server_redirect(k_server *server,k_conn *conn,k_u32 request_id){
  return k_server_send_response(server,conn,request_id,K_STATUS_REDIRECT,server?server->leader_id:0,0,0);
}
static int k_server_write_inbound_snapshot(k_server *server,const raft_install_snapshot *snapshot){
  char path[K_URI_MAX];
  vfs_file *file;
  if(!server||!snapshot||snapshot->snapshot_last_index<0||snapshot->snapshot_offset<0||snapshot->snapshot_chunk_size<0) return -1;
  if((k_u64)snapshot->snapshot_chunk_size>(k_u64)K_FRAME_MAX||k_path_snapshot(path,server->base,snapshot->snapshot_last_index)!=0) return -1;
  file=vfs_open(path);
  if(!file) return -1;
  if(vfs_write(file,(k_u64)snapshot->snapshot_offset,snapshot->snapshot_data,(k_u32)snapshot->snapshot_chunk_size)!=0||(snapshot->snapshot_done&&vfs_sync(file)!=0)){
    vfs_close(file);
    return -1;
  }
  vfs_close(file);
  return 0;
}
static int k_server_peer_frame(void *ud,k_u8 type,const k_u8 *payload,k_u32 size){
  k_conn *conn=(k_conn *)ud;
  k_server *server;
  if(!conn||!conn->server) return -1;
  server=conn->server;
  if(type==K_PEER_HELLO){
    int peer_id,node_index;
    k_u32 host_len;
    unsigned short client_port,peer_port;
    const k_u8 *host;
    k_conn *existing;
    if(conn->peer_id) return -1;
    if(size<9u) return -1;
    peer_id=(int)k_read_i32(payload);
    host_len=(k_u32)payload[4];
    if(host_len<1u||host_len>=K_HOST_MAX||9u+host_len!=size) return -1;
    host=payload+5;
    client_port=(unsigned short)k_read_u16(payload+5+host_len);
    peer_port=(unsigned short)k_read_u16(payload+7+host_len);
    if(peer_id==server->id) return -1;
    node_index=k_cluster_index(&server->cluster,peer_id);
    if(node_index<0){
      /* learn an unknown peer's address from its HELLO (Sec 4.4 dynamic
         membership: a new node's bootstrap source is "any live member",
         not a pre-listed one) */
      if(k_server_add_address(server,peer_id,(const char *)host,host_len,client_port,peer_port)!=0) return -1;
      node_index=k_cluster_index(&server->cluster,peer_id);
      if(node_index<0) return -1;
    }
    if(conn->outbound){
      if(conn->node_index!=node_index) return -1;
    }else{
      existing=server->peer_conns[node_index];
      if(existing&&existing!=conn){
        /* Mutual dial, "lower id's up-dial wins": if I am ALSO dialing this
           peer and my dial is the up-dial (higher-id peer dials down to me),
           my up-dial wins and this down-dial is rejected.  Every other case
           abandons my old connection and takes the new one: a down-dial that
           lost to the peer's up-dial, OR a stale inbound left over from a
           re-dial after a dropped link (the peer already cleared its side but
           my close event has not fired yet - rejecting here would strand the
           dialer half-connected: peer_conns set, peer_socks 0, no re-dial). */
        if(existing->outbound&&peer_id>=server->id) return -1;
        {
          void *dead=existing->sock;
          existing->sock=0;
          server->transport->close(dead);
        }
        server->peer_conns[node_index]=0;
        server->peer_socks[node_index]=0;
      }
    }
    conn->peer_id=peer_id;
    conn->node_index=node_index;
    server->peer_conns[node_index]=conn;
    server->peer_socks[node_index]=conn->sock;
    return 0;
  }
  if(type==K_PEER_RAFT){
    k_decoded_peer decoded;
    int rc;
    if(!conn->peer_id||k_peer_decode(&decoded,payload,size)!=0) return -1;
    if(decoded.message.from!=conn->peer_id||decoded.message.to!=server->id){
      k_decoded_peer_free(&decoded);
      return -1;
    }
    if(decoded.message.type==RAFT_MSG_INSTALL_SNAPSHOT&&k_server_write_inbound_snapshot(server,&decoded.message.install_snapshot)!=0){
      k_decoded_peer_free(&decoded);
      return -1;
    }
    rc=raft_recvfrom_peer(server->raft,&decoded.message);
    k_decoded_peer_free(&decoded);
    return rc;
  }
  return -1;
}
/* ================= Server: apply (state machine) ================= */
static int k_server_apply_exec(k_server *server,const void *command,k_u32 size,const void *cookie){
  treap *fork;
  k_reader reader;
  k_u8 op;
  k_u32 key_len,value_len;
  const k_u8 *key,*value;
  int rc;
  /* FCALL apply = op-replay on BOTH leader and follower, wrapped in an
     apply-time fork + commit so the multi-op write-set stays ATOMIC (all-or-
     nothing, exactly like MSET/MDEL/RSET).  The fork is taken from the live
     APPLIED tree HERE, so it is always fresh -- unlike the deleted leader
     exec-time fork, which was taken when the barrier read resolved and could be
     stale w.r.t. a write committed after that read (e.g. a CAS flushed into the
     log just before the FCALL barrier): swapping THAT fork would clobber the
     concurrent write and diverge the leader from the followers. */
  (void)cookie;
  fork=treap_fork(server->tree);
  if(!fork) return -1;
  memset(&reader,0,sizeof(reader));
  reader.data=(const k_u8 *)command;
  reader.len=size;
  (void)k_reader_u8(&reader);
  while(reader.off<reader.len){
    op=k_reader_u8(&reader);
    key_len=k_reader_u32(&reader);
    if(!key_len||key_len>K_KEY_MAX) goto exec_fail;
    key=k_reader_bytes(&reader,key_len);
    if(op==K_OP_SET){
      value_len=k_reader_u32(&reader);
      if(value_len>K_VALUE_MAX) goto exec_fail;
      value=k_reader_bytes(&reader,value_len);
      if(treap_set(fork,key,key_len,value,value_len)!=0) goto exec_fail;
    }else if(op==K_OP_DEL){
      rc=treap_delete(fork,key,key_len);
      if(rc<0) goto exec_fail;
    }else goto exec_fail;
  }
  if(reader.err||reader.off!=reader.len) goto exec_fail;
  if(treap_commit(fork,server->tree)!=0) goto exec_fail;
  return 0;
exec_fail:
  treap_abort(fork);
  return -1;
}
static int k_rset_collect(const unsigned char *key,unsigned int key_len,const unsigned char *value,unsigned int value_len,void *ud);
static int k_server_apply_command(k_server *server,const void *command,k_u32 size,const void *cookie){
  k_reader reader;
  k_u8 op;
  k_u32 key_len,value_len;
  const k_u8 *key,*value;
  int rc;
  if(!server||!command||!size) return -1;
  memset(&reader,0,sizeof(reader));
  reader.data=(const k_u8 *)command;
  reader.len=size;
  op=k_reader_u8(&reader);
  if(op==K_OP_MSET||op==K_OP_MDEL){
    /* Declarative atomic multi-op: fork + N treap_set/delete + commit (one
       entry, all-or-nothing).  No gate/barrier (unconditional writes, no
       read-modify-write).  Returns the affected-key count. */
    treap *fork;
    k_request *req;
    k_u32 count,affected,i;
    char numbuf[32];
    int n;
    count=k_reader_u32(&reader);
    if(count<1u) return -1;
    fork=treap_fork(server->tree);
    if(!fork) return -1;
    affected=0u;
    for(i=0;i<count;i++){
      key_len=k_reader_u32(&reader);
      if(!key_len||key_len>K_KEY_MAX) goto mbatch_fail;
      key=k_reader_bytes(&reader,key_len);
      if(op==K_OP_MSET){
        value_len=k_reader_u32(&reader);
        if(value_len>K_VALUE_MAX) goto mbatch_fail;
        value=k_reader_bytes(&reader,value_len);
        if(treap_set(fork,key,key_len,value,value_len)!=0) goto mbatch_fail;
        affected++;
      }else{
        rc=treap_delete(fork,key,key_len);
        if(rc<0) goto mbatch_fail;
        if(rc==1) affected++;
      }
    }
    if(reader.err||reader.off!=reader.len) goto mbatch_fail;
    if(treap_commit(fork,server->tree)!=0) goto mbatch_fail;
    req=cookie?k_request_find(server,cookie):0;
    if(req){
      n=sprintf(numbuf,"%u",(unsigned)affected);
      k_buf_free(&req->result);
      k_buf_bytes(&req->result,numbuf,(k_u32)n);
      if(req->result.err) return -1;
    }
    return 0;
mbatch_fail:
    treap_abort(fork);
    return -1;
  }
  if(op==K_OP_RSET){
    /* Range overwrite: [op=RSET][begin_len][begin][end_len][end][value_len][value].
       Collect the range keys, then fork + N treap_set + commit (atomic). */
    k_request *req;
    treap *fork;
    k_buf list;
    k_reader r2;
    const k_u8 *begin,*end,*newval,*k;
    k_u32 begin_len,end_len,newval_len,klen;
    k_u32 affected;
    treap_u64 scanned;
    char numbuf[32];
    int n;
    begin_len=k_reader_u32(&reader);
    if(begin_len>K_KEY_MAX) return -1;
    begin=k_reader_bytes(&reader,begin_len);
    end_len=k_reader_u32(&reader);
    if(end_len>K_KEY_MAX) return -1;
    end=k_reader_bytes(&reader,end_len);
    newval_len=k_reader_u32(&reader);
    if(newval_len>K_VALUE_MAX) return -1;
    newval=k_reader_bytes(&reader,newval_len);
    if(reader.err||reader.off!=reader.len) return -1;
    memset(&list,0,sizeof(list));
    scanned=treap_scan(server->tree,begin,begin_len,end,end_len,TREAP_ASC,k_rset_collect,&list);
    if(scanned==(treap_u64)-1||list.err){ k_buf_free(&list); return -1; }
    fork=treap_fork(server->tree);
    if(!fork){ k_buf_free(&list); return -1; }
    affected=0u;
    memset(&r2,0,sizeof(r2));
    r2.data=list.data;
    r2.len=list.len;
    while(r2.off<r2.len){
      klen=k_reader_u32(&r2);
      k=k_reader_bytes(&r2,klen);
      if(r2.err){ treap_abort(fork); k_buf_free(&list); return -1; }
      if(treap_set(fork,k,klen,newval,newval_len)!=0){ treap_abort(fork); k_buf_free(&list); return -1; }
      affected++;
    }
    k_buf_free(&list);
    if(treap_commit(fork,server->tree)!=0){ treap_abort(fork); return -1; }
    req=cookie?k_request_find(server,cookie):0;
    if(req){
      n=sprintf(numbuf,"%u",(unsigned)affected);
      k_buf_free(&req->result);
      k_buf_bytes(&req->result,numbuf,(k_u32)n);
      if(req->result.err) return -1;
    }
    return 0;
  }
  if(op==K_OP_FCALL) return k_server_apply_exec(server,command,size,cookie);
  if(op==K_OP_RDEL){
    k_request *req;
    k_u32 begin_len,end_len;
    const k_u8 *begin,*end;
    treap_u64 removed;
    char numbuf[32];
    int n;
    begin_len=k_reader_u32(&reader);
    if(begin_len>K_KEY_MAX) return -1;
    begin=k_reader_bytes(&reader,begin_len);
    end_len=k_reader_u32(&reader);
    if(end_len>K_KEY_MAX) return -1;
    end=k_reader_bytes(&reader,end_len);
    if(reader.err||reader.off!=reader.len) return -1;
    removed=treap_delete_range(server->tree,begin,begin_len,end,end_len);
    if(removed==(treap_u64)-1) return -1;
    req=cookie?k_request_find(server,cookie):0;
    if(req){
      n=sprintf(numbuf,"%" K_U64_FMT,(k_u64)removed);
      k_buf_free(&req->result);
      k_buf_bytes(&req->result,numbuf,(k_u32)n);
      if(req->result.err) return -1;
    }
    return 0;
  }
  if(op==K_OP_CAS){
    /* Atomic conditional write, replayed identically on every node.  The value
       field is [mode][new_len][new][old_len][old]: mode K_CAS_SETNX sets only if
       the key does not exist (no old); K_CAS_CMP sets only if the current value
       equals old.  The compare runs at apply time so leader and follower reach
       identical results from an identical applied state. */
    k_request *req;
    k_reader vr;
    const k_u8 *cur=0,*old=0,*newval=0;
    k_u32 cur_len=0,old_len=0,new_len=0;
    k_u8 mode;
    int exists,conflict;
    key_len=k_reader_u32(&reader);
    if(!key_len||key_len>K_KEY_MAX) return -1;
    key=k_reader_bytes(&reader,key_len);
    value_len=k_reader_u32(&reader);
    if(value_len>K_CAS_PACKED_MAX) return -1;
    value=k_reader_bytes(&reader,value_len);
    if(reader.err||reader.off!=reader.len) return -1;
    memset(&vr,0,sizeof(vr));
    vr.data=value;
    vr.len=value_len;
    mode=k_reader_u8(&vr);
    new_len=k_reader_u32(&vr);
    if(new_len>K_VALUE_MAX) return -1;
    newval=k_reader_bytes(&vr,new_len);
    if(mode==K_CAS_CMP){
      old_len=k_reader_u32(&vr);
      if(old_len>K_VALUE_MAX) return -1;
      old=k_reader_bytes(&vr,old_len);
    }else if(mode!=K_CAS_SETNX){
      return -1;
    }
    if(vr.err||vr.off!=vr.len) return -1;
    exists=treap_get(server->tree,key,key_len,&cur,&cur_len);
    if(exists<0) return -1;
    conflict=(mode==K_CAS_SETNX)?(exists==1)
      :(exists==1&&cur_len==old_len&&memcmp(cur,old,old_len)==0)?0:1;
    if(!conflict&&treap_set(server->tree,key,key_len,newval,new_len)!=0) return -1;
    req=cookie?k_request_find(server,cookie):0;
    if(req){
      req->conflict=conflict;
      k_buf_free(&req->result);
      if(exists==1) k_buf_bytes(&req->result,cur,cur_len);
      if(req->result.err) return -1;
    }
    return 0;
  }
  if(op==K_OP_ADDR){
    k_u32 count,host_len;
    int id,j;
    const k_u8 *host;
    unsigned short client_port,peer_port;
    count=k_reader_u32(&reader);
    if(count>K_MAX_NODES) return -1;
    for(j=0;j<(int)count;j++){
      id=(int)k_reader_i32(&reader);
      host_len=k_reader_u32(&reader);
      if(host_len<1u||host_len>=K_HOST_MAX) return -1;
      host=k_reader_bytes(&reader,host_len);
      client_port=k_reader_u16(&reader);
      peer_port=k_reader_u16(&reader);
      if(reader.err) return -1;
      if(k_server_add_address(server,id,(const char *)host,host_len,client_port,peer_port)!=0) return -1;
      /* Protect the freshly-learned address from the A2 GC: the address-book
         entry lands before the joint/C_new entry that makes `id` a voter, and
         on a follower (which does not orchestrate catch-up, so its pending set
         is empty) the GC would otherwise wipe the slot in that window.  Marking
         it pending keeps k_membership_contains true until the CONFIG apply
         graduates it into voters (and drops it from pending).  Nodes already in
         the member set are left alone. */
      if(!k_membership_contains(server,id)&&server->pending_count<K_MAX_NODES) server->pending[server->pending_count++]=id;
    }
    if(reader.err||reader.off!=reader.len) return -1;
    return 0;
  }
  key_len=k_reader_u32(&reader);
  if(!key_len||key_len>K_KEY_MAX) return -1;
  key=k_reader_bytes(&reader,key_len);
  if(op==K_OP_SET){
    value_len=k_reader_u32(&reader);
    if(value_len>K_VALUE_MAX) return -1;
    value=k_reader_bytes(&reader,value_len);
    if(reader.err||reader.off!=reader.len) return -1;
    return treap_set(server->tree,key,key_len,value,value_len);
  }
  if(op==K_OP_DEL){
    if(reader.err||reader.off!=reader.len) return -1;
    rc=treap_delete(server->tree,key,key_len);
    return rc<0?-1:0;
  }
  return -1;
}
/* ================= Server: write queue, gate, submit ================= */
static int k_server_flush_writes(k_server *server);   /* forward: the policy below submits through it */
/* Submit the queued batch and, only if it really was submitted, count WHY it was
   submitted, so sum(flush_by_*) == flush_batches holds (a failed submission frees and
   redirects the requests and must not be attributed to a target/window/drain). */
#define K_FLUSH_NO_REASON 0
#define K_FLUSH_TARGET    1
#define K_FLUSH_BYTES     2
#define K_FLUSH_WINDOW    3
#define K_FLUSH_DRAIN     4
#define K_FLUSH_BARRIER   5
#define K_FLUSH_STOP      6
static int k_server_flush_reason(k_server *server,int reason);
/* Structural group-commit policy.  Deliberately uses no wall-clock: the time window
   keeps living in k_server_advance (the tick-driven backstop), so the server core stays
   a pure function of elapsed_ms and the deterministic harness can still drive it.
   A batch is submitted when
     - it reached the target size (cfg.flush_item_limit), or
     - it reached the byte target (cfg.flush_bytes_limit), or
     - no new write arrived during the last event round (input drained),
   which leaves a lone write's latency unchanged (its own round drains at once) while a
   sustained stream accumulates a real batch instead of being flushed every round.
   Returns 1 = a batch was submitted, 0 = held (or nothing pending), -1 = submit error. */
static int k_server_flush_if_ready(k_server *server,int new_writes_arrived){
  int rc,reason;
  if(!server||!server->write_count) return 0;
  if(server->write_count>=server->cfg.flush_item_limit){ reason=K_FLUSH_TARGET; }
  else if(server->cfg.flush_bytes_limit&&server->write_bytes>=server->cfg.flush_bytes_limit){ reason=K_FLUSH_BYTES; }
  else if(!new_writes_arrived){ reason=K_FLUSH_DRAIN; }
  else return 0;
  rc=k_server_flush_reason(server,reason);
  return rc==0?1:-1;
}
static int k_server_flush_reason(k_server *server,int reason){
  k_u64 before=server->flush_batches;
  int rc=k_server_flush_writes(server);
  if(rc!=0) return rc;
  /* flush_batches only grows for a real Raft submission: a normal refusal (leadership
     lost / transfer in progress -> requests redirected) also returns 0, and must not be
     attributed to a flush reason, so sum(flush_by_*) == flush_batches holds always. */
  if(server->flush_batches==before) return 0;
  switch(reason){
  case K_FLUSH_TARGET:  server->flush_by_target++;  break;
  case K_FLUSH_BYTES:   server->flush_by_bytes++;   break;
  case K_FLUSH_WINDOW:  server->flush_by_window++;  break;
  case K_FLUSH_DRAIN:   server->flush_by_drain++;   break;
  case K_FLUSH_BARRIER: server->flush_by_barrier++; break;
  case K_FLUSH_STOP:    server->flush_by_stop++;    break;
  default: break;
  }
  return 0;
}
/* Advance one wall-clock sample into whole milliseconds, KEEPING the sub-ms
   remainder.  A driver that samples the clock once per event-loop iteration can
   run thousands of iterations per second; mapping every sub-millisecond
   iteration to 1 ms made the logical clock run several times faster than wall
   time (Raft election/heartbeat and the flush window then fired far earlier than
   configured).  Returning 0 for a sub-millisecond step is correct: the caller's
   advance is a pure function of elapsed_ms, so nothing happens until a whole
   millisecond has really passed.  A gap larger than cap_ms (a debugger pause, a
   suspended process) resynchronizes instead of replaying a burst.  Returns the
   whole milliseconds to advance; *last_us is moved forward by exactly that much. */
static unsigned int k_server_elapsed_step(k_u64 now_us,k_u64 *last_us,unsigned int cap_ms){
  k_u64 delta_us,ms;
  if(!last_us) return 0;
  if(!*last_us){
    *last_us=now_us;            /* first sample: establish the origin */
    return 0;
  }
  if(now_us<=*last_us) return 0;
  delta_us=now_us-*last_us;
  ms=delta_us/1000u;
  if(cap_ms&&ms>(k_u64)cap_ms){
    *last_us=now_us;            /* long stall: resync, do not replay the burst */
    return cap_ms;
  }
  if(!ms) return 0;             /* sub-millisecond: carry the remainder */
  *last_us+=(k_u64)ms*1000u;
  return (unsigned int)ms;
}
/* Restart the group-commit batch age.  The driver calls this when the write queue
   goes from empty to non-empty, i.e. when the writes arrived INSIDE the poll that
   just returned, and then passes batch_ms=0 to k_server_advance_at for that same
   round (see there): the poll wait before their arrival does not belong to them,
   so charging it made a fresh batch look older than flush_timeout_ms and flushed
   it at once (one write per batch, no group commit). */
static void k_server_batch_start(k_server *server){
  if(server&&server->write_count) server->write_elapsed=0;
}
/* Driver policy, shared by the production loop and the tests: how much of this
   round's wall delta belongs to the batch that is queued NOW.
   The queue must be treated as newly born when either
     - it was EMPTY before the poll (every queued write arrived inside the poll), or
     - a submission happened during the poll AND writes are queued again: those are a
       new batch that formed after the flush (one poll can both submit a batch - e.g. on
       reaching the item target mid-poll - and leave a partial batch behind), so it must
       not inherit the age the previous batch had accumulated.
   Anything else means the same batch is still being filled and the elapsed time really
   is its age.  Returns the value to pass as batch_ms. */
static unsigned int k_server_batch_elapsed_ms(k_server *server,unsigned int elapsed_ms,k_u32 pending_before,k_u64 flushes_before){
  if(!server) return elapsed_ms;
  if(server->write_count&&(pending_before==0u||server->flush_batches!=flushes_before)){
    k_server_batch_start(server);
    return 0u;
  }
  return elapsed_ms;
}
/* Remaining milliseconds before the oldest queued write must be submitted
   (flush_timeout_ms is the batch age limit), or -1 when nothing is queued.  The
   driver clamps its poll timeout to this so the window is honoured on the real
   clock: with a lone write the loop wakes at the deadline instead of waiting a
   full poll_ms.  Time itself always flows through k_server_advance(elapsed_ms),
   so the server core stays a pure function of elapsed_ms. */
static int k_server_flush_deadline_ms(const k_server *server){
  if(!server||!server->write_count) return -1;
  if(server->write_elapsed>=server->cfg.flush_timeout_ms) return 0;
  return (int)(server->cfg.flush_timeout_ms-server->write_elapsed);
}
static int k_server_flush_writes(k_server *server){
  raft_command *commands;
  raft_client_message message;
  k_request *head,*request,*next;
  k_u32 count,i;
  int rc=0,oom=0;
  if(!server||!server->write_head) return 0;
  head=server->write_head;
  count=server->write_count;
  server->write_head=0;
  server->write_tail=0;
  server->write_count=0;
  server->write_bytes=0;
  server->write_elapsed=0;
  commands=(raft_command *)K_CALLOC(count,sizeof(*commands));
  if(!commands){ rc=-1; oom=1; }
  else{
    request=head;
    for(i=0;i<count&&request;i++){
      commands[i].cookie=request;
      commands[i].command=request->command;
      commands[i].command_size=request->command_size;
      request=request->write_next;
    }
    memset(&message,0,sizeof(message));
    message.type=RAFT_CLIENT_SUBMIT;
    message.submit.commands=commands;
    message.submit.count=(int)count;
    rc=i==count&&!request?raft_recvfrom_client(server->raft,&message):-1;
    if(rc==0){
      /* From here the request is Raft's cookie AND the live payload pointer for its entry: it must
         stay alive until the terminal result arrives.  Record that so freeing it early is caught. */
      request=head;
      for(i=0;i<count&&request;i++){ request->submitted=1; request->submit_ms=server->elapsed_total_ms; request=request->write_next; }
    }
    K_FREE(commands);
    if(rc==0){
      /* Group-commit accounting: one submission (one WAL record, one fsync) for
         `count` client writes.  Mean batch size = flush_writes_total/flush_batches;
         it is the number that says whether the flush window is actually batching. */
      server->flush_batches++;
     server->write_elapsed=0;   /* the next batch's age starts at its own arrival */
      server->flush_writes_total+=(k_u64)count;
    }
  }
  for(request=head;request;request=next){
    next=request->write_next;
    request->write_next=0;
    if(rc==0){
      /* Do NOT free request->command here.  A successful submit hands `request->command` to Raft as
         the LIVE payload pointer for that entry, and on the single-node / no-other-voter path Raft
         applies it later (bundle->live_deferred).  Freeing it at submit time left the apply reading
         freed memory (page-heap read fault inside treap_blob_create / treap_merge with allocator
         filler in the value bytes).  Ownership is the request's: k_request_free releases it once the
         terminal result has arrived. */
    }else{
      if(request->conn) k_server_redirect(server,request->conn,request->id);
      k_request_free(server,request);
    }
  }
  /* A rejected SUBMIT (this node is no longer leader, or is mid transfer) is a
     NORMAL redirect: every request was already redirected above and the client
     retries at the real leader, so it must NOT be treated as a fatal error --
     the write queue drained cleanly, nothing was lost.  Only a hard allocation
     failure (OOM) is surfaced, because a server that cannot even encode its own
     write queue (K_CALLOC returned NULL) is genuinely unable to make progress. */
  return oom?-1:0;
}
static void k_server_begin_stop(k_server *server){
  if(!server||server->stopping) return;
  server->admission=0;
  if(k_server_flush_reason(server,server->write_count?K_FLUSH_STOP:K_FLUSH_NO_REASON)!=0){
    printf("fatal: flush writes failed (stop)\n");
    server->fatal=1;
  }
  server->stopping=1;
  if(server->raft) raft_stop(server->raft);
}
static int k_request_is_read(int type){
  return type==K_REQ_GET||type==K_REQ_COUNT||type==K_REQ_MIN||type==K_REQ_MAX||type==K_REQ_RGET||type==K_REQ_MEMBERS||type==K_REQ_MGET||type==K_REQ_TOPOLOGY;
}
/* Serialize the address book (the Sec 6.1 inclusive directory) as
   "id@host:client_port:peer_port, ..." for client discovery. */
static int k_server_members_build(k_server *server,k_buf *body){
  int i,len;
  char buf[K_HOST_MAX+48];
  const k_node_spec *node;
  if(!server||!body) return -1;
  for(i=0;i<server->cluster.count;i++){
    node=&server->cluster.nodes[i];
    if(node->id<=0) continue;
    if(body->len) k_buf_u8(body,(k_u8)',');
    len=sprintf(buf,"%d@%s:%u:%u",node->id,node->host,(unsigned)node->client_port,(unsigned)node->peer_port);
    k_buf_bytes(body,(const k_u8*)buf,(k_u32)len);
  }
  return body->err?-1:0;
}
/* Serialize the current Raft membership (voters / learners / pending catch-up
   targets) with roles and addresses, for operator visibility.  Unlike
   k_server_members_build (the Sec 6.1 address book for client discovery), this
   reports WHO is currently voting / catching up, not the static white list. */
static int k_server_topology_build(k_server *server,k_buf *body){
  int i,index,len;
  char buf[K_HOST_MAX+64];
  const k_node_spec *node;
  if(!server||!body) return -1;
  for(i=0;i<server->voter_count;i++){
    index=k_cluster_index(&server->cluster,server->voters[i]);
    if(index<0) continue;
    node=&server->cluster.nodes[index];
    if(body->len) k_buf_u8(body,(k_u8)',');
    len=sprintf(buf,"%d@%s:%u:%u role=voter",node->id,node->host,(unsigned)node->client_port,(unsigned)node->peer_port);
    k_buf_bytes(body,(const k_u8*)buf,(k_u32)len);
  }
  for(i=0;i<server->learner_count;i++){
    index=k_cluster_index(&server->cluster,server->learners[i]);
    if(index<0) continue;
    node=&server->cluster.nodes[index];
    if(body->len) k_buf_u8(body,(k_u8)',');
    len=sprintf(buf,"%d@%s:%u:%u role=learner",node->id,node->host,(unsigned)node->client_port,(unsigned)node->peer_port);
    k_buf_bytes(body,(const k_u8*)buf,(k_u32)len);
  }
  for(i=0;i<server->pending_count;i++){
    index=k_cluster_index(&server->cluster,server->pending[i]);
    if(index<0) continue;
    node=&server->cluster.nodes[index];
    if(body->len) k_buf_u8(body,(k_u8)',');
    len=sprintf(buf,"%d@%s:%u:%u role=pending",node->id,node->host,(unsigned)node->client_port,(unsigned)node->peer_port);
    k_buf_bytes(body,(const k_u8*)buf,(k_u32)len);
  }
  return body->err?-1:0;
}
/* Encode an unconditional (SET/DEL) or conditional (RDEL/CAS) write into its
   Raft command blob.  CAS's value already carries [mode][new_len][new][old_len][old],
   so it is appended verbatim after the key; apply re-parses it into mode/new/old. */
static int k_server_build_write_command(k_buf *cmd,int request_type,const k_u8 *key,k_u32 key_len,const k_u8 *value,k_u32 value_len){
  if(!cmd) return -1;
  if(request_type==K_REQ_MSET||request_type==K_REQ_MDEL){
    k_buf_u8(cmd,request_type==K_REQ_MSET?K_OP_MSET:K_OP_MDEL);
    k_buf_bytes(cmd,key,key_len);
  }else if(request_type==K_REQ_RSET){
    /* key carries packed [begin_len][begin][end_len][end]; value is the new value */
    k_buf_u8(cmd,K_OP_RSET);
    k_buf_bytes(cmd,key,key_len);
    k_buf_u32(cmd,value_len);
    k_buf_bytes(cmd,value,value_len);
  }else if(request_type==K_REQ_RDEL){
    k_buf_u8(cmd,K_OP_RDEL);
    k_buf_u32(cmd,key_len);
    k_buf_bytes(cmd,key,key_len);
    k_buf_u32(cmd,value_len);
    k_buf_bytes(cmd,value,value_len);
  }else if(request_type==K_REQ_CAS){
    k_buf_u8(cmd,K_OP_CAS);
    k_buf_u32(cmd,key_len);
    k_buf_bytes(cmd,key,key_len);
    k_buf_u32(cmd,value_len);
    k_buf_bytes(cmd,value,value_len);
  }else{
    k_buf_u8(cmd,request_type==K_REQ_SET?K_OP_SET:K_OP_DEL);
    k_buf_u32(cmd,key_len);
    k_buf_bytes(cmd,key,key_len);
    if(request_type==K_REQ_SET){
      k_buf_u32(cmd,value_len);
      k_buf_bytes(cmd,value,value_len);
    }
  }
  return cmd->err||cmd->len>K_RAFT_COMMAND_MAX?-1:0;
}
/* Queue a write request while the FCALL gate is closed.  The raw key/value are
   kept in the request so the gate can later re-dispatch it: either encode it into
   the group-commit queue (unconditional/conditional write) or open a fresh FCALL
   window. */
static int k_server_queue_gated(k_server *server,k_conn *conn,k_u32 request_id,int request_type,const k_u8 *key,k_u32 key_len,const k_u8 *value,k_u32 value_len){
  k_request *request;
  request=k_request_create_server(server,conn,request_id,request_type,key,key_len,value,value_len,(k_u64)key_len+(k_u64)value_len);
  if(!request) return k_server_send_response(server,conn,request_id,K_STATUS_ERROR,0,"out of memory",13u);
  request->gate_next=0;
  if(server->gate_tail) server->gate_tail->gate_next=request;
  else server->gate_head=request;
  server->gate_tail=request;
  return 0;
}
/* Open an FCALL's gate window: close the gate, flush any already-buffered writes
   (they precede this FCALL), then linearize the read with a barrier.  The command
   is forked + executed when the barrier resolves; the gate reopens the moment the
   write-set lands in the log (see k_server_exec_cmd). */
static int k_server_start_fcall(k_server *server,k_request *request){
  raft_client_message message;
  server->gate_closed=1;
  if(k_server_flush_reason(server,server->write_count?K_FLUSH_BARRIER:K_FLUSH_NO_REASON)!=0) return -1;
  memset(&message,0,sizeof(message));
  message.type=RAFT_CLIENT_BARRIER;
  message.cookie=request;
  return raft_recvfrom_client(server->raft,&message);
}
/* Reopen the gate once an FCALL's write-set has entered the log: drain the queued
   writes in order, re-encoding unconditional writes back into the group-commit queue
   and starting the next FCALL (if any) as a fresh gate window. */
static void k_server_open_gate(k_server *server){
  k_request *request;
  k_buf command_buf;
  int reason;
  server->gate_closed=0;
  while((request=server->gate_head)!=0){
    server->gate_head=request->gate_next;
    request->gate_next=0;
    if(server->gate_head==0) server->gate_tail=0;
    if(request->type==K_REQ_FCALL){
      if(k_server_start_fcall(server,request)!=0){
        if(request->conn) k_server_redirect(server,request->conn,request->id);
        k_request_free(server,request);
      }
      return;
    }
    memset(&command_buf,0,sizeof(command_buf));
    if(k_server_build_write_command(&command_buf,request->type,request->key,request->key_len,request->end,request->end_len)!=0){
      if(request->conn) k_server_redirect(server,request->conn,request->id);
      k_request_free(server,request);
      continue;
    }
    request->command=command_buf.data;
    request->command_size=command_buf.len;
    request->write_next=0;
    if(server->write_tail) server->write_tail->write_next=request;
    else server->write_head=request;
    server->write_tail=request;
    server->write_count++;
    server->writes_arrived++;
    server->write_bytes+=(k_u64)request->command_size;
  }
  if(server->write_count>=server->cfg.flush_item_limit||(server->cfg.flush_bytes_limit&&server->write_bytes>=server->cfg.flush_bytes_limit)){
    reason=(server->write_count>=server->cfg.flush_item_limit)?K_FLUSH_TARGET:K_FLUSH_BYTES;
    k_server_flush_reason(server,reason);
  }
}
/* Dropped leadership: abandon any open FCALL gate and redirect every queued write so
   clients retry against the new leader.  The in-flight FCALL itself is handled by the
   normal REDIRECT result path (fcall_fail -> open_gate, which is now a no-op because
   the queue is already drained here).  Without this, a leader that stepped down mid-
   gate would leave gate_closed=1 (no FCALL left to reopen it -> writes deadlock) and
   strand the tail of gate_head (open_gate returns on the first FCALL it meets). */
static void k_server_clear_gate(k_server *server){
  k_request *request,*next;
  if(!server) return;
  server->gate_closed=0;
  for(request=server->gate_head;request;request=next){
    next=request->gate_next;
    request->gate_next=0;
    if(request->conn) k_server_redirect(server,request->conn,request->id);
    k_request_free(server,request);
  }
  server->gate_head=0;
  server->gate_tail=0;
}
static int k_server_submit(k_server *server,k_conn *conn,k_u32 request_id,int request_type,const k_u8 *key,k_u32 key_len,const k_u8 *value,k_u32 value_len){
  k_request *request;
  k_buf command_buf;
  raft_client_message message;
  int rc,reason;
  if(!server->is_leader&&!k_request_is_read(request_type)) return k_server_redirect(server,conn,request_id);
  if(server->gate_closed&&!k_request_is_read(request_type)){
    return k_server_queue_gated(server,conn,request_id,request_type,key,key_len,value,value_len);
  }
  if(request_type==K_REQ_FCALL){
    request=k_request_create_server(server,conn,request_id,request_type,key,key_len,value,value_len,(k_u64)key_len+(k_u64)value_len);
    if(!request) return k_server_send_response(server,conn,request_id,K_STATUS_ERROR,0,"out of memory",13u);
    if(k_server_start_fcall(server,request)!=0){
      k_request_free(server,request);
      return k_server_redirect(server,conn,request_id);
    }
    return 0;
  }
  request=k_request_create_server(server,conn,request_id,request_type,k_request_is_read(request_type)?key:0,k_request_is_read(request_type)?key_len:0u,(request_type==K_REQ_RGET||request_type==K_REQ_COUNT||request_type==K_REQ_MIN||request_type==K_REQ_MAX)?value:0,(request_type==K_REQ_RGET||request_type==K_REQ_COUNT||request_type==K_REQ_MIN||request_type==K_REQ_MAX)?value_len:0u,(k_u64)key_len+(k_u64)value_len);
  if(!request) return k_server_send_response(server,conn,request_id,K_STATUS_ERROR,0,"out of memory",13u);
  memset(&message,0,sizeof(message));
  if(k_request_is_read(request_type)){
    if(k_server_flush_reason(server,server->write_count?K_FLUSH_BARRIER:K_FLUSH_NO_REASON)!=0){
      /* A client read must never be dropped silently: answer with an error.  This used to
         free the request and return -1, so the client waited forever with no reply and no
         diagnostic anywhere. */
      k_request_free(server,request);
      return k_server_send_response(server,conn,request_id,K_STATUS_ERROR,0,"flush failed",12u);
    }
    message.type=RAFT_CLIENT_BARRIER;
    message.cookie=request;
    rc=raft_recvfrom_client(server->raft,&message);
    if(rc!=0){
      /* Same contract: the barrier was not registered, so no result will ever arrive for this
         request - the previous code ignored rc and left the client pending forever.
         A rejected barrier means raft does not consider this node able to serve the read
         (Sec. 4.2.4/6.4: it has stepped down, e.g. after a higher-term vote request, and may
         not know the leader yet).  Answer with REDIRECT, never with a hard error: the client
         follows the leader hint or retries, exactly as Sec. 6.2 requires for a non-leader.
         Reconcile our own view with that mechanism signal too, so later requests take the
         redirect path directly instead of being submitted to raft and rejected. */
      server->is_leader=0;
      k_request_free(server,request);
      return k_server_redirect(server,conn,request_id);
    }
  }else{
    memset(&command_buf,0,sizeof(command_buf));
    if(k_server_build_write_command(&command_buf,request_type,key,key_len,value,value_len)!=0){
      k_buf_free(&command_buf);
      k_request_free(server,request);
      return k_server_send_response(server,conn,request_id,K_STATUS_ERROR,0,"out of memory",13u);
    }
    request->command=command_buf.data;
    request->command_size=command_buf.len;
    request->write_next=0;
    if(server->write_tail) server->write_tail->write_next=request;
    else server->write_head=request;
    server->write_tail=request;
    server->write_count++;
    server->writes_arrived++;
    server->write_bytes+=(k_u64)request->command_size;
    if(server->write_count>=server->cfg.flush_item_limit||(server->cfg.flush_bytes_limit&&server->write_bytes>=server->cfg.flush_bytes_limit)){
      /* k_server_flush_writes always drains the entire batch: on success
         the request is kept as a Raft cookie; on failure all requests are
         freed and redirects already sent.  In either case we are done. */
      reason=(server->write_count>=server->cfg.flush_item_limit)?K_FLUSH_TARGET:K_FLUSH_BYTES;
      k_server_flush_reason(server,reason);
      return 0;
    }
    rc=0;
  }
  if(rc!=0){
    k_request_free(server,request);
    return k_server_redirect(server,conn,request_id);
  }
  return 0;
}
/* Build the K_OP_ADDR full-member-map command: [op][count][id i32][host_len u32]
   [host][client_port u16][peer_port u16]...  The map lists every id in `ids`
   that has a known address, so a replicated ADDR entry makes every node's
   address book converge (Sec 6.1: the directory is "inclusive" and is updated
   around membership changes). */
/* ================= Server: membership & address book ================= */
static int k_server_build_addr_command(k_buf *cmd,const k_server *server,const int *ids,int id_count){
  k_u32 count=0;
  int i,ni;
  if(!cmd||!server||id_count<0) return -1;
  for(i=0;i<id_count;i++) if(k_cluster_index(&server->cluster,ids[i])>=0) count++;
  k_buf_u8(cmd,K_OP_ADDR);
  k_buf_u32(cmd,count);
  for(i=0;i<id_count;i++){
    const k_node_spec *node;
    k_u32 host_len;
    ni=k_cluster_index(&server->cluster,ids[i]);
    if(ni<0) continue;
    node=&server->cluster.nodes[ni];
    host_len=(k_u32)strlen(node->host);
    k_buf_i32(cmd,(k_i32)node->id);
    k_buf_u32(cmd,host_len);
    k_buf_bytes(cmd,node->host,host_len);
    k_buf_u16(cmd,node->client_port);
    k_buf_u16(cmd,node->peer_port);
  }
  return cmd->err?-1:0;
}
/* Append a replicated ADDR entry to the leader's log.  It carries a full
   member-address map, needs no client reply, so a fixed non-NULL cookie is
   used and the COMMITTED result is dropped by the result handler.  It bypasses
   the FCALL gate on purpose: it mutates the address book (routing state), not
   the treap, so it never conflicts with a swap-root write-set. */
static int k_server_submit_addr(k_server *server,const int *ids,int id_count){
  static int addr_cookie_stub;
  raft_client_message message;
  raft_command command;
  k_buf cmd;
  memset(&cmd,0,sizeof(cmd));
  if(k_server_build_addr_command(&cmd,server,ids,id_count)!=0) return -1;
  memset(&command,0,sizeof(command));
  command.cookie=&addr_cookie_stub;
  command.command=cmd.data;
  command.command_size=cmd.len;
  memset(&message,0,sizeof(message));
  message.type=RAFT_CLIENT_SUBMIT;
  message.submit.commands=&command;
  message.submit.count=1;
  if(raft_recvfrom_client(server->raft,&message)!=0){
    k_buf_free(&cmd);
    return -1;
  }
  k_buf_free(&cmd);
  return 0;
}
static int k_server_submit_member(k_server *server,k_conn *conn,k_u32 request_id,int subcmd,const int *ids,int id_count){
  k_request *request;
  raft_client_message message;
  int final_ids[K_MAX_NODES];
  int map_ids[K_MAX_NODES];
  int final_count=0,map_count=0,i;
  if(!server||id_count<0||id_count>K_MAX_NODES) return k_server_send_response(server,conn,request_id,K_STATUS_ERROR,0,"bad member request",17u);
  if(!server->is_leader) return k_server_redirect(server,conn,request_id);
  /* every target id must be in the address book (the pre-listed white list) */
  for(i=0;i<id_count;i++) if(k_cluster_index(&server->cluster,ids[i])<0) return k_server_send_response(server,conn,request_id,K_STATUS_ERROR,0,"unknown node id",14u);
  if(subcmd==K_MEMBER_RECONFIG){
    for(i=0;i<id_count;i++) final_ids[i]=ids[i];
    final_count=id_count;
  }else if(subcmd==K_MEMBER_ADD){
    for(i=0;i<server->voter_count;i++) final_ids[i]=server->voters[i];
    final_count=server->voter_count;
    for(i=0;i<id_count;i++){
      if(!k_membership_has(final_ids,final_count,ids[i])){
        if(final_count>=K_MAX_NODES) return k_server_send_response(server,conn,request_id,K_STATUS_ERROR,0,"voter set full",14u);
        final_ids[final_count++]=ids[i];
      }
    }
  }else{ /* K_MEMBER_REMOVE */
    for(i=0;i<server->voter_count;i++){
      if(!k_membership_has(ids,id_count,server->voters[i])) final_ids[final_count++]=server->voters[i];
    }
  }
  if(final_count<=0) return k_server_send_response(server,conn,request_id,K_STATUS_ERROR,0,"empty voter set",15u);
  /* The replicated address map must include every node that should stay
     reachable: the target voter set, learners, and in-flight catch-up targets
     (a pending target is not yet in the voter snapshot, but its address must
     not be cleared by a concurrent ADDR apply). */
  for(i=0;i<final_count;i++) if(!k_membership_has(map_ids,map_count,final_ids[i])) map_ids[map_count++]=final_ids[i];
  for(i=0;i<server->learner_count;i++) if(!k_membership_has(map_ids,map_count,server->learners[i])) map_ids[map_count++]=server->learners[i];
  for(i=0;i<server->pending_count;i++) if(!k_membership_has(map_ids,map_count,server->pending[i])) map_ids[map_count++]=server->pending[i];
  /* Sec 6.1: publish the (inclusive) directory BEFORE the membership change,
     so every node already knows a new member's address when it joins. */
  if(subcmd!=K_MEMBER_REMOVE&&k_server_submit_addr(server,map_ids,map_count)!=0)
    return k_server_send_response(server,conn,request_id,K_STATUS_ERROR,0,"address log failed",18u);
  request=k_request_create_server(server,conn,request_id,K_REQ_MEMBER,0,0,0,0,(k_u64)id_count*sizeof(int));
  if(request&&!conn) request->internal=1;   /* server-created: no client to answer */
  if(!request) return k_server_send_response(server,conn,request_id,K_STATUS_ERROR,0,"out of memory",13u);
  memset(&message,0,sizeof(message));
  message.type=RAFT_CLIENT_RECONFIG;
  message.cookie=request;
  message.reconfig.ids=final_ids;
  message.reconfig.id_count=final_count;
  if(raft_recvfrom_client(server->raft,&message)!=0){
    k_request_free(server,request);
    return k_server_send_response(server,conn,request_id,K_STATUS_ERROR,0,"reconfig rejected",17u);
  }
  /* accepted: any voter not yet in the snapshot is catch-up material.  Mark it
     a pending catch-up target so the peer topology dials it during catch-up (its
     connection would otherwise be dropped as "not a member").  It graduates to
     voter on the joint/C_new apply via k_membership_update (which also drops it
     from pending); a failed catch-up clears pending via the result path. */
  for(i=0;i<final_count;i++){
    if(!k_membership_has(server->voters,server->voter_count,final_ids[i])&&!k_membership_has(server->learners,server->learner_count,final_ids[i])&&!k_membership_has(server->pending,server->pending_count,final_ids[i])){
      if(server->pending_count<K_MAX_NODES) server->pending[server->pending_count++]=final_ids[i];
    }
  }
  return 0;
}
/* Sec 4.4 auto-replacement: when a voter is unreachable for auto_replace_threshold
   heartbeat rounds, replace it add-before-remove with the configured node.
   Leader-only, driven by ready.peer_health. */
static void k_server_auto_replace(k_server *server,const raft_ready *ready){
  int i,pid,failed_id=0;
  int new_ids[1];
  int failed_ids[1];
  if(!server||!ready||!server->is_leader||!server->auto_replace||server->auto_replace_phase!=0) return;
  for(i=0;i<ready->peer_health_count;i++){
    if(ready->peer_health[i].missed_rounds>=server->auto_replace_threshold){
      pid=ready->peer_health[i].id;
      if(k_membership_has(server->voters,server->voter_count,pid)){ failed_id=pid; break; }
    }
  }
  if(failed_id>0){
    server->auto_replace_failed_id=failed_id;
    if(k_membership_has(server->voters,server->voter_count,server->auto_replace_new_id)){
      /* replacement already a voter (a prior REMOVE failed): retry the REMOVE */
      failed_ids[0]=failed_id;
      server->auto_replace_phase=2;
      if(k_server_submit_member(server,0,0,K_MEMBER_REMOVE,failed_ids,1)!=0) server->auto_replace_phase=0;
    }else{
      k_server_add_address(server,server->auto_replace_new_id,server->auto_replace_new_host,(k_u32)strlen(server->auto_replace_new_host),server->auto_replace_new_client_port,server->auto_replace_new_peer_port);
      new_ids[0]=server->auto_replace_new_id;
      server->auto_replace_phase=1;
      if(k_server_submit_member(server,0,0,K_MEMBER_ADD,new_ids,1)!=0) server->auto_replace_phase=0;
    }
  }
}
static void k_server_auto_replace_committed(k_server *server){
  int failed_ids[1];
  if(!server||!server->auto_replace) return;
  if(server->auto_replace_phase==1){
    failed_ids[0]=server->auto_replace_failed_id;
    server->auto_replace_phase=2;
    if(k_server_submit_member(server,0,0,K_MEMBER_REMOVE,failed_ids,1)!=0) server->auto_replace_phase=0;
  }else if(server->auto_replace_phase==2){
    server->auto_replace_phase=0;
  }
}
static int k_server_submit_rget(k_server *server,k_conn *conn,k_u32 request_id,k_u8 *command,k_u32 command_size){
  k_request *request;
  raft_client_message message;
  int rc;
  /* RGET is a read: served via ReadIndex on any node (no leader redirect). */
  request=k_request_create_server(server,conn,request_id,K_REQ_RGET,0,0,0,0,(k_u64)command_size);
  if(!request){
    K_FREE(command);
    return k_server_send_response(server,conn,request_id,K_STATUS_ERROR,0,"out of memory",13u);
  }
  request->command=command;
  request->command_size=command_size;
  if(k_server_flush_reason(server,server->write_count?K_FLUSH_BARRIER:K_FLUSH_NO_REASON)!=0){
    /* A range read must never be dropped silently: answer with an error so the client sees it
       (mirrors the point-read path). */
    k_request_free(server,request);
    return k_server_send_response(server,conn,request_id,K_STATUS_ERROR,0,"flush failed",12u);
  }
  memset(&message,0,sizeof(message));
  message.type=RAFT_CLIENT_BARRIER;
  message.cookie=request;
  rc=raft_recvfrom_client(server->raft,&message);
  if(rc!=0){
    k_request_free(server,request);
    return k_server_redirect(server,conn,request_id);
  }
  return 0;
}
/* ================= Server: client frame dispatcher ================= */
static int k_server_client_frame(void *ud,k_u8 type,const k_u8 *payload,k_u32 size){
  k_conn *conn=(k_conn *)ud;
  k_server *server;
  k_reader reader;
  k_u32 request_id,key_len,value_len;
  const k_u8 *key=0,*value=0;
  if(!conn||!conn->server) return -1;
  server=conn->server;
  memset(&reader,0,sizeof(reader));
  reader.data=payload;
  reader.len=size;
  request_id=k_reader_u32(&reader);
  if(reader.err) return -1;
  if(!server->admission&&type!=K_REQ_INFO&&type!=K_REQ_STATS&&type!=K_REQ_HELP&&type!=K_REQ_TOPOLOGY) return k_server_send_response(server,conn,request_id,K_STATUS_ERROR,0,"server stopping",15u);
  if(type==K_REQ_SET||type==K_REQ_GET||type==K_REQ_DEL||type==K_REQ_CAS){
    key_len=k_reader_u32(&reader);
    if(!key_len||key_len>K_KEY_MAX) return -1;
    key=k_reader_bytes(&reader,key_len);
    value_len=0;
    if(type==K_REQ_SET||type==K_REQ_CAS){
      value_len=k_reader_u32(&reader);
      if(value_len>(type==K_REQ_CAS?K_CAS_PACKED_MAX:K_VALUE_MAX)) return -1;
      value=k_reader_bytes(&reader,value_len);
    }
    if(reader.err||reader.off!=reader.len) return -1;
    return k_server_submit(server,conn,request_id,(int)type,key,key_len,value,value_len);
  }
  if(type==K_REQ_RSET){
    /* key carries packed [begin_len][begin][end_len][end]; value is the new value */
    key_len=k_reader_u32(&reader);
    if(!key_len||key_len>2u*K_KEY_MAX+8u) return -1;
    key=k_reader_bytes(&reader,key_len);
    value_len=k_reader_u32(&reader);
    if(value_len>K_VALUE_MAX) return -1;
    value=k_reader_bytes(&reader,value_len);
    if(reader.err||reader.off!=reader.len) return -1;
    return k_server_submit(server,conn,request_id,(int)type,key,key_len,value,value_len);
  }
  if(type==K_REQ_RDEL){
    key_len=k_reader_u32(&reader);
    if(key_len>K_KEY_MAX) return -1;
    key=k_reader_bytes(&reader,key_len);
    value_len=k_reader_u32(&reader);
    if(value_len>K_KEY_MAX) return -1;
    value=k_reader_bytes(&reader,value_len);
    if(reader.err||reader.off!=reader.len) return -1;
    return k_server_submit(server,conn,request_id,(int)type,key,key_len,value,value_len);
  }
  if(type==K_REQ_MSET||type==K_REQ_MDEL||type==K_REQ_MGET){
    /* batch: [count][k1_len][k1][v1_len][v1]... (MSET) or [count][k1_len][k1]... (MDEL/MGET).
       Walk the whole list here, with the same bounds apply enforces (key 1..K_KEY_MAX,
       value <= K_VALUE_MAX, no trailing bytes), and reject a malformed list before it
       becomes a raft entry.  Checking only "there are 4 bytes" let a bogus count (e.g.
       0xffffffff) through: the write replicated and answered 200, then apply rejected it
       and the node flagged itself fatal (fail-stop), i.e. an unauthenticated client could
       take a node down. */
    const k_u8 *list;
    k_u32 list_len,count,i,item_len;
    k_reader lr;
    list=reader.data+reader.off;
    list_len=reader.len-reader.off;
    if(list_len<4u) return -1;
    memset(&lr,0,sizeof(lr));
    lr.data=list;
    lr.len=list_len;
    count=k_reader_u32(&lr);
    /* every item carries at least its key length + one key byte */
    if(count<1u||count>list_len) return -1;
    for(i=0;i<count;i++){
      item_len=k_reader_u32(&lr);
      if(lr.err||!item_len||item_len>K_KEY_MAX) return -1;
      if(!k_reader_bytes(&lr,item_len)) return -1;
      if(type==K_REQ_MSET){
        item_len=k_reader_u32(&lr);
        if(lr.err||item_len>K_VALUE_MAX) return -1;
        if(!k_reader_bytes(&lr,item_len)) return -1;
      }
    }
    if(lr.err||lr.off!=lr.len) return -1;
    return k_server_submit(server,conn,request_id,(int)type,list,list_len,0,0);
  }
  if(type==K_REQ_FCALL){
    key_len=k_reader_u32(&reader);
    if(!key_len||key_len>64) return -1;
    key=k_reader_bytes(&reader,key_len);
    value_len=k_reader_u32(&reader);
    if(value_len>K_FRAME_MAX) return -1;
    value=k_reader_bytes(&reader,value_len);
    if(reader.err||reader.off!=reader.len) return -1;
    return k_server_submit(server,conn,request_id,(int)type,key,key_len,value,value_len);
  }
  if(type==K_REQ_RGET){
    k_buf cmd;
    int direction;
    k_u32 limit;
    key_len=k_reader_u32(&reader);
    if(key_len>K_KEY_MAX) return -1;
    key=k_reader_bytes(&reader,key_len);
    value_len=k_reader_u32(&reader);
    if(value_len>K_KEY_MAX) return -1;
    value=k_reader_bytes(&reader,value_len);
    direction=(int)k_reader_u8(&reader);
    limit=0;
    if(reader.off<reader.len) limit=k_reader_u32(&reader);
    if(reader.err||reader.off!=reader.len||(direction!=K_SCAN_ASC&&direction!=K_SCAN_DESC)) return -1;
    /* RGET args serialized into the command: [begin_len][begin][end_len][end][direction][limit] */
    memset(&cmd,0,sizeof(cmd));
    k_buf_u32(&cmd,key_len);
    k_buf_bytes(&cmd,key,key_len);
    k_buf_u32(&cmd,value_len);
    k_buf_bytes(&cmd,value,value_len);
    k_buf_u8(&cmd,(k_u8)direction);
    k_buf_u32(&cmd,limit);
    if(cmd.err){ k_buf_free(&cmd); return -1; }
    return k_server_submit_rget(server,conn,request_id,cmd.data,cmd.len);
  }
  if(type==K_REQ_MEMBER){
    k_u8 subcmd;
    k_u32 id_count;
    int ids[K_MAX_NODES];
    int j;
    subcmd=k_reader_u8(&reader);
    id_count=k_reader_u32(&reader);
    if(id_count<1||id_count>K_MAX_NODES) return -1;
    if(subcmd==K_MEMBER_ADD){
      /* ADD carries each node's address: [id][host_len][host][client_port][peer_port] */
      for(j=0;j<(int)id_count;j++){
        k_u32 host_len;
        const k_u8 *host;
        unsigned short client_port,peer_port;
        ids[j]=(int)k_reader_u32(&reader);
        host_len=k_reader_u32(&reader);
        if(host_len<1||host_len>=K_HOST_MAX) return -1;
        host=k_reader_bytes(&reader,host_len);
        if(!host) return -1;
        client_port=k_reader_u16(&reader);
        peer_port=k_reader_u16(&reader);
        if(k_server_add_address(server,ids[j],(const char *)host,host_len,client_port,peer_port)!=0) return -1;
      }
    }else{
      for(j=0;j<(int)id_count;j++) ids[j]=(int)k_reader_u32(&reader);
    }
    if(reader.err||reader.off!=reader.len) return -1;
    if(subcmd!=K_MEMBER_ADD&&subcmd!=K_MEMBER_REMOVE&&subcmd!=K_MEMBER_RECONFIG) return -1;
    return k_server_submit_member(server,conn,request_id,(int)subcmd,ids,(int)id_count);
  }
  if(type==K_REQ_COUNT||type==K_REQ_MIN||type==K_REQ_MAX){
    key_len=k_reader_u32(&reader);
    if(key_len>K_KEY_MAX) return -1;
    key=k_reader_bytes(&reader,key_len);
    value_len=k_reader_u32(&reader);
    if(value_len>K_KEY_MAX) return -1;
    value=k_reader_bytes(&reader,value_len);
    if(reader.err||reader.off!=reader.len) return -1;
    return k_server_submit(server,conn,request_id,(int)type,key,key_len,value,value_len);
  }
  if(type==K_REQ_MEMBERS||type==K_REQ_TOPOLOGY){
    if(reader.off!=reader.len) return -1;
    return k_server_submit(server,conn,request_id,(int)type,0,0,0,0);
  }
  if(type==K_REQ_HELP){
    static const char help[]="HELP\nGET <key>\nMGET <key> ...\nRGET [begin] [end] [asc|desc] [limit]\nSET <key> <value>\nMSET <key> <value> ...\nRSET <begin> <end> <value>\nDEL <key>\nMDEL <key> ...\nRDEL <begin> <end>\nCOUNT [begin [end]]\nMIN [begin [end]]\nMAX [begin [end]]\nCAS <key> <new> [old]\nFCALL <name> [arg...]\nINFO\nSTATS\nMEMBER ADD|REMOVE|RECONFIG <id,...>\nMEMBERS\nTOPOLOGY\nSHUTDOWN\nEXIT\nQUIT";
    if(reader.off!=reader.len) return -1;
    return k_server_send_response(server,conn,request_id,K_STATUS_OK,server->id,help,(k_u32)(sizeof(help)-1u));
  }
  if(type==K_REQ_INFO||type==K_REQ_STATS){
    raft_info info;
    treap_info tree_info;
    /* 4096, not 2048: the counter line grew to ~1330 bytes once the stall diagnostics were added, and
       every append below is bounded (k_text_append), so no future field can overflow this.  A line that
       does not fit is marked and counted instead of being silently shortened. */
    char text[4096];
    int len,over;
    memset(&info,0,sizeof(info));
    if(reader.off!=reader.len||raft_inspect(server->raft,&info)!=0||treap_inspect(server->tree,&tree_info)!=0) return -1;
    len=0;
    over=0;
    k_text_append(text,sizeof(text),&len,&over,"id=%d state=%d leader=%d term=%" K_I64_FMT " commit=%" K_I64_FMT " applied=%" K_I64_FMT " snapshot=%" K_I64_FMT " log=%" K_I64_FMT " count=%" K_U64_FMT " height=%u bytes=%" K_U64_FMT " pending_frees=%" K_U64_FMT " pending_bytes=%" K_U64_FMT " persist_generation=%" K_U64_FMT " wal_segment=%" K_U64_FMT " wal_offset=%" K_U64_FMT " wal_size=%" K_U64_FMT " wal_next_segment=%" K_U64_FMT " wal_next_offset=%" K_U64_FMT " wal_pending=%" K_U64_FMT " wal_events=%" K_U64_FMT " wal_records=%" K_U64_FMT " wal_open_files=%d wal_post_failed=%" K_U64_FMT " sync_us_ewma=%" K_U64_FMT " sync_us_max=%" K_U64_FMT " slow_syncs=%" K_U64_FMT " window_ms=%" K_U64_FMT " flush_by_target=%" K_U64_FMT " flush_by_drain=%" K_U64_FMT " flush_by_window=%" K_U64_FMT " flush_by_bytes=%" K_U64_FMT " flush_by_barrier=%" K_U64_FMT " flush_by_stop=%" K_U64_FMT " write_bytes=%" K_U64_FMT " batch_bytes_limit=%u wal_inflight_max=%u latency_budget_us=%u rounds=%" K_U64_FMT " client_requests=%" K_U64_FMT " snapshot_inflight=%d snapshot_failed=%d snapshot_cleanup_busy=%d cleanup_failed=%d flush_batches=%" K_U64_FMT " flush_writes=%" K_U64_FMT " peer_send_drops=%" K_U64_FMT " round_us_last=%" K_U64_FMT " round_us_max=%" K_U64_FMT " round_us_ewma=%" K_U64_FMT " slow_rounds=%" K_U64_FMT " req_age_ms_last=%" K_U64_FMT " req_age_ms_max=%" K_U64_FMT " slow_acks=%" K_U64_FMT " req_wait_ms_last=%" K_U64_FMT " req_wait_ms_max=%" K_U64_FMT " req_svc_ms_last=%" K_U64_FMT " req_svc_ms_max=%" K_U64_FMT " wake_us_last=%" K_U64_FMT " wake_us_max=%" K_U64_FMT " slow_wakes=%" K_U64_FMT " handoff_us_last=%" K_U64_FMT " handoff_us_max=%" K_U64_FMT " slow_handoffs=%" K_U64_FMT " handoff_samples=%" K_U64_FMT " wake_samples=%" K_U64_FMT " wake_pre_us_last=%" K_U64_FMT " wake_pre_us_max=%" K_U64_FMT " slow_wake_pres=%" K_U64_FMT " wake_pre_samples=%" K_U64_FMT " poll_us_last=%" K_U64_FMT " poll_us_max=%" K_U64_FMT " poll_us_ewma=%" K_U64_FMT " frames_last=%" K_U64_FMT " frames_max=%" K_U64_FMT " poll_samples=%" K_U64_FMT " ",
      info.id,info.state,info.leader_id,(k_i64)info.term,(k_i64)info.commit_index,(k_i64)info.last_applied,(k_i64)info.last_included_index,(k_i64)info.log_entry_count,(k_u64)tree_info.count,tree_info.height,(k_u64)tree_info.tree_bytes,(k_u64)tree_info.pending_free_count,(k_u64)tree_info.pending_free_bytes,server->persist_generation,server->wal_meta.record.segment,server->wal_meta.record.offset,server->wal_meta.record_size,server->wal_meta.next.segment,server->wal_meta.next.offset,server->wal_pending_items,server->wal_accumulated_events,server->wal_records,server->wal_worker.open_files,server->wal_post_failed,server->wal_worker.sync_us_ewma,server->wal_worker.sync_us_max,server->wal_worker.slow_syncs,(k_u64)server->cfg.flush_timeout_ms,server->flush_by_target,server->flush_by_drain,server->flush_by_window,server->flush_by_bytes,server->flush_by_barrier,server->flush_by_stop,server->write_bytes,(unsigned)server->cfg.flush_bytes_limit,(unsigned)server->wal_inflight_max,(unsigned)server->latency_budget_us,server->rounds,server->client_requests,server->snapshot_inflight,server->snapshot_failed,server->snapshot_cleanup_busy,server->snapshot_cleanup_failed,server->flush_batches,server->flush_writes_total,server->peer_send_drops,server->round_us_last,server->round_us_max,server->round_us_ewma,server->slow_rounds,server->req_age_ms_last,server->req_age_ms_max,server->slow_acks,server->req_wait_ms_last,server->req_wait_ms_max,server->req_svc_ms_last,server->req_svc_ms_max,server->wake_us_last,server->wake_us_max,server->slow_wakes,server->handoff_us_last,server->handoff_us_max,server->slow_handoffs,server->handoff_samples,server->wake_samples,server->wake_pre_us_last,server->wake_pre_us_max,server->slow_wake_pres,server->wake_pre_samples,server->poll_us_last,server->poll_us_max,server->poll_us_ewma,server->frames_last,server->frames_max,server->poll_samples);
    k_text_append(text,sizeof(text),&len,&over,"wal_inflight=%d client_connections=%u client_connection_limit=%u pending_requests=%u pending_request_limit=%u pending_request_bytes=%" K_U64_FMT " pending_request_bytes_limit=%" K_U64_FMT,
      server->wal_inflight_count,(unsigned)server->client_connection_count,(unsigned)K_CLIENT_CONNECTION_MAX,(unsigned)server->request_count,(unsigned)K_REQUEST_INFLIGHT_MAX,server->request_bytes,(k_u64)K_REQUEST_BYTES_MAX);
    k_text_append(text,sizeof(text),&len,&over," rx_buffer_bytes=%" K_U64_FMT " rx_buffer_bytes_limit=%" K_U64_FMT " stats_truncated=%" K_U64_FMT,server->rx_buffer_bytes,(k_u64)K_RX_BYTES_MAX,server->stats_truncated);
    if(over){
      server->stats_truncated++;
      k_text_append(text,sizeof(text),&len,&over," ...[TRUNCATED]");
      printf("warning: INFO/STATS reply did not fit %u bytes; marked and counted\n",(unsigned)sizeof(text));
    }
    return k_server_send_response(server,conn,request_id,K_STATUS_OK,info.leader_id,text,(k_u32)len);
  }
  if(type==K_REQ_SHUTDOWN){
    if(reader.off!=reader.len) return -1;
    if(k_server_send_response(server,conn,request_id,K_STATUS_OK,server->id,"stopping",8u)!=0) return -1;
    k_server_begin_stop(server);
    return 0;
  }
  return -1;
}
/* ================= Server: hello & connection IO ================= */
static int k_server_send_hello(k_conn *conn){
  k_u8 payload[K_HOST_MAX+9];
  k_server *server;
  const char *host;
  int local_index;
  size_t host_len;
  if(!conn||!conn->sock||!conn->server) return -1;
  server=conn->server;
  host="127.0.0.1";  /* fallback if the self entry is absent */
  local_index=k_cluster_index(&server->cluster,server->id);
  if(local_index>=0) host=server->cluster.nodes[local_index].host;
  host_len=strlen(host);
  if(host_len<1||host_len>=K_HOST_MAX) return -1;
  k_write_i32(payload,(k_i32)server->id);
  payload[4]=(k_u8)host_len;
  memcpy(payload+5,host,host_len);
  k_write_u16(payload+5+host_len,(k_u16)server->client_port);
  k_write_u16(payload+7+host_len,(k_u16)server->peer_port);
  return server->transport->send_frame(conn->sock,K_PEER_MAGIC,K_PEER_HELLO,payload,(k_u32)(9+host_len),1);
}
/* Inbound handlers: link logic pulled out of the cemon callbacks so a
   deterministic harness can feed frames directly instead of using sockets.
   All I/O goes through server->transport; nothing here touches cemon. */
static void k_server_peer_accepted(k_server *server,void *sock){
  k_conn *conn;
  if(!server||!server->admission){ server->transport->close(sock); return; }
  conn=k_conn_create(server,sock,K_CONN_PEER,0,-1);
  if(!conn){ server->transport->close(sock); return; }
  if(k_server_send_hello(conn)!=0||server->transport->recv(conn->sock)!=0){
    /* Stash, clear, close - in that order: cemon's close emits CEMON_CLOSED inline, and the handler
       frees this k_conn, so touching conn->sock afterwards writes into freed memory. */
    { void *dead=conn->sock; conn->sock=0; server->transport->close(dead); }
  }
}
static void k_server_peer_dialed(k_conn *conn){
  k_server *server=conn->server;
  if(k_server_send_hello(conn)!=0||server->transport->recv(conn->sock)!=0){
    /* Stash, clear, close - in that order: cemon's close emits CEMON_CLOSED inline, and the handler
       frees this k_conn, so touching conn->sock afterwards writes into freed memory. */
    { void *dead=conn->sock; conn->sock=0; server->transport->close(dead); }
  }
}
static void k_server_peer_received(k_conn *conn,const void *data,k_u32 size){
  k_server *server=conn->server;
  if(k_rx_feed(&conn->rx,K_PEER_MAGIC,data,size,k_server_peer_frame,conn)!=0||server->transport->recv(conn->sock)!=0){
    /* Stash, clear, close - in that order: cemon's close emits CEMON_CLOSED inline, and the handler
       frees this k_conn, so touching conn->sock afterwards writes into freed memory. */
    { void *dead=conn->sock; conn->sock=0; server->transport->close(dead); }
  }
}
static void k_server_client_accepted(k_server *server,void *sock){
  k_conn *conn;
  if(!server||!server->admission||server->client_connection_count>=K_CLIENT_CONNECTION_MAX){ server->transport->close(sock); return; }
  conn=k_conn_create(server,sock,K_CONN_CLIENT,0,-1);
  if(!conn){ server->transport->close(sock); return; }
  if(server->wal_inflight_count>=K_WAL_INFLIGHT_MAX||server->request_count>=K_REQUEST_INFLIGHT_MAX||server->request_bytes>=K_REQUEST_BYTES_MAX||server->rx_buffer_bytes>=K_RX_BYTES_MAX){
    conn->recv_paused=1;
  }else if(server->transport->recv(conn->sock)!=0){
    /* A failed read arm on a brand new connection used to be treated as fatal, which closed
       the connection of a client that had already sent its request.  Retry through the
       existing paused-connection path instead of dropping the client. */
    conn->recv_paused=1;
  }
}
static void k_server_client_received(k_conn *conn,const void *data,k_u32 size){
  k_server *server=conn->server;
  k_u32 old_len=conn->rx.len;
  int rc;
  if(server->rx_buffer_bytes>=K_RX_BYTES_MAX||(k_u64)size>K_RX_BYTES_MAX-server->rx_buffer_bytes){
    /* Stash, clear, close - in that order: cemon's close emits CEMON_CLOSED inline, and the handler
       frees this k_conn, so touching conn->sock afterwards writes into freed memory. */
    { void *dead=conn->sock; conn->sock=0; server->transport->close(dead); }
    return;
  }
  rc=k_rx_feed(&conn->rx,K_CLIENT_MAGIC,data,size,k_server_client_frame,conn);
  if(conn->rx.len>=old_len) server->rx_buffer_bytes+=(k_u64)(conn->rx.len-old_len);
  else server->rx_buffer_bytes-=(k_u64)(old_len-conn->rx.len);
  if(rc!=0){
    /* Stash, clear, close - in that order: cemon's close emits CEMON_CLOSED inline, and the handler
       frees this k_conn, so touching conn->sock afterwards writes into freed memory. */
    { void *dead=conn->sock; conn->sock=0; server->transport->close(dead); }
  }else if(server->wal_inflight_count>=K_WAL_INFLIGHT_MAX||server->request_count>=K_REQUEST_INFLIGHT_MAX||server->request_bytes>=K_REQUEST_BYTES_MAX||server->rx_buffer_bytes>=K_RX_BYTES_MAX){
    conn->recv_paused=1;
  }else if(server->transport->recv(conn->sock)!=0){
    /* Stash, clear, close - in that order: cemon's close emits CEMON_CLOSED inline, and the handler
       frees this k_conn, so touching conn->sock afterwards writes into freed memory. */
    { void *dead=conn->sock; conn->sock=0; server->transport->close(dead); }
  }
}
/* ================= Server: snapshot chunk streaming ================= */
static void k_snapshot_chunks_free(k_snapshot_chunk *chunk){
  while(chunk){
    k_snapshot_chunk *next=chunk->next;
    K_FREE(chunk->data);
    K_FREE(chunk);
    chunk=next;
  }
}
static int k_server_collect_snapshot_read(k_server *server,const raft_snapshot_read_req *read,k_snapshot_chunk **head){
  char path[K_URI_MAX];
  vfs_file *file;
  k_snapshot_chunk *chunk;
  raft_i64 remaining;
  k_u32 size;
  if(!server||!read||!head||read->byte_offset<0) return -1;
  /* Raft Sec. 7: snapshot_read always references the current last_included_index.
     A mismatch means the snapshot advanced between Ready emission and our
     processing, or the read is a stale leftover.  Discard it gracefully --
     the leader will issue a fresh InstallSnapshot for the new index. */
  if(read->last_included_index!=server->snapshot.index) return 0;
  remaining=server->snapshot.size-read->byte_offset;
  if(remaining<=0) return -1;
  size=read->byte_count;
  if((raft_i64)size>remaining) size=(k_u32)remaining;
  chunk=(k_snapshot_chunk *)K_CALLOC(1,sizeof(*chunk));
  if(!chunk) return -1;
  chunk->data=(k_u8 *)K_MALLOC(size);
  if(!chunk->data){
    K_FREE(chunk);
    return -1;
  }
  if(k_path_snapshot(path,server->base,read->last_included_index)!=0){
    k_snapshot_chunks_free(chunk);
    return -1;
  }
  file=vfs_open(path);
  if(!file){
    k_snapshot_chunks_free(chunk);
    return -1;
  }
  if(vfs_read(file,(k_u64)read->byte_offset,chunk->data,size)!=0){
    vfs_close(file);
    k_snapshot_chunks_free(chunk);
    return -1;
  }
  vfs_close(file);
  chunk->follower_id=read->follower_id;
  chunk->offset=read->byte_offset;
  chunk->size=size;
  chunk->next=*head;
  *head=chunk;
  return 0;
}
/* ================= Server: range scan & FCALL exec ================= */
typedef struct k_net_scan{
  k_buf body;
  int too_large;
  k_u32 limit;        /* max entries per page (0 = unlimited) */
  k_u32 count;        /* entries already serialized */
  int has_more;       /* 1 = more entries remain (next_key is the next one) */
  const k_u8 *next_key;
  k_u32 next_key_len;
} k_net_scan;
static int k_net_scan_visit(const unsigned char *key,unsigned int key_len,const unsigned char *value,unsigned int value_len,void *ud){
  k_net_scan *scan=(k_net_scan *)ud;
  k_u32 needed;
  if(scan->limit&&scan->count>=scan->limit){
    scan->next_key=key;
    scan->next_key_len=key_len;
    scan->has_more=1;
    return 1;
  }
  needed=key_len+value_len+1u+(scan->body.len?1u:0u);
  if(needed>K_RESPONSE_BODY_MAX-scan->body.len){
    scan->too_large=1;
    return 1;
  }
  if(scan->body.len) k_buf_u8(&scan->body,(k_u8)'\n');
  k_buf_bytes(&scan->body,key,key_len);
  k_buf_u8(&scan->body,(k_u8)'=');
  k_buf_bytes(&scan->body,value,value_len);
  scan->count++;
  return scan->body.err?1:0;
}
/* RSET visitor: append [key_len][key] for every key in the range so the apply
   can fork + overwrite each collected key atomically. */
static int k_rset_collect(const unsigned char *key,unsigned int key_len,const unsigned char *value,unsigned int value_len,void *ud){
  k_buf *list=(k_buf *)ud;
  (void)value; (void)value_len;
  k_buf_u32(list,key_len);
  k_buf_bytes(list,key,key_len);
  return list->err?-1:0;
}
/* Abort an FCALL that never reached apply (unknown/command error/OOM/redirect):
   free the request and reopen the gate so any queued writes can proceed. */
static void k_server_fcall_fail(k_server *server,k_request *request){
  k_request_free(server,request);
  k_server_open_gate(server);
}
static int k_server_exec_cmd(k_server *server,k_request *request){
  k_cmd_fn fn;
  k_cmd_ctx ctx;
  k_buf entry;
  int rc;
  fn=k_cmd_find(request->key,request->key_len);
  if(!fn){
    k_server_send_response(server,request->conn,request->id,K_STATUS_ERROR,server->id,"unknown command",15u);
    k_server_fcall_fail(server,request);
    return 0;
  }
  request->txn=treap_fork(server->tree);
  if(!request->txn){
    k_server_send_response(server,request->conn,request->id,K_STATUS_ERROR,server->id,"out of memory",13u);
    k_server_fcall_fail(server,request);
    return 0;
  }
  memset(&request->writes,0,sizeof(request->writes));
  memset(&request->result,0,sizeof(request->result));
  memset(&ctx,0,sizeof(ctx));
  ctx.txn=request->txn;
  ctx.writes=&request->writes;
  ctx.result=&request->result;
  ctx.args=request->end;
  ctx.args_len=request->end_len;
  ctx.write_count=0;
  rc=fn(&ctx);
  if(rc!=0||request->writes.err||request->result.err){
    k_buf_free(&request->writes);
    k_buf_free(&request->result);
    treap_abort(request->txn);
    request->txn=0;
    k_server_send_response(server,request->conn,request->id,K_STATUS_ERROR,server->id,"command failed",14u);
    k_server_fcall_fail(server,request);
    return 0;
  }
  memset(&entry,0,sizeof(entry));
  k_buf_u8(&entry,(k_u8)K_OP_FCALL);
  k_buf_bytes(&entry,request->writes.data,request->writes.len);
  if(entry.err||entry.len>K_RAFT_COMMAND_MAX){
    k_buf_free(&request->writes);
    k_buf_free(&request->result);
    treap_abort(request->txn);
    request->txn=0;
    k_buf_free(&entry);
    k_server_send_response(server,request->conn,request->id,K_STATUS_ERROR,server->id,"out of memory",13u);
    k_server_fcall_fail(server,request);
    return 0;
  }
  /* Attach the write-set entry to request->command; it enters write_head to batch-commit with subsequent ordinary writes */
  request->command=entry.data;
  request->command_size=entry.len;
  request->write_next=0;
  if(server->write_tail) server->write_tail->write_next=request;
  else server->write_head=request;
  server->write_tail=request;
  server->write_count++;
  server->writes_arrived++;
  server->write_bytes+=(k_u64)request->command_size;
  k_buf_free(&request->writes);
  /* The write-set W is already serialized into entry bytes for replication, so
     the leader's COW fork is no longer needed: apply replays W op-by-op on every
     node (see k_server_apply_exec).  Abort it here rather than carrying it to
     apply, so the apply path can never swap-root a (possibly stale) snapshot. */
  treap_abort(request->txn);
  request->txn=0;
  /* Do NOT flush immediately: the write-set entry batch-commits with the writes
     drained from gate_head by open_gate below (they append AFTER it, FIFO).  The
     gate can reopen immediately because every subsequent write appends after the
     write-set in write_head, and every read/scan/FCALL flushes write_head before
     its own barrier - so the write-set is always the immediate next log entry
     after the forked read and nothing can slip between. */
  k_server_open_gate(server);
  return 0;
}
/* ================= Server: client result handler ================= */
static int k_server_handle_client_result(k_server *server,const raft_client_result *result){
  k_request *request;
  const unsigned char *key,*value;
  unsigned int key_len,value_len;
  char text[64];
  int rc,len;
  request=k_request_find(server,result->cookie);
  if(!request){
    printf("warning: result (status=%d) for an unknown request cookie %p dropped\n",(int)result->status,result->cookie);
    return 0;
  }
  /* Age of a TERMINAL result, in injected milliseconds: how long the server held this request after
     admitting it.  A large value here means the server owns the latency; a small one with a slow
     client means the time went somewhere the server never saw, which is the split this project could
     not make before.  Intermediate catch-up results are not terminal and are not counted. */
  if(result->status!=RAFT_CLIENT_CATCHUP_READY&&request->admit_ms){
    k_u64 age_ms=(server->elapsed_total_ms>request->admit_ms)?(server->elapsed_total_ms-request->admit_ms):0u;
    k_u64 wait_ms=(request->submit_ms&&request->submit_ms>request->admit_ms)?(request->submit_ms-request->admit_ms):0u;
    k_u64 svc_ms=(request->submit_ms&&server->elapsed_total_ms>request->submit_ms)?(server->elapsed_total_ms-request->submit_ms):0u;
    server->req_age_ms_last=age_ms;
    if(age_ms>server->req_age_ms_max) server->req_age_ms_max=age_ms;
    if(age_ms>K_SLOW_ACK_MS) server->slow_acks++;
    /* The two halves answer different questions: the wait half is batching policy and backpressure,
       the service half is durability and the cross-thread wake.  Kept separately so a spike can be
       attributed to one of them instead of to "the server". */
    server->req_wait_ms_last=wait_ms;
    if(wait_ms>server->req_wait_ms_max) server->req_wait_ms_max=wait_ms;
    server->req_svc_ms_last=svc_ms;
    if(svc_ms>server->req_svc_ms_max) server->req_svc_ms_max=svc_ms;
    request->admit_ms=0;
    request->submit_ms=0;
  }
  /* A terminal result ends Raft's use of this request (including its live payload pointer). */
  if(result->status!=RAFT_CLIENT_CATCHUP_READY) request->submitted=0;
  if(!request->conn||request->internal){
    if(!request->internal) printf("warning: result for request id=%u whose connection is gone (type=%d status=%d)\n",(unsigned)request->id,(int)request->type,(int)result->status);
    /* No client to answer: either a server-created request (request->internal, the
       Sec 4.4 auto-replace state machine, driven entirely by these terminal results)
       or a client request whose connection was closed while it was in flight.  The
       two are told apart by the flag - inferring "internal" from conn==0 would let a
       disconnecting client drive the auto-replace state machine.  Intermediate
       catch-up progress keeps the request for the terminal result. */
    if(result->status!=RAFT_CLIENT_CATCHUP_READY){
      if(request->internal&&request->type==K_REQ_MEMBER&&server->auto_replace){
        if(result->status==RAFT_CLIENT_COMMITTED) k_server_auto_replace_committed(server);
        else if(result->status==RAFT_CLIENT_FAILED||result->status==RAFT_CLIENT_CATCHUP_FAILED||
                result->status==RAFT_CLIENT_REDIRECT) server->auto_replace_phase=0;
      }
      k_request_free(server,request);
    }
    return 0;
  }
  if(result->status==RAFT_CLIENT_REDIRECT){
    k_server_send_response(server,request->conn,request->id,K_STATUS_REDIRECT,result->leader_id,0,0);
    if(request->type==K_REQ_FCALL){ k_server_fcall_fail(server,request); return 0; }
  }
  else if(result->status==RAFT_CLIENT_FAILED){
    k_server_send_response(server,request->conn,request->id,K_STATUS_ERROR,result->leader_id,"raft request failed",19u);
    if(request->type==K_REQ_FCALL){ k_server_fcall_fail(server,request); return 0; }
  }
  else if(result->status==RAFT_CLIENT_CATCHUP_FAILED){
    /* reconfigs are serialized, so the in-flight catch-up is the only pending
       set: clear it so the failed node stops being dialed (its connection is
       dropped by the next reconnect as "not a member") */
    server->pending_count=0;
    k_server_send_response(server,request->conn,request->id,K_STATUS_ERROR,result->leader_id,"catch-up failed",16u);
  }
  else if(result->status==RAFT_CLIENT_CATCHUP_READY){
    return 0; /* intermediate catch-up progress: keep the request for the terminal result */
  }
  else if(result->status==RAFT_CLIENT_COMMITTED){
    if(request->type==K_REQ_FCALL||request->type==K_REQ_RDEL||request->type==K_REQ_MSET||request->type==K_REQ_MDEL||request->type==K_REQ_RSET){
      k_server_send_response(server,request->conn,request->id,K_STATUS_OK,result->leader_id,request->result.data,request->result.len);
      k_buf_free(&request->result);
      k_request_free(server,request);
      return 0;
    }
    if(request->type==K_REQ_CAS){
      /* The CAS compare ran at apply time; conflict was set there. */
      k_server_send_response(server,request->conn,request->id,request->conflict?K_STATUS_CONFLICT:K_STATUS_OK,result->leader_id,request->result.data,request->result.len);
      k_buf_free(&request->result);
      k_request_free(server,request);
      return 0;
    }
    k_server_send_response(server,request->conn,request->id,K_STATUS_OK,result->leader_id,0,0);
  }
  else if(result->status==RAFT_CLIENT_READY&&k_request_is_read(request->type)){
    if(request->type==K_REQ_GET){
      rc=treap_get(server->tree,request->key,request->key_len,&value,&value_len);
      if(rc>0) k_server_send_response(server,request->conn,request->id,K_STATUS_OK,result->leader_id,value,value_len);
      else if(rc==0) k_server_send_response(server,request->conn,request->id,K_STATUS_NOT_FOUND,result->leader_id,0,0);
      else k_server_send_response(server,request->conn,request->id,K_STATUS_ERROR,result->leader_id,"read failed",11u);
    }else if(request->type==K_REQ_MGET){
      /* batch point read: [count][k1_len][k1]... -> "k=v\n" for found keys (missing skipped) */
      k_reader r;
      k_buf body;
      k_u32 count,i;
      int too_large;
      memset(&r,0,sizeof(r));
      r.data=request->key;
      r.len=request->key_len;
      count=k_reader_u32(&r);
      memset(&body,0,sizeof(body));
      too_large=0;
      for(i=0;i<count;i++){
        key_len=k_reader_u32(&r);
        key=k_reader_bytes(&r,key_len);
        if(r.err) break;
        if(treap_get(server->tree,key,key_len,&value,&value_len)==1){
          if(key_len+value_len+2u>K_RESPONSE_BODY_MAX-body.len){ too_large=1; break; }
          k_buf_bytes(&body,key,key_len);
          k_buf_u8(&body,(k_u8)'=');
          k_buf_bytes(&body,value,value_len);
          k_buf_u8(&body,(k_u8)'\n');
        }
      }
      if(r.err||body.err) k_server_send_response(server,request->conn,request->id,K_STATUS_ERROR,result->leader_id,"mget failed",11u);
      else if(too_large) k_server_send_response(server,request->conn,request->id,K_STATUS_ERROR,result->leader_id,"result too large",16u);
      else k_server_send_response(server,request->conn,request->id,K_STATUS_OK,result->leader_id,body.data,body.len);
      k_buf_free(&body);
    }else if(request->type==K_REQ_COUNT){
      len=sprintf(text,"%" K_U64_FMT,(k_u64)((request->key_len||request->end_len)?treap_count_range(server->tree,request->key,request->key_len,request->end,request->end_len):treap_count(server->tree)));
      k_server_send_response(server,request->conn,request->id,K_STATUS_OK,result->leader_id,text,(k_u32)len);
    }else if(request->type==K_REQ_MIN||request->type==K_REQ_MAX){
      k_buf body;
      if(request->key_len||request->end_len){
        if(request->type==K_REQ_MIN) rc=treap_min_range(server->tree,request->key,request->key_len,request->end,request->end_len,&key,&key_len,&value,&value_len);
        else rc=treap_max_range(server->tree,request->key,request->key_len,request->end,request->end_len,&key,&key_len,&value,&value_len);
      }else{
        if(request->type==K_REQ_MIN) rc=treap_min(server->tree,&key,&key_len,&value,&value_len);
        else rc=treap_max(server->tree,&key,&key_len,&value,&value_len);
      }
      if(rc==0) k_server_send_response(server,request->conn,request->id,K_STATUS_OK,result->leader_id,"(empty)",7u);
      else if(rc<0) k_server_send_response(server,request->conn,request->id,K_STATUS_ERROR,result->leader_id,"read failed",11u);
      else{
        memset(&body,0,sizeof(body));
        k_buf_bytes(&body,key,key_len);
        k_buf_u8(&body,(k_u8)'=');
        k_buf_bytes(&body,value,value_len);
        if(body.err||body.len>K_RESPONSE_BODY_MAX) k_server_send_response(server,request->conn,request->id,K_STATUS_ERROR,result->leader_id,"result too large",16u);
        else k_server_send_response(server,request->conn,request->id,K_STATUS_OK,result->leader_id,body.data,body.len);
        k_buf_free(&body);
      }
    }else if(request->type==K_REQ_MEMBERS){
      k_buf body;
      memset(&body,0,sizeof(body));
      if(k_server_members_build(server,&body)!=0||body.len>K_RESPONSE_BODY_MAX) k_server_send_response(server,request->conn,request->id,K_STATUS_ERROR,result->leader_id,"result too large",16u);
      else k_server_send_response(server,request->conn,request->id,K_STATUS_OK,result->leader_id,body.data,body.len);
      k_buf_free(&body);
    }else if(request->type==K_REQ_TOPOLOGY){
      k_buf body;
      memset(&body,0,sizeof(body));
      if(k_server_topology_build(server,&body)!=0||body.len>K_RESPONSE_BODY_MAX) k_server_send_response(server,request->conn,request->id,K_STATUS_ERROR,result->leader_id,"result too large",16u);
      else k_server_send_response(server,request->conn,request->id,K_STATUS_OK,result->leader_id,body.data,body.len);
      k_buf_free(&body);
    }else if(request->type==K_REQ_RGET){
      k_reader r;
      k_net_scan scan;
      k_buf resp;
      treap_u64 scan_count;
      k_u32 begin_len,end_len,limit;
      const k_u8 *begin,*end;
      int direction;
      memset(&r,0,sizeof(r));
      r.data=request->command;
      r.len=request->command_size;
      begin_len=k_reader_u32(&r);
      begin=k_reader_bytes(&r,begin_len);
      end_len=k_reader_u32(&r);
      end=k_reader_bytes(&r,end_len);
      direction=(int)k_reader_u8(&r);
      limit=0;
      if(r.off<r.len) limit=k_reader_u32(&r);
      if(r.err||r.off!=r.len){ k_server_send_response(server,request->conn,request->id,K_STATUS_ERROR,result->leader_id,"scan failed",11u); }
      else{
        memset(&scan,0,sizeof(scan));
        scan.limit=limit;
        scan_count=treap_scan(server->tree,begin,begin_len,end,end_len,direction,k_net_scan_visit,&scan);
        if(scan_count==(treap_u64)-1||scan.body.err) k_server_send_response(server,request->conn,request->id,K_STATUS_ERROR,result->leader_id,"scan failed",11u);
        else if(scan.too_large) k_server_send_response(server,request->conn,request->id,K_STATUS_ERROR,result->leader_id,"scan result too large",21u);
        else if(limit>0){
          memset(&resp,0,sizeof(resp));
          k_buf_u8(&resp,scan.has_more?1u:0u);
          if(scan.has_more){
            k_buf_u32(&resp,scan.next_key_len);
            k_buf_bytes(&resp,scan.next_key,scan.next_key_len);
          }
          k_buf_bytes(&resp,scan.body.data,scan.body.len);
          if(resp.err||resp.len>K_RESPONSE_BODY_MAX) k_server_send_response(server,request->conn,request->id,K_STATUS_ERROR,result->leader_id,"scan result too large",21u);
          else k_server_send_response(server,request->conn,request->id,K_STATUS_OK,result->leader_id,resp.data,resp.len);
          k_buf_free(&resp);
        }else k_server_send_response(server,request->conn,request->id,K_STATUS_OK,result->leader_id,scan.body.data,scan.body.len);
        k_buf_free(&scan.body);
      }
    }
  }else if(result->status==RAFT_CLIENT_READY&&request->type==K_REQ_FCALL){
    k_server_exec_cmd(server,request);
    return 0;
  }else k_server_send_response(server,request->conn,request->id,K_STATUS_ERROR,result->leader_id,"unexpected raft result",22u);
  k_request_free(server,request);
  return 0;
}
static void k_ready_bundle_free(k_ready_bundle *bundle){
  int i;
  if(!bundle) return;
  K_FREE(bundle->wal_payload);
  for(i=0;i<bundle->message_count;i++) K_FREE(bundle->messages[i].payload);
  for(i=0;i<bundle->apply_count;i++){
    K_FREE(bundle->applies[i].command);
    K_FREE(bundle->applies[i].cfg_old_ids);
    K_FREE(bundle->applies[i].cfg_new_ids);
    K_FREE(bundle->applies[i].cfg_learners_ids);
  }
  for(i=0;i<bundle->client_count;i++) K_FREE(bundle->clients[i].response);
  K_FREE(bundle->messages);
  K_FREE(bundle->applies);
  K_FREE(bundle->clients);
  K_FREE(bundle->snapshot_reads);
  k_snapshot_cache_free(&bundle->snapshot_after);
  K_FREE(bundle);
}
static k_ready_bundle *k_ready_bundle_copy(k_server *server,const raft_ready *ready){
  k_ready_bundle *bundle;
  int i;
  if(!server||!ready) return 0;
  bundle=(k_ready_bundle *)K_CALLOC(1,sizeof(*bundle));
  if(!bundle) return 0;
  bundle->server=server;
  bundle->phase_stopped=ready->phase_stopped;
  bundle->snapshot_install_needed=ready->snapshot_install_needed;
  bundle->snapshot_install_index=ready->snapshot_last_index;
  bundle->is_leader=ready->is_leader;
  server->cfg_joint=ready->config_joint;
  server->self_is_voter=ready->self_is_voter;
  server->self_is_learner=ready->self_is_learner;
  bundle->leader_id=ready->leader_id;
  if(ready->message_count<0||ready->apply_count<0||ready->client_result_count<0||ready->snapshot_read_count<0) goto fail;
  if(ready->message_count){
    bundle->messages=(k_ready_message *)K_CALLOC((unsigned int)ready->message_count,sizeof(k_ready_message));
    if(!bundle->messages) goto fail;
  }
  bundle->message_count=ready->message_count;
  for(i=0;i<ready->message_count;i++){
    k_buf encoded;
    const raft_peer_message *message=&ready->messages[i];
    if(k_peer_encode(message,&encoded)!=0) goto fail;
    bundle->messages[i].to=message->to;
    bundle->messages[i].control=message->type!=RAFT_MSG_APPEND&&message->type!=RAFT_MSG_INSTALL_SNAPSHOT;
    if(message->type==RAFT_MSG_APPEND&&message->append_entries.entry_count==0) bundle->messages[i].control=1;
    bundle->messages[i].payload=encoded.data;
    bundle->messages[i].size=encoded.len;
  }
  if(ready->apply_count){
    bundle->applies=(k_ready_apply *)K_CALLOC((unsigned int)ready->apply_count,sizeof(k_ready_apply));
    if(!bundle->applies) goto fail;
  }
  bundle->apply_count=ready->apply_count;
  for(i=0;i<ready->apply_count;i++){
    bundle->applies[i].index=ready->apply_entries[i].index;
    bundle->applies[i].command_size=ready->apply_entries[i].command_size;
    bundle->applies[i].cookie=ready->apply_entries[i].cookie;
    bundle->applies[i].kind=ready->apply_entries[i].kind;
    if(ready->apply_entries[i].command_size){
      if(!ready->apply_entries[i].command) goto fail;
      bundle->applies[i].command=(k_u8 *)K_MALLOC(ready->apply_entries[i].command_size);
      if(!bundle->applies[i].command) goto fail;
      memcpy(bundle->applies[i].command,ready->apply_entries[i].command,ready->apply_entries[i].command_size);
    }
    /* CONFIG entries carry no data: deep-copy the new member set so the async
       bundle stays self-contained after raft_ready_consumed invalidates the
       raft-internal cfg pointers */
    if(ready->apply_entries[i].kind==RAFT_ENTRY_CONFIG&&ready->apply_entries[i].cfg_new){
      if(ready->apply_entries[i].cfg_old){
        bundle->applies[i].cfg_old_count=ready->apply_entries[i].cfg_old->id_count;
        if(bundle->applies[i].cfg_old_count>0){
          bundle->applies[i].cfg_old_ids=(int *)K_MALLOC(sizeof(int)*(unsigned int)bundle->applies[i].cfg_old_count);
          if(!bundle->applies[i].cfg_old_ids) goto fail;
          memcpy(bundle->applies[i].cfg_old_ids,ready->apply_entries[i].cfg_old->ids,sizeof(int)*(unsigned int)bundle->applies[i].cfg_old_count);
        }
      }
      bundle->applies[i].cfg_new_count=ready->apply_entries[i].cfg_new->id_count;
      if(bundle->applies[i].cfg_new_count>0){
        bundle->applies[i].cfg_new_ids=(int *)K_MALLOC(sizeof(int)*(unsigned int)bundle->applies[i].cfg_new_count);
        if(!bundle->applies[i].cfg_new_ids) goto fail;
        memcpy(bundle->applies[i].cfg_new_ids,ready->apply_entries[i].cfg_new->ids,sizeof(int)*(unsigned int)bundle->applies[i].cfg_new_count);
      }
      if(ready->apply_entries[i].cfg_learners){
        bundle->applies[i].cfg_learners_count=ready->apply_entries[i].cfg_learners->id_count;
        if(bundle->applies[i].cfg_learners_count>0){
          bundle->applies[i].cfg_learners_ids=(int *)K_MALLOC(sizeof(int)*(unsigned int)bundle->applies[i].cfg_learners_count);
          if(!bundle->applies[i].cfg_learners_ids) goto fail;
          memcpy(bundle->applies[i].cfg_learners_ids,ready->apply_entries[i].cfg_learners->ids,sizeof(int)*(unsigned int)bundle->applies[i].cfg_learners_count);
        }
      }
    }
  }
  if(ready->client_result_count){
    bundle->clients=(k_ready_client *)K_CALLOC((unsigned int)ready->client_result_count,sizeof(k_ready_client));
    if(!bundle->clients) goto fail;
  }
  bundle->client_count=ready->client_result_count;
  for(i=0;i<ready->client_result_count;i++){
    bundle->clients[i].result=ready->client_results[i];
    if(ready->client_results[i].response_size){
      if(!ready->client_results[i].response) goto fail;
      bundle->clients[i].response=(k_u8 *)K_MALLOC(ready->client_results[i].response_size);
      if(!bundle->clients[i].response) goto fail;
      memcpy(bundle->clients[i].response,ready->client_results[i].response,ready->client_results[i].response_size);
      bundle->clients[i].result.response=bundle->clients[i].response;
    }
  }
  if(ready->snapshot_read_count){
    bundle->snapshot_reads=(raft_snapshot_read_req *)K_MALLOC(sizeof(raft_snapshot_read_req)*(unsigned int)ready->snapshot_read_count);
    if(!bundle->snapshot_reads) goto fail;
    memcpy(bundle->snapshot_reads,ready->snapshot_reads,sizeof(raft_snapshot_read_req)*(unsigned int)ready->snapshot_read_count);
  }
  bundle->snapshot_read_count=ready->snapshot_read_count;
  bundle->item_count=(k_u32)(ready->message_count+ready->apply_count+ready->client_result_count+ready->snapshot_read_count);
  if(ready->persist_needed&&k_state_serialize(server,&ready->persist,bundle)!=0) goto fail;
  return bundle;
fail:
  k_ready_bundle_free(bundle);
  return 0;
}
/* ================= Server: WAL worker ================= */
static void k_wal_worker_entry(runtime_ctx *runtime,void *arg){
  k_wal_worker *worker=(k_wal_worker *)arg;
  runtime_worker_ready(runtime);
  while(!runtime_should_stop(runtime)){
    runtime_fn fn;
    void *task_arg=0;
    int rc=runtime_task_poll(runtime,(int)worker->server->cfg.flush_timeout_ms,&fn,&task_arg);
    (void)fn;
    if(rc<0) break;
    if(rc>0&&task_arg){
      k_u64 t0,t1;
      k_ready_bundle *bundle=(k_ready_bundle *)task_arg;
      t0=k_wal_sync_now();
      bundle->wal_ok=k_wal_append_bundle(worker,bundle)==0;
      t1=k_wal_sync_now();
      if(t1>=t0){
        /* Min-biased EWMA: the window is a LATENCY knob, so it should track the fast
           end of the sync distribution (drop toward a fast sample at once, creep up
           slowly).  A plain average of 1.7-12 ms samples made the window flap. */
        k_u64 dur=t1-t0;
        if(worker->sync_us_ewma==0u||dur<worker->sync_us_ewma) worker->sync_us_ewma=dur;
        else if(dur<worker->sync_us_ewma*4u) worker->sync_us_ewma=worker->sync_us_ewma+(dur-worker->sync_us_ewma)/8u;
        /* A sample far above the current estimate does NOT raise it.  The window this feeds must track
           the sync the device normally delivers; rare multi-millisecond syncs (they are visible in
           sync_us_max / slow_syncs) used to drag it up by ~600 us each, and on the fast guest that
           inflation cost half the measured throughput: 67247 ops/s at a 1 ms window against 35822
           once outliers had grown it to 2-3 ms.  Outliers are still recorded and still counted. */
        if(dur>worker->sync_us_max) worker->sync_us_max=dur;
        if(dur>K_SLOW_SYNC_US) worker->slow_syncs++;
        /* How long the queued bundle waited before THIS worker thread started writing it - the half of
           the handoff that the loop's own accounting cannot see, because the loop is not working while
           it waits.  Same real-clock helper as the sync measurement above, diagnostics only. */
        if(worker->server&&worker->server->wal_task_post_us){
          k_u64 ho=(t0>=worker->server->wal_task_post_us)?(t0-worker->server->wal_task_post_us):0u;
          worker->server->handoff_us_last=ho;
          if(ho>worker->server->handoff_us_max) worker->server->handoff_us_max=ho;
          if(ho>K_SLOW_HANDOFF_US) worker->server->slow_handoffs++;
          worker->server->handoff_samples++;
        }
      }
      if(worker->server) worker->server->wal_post_us=k_wal_sync_now();   /* diagnostic stamp only */
      if(runtime_result_post(runtime,0,bundle)!=0){
        /* The loop incremented wal_inflight_count for this bundle and only a DELIVERED result can
           decrement it.  A silently dropped result leaks one of the K_WAL_INFLIGHT_MAX slots, and
           once they are gone the node refuses writes and stalls reads while still looking alive.
           Losing a result is therefore fail-stop, with the reason printed. */
        if(worker->server){
          /* Distinguish shutdown from a live failure: during an orderly stop the loop is going away
             and a refused post is expected (the last in-flight result arrives after runtime_stop),
             so it must not turn into a fatal.  A refused post while the server is serving is a lost
             result and IS fatal (it would leak a wal_inflight_count slot forever). */
          if(server_is_stopping(worker->server)){
            k_ready_bundle_free(bundle);
            continue;
          }
          worker->server->wal_post_failed++;
          worker->server->fatal=1;
          fprintf(stderr,"fatal: WAL result could not be posted back to the event loop (in-flight=%d) - fail-stop\n",
                  (int)worker->server->wal_inflight_count);
          if(worker->server->on_wal_result) worker->server->on_wal_result(worker->server->loop,worker->server);
        }
        k_ready_bundle_free(bundle);
      }
      else if(worker->server->on_wal_result) worker->server->on_wal_result(worker->server->loop,worker->server);
    }
  }
  /* Release the held handles only on a real stop.  The deterministic "sync" backend
     re-enters this entry once per drain (rc<0 means "batch drained", not "stopped"),
     so closing on every exit would defeat the cache in exactly the tests that drive
     the worker inline.  k_server_release stops the runtime and waits for the workers,
     so this close is guaranteed to run on shutdown. */
  if(runtime_should_stop(runtime)) k_wal_files_close(worker);
  runtime_worker_exit(runtime);
}
/* ================= Server: snapshot worker ================= */
static void k_snapshot_task_free(k_snapshot_task *task){
  if(!task) return;
  K_FREE(task);
}
/* Streaming snapshot sink: bytes go STRAIGHT to the versioned snapshot file while the
   CRC is computed on the fly, so the save never materializes the state in memory (the
   previous version built the whole image in a k_buf first: peak memory was ~the state
   size on top of the live tree).  The file is written in place (no temp file + rename):
   atomicity comes from the versioned name plus the CRC trailer - a torn/partial file
   fails validation below and is never trusted, and the reader falls back to the
   previous snapshot. */
typedef struct k_snapshot_sink{
  vfs_file *file;
  k_u64 off;
  k_crc32_ctx crc;
  int err;
} k_snapshot_sink;
static int k_snapshot_sink_write(k_snapshot_sink *sink,const void *data,unsigned int size){
  if(!sink||sink->err) return -1;
  if(vfs_write(sink->file,sink->off,data,size)!=0){
    sink->err=1;
    return -1;
  }
  sink->off+=(k_u64)size;
  k_crc32_update(&sink->crc,data,size);
  return 0;
}
static int k_snapshot_sink_write_cb(void *ud,const unsigned char *data,unsigned int size){
  return k_snapshot_sink_write((k_snapshot_sink *)ud,data,size);
}
static void k_snapshot_worker_save(k_snapshot_task *task){
  char path[K_URI_MAX];
  vfs_file *file;
  k_snapshot_sink sink;
  k_buf hdr;
  k_u8 trailer[4],check[4],probe;
  k_u32 i,crc;
  int ok=1;
  task->ok=0;
  if(k_path_snapshot(path,task->server->base,task->index)!=0) return;
  file=vfs_open(path);
  if(!file) return;
  /* Header first: magic + the captured address book, then the treap bytes streamed by
     treap_save through the sink. */
  memset(&hdr,0,sizeof(hdr));
  k_buf_u32(&hdr,K_SNAPSHOT_MAGIC);
  k_buf_u32(&hdr,(k_u32)task->cluster_copy.count);
  for(i=0;i<(k_u32)task->cluster_copy.count;i++){
    const k_node_spec *node=&task->cluster_copy.nodes[i];
    k_u32 host_len=(k_u32)strlen(node->host);
    k_buf_i32(&hdr,(k_i32)node->id);
    k_buf_u32(&hdr,host_len);
    k_buf_bytes(&hdr,node->host,host_len);
    k_buf_u16(&hdr,node->client_port);
    k_buf_u16(&hdr,node->peer_port);
  }
  memset(&sink,0,sizeof(sink));
  sink.file=file;
  k_crc32_init(&sink.crc);
  if(hdr.err||k_snapshot_sink_write(&sink,hdr.data,hdr.len)!=0) ok=0;
  k_buf_free(&hdr);
  if(ok&&treap_save(task->server->tree,k_snapshot_sink_write_cb,&sink)!=0) ok=0;
  if(ok){
    /* CRC32 trailer over everything before it (same layout the loader expects). */
    k_crc32_final(&sink.crc,&crc);
    k_write_u32(trailer,crc);
    if(vfs_write(file,sink.off,trailer,4)!=0) ok=0;
    else sink.off+=4u;
  }
  if(ok&&vfs_sync(file)!=0) ok=0;
  if(ok){
    /* The file is complete only if it ENDS exactly at the trailer: read the trailer
       back and probe for bytes past the end.  A short/torn write, or bytes left over
       from a previous longer incarnation, fail the check - and a save that does not
       pass it is never reported as ready nor used to unlink anything. */
    if(vfs_read(file,sink.off-4u,check,4u)!=0||memcmp(check,trailer,4)!=0) ok=0;
    else if(vfs_read(file,sink.off,&probe,1u)==0) ok=0;
  }
  vfs_close(file);
  task->size=sink.off;
  task->serialized=1;            /* the captured view has been consumed */
  task->ok=ok?1:0;
}
static void k_snapshot_worker_cleanup(k_snapshot_task *task){
  char path[K_URI_MAX];
  k_u64 segment;
  int failed=0;
  if(task->old_snapshot_index>0){
    if(k_path_snapshot(path,task->server->base,task->old_snapshot_index)!=0||vfs_unlink(path)!=0) failed=1;
  }
  for(segment=0;segment<task->keep_segment;segment++){
    if(k_path_wal_segment(path,task->server->base,segment)!=0||vfs_unlink(path)!=0) failed=1;
  }
  task->cleanup_failed=failed;
  task->ok=1;
}
static void k_snapshot_worker_entry(runtime_ctx *runtime,void *arg){
  k_server *server=(k_server *)arg;
  runtime_worker_ready(runtime);
  while(!runtime_should_stop(runtime)){
    runtime_fn fn;
    void *task_arg=0;
    int rc=runtime_task_poll(runtime,(int)server->cfg.poll_ms,&fn,&task_arg);
    if(rc<0) break;
    if(rc>0&&task_arg){
      k_snapshot_task *task=(k_snapshot_task *)task_arg;
      if(task->kind==K_SNAPSHOT_TASK_SAVE) k_snapshot_worker_save(task);
      else if(task->kind==K_SNAPSHOT_TASK_CLEANUP) k_snapshot_worker_cleanup(task);
      else task->ok=0;
      if(runtime_result_post(runtime,0,task)!=0){
        /* Do NOT free here: the main thread still holds this pointer in server->snapshot_task and
           would free it again (double free).  On a post failure the snapshot simply never
           completes - the main thread keeps snapshot_inflight set, so no further snapshot is
           attempted and nothing is used after free.  Memory safety beats the leak. */
      }
    }
  }
  runtime_worker_exit(runtime);
}
/* ================= Server: stop & snapshot poll ================= */
static void k_server_maybe_finish_stop(k_server *server){
  if(!server||!server->raft_stopped||server->wal_inflight_count>0||server->snapshot_task||server->snapshot_inflight||server->snapshot_cleanup_busy) return;
  /* Latch: this is reached from the snapshot poll, the WAL poll AND the no-work path of one advance, so
     without the test the host's on_stop - documented as fired once - could run up to three times in a
     single step. */
  if(server->stopped) return;
  server->stopped=1;
  if(server->on_stop) server->on_stop(server->loop);
}
static void k_server_set_snapshot_failed_baseline(k_server *server){
  if(!server) return;
  server->snapshot_failed=1;
  server->snapshot_failed_segment_baseline=server->wal_meta.generation?server->wal_meta.record.segment:0;
  server->snapshot_failed_apply_baseline=server->last_applied;
}
static void k_server_schedule_cleanup(k_server *server,raft_i64 old_snapshot_index,k_u64 keep_segment);
static int k_server_poll_snapshot(k_server *server){
  runtime_fn fn;
  void *arg;
  int rc;
  if(!server||!server->snapshot_rt) return -1;
  for(;;){
    arg=0;
    rc=runtime_result_poll(server->snapshot_rt,0,&fn,&arg);
    if(rc<0) return server->stopping?0:-1;
    if(rc==0) break;
    if(arg){
      k_snapshot_task *task=(k_snapshot_task *)arg;
      if(task->kind==K_SNAPSHOT_TASK_CLEANUP){
        if(server->snapshot_cleanup_busy>0) server->snapshot_cleanup_busy--;
        if(task->cleanup_failed) server->snapshot_cleanup_failed=1;
        k_snapshot_task_free(task);
      }else if(task->kind==K_SNAPSHOT_TASK_SAVE){
        if(task!=server->snapshot_task){
          k_snapshot_task_free(task);
          return -1;
        }
        server->snapshot_task=0;
        /* Completion point: the save has reported (ok or failed), so release the captured view
           HERE - on the owner thread - instead of inside the snapshot worker. */
        treap_save_finish(server->tree);
        if(server->stopping||server->raft_stopped){
          server->snapshot_inflight=0;
          k_snapshot_task_free(task);
        }else if(task->ok&&raft_snapshot_data_ready(server->raft,(raft_i64)task->size)==0){
          /* The new snapshot is durable AND verified, so the older-than-previous one can
             go - together with the segments before the PREVIOUS base's segment, which the
             rollback target still needs. */
          k_server_schedule_cleanup(server,server->snapshot_older_index,server->snapshot_prev_wal_segment);
          k_snapshot_task_free(task);
        }else{
          /* A failed/torn save is not retried from the same task: there is no
             in-memory image to reuse (it was streamed), and the captured view is
             already released.  The failure baseline makes the snapshot policy fire
             again, so the next attempt captures the CURRENT state instead. */
          server->snapshot_inflight=0;
          k_server_set_snapshot_failed_baseline(server);
          server->snapshot_retry=0;
          k_snapshot_task_free(task);
        }
      }else k_snapshot_task_free(task);
    }
  }
  k_server_maybe_finish_stop(server);
  return 0;
}
static void k_server_schedule_cleanup(k_server *server,raft_i64 old_snapshot_index,k_u64 keep_segment){
  k_snapshot_task *task;
  if(!server||(!old_snapshot_index&&!keep_segment)) return;
  task=(k_snapshot_task *)K_CALLOC(1,sizeof(*task));
  if(!task){
    server->snapshot_cleanup_failed=1;
    return;
  }
  task->server=server;
  task->kind=K_SNAPSHOT_TASK_CLEANUP;
  task->old_snapshot_index=old_snapshot_index;
  task->keep_segment=keep_segment;
  if(runtime_task_post(server->snapshot_rt,0,task)!=0){
    k_snapshot_task_free(task);
    server->snapshot_cleanup_failed=1;
    return;
  }
  server->snapshot_cleanup_busy++;
}
/* Send one raft message to a peer.  A missing/unconnected peer and a failed
   send are BOTH non-fatal: the connection is re-dialed by k_server_reconnect
   and the message is re-emitted by raft on the next Ready (nothing here ever
   fails in a way the caller must handle). */
/* Returns 0 when the frame was handed to the transport, -1 when it was dropped.
   Drops used to be silent (the transport result was ignored and a missing peer
   socket returned early), which hides a link that cannot keep up: the failure
   shows up much later as a follower timing out.  They are counted here so the
   server can report them, and the count is asserted by kserver_test. */
static int k_server_send_ready_message(k_server *server,const k_ready_message *message){
  int node_index;
  int rc;
  if(!server||!message) return -1;
  node_index=k_cluster_index(&server->cluster,message->to);
  if(node_index<0){
    server->peer_send_drops++;
    return -1;
  }
  if(!server->peer_socks[node_index]){
    server->peer_send_drops++;
    return -1;
  }
  rc=server->transport->send_frame(server->peer_socks[node_index],K_PEER_MAGIC,K_PEER_RAFT,message->payload,message->size,message->control);
  if(rc!=0){
    server->peer_send_drops++;
    return -1;
  }
  return 0;
}
/* Immediate half of Ready processing.  Applies, snapshot install, messages,
   client results, and snapshot reads are all independent of persist durability,
   so they run synchronously right after raft_advance - heartbeats and state-
   machine applies are never stalled by the WAL fsync.  The deferred half
   (raft_persist_complete) runs on the main thread once the WAL write lands. */
/* ================= Server: bundle live/persist & WAL poll ================= */
static int k_server_process_bundle_live(k_server *server,k_ready_bundle *bundle){
  raft_i64 applied_index=0,install_index=0;
  k_snapshot_chunk *reads=0,*chunk,*next;
  int i,apply_done=0,install_done=0;
  if(!server||!bundle) return -1;
  for(i=0;i<bundle->apply_count;i++){
    if(bundle->applies[i].kind==RAFT_ENTRY_CONFIG){
      k_membership_update(server,bundle->applies[i].cfg_old_ids,bundle->applies[i].cfg_old_count,bundle->applies[i].cfg_new_ids,bundle->applies[i].cfg_new_count,bundle->applies[i].cfg_learners_ids,bundle->applies[i].cfg_learners_count);
    }else if(bundle->applies[i].command_size&&k_server_apply_command(server,bundle->applies[i].command,bundle->applies[i].command_size,bundle->applies[i].cookie)!=0){
      printf("fatal: apply command failed\n");
      server->fatal=1;
    }
    applied_index=bundle->applies[i].index;
    apply_done=1;
  }
  if(apply_done) server->last_applied=applied_index;
  if(bundle->snapshot_install_needed&&!server->snapshot_inflight){
    if(k_snapshot_load_file(server,bundle->snapshot_install_index,server->tree)!=0){ printf("fatal: snapshot load failed\n"); server->fatal=1; }
    else{
      install_index=bundle->snapshot_install_index;
      install_done=1;
      /* The installed snapshot advances the state machine to install_index, but
         the applies above (if any) only reached applied_index <= install_index.
         Keep server->last_applied aligned with the tree and with r->last_applied
         (raft_apply_complete(install_index) below), otherwise a later local
         snapshot would name its file .snapshot.<stale> while raft publishes at
         install_index, and streaming that snapshot would open the wrong file and
         fatal the node. */
      if(server->last_applied<install_index) server->last_applied=install_index;
    }
  }
  for(i=0;i<bundle->message_count;i++) k_server_send_ready_message(server,&bundle->messages[i]);
  for(i=0;i<bundle->client_count;i++) k_server_handle_client_result(server,&bundle->clients[i].result);
  for(i=0;i<bundle->snapshot_read_count;i++) if(k_server_collect_snapshot_read(server,&bundle->snapshot_reads[i],&reads)!=0){ printf("fatal: collect snapshot read failed\n"); server->fatal=1; }
  if(apply_done&&raft_apply_complete(server->raft,applied_index)!=0){ printf("fatal: raft apply complete failed\n"); server->fatal=1; }
  if(install_done&&raft_apply_complete(server->raft,install_index)!=0){ printf("fatal: raft install complete failed\n"); server->fatal=1; }
  for(chunk=reads;chunk;chunk=next){
    next=chunk->next;
    if(raft_snapshot_data_provided(server->raft,chunk->follower_id,chunk->offset,chunk->data,chunk->size)!=0){
      K_FREE(chunk->data);
      K_FREE(chunk);
      printf("fatal: snapshot data provided failed\n");
      server->fatal=1;
    }else{
      chunk->next=server->provided_chunks;
      server->provided_chunks=chunk;
    }
  }
  /* Release the live payloads consumed above; keep wal_payload + persist
     metadata for the deferred WAL write (k_ready_bundle_free skips the now-null
     arrays, so no double free). */
  for(i=0;i<bundle->message_count;i++) K_FREE(bundle->messages[i].payload);
  K_FREE(bundle->messages); bundle->messages=0; bundle->message_count=0;
  for(i=0;i<bundle->apply_count;i++){
    K_FREE(bundle->applies[i].command);
    K_FREE(bundle->applies[i].cfg_old_ids);
    K_FREE(bundle->applies[i].cfg_new_ids);
    K_FREE(bundle->applies[i].cfg_learners_ids);
  }
  K_FREE(bundle->applies); bundle->applies=0; bundle->apply_count=0;
  for(i=0;i<bundle->client_count;i++) K_FREE(bundle->clients[i].response);
  K_FREE(bundle->clients); bundle->clients=0; bundle->client_count=0;
  K_FREE(bundle->snapshot_reads); bundle->snapshot_reads=0; bundle->snapshot_read_count=0;
  return server->fatal?-1:0;
}
/* Deferred half.  raft_persist_complete mutates the raft context, which is
   main-thread-owned and lock-free, so it MUST run on the main thread (never in
   the WAL worker) and only after the worker has durably written the record.
   Called by k_server_poll_wal as each write result arrives, in append order. */
static int k_server_process_bundle_persist(k_server *server,k_ready_bundle *bundle){
  if(!server||!bundle) return -1;
  if(bundle->persist_needed){
    /* raft_persist_complete only accepts RUNNING/DRAINING.  Under the non-
       blocking WAL pipeline a persist (including the DRAINING phase's final
       flush) can still be in flight when the raft reaches STOPPING/STOPPED; the
       record is already durable on disk (recovery reads the latest WAL record),
       so the durable_index report is moot for a stopping node.  Track the stop
       with the app's own flag (set exactly where raft_stop is called) rather
       than reading raft internals: app state comes from raft_ready, not the
       library struct. */
    if(!server->stopping){
      if(raft_persist_complete(server->raft,bundle->durable_index)!=0){ printf("fatal: raft persist complete failed\n"); server->fatal=1; }
      if(bundle->snapshot_dirty){
        if(raft_snapshot_persist_complete(server->raft,bundle->durable_index)!=0){ printf("fatal: raft snapshot persist complete failed\n"); server->fatal=1; }
      }
    }
  }
  if(bundle->phase_stopped){
    server->raft_stopped=1;
    if(!server->snapshot_task) server->snapshot_inflight=0;
  }
  return server->fatal?-1:0;
}
static int k_server_poll_wal(k_server *server){
  runtime_fn fn;
  void *arg=0;
  int rc;
  k_ready_bundle *bundle;
  if(!server||!server->wal_rt) return -1;
  /* Drain every completed WAL write in FIFO order (the result queue is FIFO and
     the single worker appends in submission order), decrementing the in-flight
     count so backpressure releases promptly. */
  for(;;){
    rc=runtime_result_poll(server->wal_rt,0,&fn,&arg);
    if(rc<0) return server->stopping?0:-1;
    if(rc==0) break;
    bundle=(k_ready_bundle *)arg;
    if(!bundle) return -1;
    if(server->wal_inflight_count>0) server->wal_inflight_count--;
    server->wal_pending_items=(bundle->item_count<=server->wal_pending_items)?(server->wal_pending_items-(k_u64)bundle->item_count):0u;
    if(!bundle->wal_ok){
      /* A failed WAL write is not a clean shutdown: mark fatal so the process exits non-zero
         with a reason instead of reporting "stopped" to whoever supervises it. */
      printf("fatal: WAL write failed\n");
      server->fatal=1;
      k_ready_bundle_free(bundle);
      return -1;
    }
    if(bundle->live_deferred&&k_server_process_bundle_live(server,bundle)!=0){
      k_ready_bundle_free(bundle);
      return -1;
    }
        server->wal_meta=bundle->wal_result;
    if(bundle->wal_result.generation&&bundle->wal_result.generation!=server->persist_generation) server->wal_records++;
    server->persist_generation=bundle->generation;
    if(bundle->snapshot_dirty){
      if(server->snapshot.index>0){
        server->snapshot_older_index=server->snapshot_prev_index;
        server->snapshot_prev_index=server->snapshot.index;
        server->snapshot_prev_wal_segment=server->snapshot_wal_segment;
      }
      k_snapshot_cache_free(&server->snapshot);
      server->snapshot=bundle->snapshot_after;
      memset(&bundle->snapshot_after,0,sizeof(bundle->snapshot_after));
      server->snapshot_wal_segment=bundle->wal_result.next.segment;
      server->snapshot_wal_offset=bundle->wal_result.next.offset;
    }
    rc=k_server_process_bundle_persist(server,bundle);
    if(bundle->snapshot_dirty){
      server->snapshot_inflight=0;
      server->snapshot_failed=0;
      server->snapshot_failed_apply_baseline=0;
      server->snapshot_failed_segment_baseline=0;
      server->snapshot_cleanup_failed=0;
      /* Nothing is unlinked here: the previous snapshot and the segments from its base
         onward are the rollback target, and they are released only after the NEW file
         has been written and verified (see the snapshot result handler). */
    }
    k_ready_bundle_free(bundle);
    if(rc!=0) return -1;
  }
  k_server_maybe_finish_stop(server);
  return 0;
}
/* ================= Server: drive loop, snapshot, reconnect, tick ================= */
static int k_server_drive(k_server *server,unsigned int elapsed_ms){
  raft_ready ready;
  k_snapshot_chunk *old_provided;
  k_ready_bundle *bundle;
  int rc;
  if(!server||!server->raft) return -1;
  server->rounds++;
  if(k_server_poll_snapshot(server)!=0) return -1;
  if(k_server_poll_wal(server)!=0) return -1;
  if((k_u32)server->wal_inflight_count<server->wal_inflight_max&&server->request_count<K_REQUEST_INFLIGHT_MAX&&server->request_bytes<K_REQUEST_BYTES_MAX&&server->rx_buffer_bytes<K_RX_BYTES_MAX){
    k_conn *conn;
    for(conn=server->connections;conn;conn=conn->next){
      if(conn->kind==K_CONN_CLIENT&&conn->recv_paused){
        /* Re-arm the read; if the arm still fails, stay paused and retry on a later round.
           A paused connection must NEVER be closed here: pausing is backpressure, not a fault,
           and dropping the client loses requests it has already sent. */
        conn->recv_paused=0;
        if(!conn->sock||server->transport->recv(conn->sock)!=0) conn->recv_paused=1;
      }
    }
  }
  /* With no other voter, Raft may commit using self alone before the local log
     is durable.  Serialize that path so apply/client success cannot overtake
     the WAL record that makes the acknowledgement crash-safe. */
  if(!raft_has_voter_peer(server->raft)&&server->wal_inflight_count>0) return 0;
  /* Backpressure: cap the in-flight WAL writes so a slow disk can't grow the
     queue (and its deep-copied persist payloads) without bound.  In the common
     case the worker keeps up and this never triggers, so raft advances every
     tick regardless of fsync latency. */
  if((k_u32)server->wal_inflight_count>=server->wal_inflight_max){
    if(!server->wal_blocked_warned&&server->wal_blocked_elapsed>=3000u){
      fprintf(stderr,"warning: WAL submissions blocked for %ums (in_flight=%d/%d): the persistence worker is not completing\n",
              (unsigned)server->wal_blocked_elapsed,(int)server->wal_inflight_count,(int)K_WAL_INFLIGHT_MAX);
      server->wal_blocked_warned=1;
    }
    if(server->wal_blocked_elapsed<=0xffffffffu-elapsed_ms) server->wal_blocked_elapsed+=elapsed_ms;
    else server->wal_blocked_elapsed=0xffffffffu;
    server->wal_accumulated_events++;
    return 0;
  }
  if(server->wal_blocked_elapsed){
    if(elapsed_ms<=0xffffffffu-server->wal_blocked_elapsed) elapsed_ms+=server->wal_blocked_elapsed;
    else elapsed_ms=0xffffffffu;
    server->wal_blocked_elapsed=0;
  }
  old_provided=server->provided_chunks;
  server->provided_chunks=0;
  rc=raft_advance(server->raft,elapsed_ms,&ready);
  if(rc<0){
    k_snapshot_chunks_free(old_provided);
    return -1;
  }
  /* No work: skip the bundle entirely.  The loop runs once per drained network
     completion, so an unconditional alloc/free pair per round is pure allocator
     pressure (a Ready with nothing to do carries no state the caller needs). */
  if(!ready.has_work){
    raft_ready_consumed(server->raft);
    k_snapshot_chunks_free(old_provided);
    k_server_maybe_finish_stop(server);
    return 0;
  }
  /* ready.is_leader / ready.leader_id are event flags: only valid when
     ready.leader_change is set (Raft Sec. 5.2 leadership transfer event).
     Track cumulatively so redirects work even during WAL inflight. */
  if(ready.leader_change){
    int lost=server->is_leader&&!ready.is_leader;
    server->is_leader=ready.is_leader;
    server->leader_id=ready.leader_id;
    /* Dropping leadership abandons any in-flight FCALL gate, so queued writes are
       not stranded; the FCALL itself gets its REDIRECT via the normal result path.
       An in-flight catch-up is likewise abandoned: clear pending so the failed
       node stops being dialed. */
    if(lost){ k_server_clear_gate(server); server->pending_count=0; }
  }
  if(server->is_leader&&server->auto_replace) k_server_auto_replace(server,&ready);
  bundle=k_ready_bundle_copy(server,&ready);
  k_snapshot_chunks_free(old_provided);
  if(!bundle) return -1;
  /* Consume ONLY after the copy succeeded: raft_ready_consumed resets the counts, so
     consuming first and then failing (the copy can fail on allocation) silently discarded
     every result in the batch - client requests stayed unanswered forever while Raft went
     on reporting new barriers, and nothing was logged.  Leaving the ready intact lets the
     next drive re-copy it. */
  raft_ready_consumed(server->raft);
  /* Multi-voter commits already reside durably on a quorum, so their live work
     remains parallel with the leader's own fsync.  A sole voter has no such
     external durability and defers live work until k_server_poll_wal. */
  bundle->live_deferred=bundle->persist_needed&&!raft_has_voter_peer(server->raft);
  if(!bundle->live_deferred&&k_server_process_bundle_live(server,bundle)!=0){
    k_ready_bundle_free(bundle);
    return -1;
  }
  if(bundle->persist_needed){
    server->wal_inflight_count++;
    server->wal_pending_items+=(k_u64)bundle->item_count;
    server->wal_accumulated_events=0;
    server->wal_task_post_us=k_wal_sync_now();   /* diagnostic stamp: paired with the worker's start */
    if(runtime_task_post(server->wal_rt,0,bundle)!=0){
      server->wal_inflight_count--;
      server->wal_pending_items=(bundle->item_count<=server->wal_pending_items)?(server->wal_pending_items-(k_u64)bundle->item_count):0u;
      k_ready_bundle_free(bundle);
      return -1;
    }
    return 0;
  }
  /* No persist: no WAL round-trip to wait for, so complete the deferred half
     inline. */
  rc=k_server_process_bundle_persist(server,bundle);
  k_ready_bundle_free(bundle);
  k_server_maybe_finish_stop(server);
  return rc;
}
static k_u64 k_server_wal_bytes_since_snapshot(const k_server *server){
  k_u64 segment_gap,bytes;
  if(!server||server->wal_meta.next.segment<server->snapshot_wal_segment) return 0;
  segment_gap=server->wal_meta.next.segment-server->snapshot_wal_segment;
  if(!segment_gap){
    return server->wal_meta.next.offset>=server->snapshot_wal_offset?server->wal_meta.next.offset-server->snapshot_wal_offset:0;
  }
  bytes=server->cfg.wal_seg_size>server->snapshot_wal_offset?server->cfg.wal_seg_size-server->snapshot_wal_offset:0;
  if(segment_gap>1u){
    k_u64 middle=segment_gap-1u;
    if(middle>(~(k_u64)0-bytes)/server->cfg.wal_seg_size) return ~(k_u64)0;
    bytes+=middle*server->cfg.wal_seg_size;
  }
  if(server->wal_meta.next.offset>~(k_u64)0-bytes) return ~(k_u64)0;
  return bytes+server->wal_meta.next.offset;
}
static int k_server_maybe_snapshot(k_server *server){
  k_snapshot_task *task;
  raft_i64 entry_baseline;
  k_u64 segment_baseline,current_segment,segment_gap,wal_bytes;
  int entry_ready,segment_ready,size_ready;
  if(!server||server->stopping||server->snapshot_inflight||server->snapshot_task||server->snapshot_cleanup_busy) return 0;
  entry_baseline=server->snapshot_failed?server->snapshot_failed_apply_baseline:server->snapshot.index;
  segment_baseline=server->snapshot_failed?server->snapshot_failed_segment_baseline:server->snapshot_wal_segment;
  current_segment=server->wal_meta.generation?server->wal_meta.record.segment:0;
  entry_ready=(server->snapshot_failed||server->snapshot.index<=0)&&server->last_applied>=entry_baseline&&server->last_applied-entry_baseline>=(raft_i64)server->cfg.snapshot_entries;
  segment_gap=current_segment>=segment_baseline?current_segment-segment_baseline:0;
  segment_ready=segment_gap>=(k_u64)server->cfg.snapshot_segments;
  wal_bytes=k_server_wal_bytes_since_snapshot(server);
  size_ready=!server->snapshot_failed&&server->snapshot.index>0&&server->snapshot.size>0&&wal_bytes/(k_u64)server->snapshot.size>=K_SNAPSHOT_WAL_RATIO;
  if(!entry_ready&&!segment_ready&&!size_ready) return 0;
  if(server->snapshot_retry){
    task=server->snapshot_retry;
    server->snapshot_retry=0;
  }else{
    task=(k_snapshot_task *)K_CALLOC(1,sizeof(*task));
    if(!task){
      k_server_set_snapshot_failed_baseline(server);
      return 0;
    }
    task->server=server;
    task->kind=K_SNAPSHOT_TASK_SAVE;
    task->index=server->last_applied;
    task->cluster_copy=server->cluster;
    if(raft_snapshot(server->raft)!=0){
      k_snapshot_task_free(task);
      k_server_set_snapshot_failed_baseline(server);
      return 0;
    }
    treap_capture(server->tree);
  }
  if(runtime_task_post(server->snapshot_rt,0,task)!=0){
    /* No view release here: a retry task was already serialized (never reaches a captured view),
       and for a fresh task the capture is simply superseded - the next treap_capture() overwrites
       t->snap.root and folds this view's retired chain into the new one, whose treap_save_finish()
       reclaims it.  A view left over on this path therefore only delays reclamation. */
    k_server_set_snapshot_failed_baseline(server);
    if(task->serialized) server->snapshot_retry=task;
    else k_snapshot_task_free(task);
    return 0;
  }
  server->snapshot_task=task;
  server->snapshot_inflight=1;
  server->last_snapshot_task_index=task->index;
  return 0;
}
/* Silent-standby gate (Sec 4.4: a removed server leaves service; re-adding
   re-enters it).  Pure predicate so the bootstrap-vs-removed distinction is
   unit-testable.  A removed follower must stop dialing former peers (the dial
   churn the members' A2 GC already prunes) but keep listening + raft state. */
static int k_server_silent_standby(const k_server *server){
  if(!server) return 0;
  if(server->is_leader) return 0;
  if(server->voter_count<=0) return 0;
  /* applied view: membership updates only once a CONFIG entry COMMITS */
  if(!k_membership_contains(server,server->id)) return 1;
  /* config-immediate view: a removed node never receives the final C_new (the
     leader drops it from the peer list the moment it finalizes the joint), so
     its applied voters still contain self.  The config_joint gate is REQUIRED:
     a node being ADDED (a bootstrap replays the leader's pre-ADD log while
     catching up) has config_joint==0 with config_new excluding self, and must
     NOT be read as removed - or it drops its up-dial and strands the catch-up
     if the leader's down-dial ever fails.  Learners live in config_learners
     (not config_new), so the learner gate is preserved. */
  /* These three facts arrive through raft_ready (issue #14): the application must not read raft_ctx's
     config_* fields for a policy decision, and the checker's ratchet for that is now at zero. */
  if(server->cfg_joint&&!server->self_is_voter&&!server->self_is_learner) return 1;
  return 0;
}
static void k_server_reconnect(k_server *server){
  int i;
  if(!server||!server->admission) return;
  if(k_server_silent_standby(server)) return;
  for(i=0;i<server->cluster.count;i++){
    k_conn *conn;
    void *sock;
    const k_node_spec *node=&server->cluster.nodes[i];
    if(!k_membership_contains(server,node->id)){
      /* Not in the member set. A node still bootstrapping (self-only config,
         voter_count<=1) keeps its contacts: the leader reaching out to catch it
         up is not a member yet but must not be dropped. Only a node with an
         established membership drops/prunes non-members (Sec 4.4: the address
         book is a cache of member addresses, not a static pre-list). */
      if(server->voter_count>1){
        if(server->peer_conns[i]){
          {
            void *dead=server->peer_conns[i]->sock;
            server->peer_conns[i]->sock=0;
            server->peer_socks[i]=0;
            server->transport->close(dead);
          }
          server->peer_conns[i]=0;
          server->peer_socks[i]=0;
        }
        if(node->id!=server->id) server->cluster.nodes[i].id=0;
      }
      continue;
    }
    /* dial up (id-order) OR dial a pending catch-up target regardless of id, so
       a lower-id new node is still contacted by the leader (Sec 4.4: the leader
       contacts the added server; a static "lower dials higher" rule would orphan
       a lower-id new node).  Skip only when the HELLO handshake has COMPLETED
       (peer_socks set): a half-open link (dialed, but the peer's HELLO never
       arrived - peer_conns set, peer_socks still 0) must be re-dialed, or the
       peer is stranded forever (k_server_send_ready_message drops its messages). */
    if((node->id<=server->id&&!k_membership_has(server->pending,server->pending_count,node->id))||server->peer_socks[i]) continue;
    if(server->peer_conns[i]){
      {
        void *dead=server->peer_conns[i]->sock;
        server->peer_conns[i]->sock=0;
        server->peer_socks[i]=0;
        server->transport->close(dead);
      }
      server->peer_conns[i]=0;
      server->peer_socks[i]=0;
    }
    sock=server->transport->dial(server,node);
    if(!sock) continue;
    conn=k_conn_create(server,sock,K_CONN_PEER,1,i);
    if(!conn){
      server->transport->close(sock);
      continue;
    }
    server->peer_conns[i]=conn;
    server->peer_socks[i]=0;
  }
}
static void k_server_advance_at(k_server *server,unsigned int elapsed_ms,unsigned int batch_ms);
/* Deterministic/single-clock entry point: the injected step counts for both the wall
   clock and the batch age (used by the test harness and by drivers that do not split
   the two). */
static void k_server_advance(k_server *server,unsigned int elapsed_ms){
  k_server_advance_at(server,elapsed_ms,elapsed_ms);
}
/* Pure advance: one deterministic time step, with TWO time quantities because they
   answer different questions:
     - elapsed_ms is WALL time since the previous advance: Raft timers, reconnect and
       the snapshot policy measure real elapsed time, and that time really passed;
     - batch_ms is how much of it belongs to the queued write batch: a batch whose
       writes arrived INSIDE the poll that just returned gets 0, because the wait
       before their arrival is not part of their age.  Charging the full elapsed to a
       fresh batch expired its window instantly (every batch = the writes of one poll,
       i.e. no group commit at all).
   A harness that drives the server with injected time calls k_server_advance, which
   passes the same value for both (the deterministic meaning of an injected step). */
static void k_server_advance_at(k_server *server,unsigned int elapsed_ms,unsigned int batch_ms){
  unsigned int write_ms;
  if(!server) return;
  if(server->elapsed_total_ms<=~(k_u64)0-(k_u64)elapsed_ms) server->elapsed_total_ms+=(k_u64)elapsed_ms;
  else server->elapsed_total_ms=~(k_u64)0;
  if(server->runtime&&runtime_should_stop(server->runtime)) k_server_begin_stop(server);
  write_ms=batch_ms;
  if(server->write_count){
    if(server->write_elapsed<=0xffffffffu-write_ms) server->write_elapsed+=write_ms;
    else server->write_elapsed=0xffffffffu;
    if(server->write_elapsed>=server->cfg.flush_timeout_ms&&k_server_flush_reason(server,K_FLUSH_WINDOW)!=0){
      printf("fatal: flush writes failed (tick)\n");
      server->fatal=1;
      k_server_begin_stop(server);
    }
  }
  if(k_server_drive(server,elapsed_ms)!=0){
    /* Fail-stop paths inside drive print their own reason when they have one; the stop itself must never
       be silent, or the process exits with "fatal=0" and the failure class is lost. */
    fprintf(stderr,"fatal: server drive failed; stopping (admission closed, clients see \"server stopping\")\n");
    server->fatal=1;
    k_server_begin_stop(server);
  }
  if(!server->stopping&&k_server_maybe_snapshot(server)!=0){
    printf("fatal: snapshot failed\n");
    server->fatal=1;
    k_server_begin_stop(server);
  }
  server->reconnect_elapsed+=elapsed_ms;
  if(server->reconnect_elapsed>=K_RECONNECT_MS){
    server->reconnect_elapsed=0;
    k_server_reconnect(server);
  }
}
/* ================= Server: runtime drain, release, open, run, init ================= */
static void k_wal_runtime_drain(runtime_ctx *runtime){
  runtime_fn fn;
  void *arg;
  if(!runtime) return;
  while(runtime_task_poll(runtime,0,&fn,&arg)>0) k_ready_bundle_free((k_ready_bundle *)arg);
  while(runtime_result_poll(runtime,0,&fn,&arg)>0) k_ready_bundle_free((k_ready_bundle *)arg);
}
static void k_snapshot_runtime_drain(runtime_ctx *runtime){
  runtime_fn fn;
  void *arg;
  if(!runtime) return;
  while(runtime_task_poll(runtime,0,&fn,&arg)>0) k_snapshot_task_free((k_snapshot_task *)arg);
  while(runtime_result_poll(runtime,0,&fn,&arg)>0) k_snapshot_task_free((k_snapshot_task *)arg);
}
static void k_server_release(k_server *server){
  k_conn *conn,*next_conn;
  k_request *request,*next_request;
  if(!server) return;
  server->releasing=1;   /* requested shutdown: freeing still-submitted requests here is expected */
  if(server->wal_rt){
    runtime_stop(server->wal_rt);
    runtime_wait_workers_exit(server->wal_rt);
    /* The thread backend closes the held WAL handles from the worker entry on stop;
       the deterministic "sync" backend never re-enters the entry, so release owns the
       close as well.  k_wal_files_close is idempotent, so this is safe for both. */
    k_wal_files_close(&server->wal_worker);
    k_wal_runtime_drain(server->wal_rt);
    runtime_destroy(server->wal_rt);
    server->wal_rt=0;
    server->wal_inflight_count=0;
  }
  if(server->snapshot_rt){
    runtime_stop(server->snapshot_rt);
    runtime_wait_workers_exit(server->snapshot_rt);
    k_snapshot_runtime_drain(server->snapshot_rt);
    runtime_destroy(server->snapshot_rt);
    server->snapshot_rt=0;
    server->snapshot_task=0;
  }
  k_snapshot_task_free(server->snapshot_retry);
  server->snapshot_retry=0;
  /* Requests FIRST: k_request_free -> k_request_unlink dereferences request->conn (and writes through
     it), so freeing the connections before the requests that point at them is a use-after-free on every
     shutdown.  Freeing the requests also drains the FCALL write gate, which walks its own list. */
  for(request=server->requests;request;request=next_request){
    next_request=request->next;
    k_request_free(server,request);
  }
  server->requests=0;
  memset(server->request_hash,0,sizeof(server->request_hash));
  server->request_count=0;
  server->request_bytes=0;
  for(conn=server->connections;conn;conn=next_conn){
    next_conn=conn->next;
    k_rx_free(&conn->rx);
    K_FREE(conn);
  }
  server->connections=0;
  server->client_connection_count=0;
  server->rx_buffer_bytes=0;
  /* The peer_socks/peer_conns index arrays dangling-reference the just-freed
     connections; clear them too, or a reopen sees stale non-NULL socks and
     reconnect's `peer_socks[i]` guard skips re-dialing -- stranding the node. */
  memset(server->peer_socks,0,sizeof(server->peer_socks));
  memset(server->peer_conns,0,sizeof(server->peer_conns));
  server->write_head=0;
  server->write_tail=0;
  server->write_count=0;
  /* The FCALL gate is server state too: a dangling gate_head means a later k_request_free of an FCALL
     walks a chain whose members were already freed - a double free if the shutdown lands inside the
     gate's fork->commit window. */
  server->gate_head=0;
  server->gate_tail=0;
  server->gate_closed=0;
  k_snapshot_chunks_free(server->provided_chunks);
  server->provided_chunks=0;
  if(server->raft){
    raft_destroy(server->raft);
    server->raft=0;
  }
  if(server->tree){
    treap_free(server->tree);
    server->tree=0;
  }
  k_snapshot_cache_free(&server->snapshot);
}
/* Every step of the open/restore sequence used to collapse into the caller's single "failed to start
   server N", so a refused start named nothing (issue #7).  Each step now says which one refused and why,
   and the store-backed failures include the base path, because that is what the operator has to look at. */
static void k_open_fail(const char *step,const char *why,const char *base){
  printf("fatal: cannot start server: %s failed (%s)%s%s\n",step,why,base?" at ":"",base?base:"");
}
static int k_server_open(k_server *server){
  k_restore restore;
  raft_config config;
  const char *runtime_be;
  int peer_ids[K_MAX_NODES];
  int restore_rc,i,local_index,wal_exists;
  if(!server||server->id<=0||server->cluster.count<=0){ k_open_fail("argument check","bad server, id or empty cluster",0); return -1; }
  local_index=k_cluster_index(&server->cluster,server->id);
  if(local_index<0){ k_open_fail("cluster lookup","this node id is not in the configured cluster",0); return -1; }
  if(k_cfg_open(server->base,&server->cfg)!=0){ k_open_fail("config open","unreadable or invalid config",server->base); return -1; }
  /* The adaptive flush window (kdbsvr's serve loop) may shrink cfg.flush_timeout_ms
     toward the measured sync cost; remember the configured ceiling so it can never
     grow past what the operator asked for. */
  server->flush_window_max_ms=server->cfg.flush_timeout_ms;
  server->wal_inflight_max=(k_u32)K_WAL_INFLIGHT_MAX;   /* the cap; the driver narrows it with a budget */
  restore_rc=k_state_load(server->base,&restore,&server->wal_meta,&wal_exists);
  if(restore_rc<0){ k_open_fail("state restore","unreadable store or corrupt state",server->base); return -1; }
  if(!wal_exists){
    if(k_wal_meta_init(server->base)!=0||k_wal_meta_load(server->base,&server->wal_meta)!=1){
      k_open_fail("WAL metadata","cannot initialise or reload the WAL metadata",server->base);
      return -1;
    }
  }
  server->tree=treap_create((treap_u64)server->cfg.seed);
  if(!server->tree){
    if(restore_rc>0) k_restore_free(&restore);
    k_open_fail("tree allocation","out of memory creating the tree",0);
    return -1;
  }
  /* A fresh tree restores to empty, so reset the applied watermark: a reopen
     (release leaves last_applied set) must not think the replayed log is
     already applied -- otherwise raft re-applies from 0 while last_applied
     points at the old watermark and the state machine silently diverges.  A
     restored snapshot then advances the watermark to its own index. */
  server->last_applied=0;
  k_cmd_register("transfer",k_cmd_transfer);
  if(restore_rc>0&&restore.persist.last_included_index>0){
    if(k_snapshot_load_file(server,restore.persist.last_included_index,server->tree)!=0){
      printf("fatal: snapshot load failed at base %" K_I64_FMT " (refusing to start)\n",
             (k_i64)restore.persist.last_included_index);
      k_restore_free(&restore);
      return -1;
    }
    server->last_applied=restore.persist.last_included_index;
  }
  if(server->bootstrap){
    /* bootstrap: empty config (non-voting new node); the address book still
       pre-lists a bootstrap source so HELLO/peer links can form, but the node
       has no quorum of its own until MEMBER ADD promotes it (Sec 4.4) */
  }else{
    for(i=0;i<server->cluster.count;i++) peer_ids[i]=server->cluster.nodes[i].id;
  }
  memset(&config,0,sizeof(config));
  config.id=server->id;
  config.peers=peer_ids;
  config.peer_count=server->bootstrap?0:server->cluster.count;
  config.bootstrap=server->bootstrap;
  config.heartbeat_ms=K_HEARTBEAT_MS;
  config.election_min_ms=K_ELECTION_MIN_MS;
  config.election_max_ms=K_ELECTION_MAX_MS;
  config.seed=(unsigned int)(server->cfg.seed^(k_u64)((unsigned int)server->id*2654435761u));
  config.snapshot_chunk_size=K_SNAPSHOT_CHUNK;
  config.log_chunk_size=64;
  config.restore=restore_rc>0?&restore.persist:0;
  server->raft=raft_create(&config);
  if(!server->raft){
    if(restore_rc>0) k_restore_free(&restore);
    return -1;
  }
  /* membership snapshot: full address book by default (the peer topology must
     keep dialing pre-listed nodes even when this node's raft config is EMPTY as
     a bootstrap new node, Sec 4.4); a restored snapshot config overrides it. */
  server->voter_count=server->cluster.count;
  for(i=0;i<server->cluster.count;i++) server->voters[i]=server->cluster.nodes[i].id;
  server->learner_count=0;
  if(restore_rc>0&&restore.persist.snapshot_cfg_new.id_count>0){
    server->voter_count=restore.persist.snapshot_cfg_new.id_count;
    for(i=0;i<server->voter_count;i++) server->voters[i]=restore.persist.snapshot_cfg_new.ids[i];
    server->learner_count=restore.persist.snapshot_cfg_learners.id_count;
    for(i=0;i<server->learner_count;i++) server->learners[i]=restore.persist.snapshot_cfg_learners.ids[i];
  }
  if(restore_rc>0){
    server->persist_generation=restore.generation;
    if(k_snapshot_cache_set(&server->snapshot,restore.persist.last_included_index,restore.persist.last_included_term,restore.persist.snapshot_size,restore.persist.snapshot_cfg_old,restore.persist.snapshot_cfg_new,restore.persist.snapshot_cfg_learners)!=0){
      k_restore_free(&restore);
      return -1;
    }
    k_restore_free(&restore);
  }
  if(server->snapshot.index>0&&server->wal_meta.generation){
    server->snapshot_wal_segment=server->wal_meta.next.segment;
    server->snapshot_wal_offset=server->wal_meta.next.offset;
  }
  server->wal_worker.server=server;
  server->wal_worker.seg_file=0;
  server->wal_worker.seg_file_segment=0;
  server->wal_worker.meta_file=0;
  server->wal_worker.open_files=0;
  server->wal_worker.meta_slot=server->wal_meta.slot;
  server->wal_worker.generation=server->persist_generation;
  server->wal_worker.next=server->wal_meta.next;
  server->wal_worker.meta_since_sync=(int)K_WAL_META_FSYNC_EVERY;
  server->wal_worker.meta_sync_segment=K_U64_C(0xffffffffffffffff);
  runtime_be=server->runtime_backend?server->runtime_backend:"thread";
  server->wal_rt=runtime_create(runtime_be,1,k_wal_worker_entry,&server->wal_worker);
  if(!server->wal_rt){ k_open_fail("WAL worker thread","cannot create the runtime worker",0); return -1; }
  server->snapshot_rt=runtime_create(runtime_be,1,k_snapshot_worker_entry,server);
  if(!server->snapshot_rt){ k_open_fail("snapshot worker thread","cannot create the runtime worker",0); return -1; }
  runtime_wait_workers_ready(server->wal_rt);
  runtime_wait_workers_ready(server->snapshot_rt);
  return 0;
}
static void k_server_init(k_server *server,int id,unsigned short client_port,unsigned short peer_port,const char *base,const k_cluster *cluster){
  memset(server,0,sizeof(*server));
  server->ud_kind=K_UD_SERVER;
  server->id=id;
  server->client_port=client_port;
  server->peer_port=peer_port;
  server->cluster=*cluster;
  memcpy(server->base,base,strlen(base)+1u);
}

#endif
