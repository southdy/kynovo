#ifndef CEMON_H
#define CEMON_H
#ifdef __cplusplus
extern "C" {
#endif
#ifndef CEMON_DEF
#ifdef CEMON_STATIC
#define CEMON_DEF static
#else
#define CEMON_DEF extern
#endif
#endif
#if defined(_MSC_VER)
typedef unsigned __int64 cemon_u64;
#else
typedef unsigned long long cemon_u64;
#endif
/* cemon is a single-owner lifecycle-convergent event runtime kernel.
  It owns I/O object lifetime, callback execution rights, owner-thread
  continuation scheduling, stop/drain admission, and convergence to stable
  terminal states.
  The stable public contract is semantic runtime state: loop phase, socket
  phase, close progress, delivery mode, capability mask, and observability.
  Backend readiness and transport machinery such as IOCP/epoll/kqueue state,
  recv arming, retry queues, and shutdown plumbing remain internal details.
  Non-goals: protocol stacks, RPC, TLS, storage, async DNS runtime, actor
  mailboxes, futures/promises, coroutine scheduling, or other higher-level
  application frameworks. */
/* Stable public semantic surface. These enums describe runtime-observable
  ownership and lifecycle state, not backend readiness bits.
  Naming strata in this header:
  - stable semantic projection:
      socket_role / socket_phase / socket_close_progress / socket_delivery /
      socket_capability_mask / loop_phase / loop_mailbox / loop_ownership
  - exported diagnostics:
      socket_kind / socket_state / socket_tcp_phase via cemon_inspect
  - private implementation aliases:
      CEMON_*_SOCK / CEMON_*_STATE / CEMON_TCP_PHASE_*
  Shared integer values across these strata are an implementation detail. The
  names are intentionally not interchangeable semantic contracts. */
enum{
  CEMON_ACCEPT=1,
  CEMON_CONNECT=2,
  CEMON_DATA=3,
  CEMON_SENT=4,
  CEMON_CLOSED=5,
  CEMON_EOF=6
};
enum{
  CEMON_SOCKET_ROLE_LISTENER=1,
  CEMON_SOCKET_ROLE_DATAGRAM=2,
  CEMON_SOCKET_ROLE_STREAM=3
};
/* socket_phase is the coarse stable lifecycle. It intentionally omits raw NEW
  and collapses transport-specific shutdown substates into TERMINATING. CLOSED
  is reached only after terminal transport death; use socket_close_progress to
  refine terminating TCP shutdown semantics. */
enum{
  CEMON_SOCKET_PHASE_CONNECTING=1,
  CEMON_SOCKET_PHASE_OPEN=2,
  CEMON_SOCKET_PHASE_TERMINATING=3,
  CEMON_SOCKET_PHASE_CLOSED=4
};
/* socket_close_progress refines TCP termination. For non-TCP sockets and for
  CONNECTING/CLOSED states, the stable answer is NONE. */
enum{
  CEMON_SOCKET_CLOSE_PROGRESS_NONE=0,
  CEMON_SOCKET_CLOSE_PROGRESS_PEER_EOF=1,
  CEMON_SOCKET_CLOSE_PROGRESS_LOCAL_SHUTDOWN=2,
  CEMON_SOCKET_CLOSE_PROGRESS_BOTH=3
};
/* Delivery mode is semantic callback policy. STOP_SUPPRESSED means stop/drain
  gating suppressed new callback delivery; it is not itself a loop phase. */
enum{
  CEMON_SOCKET_DELIVERY_NORMAL=0,
  CEMON_SOCKET_DELIVERY_STOP_SUPPRESSED=1
};
/* Capability values are combinable mask bits. */
enum{
  CEMON_SOCKET_CAP_RECV=1,
  CEMON_SOCKET_CAP_SEND=2,
  CEMON_SOCKET_CAP_SHUTDOWN=4
};
/* loop_phase is the stable lifecycle projection over internal run/stop
  control flags. */
enum{
  CEMON_LOOP_PHASE_READY=1,
  CEMON_LOOP_PHASE_RUNNING=2,
  CEMON_LOOP_PHASE_STOP_REQUESTED=3,
  CEMON_LOOP_PHASE_DRAINING=4,
  CEMON_LOOP_PHASE_STOPPED=5,
  CEMON_LOOP_PHASE_CLOSING=6
};
/* loop_mailbox is a derived post-admission gate, not the underlying queued
  post storage. Queue occupancy lives in post_head/post_tail/post_count.
  OPEN accepts new post/ingress work, STOP_GATED permits only owner-callback
  continuation posting, and CLOSED rejects new admissions. */
enum{
  CEMON_LOOP_MAILBOX_OPEN=0,
  CEMON_LOOP_MAILBOX_STOP_GATED=1,
  CEMON_LOOP_MAILBOX_CLOSED=2
};
enum{
  CEMON_LOOP_OWNERSHIP_UNBOUND=0,
  CEMON_LOOP_OWNERSHIP_BOUND=1,
  CEMON_LOOP_OWNERSHIP_CLOSING=2
};
/* Extended socket diagnostic axes exposed through cemon_stats. Application
  logic should prefer socket_role/socket_phase/socket_close_progress over
  kind/state raw transport axes. socket_state is a raw transport lifetime
  state, not a synonym for the stable socket_phase projection. */
enum{
  CEMON_SOCKET_KIND_TCP=1,
  CEMON_SOCKET_KIND_UDP=2,
  CEMON_SOCKET_KIND_LISTEN=3
};
enum{
  CEMON_SOCKET_STATE_NEW=0,
  CEMON_SOCKET_STATE_CONNECTING=1,
  CEMON_SOCKET_STATE_OPEN=2,
  CEMON_SOCKET_STATE_DEAD=3
};
/* Extended TCP progress observation for inspect/debug. PUBLIC here means the
  numeric domain is exported through cemon_stats.socket_tcp_phase. It does not
  make these values part of the stable semantic decision surface. Prefer
  socket_phase and socket_close_progress when making stable semantic decisions. */
enum{
  CEMON_SOCKET_TCP_PHASE_PUBLIC_OPEN=0,
  CEMON_SOCKET_TCP_PHASE_PUBLIC_READ_EOF=1,
  CEMON_SOCKET_TCP_PHASE_PUBLIC_WRITE_DRAIN=2,
  CEMON_SOCKET_TCP_PHASE_PUBLIC_WRITE_CLOSED=3,
  CEMON_SOCKET_TCP_PHASE_PUBLIC_READ_EOF_WRITE_DRAIN=4,
  CEMON_SOCKET_TCP_PHASE_PUBLIC_HALF_CLOSED=5
};
#ifndef CEMON_ADDR_STORAGE
#define CEMON_ADDR_STORAGE 128
#endif
typedef struct cemon_addr{
  int len;
  char data[CEMON_ADDR_STORAGE];
} cemon_addr;
typedef struct cemon_event{
  int type;
  int status;
  int size;
  void *data;
  cemon_addr addr;
} cemon_event;
/* cemon_stats intentionally stays as one easy-to-use aggregate surface for
  cemon_inspect. Read it in two bands:
  - semantic runtime state: stable contract for application logic
  - diagnostics: counters and raw transport/runtime observations */
typedef struct cemon_stats{
  /* Stable loop semantic state. loop_mailbox is admission openness, not post
    queue depth. */
  int loop_phase;
  int loop_mailbox;
  int loop_ownership;
  /* Loop resource topology. */
  unsigned int loop_sock_total;
  unsigned int loop_timer_count;
  /* Extended loop diagnostics. */
  unsigned int loop_send_cost;
  unsigned int loop_send_cost_peak;
  unsigned int loop_post_count;
  unsigned int loop_post_count_peak;
  cemon_u64 loop_blocked_accept_count;
  cemon_u64 loop_blocked_connect_count;
  cemon_u64 loop_blocked_data_count;
  cemon_u64 loop_blocked_sent_count;
  cemon_u64 loop_terminal_eof_count;
  cemon_u64 loop_terminal_close_count;
  cemon_u64 loop_stop_begin_count;
  cemon_u64 loop_stop_socket_close_count;
  cemon_u64 loop_accept_retry_queue_count;
  cemon_u64 loop_accept_retry_run_count;
  /* Stable socket semantic state. socket_phase is coarse lifecycle;
    socket_close_progress refines terminating TCP state. */
  int socket_role;
  int socket_phase;
  int socket_close_progress;
  int socket_delivery;
  unsigned int socket_capability_mask;
  /* Extended socket diagnostics. These are raw transport axes exposed for
    observability; prefer the stable semantic fields when branching in
    application logic. */
  int socket_kind;
  int socket_state;
  int socket_tcp_phase;
  int socket_recv_armed;
  int socket_shutdown_pending;
  int socket_write_closed;
  int socket_write_shutdown_needed;
  int socket_eof_seen;
  int socket_close_ready;
  unsigned int socket_send_queue_count;
  unsigned int socket_send_cost;
} cemon_stats;
typedef struct cemon cemon;
typedef struct cemon_socket cemon_socket;
typedef struct cemon_timer cemon_timer;
typedef void (*cemon_io_fn)(cemon_socket *sock,const cemon_event *ev);
typedef void (*cemon_task_fn)(cemon *loop,void *arg);
/* Core runtime invariants:
  - every user callback runs on the owner thread
  - stop seals new ACCEPT/CONNECT/DATA/SENT ingress
  - EOF/CLOSE are terminal convergence events
  - handle validity follows runtime lifecycle, not backend completion timing
  - semantic inspect fields are stable projections; raw fields are diagnostics */
/* CEMON_DATA payload points at an internal receive buffer and is valid only for
  the current callback. UDP CEMON_DATA always represents one complete datagram. */
/* CEMON_SENT is always delivered asynchronously on the owner thread after
  cemon_send/cemon_sendto returns; it is not fired synchronously in the call
  stack of those APIs. */
/* After cemon_stop is requested, the runtime does not deliver new DATA, SENT,
  CONNECT, or ACCEPT events; late completions collapse into CLOSED or are
  drained internally. */
/* CEMON_EOF and CEMON_CLOSED are terminal convergence events and are not part
  of the stop-window suppression set above. */
/* CEMON_EOF means the peer has closed its write side; it is delivered on an
  armed receive, and the socket remains valid until you close it or call
  cemon_shutdown to finish a graceful write-side drain. */
/* cemon_socket* and cemon_timer* are valid only while their owning loop lives.
  Outside callbacks, after cemon_destroy returns all handles from that loop are
  invalid and must not be reused. If cemon_destroy is called from a socket, timer,
  or post callback, it only requests cemon_stop; final destruction still happens
  when the owner thread later calls cemon_destroy after cemon_poll returns non-zero. After
  cemon_close or after a CEMON_CLOSED callback for a socket returns, that socket
  handle is invalid and must not be reused. After cemon_timer_stop or after a
  one-shot timer callback returns, that timer handle is invalid too. */
/* Userdata is a borrowed pointer. The runtime stores and forwards it but never
  retains, frees, or version-tracks it for you. cemon_getud/cemon_setud are
  owner-thread APIs; socket and timer callbacks already run on that thread. */
/* cemon_tcp_listen/cemon_tcp_connect/cemon_udp_bind accept numeric addresses
  only; use cemon_resolve explicitly if you need blocking name resolution
  outside the hot path. */
CEMON_DEF cemon *cemon_create(void);
/* Explicit ownership.  Only the owner thread may drive (cemon_poll), inspect and tear
  down the loop, and only the owner thread may send, arm reads or close a socket: those
  paths mutate per-socket queues and counters without a lock, so a second thread would race
  with the completion handlers - silently losing queued frames and leaking send quota until
  every send fails.  Cross-thread work must go through cemon_post (the mailbox is the one
  thread-safe entry point); a non-owner send or read-arm is refused with -1 rather than raced.
  The owner is otherwise bound implicitly by the FIRST admission, so a
  helper thread that touches the loop before the poller captures ownership and
  locks the real poller out (cemon_poll then returns -1 forever).  Call this right
  after cemon_create from the thread that will poll.  Returns 0 when the caller
  owns the loop, -1 when another thread already owns it (or loop is null). */
CEMON_DEF int cemon_bind_owner(cemon *loop);
/* 1 when the caller is the owner (or the loop is not bound yet), 0 when another
  thread owns it, -1 on a null loop. */
CEMON_DEF int cemon_is_owner(cemon *loop);
/* Returns 0 when the teardown was accepted, -1 when it was refused because the
  caller is not the owner (a violation used to be a silent no-op on a void API).
  Called from inside a callback it returns 0 and completes on a later turn. */
CEMON_DEF int cemon_destroy(cemon *loop);
/* cemon_poll executes one iteration of the event loop: fires due timers,
  drains posts and deferred work, checks stop convergence, and polls for I/O.
  It auto-enters the RUNNING lifecycle on first call and auto-exits (with a
  final drain) when stop converges or a fatal backend error occurs.
  timeout_ms: <0 = use computed timeout (based on next timer deadline),
  0 = non-blocking, >0 = block at most this many milliseconds.
  Returns 0 to continue, non-zero to stop.  The caller should call
  cemon_destroy after a non-zero return. */
CEMON_DEF int cemon_poll(cemon *loop,int timeout_ms);
/* cemon_stop is a fast stop request: it stops admitting new work and converges
  by closing live sockets; it does not promise to flush queued network sends,
  and it suppresses late DATA/SENT/CONNECT/ACCEPT delivery. */
CEMON_DEF void cemon_stop(cemon *loop);
/* cemon_should_stop becomes nonzero after cemon_stop is requested and also
  while cemon_destroy is shutting the loop down. */
CEMON_DEF int cemon_should_stop(cemon *loop);
CEMON_DEF cemon_socket *cemon_tcp_listen(cemon *loop,const char *host,unsigned short port,cemon_io_fn fn,void *ud);
CEMON_DEF cemon_socket *cemon_tcp_connect(cemon *loop,const char *host,unsigned short port,cemon_io_fn fn,void *ud);
CEMON_DEF cemon_socket *cemon_udp_bind(cemon *loop,const char *host,unsigned short port,cemon_io_fn fn,void *ud);
CEMON_DEF int cemon_send(cemon_socket *sock,const void *buf,int len);
CEMON_DEF int cemon_sendto(cemon_socket *sock,const void *buf,int len,const cemon_addr *to);
/* Control sends use the reserved loop-wide control budget instead of the bulk
  send budget. Use them for heartbeats, votes, acks, and other latency-sensitive
  traffic. */
CEMON_DEF int cemon_send_control(cemon_socket *sock,const void *buf,int len);
CEMON_DEF int cemon_sendto_control(cemon_socket *sock,const void *buf,int len,const cemon_addr *to);
/* cemon_recv/cemon_recvfrom arm one receive completion; call again after each
  handled message when you want to keep receiving. */
CEMON_DEF int cemon_recv(cemon_socket *sock);
CEMON_DEF int cemon_recvfrom(cemon_socket *sock);
CEMON_DEF void cemon_close(cemon_socket *sock);
/* cemon_shutdown is a TCP write-half close. After it succeeds, new sends are
  rejected, queued sends drain, and the runtime issues a real write shutdown. */
CEMON_DEF int cemon_shutdown(cemon_socket *sock);
CEMON_DEF void *cemon_getud(cemon_socket *sock);
CEMON_DEF void cemon_setud(cemon_socket *sock,void *ud);
CEMON_DEF cemon_timer *cemon_after(cemon *loop,unsigned int after_ms,unsigned int period_ms,cemon_task_fn fn,void *ud);
CEMON_DEF void cemon_timer_stop(cemon_timer *timer);
CEMON_DEF int cemon_post(cemon *loop,cemon_task_fn fn,void *ud);
CEMON_DEF int cemon_resolve(cemon_addr *out,const char *host,unsigned short port,int udp);
/* cemon_inspect reads the aggregate observability surface for a loop and/or
  socket. Semantic fields such as phase, delivery, close progress, capability,
  mailbox admission, and ownership are the stable runtime contract. The
  remaining fields are diagnostics: useful for observability and tests, but
  more tightly coupled to internal transport/runtime mechanics. loop_mailbox is
  a derived admission gate, not post queue depth, and socket_kind/state/
  socket_tcp_phase are raw diagnostic axes rather than stable semantic state.
  Pass loop-only state as (loop,0,out), socket-only state as (0,sock,out), or
  both when sock belongs to loop. Call it from the owner thread while the
  inspected objects are still alive. */
CEMON_DEF int cemon_inspect(cemon *loop,cemon_socket *sock,cemon_stats *out);
#ifdef __cplusplus
}
#endif
#endif
#if defined(CEMON_IMPLEMENTATION)&&!defined(CEMON_IMPLEMENTATION_ONCE)
#define CEMON_IMPLEMENTATION_ONCE
#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#else
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if defined(__APPLE__)||defined(__FreeBSD__)||defined(__OpenBSD__)||defined(__NetBSD__)||defined(__DragonFly__)
#define CEMON_USE_KQUEUE 1
#endif
#include <unistd.h>
#include <sched.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#ifdef CEMON_USE_KQUEUE
#include <sys/event.h>
#else
#include <sys/epoll.h>
#if defined(__linux__)
#include <sys/eventfd.h>
#endif
#endif
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#ifndef CEMON_MALLOC
#define CEMON_MALLOC malloc
#endif
#ifndef CEMON_FREE
#define CEMON_FREE free

#ifdef K_ALLOC_DEBUG
/* Debug builds only: route this layer's allocator through kbase's self-describing allocator, which
   quarantines freed blocks with canaries and records each allocation site.  These layers allocate
   with their own macros (not K_MALLOC) and this header does not depend on kbase, so the three
   functions are forward-declared here; their definitions live in kbase.h's K_ALLOC_DEBUG block.
   Without this redirection the instrument is blind to exactly the objects a cross-thread bug
   corrupts (treap nodes/blobs, cemon sockets, runtime queue nodes).  Inert in production builds. */
static void *k_dbg_malloc(size_t size,int line,const char *file);
static void *k_dbg_calloc(size_t count,size_t size,int line,const char *file);
static void k_dbg_free(void *p);
static void k_dbg_free_site(void *p,int line,const char *file);
#define CEMON_MALLOC(n) k_dbg_malloc((size_t)(n),__LINE__,__FILE__)
#define CEMON_CALLOC(n,s) k_dbg_calloc((size_t)(n),(size_t)(s),__LINE__,__FILE__)
#define CEMON_FREE(p) k_dbg_free_site((void *)(p),__LINE__,__FILE__)
#endif
#endif
/* I/O geometry and backend sizing. */
/* Per-socket receive chunk.  One WSARecv is posted at a time (see
   cemon_recv), so this is also the largest amount of a stream that one network
   completion can deliver: a 512 KiB frame used to arrive as 256 completions,
   each of which drove a full server round.  8 KiB (XP-compatible; no OS API
   dependency) cuts that by 4 while keeping the per-socket post buffer bounded
   (4096 connections x 8 KiB = 32 MiB). */
#define CEMON_BUF 8192
#define CEMON_UDP_BUF 65536
#define CEMON_NUMERIC_HOST_TEXT 128
#define CEMON_PORT_TEXT 16
#define CEMON_WAKE_FDS 2
#define CEMON_WAKE_READ 0
#define CEMON_WAKE_WRITE 1
#define CEMON_UNIX_POLL_BATCH 64
#define CEMON_UNIX_SLOT_BASE 64
#define CEMON_WIN_SEND_CHUNK 65536U
#define CEMON_ACCEPT_RETRY_MS 1
#define CEMON_ACCEPT_ADDR_LEN (CEMON_ADDR_STORAGE+16)
#define CEMON_ACCEPT_BUF_LEN (CEMON_ACCEPT_ADDR_LEN*2)
/* Capacity limits. */
#define CEMON_BACKLOG 128
/* The XP-era SDK provides the mswsock.h extension symbols (LPFN_ACCEPTEX/LPFN_CONNECTEX,
   WSAID_ACCEPTEX/CONNECTEX, SO_UPDATE_*_CONTEXT); an ancient SDK without them fails to
   compile here by design - pin the toolchain (see build.sh) instead of hand-rolling the
   function-pointer types. */
#ifndef _WIN32
#define CEMON_ERR_NOBUFS ENOMEM
#else
#define CEMON_ERR_NOBUFS WSAENOBUFS   /* 12/ENOMEM is not a Winsock code */
#endif
#define CEMON_ACCEPT_CREDIT 4
#define CEMON_SENDQ_LIMIT 1048576U
#define CEMON_LOOP_SENDQ_LIMIT (CEMON_SENDQ_LIMIT*4U)
#define CEMON_CONTROL_SEND_RESERVE 16384U
#define CEMON_POSTQ_LIMIT 4096
#define CEMON_SEND_CLASS_BULK 0
#define CEMON_SEND_CLASS_CONTROL 1
/* Fairness slices. */
#define CEMON_DISPATCH_BUDGET 256
/* Completions drained per Windows owner iteration (mirrors CEMON_UNIX_POLL_BATCH
   for the kevent/epoll paths). */
#define CEMON_WIN_POLL_BATCH 64
#ifdef CEMON_USE_KQUEUE
#define CEMON_KQ_READ_REG 1
#define CEMON_KQ_WRITE_REG 2
#define CEMON_KQ_WAKE_ID 1
#endif
/* Private implementation aliases for the raw diagnostic domains above. These
  keep implementation code terse; they are not an additional semantic layer.
  CEMON_TCP_PHASE_* shares the same numeric domain as
  CEMON_SOCKET_TCP_PHASE_PUBLIC_* for inspect/debug export. */
#define CEMON_TCP_SOCK 1
#define CEMON_UDP_SOCK 2
#define CEMON_LISTEN_SOCK 3
#define CEMON_CONNECTING_STATE 1
#define CEMON_OPEN_STATE 2
#define CEMON_DEAD_STATE 3
/* TCP stream phases collapse peer EOF and local write shutdown into one explicit lifecycle. */
#define CEMON_TCP_PHASE_OPEN 0
#define CEMON_TCP_PHASE_READ_EOF 1
#define CEMON_TCP_PHASE_WRITE_DRAIN 2
#define CEMON_TCP_PHASE_WRITE_CLOSED 3
#define CEMON_TCP_PHASE_READ_EOF_WRITE_DRAIN 4
#define CEMON_TCP_PHASE_HALF_CLOSED 5
#define CEMON_TCP_PHASE_COUNT 6
#define CEMON_TCP_TRANSITION_NO_CHANGE (-1)
#define CEMON_TCP_TRANSITION_EOF 1
#define CEMON_TCP_TRANSITION_SHUTDOWN 2
#define CEMON_TCP_TRANSITION_WRITE_CLOSED 3
#define CEMON_TCP_TRANSITION_COUNT 4
/* Internal implementation types. */
typedef struct cemon_post_node cemon_post_node;
typedef struct cemon_send_node cemon_send_node;
/* Internal loop control flags (bitmask), not externally visible phases. */
enum{
  CEMON_LOOP_POST_RUNNING=1,
  CEMON_LOOP_POST_STOP_REQUESTED=2,
  CEMON_LOOP_POST_STOPPING=4,
  CEMON_LOOP_POST_OWNER_BOUND=8,
  CEMON_LOOP_POST_CLOSING=16
};
/* Internal lifecycle transition actions. These mutate post_bits; they are not
  loop phases, mailbox states, or externally observable semantic values. */
enum{
  CEMON_LOOP_LIFECYCLE_TRANSITION_STOP_REQUEST=0,
  CEMON_LOOP_LIFECYCLE_TRANSITION_BEGIN_STOP=1,
  CEMON_LOOP_LIFECYCLE_TRANSITION_RUN_ENTER=2,
  CEMON_LOOP_LIFECYCLE_TRANSITION_RUN_EXIT=3,
  CEMON_LOOP_LIFECYCLE_TRANSITION_COUNT=4
};
enum{
  CEMON_LOOP_ADMISSION_OWNER_ONLY=0,
  CEMON_LOOP_ADMISSION_OPEN=1,
  CEMON_LOOP_ADMISSION_OWNER_CALLBACK=2
};
typedef struct cemon_loop_control{
  unsigned int post_bits;
  int ingress_count;
} cemon_loop_control;
#if defined(_WIN32)
#define CEMON_WAKE_KEY ((ULONG_PTR)1)
#define CEMON_BAD_FD INVALID_SOCKET
typedef SOCKET cemon_fd;
typedef int cemon_socklen;
#else
#define CEMON_BAD_FD (-1)
typedef int cemon_fd;
typedef socklen_t cemon_socklen;
#endif
#if defined(_WIN32)
enum{
  CEMON_OV_ACCEPT=1,
  CEMON_OV_CONNECT=2,
  CEMON_OV_RECV=3,
  CEMON_OV_SEND=4
};
typedef struct cemon_win_op{
  OVERLAPPED ol;
  cemon_socket *sock;
  void *owner;
  int op;
} cemon_win_op;
typedef struct cemon_win_accept_slot{
  cemon_win_op op;
  SOCKET fd;
  int slot;
  char buf[CEMON_ACCEPT_BUF_LEN];
} cemon_win_accept_slot;
typedef struct cemon_win_accept{
  int family;
  cemon_win_accept_slot slotv[CEMON_ACCEPT_CREDIT];
} cemon_win_accept;
typedef struct cemon_win_connect{
  int busy;
  cemon_win_op op;
} cemon_win_connect;
typedef struct cemon_win_recv{
  int busy;
  cemon_win_op op;
  char buf[CEMON_BUF];
} cemon_win_recv;
typedef struct cemon_win_udp_recv{
  char addr[CEMON_ADDR_STORAGE];
  int addr_len;
  char buf[CEMON_UDP_BUF];
} cemon_win_udp_recv;
#endif
struct cemon_post_node{
  cemon_post_node *next;
  cemon_task_fn fn;
  void *ud;
  void *storage;
};
struct cemon_send_node{
  cemon_send_node *next;
  cemon_socket *owner;
  unsigned int cost;
  int addr_len;
  int len;
  int off;
  int send_class;   /* CEMON_SEND_CLASS_BULK / CONTROL: drives the dequeue priority */
  char *buf;
#if defined(_WIN32)
  cemon_win_op op;
#endif
};
#ifndef _WIN32
typedef struct cemon_unix_slot{
  cemon_socket *sock;
  unsigned int gen;
  int next_free;
} cemon_unix_slot;
#endif
struct cemon_timer{
  cemon *loop;
  cemon_u64 at;
  cemon_u64 period;
  cemon_u64 seq;
  cemon_task_fn fn;
  void *ud;
  int heap_index;
  int firing;
  int dead;
};
struct cemon_socket{
  cemon *loop;
  cemon_socket *next;
  cemon_socket *prev;
  cemon_io_fn fn;
  void *ud;
  int kind;
  int state;
  int refs;
  int tcp_phase;
  int recv_armed;
  cemon_fd fd;
  unsigned int send_cost;
  cemon_send_node *send_head;
  cemon_send_node *send_tail;
  cemon_socket *accept_retry_next;
  int accept_retry_pending;
#if defined(_WIN32)
  int send_busy;
  int accept_busy;
  cemon_win_accept *accept;
  cemon_win_connect *connect;
  cemon_win_recv *recv;
  cemon_win_udp_recv *udp_recv;
#else
  int unix_slot;
  unsigned int unix_gen;
  int reg;
  int watch_r;
  int watch_w;
  char recv_buf[CEMON_BUF];
  char *udp_recv_buf;
#endif
};
struct cemon{
  cemon_loop_control control;
  unsigned int sock_total;
  unsigned int send_cost;
  unsigned int send_cost_peak;
  unsigned int post_count;
  unsigned int post_count_peak;
  unsigned int timer_count;
  cemon_u64 blocked_accept_count;
  cemon_u64 blocked_connect_count;
  cemon_u64 blocked_data_count;
  cemon_u64 blocked_sent_count;
  cemon_u64 terminal_eof_count;
  cemon_u64 terminal_close_count;
  cemon_u64 stop_begin_count;
  cemon_u64 stop_socket_close_count;
  cemon_u64 accept_retry_queue_count;
  cemon_u64 accept_retry_run_count;
  cemon_u64 timer_cap;
  int callback_depth;
  cemon_socket *sock_head;
  cemon_send_node *sent_head;
  cemon_send_node *sent_tail;
  cemon_timer **timers;
  cemon_u64 timer_seq;
  cemon_post_node *post_head;
  cemon_post_node *post_tail;
  int wake_pending;
  cemon_socket *accept_retry_ready_head;
  cemon_socket *accept_retry_ready_tail;
  cemon_socket *accept_retry_head;
  cemon_socket *accept_retry_tail;
  cemon_u64 accept_retry_due;
#if defined(_WIN32)
  DWORD owner_tid;
  CRITICAL_SECTION post_lock;
  HANDLE port;
  LPFN_ACCEPTEX acceptex;
  LPFN_CONNECTEX connectex;
  LPFN_GETACCEPTEXSOCKADDRS getacceptexsockaddrs;
#else
  pthread_t owner_tid;
  pthread_mutex_t post_lock;
  cemon_unix_slot *unix_slots;
  int unix_slot_cap;
  int unix_slot_free;
  int fd;
  int wake_fd[CEMON_WAKE_FDS];
#endif
};
static void cemon_socket_die(cemon_socket *sock,int status,int notify);
static void cemon_drain_posts(cemon *loop,int *budget_io,int consume_wake);
#ifndef _WIN32
static int cemon_accept_retry_resume(cemon_socket *sock);
static void cemon_unix_unwatch(cemon_socket *sock);
#endif
static cemon_u64 cemon_monotonic_us(void){
#if defined(_WIN32)
  LARGE_INTEGER freq,counter;
  cemon_u64 whole,part;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&counter);
  whole=(cemon_u64)(counter.QuadPart/freq.QuadPart);
  part=(cemon_u64)(counter.QuadPart%freq.QuadPart);
  return whole*(cemon_u64)1000000+(part*(cemon_u64)1000000)/(cemon_u64)freq.QuadPart;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC,&ts);
  return (cemon_u64)ts.tv_sec*(cemon_u64)1000000+(cemon_u64)(ts.tv_nsec/(cemon_u64)1000);
#endif
}
static cemon_u64 cemon_timer_repeat_at(cemon_u64 at,cemon_u64 period,cemon_u64 now){
  cemon_u64 steps,limit;
  if(period==0) return now;
  if(now<at) return at+period;
  steps=((now-at)/period)+1;
  limit=(~(cemon_u64)0)-at;
  if(period>0&&steps>limit/period){
    if(now>(~(cemon_u64)0)-period) return ~(cemon_u64)0;
    return now+period;
  }
  return at+steps*period;
}
static int cemon_us_to_int(cemon_u64 us){
  if(us){
    us=(us+999)/1000;
    if(us>(cemon_u64)2147483647) return 2147483647;   /* plain int constant: no C99 ULL suffix */
  }
  return (int)us;
}
static int cemon_addr_copy(cemon_addr *out,const struct sockaddr *addr,int len){
  if(out==0||addr==0||len<=0||len>(int)sizeof(out->data)) return -1;
  memset(out,0,sizeof(*out));
  out->len=len;
  memcpy(out->data,addr,(size_t)len);
  return 0;
}
static int cemon_numeric_addr(cemon_addr *out,const char *host,unsigned short port,int passive){
  if(out==0) return -1;
  if(host==0||host[0]==0){
    struct sockaddr_in addr4;
    if(!passive) return -1;
    memset(&addr4,0,sizeof(addr4));
    addr4.sin_family=AF_INET;
    addr4.sin_port=htons(port);
    addr4.sin_addr.s_addr=htonl(INADDR_ANY);
    return cemon_addr_copy(out,(const struct sockaddr*)&addr4,sizeof(addr4));
  }
#if defined(_WIN32)
  {
    char text[CEMON_NUMERIC_HOST_TEXT];
    struct sockaddr_in addr4;
    int len;
    if(strlen(host)>=sizeof(text)) return -1;
    strcpy(text,host);
    memset(&addr4,0,sizeof(addr4));
    len=(int)sizeof(addr4);
    if(WSAStringToAddressA(text,AF_INET,0,(struct sockaddr*)&addr4,&len)==0){
      addr4.sin_port=htons(port);
      return cemon_addr_copy(out,(const struct sockaddr*)&addr4,len);
    }
  }
#ifdef AF_INET6
  {
    char text[CEMON_NUMERIC_HOST_TEXT];
    struct sockaddr_in6 addr6;
    int len;
    if(strlen(host)>=sizeof(text)) return -1;
    strcpy(text,host);
    memset(&addr6,0,sizeof(addr6));
    len=(int)sizeof(addr6);
    if(WSAStringToAddressA(text,AF_INET6,0,(struct sockaddr*)&addr6,&len)==0){
      addr6.sin6_port=htons(port);
      return cemon_addr_copy(out,(const struct sockaddr*)&addr6,len);
    }
  }
#endif
#else
  {
    struct in_addr addr4;
    if(inet_pton(AF_INET,host,&addr4)==1){
      struct sockaddr_in sa4;
      memset(&sa4,0,sizeof(sa4));
      sa4.sin_family=AF_INET;
      sa4.sin_port=htons(port);
      sa4.sin_addr=addr4;
      return cemon_addr_copy(out,(const struct sockaddr*)&sa4,sizeof(sa4));
    }
  }
  {
    struct in6_addr addr6;
    if(inet_pton(AF_INET6,host,&addr6)==1){
      struct sockaddr_in6 sa6;
      memset(&sa6,0,sizeof(sa6));
      sa6.sin6_family=AF_INET6;
      sa6.sin6_port=htons(port);
      sa6.sin6_addr=addr6;
      return cemon_addr_copy(out,(const struct sockaddr*)&sa6,sizeof(sa6));
    }
  }
#endif
  return -1;
}
/* UDP reuse (SO_REUSEADDR): lets multiple sockets bind one UDP port (multicast).
   NOT for TCP listen - see cemon_set_listen_opts. */
static int cemon_set_reuse(cemon_fd fd){
  int on=1;
  return setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,(const char*)&on,sizeof(on));
}
/* TCP listener bind options, split out from cemon_set_reuse because the correct flag
   DIFFERS by platform. POSIX SO_REUSEADDR = safe TIME_WAIT rebind (no sharing).
   Windows SO_REUSEADDR ALSO allows PORT SHARING - two SO_REUSEADDR sockets bind the
   same port and the kernel routes connects nondeterministically (a security hole and
   a source of "dirty" non-deterministic failures), so use SO_EXCLUSIVEADDRUSE there:
   binding an already-listened port fails clean. Its cost (no TIME_WAIT rebind) is
   acceptable for a server listener. */
static int cemon_set_listen_opts(cemon_fd fd){
  int on=1;
#ifdef _WIN32
  return setsockopt(fd,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(const char*)&on,sizeof(on));
#else
  return setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,(const char*)&on,sizeof(on));
#endif
}
static int cemon_set_nodelay(cemon_fd fd){
  int on=1;
  return setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,(const char*)&on,sizeof(on));
}
#if defined(_WIN32)
static int cemon_last_error(void){
  return (int)WSAGetLastError();
}
static int cemon_win_iocp_pending(int err){
  return err==WSA_IO_PENDING;
}
static int cemon_win_udp_recv_soft_error(int err){
  return err==WSAECONNRESET||err==WSAEMSGSIZE||err==ERROR_MORE_DATA;
}
static void cemon_fd_close(cemon_fd fd){
  if(fd!=CEMON_BAD_FD) closesocket(fd);
}
static int cemon_win_udp_no_reset(cemon_fd fd){
  BOOL off=FALSE;
  DWORD got=0;
  int err;
  if(WSAIoctl(fd,SIO_UDP_CONNRESET,&off,sizeof(off),0,0,&got,0,0)==0) return 0;
  err=cemon_last_error();
  if(err==WSAEINVAL||err==WSAENOPROTOOPT||err==WSAEOPNOTSUPP) return 0;
  return -1;
}
#else
static int cemon_last_error(void){
  return errno;
}
static int cemon_wouldblock(int err){
  return err==EAGAIN||err==EWOULDBLOCK;
}
static int cemon_connect_in_progress(int err){
  return err==EINTR||err==EINPROGRESS||cemon_wouldblock(err);
}
static int cemon_unix_udp_recv_soft_error(int err){
  return err==ECONNREFUSED||err==ECONNRESET||err==EPROTO||err==ENETDOWN||err==EHOSTDOWN||err==EHOSTUNREACH||err==ENETUNREACH;
}
static int cemon_unix_accept_soft_error(int err){
  return err==ECONNABORTED||err==ECONNRESET||err==EPROTO||err==ENETDOWN||err==ENOPROTOOPT||err==EHOSTDOWN||err==EHOSTUNREACH||err==EOPNOTSUPP||err==ENETUNREACH;
}
static int cemon_unix_accept_retry_error(int err){
  return err==EMFILE||err==ENFILE||err==ENOBUFS||err==ENOMEM||err==ENOSR||err==ENOSPC;
}
static void cemon_fd_close(cemon_fd fd){
  if(fd!=CEMON_BAD_FD) close(fd);
}
static int cemon_set_nonblock(cemon_fd fd){
  int flags=fcntl(fd,F_GETFL,0);
  if(flags<0) return -1;
  return fcntl(fd,F_SETFL,flags|O_NONBLOCK);
}
static int cemon_set_nosigpipe(cemon_fd fd){
#ifdef SO_NOSIGPIPE
  int on=1;
  return setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,(const char*)&on,sizeof(on));
#else
  return 0;
#endif
}
static int cemon_unix_send_raw(cemon_fd fd,const void *buf,size_t len,int flags,const struct sockaddr *addr,cemon_socklen addrlen){
  if(addr) return sendto(fd,buf,len,flags,addr,addrlen);
  return send(fd,buf,len,flags);
}
static int cemon_unix_slot_grow(cemon *loop){
  int i;
  int old_cap=loop->unix_slot_cap;
  int cap=old_cap?old_cap*2:CEMON_UNIX_SLOT_BASE;
  cemon_unix_slot *slots=(cemon_unix_slot*)CEMON_MALLOC(sizeof(*slots)*(size_t)cap);
  if(slots==0) return -1;
  if(loop->unix_slots&&old_cap>0) memcpy(slots,loop->unix_slots,sizeof(*slots)*(size_t)old_cap);
  for(i=old_cap;i<cap;i++){
    slots[i].sock=0;
    slots[i].gen=0;
    slots[i].next_free=i+1<cap?i+1:loop->unix_slot_free;
  }
  if(loop->unix_slots) CEMON_FREE(loop->unix_slots);
  loop->unix_slots=slots;
  loop->unix_slot_cap=cap;
  loop->unix_slot_free=old_cap;
  return 0;
}
static int cemon_unix_slot_alloc(cemon_socket *sock){
  cemon *loop;
  cemon_unix_slot *slot;
  int idx;
  if(sock==0||sock->loop==0) return -1;
  loop=sock->loop;
  if(loop->unix_slot_free<0&&cemon_unix_slot_grow(loop)<0) return -1;
  idx=loop->unix_slot_free;
  if(idx<0||idx>=loop->unix_slot_cap) return -1;
  slot=&loop->unix_slots[idx];
  loop->unix_slot_free=slot->next_free;
  slot->next_free=-1;
  slot->sock=sock;
  slot->gen++;
  if(slot->gen==0) slot->gen=1;
  sock->unix_slot=idx;
  sock->unix_gen=slot->gen;
  return 0;
}
static void cemon_unix_slot_release(cemon_socket *sock){
  cemon *loop;
  cemon_unix_slot *slot;
  int idx;
  if(sock==0||sock->loop==0) return;
  idx=sock->unix_slot;
  if(idx<0) return;
  loop=sock->loop;
  if(idx>=loop->unix_slot_cap){
    sock->unix_slot=-1;
    sock->unix_gen=0;
    return;
  }
  slot=&loop->unix_slots[idx];
  if(slot->sock==sock&&slot->gen==sock->unix_gen) slot->sock=0;
  slot->next_free=loop->unix_slot_free;
  loop->unix_slot_free=idx;
  sock->unix_slot=-1;
  sock->unix_gen=0;
}
static unsigned long long cemon_unix_cookie(cemon_socket *sock){
  if(sock==0||sock->unix_slot<0||sock->unix_gen==0) return 0;
  return (((unsigned long long)sock->unix_gen)<<32)|((unsigned long long)(unsigned int)(sock->unix_slot+1));
}
static cemon_socket *cemon_unix_event_sock(cemon *loop,unsigned long long key){
  cemon_unix_slot *slot;
  unsigned int gen;
  int idx;
  if(loop==0||key==0) return 0;
  idx=(int)((unsigned int)(key&0xffffffffU));
  if(idx<=0) return 0;
  idx--;
  if(idx>=loop->unix_slot_cap) return 0;
  gen=(unsigned int)(key>>32);
  slot=&loop->unix_slots[idx];
  if(slot->gen!=gen) return 0;
  return slot->sock;
}
static int cemon_sock_error(cemon_socket *sock){
  int so=0;
  cemon_socklen sl=(cemon_socklen)sizeof(so);
  if(sock==0||sock->fd==CEMON_BAD_FD) return 1;
  if(getsockopt(sock->fd,SOL_SOCKET,SO_ERROR,(char*)&so,&sl)<0) return cemon_last_error();
  return so;
}
#endif
static void cemon_bind_owner_locked(cemon *loop){
  if(loop==0||(loop->control.post_bits&CEMON_LOOP_POST_OWNER_BOUND)!=0) return;
#if defined(_WIN32)
  loop->owner_tid=GetCurrentThreadId();
#else
  loop->owner_tid=pthread_self();
#endif
  loop->control.post_bits|=CEMON_LOOP_POST_OWNER_BOUND;
}
static int cemon_owner_ok_locked(cemon *loop){
  if(loop==0) return 0;
  if((loop->control.post_bits&CEMON_LOOP_POST_OWNER_BOUND)==0) return 1;
#if defined(_WIN32)
  return loop->owner_tid==GetCurrentThreadId();
#else
  return pthread_equal(loop->owner_tid,pthread_self())!=0;
#endif
}
static int cemon_loop_mailbox_from_post_bits(unsigned int post_bits){
  if((post_bits&CEMON_LOOP_POST_CLOSING)!=0) return CEMON_LOOP_MAILBOX_CLOSED;
  return (post_bits&(CEMON_LOOP_POST_STOP_REQUESTED|CEMON_LOOP_POST_STOPPING))!=0?CEMON_LOOP_MAILBOX_STOP_GATED:CEMON_LOOP_MAILBOX_OPEN;
}
static int cemon_loop_ownership_from_post_bits(unsigned int post_bits){
  if((post_bits&CEMON_LOOP_POST_CLOSING)!=0) return CEMON_LOOP_OWNERSHIP_CLOSING;
  return (post_bits&CEMON_LOOP_POST_OWNER_BOUND)!=0?CEMON_LOOP_OWNERSHIP_BOUND:CEMON_LOOP_OWNERSHIP_UNBOUND;
}
static int cemon_loop_mailbox_accepts_mode(int mailbox,int mode,int owner,int callback_depth){
  if(mode==CEMON_LOOP_ADMISSION_OWNER_ONLY) return 1;
  if(mailbox==CEMON_LOOP_MAILBOX_OPEN) return 1;
  return mode==CEMON_LOOP_ADMISSION_OWNER_CALLBACK&&mailbox==CEMON_LOOP_MAILBOX_STOP_GATED&&owner&&callback_depth>0;
}
static int cemon_loop_lifecycle_index_from_post_bits(unsigned int post_bits){
  int index=0;
  if((post_bits&CEMON_LOOP_POST_RUNNING)!=0) index|=4;
  if((post_bits&CEMON_LOOP_POST_STOP_REQUESTED)!=0) index|=2;
  if((post_bits&CEMON_LOOP_POST_STOPPING)!=0) index|=1;
  return index;
}
static unsigned int cemon_loop_post_bits_with_lifecycle(unsigned int post_bits,int lifecycle_index){
  post_bits&=~(CEMON_LOOP_POST_RUNNING|CEMON_LOOP_POST_STOP_REQUESTED|CEMON_LOOP_POST_STOPPING);
  if((lifecycle_index&4)!=0) post_bits|=CEMON_LOOP_POST_RUNNING;
  if((lifecycle_index&2)!=0) post_bits|=CEMON_LOOP_POST_STOP_REQUESTED;
  if((lifecycle_index&1)!=0) post_bits|=CEMON_LOOP_POST_STOPPING;
  return post_bits;
}
static int cemon_loop_transition_post_bits_locked(cemon *loop,int action){
  static const signed char next_lifecycle[CEMON_LOOP_LIFECYCLE_TRANSITION_COUNT][8]={
    { 2, 3, 2, 3, 6, 7, 6, 7 },
    { 1, 1, 3, 3, 5, 5, 7, 7 },
    { 4, 5, 6, 7,-1,-1,-1,-1 },
    { 0, 1, 2, 3, 0, 1, 2, 3 }
  };
  int current,next;
  if(loop==0||action<0||action>=CEMON_LOOP_LIFECYCLE_TRANSITION_COUNT) return -1;
  current=cemon_loop_lifecycle_index_from_post_bits(loop->control.post_bits);
  next=next_lifecycle[action][current];
  if(next<0) return -1;
  loop->control.post_bits=cemon_loop_post_bits_with_lifecycle(loop->control.post_bits,next);
  return current!=next;
}
static int cemon_loop_mailbox_accepts_locked(cemon *loop,int mode,int force){
  int mailbox,owner;
  if(loop==0) return 0;
  if(force) return 1;
  mailbox=cemon_loop_mailbox_from_post_bits(loop->control.post_bits);
  owner=cemon_owner_ok_locked(loop);
  return cemon_loop_mailbox_accepts_mode(mailbox,mode,owner,owner?loop->callback_depth:0);
}
static int cemon_loop_admit_locked(cemon *loop,int mode){
  if(loop==0) return 0;
  if((loop->control.post_bits&CEMON_LOOP_POST_OWNER_BOUND)==0) cemon_bind_owner_locked(loop);
  if(!cemon_owner_ok_locked(loop)) return 0;
  return cemon_loop_mailbox_accepts_locked(loop,mode,0);
}
static int cemon_loop_admit(cemon *loop,int mode){
  int ok;
  if(loop==0) return 0;
#if defined(_WIN32)
  EnterCriticalSection(&loop->post_lock);
#else
  pthread_mutex_lock(&loop->post_lock);
#endif
  ok=cemon_loop_admit_locked(loop,mode);
#if defined(_WIN32)
  LeaveCriticalSection(&loop->post_lock);
#else
  pthread_mutex_unlock(&loop->post_lock);
#endif
  return ok;
}
static int cemon_socket_admit(cemon_socket *sock,int mode){
  return sock&&sock->loop&&cemon_loop_admit(sock->loop,mode);
}
static int cemon_timer_admit(cemon_timer *timer,int mode){
  return timer&&timer->loop&&cemon_loop_admit(timer->loop,mode);
}
static int cemon_loop_mailbox_state(cemon *loop){
  unsigned int post_bits;
  if(loop==0) return CEMON_LOOP_MAILBOX_CLOSED;
#if defined(_WIN32)
  EnterCriticalSection(&loop->post_lock);
#else
  pthread_mutex_lock(&loop->post_lock);
#endif
  post_bits=loop->control.post_bits;
#if defined(_WIN32)
  LeaveCriticalSection(&loop->post_lock);
#else
  pthread_mutex_unlock(&loop->post_lock);
#endif
  return cemon_loop_mailbox_from_post_bits(post_bits);
}
static int cemon_ingress_enter(cemon *loop,int mode){
  int ok;
  if(loop==0) return 0;
#if defined(_WIN32)
  EnterCriticalSection(&loop->post_lock);
#else
  pthread_mutex_lock(&loop->post_lock);
#endif
  ok=cemon_loop_mailbox_accepts_locked(loop,mode,0);
  if(ok) loop->control.ingress_count++;
#if defined(_WIN32)
  LeaveCriticalSection(&loop->post_lock);
#else
  pthread_mutex_unlock(&loop->post_lock);
#endif
  return ok;
}
static void cemon_ingress_leave(cemon *loop){
  if(loop==0) return;
#if defined(_WIN32)
  EnterCriticalSection(&loop->post_lock);
#else
  pthread_mutex_lock(&loop->post_lock);
#endif
  if(loop->control.ingress_count>0) loop->control.ingress_count--;
#if defined(_WIN32)
  LeaveCriticalSection(&loop->post_lock);
#else
  pthread_mutex_unlock(&loop->post_lock);
#endif
}
static void cemon_ingress_close(cemon *loop){
  int remaining;
  if(loop==0) return;
  for(;;){
#if defined(_WIN32)
    EnterCriticalSection(&loop->post_lock);
#else
    pthread_mutex_lock(&loop->post_lock);
#endif
    loop->control.post_bits|=CEMON_LOOP_POST_CLOSING;
    remaining=loop->control.ingress_count;
#if defined(_WIN32)
    LeaveCriticalSection(&loop->post_lock);
#else
    pthread_mutex_unlock(&loop->post_lock);
#endif
    if(remaining==0) return;
#if defined(_WIN32)
    SwitchToThread();
#else
    sched_yield();
#endif
  }
}
static void cemon_loop_control_flags_locked(cemon *loop,int *active_out,int *stop_out,int *stopping_out){
  unsigned int post_bits;
  if(active_out) *active_out=0;
  if(stop_out) *stop_out=1;
  if(stopping_out) *stopping_out=1;
  if(loop==0) return;
  post_bits=loop->control.post_bits;
  if(active_out) *active_out=(post_bits&CEMON_LOOP_POST_RUNNING)!=0;
  if(stop_out) *stop_out=(post_bits&CEMON_LOOP_POST_STOP_REQUESTED)!=0;
  if(stopping_out) *stopping_out=(post_bits&CEMON_LOOP_POST_STOPPING)!=0;
}
static int cemon_stop_pending(cemon *loop){
  return cemon_loop_mailbox_state(loop)!=CEMON_LOOP_MAILBOX_OPEN;
}
static int cemon_loop_periodic_rearm_allowed(cemon *loop){
  return cemon_loop_mailbox_state(loop)==CEMON_LOOP_MAILBOX_OPEN;
}
static int cemon_post_wake_needed(int owner,int active,int pending,int in_callback){
  return pending&&active&&(!owner||!in_callback);
}
static int cemon_stop_request_wake_needed(int owner,int active){
  return active&&!owner;
}
static int cemon_socket_is_dead(cemon_socket *sock){
  return sock==0||sock->state==CEMON_DEAD_STATE;
}
static int cemon_socket_is_open(cemon_socket *sock){
  return sock&&sock->state==CEMON_OPEN_STATE;
}
#ifndef _WIN32
static int cemon_socket_is_connecting(cemon_socket *sock){
  return sock&&sock->state==CEMON_CONNECTING_STATE;
}
#endif
static int cemon_socket_mark_dead(cemon_socket *sock){
  if(cemon_socket_is_dead(sock)) return 0;
  sock->state=CEMON_DEAD_STATE;
  return 1;
}
static void cemon_socket_mark_open(cemon_socket *sock){
  if(sock) sock->state=CEMON_OPEN_STATE;
}
static void cemon_socket_mark_connecting(cemon_socket *sock){
  if(sock) sock->state=CEMON_CONNECTING_STATE;
}
static int cemon_event_blocked(cemon_socket *sock,int type){
  if(cemon_socket_is_dead(sock)) return 1;
  return (type==CEMON_ACCEPT||type==CEMON_CONNECT||type==CEMON_DATA||type==CEMON_SENT)&&cemon_stop_pending(sock->loop)!=0;
}
static void cemon_observe_blocked(cemon_socket *sock,int type){
  cemon *loop;
  if(sock==0||sock->fn==0||sock->loop==0) return;
  loop=sock->loop;
  if(type==CEMON_ACCEPT) loop->blocked_accept_count++;
  else if(type==CEMON_CONNECT) loop->blocked_connect_count++;
  else if(type==CEMON_DATA) loop->blocked_data_count++;
  else if(type==CEMON_SENT) loop->blocked_sent_count++;
}
static void cemon_observe_terminal(cemon_socket *sock,int type){
  cemon *loop;
  if(sock==0||sock->fn==0||sock->loop==0) return;
  loop=sock->loop;
  if(type==CEMON_EOF) loop->terminal_eof_count++;
  else if(type==CEMON_CLOSED) loop->terminal_close_count++;
}
static void cemon_send_drop(cemon_socket *sock,cemon_send_node *node){
  cemon *loop;
  if(node==0) return;
  loop=sock?sock->loop:0;
  if(sock){
    if(sock->send_cost>=node->cost) sock->send_cost-=node->cost;
    else sock->send_cost=0;
  }
  if(loop){
    if(loop->send_cost>=node->cost) loop->send_cost-=node->cost;
    else loop->send_cost=0;
  }
  CEMON_FREE(node);
}
static void cemon_send_release_cost(cemon_socket *sock,cemon_send_node *node){
  cemon *loop;
  if(node==0) return;
  loop=sock?sock->loop:0;
  if(sock){
    if(sock->send_cost>=node->cost) sock->send_cost-=node->cost;
    else sock->send_cost=0;
  }
  if(loop){
    if(loop->send_cost>=node->cost) loop->send_cost-=node->cost;
    else loop->send_cost=0;
  }
  node->cost=0;
}
static void cemon_send_progress(cemon_socket *sock,cemon_send_node *node,int sent){
  cemon *loop;
  unsigned int keep,delta;
  int remain;
  if(sock==0||node==0||sent<=0) return;
  loop=sock->loop;
  remain=node->len-node->off;
  if(sent>remain) sent=remain;
  keep=(unsigned int)sizeof(*node)+(unsigned int)node->addr_len+(unsigned int)(remain-sent);
  if(node->cost<=keep) return;
  delta=node->cost-keep;
  node->cost=keep;
  if(sock->send_cost>=delta) sock->send_cost-=delta;
  else sock->send_cost=0;
  if(loop){
    if(loop->send_cost>=delta) loop->send_cost-=delta;
    else loop->send_cost=0;
  }
}
static int cemon_tcp_phase_has_eof(int phase){
  return phase==CEMON_TCP_PHASE_READ_EOF||phase==CEMON_TCP_PHASE_READ_EOF_WRITE_DRAIN||phase==CEMON_TCP_PHASE_HALF_CLOSED;
}
static int cemon_tcp_phase_write_draining(int phase){
  return phase==CEMON_TCP_PHASE_WRITE_DRAIN||phase==CEMON_TCP_PHASE_READ_EOF_WRITE_DRAIN;
}
static int cemon_tcp_phase_write_closed(int phase){
  return phase==CEMON_TCP_PHASE_WRITE_CLOSED||phase==CEMON_TCP_PHASE_HALF_CLOSED;
}
static int cemon_tcp_has_eof(cemon_socket *sock){
  return sock&&sock->kind==CEMON_TCP_SOCK&&cemon_tcp_phase_has_eof(sock->tcp_phase);
}
static int cemon_tcp_shutdown_pending(cemon_socket *sock){
  return sock&&sock->kind==CEMON_TCP_SOCK&&(cemon_tcp_phase_write_draining(sock->tcp_phase)||cemon_tcp_phase_write_closed(sock->tcp_phase));
}
static int cemon_tcp_is_write_closed(cemon_socket *sock){
  return sock&&sock->kind==CEMON_TCP_SOCK&&cemon_tcp_phase_write_closed(sock->tcp_phase);
}
static int cemon_tcp_recv_admissible(cemon_socket *sock){
  return sock&&sock->kind==CEMON_TCP_SOCK&&!cemon_tcp_has_eof(sock);
}
static int cemon_tcp_send_admissible(cemon_socket *sock){
  return sock&&sock->kind==CEMON_TCP_SOCK&&!cemon_tcp_shutdown_pending(sock);
}
static int cemon_tcp_send_io_active(cemon_socket *sock){
  return sock&&sock->kind==CEMON_TCP_SOCK&&cemon_socket_is_open(sock);
}
static int cemon_tcp_shutdown_admissible(cemon_socket *sock){
  return sock&&sock->kind==CEMON_TCP_SOCK&&!cemon_socket_is_dead(sock)&&cemon_socket_is_open(sock);
}
static int cemon_tcp_shutdown_write_needed(cemon_socket *sock){
  return sock&&sock->kind==CEMON_TCP_SOCK&&cemon_tcp_shutdown_pending(sock)&&sock->send_head==0&&!cemon_tcp_is_write_closed(sock);
}
static int cemon_tcp_finish_closes(cemon_socket *sock){
  return sock&&sock->kind==CEMON_TCP_SOCK&&cemon_tcp_has_eof(sock)&&cemon_tcp_is_write_closed(sock);
}
static int cemon_tcp_transition(cemon_socket *sock,int action){
  static const signed char next_phase[CEMON_TCP_TRANSITION_COUNT][CEMON_TCP_PHASE_COUNT]={
    { CEMON_TCP_TRANSITION_NO_CHANGE, CEMON_TCP_TRANSITION_NO_CHANGE, CEMON_TCP_TRANSITION_NO_CHANGE, CEMON_TCP_TRANSITION_NO_CHANGE, CEMON_TCP_TRANSITION_NO_CHANGE, CEMON_TCP_TRANSITION_NO_CHANGE },
    { CEMON_TCP_PHASE_READ_EOF, CEMON_TCP_TRANSITION_NO_CHANGE, CEMON_TCP_PHASE_READ_EOF_WRITE_DRAIN, CEMON_TCP_PHASE_HALF_CLOSED, CEMON_TCP_TRANSITION_NO_CHANGE, CEMON_TCP_TRANSITION_NO_CHANGE },
    { CEMON_TCP_PHASE_WRITE_DRAIN, CEMON_TCP_PHASE_READ_EOF_WRITE_DRAIN, CEMON_TCP_TRANSITION_NO_CHANGE, CEMON_TCP_TRANSITION_NO_CHANGE, CEMON_TCP_TRANSITION_NO_CHANGE, CEMON_TCP_TRANSITION_NO_CHANGE },
    { CEMON_TCP_PHASE_WRITE_CLOSED, CEMON_TCP_PHASE_HALF_CLOSED, CEMON_TCP_PHASE_WRITE_CLOSED, CEMON_TCP_TRANSITION_NO_CHANGE, CEMON_TCP_PHASE_HALF_CLOSED, CEMON_TCP_TRANSITION_NO_CHANGE }
  };
  int phase,next;
  if(sock==0||sock->kind!=CEMON_TCP_SOCK) return -1;
  if(action<=0||action>=CEMON_TCP_TRANSITION_COUNT) return -1;
  phase=sock->tcp_phase;
  if(phase<CEMON_TCP_PHASE_OPEN||phase>=CEMON_TCP_PHASE_COUNT) return action==CEMON_TCP_TRANSITION_SHUTDOWN?-1:1;
  next=next_phase[action][phase];
  if(next==CEMON_TCP_TRANSITION_NO_CHANGE) return 1;
  sock->tcp_phase=next;
  return 0;
}
static void cemon_tcp_note_eof(cemon_socket *sock){
  cemon_tcp_transition(sock,CEMON_TCP_TRANSITION_EOF);
}
static int cemon_tcp_start_shutdown(cemon_socket *sock){
  return cemon_tcp_transition(sock,CEMON_TCP_TRANSITION_SHUTDOWN);
}
static void cemon_tcp_note_write_closed(cemon_socket *sock){
  cemon_tcp_transition(sock,CEMON_TCP_TRANSITION_WRITE_CLOSED);
}
static int cemon_tcp_shutdown_write(cemon_socket *sock){
  if(sock==0||!cemon_tcp_shutdown_admissible(sock)||cemon_tcp_is_write_closed(sock)||sock->fd==CEMON_BAD_FD) return -1;
#if defined(_WIN32)
  if(shutdown(sock->fd,SD_SEND)!=0) return -1;
#else
  if(shutdown(sock->fd,SHUT_WR)!=0) return -1;
#endif
  cemon_tcp_note_write_closed(sock);
  return 0;
}
/* 1 = the socket is closed (dead after this call, and it may already be FREED:
   cemon_socket_die drops the last reference, so the caller must not touch `sock`
   again), 0 = left alive for the loop, -1 = it died on a failed write shutdown.
   Returning the outcome is required, not a convenience: re-reading the socket
   after a close is a use-after-free (cemon_shutdown did exactly that). */
static int cemon_tcp_finish(cemon_socket *sock){
  if(sock==0||sock->kind!=CEMON_TCP_SOCK) return 0;
  if(cemon_socket_is_dead(sock)) return 1;
  if(cemon_tcp_shutdown_write_needed(sock)){
    if(cemon_tcp_shutdown_write(sock)<0){
      cemon_socket_die(sock,cemon_last_error(),1);
      return -1;
    }
  }
  if(cemon_tcp_finish_closes(sock)){
    cemon_socket_die(sock,0,1);
    return 1;
  }
  return 0;
}
static int cemon_recv_request_valid(cemon_socket *sock,int udp){
  if(cemon_socket_is_dead(sock)||sock->kind==CEMON_LISTEN_SOCK) return 0;
  if(udp) return sock->kind==CEMON_UDP_SOCK;
  return sock->kind!=CEMON_UDP_SOCK&&cemon_tcp_recv_admissible(sock);
}
static int cemon_recv_active(cemon_socket *sock){
  if(cemon_socket_is_dead(sock)||!sock->recv_armed) return 0;
  return sock->kind==CEMON_UDP_SOCK||cemon_socket_is_open(sock);
}
static int cemon_send_request_valid(cemon_socket *sock){
  if(cemon_socket_is_dead(sock)||sock->kind==CEMON_LISTEN_SOCK) return 0;
  return sock->kind!=CEMON_TCP_SOCK||cemon_tcp_send_admissible(sock);
}
static int cemon_send_post_active(cemon_socket *sock){
  if(cemon_socket_is_dead(sock)||sock->kind==CEMON_LISTEN_SOCK||sock->send_head==0) return 0;
  return sock->kind!=CEMON_TCP_SOCK||cemon_tcp_send_io_active(sock);
}
#ifndef _WIN32
static int cemon_send_flush_active(cemon_socket *sock){
  return sock&&sock->kind!=CEMON_LISTEN_SOCK&&sock->send_head&&!cemon_socket_is_dead(sock)&&!cemon_socket_is_connecting(sock);
}
#endif
static void cemon_list_add(cemon *loop,cemon_socket *sock){
  sock->prev=0;
  sock->next=loop->sock_head;
  if(loop->sock_head) loop->sock_head->prev=sock;
  loop->sock_head=sock;
}
static void cemon_list_del(cemon *loop,cemon_socket *sock){
  if(sock->prev) sock->prev->next=sock->next;
  else if(loop->sock_head==sock) loop->sock_head=sock->next;
  if(sock->next) sock->next->prev=sock->prev;
  sock->next=0;
  sock->prev=0;
}
static cemon_socket *cemon_sock_new(cemon *loop,cemon_fd fd,int kind,cemon_io_fn fn,void *ud){
  cemon_socket *sock;
  sock=(cemon_socket*)CEMON_MALLOC(sizeof(*sock));
  if(sock==0) return 0;
  memset(sock,0,sizeof(*sock));
  sock->loop=loop;
  sock->fd=fd;
  sock->kind=kind;
  sock->fn=fn;
  sock->ud=ud;
  cemon_socket_mark_open(sock);
  sock->refs=1;
#if defined(_WIN32)
  if(kind==CEMON_LISTEN_SOCK){
    int i;
    sock->accept=(cemon_win_accept*)CEMON_MALLOC(sizeof(*sock->accept));
    if(sock->accept==0) goto fail;
    memset(sock->accept,0,sizeof(*sock->accept));
    for(i=0;i<CEMON_ACCEPT_CREDIT;i++){
      sock->accept->slotv[i].fd=CEMON_BAD_FD;
      sock->accept->slotv[i].slot=i;
    }
  }else{
    sock->recv=(cemon_win_recv*)CEMON_MALLOC(sizeof(*sock->recv));
    if(sock->recv==0) goto fail;
    memset(sock->recv,0,sizeof(*sock->recv));
  }
  if(kind==CEMON_UDP_SOCK){
    sock->udp_recv=(cemon_win_udp_recv*)CEMON_MALLOC(sizeof(*sock->udp_recv));
    if(sock->udp_recv==0) goto fail;
    memset(sock->udp_recv,0,sizeof(*sock->udp_recv));
  }
#else
  sock->unix_slot=-1;
  if(kind==CEMON_UDP_SOCK){
    sock->udp_recv_buf=(char*)CEMON_MALLOC((size_t)CEMON_UDP_BUF);
    if(sock->udp_recv_buf==0) goto fail;
  }
  if(cemon_unix_slot_alloc(sock)<0) goto fail;
#endif
  loop->sock_total++;
  cemon_list_add(loop,sock);
  return sock;
fail:
#if defined(_WIN32)
  if(sock->udp_recv) CEMON_FREE(sock->udp_recv);
  if(sock->recv) CEMON_FREE(sock->recv);
  if(sock->accept) CEMON_FREE(sock->accept);
#else
  if(sock->udp_recv_buf) CEMON_FREE(sock->udp_recv_buf);
  cemon_unix_slot_release(sock);
#endif
  CEMON_FREE(sock);
  return 0;
}
static void cemon_sock_free(cemon_socket *sock){
  cemon_send_node *node,*next;
  cemon *loop;
  if(sock==0) return;
  loop=sock->loop;
  node=sock->send_head;
  while(node){
    next=node->next;
    cemon_send_drop(sock,node);
    node=next;
  }
#if defined(_WIN32)
  if(sock->fd!=CEMON_BAD_FD) cemon_fd_close(sock->fd);
  if(sock->accept){
    int i;
    for(i=0;i<CEMON_ACCEPT_CREDIT;i++) if(sock->accept->slotv[i].fd!=CEMON_BAD_FD) cemon_fd_close(sock->accept->slotv[i].fd);
    CEMON_FREE(sock->accept);
  }
  if(sock->connect) CEMON_FREE(sock->connect);
  if(sock->recv) CEMON_FREE(sock->recv);
  if(sock->udp_recv) CEMON_FREE(sock->udp_recv);
#else
  if(sock->fd!=CEMON_BAD_FD) cemon_fd_close(sock->fd);
  if(sock->udp_recv_buf) CEMON_FREE(sock->udp_recv_buf);
#endif
  if(loop&&loop->sock_total>0) loop->sock_total--;
  CEMON_FREE(sock);
}
static void cemon_sock_hold(cemon_socket *sock){
  sock->refs++;
}
static void cemon_sock_release(cemon_socket *sock){
  if(--sock->refs==0) cemon_sock_free(sock);
}
static void cemon_emit(cemon_socket *sock,int type,int status,void *data,int size,const cemon_addr *addr){
  cemon_event ev;
  cemon *loop;
  if(sock==0||sock->fn==0) return;
  loop=sock->loop;
  cemon_sock_hold(sock);
  memset(&ev,0,sizeof(ev));
  ev.type=type;
  ev.status=status;
  ev.size=size;
  ev.data=data;
  if(addr) ev.addr=*addr;
  loop->callback_depth++;
  sock->fn(sock,&ev);
  loop->callback_depth--;
  cemon_sock_release(sock);
}
#define CEMON_EMIT_SKIP 0
#define CEMON_EMIT_DONE 1
#define CEMON_EMIT_CONTINUE 2
static int cemon_emit_blockable(cemon_socket *sock,int type,int status,void *data,int size,const cemon_addr *addr){
  int rc;
  if(cemon_event_blocked(sock,type)){
    cemon_observe_blocked(sock,type);
    return CEMON_EMIT_SKIP;
  }
  cemon_sock_hold(sock);
  cemon_emit(sock,type,status,data,size,addr);
  rc=(cemon_socket_is_dead(sock)||cemon_event_blocked(sock,type))?CEMON_EMIT_DONE:CEMON_EMIT_CONTINUE;
  cemon_sock_release(sock);
  return rc;
}
static int cemon_emit_terminal(cemon_socket *sock,int type,int status,void *data,int size,const cemon_addr *addr){
  int rc;
  cemon_sock_hold(sock);
  cemon_observe_terminal(sock,type);
  cemon_emit(sock,type,status,data,size,addr);
  rc=cemon_socket_is_dead(sock)?CEMON_EMIT_DONE:CEMON_EMIT_CONTINUE;
  cemon_sock_release(sock);
  return rc;
}
static int cemon_sent_enqueue(cemon_socket *sock,cemon_send_node *node){
  cemon *loop;
  if(sock==0||node==0) return -1;
  loop=sock->loop;
  if(cemon_event_blocked(sock,CEMON_SENT)){
    cemon_observe_blocked(sock,CEMON_SENT);
    CEMON_FREE(node);
    return 0;
  }
  node->owner=sock;
  node->next=0;
  cemon_sock_hold(sock);
  if(loop->sent_tail) loop->sent_tail->next=node;
  else loop->sent_head=node;
  loop->sent_tail=node;
  return 0;
}
static void cemon_drain_sent(cemon *loop,int *budget_io){
  cemon_send_node *node;
  cemon_socket *sock;
  int budget;
  if(loop==0) return;
  budget=budget_io?*budget_io:CEMON_DISPATCH_BUDGET;
  while(loop->sent_head&&budget>0){
    node=loop->sent_head;
    loop->sent_head=node->next;
    if(loop->sent_head==0) loop->sent_tail=0;
    sock=node->owner;
    if(sock) cemon_emit_blockable(sock,CEMON_SENT,0,0,node->len,0);
    if(sock) cemon_sock_release(sock);
    CEMON_FREE(node);
    budget--;
  }
  if(budget_io) *budget_io=budget;
}
static void cemon_clear_sent(cemon *loop){
  cemon_send_node *node,*next;
  if(loop==0) return;
  node=loop->sent_head;
  loop->sent_head=0;
  loop->sent_tail=0;
  while(node){
    next=node->next;
    if(node->owner) cemon_sock_release(node->owner);
    CEMON_FREE(node);
    node=next;
  }
}
static int cemon_sent_ready(cemon *loop){
  return loop&&loop->sent_head!=0;
}
static int cemon_post_ready(cemon *loop){
  return loop&&loop->post_head!=0;
}
static int cemon_accept_retry_due_now(cemon *loop){
  if(loop==0||loop->accept_retry_head==0) return 0;
  return loop->accept_retry_due==0||cemon_monotonic_us()>=loop->accept_retry_due;
}
static int cemon_accept_retry_ready(cemon *loop){
  if(loop==0) return 0;
  if(loop->accept_retry_ready_head) return 1;
  return cemon_accept_retry_due_now(loop);
}
static int cemon_deferred_phase_ready(cemon *loop){
  return loop&&(cemon_sent_ready(loop)||cemon_accept_retry_ready(loop));
}
static int cemon_tail_phase_ready(cemon *loop){
  return loop&&(cemon_post_ready(loop)||cemon_deferred_phase_ready(loop));
}
#if defined(_WIN32)
static void cemon_win_op_init(cemon_win_op *op,cemon_socket *sock,int kind,void *owner){
  memset(op,0,sizeof(*op));
  op->sock=sock;
  op->owner=owner;
  op->op=kind;
}
#endif
static unsigned int cemon_loop_send_limit(int send_class){
  if(send_class==CEMON_SEND_CLASS_CONTROL) return CEMON_LOOP_SENDQ_LIMIT;
  return CEMON_LOOP_SENDQ_LIMIT-(CEMON_LOOP_SENDQ_LIMIT>CEMON_CONTROL_SEND_RESERVE?CEMON_CONTROL_SEND_RESERVE:0u);
}
static int cemon_timer_less(cemon_timer *a,cemon_timer *b){
  if(a->at!=b->at) return a->at<b->at;
  return a->seq<b->seq;
}
static void cemon_timer_swap(cemon *loop,int a,int b){
  cemon_timer *timer;
  timer=loop->timers[a];
  loop->timers[a]=loop->timers[b];
  loop->timers[b]=timer;
  loop->timers[a]->heap_index=a;
  loop->timers[b]->heap_index=b;
}
static void cemon_timer_up(cemon *loop,int idx){
  while(idx>0){
    int parent=(idx-1)/2;
    if(!cemon_timer_less(loop->timers[idx],loop->timers[parent])) break;
    cemon_timer_swap(loop,idx,parent);
    idx=parent;
  }
}
static void cemon_timer_down(cemon *loop,int idx){
  for(;;){
    int left=idx*2+1;
    int right=left+1;
    int best=idx;
    if((unsigned int)left<loop->timer_count&&cemon_timer_less(loop->timers[left],loop->timers[best])) best=left;
    if((unsigned int)right<loop->timer_count&&cemon_timer_less(loop->timers[right],loop->timers[best])) best=right;
    if(best==idx) break;
    cemon_timer_swap(loop,idx,best);
    idx=best;
  }
}
static int cemon_timer_grow(cemon *loop){
  int cap=loop->timer_cap?loop->timer_cap*2:16;
  cemon_timer **timers=(cemon_timer**)CEMON_MALLOC(sizeof(*timers)*(size_t)cap);
  if(timers==0) return -1;
  if(loop->timers&&loop->timer_count>0) memcpy(timers,loop->timers,sizeof(*timers)*(size_t)loop->timer_count);
  if(loop->timers) CEMON_FREE(loop->timers);
  loop->timers=timers;
  loop->timer_cap=cap;
  return 0;
}
static int cemon_timer_link(cemon *loop,cemon_timer *timer){
  int idx;
  if(loop->timer_count==loop->timer_cap&&cemon_timer_grow(loop)<0) return -1;
  idx=loop->timer_count;
  loop->timer_count++;
  timer->seq=loop->timer_seq;
  loop->timer_seq++;
  timer->heap_index=idx;
  loop->timers[idx]=timer;
  cemon_timer_up(loop,idx);
  return 0;
}
static void cemon_timer_unlink(cemon_timer *timer){
  cemon *loop;
  int idx,last;
  if(timer==0||timer->loop==0||timer->heap_index<0) return;
  loop=timer->loop;
  idx=timer->heap_index;
  last=loop->timer_count-1;
  timer->heap_index=-1;
  if(last<0) return;
  loop->timer_count=last;
  if(idx!=last){
    loop->timers[idx]=loop->timers[last];
    loop->timers[idx]->heap_index=idx;
    if(idx>0&&cemon_timer_less(loop->timers[idx],loop->timers[(idx-1)/2])) cemon_timer_up(loop,idx);
    else cemon_timer_down(loop,idx);
  }
  loop->timers[last]=0;
}
static void cemon_timer_clear(cemon *loop){
  while(loop->timer_count>0){
    cemon_timer *timer;
    loop->timer_count--;
    timer=loop->timers[loop->timer_count];
    loop->timers[loop->timer_count]=0;
    if(timer) CEMON_FREE(timer);
  }
}
static int cemon_timer_due(cemon *loop){
  return loop&&loop->timer_count>0&&cemon_monotonic_us()>=loop->timers[0]->at;
}
/* The timer-phase run loop already proved a due timer exists, so this helper
  avoids a second due check and clock read on the hot path. */
static void cemon_timer_phase_step(cemon *loop,int *budget_io){
  cemon_u64 now;
  cemon_timer *timer;
  int budget;
  if(loop==0) return;
  budget=budget_io?*budget_io:CEMON_DISPATCH_BUDGET;
  if(budget<=0){
    if(budget_io) *budget_io=budget;
    return;
  }
  timer=loop->timers[0];
  cemon_timer_unlink(timer);
  if(timer->dead){
    CEMON_FREE(timer);
    budget--;
    if(budget_io) *budget_io=budget;
    return;
  }
  timer->firing=1;
  loop->callback_depth++;
  timer->fn(loop,timer->ud);
  loop->callback_depth--;
  timer->firing=0;
  now=cemon_monotonic_us();
  if(timer->dead||timer->period==0||!cemon_loop_periodic_rearm_allowed(loop)){
    CEMON_FREE(timer);
  }else{
    timer->at=cemon_timer_repeat_at(timer->at,timer->period,now);
    cemon_timer_link(loop,timer);
  }
  budget--;
  if(budget_io) *budget_io=budget;
}
static void cemon_run_timer_phase(cemon *loop,int *budget_io){
  int budget=budget_io?*budget_io:CEMON_DISPATCH_BUDGET;
  while(budget>0&&cemon_timer_due(loop)){
    int step_budget=1;
    if(budget==1&&cemon_tail_phase_ready(loop)) break;
    cemon_timer_phase_step(loop,&step_budget);
    budget-=1-step_budget;
  }
  if(budget_io) *budget_io=budget;
}
static int cemon_accept_retry_timeout(cemon *loop,int timeout){
  cemon_u64 now;
  int wait;
  if(loop==0) return timeout;
  if(loop->accept_retry_ready_head) return 0;
  if(loop->accept_retry_head==0) return timeout;
  now=cemon_monotonic_us();
  if(loop->accept_retry_due<=now) return 0;
  wait=cemon_us_to_int(loop->accept_retry_due-now);
  if(timeout<0||wait<timeout) return wait;
  return timeout;
}
static int cemon_timeout(cemon *loop){
  int timeout=-1;
  if(loop->timer_count>0){
    cemon_u64 now=cemon_monotonic_us();
    if(loop->timers[0]->at<=now) timeout=0;
    else timeout=cemon_us_to_int(loop->timers[0]->at-now);
  }
  if(cemon_tail_phase_ready(loop)) timeout=0;
  return cemon_accept_retry_timeout(loop,timeout);
}
#if defined(_WIN32)
static int cemon_win_guid(SOCKET fd,const GUID *id,void *out){
  DWORD got=0;
  return WSAIoctl(fd,SIO_GET_EXTENSION_FUNCTION_POINTER,(void*)id,sizeof(*id),out,sizeof(void*),&got,0,0)==0?0:-1;
}
static int cemon_win_load_ext(cemon *loop,SOCKET fd){
  GUID g1=WSAID_ACCEPTEX;
  GUID g2=WSAID_GETACCEPTEXSOCKADDRS;
  GUID g3=WSAID_CONNECTEX;
  if(loop->acceptex==0&&cemon_win_guid(fd,&g1,&loop->acceptex)<0) return -1;
  if(loop->getacceptexsockaddrs==0&&cemon_win_guid(fd,&g2,&loop->getacceptexsockaddrs)<0) return -1;
  if(loop->connectex==0&&cemon_win_guid(fd,&g3,&loop->connectex)<0) return -1;
  return 0;
}
static int cemon_bind_any(SOCKET fd,int family){
  if(family==AF_INET6){
    struct sockaddr_in6 a;
    memset(&a,0,sizeof(a));
    a.sin6_family=AF_INET6;
    return bind(fd,(struct sockaddr*)&a,sizeof(a));
  }else{
    struct sockaddr_in a;
    memset(&a,0,sizeof(a));
    a.sin_family=AF_INET;
    return bind(fd,(struct sockaddr*)&a,sizeof(a));
  }
}
static int cemon_win_post_recv(cemon_socket *sock){
  cemon_win_recv *recv;
  cemon_win_udp_recv *udp_recv;
  WSABUF buf;
  DWORD got,flags;
  int rc;
  recv=sock->recv;
  if(!cemon_recv_active(sock)) return 0;
  if(recv==0) return -1;
  if(recv->busy) return 0;
  cemon_win_op_init(&recv->op,sock,CEMON_OV_RECV,recv);
  flags=0;
  got=0;
  recv->busy=1;
  cemon_sock_hold(sock);
  if(sock->kind==CEMON_UDP_SOCK){
    udp_recv=sock->udp_recv;
    if(udp_recv==0){
      recv->busy=0;
      cemon_sock_release(sock);
      return -1;
    }
    buf.buf=udp_recv->buf;
    buf.len=(ULONG)sizeof(udp_recv->buf);
    udp_recv->addr_len=sizeof(udp_recv->addr);
    rc=WSARecvFrom(sock->fd,&buf,1,&got,&flags,(struct sockaddr*)udp_recv->addr,&udp_recv->addr_len,&recv->op.ol,0);
  }else{
    buf.buf=recv->buf;
    buf.len=(ULONG)sizeof(recv->buf);
    rc=WSARecv(sock->fd,&buf,1,&got,&flags,&recv->op.ol,0);
  }
  if(rc==SOCKET_ERROR){
    rc=cemon_last_error();
    if(!cemon_win_iocp_pending(rc)){
      recv->busy=0;
      cemon_sock_release(sock);
      return -1;
    }
  }
  return 0;
}
static int cemon_win_post_send(cemon_socket *sock){
  cemon_send_node *node;
  struct sockaddr *addr;
  WSABUF buf;
  DWORD got;
  unsigned int remain;
  int rc;
  if(!cemon_send_post_active(sock)||sock->send_busy) return 0;
  node=sock->send_head;
  cemon_win_op_init(&node->op,sock,CEMON_OV_SEND,node);
  if(node->addr_len>0){
    buf.buf=node->buf;
    buf.len=(ULONG)node->len;
  }else{
    buf.buf=node->buf+node->off;
    remain=(unsigned int)(node->len-node->off);
    if(remain>CEMON_WIN_SEND_CHUNK) remain=CEMON_WIN_SEND_CHUNK;
    buf.len=(ULONG)remain;
  }
  got=0;
  sock->send_busy=1;
  cemon_sock_hold(sock);
  if(node->addr_len>0){
    addr=(struct sockaddr*)(node->buf+node->len);
    rc=WSASendTo(sock->fd,&buf,1,&got,0,addr,node->addr_len,&node->op.ol,0);
  }else{
    rc=WSASend(sock->fd,&buf,1,&got,0,&node->op.ol,0);
  }
  if(rc==SOCKET_ERROR){
    rc=cemon_last_error();
    if(!cemon_win_iocp_pending(rc)){
      sock->send_busy=0;
      cemon_sock_release(sock);
      return -1;
    }
  }
  return 0;
}
static int cemon_win_post_accept_one(cemon_socket *sock,int slot){
  cemon_win_accept *accept;
  cemon_win_accept_slot *slot_op;
  DWORD got;
  BOOL ok;
  accept=sock->accept;
  if(cemon_socket_is_dead(sock)||accept==0||slot<0||slot>=CEMON_ACCEPT_CREDIT||accept->slotv[slot].fd!=CEMON_BAD_FD) return 0;
  slot_op=&accept->slotv[slot];
  slot_op->fd=WSASocketA(accept->family,SOCK_STREAM,IPPROTO_TCP,0,0,WSA_FLAG_OVERLAPPED);
  if(slot_op->fd==INVALID_SOCKET) return -1;
  cemon_win_op_init(&slot_op->op,sock,CEMON_OV_ACCEPT,slot_op);
  sock->accept_busy++;
  got=0;
  cemon_sock_hold(sock);
  ok=sock->loop->acceptex(sock->fd,slot_op->fd,slot_op->buf,0,CEMON_ACCEPT_ADDR_LEN,CEMON_ACCEPT_ADDR_LEN,&got,&slot_op->op.ol);
  if(!ok){
    int err=cemon_last_error();
    if(err!=WSA_IO_PENDING){
      sock->accept_busy--;
      cemon_fd_close(slot_op->fd);
      slot_op->fd=CEMON_BAD_FD;
      cemon_sock_release(sock);
      return -1;
    }
  }
  return 0;
}
static int cemon_win_post_accept(cemon_socket *sock){
  int i,posted;
  if(cemon_socket_is_dead(sock)||sock->accept==0) return 0;
  posted=0;
  for(i=0;i<CEMON_ACCEPT_CREDIT;i++){
    if(sock->accept->slotv[i].fd!=CEMON_BAD_FD) continue;
    if(cemon_win_post_accept_one(sock,i)<0){
      if(posted>0||sock->accept_busy>0) return 0;
      return -1;
    }
    posted++;
  }
  return 0;
}
#endif
#if defined(_WIN32)
static int cemon_accept_retry_resume(cemon_socket *sock){
  return cemon_win_post_accept(sock);
}
#endif
static cemon_socket *cemon_accept_retry_list_tail(cemon_socket *head){
  cemon_socket *tail=head;
  while(tail&&tail->accept_retry_next) tail=tail->accept_retry_next;
  return tail;
}
static void cemon_accept_retry_list_append(cemon_socket **head_io,cemon_socket **tail_io,cemon_socket *head,cemon_socket *tail){
  if(head_io==0||tail_io==0||head==0) return;
  if(tail==0) tail=cemon_accept_retry_list_tail(head);
  if(*tail_io) (*tail_io)->accept_retry_next=head;
  else *head_io=head;
  *tail_io=tail;
}
static int cemon_accept_retry_remove(cemon_socket **head_io,cemon_socket **tail_io,cemon_socket *sock){
  cemon_socket *prev,*node;
  if(head_io==0||tail_io==0||sock==0) return 0;
  prev=0;
  node=*head_io;
  while(node){
    if(node==sock){
      if(prev) prev->accept_retry_next=node->accept_retry_next;
      else *head_io=node->accept_retry_next;
      if(*tail_io==node) *tail_io=prev;
      if(*head_io==0) *tail_io=0;
      return 1;
    }
    prev=node;
    node=node->accept_retry_next;
  }
  return 0;
}
static void cemon_accept_retry_cancel(cemon_socket *sock){
  if(sock==0||sock->loop==0||!sock->accept_retry_pending) return;
  if(!cemon_accept_retry_remove(&sock->loop->accept_retry_ready_head,&sock->loop->accept_retry_ready_tail,sock)) cemon_accept_retry_remove(&sock->loop->accept_retry_head,&sock->loop->accept_retry_tail,sock);
  sock->accept_retry_next=0;
  sock->accept_retry_pending=0;
  if(sock->loop->accept_retry_head==0) sock->loop->accept_retry_due=0;
}
static void cemon_accept_retry_schedule(cemon *loop){
  if(loop==0) return;
  if(loop->accept_retry_head==0) loop->accept_retry_due=0;
  else if(loop->accept_retry_due==0) loop->accept_retry_due=cemon_monotonic_us()+((cemon_u64)CEMON_ACCEPT_RETRY_MS)*1000;
}
static void cemon_accept_retry_promote_due(cemon *loop){
  if(loop==0||loop->accept_retry_head==0||!cemon_accept_retry_due_now(loop)) return;
  cemon_accept_retry_list_append(&loop->accept_retry_ready_head,&loop->accept_retry_ready_tail,loop->accept_retry_head,loop->accept_retry_tail);
  loop->accept_retry_head=0;
  loop->accept_retry_tail=0;
  loop->accept_retry_due=0;
}
static int cemon_accept_retry_queue(cemon_socket *sock){
  if(sock==0||sock->loop==0||sock->kind!=CEMON_LISTEN_SOCK||cemon_socket_is_dead(sock)||cemon_should_stop(sock->loop)) return -1;
  if(sock->accept_retry_pending) return 0;
#ifndef _WIN32
  cemon_unix_unwatch(sock);
#endif
  sock->accept_retry_pending=1;
  sock->loop->accept_retry_queue_count++;
  sock->accept_retry_next=sock->loop->accept_retry_head;
  sock->loop->accept_retry_head=sock;
  if(sock->loop->accept_retry_tail==0) sock->loop->accept_retry_tail=sock;
  cemon_accept_retry_schedule(sock->loop);
  return 0;
}
static void cemon_accept_retry_run(cemon *loop,int *budget_io){
  cemon_socket *pending,*pending_tail,*sock;
  int err,budget;
  if(loop==0) return;
  budget=budget_io?*budget_io:CEMON_DISPATCH_BUDGET;
  cemon_accept_retry_promote_due(loop);
  if(loop->accept_retry_ready_head==0){
    loop->accept_retry_due=0;
    return;
  }
  pending=loop->accept_retry_ready_head;
  pending_tail=loop->accept_retry_ready_tail;
  if(pending==0) return;
  loop->accept_retry_ready_head=0;
  loop->accept_retry_ready_tail=0;
  while(pending&&budget>0){
    sock=pending;
    pending=sock->accept_retry_next;
    sock->accept_retry_next=0;
    if(cemon_socket_is_dead(sock)||!sock->accept_retry_pending) continue;
    sock->accept_retry_pending=0;
    loop->accept_retry_run_count++;
    if(cemon_should_stop(loop)==0&&cemon_accept_retry_resume(sock)<0){
      err=cemon_last_error();
      if(cemon_accept_retry_queue(sock)<0) cemon_socket_die(sock,err,1);
    }
    budget--;
  }
  if(pending){
    pending_tail=cemon_accept_retry_list_tail(pending);
    cemon_accept_retry_list_append(&loop->accept_retry_ready_head,&loop->accept_retry_ready_tail,pending,pending_tail);
  }
  if(budget_io) *budget_io=budget;
}
/* Owner-thread continuations now share one dispatch entrypoint even while
  storage remains split: due timers keep deadline ordering, and SENT delivery
  plus accept retry stay deferred queues.
  They still share one owner thread, one dispatch budget, and one stop/drain
  contract. */
static void cemon_deferred_phase_step(cemon *loop,int *budget_io){
  int budget;
  if(loop==0) return;
  budget=budget_io?*budget_io:CEMON_DISPATCH_BUDGET;
  if(budget<=0||!cemon_deferred_phase_ready(loop)){
    if(budget_io) *budget_io=budget;
    return;
  }
  if(cemon_sent_ready(loop)){
    int step_budget=1;
    cemon_drain_sent(loop,&step_budget);
    budget-=1-step_budget;
  }
  if(budget>0&&cemon_accept_retry_ready(loop)){
    int step_budget=1;
    cemon_accept_retry_run(loop,&step_budget);
    budget-=1-step_budget;
  }
  if(budget_io) *budget_io=budget;
}
static void cemon_run_deferred_phase(cemon *loop,int *budget_io){
  int budget;
  if(loop==0) return;
  budget=budget_io?*budget_io:CEMON_DISPATCH_BUDGET;
  while(budget>0&&cemon_deferred_phase_ready(loop)){
    int budget_before=budget;
    cemon_deferred_phase_step(loop,&budget);
    if(budget==budget_before) break;
  }
  if(budget_io) *budget_io=budget;
}
static void cemon_tail_phase_step(cemon *loop,int *budget_io,int *consume_wake_io){
  int budget;
  if(loop==0) return;
  budget=budget_io?*budget_io:CEMON_DISPATCH_BUDGET;
  if(budget<=0||!cemon_tail_phase_ready(loop)){
    if(budget_io) *budget_io=budget;
    return;
  }
  if(cemon_post_ready(loop)){
    int consume_wake=consume_wake_io?*consume_wake_io:0;
    int step_budget=1;
    cemon_drain_posts(loop,&step_budget,consume_wake);
    budget-=1-step_budget;
    if(step_budget<1&&consume_wake_io) *consume_wake_io=0;
  }
  if(budget>0&&cemon_deferred_phase_ready(loop)){
    int step_budget=1;
    cemon_run_deferred_phase(loop,&step_budget);
    budget-=1-step_budget;
  }
  if(budget_io) *budget_io=budget;
}
static void cemon_run_tail_phase(cemon *loop,int *budget_io,int consume_wake){
  int budget,consume_post_wake;
  if(loop==0) return;
  budget=budget_io?*budget_io:CEMON_DISPATCH_BUDGET;
  consume_post_wake=consume_wake;
  while(budget>0&&cemon_tail_phase_ready(loop)){
    int budget_before=budget;
    cemon_tail_phase_step(loop,&budget,&consume_post_wake);
    if(budget==budget_before) break;
  }
  if(budget_io) *budget_io=budget;
}
static void cemon_dispatch_owner(cemon *loop,int *budget_io,int consume_wake,int include_timers){
  int budget;
  if(loop==0) return;
  budget=budget_io?*budget_io:CEMON_DISPATCH_BUDGET;
  if(budget<=0){
    if(budget_io) *budget_io=budget;
    return;
  }
  if(include_timers) cemon_run_timer_phase(loop,&budget);
  if(budget>0&&cemon_tail_phase_ready(loop)) cemon_run_tail_phase(loop,&budget,consume_wake);
  if(budget_io) *budget_io=budget;
}
static void cemon_handle_owner_wake(cemon *loop,int consume_wake){
  int budget;
#if defined(__linux__)
  if(consume_wake){
    unsigned long long n;
    while(read(loop->wake_fd[CEMON_WAKE_READ],&n,sizeof(n))>0){}
  }
#endif
  /* Consume the wake event unconditionally: this poll just dequeued one wake
     packet, so wake_pending must clear here even if the post queue is already
     empty. Otherwise a later post sees wake_pending==1, skips emitting its own
     wake, and the loop stalls until the next poll timeout. */
  if(consume_wake){
#if defined(_WIN32)
    EnterCriticalSection(&loop->post_lock);
    loop->wake_pending=0;
    LeaveCriticalSection(&loop->post_lock);
#else
    pthread_mutex_lock(&loop->post_lock);
    loop->wake_pending=0;
    pthread_mutex_unlock(&loop->post_lock);
#endif
  }
  budget=CEMON_DISPATCH_BUDGET;
  cemon_dispatch_owner(loop,&budget,consume_wake,1);
}
#if defined(_WIN32)
static int cemon_win_post_connect(cemon_socket *sock,const cemon_addr *addr){
  cemon_win_connect *connect;
  DWORD got;
  BOOL ok;
  if(sock->state==CEMON_DEAD_STATE) return 0;
  connect=sock->connect;
  if(connect==0){
    connect=(cemon_win_connect*)CEMON_MALLOC(sizeof(*connect));
    if(connect==0) return -1;
    memset(connect,0,sizeof(*connect));
    sock->connect=connect;
  }else if(connect->busy) return -1;
  cemon_win_op_init(&connect->op,sock,CEMON_OV_CONNECT,connect);
  connect->busy=1;
  got=0;
  cemon_sock_hold(sock);
  ok=sock->loop->connectex(sock->fd,(const struct sockaddr*)addr->data,addr->len,0,0,&got,&connect->op.ol);
  if(!ok){
    int err=cemon_last_error();
    if(err!=WSA_IO_PENDING){
      connect->busy=0;
      cemon_sock_release(sock);
      return -1;
    }
  }
  return 0;
}
static int cemon_win_accept_recover(cemon_socket *sock,int status){
  if(sock==0||sock->state==CEMON_DEAD_STATE) return 0;
  if(cemon_should_stop(sock->loop)){
    cemon_socket_die(sock,0,1);
    return -1;
  }
  if(cemon_win_post_accept(sock)<0&&cemon_accept_retry_queue(sock)<0){
    cemon_socket_die(sock,status,1);
    return -1;
  }
  return 0;
}
static void cemon_win_accept_done(cemon_win_accept_slot *slot_op,int err){
  cemon_win_accept *accept;
  cemon_socket *peer,*sock;
  cemon_addr addr;
  struct sockaddr *local_addr,*remote_addr;
  int accept_err,local_len,remote_len,slot;
  SOCKET fd;
  if(slot_op==0||slot_op->op.sock==0) return;
  sock=slot_op->op.sock;
  accept=sock->accept;
  slot=slot_op->slot;
  if(accept==0||slot<0||slot>=CEMON_ACCEPT_CREDIT||&accept->slotv[slot]!=slot_op){
    cemon_socket_die(sock,1,1);
    return;
  }
  if(sock->accept_busy>0) sock->accept_busy--;
  fd=slot_op->fd;
  slot_op->fd=CEMON_BAD_FD;
  if(sock->state==CEMON_DEAD_STATE){
    if(fd!=CEMON_BAD_FD) cemon_fd_close(fd);
    return;
  }
  if(err!=0){
    if(fd!=CEMON_BAD_FD) cemon_fd_close(fd);
    if(err!=WSA_OPERATION_ABORTED) cemon_win_accept_recover(sock,err);
    return;
  }
  if(cemon_should_stop(sock->loop)){
    if(fd!=CEMON_BAD_FD) cemon_fd_close(fd);
    cemon_socket_die(sock,0,1);
    return;
  }
  if(setsockopt(fd,SOL_SOCKET,SO_UPDATE_ACCEPT_CONTEXT,(const char*)&sock->fd,sizeof(sock->fd))!=0){
    accept_err=cemon_last_error();
    cemon_fd_close(fd);
    cemon_win_accept_recover(sock,accept_err);
    return;
  }
  sock->loop->getacceptexsockaddrs(slot_op->buf,0,CEMON_ACCEPT_ADDR_LEN,CEMON_ACCEPT_ADDR_LEN,&local_addr,&local_len,&remote_addr,&remote_len);
  peer=cemon_sock_new(sock->loop,fd,CEMON_TCP_SOCK,sock->fn,sock->ud);
  if(peer==0){
    cemon_fd_close(fd);
    cemon_win_accept_recover(sock,12);
    return;
  }
  if(CreateIoCompletionPort((HANDLE)fd,sock->loop->port,0,0)==0){
    accept_err=cemon_last_error();
    cemon_socket_die(peer,0,0);
    cemon_win_accept_recover(sock,accept_err);
    return;
  }
  cemon_set_nodelay(fd);
  if(remote_len>0&&remote_len<=(int)sizeof(addr.data)){
    memset(&addr,0,sizeof(addr));
    addr.len=remote_len;
    memcpy(addr.data,remote_addr,remote_len);
  }else memset(&addr,0,sizeof(addr));
  if(cemon_win_post_accept(sock)<0){
    accept_err=cemon_last_error();
    cemon_socket_die(peer,0,0);
    if(cemon_accept_retry_queue(sock)<0) cemon_socket_die(sock,accept_err,1);
    return;
  }
  /* The accepted socket must be OPEN before the application sees CEMON_ACCEPT: the handler
     arms a read on it (cemon_recv), and cemon_recv refuses a socket that is not in the open
     state - so the arm failed for every freshly accepted connection and the server closed it
     in the same loop turn as the accept (the client saw CONNECT then a reset and never got a
     reply to the request it had already sent).  The connect path already opens its socket
     before emitting. */
  cemon_socket_mark_open(peer);
  if(cemon_emit_blockable(sock,CEMON_ACCEPT,0,peer,0,&addr)==CEMON_EMIT_SKIP){
    cemon_socket_die(peer,0,0);
    cemon_socket_die(sock,0,1);
  }
}
static void cemon_win_connect_done(cemon_win_connect *connect,int err){
  cemon_socket *sock;
  int post_err,emit_rc;
  if(connect==0||connect->op.sock==0) return;
  sock=connect->op.sock;
  connect->busy=0;
  if(sock->connect==connect) sock->connect=0;
  CEMON_FREE(connect);
  if(cemon_socket_is_dead(sock)) return;
  if(err!=0){
    if(err!=WSA_OPERATION_ABORTED) cemon_socket_die(sock,err,1);
    return;
  }
  if(cemon_should_stop(sock->loop)){
    cemon_socket_die(sock,0,1);
    return;
  }
  if(setsockopt(sock->fd,SOL_SOCKET,SO_UPDATE_CONNECT_CONTEXT,0,0)!=0){
    cemon_socket_die(sock,cemon_last_error(),1);
    return;
  }
  cemon_socket_mark_open(sock);
  cemon_set_nodelay(sock->fd);
  emit_rc=cemon_emit_blockable(sock,CEMON_CONNECT,0,0,0,0);
  if(emit_rc==CEMON_EMIT_SKIP){
    cemon_socket_die(sock,0,1);
    return;
  }
  if(emit_rc!=CEMON_EMIT_CONTINUE) return;
  if(!cemon_socket_is_dead(sock)&&cemon_should_stop(sock->loop)==0){
    post_err=0;
    if(cemon_recv_active(sock)) post_err=cemon_win_post_recv(sock);
    if(post_err<0) cemon_socket_die(sock,sock->recv?cemon_last_error():WSAEINVAL,1);
    else if(cemon_win_post_send(sock)<0) cemon_socket_die(sock,cemon_last_error(),1);
  }
}
static void cemon_win_recv_done(cemon_win_recv *recv,DWORD bytes,int err){
  cemon_addr addr;
  cemon_socket *sock;
  int post_err;
  cemon_win_udp_recv *udp_recv;
  if(recv==0||recv->op.sock==0) return;
  sock=recv->op.sock;
  recv->busy=0;
  if(cemon_socket_is_dead(sock)||sock->recv!=recv) return;
  if(err!=0){
    if(err==WSA_OPERATION_ABORTED) return;
    if(sock->kind==CEMON_UDP_SOCK&&cemon_win_udp_recv_soft_error(err)){
      post_err=0;
      if(cemon_recv_active(sock)) post_err=cemon_win_post_recv(sock);
      if(post_err<0) cemon_socket_die(sock,sock->udp_recv?cemon_last_error():WSAEINVAL,1);
      return;
    }
    cemon_socket_die(sock,err,1);
    return;
  }
  if(sock->kind==CEMON_TCP_SOCK&&bytes==0){
    sock->recv_armed=0;
    cemon_tcp_note_eof(sock);
    if(cemon_emit_terminal(sock,CEMON_EOF,0,0,0,0)==CEMON_EMIT_CONTINUE) cemon_tcp_finish(sock);
    return;
  }
  sock->recv_armed=0;
  memset(&addr,0,sizeof(addr));
  udp_recv=sock->udp_recv;
  if(sock->kind==CEMON_UDP_SOCK&&udp_recv&&udp_recv->addr_len>0&&udp_recv->addr_len<=(int)sizeof(addr.data)){
    addr.len=udp_recv->addr_len;
    memcpy(addr.data,udp_recv->addr,udp_recv->addr_len);
    if(cemon_emit_blockable(sock,CEMON_DATA,0,udp_recv->buf,(int)bytes,&addr)==CEMON_EMIT_SKIP) return;
  }else if(bytes>0){
    if(cemon_emit_blockable(sock,CEMON_DATA,0,recv->buf,(int)bytes,0)==CEMON_EMIT_SKIP) return;
  }
}
static void cemon_win_send_done(cemon_send_node *node,DWORD bytes,int err){
  cemon_socket *sock;
  if(node==0||node->owner==0) return;
  sock=node->owner;
  sock->send_busy=0;
  if(sock->send_head!=node||cemon_socket_is_dead(sock)) return;
  if(err!=0){
    if(err!=WSA_OPERATION_ABORTED) cemon_socket_die(sock,err,1);
    return;
  }
  cemon_send_progress(sock,node,(int)bytes);
  if(node->addr_len>0){
    if(bytes!=(DWORD)node->len){
      cemon_socket_die(sock,WSAEMSGSIZE,1);
      return;
    }
  }else{
    node->off+=bytes;
    if(node->off<node->len){
      if(cemon_win_post_send(sock)<0) cemon_socket_die(sock,cemon_last_error(),1);
      return;
    }
  }
  sock->send_head=node->next;
  if(sock->send_head==0) sock->send_tail=0;
  cemon_send_release_cost(sock,node);
  if(cemon_sent_enqueue(sock,node)<0){
    cemon_socket_die(sock,CEMON_ERR_NOBUFS,1);
    return;
  }
  if(!cemon_socket_is_dead(sock)){
    /* cemon_tcp_finish may free the socket: never re-read it afterwards (this
       path is ref-held by the completion, but relying on that is fragile). */
    if(cemon_tcp_finish(sock)==0&&cemon_win_post_send(sock)<0) cemon_socket_die(sock,cemon_last_error(),1);
  }
}
static void cemon_win_handle(cemon_win_op *ov,DWORD bytes,int err){
  cemon_socket *sock;
  if(ov==0||ov->sock==0) return;
  sock=ov->sock;
  if(ov->op==CEMON_OV_ACCEPT) cemon_win_accept_done((cemon_win_accept_slot*)ov->owner,err);
  else if(ov->op==CEMON_OV_CONNECT) cemon_win_connect_done((cemon_win_connect*)ov->owner,err);
  else if(ov->op==CEMON_OV_RECV) cemon_win_recv_done((cemon_win_recv*)ov->owner,bytes,err);
  else if(ov->op==CEMON_OV_SEND) cemon_win_send_done((cemon_send_node*)ov->owner,bytes,err);
  cemon_sock_release(sock);
}
#else
#ifdef CEMON_USE_KQUEUE
static int cemon_kqueue_watch(cemon_socket *sock,int filter,int want,int *watch,int bit){
  struct kevent ev;
  unsigned long long key;
  int flags;
  if(!want&&*watch==0) return 0;
  if(want&&*watch!=0&&(sock->reg&bit)!=0) return 0;
  key=cemon_unix_cookie(sock);
  flags=want?((sock->reg&bit)?EV_ENABLE:(EV_ADD|EV_ENABLE|EV_CLEAR)):EV_DISABLE;
  EV_SET(&ev,sock->fd,filter,flags,0,0,(void*)(size_t)key);
  if(kevent(sock->loop->fd,&ev,1,0,0,0)<0){
    if(!want&&errno==ENOENT){
      sock->reg&=~bit;
      *watch=0;
      return 0;
    }
    return -1;
  }
  if(want) sock->reg|=bit;
  *watch=want;
  return 0;
}
#endif
static int cemon_unix_watch(cemon_socket *sock){
  int want_r,want_w;
  if(cemon_socket_is_dead(sock)) return 0;
  want_r=sock->kind==CEMON_LISTEN_SOCK||cemon_recv_active(sock);
  want_w=(cemon_socket_is_connecting(sock)||sock->send_head!=0);
#ifdef CEMON_USE_KQUEUE
  if(cemon_kqueue_watch(sock,EVFILT_READ,want_r,&sock->watch_r,CEMON_KQ_READ_REG)<0) return -1;
  if(cemon_kqueue_watch(sock,EVFILT_WRITE,want_w,&sock->watch_w,CEMON_KQ_WRITE_REG)<0) return -1;
#else
  cemon *loop;
  struct epoll_event ev;
  int op;
  unsigned long long key;
  loop=sock->loop;
  key=cemon_unix_cookie(sock);
  if(sock->reg&&want_r==sock->watch_r&&want_w==sock->watch_w) return 0;
  memset(&ev,0,sizeof(ev));
  if(want_r) ev.events|=EPOLLIN;
  if(want_r&&sock->kind==CEMON_TCP_SOCK) ev.events|=EPOLLRDHUP;
  if(want_w) ev.events|=EPOLLOUT;
  ev.events|=EPOLLERR|EPOLLHUP|EPOLLET;
  ev.data.u64=key;
  op=sock->reg?EPOLL_CTL_MOD:EPOLL_CTL_ADD;
  if(epoll_ctl(loop->fd,op,sock->fd,&ev)<0){
    if(op==EPOLL_CTL_MOD&&errno==ENOENT&&epoll_ctl(loop->fd,EPOLL_CTL_ADD,sock->fd,&ev)==0){
      sock->reg=1;
      sock->watch_r=want_r;
      sock->watch_w=want_w;
      return 0;
    }
    return -1;
  }
  sock->reg=1;
  sock->watch_r=want_r;
  sock->watch_w=want_w;
#endif
  return 0;
}
static void cemon_unix_unwatch(cemon_socket *sock){
#ifdef CEMON_USE_KQUEUE
  if(sock->watch_r) cemon_kqueue_watch(sock,EVFILT_READ,0,&sock->watch_r,CEMON_KQ_READ_REG);
  if(sock->watch_w) cemon_kqueue_watch(sock,EVFILT_WRITE,0,&sock->watch_w,CEMON_KQ_WRITE_REG);
#else
  if(sock->reg){
    epoll_ctl(sock->loop->fd,EPOLL_CTL_DEL,sock->fd,0);
    sock->reg=0;
  }
  sock->watch_r=0;
  sock->watch_w=0;
#endif
}
static int cemon_unix_flush(cemon_socket *sock){
#ifdef MSG_NOSIGNAL
  int flags=MSG_NOSIGNAL;
#else
  int flags=0;
#endif
  while(cemon_send_flush_active(sock)){
    int rc,err;
    struct sockaddr *addr;
    cemon_send_node *node=sock->send_head;
    if(node->addr_len>0){
      addr=(struct sockaddr*)(node->buf+node->len);
      rc=cemon_unix_send_raw(sock->fd,node->buf,(size_t)node->len,flags,addr,(cemon_socklen)node->addr_len);
    }
    else rc=cemon_unix_send_raw(sock->fd,node->buf+node->off,(size_t)(node->len-node->off),flags,0,0);
    if(rc>0){
      cemon_send_progress(sock,node,rc);
      if(node->addr_len>0){
        if(rc!=node->len){
          cemon_socket_die(sock,EMSGSIZE,1);
          return -1;
        }
      }else{
        node->off+=rc;
        if(node->off<node->len){
          continue;
        }
      }
      sock->send_head=node->next;
      if(sock->send_head==0) sock->send_tail=0;
      cemon_send_release_cost(sock,node);
      if(cemon_sent_enqueue(sock,node)<0){
        cemon_socket_die(sock,CEMON_ERR_NOBUFS,1);
        return -1;
      }
      continue;
    }
    err=cemon_last_error();
    if(err==EINTR) continue;
    if(rc<0&&cemon_wouldblock(err)) break;
    cemon_socket_die(sock,err,1);
    return -1;
  }
  if(!cemon_socket_is_dead(sock)&&sock->kind==CEMON_TCP_SOCK){
    /* finish may free the socket; its return value says whether it is gone */
    if(cemon_tcp_finish(sock)!=0) return -1;
  }
  if(!cemon_socket_is_dead(sock)&&cemon_unix_watch(sock)<0){
    cemon_socket_die(sock,cemon_last_error(),1);
    return -1;
  }
  return 0;
}
static int cemon_unix_accept_retry_listener(cemon_socket *sock,int err){
  if(!cemon_unix_accept_retry_error(err)) return 0;
  if(cemon_accept_retry_queue(sock)<0) cemon_socket_die(sock,err,1);
  return 1;
}
static void cemon_unix_accept(cemon_socket *sock){
  for(;;){
    cemon_socklen len;
    cemon_fd fd;
    cemon_socket *peer;
    cemon_addr addr;
    int err;
    len=(cemon_socklen)sizeof(addr.data);
    fd=accept(sock->fd,(struct sockaddr*)addr.data,&len);
    if(fd==CEMON_BAD_FD){
      err=cemon_last_error();
      if(err==EAGAIN||err==EWOULDBLOCK) return;
      if(err==EINTR||cemon_unix_accept_soft_error(err)) continue;
      if(cemon_unix_accept_retry_listener(sock,err)) return;
      cemon_socket_die(sock,err,1);
      return;
    }
    if(cemon_set_nonblock(fd)<0){
      err=cemon_last_error();
      cemon_fd_close(fd);
      if(cemon_unix_accept_retry_listener(sock,err)) return;
      continue;
    }
    if(cemon_set_nosigpipe(fd)<0){
      err=cemon_last_error();
      cemon_fd_close(fd);
      if(cemon_unix_accept_retry_listener(sock,err)) return;
      continue;
    }
    cemon_set_nodelay(fd);
    peer=cemon_sock_new(sock->loop,fd,CEMON_TCP_SOCK,sock->fn,sock->ud);
    if(peer==0){
      err=ENOMEM;
      cemon_fd_close(fd);
      if(cemon_unix_accept_retry_listener(sock,err)) return;
      continue;
    }
    if(cemon_unix_watch(peer)<0){
      err=cemon_last_error();
      cemon_socket_die(peer,0,0);
      if(cemon_unix_accept_retry_listener(sock,err)) return;
      continue;
    }
    memset(&addr.data[len],0,sizeof(addr.data)-len);
    addr.len=(int)len;
    err=cemon_emit_blockable(sock,CEMON_ACCEPT,0,peer,0,&addr);
    if(err==CEMON_EMIT_SKIP){
      cemon_socket_die(peer,0,0);
      return;
    }
    if(err!=CEMON_EMIT_CONTINUE) return;
  }
}
static int cemon_accept_retry_resume(cemon_socket *sock){
  if(cemon_unix_watch(sock)<0) return -1;
  cemon_unix_accept(sock);
  return cemon_socket_is_dead(sock)?-1:0;
}
static void cemon_unix_recv(cemon_socket *sock){
  int rc,err;
  if(!cemon_recv_active(sock)) return;
  for(;;){
    rc=recv(sock->fd,sock->recv_buf,sizeof(sock->recv_buf),0);
    if(rc>0){
      int emit_rc;
      sock->recv_armed=0;
      emit_rc=cemon_emit_blockable(sock,CEMON_DATA,0,sock->recv_buf,rc,0);
      if(emit_rc==CEMON_EMIT_SKIP||emit_rc!=CEMON_EMIT_CONTINUE) return;
      continue;
    }
    if(rc==0){
      sock->recv_armed=0;
      cemon_tcp_note_eof(sock);
      if(cemon_emit_terminal(sock,CEMON_EOF,0,0,0,0)==CEMON_EMIT_CONTINUE) cemon_tcp_finish(sock);
      return;
    }
    err=cemon_last_error();
    if(err==EINTR) continue;
    if(cemon_wouldblock(err)) return;
    cemon_socket_die(sock,err,1);
    return;
  }
}
static void cemon_unix_recvfrom(cemon_socket *sock){
  cemon_addr addr;
  cemon_socklen len;
  int rc,err;
  if(!cemon_recv_active(sock)) return;
  if(sock->udp_recv_buf==0){
    cemon_socket_die(sock,CEMON_ERR_NOBUFS,1);
    return;
  }
  for(;;){
    memset(&addr,0,sizeof(addr));
    len=(cemon_socklen)sizeof(addr.data);
    rc=(int)recvfrom(sock->fd,sock->udp_recv_buf,(size_t)CEMON_UDP_BUF,0,(struct sockaddr*)addr.data,&len);
    if(rc>=0){
      sock->recv_armed=0;
      addr.len=(int)len;
      int emit_rc=cemon_emit_blockable(sock,CEMON_DATA,0,sock->udp_recv_buf,rc,&addr);
      if(emit_rc==CEMON_EMIT_SKIP||emit_rc!=CEMON_EMIT_CONTINUE) return;
      continue;
    }
    err=cemon_last_error();
    if(err==EINTR) continue;
    if(cemon_wouldblock(err)||cemon_unix_udp_recv_soft_error(err)) return;
    cemon_socket_die(sock,err,1);
    return;
  }
}
static void cemon_unix_ready(cemon_socket *sock,int rd,int wr,int er){
  if(cemon_socket_is_dead(sock)) return;
  if(er){
    cemon_socket_die(sock,er,1);
    return;
  }
  if(wr&&cemon_socket_is_connecting(sock)){
    int so;
    cemon_socklen sl;
    so=0;
    sl=(cemon_socklen)sizeof(so);
    if(getsockopt(sock->fd,SOL_SOCKET,SO_ERROR,(char*)&so,&sl)<0) so=cemon_last_error();
    if(so!=0){
      cemon_socket_die(sock,so,1);
      return;
    }
    cemon_socket_mark_open(sock);
    cemon_set_nodelay(sock->fd);
    if(cemon_unix_watch(sock)<0){
      cemon_socket_die(sock,cemon_last_error(),1);
      return;
    }
    if(cemon_emit_blockable(sock,CEMON_CONNECT,0,0,0,0)!=CEMON_EMIT_CONTINUE) return;
  }
  if(cemon_socket_is_dead(sock)) return;
  if(wr&&sock->send_head) cemon_unix_flush(sock);
  if(cemon_socket_is_dead(sock)) return;
  if(rd){
    if(sock->kind==CEMON_LISTEN_SOCK) cemon_unix_accept(sock);
    else if(sock->kind==CEMON_UDP_SOCK) cemon_unix_recvfrom(sock);
    else cemon_unix_recv(sock);
  }
}
#endif
static void cemon_socket_die(cemon_socket *sock,int status,int notify){
  cemon *loop;
  if(!cemon_socket_mark_dead(sock)) return;
  loop=sock->loop;
  cemon_accept_retry_cancel(sock);
#if defined(_WIN32)
  if(sock->fd!=CEMON_BAD_FD){
    cemon_fd_close(sock->fd);
    sock->fd=CEMON_BAD_FD;
  }
  if(sock->accept){
    int i;
    for(i=0;i<CEMON_ACCEPT_CREDIT;i++) if(sock->accept->slotv[i].fd!=CEMON_BAD_FD){
      cemon_fd_close(sock->accept->slotv[i].fd);
      sock->accept->slotv[i].fd=CEMON_BAD_FD;
    }
  }
#else
  cemon_unix_unwatch(sock);
  cemon_unix_slot_release(sock);
  if(sock->fd!=CEMON_BAD_FD){
    cemon_fd_close(sock->fd);
    sock->fd=CEMON_BAD_FD;
  }
#endif
  cemon_list_del(loop,sock);
  if(notify) cemon_emit_terminal(sock,CEMON_CLOSED,status,0,0,0);
  cemon_sock_release(sock);
}
static void cemon_begin_stop(cemon *loop){
  int already_stopping;
  if(loop==0) return;
#if defined(_WIN32)
  EnterCriticalSection(&loop->post_lock);
#else
  pthread_mutex_lock(&loop->post_lock);
#endif
  already_stopping=cemon_loop_transition_post_bits_locked(loop,CEMON_LOOP_LIFECYCLE_TRANSITION_BEGIN_STOP)<=0;
#if defined(_WIN32)
  LeaveCriticalSection(&loop->post_lock);
#else
  pthread_mutex_unlock(&loop->post_lock);
#endif
  if(already_stopping) return;
  loop->stop_begin_count++;
  cemon_timer_clear(loop);
  while(loop->sock_head){
    loop->stop_socket_close_count++;
    cemon_socket_die(loop->sock_head,0,1);
  }
}
static int cemon_stop_ready(cemon *loop){
  int posts;
  if(loop->sock_total!=0) return 0;
#if defined(_WIN32)
  EnterCriticalSection(&loop->post_lock);
  posts=loop->post_count;
  LeaveCriticalSection(&loop->post_lock);
#else
  pthread_mutex_lock(&loop->post_lock);
  posts=loop->post_count;
  pthread_mutex_unlock(&loop->post_lock);
#endif
  return posts==0;
}
static int cemon_wake_prepare_locked(cemon *loop){
  if(loop==0||loop->wake_pending) return 0;
  loop->wake_pending=1;
  return 1;
}
static void cemon_wake_emit(cemon *loop,int do_wake){
  if(loop==0||!do_wake) return;
#if defined(_WIN32)
  if(PostQueuedCompletionStatus(loop->port,0,CEMON_WAKE_KEY,0)==0){
    EnterCriticalSection(&loop->post_lock);
    if(loop->wake_pending) loop->wake_pending=0;
    LeaveCriticalSection(&loop->post_lock);
  }
#elif defined(__linux__)
  {
    unsigned long long one=1;
    int fd=loop->wake_fd[CEMON_WAKE_READ];
    int err;
    for(;;){
      if((int)write(fd,&one,sizeof(one))==(int)sizeof(one)) break;
      err=cemon_last_error();
      if(err==EINTR) continue;
      if(!cemon_wouldblock(err)){
        pthread_mutex_lock(&loop->post_lock);
        loop->wake_pending=0;
        pthread_mutex_unlock(&loop->post_lock);
      }
      break;
    }
  }
#else
  {
    struct kevent ev;
    EV_SET(&ev,CEMON_KQ_WAKE_ID,EVFILT_USER,0,NOTE_TRIGGER,0,0);
    while(kevent(loop->fd,&ev,1,0,0,0)<0&&cemon_last_error()==EINTR){}
  }
#endif
}
static void cemon_post_take(cemon *loop,cemon_post_node **head_out,cemon_post_node **tail_out,int consume_wake){
  if(head_out) *head_out=0;
  if(tail_out) *tail_out=0;
  if(loop==0) return;
#if defined(_WIN32)
  EnterCriticalSection(&loop->post_lock);
  if(consume_wake) loop->wake_pending=0;
#else
  pthread_mutex_lock(&loop->post_lock);
  if(consume_wake) loop->wake_pending=0;
#endif
  if(loop->post_head){
    if(head_out) *head_out=loop->post_head;
    if(tail_out) *tail_out=loop->post_tail;
    loop->post_head=0;
    loop->post_tail=0;
  }
#if defined(_WIN32)
  LeaveCriticalSection(&loop->post_lock);
#else
  pthread_mutex_unlock(&loop->post_lock);
#endif
}
static void cemon_post_release(cemon_post_node *node){
  if(node==0) return;
  if(node->storage) CEMON_FREE(node->storage);
  else CEMON_FREE(node);
}
static void cemon_post_finish(cemon *loop,int done,cemon_post_node *node,cemon_post_node *tail){
  if(loop==0) return;
  if(done||node){
#if defined(_WIN32)
    EnterCriticalSection(&loop->post_lock);
#else
    pthread_mutex_lock(&loop->post_lock);
#endif
    if(loop->post_count>=(unsigned int)done) loop->post_count-=(unsigned int)done;
    else loop->post_count=0;
    if(node){
      tail->next=loop->post_head;
      loop->post_head=node;
      if(loop->post_tail==0) loop->post_tail=tail;
    }
#if defined(_WIN32)
    LeaveCriticalSection(&loop->post_lock);
#else
    pthread_mutex_unlock(&loop->post_lock);
#endif
  }
}
static void cemon_drain_posts(cemon *loop,int *budget_io,int consume_wake){
  cemon_post_node *node,*head=0,*tail=0;
  int done=0;
  int budget=budget_io?*budget_io:CEMON_DISPATCH_BUDGET;
  cemon_post_take(loop,&head,&tail,consume_wake);
  node=head;
  while(node&&budget>0){
    cemon_post_node *next;
    next=node->next;
    loop->callback_depth++;
    node->fn(loop,node->ud);
    loop->callback_depth--;
    cemon_post_release(node);
    node=next;
    done++;
    budget--;
  }
  if(budget_io) *budget_io=budget;
  cemon_post_finish(loop,done,node,tail);
}
static int cemon_enqueue_send(cemon_socket *sock,const void *buf,int len,const cemon_addr *to,int send_class){
  cemon *loop;
  cemon_send_node *node;
  unsigned int cost,loop_limit;
  int addr_len;
#ifndef _WIN32
  int was_empty;
#endif
  if(!cemon_send_request_valid(sock)||buf==0||len<=0||!cemon_socket_admit(sock,CEMON_LOOP_ADMISSION_OWNER_ONLY)) return -1;   /* owner-thread only: see cemon_bind_owner */
  if(to&&(to->len<=0||to->len>(int)sizeof(to->data))) return -1;
  loop=sock->loop;
  addr_len=to?to->len:0;
  cost=(unsigned int)(sizeof(*node)+(size_t)len+(size_t)addr_len);
  loop_limit=cemon_loop_send_limit(send_class);
  if(cost>CEMON_SENDQ_LIMIT||sock->send_cost>CEMON_SENDQ_LIMIT-cost||cost>loop_limit||loop->send_cost>loop_limit-cost) return -1;
#ifndef _WIN32
  was_empty=sock->send_head==0;
#endif
  node=(cemon_send_node*)CEMON_MALLOC(sizeof(*node)+(size_t)len+(size_t)addr_len);
  if(node==0) return -1;
  memset(node,0,sizeof(*node));
  node->owner=sock;
  node->cost=cost;
  node->buf=(char*)(node+1);
  memcpy(node->buf,buf,(size_t)len);
  node->len=len;
  if(to){
    node->addr_len=addr_len;
    memcpy(node->buf+len,to->data,(size_t)addr_len);
  }
  node->send_class=send_class;
  if(send_class==CEMON_SEND_CLASS_CONTROL&&sock->send_head){
    /* Control frames are inserted right behind the frame currently being written
       instead of at the tail.  The send queue is a byte-ordered FIFO, so tail-
       queueing a heartbeat behind bulk traffic delays it by the WHOLE queue:
       measured, 1 MB of queued bulk held the next control frame for 380 ms at
       ~2.8 MB/s (its first byte arrived at stream offset 1048576), which is beyond
       the 250 ms minimum election timeout - a spurious election on a slow link.
       With the insert the delay is bounded by one frame.  O(1): only the head's
       link changes, and send_tail moves only when the head was the tail. */
    if(sock->send_tail==sock->send_head) sock->send_tail=node;
    node->next=sock->send_head->next;
    sock->send_head->next=node;
  }else{
    if(sock->send_tail) sock->send_tail->next=node;
    else sock->send_head=node;
    sock->send_tail=node;
  }
  sock->send_cost+=cost;
  loop->send_cost+=cost;
  if(loop->send_cost>loop->send_cost_peak) loop->send_cost_peak=loop->send_cost;
#if defined(_WIN32)
  if(cemon_win_post_send(sock)<0){
    int send_err=cemon_last_error();
    cemon_socket_die(sock,send_err,1);
    return -1;
  }
#else
  if(was_empty&&cemon_unix_flush(sock)<0) return -1;
#endif
  return 0;
}
CEMON_DEF cemon *cemon_create(void){
  cemon *loop=(cemon*)CEMON_MALLOC(sizeof(*loop));
  if(loop==0) return 0;
  memset(loop,0,sizeof(*loop));
#if defined(_WIN32)
  {
    WSADATA wd;
    if(WSAStartup(MAKEWORD(2,2),&wd)!=0){
      CEMON_FREE(loop);
      return 0;
    }
  }
  InitializeCriticalSection(&loop->post_lock);
  loop->port=CreateIoCompletionPort(INVALID_HANDLE_VALUE,0,0,0);
  if(loop->port==0){
    cemon_destroy(loop);
    return 0;
  }
#else
  loop->fd=CEMON_BAD_FD;
  loop->wake_fd[CEMON_WAKE_READ]=CEMON_BAD_FD;
  loop->wake_fd[CEMON_WAKE_WRITE]=CEMON_BAD_FD;
  loop->unix_slot_free=-1;
  pthread_mutex_init(&loop->post_lock,0);
#ifdef CEMON_USE_KQUEUE
  loop->fd=kqueue();
#else
  loop->fd=epoll_create(32);
#endif
  if(loop->fd<0){
    cemon_destroy(loop);
    return 0;
  }
#if defined(__linux__)
  {
    int fd=eventfd(0,EFD_NONBLOCK);
    if(fd<0){
      cemon_destroy(loop);
      return 0;
    }
    loop->wake_fd[CEMON_WAKE_READ]=fd;
    loop->wake_fd[CEMON_WAKE_WRITE]=CEMON_BAD_FD;
  }
#endif
#ifdef CEMON_USE_KQUEUE
  {
    struct kevent ev;
    EV_SET(&ev,CEMON_KQ_WAKE_ID,EVFILT_USER,EV_ADD|EV_ENABLE|EV_CLEAR,0,0,0);
    if(kevent(loop->fd,&ev,1,0,0,0)<0){
      cemon_destroy(loop);
      return 0;
    }
  }
#else
  {
    struct epoll_event ev;
    memset(&ev,0,sizeof(ev));
    ev.events=EPOLLIN;
    ev.data.ptr=0;
    if(epoll_ctl(loop->fd,EPOLL_CTL_ADD,loop->wake_fd[CEMON_WAKE_READ],&ev)<0){
      cemon_destroy(loop);
      return 0;
    }
  }
#endif
#endif
  return loop;
}
CEMON_DEF int cemon_bind_owner(cemon *loop){
  int rc;
  if(loop==0) return -1;
#if defined(_WIN32)
  EnterCriticalSection(&loop->post_lock);
#else
  pthread_mutex_lock(&loop->post_lock);
#endif
  if((loop->control.post_bits&CEMON_LOOP_POST_OWNER_BOUND)==0){
    cemon_bind_owner_locked(loop);
    rc=0;
  }else{
    rc=cemon_owner_ok_locked(loop)?0:-1;
  }
#if defined(_WIN32)
  LeaveCriticalSection(&loop->post_lock);
#else
  pthread_mutex_unlock(&loop->post_lock);
#endif
  return rc;
}
CEMON_DEF int cemon_is_owner(cemon *loop){
  int rc;
  if(loop==0) return -1;
#if defined(_WIN32)
  EnterCriticalSection(&loop->post_lock);
#else
  pthread_mutex_lock(&loop->post_lock);
#endif
  rc=cemon_owner_ok_locked(loop)?1:0;
#if defined(_WIN32)
  LeaveCriticalSection(&loop->post_lock);
#else
  pthread_mutex_unlock(&loop->post_lock);
#endif
  return rc;
}
CEMON_DEF int cemon_destroy(cemon *loop){
  cemon_post_node *post;
  cemon_socket *sock,*next;
  if(loop==0||!cemon_loop_admit(loop,CEMON_LOOP_ADMISSION_OWNER_ONLY)) return -1;
  if(loop->callback_depth>0){
    cemon_stop(loop);
    return 0;
  }
  cemon_stop(loop);
  cemon_ingress_close(loop);
  sock=loop->sock_head;
  while(sock){
    next=sock->next;
    cemon_socket_die(sock,0,0);
    sock=next;
  }
  cemon_clear_sent(loop);
#if defined(_WIN32)
  /* Drain the remaining completions with a BOUNDED wait: a missing completion (an
     operation whose socket was already closed, a socket that never entered the
     list, ...) used to block destroy on INFINITE forever.  On timeout the drain
     gives up; the IOCP handle is closed a few lines below, so no completion
     handler can run afterwards and touch freed state. */
  while(loop->sock_total>0){
    DWORD bytes;
    ULONG_PTR key=0;   /* GQCS leaves it untouched on failure; an uninitialised read could look like a wake */
    OVERLAPPED *ov;
    BOOL ok=GetQueuedCompletionStatus(loop->port,&bytes,&key,&ov,2000);
    if(!ok&&ov==0) break;                     /* timed out: do not hang destroy */
    if(ov) cemon_win_handle((cemon_win_op*)ov,bytes,ok?0:(int)GetLastError());
  }
#endif
  post=loop->post_head;
  while(post){
    cemon_post_node *next_post=post->next;
    cemon_post_release(post);
    post=next_post;
  }
  cemon_timer_clear(loop);
  if(loop->timers) CEMON_FREE(loop->timers);
#if defined(_WIN32)
  if(loop->port) CloseHandle(loop->port);
  DeleteCriticalSection(&loop->post_lock);
  WSACleanup();
#else
  if(loop->fd!=CEMON_BAD_FD) close(loop->fd);
  if(loop->wake_fd[CEMON_WAKE_READ]!=CEMON_BAD_FD) close(loop->wake_fd[CEMON_WAKE_READ]);
  if(loop->wake_fd[CEMON_WAKE_WRITE]!=CEMON_BAD_FD) close(loop->wake_fd[CEMON_WAKE_WRITE]);
  if(loop->unix_slots) CEMON_FREE(loop->unix_slots);
  pthread_mutex_destroy(&loop->post_lock);
#endif
  CEMON_FREE(loop);
  return 0;
}
CEMON_DEF void cemon_stop(cemon *loop){
  int active,owner,wake;
  if(loop==0) return;
#if defined(_WIN32)
  EnterCriticalSection(&loop->post_lock);
#else
  pthread_mutex_lock(&loop->post_lock);
#endif
  owner=cemon_owner_ok_locked(loop);
  cemon_loop_control_flags_locked(loop,&active,0,0);
  cemon_loop_transition_post_bits_locked(loop,CEMON_LOOP_LIFECYCLE_TRANSITION_STOP_REQUEST);
  wake=cemon_stop_request_wake_needed(owner,active)?cemon_wake_prepare_locked(loop):0;
#if defined(_WIN32)
  LeaveCriticalSection(&loop->post_lock);
#else
  pthread_mutex_unlock(&loop->post_lock);
#endif
  cemon_wake_emit(loop,wake);
}
CEMON_DEF int cemon_should_stop(cemon *loop){
  return cemon_stop_pending(loop);
}
/* Stable loop algebra: runtime flags collapse into one semantic phase. */
static int cemon_loop_phase_from_flags(int running,int stop,int stopping){
  static const signed char phase_by_flags[2][2][2]={{
    { CEMON_LOOP_PHASE_READY, CEMON_LOOP_PHASE_STOPPED },
    { CEMON_LOOP_PHASE_STOP_REQUESTED, CEMON_LOOP_PHASE_STOPPED }
  },{
    { CEMON_LOOP_PHASE_RUNNING, CEMON_LOOP_PHASE_DRAINING },
    { CEMON_LOOP_PHASE_STOP_REQUESTED, CEMON_LOOP_PHASE_DRAINING }
  }};
  return phase_by_flags[running!=0][stop!=0][stopping!=0];
}
static int cemon_loop_phase_from_control(unsigned int post_bits){
  int running=(post_bits&CEMON_LOOP_POST_RUNNING)!=0;
  int stop=(post_bits&CEMON_LOOP_POST_STOP_REQUESTED)!=0;
  int stopping=(post_bits&CEMON_LOOP_POST_STOPPING)!=0;
  if((post_bits&CEMON_LOOP_POST_CLOSING)!=0) return CEMON_LOOP_PHASE_CLOSING;
  return cemon_loop_phase_from_flags(running,stop,stopping);
}
static int cemon_loop_note_run_enter(cemon *loop){
  int ok;
  if(loop==0) return -1;
#if defined(_WIN32)
  EnterCriticalSection(&loop->post_lock);
#else
  pthread_mutex_lock(&loop->post_lock);
#endif
  ok=cemon_loop_transition_post_bits_locked(loop,CEMON_LOOP_LIFECYCLE_TRANSITION_RUN_ENTER)>=0;
#if defined(_WIN32)
  LeaveCriticalSection(&loop->post_lock);
#else
  pthread_mutex_unlock(&loop->post_lock);
#endif
  return ok?0:-1;
}
static void cemon_loop_note_run_exit(cemon *loop){
  if(loop==0) return;
#if defined(_WIN32)
  EnterCriticalSection(&loop->post_lock);
#else
  pthread_mutex_lock(&loop->post_lock);
#endif
  cemon_loop_transition_post_bits_locked(loop,CEMON_LOOP_LIFECYCLE_TRANSITION_RUN_EXIT);
#if defined(_WIN32)
  LeaveCriticalSection(&loop->post_lock);
#else
  pthread_mutex_unlock(&loop->post_lock);
#endif
}
typedef struct cemon_socket_stream_semantic_spec{
  int phase;
  int close_progress;
} cemon_socket_stream_semantic_spec;
/* Stable stream algebra: implementation tcp_phase collapses into one semantic
  phase plus one close-progress value.
  - READ_EOF -> TERMINATING + PEER_EOF
  - WRITE_DRAIN / WRITE_CLOSED -> TERMINATING + LOCAL_SHUTDOWN
  - READ_EOF_WRITE_DRAIN / HALF_CLOSED -> TERMINATING + BOTH */
static const cemon_socket_stream_semantic_spec cemon_socket_stream_semantic_table[CEMON_TCP_PHASE_COUNT]={
  { CEMON_SOCKET_PHASE_OPEN, CEMON_SOCKET_CLOSE_PROGRESS_NONE },
  { CEMON_SOCKET_PHASE_TERMINATING, CEMON_SOCKET_CLOSE_PROGRESS_PEER_EOF },
  { CEMON_SOCKET_PHASE_TERMINATING, CEMON_SOCKET_CLOSE_PROGRESS_LOCAL_SHUTDOWN },
  { CEMON_SOCKET_PHASE_TERMINATING, CEMON_SOCKET_CLOSE_PROGRESS_LOCAL_SHUTDOWN },
  { CEMON_SOCKET_PHASE_TERMINATING, CEMON_SOCKET_CLOSE_PROGRESS_BOTH },
  { CEMON_SOCKET_PHASE_TERMINATING, CEMON_SOCKET_CLOSE_PROGRESS_BOTH }
};
static const cemon_socket_stream_semantic_spec *cemon_socket_stream_semantic_view(cemon_socket *sock){
  int tcp_phase;
  if(sock==0||sock->kind!=CEMON_TCP_SOCK) return 0;
  tcp_phase=sock->tcp_phase;
  if(tcp_phase<CEMON_TCP_PHASE_OPEN||tcp_phase>=CEMON_TCP_PHASE_COUNT) tcp_phase=CEMON_TCP_PHASE_OPEN;
  return &cemon_socket_stream_semantic_table[tcp_phase];
}
static int cemon_socket_role_view(cemon_socket *sock){
  if(sock==0) return 0;
  if(sock->kind==CEMON_LISTEN_SOCK) return CEMON_SOCKET_ROLE_LISTENER;
  if(sock->kind==CEMON_UDP_SOCK) return CEMON_SOCKET_ROLE_DATAGRAM;
  return CEMON_SOCKET_ROLE_STREAM;
}
static int cemon_socket_phase_view(cemon_socket *sock){
  const cemon_socket_stream_semantic_spec *spec;
  if(sock==0||cemon_socket_is_dead(sock)) return CEMON_SOCKET_PHASE_CLOSED;
  if(sock->kind!=CEMON_TCP_SOCK) return CEMON_SOCKET_PHASE_OPEN;
  if(sock->state==CEMON_CONNECTING_STATE) return CEMON_SOCKET_PHASE_CONNECTING;
  spec=cemon_socket_stream_semantic_view(sock);
  return spec?spec->phase:CEMON_SOCKET_PHASE_OPEN;
}
static int cemon_socket_close_progress_view(cemon_socket *sock){
  const cemon_socket_stream_semantic_spec *spec;
  if(sock==0||cemon_socket_is_dead(sock)||sock->kind!=CEMON_TCP_SOCK||sock->state==CEMON_CONNECTING_STATE) return CEMON_SOCKET_CLOSE_PROGRESS_NONE;
  spec=cemon_socket_stream_semantic_view(sock);
  return spec?spec->close_progress:CEMON_SOCKET_CLOSE_PROGRESS_NONE;
}
static int cemon_socket_delivery_view(cemon_socket *sock){
  if(sock&&sock->loop&&cemon_loop_mailbox_state(sock->loop)!=CEMON_LOOP_MAILBOX_OPEN) return CEMON_SOCKET_DELIVERY_STOP_SUPPRESSED;
  return CEMON_SOCKET_DELIVERY_NORMAL;
}
static unsigned int cemon_socket_capability_mask_view(cemon_socket *sock){
  unsigned int mask=0;
  int admit_open=cemon_socket_admit(sock,CEMON_LOOP_ADMISSION_OPEN);
  if(admit_open&&cemon_recv_request_valid(sock,sock&&sock->kind==CEMON_UDP_SOCK)) mask|=CEMON_SOCKET_CAP_RECV;
  if(admit_open&&cemon_send_request_valid(sock)) mask|=CEMON_SOCKET_CAP_SEND;
  if(admit_open&&cemon_tcp_shutdown_admissible(sock)) mask|=CEMON_SOCKET_CAP_SHUTDOWN;
  return mask;
}
static unsigned int cemon_socket_send_queue_depth(cemon_socket *sock){
  unsigned int count=0;
  if(sock){
    cemon_send_node *node;
    for(node=sock->send_head;node;node=node->next) count++;
  }
  return count;
}
static void cemon_inspect_copy_loop_semantics(cemon_stats *out,unsigned int post_bits){
  if(out==0) return;
  out->loop_phase=cemon_loop_phase_from_control(post_bits);
  out->loop_mailbox=cemon_loop_mailbox_from_post_bits(post_bits);
  out->loop_ownership=cemon_loop_ownership_from_post_bits(post_bits);
}
static void cemon_inspect_copy_loop_post_diagnostics_locked(cemon *loop,cemon_stats *out){
  if(loop==0||out==0) return;
  out->loop_sock_total=loop->sock_total;
  out->loop_timer_count=loop->timer_count;
  out->loop_send_cost=loop->send_cost;
  out->loop_send_cost_peak=loop->send_cost_peak;
  out->loop_post_count=loop->post_count;
  out->loop_post_count_peak=loop->post_count_peak;
  out->loop_blocked_accept_count=loop->blocked_accept_count;
  out->loop_blocked_connect_count=loop->blocked_connect_count;
  out->loop_blocked_data_count=loop->blocked_data_count;
  out->loop_blocked_sent_count=loop->blocked_sent_count;
  out->loop_terminal_eof_count=loop->terminal_eof_count;
  out->loop_terminal_close_count=loop->terminal_close_count;
  out->loop_stop_begin_count=loop->stop_begin_count;
  out->loop_stop_socket_close_count=loop->stop_socket_close_count;
  out->loop_accept_retry_queue_count=loop->accept_retry_queue_count;
  out->loop_accept_retry_run_count=loop->accept_retry_run_count;
}
static void cemon_inspect_copy_socket_semantics(cemon_socket *sock,cemon_stats *out){
  if(sock==0||out==0) return;
  out->socket_role=cemon_socket_role_view(sock);
  out->socket_phase=cemon_socket_phase_view(sock);
  out->socket_close_progress=cemon_socket_close_progress_view(sock);
  out->socket_delivery=cemon_socket_delivery_view(sock);
  out->socket_capability_mask=cemon_socket_capability_mask_view(sock);
}
static void cemon_inspect_copy_socket_diagnostics(cemon_socket *sock,cemon_stats *out){
  if(sock==0||out==0) return;
  out->socket_kind=sock->kind;
  out->socket_state=sock->state;
  out->socket_tcp_phase=sock->tcp_phase;
  out->socket_recv_armed=sock->recv_armed;
  out->socket_shutdown_pending=cemon_tcp_shutdown_pending(sock);
  out->socket_write_closed=cemon_tcp_is_write_closed(sock);
  out->socket_write_shutdown_needed=cemon_tcp_shutdown_write_needed(sock);
  out->socket_eof_seen=cemon_tcp_has_eof(sock);
  out->socket_close_ready=cemon_tcp_finish_closes(sock);
  out->socket_send_queue_count=cemon_socket_send_queue_depth(sock);
  out->socket_send_cost=sock->send_cost;
}
CEMON_DEF int cemon_inspect(cemon *loop,cemon_socket *sock,cemon_stats *out){
  unsigned int post_bits;
  if(out==0) return -1;
  if(sock&&loop==0) loop=sock->loop;
  if(sock==0&&loop==0) return -1;
  if(sock&&sock->loop!=loop) return -1;
  if(loop==0||!cemon_loop_admit(loop,CEMON_LOOP_ADMISSION_OWNER_ONLY)) return -1;
  memset(out,0,sizeof(*out));
  post_bits=0;
#if defined(_WIN32)
  EnterCriticalSection(&loop->post_lock);
#else
  pthread_mutex_lock(&loop->post_lock);
#endif
  post_bits=loop->control.post_bits;
  cemon_inspect_copy_loop_post_diagnostics_locked(loop,out);
#if defined(_WIN32)
  LeaveCriticalSection(&loop->post_lock);
#else
  pthread_mutex_unlock(&loop->post_lock);
#endif
  cemon_inspect_copy_loop_semantics(out,post_bits);
  cemon_inspect_copy_socket_semantics(sock,out);
  cemon_inspect_copy_socket_diagnostics(sock,out);
  return 0;
}
static int cemon_progress_stop(cemon *loop){
  if(!cemon_should_stop(loop)) return 0;
  cemon_begin_stop(loop);
  return cemon_stop_ready(loop);
}
#if defined(_WIN32)
/* One owner iteration drains up to CEMON_WIN_POLL_BATCH completions (the POSIX
   paths already batch CEMON_UNIX_POLL_BATCH events per poll).  The owner's
   downstream work - one Raft advance and one Ready bundle - costs far more than
   a completion, and one receive completion carries at most CEMON_BUF bytes, so
   without draining this loop ran once per chunk of every large frame.
   GetQueuedCompletionStatusEx would be fewer syscalls but is Windows Vista+;
   the non-blocking drain below is the XP-compatible way to get the batching.
   The first call keeps the blocking wait so an idle loop still sleeps. */
static int cemon_poll_once(cemon *loop,int timeout_ms){
  DWORD bytes;
  ULONG_PTR key;
  OVERLAPPED *ov;
  int timeout;
  BOOL ok;
  int err;
  int n=0;
  timeout=cemon_timeout(loop);
  if(timeout_ms>=0&&(timeout<0||timeout_ms<timeout)) timeout=timeout_ms;
  for(;;){
    DWORD wait_ms;
    wait_ms=n?0u:((timeout<0)?INFINITE:(DWORD)timeout);
    ok=GetQueuedCompletionStatus(loop->port,&bytes,&key,&ov,wait_ms);
    err=ok?0:(int)GetLastError();
    if(ov==0){
      /* No completion: either the wait timed out (queue empty) or the packet was
         an owner wake / a failed dequeue. */
      if(err==WAIT_TIMEOUT) break;
      if(!ok&&key!=CEMON_WAKE_KEY&&cemon_should_stop(loop)==0) return -1;
      cemon_handle_owner_wake(loop,1);
      break;
    }
    cemon_win_handle((cemon_win_op*)ov,bytes,err);
    n++;
    if(n>=CEMON_WIN_POLL_BATCH) break;
  }
  return 0;
}
#else
#ifdef CEMON_USE_KQUEUE
static int cemon_poll_once(cemon *loop,int timeout_ms){
  struct kevent evs[CEMON_UNIX_POLL_BATCH];
  struct timespec ts;
  struct timespec *pts;
  cemon_socket *held[CEMON_UNIX_POLL_BATCH];
  int i,n,timeout;
  memset(held,0,sizeof(held));
  timeout=cemon_timeout(loop);
  if(timeout_ms>=0&&(timeout<0||timeout_ms<timeout)) timeout=timeout_ms;
  pts=0;
  if(timeout>=0){
    ts.tv_sec=timeout/1000;
    ts.tv_nsec=(timeout%1000)*1000000L;
    pts=&ts;
  }
  n=kevent(loop->fd,0,0,evs,CEMON_UNIX_POLL_BATCH,pts);
  if(n<0){
    if(errno==EINTR) return 0;
    return -1;
  }
  for(i=0;i<n;i++){
    if(evs[i].filter==EVFILT_USER) continue;
    held[i]=cemon_unix_event_sock(loop,(unsigned long long)(size_t)evs[i].udata);
    if(held[i]) cemon_sock_hold(held[i]);
  }
  for(i=0;i<n;i++){
    if(evs[i].filter==EVFILT_USER){
      cemon_handle_owner_wake(loop,1);
    }else{
      cemon_socket *sock2=held[i];
      if(sock2){
        int rd=evs[i].filter==EVFILT_READ;
        int wr=evs[i].filter==EVFILT_WRITE;
        int er=(evs[i].flags&EV_ERROR)?(int)evs[i].data:0;
        cemon_unix_ready(sock2,rd,wr,er);
        cemon_sock_release(sock2);
      }
    }
  }
  return 0;
}
#else
static int cemon_poll_once(cemon *loop,int timeout_ms){
  struct epoll_event evs[CEMON_UNIX_POLL_BATCH];
  unsigned long long keys[CEMON_UNIX_POLL_BATCH];
  cemon_socket *held[CEMON_UNIX_POLL_BATCH];
  int i,n,timeout;
  memset(held,0,sizeof(held));
  timeout=cemon_timeout(loop);
  if(timeout_ms>=0&&(timeout<0||timeout_ms<timeout)) timeout=timeout_ms;
  n=epoll_wait(loop->fd,evs,CEMON_UNIX_POLL_BATCH,timeout);
  if(n<0) return (errno==EINTR)?0:-1;
  for(i=0;i<n;i++){
    keys[i]=evs[i].data.u64;
    if(keys[i]){
      held[i]=cemon_unix_event_sock(loop,keys[i]);
      if(held[i]) cemon_sock_hold(held[i]);
    }
  }
  for(i=0;i<n;i++){
    if(keys[i]){
      cemon_socket *sock2=held[i];
      if(sock2){
        int rd=(evs[i].events&(EPOLLIN|EPOLLRDHUP))!=0;
        int wr=(evs[i].events&EPOLLOUT)!=0;
        int er=0;
        if(evs[i].events&(EPOLLERR|EPOLLHUP)){
          er=cemon_sock_error(sock2);
          if(er==0&&!(evs[i].events&(EPOLLIN|EPOLLOUT|EPOLLRDHUP))) er=1;
        }
        cemon_unix_ready(sock2,rd,wr,er);
        cemon_sock_release(sock2);
      }
    }else cemon_handle_owner_wake(loop,1);
  }
  return 0;
}
#endif
#endif
CEMON_DEF int cemon_poll(cemon *loop,int timeout_ms){
  int budget;
  if(loop==0||!cemon_loop_admit(loop,CEMON_LOOP_ADMISSION_OWNER_ONLY)) return -1;
  /* Idempotent enter: only the first call transitions into RUNNING;
    subsequent calls find the loop already RUNNING and skip. */
  cemon_loop_note_run_enter(loop);
  budget=CEMON_DISPATCH_BUDGET;
  cemon_dispatch_owner(loop,&budget,0,1);
  if(cemon_progress_stop(loop)||cemon_poll_once(loop,timeout_ms)<0){
    budget=CEMON_DISPATCH_BUDGET;
    cemon_dispatch_owner(loop,&budget,0,0);
    cemon_loop_note_run_exit(loop);
    return -1;
  }
  return 0;
}
CEMON_DEF cemon_socket *cemon_tcp_listen(cemon *loop,const char *host,unsigned short port,cemon_io_fn fn,void *ud){
  cemon_addr addr;
  cemon_fd fd;
  cemon_socket *sock;
  int family;
  if(loop==0||!cemon_loop_admit(loop,CEMON_LOOP_ADMISSION_OPEN)||cemon_numeric_addr(&addr,host,port,1)<0) return 0;
  family=((struct sockaddr*)addr.data)->sa_family;
#if defined(_WIN32)
  fd=WSASocketA(family,SOCK_STREAM,IPPROTO_TCP,0,0,WSA_FLAG_OVERLAPPED);
#else
  fd=socket(family,SOCK_STREAM,IPPROTO_TCP);
#endif
  if(fd==CEMON_BAD_FD) return 0;
  cemon_set_listen_opts(fd);
#ifndef _WIN32
  if(cemon_set_nosigpipe(fd)<0||cemon_set_nonblock(fd)<0){
    cemon_fd_close(fd);
    return 0;
  }
#endif
  if(bind(fd,(struct sockaddr*)addr.data,(cemon_socklen)addr.len)!=0||listen(fd,CEMON_BACKLOG)!=0){
    cemon_fd_close(fd);
    return 0;
  }
  sock=cemon_sock_new(loop,fd,CEMON_LISTEN_SOCK,fn,ud);
  if(sock==0){
    cemon_fd_close(fd);
    return 0;
  }
#if defined(_WIN32)
  sock->accept->family=family;
  if(cemon_win_load_ext(loop,fd)<0||CreateIoCompletionPort((HANDLE)fd,loop->port,0,0)==0||cemon_win_post_accept(sock)<0){
    cemon_socket_die(sock,cemon_last_error(),0);
    return 0;
  }
#else
  if(cemon_unix_watch(sock)<0){
    cemon_socket_die(sock,cemon_last_error(),0);
    return 0;
  }
#endif
  return sock;
}
CEMON_DEF cemon_socket *cemon_tcp_connect(cemon *loop,const char *host,unsigned short port,cemon_io_fn fn,void *ud){
  cemon_addr addr;
  cemon_fd fd;
  cemon_socket *sock;
  int family;
  if(loop==0||!cemon_loop_admit(loop,CEMON_LOOP_ADMISSION_OPEN)||cemon_numeric_addr(&addr,host,port,0)<0) return 0;
  family=((struct sockaddr*)addr.data)->sa_family;
#if defined(_WIN32)
  fd=WSASocketA(family,SOCK_STREAM,IPPROTO_TCP,0,0,WSA_FLAG_OVERLAPPED);
#else
  fd=socket(family,SOCK_STREAM,IPPROTO_TCP);
#endif
  if(fd==CEMON_BAD_FD) return 0;
#ifndef _WIN32
  if(cemon_set_nosigpipe(fd)<0||cemon_set_nonblock(fd)<0){
    cemon_fd_close(fd);
    return 0;
  }
#endif
  sock=cemon_sock_new(loop,fd,CEMON_TCP_SOCK,fn,ud);
  if(sock==0){
    cemon_fd_close(fd);
    return 0;
  }
  cemon_socket_mark_connecting(sock);
#if defined(_WIN32)
  if(cemon_win_load_ext(loop,fd)<0||CreateIoCompletionPort((HANDLE)fd,loop->port,0,0)==0||cemon_bind_any(fd,family)!=0){
    cemon_socket_die(sock,cemon_last_error(),0);
    return 0;
  }
  if(cemon_win_post_connect(sock,&addr)<0){
    cemon_socket_die(sock,sock->connect?cemon_last_error():12,0);
    return 0;
  }
#else
  if(connect(fd,(struct sockaddr*)addr.data,(cemon_socklen)addr.len)!=0&&!cemon_connect_in_progress(cemon_last_error())){
    cemon_socket_die(sock,cemon_last_error(),0);
    return 0;
  }
  if(cemon_unix_watch(sock)<0){
    cemon_socket_die(sock,cemon_last_error(),0);
    return 0;
  }
#endif
  return sock;
}
CEMON_DEF cemon_socket *cemon_udp_bind(cemon *loop,const char *host,unsigned short port,cemon_io_fn fn,void *ud){
  cemon_addr addr;
  cemon_fd fd;
  cemon_socket *sock;
  int family;
  if(loop==0||!cemon_loop_admit(loop,CEMON_LOOP_ADMISSION_OPEN)||cemon_numeric_addr(&addr,host,port,1)<0) return 0;
  family=((struct sockaddr*)addr.data)->sa_family;
#if defined(_WIN32)
  fd=WSASocketA(family,SOCK_DGRAM,IPPROTO_UDP,0,0,WSA_FLAG_OVERLAPPED);
#else
  fd=socket(family,SOCK_DGRAM,IPPROTO_UDP);
#endif
  if(fd==CEMON_BAD_FD) return 0;
  cemon_set_reuse(fd);
#ifndef _WIN32
  if(cemon_set_nosigpipe(fd)<0||cemon_set_nonblock(fd)<0){
    cemon_fd_close(fd);
    return 0;
  }
#endif
  if(bind(fd,(struct sockaddr*)addr.data,(cemon_socklen)addr.len)!=0){
    cemon_fd_close(fd);
    return 0;
  }
  sock=cemon_sock_new(loop,fd,CEMON_UDP_SOCK,fn,ud);
  if(sock==0){
    cemon_fd_close(fd);
    return 0;
  }
#if defined(_WIN32)
  if(CreateIoCompletionPort((HANDLE)fd,loop->port,0,0)==0||cemon_win_udp_no_reset(fd)<0){
    cemon_socket_die(sock,cemon_last_error(),0);
    return 0;
  }
  if(cemon_win_post_recv(sock)<0){
    cemon_socket_die(sock,(sock->recv&&sock->udp_recv)?cemon_last_error():WSAEINVAL,0);
    return 0;
  }
#else
  if(cemon_unix_watch(sock)<0){
    cemon_socket_die(sock,cemon_last_error(),0);
    return 0;
  }
#endif
  return sock;
}
CEMON_DEF int cemon_send(cemon_socket *sock,const void *buf,int len){
  return cemon_enqueue_send(sock,buf,len,0,CEMON_SEND_CLASS_BULK);
}
CEMON_DEF int cemon_sendto(cemon_socket *sock,const void *buf,int len,const cemon_addr *to){
  return cemon_enqueue_send(sock,buf,len,to,CEMON_SEND_CLASS_BULK);
}
CEMON_DEF int cemon_send_control(cemon_socket *sock,const void *buf,int len){
  return cemon_enqueue_send(sock,buf,len,0,CEMON_SEND_CLASS_CONTROL);
}
CEMON_DEF int cemon_sendto_control(cemon_socket *sock,const void *buf,int len,const cemon_addr *to){
  return cemon_enqueue_send(sock,buf,len,to,CEMON_SEND_CLASS_CONTROL);
}
static int cemon_recv_arm(cemon_socket *sock,int udp){
  if(sock==0||!cemon_socket_admit(sock,CEMON_LOOP_ADMISSION_OWNER_ONLY)||!cemon_recv_request_valid(sock,udp)) return -1;   /* owner-thread only */
  sock->recv_armed=1;
#if defined(_WIN32)
  if(sock->kind==CEMON_TCP_SOCK&&!cemon_socket_is_open(sock)) return 0;
  /* A failed arm must not leave recv_armed set: the flag is what cemon_recv_active() and the
     stats report, and nothing re-arms a socket on its own, so a lying flag means a connection
     that silently never receives again. */
  if(cemon_win_post_recv(sock)<0){ sock->recv_armed=0; return -1; }
  return 0;
#else
  if(cemon_unix_watch(sock)<0){ sock->recv_armed=0; return -1; }
  if(sock->loop->callback_depth==0){
    if(sock->kind==CEMON_UDP_SOCK) cemon_unix_recvfrom(sock);
    else cemon_unix_recv(sock);
  }
  return cemon_socket_is_dead(sock)?-1:0;
#endif
}
CEMON_DEF int cemon_recv(cemon_socket *sock){
  return cemon_recv_arm(sock,0);
}
CEMON_DEF int cemon_recvfrom(cemon_socket *sock){
  return cemon_recv_arm(sock,1);
}
CEMON_DEF void cemon_close(cemon_socket *sock){
  if(sock&&cemon_socket_admit(sock,CEMON_LOOP_ADMISSION_OWNER_ONLY)) cemon_socket_die(sock,0,1);
}
CEMON_DEF int cemon_shutdown(cemon_socket *sock){
  int rc;
  if(!cemon_tcp_shutdown_admissible(sock)||!cemon_socket_admit(sock,CEMON_LOOP_ADMISSION_OPEN)) return -1;
  rc=cemon_tcp_start_shutdown(sock);
  if(rc>0) return 0;
  if(rc<0) return -1;
  /* cemon_tcp_finish may FREE the socket here (cemon_socket_die drops the last
     reference and no completion hold is active on this path), so its return
     value carries the outcome: the previous version dereferenced `sock` right
     after this call to test state/tcp_phase, a use-after-free that faults as
     soon as the freed block is unmapped or reused (probe:
     cemon_uaf_probe.c with a page-poisoned CEMON_FREE -> SIGSEGV). */
  return cemon_tcp_finish(sock)<0?-1:0;
}
CEMON_DEF void *cemon_getud(cemon_socket *sock){
  if(sock==0||!cemon_socket_admit(sock,CEMON_LOOP_ADMISSION_OWNER_ONLY)) return 0;
  return sock->ud;
}
CEMON_DEF void cemon_setud(cemon_socket *sock,void *ud){
  if(sock&&!cemon_socket_is_dead(sock)&&cemon_socket_admit(sock,CEMON_LOOP_ADMISSION_OWNER_ONLY)) sock->ud=ud;
}
CEMON_DEF cemon_timer *cemon_after(cemon *loop,unsigned int after_ms,unsigned int period_ms,cemon_task_fn fn,void *ud){
  cemon_timer *timer;
  if(loop==0||fn==0||!cemon_loop_admit(loop,CEMON_LOOP_ADMISSION_OPEN)) return 0;
  timer=(cemon_timer*)CEMON_MALLOC(sizeof(*timer));
  if(timer==0) return 0;
  memset(timer,0,sizeof(*timer));
  timer->loop=loop;
  timer->fn=fn;
  timer->ud=ud;
  timer->heap_index=-1;
  timer->period=((cemon_u64)period_ms)*1000;
  timer->at=cemon_monotonic_us()+((cemon_u64)after_ms)*1000;
  if(cemon_timer_link(loop,timer)<0){
    CEMON_FREE(timer);
    return 0;
  }
  return timer;
}
CEMON_DEF void cemon_timer_stop(cemon_timer *timer){
  if(timer==0||timer->dead||!cemon_timer_admit(timer,CEMON_LOOP_ADMISSION_OWNER_ONLY)) return;
  timer->dead=1;
  if(!timer->firing){
    cemon_timer_unlink(timer);
    CEMON_FREE(timer);
  }
}
static int cemon_post_push(cemon *loop,cemon_post_node *node,int force,int inline_ingress_leave,int *left_ingress){
  int active,in_callback,leave_now,owner,running,wake;
  if(loop==0||node==0||node->fn==0) return -1;
  if(left_ingress) *left_ingress=0;
  node->next=0;
#if defined(_WIN32)
  EnterCriticalSection(&loop->post_lock);
#else
  pthread_mutex_lock(&loop->post_lock);
#endif
  owner=cemon_owner_ok_locked(loop);
  cemon_loop_control_flags_locked(loop,&active,0,0);
  in_callback=owner&&loop->callback_depth>0;
  running=active;
  leave_now=0;
  if((!force&&loop->post_count>=CEMON_POSTQ_LIMIT)||!cemon_loop_mailbox_accepts_locked(loop,CEMON_LOOP_ADMISSION_OWNER_CALLBACK,force)){
    /* Leave the ingress count INSIDE the lock: decrementing it unlocked is a
       lost-update race against cemon_ingress_close's handshake (two producers
       that both read a positive count and both store count-1 leave it above
       zero, so the closer never observes 0 and its wait loop spins forever). */
    if(inline_ingress_leave&&loop->control.ingress_count>0){
      loop->control.ingress_count--;
      leave_now=1;
    }
#if defined(_WIN32)
    LeaveCriticalSection(&loop->post_lock);
#else
    pthread_mutex_unlock(&loop->post_lock);
#endif
    if(left_ingress) *left_ingress=leave_now;
    return -1;
  }
  if(loop->post_tail) loop->post_tail->next=node;
  else loop->post_head=node;
  loop->post_tail=node;
  loop->post_count++;
  if(loop->post_count>loop->post_count_peak) loop->post_count_peak=loop->post_count;
  wake=cemon_post_wake_needed(owner,running,1,in_callback)?cemon_wake_prepare_locked(loop):0;
  if(inline_ingress_leave&&wake==0&&loop->control.ingress_count>0){
    loop->control.ingress_count--;
    leave_now=1;
  }
#if defined(_WIN32)
  LeaveCriticalSection(&loop->post_lock);
#else
  pthread_mutex_unlock(&loop->post_lock);
#endif
  if(left_ingress) *left_ingress=leave_now;
  cemon_wake_emit(loop,wake);
  return 0;
}
CEMON_DEF int cemon_post(cemon *loop,cemon_task_fn fn,void *ud){
  int left_ingress;
  cemon_post_node *node;
  if(loop==0||fn==0) return -1;
  node=(cemon_post_node*)CEMON_MALLOC(sizeof(*node));
  if(node==0) return -1;
  memset(node,0,sizeof(*node));
  node->fn=fn;
  node->ud=ud;
  if(!cemon_ingress_enter(loop,CEMON_LOOP_ADMISSION_OWNER_CALLBACK)){
    cemon_post_release(node);
    return -1;
  }
  left_ingress=0;
  if(cemon_post_push(loop,node,0,1,&left_ingress)<0){
    cemon_post_release(node);
    if(!left_ingress) cemon_ingress_leave(loop);
    return -1;
  }
  if(!left_ingress) cemon_ingress_leave(loop);
  return 0;
}
CEMON_DEF int cemon_resolve(cemon_addr *out,const char *host,unsigned short port,int udp){
  struct addrinfo hints,*ai,*node;
  char serv[CEMON_PORT_TEXT];
  int rc;
  if(out==0) return -1;
  if(cemon_numeric_addr(out,host,port,0)==0) return 0;
  memset(&hints,0,sizeof(hints));
  hints.ai_family=AF_UNSPEC;
  hints.ai_socktype=udp?SOCK_DGRAM:SOCK_STREAM;
  hints.ai_protocol=udp?IPPROTO_UDP:IPPROTO_TCP;
  sprintf(serv,"%u",(unsigned)port);
  rc=getaddrinfo(host&&host[0]?host:0,serv,&hints,&ai);
  if(rc!=0||ai==0) return -1;
  for(node=ai;node;node=node->ai_next){
    if(node->ai_addr&&(node->ai_family==AF_INET||node->ai_family==AF_INET6)&&cemon_addr_copy(out,node->ai_addr,(int)node->ai_addrlen)==0){
      freeaddrinfo(ai);
      return 0;
    }
  }
  freeaddrinfo(ai);
  return -1;
}
#endif

