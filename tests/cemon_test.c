/* cemon_test.c -- cemon.h unit tests (C89, always-static).
 *
 * Regression cover for the shutdown path, which had a use-after-free:
 * cemon_shutdown() called cemon_tcp_finish(), which closes the socket by
 * cemon_socket_die() -> cemon_sock_release() (refs 1 -> 0) -> cemon_sock_free(),
 * and then read the freed socket again (state / tcp_phase).  The default
 * allocator hides it - free() usually keeps the page mapped, so the stale read
 * is harmless until the block is reused.  This test therefore installs a
 * page-poisoning allocator: every block is right-aligned at the end of its own
 * pages with a trailing page after it, and CEMON_FREE() makes the whole block
 * inaccessible (VirtualProtect PAGE_NOACCESS on Windows, mprotect PROT_NONE on
 * POSIX), so any read after free faults immediately and deterministically
 * instead of passing silently.
 *
 * Portability: the Windows path is unchanged; POSIX gets the same behaviour
 * behind this file's own seam - probe_os_reserve / probe_os_poison /
 * probe_os_release (mmap / mprotect / munmap), probe_fd + PROBE_FD_INVALID +
 * probe_fd_close, probe_set_nonblocking and probe_sleep_ms - rather than
 * #define-ing Windows names to POSIX functions.
 *
 * Scope: the shutdown/close path, driven through the internal socket constructor
 * with a real (never used for I/O) TCP fd.  A full listen/connect/accept
 * round-trip through the loop is NOT covered here.
 *
 * Build: gcc -std=c89 -O2 -Wall -Wextra -Wno-unused-function \
 *           tests/cemon_test.c -o build/cemon_test.exe -lws2_32      (Windows)
 *        gcc -std=c89 -O2 -Wall -Wextra -Wno-unused-function \
 *           -D_POSIX_C_SOURCE=200809L -pthread \
 *           tests/cemon_test.c -o build/cemon_test.exe               (POSIX)
 */
/* The POSIX poisoning allocator needs mmap/mprotect.  MAP_ANONYMOUS is a glibc
   extension that only exists when a feature macro was defined before the first
   header (glibc's features.h is already processed here on CentOS 7), and the
   Linux build passes -D_POSIX_C_SOURCE=200809L, which on its own hides it. */
#if !defined(_WIN32)&&!defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#if defined(_WIN32)
#include <winsock2.h>
#endif
#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#include <io.h>          /* _dup/_dup2/_fileno/_close for the stderr capture below (MSVC 6 names) */
#else
#include <pthread.h>
#endif
#if defined(_WIN32)
#else
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#endif
#include "test.h"

/* ---- platform seam -------------------------------------------------------
   Two fd types with different sentinels (SOCKET/INVALID_SOCKET vs int/-1),
   different close calls and different sleep calls.  Everything below this
   block is written against these names only. */
#if defined(_WIN32)
typedef SOCKET probe_fd;
#define PROBE_FD_INVALID INVALID_SOCKET
#else
typedef int probe_fd;
#define PROBE_FD_INVALID (-1)
#endif

static void probe_fd_close(probe_fd fd){
#if defined(_WIN32)
  closesocket(fd);
#else
  close(fd);
#endif
}
static void probe_sleep_ms(int ms){
#if defined(_WIN32)
  Sleep((DWORD)ms);
#else
  usleep((useconds_t)ms*1000u);
#endif
}
static int probe_set_nonblocking(probe_fd fd){
#if defined(_WIN32)
  u_long nb=1;
  return ioctlsocket(fd,FIONBIO,&nb);
#else
  int nb=1;
  return ioctl(fd,FIONBIO,&nb);
#endif
}
/* Did a receive report the END of the connection?  A close ends a TCP connection with a FIN (recv returns
   0) or, when a pending overlapped read is aborted underneath it, with a reset - both mean "this socket is
   gone", which is exactly what a teardown has to prove.  A would-block is neither, so a socket a teardown
   leaked stays silent through this predicate and fails the assertion. */
static int probe_last_net_error(void){
#if defined(_WIN32)
  return WSAGetLastError();
#else
  return errno;
#endif
}
static int probe_conn_ended(int got){
  int err;
  if(got==0) return 1;
  if(got>0) return 0;
  err=probe_last_net_error();
#if defined(_WIN32)
  return (err==WSAECONNRESET||err==WSAECONNABORTED)?1:0;
#else
  return (err==ECONNRESET||err==ECONNABORTED)?1:0;
#endif
}
/* Reserve `total` bytes of read/write memory for one block + its trailing page. */
static void *probe_os_reserve(size_t total){
#if defined(_WIN32)
  return VirtualAlloc(0,total,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
#else
  void *p=mmap(0,total,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(p==MAP_FAILED) return 0;
  return p;
#endif
}
/* Poison: make the whole reservation inaccessible, so any later read or write
   of the freed block faults instead of silently touching reused memory. */
static void probe_os_poison(void *base,size_t total){
#if defined(_WIN32)
  DWORD old;
  VirtualProtect(base,total,PAGE_NOACCESS,&old);
#else
  mprotect(base,total,PROT_NONE);
#endif
}
static void probe_os_release(void *base,size_t total){
#if defined(_WIN32)
  (void)total;
  VirtualFree(base,0,MEM_RELEASE);
#else
  munmap(base,total);
#endif
}

/* ---- page-poisoning allocator (must be defined before cemon.h) ---- */
#define PROBE_MAX_BLOCKS 8192
typedef struct probe_block{ void *ptr; char *base; size_t bytes; } probe_block;
static probe_block probe_blocks[PROBE_MAX_BLOCKS];
static int probe_block_count;

static void *probe_alloc(size_t n){
  size_t pages,total,off;
  char *base;
  if(n==0) n=1;
  pages=(n+4096u-1u)/4096u;
  total=(pages+1u)*4096u;                     /* one trailing guard page */
  base=(char*)probe_os_reserve(total);
  if(!base) return 0;
  if(probe_block_count>=PROBE_MAX_BLOCKS){ probe_os_release(base,total); return 0; }
  off=pages*4096u-n;                          /* right-align: block ends at the page end */
  probe_blocks[probe_block_count].ptr=base+off;
  probe_blocks[probe_block_count].base=base;
  probe_blocks[probe_block_count].bytes=total;
  probe_block_count++;
  return base+off;
}
static probe_block *probe_find(void *p){
  int i;
  for(i=0;i<probe_block_count;i++) if(probe_blocks[i].ptr==p) return &probe_blocks[i];
  return 0;
}
static void probe_free(void *p){
  probe_block *b;
  if(!p) return;
  b=probe_find(p);
  if(!b) return;                              /* foreign pointer: nothing to poison */
  probe_os_poison(b->base,b->bytes);          /* any later use faults */
}
static void *probe_realloc(void *p,size_t n){
  probe_block *b;
  void *np;
  if(!p) return probe_alloc(n);
  b=probe_find(p);
  np=probe_alloc(n);
  if(!np) return 0;
  if(b){
    size_t off=(size_t)((char*)b->ptr-b->base);
    size_t old=b->bytes-off;
    memcpy(np,b->ptr,old<n?old:n);
  }
  probe_free(p);
  return np;
}

#define CEMON_MALLOC probe_alloc
#define CEMON_FREE probe_free
#define CEMON_REALLOC probe_realloc
#define CEMON_IMPLEMENTATION
#include "../code/cemon.h"

/* Shutdown with the write half still open: cemon_tcp_finish() runs
   cemon_tcp_shutdown_write(), which fails on this never-connected fd, so the
   socket dies with an error and is FREED - the old code then read it. */
static void test_shutdown_write_open(void){
  cemon *loop;
  cemon_socket *sock;
  probe_fd fd;
  int rc;
  TEST_BEGIN("cemon_shutdown: write half open (socket freed on error)");
  loop=cemon_create();
  TEST_ASSERT(loop!=0,"loop created");
  fd=socket(AF_INET,SOCK_STREAM,0);
  TEST_ASSERT(fd!=PROBE_FD_INVALID,"tcp fd");
  sock=cemon_sock_new(loop,(cemon_fd)fd,CEMON_TCP_SOCK,0,0);
  TEST_ASSERT(sock!=0,"socket created");
  cemon_tcp_note_eof(sock);                   /* the peer closed its half */
  rc=cemon_shutdown(sock);                    /* reachable UAF: freed, then read */
  TEST_ASSERT_I64_EQ(rc,-1,"shutdown reports the failed write close");
  cemon_destroy(loop);
  TEST_END();
}

/* Shutdown with both halves closed: finish closes the connection cleanly and the
   socket is freed inside the call (the "success" path of the same UAF). */
static void test_shutdown_write_closed(void){
  cemon *loop;
  cemon_socket *sock;
  probe_fd fd;
  int rc;
  TEST_BEGIN("cemon_shutdown: write half already closed (clean close)");
  loop=cemon_create();
  TEST_ASSERT(loop!=0,"loop created");
  fd=socket(AF_INET,SOCK_STREAM,0);
  TEST_ASSERT(fd!=PROBE_FD_INVALID,"tcp fd");
  sock=cemon_sock_new(loop,(cemon_fd)fd,CEMON_TCP_SOCK,0,0);
  TEST_ASSERT(sock!=0,"socket created");
  cemon_tcp_note_eof(sock);
  cemon_tcp_note_write_closed(sock);
  rc=cemon_shutdown(sock);
  TEST_ASSERT_I64_EQ(rc,0,"shutdown accepted");
  cemon_destroy(loop);
  TEST_END();
}

/* A socket keeps `sock_total` above zero while its last reference is held by
   something that never completes - e.g. a queued post holds one (cemon_sock_hold)
   at destroy time, and destroy drains the posts only AFTER the IOCP drain.  No
   completion can arrive for it, so the IOCP drain must be bounded: with the old
   INFINITE wait destroy blocked forever.  Hold the reference by hand to model
   that state deterministically. */
/* The drain must be bounded.  The assertion's point is that the call RETURNS in finite time: a regression to an
   INFINITE wait would hang the suite, which the gate's per-layer watchdog reports (E1).  The bound itself is 5x the
   measured 2.0 s drain, so a finite-but-absurd increase - the shape a "just wait longer" fix takes - fails it. */
#define CEMON_DESTROY_BOUND_US 10000000ul   /* 5x the measured 2.0 s drain */

static void test_destroy_bounded_drain(void){
  cemon *loop;
  cemon_socket *sock;
  probe_fd fd;
  unsigned long t0,elapsed;
  TEST_BEGIN("cemon_destroy returns with a never-completing socket ref (bounded drain)");
  loop=cemon_create();
  TEST_ASSERT(loop!=0,"loop created");
  fd=socket(AF_INET,SOCK_STREAM,0);
  TEST_ASSERT(fd!=PROBE_FD_INVALID,"tcp fd");
  sock=cemon_sock_new(loop,(cemon_fd)fd,CEMON_TCP_SOCK,0,0);
  TEST_ASSERT(sock!=0,"socket created");
  cemon_sock_hold(sock);                      /* the "holder that never completes" */
  cemon_socket_die(sock,0,0);                 /* unlinked + fd closed, but not freed */
  /* `TEST_ASSERT(1, "destroy returned")` used to sit here: an assertion that cannot fail, so the case only had
     the *absence* of a hang as evidence, and a hang would have wedged cemon_test rather than failing it (fourth
     review round E2).  Measure the return instead - and say where a true hang is caught, so the bound is not
     mistaken for the whole guard. */
  t0=_test_now_us();
  cemon_destroy(loop);                        /* must return instead of hanging */
  elapsed=_test_now_us()-t0;
  TEST_ASSERT(elapsed<CEMON_DESTROY_BOUND_US,
              "destroy returned inside the bound (a hang that never returns is reported by the gate's per-layer "
              "watchdog, not by this thread)");
  TEST_END();
}

/* Control frames must not queue behind bulk traffic.  The send queue is a
   byte-ordered FIFO, so a heartbeat tail-queued behind bulk waits for the WHOLE
   queue: measured 380 ms behind 1 MB of bulk at ~2.8 MB/s - beyond the 250 ms
   minimum election timeout.  The fix inserts a control frame right behind the
   frame being written, bounding the head-of-line delay by one frame.  This test
   measures the stream offset at which a marked control frame arrives after the
   socket has been saturated with bulk. */
static int g_ctl_connected;
static void ctl_io(cemon_socket *sock,const cemon_event *event){
  if(!sock||!event) return;
  if(event->type==CEMON_CONNECT) g_ctl_connected=1;
}
static void test_control_frame_priority(void){
  cemon *loop;
  cemon_socket *sock;
  probe_fd listener,peer;
  struct sockaddr_in addr;
  cemon_socklen addr_len;
  int opt,i,rc,tries,fails,found;
  static unsigned char bulk[256u*1024u];
  static unsigned char ctl[64];
  static unsigned char rbuf[65536];
  cemon_u64 seen,offset,t0,now;
  TEST_BEGIN("cemon: a control frame is not queued behind bulk traffic");
  memset(bulk,0x5a,sizeof(bulk));
  memset(ctl,0xc7,sizeof(ctl));
  listener=socket(AF_INET,SOCK_STREAM,0);
  TEST_ASSERT(listener!=PROBE_FD_INVALID,"listener socket");
  opt=65536;
  setsockopt(listener,SOL_SOCKET,SO_RCVBUF,(const char *)&opt,(cemon_socklen)sizeof(opt));
  memset(&addr,0,sizeof(addr));
  addr.sin_family=AF_INET;
  addr.sin_port=0;
  addr.sin_addr.s_addr=inet_addr("127.0.0.1");
  TEST_ASSERT(bind(listener,(struct sockaddr *)&addr,(cemon_socklen)sizeof(addr))==0,"bind ephemeral port");
  addr_len=(cemon_socklen)sizeof(addr);
  TEST_ASSERT(getsockname(listener,(struct sockaddr *)&addr,&addr_len)==0,"getsockname");
  TEST_ASSERT(listen(listener,1)==0,"listen");
  loop=cemon_create();
  TEST_ASSERT(loop!=0,"loop created");
  g_ctl_connected=0;
  sock=cemon_tcp_connect(loop,"127.0.0.1",ntohs(addr.sin_port),ctl_io,0);
  TEST_ASSERT(sock!=0,"connect socket");
  for(i=0;i<200&&!g_ctl_connected;i++) cemon_poll(loop,10);
  TEST_ASSERT(g_ctl_connected,"connected");
  /* keep the kernel buffers small so the measured offset reflects QUEUE order */
  opt=16384;
  setsockopt(sock->fd,SOL_SOCKET,SO_SNDBUF,(const char *)&opt,(cemon_socklen)sizeof(opt));
  tries=0; fails=0;
  while(fails<8&&tries<600){
    if(cemon_send(sock,bulk,(int)sizeof(bulk))!=0) fails++;
    else fails=0;
    tries++;
    cemon_poll(loop,0);
  }
  TEST_ASSERT(fails>=8,"socket saturated with bulk");
  rc=cemon_send_control(sock,ctl,(int)sizeof(ctl));
  TEST_ASSERT_I64_EQ(rc,0,"control frame accepted while the socket is saturated");
  peer=accept(listener,0,0);
  TEST_ASSERT(peer!=PROBE_FD_INVALID,"peer accepted");
  probe_set_nonblocking(peer);
  seen=0; found=0; offset=0;
  t0=cemon_monotonic_us();
  while(!found){
    cemon_poll(loop,0);
    rc=recv(peer,(char *)rbuf,(int)sizeof(rbuf),0);
    if(rc>0){
      for(i=0;i<rc;i++){
        if(rbuf[i]==0xc7){ found=1; offset=seen+(cemon_u64)i; break; }
      }
      seen+=(cemon_u64)rc;
    }
    now=cemon_monotonic_us();
    if(now-t0>10000000u) break;
    if(!found&&rc<=0) probe_sleep_ms(1);
  }
  printf("   [info] control frame at stream offset %lu (one bulk frame = %lu bytes, queue cap = %u)\n",
         (unsigned long)offset,(unsigned long)sizeof(bulk),(unsigned)1048576u);
  TEST_ASSERT(found,"control frame arrived");
  TEST_ASSERT(offset>0,"still ordered after the bytes already queued ahead of it");
  TEST_ASSERT(offset<=2u*(cemon_u64)sizeof(bulk),"head-of-line delay bounded by ~one frame, not the whole queue");
  probe_fd_close(peer);
  probe_fd_close(listener);
  cemon_close(sock);
  cemon_destroy(loop);
  TEST_END();
}

/* A refused loop must say why (issue #11).  The Windows completion port and the kqueue
   poller recorded a cause; the epoll branch returned a bare -1 with no note at all, so on
   Linux cemon_fatal_what stayed empty and the caller had nothing to log - it could not
   even tell a backend failure from a converged stop.  Make the wait itself fail
   (invalidate the poller handle), then read the recorded note back. */
static void test_poll_fatal_reports_cause(void){
  cemon *loop;
  const char *expect_what;
  int expect_code;
  int rc;
  TEST_BEGIN("cemon_poll: a backend failure records its cause (failing step + code)");
  loop=cemon_create();
  TEST_ASSERT(loop!=0,"loop created");
#if defined(_WIN32)
  /* An invalid completion port makes GetQueuedCompletionStatus fail with a real code. */
  CloseHandle(loop->port);
  loop->port=0;
  expect_what="completion port poll";
  expect_code=ERROR_INVALID_HANDLE;
#elif defined(CEMON_USE_KQUEUE)
  close(loop->fd);
  loop->fd=CEMON_BAD_FD;   /* also keeps cemon_destroy from closing a reused descriptor */
  expect_what="kevent";
  expect_code=EBADF;
#else
  close(loop->fd);
  loop->fd=CEMON_BAD_FD;
  expect_what="epoll_wait";
  expect_code=EBADF;
#endif
  rc=cemon_poll(loop,0);
  TEST_ASSERT_I64_EQ(rc,-1,"cemon_poll reports the backend failure");
  TEST_ASSERT_I64_EQ(cemon_fatal_code(loop),expect_code,
                     "the platform code is recorded (Winsock code on Windows, errno elsewhere)");
  TEST_ASSERT(strcmp(cemon_fatal_what(loop),expect_what)==0,"the failing step is named");
  cemon_destroy(loop);
  TEST_END();
}

/* The owner thread is the only one allowed to poll/inspect/destroy the loop; other
   threads may post and send while it is open.  The owner used to be bound only
   implicitly (by whichever thread admitted first), so a helper thread could capture
   it and lock the real poller out, and a non-owner cemon_destroy was a silent no-op
   on a void API.  Explicit binding + a reported refusal close both gaps. */
static cemon *g_own_loop;
static int g_own_bind_rc,g_own_is_owner,g_own_poll_rc,g_own_destroy_rc;
static void *owner_probe_thread(void *arg){
  (void)arg;
  g_own_bind_rc=cemon_bind_owner(g_own_loop);
  g_own_is_owner=cemon_is_owner(g_own_loop);
  g_own_poll_rc=cemon_poll(g_own_loop,0);
  g_own_destroy_rc=cemon_destroy(g_own_loop);
  return 0;
}
/* The probe needs two thread primitives and nothing else.  Including <pthread.h> unconditionally made this
   file depend on MinGW-w64's winpthreads on Windows, which MSVC 6 of course does not have (it failed with
   "fatal error C1083: Cannot open include file: 'pthread.h'"), so the project's own test could not be built
   with the compiler the project claims to support.  Provide the Windows side here instead. */
#if defined(_WIN32)
typedef HANDLE test_thread;
static unsigned __int32 __stdcall owner_probe_thread_win(void *arg){
  owner_probe_thread(arg);
  return 0;
}
static int test_thread_start(test_thread *th){
  *th=(HANDLE)_beginthreadex(0,0,owner_probe_thread_win,0,0,0);
  return *th!=0?0:-1;
}
static void test_thread_join(test_thread *th){
  WaitForSingleObject(*th,INFINITE);
  CloseHandle(*th);
}
#else
typedef pthread_t test_thread;
static int test_thread_start(test_thread *th){ return pthread_create(th,0,owner_probe_thread,0); }
static void test_thread_join(test_thread *th){ pthread_join(*th,0); }
#endif
/* A second body needs to run on its own thread (the teardown cases below wedge a PRODUCER inside the loop
   while the owner keeps running), so the same two primitives are exposed for any body.  Keeping both
   signatures means the existing owner probe stays exactly as it was. */
typedef void (*test_thread_body)(void);
#if defined(_WIN32)
static test_thread_body g_test_thread_body;
static unsigned __int32 __stdcall test_thread_tramp(void *arg){
  (void)arg;
  if(g_test_thread_body) g_test_thread_body();
  return 0;
}
static int test_thread_start_fn(test_thread *th,test_thread_body fn){
  g_test_thread_body=fn;
  *th=(HANDLE)_beginthreadex(0,0,test_thread_tramp,0,0,0);
  return *th!=0?0:-1;
}
#else
static test_thread_body g_test_thread_body;
static void *test_thread_tramp(void *arg){
  (void)arg;
  if(g_test_thread_body) g_test_thread_body();
  return 0;
}
static int test_thread_start_fn(test_thread *th,test_thread_body fn){
  g_test_thread_body=fn;
  return pthread_create(th,0,test_thread_tramp,0);
}
#endif
static void test_owner_binding(void){
  test_thread th;
  cemon *loop;
  TEST_BEGIN("cemon: owner is explicitly bindable and violations are reported");
  loop=cemon_create();
  TEST_ASSERT(loop!=0,"loop created");
  g_own_loop=loop;
  g_own_bind_rc=99; g_own_is_owner=99; g_own_poll_rc=99; g_own_destroy_rc=99;
  TEST_ASSERT_I64_EQ(cemon_is_owner(loop),1,"an unbound loop accepts the caller as owner");
  TEST_ASSERT_I64_EQ(cemon_bind_owner(loop),0,"explicit bind from this thread succeeds");
  TEST_ASSERT_I64_EQ(cemon_is_owner(loop),1,"this thread is the owner");
  TEST_ASSERT(test_thread_start(&th)==0,"probe thread started");
  test_thread_join(&th);
  TEST_ASSERT_I64_EQ(g_own_bind_rc,-1,"another thread cannot bind an owned loop");
  TEST_ASSERT_I64_EQ(g_own_is_owner,0,"another thread is not the owner");
  TEST_ASSERT_I64_EQ(g_own_poll_rc,-1,"another thread cannot poll");
  TEST_ASSERT_I64_EQ(g_own_destroy_rc,-1,"another thread cannot destroy (was a silent no-op)");
  TEST_ASSERT_I64_EQ(cemon_destroy(loop),0,"the owner can destroy");
  TEST_END();
}

/* ---- loop teardown: issue #9 / audit C11 -----------------------------------------------
   Two waits inside cemon_destroy used to have no exit condition.  (1) cemon_ingress_close spun on
   SwitchToThread forever while a producer thread held an ingress section, and (2) the Windows completion
   drain waited with INFINITE for a completion that could never arrive.  Both are bounded and both SAY so;
   these cases pin the exit condition, the visible warning, and that nothing is left owned afterwards.

   A PEER cannot hold the ingress section: it covers a post/send call made by a PRODUCER thread, and
   nothing a peer does enters it (doc/gaps-audit.md O2 says the same).  The reachable condition behind the
   warning is a producer thread that stops making progress, so that is what the first case builds: the
   thread enters through cemon_ingress_enter - the same entry function cemon_post uses - and then blocks.
   The second case is the peer-facing half and IS driven by a real socket: a live connection with a read
   armed whose peer stays silent, which must come out with no socket left behind. */

/* Capture what goes to stderr while a teardown runs.  The contract is a VISIBLE warning, so the assertion
   has to read the text an operator would see; tmpfile() keeps the capture out of the working tree. */
typedef struct probe_capture{ FILE *file; int saved_fd; int active; } probe_capture;
#if defined(_MSC_VER)
#define probe_dup(fd)      _dup(fd)
#define probe_dup2(fd,to)  _dup2((fd),(to))
#define probe_fileno(f)    _fileno(f)
#define probe_close_fd(fd) _close(fd)
#else
#define probe_dup(fd)      dup(fd)
#define probe_dup2(fd,to)  dup2((fd),(to))
#define probe_fileno(f)    fileno(f)
#define probe_close_fd(fd) close(fd)
#endif
static int probe_capture_begin(probe_capture *c){
  c->file=tmpfile();
  c->active=0;
  if(!c->file) return -1;
  fflush(stderr);
  c->saved_fd=probe_dup(2);
  if(c->saved_fd<0){ fclose(c->file); c->file=0; return -1; }
  if(probe_dup2(probe_fileno(c->file),2)<0){ probe_close_fd(c->saved_fd); fclose(c->file); c->file=0; return -1; }
  c->active=1;
  return 0;
}
static int probe_capture_end(probe_capture *c,char *buf,int cap){
  int n=0,got;
  if(cap>0) buf[0]='\0';
  if(!c->active) return -1;
  fflush(stderr);
  probe_dup2(c->saved_fd,2);
  probe_close_fd(c->saved_fd);
  c->active=0;
  rewind(c->file);
  while(n<cap-1&&(got=(int)fread(buf+n,1,(size_t)(cap-1-n),c->file))>0) n+=got;
  buf[n]='\0';
  fclose(c->file);
  c->file=0;
  return n;
}
static int probe_capture_has(const char *text,const char *needle){
  return (text&&needle&&strstr(text,needle)!=0)?1:0;
}

/* The bounded waits are 5 s and 2 s, so the assertion is "this call RETURNS" with slack on top; an
   unbounded regression never returns at all and is reported by the gate's per-layer watchdog, not here. */
#define CEMON_INGRESS_BOUND_US 12000000ul
#define CEMON_PEER_DRAIN_BOUND_US 2000000ul

static cemon *g_ing_loop;
static int g_ing_enter_rc,g_ing_left;
static volatile int g_ing_held,g_ing_release;
static void ingress_producer_body(void){
  g_ing_enter_rc=cemon_ingress_enter(g_ing_loop,CEMON_LOOP_ADMISSION_OWNER_CALLBACK);
  g_ing_held=1;
  while(!g_ing_release) probe_sleep_ms(1);
  /* The next statement the runtime's own producer would run, on the loop object destroy was asked to free:
     if destroy freed it anyway, the page-poisoning allocator turns this into a fault, not a silent pass. */
  cemon_ingress_leave(g_ing_loop);
  g_ing_left=1;
}
static void test_ingress_close_bounded(void){
  cemon *loop;
  test_thread th;
  probe_capture cap;
  char text[2048];
  unsigned long t0,elapsed;
  int rc;
  TEST_BEGIN("cemon_destroy: a stuck producer is bounded, reported, and left with a live loop");
  g_ing_loop=cemon_create();
  loop=g_ing_loop;
  TEST_ASSERT(loop!=0,"loop created");
  TEST_ASSERT_I64_EQ(cemon_bind_owner(loop),0,"this thread owns the loop");
  g_ing_enter_rc=99; g_ing_left=0; g_ing_held=0; g_ing_release=0;
  TEST_ASSERT_I64_EQ(test_thread_start_fn(&th,ingress_producer_body),0,"producer thread started");
  t0=_test_now_us();
  while(!g_ing_held&&_test_now_us()-t0<5000000ul) probe_sleep_ms(1);
  if(!g_ing_held||g_ing_enter_rc!=1){
    g_ing_release=1;
    test_thread_join(&th);
    TEST_ASSERT(0,"the producer holds an ingress section (without this the rest of the case proves nothing)");
  }
  TEST_ASSERT_I64_EQ(probe_capture_begin(&cap),0,"stderr captured");
  t0=_test_now_us();
  rc=cemon_destroy(loop);
  elapsed=_test_now_us()-t0;
  probe_capture_end(&cap,text,sizeof(text));
  TEST_ASSERT(elapsed<CEMON_INGRESS_BOUND_US,
              "destroy returned inside the bound (the unbounded spin never returned at all)");
  TEST_ASSERT(probe_capture_has(text,"ingress close gave up"),
              "the give-up is visible: stderr names it instead of hanging silently");
  TEST_ASSERT(probe_capture_has(text,"1 ingress operation"),
              "the warning carries the residual count, not just a generic message");
  TEST_ASSERT_I64_EQ(rc,-1,"destroy reports the abandonment instead of a completed teardown");
  g_ing_release=1;
  test_thread_join(&th);
  TEST_ASSERT_I64_EQ(g_ing_left,1,"the released producer left the section on a loop that is still allocated");
  TEST_ASSERT_I64_EQ(cemon_destroy(loop),0,"a second destroy, with no producer left, completes the teardown");
  TEST_END();
}

/* The peer-facing half: a silent peer, a read armed on a live connection, and a teardown that has to settle
   it.  Two positive signals are asserted before the teardown is read - the connection completed and the
   loop really holds the socket - so a run where nothing happened cannot pass as "no leak". */
static int g_peer_connected;
static void peer_io(cemon_socket *sock,const cemon_event *event){
  (void)sock;
  if(event&&event->type==CEMON_CONNECT) g_peer_connected=1;
}
static void test_destroy_silent_peer_no_leak(void){
  cemon *loop;
  cemon_socket *sock;
  cemon_stats st;
  probe_fd listener,peer;
  struct sockaddr_in addr;
  cemon_socklen alen;
  char byte;
  unsigned long t0;
  int i,rc,got;
  TEST_BEGIN("cemon_destroy: a silent peer with a read armed leaves no socket behind");
  listener=socket(AF_INET,SOCK_STREAM,0);
  TEST_ASSERT(listener!=PROBE_FD_INVALID,"listener socket");
  memset(&addr,0,sizeof(addr));
  addr.sin_family=AF_INET;
  addr.sin_port=0;
  addr.sin_addr.s_addr=inet_addr("127.0.0.1");
  TEST_ASSERT(bind(listener,(struct sockaddr *)&addr,(cemon_socklen)sizeof(addr))==0,"bind an ephemeral port");
  alen=(cemon_socklen)sizeof(addr);
  TEST_ASSERT(getsockname(listener,(struct sockaddr *)&addr,&alen)==0,"getsockname");
  TEST_ASSERT(listen(listener,1)==0,"listen");
  loop=cemon_create();
  TEST_ASSERT(loop!=0,"loop created");
  TEST_ASSERT_I64_EQ(cemon_bind_owner(loop),0,"this thread owns the loop");
  g_peer_connected=0;
  sock=cemon_tcp_connect(loop,"127.0.0.1",(unsigned short)ntohs(addr.sin_port),peer_io,0);
  TEST_ASSERT(sock!=0,"connect started");
  for(i=0;i<200&&!g_peer_connected;i++) cemon_poll(loop,5);
  TEST_ASSERT(g_peer_connected,"the connection completed");
  peer=accept(listener,0,0);
  TEST_ASSERT(peer!=PROBE_FD_INVALID,"peer accepted (the peer now stays silent)");
  TEST_ASSERT_I64_EQ(cemon_recv(sock),0,"a read is armed: the socket carries an outstanding operation");
  memset(&st,0,sizeof(st));
  TEST_ASSERT_I64_EQ(cemon_inspect(loop,0,&st),0,"inspect");
  TEST_ASSERT(st.loop_sock_total>=1,"the loop really holds the socket (the drain has something to settle)");
  t0=_test_now_us();
  rc=cemon_destroy(loop);
  TEST_ASSERT_I64_EQ(rc,0,"destroy completed");
  TEST_ASSERT(_test_now_us()-t0<CEMON_PEER_DRAIN_BOUND_US,
              "destroy settled the pending operation instead of waiting out the 2000 ms drain cap");
  /* No socket may survive the teardown, and the peer's own connection is where that is visible: a closed
     socket ends it (EOF, or a reset - Windows resets when an overlapped read is aborted by the close),
     while a leaked owner keeps the handle open and leaves this peer with nothing at all.  The distinction
     that matters is "ended" versus "still open and silent", so both end-of-connection forms are accepted
     and a would-block is not. */
  probe_set_nonblocking(peer);
  got=-1;
  t0=_test_now_us();
  while(_test_now_us()-t0<CEMON_PEER_DRAIN_BOUND_US){
    got=(int)recv(peer,&byte,1,0);
    if(got>=0) break;
    if(probe_conn_ended(got)) break;
    probe_sleep_ms(5);
  }
  TEST_ASSERT(probe_conn_ended(got),
              "the peer's connection was ended by the teardown: nothing kept the socket alive");
  TEST_ASSERT_I64_EQ(got<=0?0:1,0,"and no data was fabricated for the silent peer");
  probe_fd_close(peer);
  probe_fd_close(listener);
  TEST_END();
}


/* ---- UDP delivery (issue #10 / audit C12) --------------------------------
   Two defects shared one site in the Windows UDP receive completion and neither
   had a regression case: (1) the emit fell through to the TCP-style `recv->buf`
   whenever WSARecvFrom reported no usable source address, so the application was
   handed a "complete datagram" made of uninitialised bytes; (2) a soft receive
   error was absorbed and re-armed without a trace.

   Reachability, read from the code rather than assumed: cemon_udp_bind REFUSES the
   socket when cemon_win_udp_no_reset cannot switch reset reporting off (a refused
   ioctl dies the socket in cemon_udp_bind), and the datagram buffer (65536) is larger
   than the largest UDP payload (65507) - so neither WSAECONNRESET nor WSAEMSGSIZE can
   arrive from the OS on this platform.  The completion is therefore injected directly.
   What these cases pin is the handler's own behaviour (which buffer it emits, that the
   counter moves, that the socket stays usable and re-armed), not an OS-delivered error. */
static int g_udp_events;
static int g_udp_len;
static void *g_udp_src;
static char g_udp_copy[128];

static void udp_capture(cemon_socket *sock,const cemon_event *event){
  if(!sock||!event) return;
  if(event->type!=CEMON_DATA) return;
  g_udp_events++;
  g_udp_len=event->size;
  g_udp_src=event->data;
  if(event->data&&event->size>0&&event->size<=(int)sizeof(g_udp_copy))
    memcpy(g_udp_copy,event->data,(size_t)event->size);
}

/* The end-to-end half: whatever branch the receive takes, the application must get
   the datagram's own bytes and its exact length (the header's own words for UDP
   CEMON_DATA).  Portable, so it runs on both platforms. */
static void test_udp_loopback_datagram(void){
  cemon *loop;
  cemon_socket *rx,*tx;
  cemon_addr to;
  struct sockaddr_in bound;
  cemon_socklen blen;
  char payload[64];
  int i;
  TEST_BEGIN("udp: a loopback datagram arrives whole, with its own bytes");
  loop=cemon_create();
  TEST_ASSERT(loop!=0,"loop created");
  rx=cemon_udp_bind(loop,"127.0.0.1",0,udp_capture,0);
  TEST_ASSERT(rx!=0,"receiving socket bound");
  memset(&bound,0,sizeof(bound));
  blen=(cemon_socklen)sizeof(bound);
  TEST_ASSERT(getsockname((probe_fd)rx->fd,(struct sockaddr *)&bound,&blen)==0,"getsockname");
  TEST_ASSERT_I64_EQ(cemon_resolve(&to,"127.0.0.1",(unsigned short)ntohs(bound.sin_port),1),0,
                     "resolve the destination address");
  tx=cemon_udp_bind(loop,"127.0.0.1",0,0,0);
  TEST_ASSERT(tx!=0,"sending socket bound");
  TEST_ASSERT_I64_EQ(cemon_recvfrom(rx),0,"receive armed");
  for(i=0;i<(int)sizeof(payload);i++) payload[i]=(char)(i+1);
  g_udp_events=0; g_udp_len=-1; g_udp_src=0;
  TEST_ASSERT_I64_EQ(cemon_sendto(tx,payload,(int)sizeof(payload),&to),0,"send accepted");
  for(i=0;i<500&&g_udp_events==0;i++) cemon_poll(loop,5);
  TEST_ASSERT_I64_EQ(g_udp_events,1,"exactly one datagram delivered");
  TEST_ASSERT_I64_EQ(g_udp_len,(int)sizeof(payload),"the whole payload, not a prefix");
  TEST_ASSERT_MEM_EQ(g_udp_copy,(int)sizeof(payload),payload,(int)sizeof(payload),"bytes intact");
  cemon_destroy(loop);
  TEST_END();
}

#if defined(_WIN32)
/* The unusable-source-address case: WSARecvFrom left addr_len at 0, which used to
   send the emit down the shared `else if(bytes>0)` branch and out of recv->buf. */
static void test_udp_datagram_buffer_when_address_unusable(void){
  cemon *loop;
  cemon_socket *sock;
  int i;
  TEST_BEGIN("udp: a datagram with no usable source address is read from its own buffer");
  loop=cemon_create();
  TEST_ASSERT(loop!=0,"loop created");
  sock=cemon_udp_bind(loop,"127.0.0.1",0,udp_capture,0);
  TEST_ASSERT(sock!=0,"udp socket bound");
  /* Arming is what posts the receive, and the completion carries the socket it was
     posted for - injecting one without arming first is a no-op that would read as
     "the handler emitted nothing". */
  TEST_ASSERT_I64_EQ(cemon_recvfrom(sock),0,"receive armed");
  TEST_ASSERT_PTR_EQ(sock->recv->op.sock,(void *)sock,"the receive was really posted");
  for(i=0;i<32;i++){
    sock->udp_recv->buf[i]=(char)('A'+i);
    sock->recv->buf[i]=(char)0xEE;          /* TCP-style buffer: a UDP receive never fills it */
  }
  sock->udp_recv->addr_len=0;               /* no source address was reported */
  g_udp_events=0; g_udp_len=-1; g_udp_src=0;
  memset(g_udp_copy,0,sizeof(g_udp_copy));
  cemon_win_recv_done(sock->recv,32,0);
  TEST_ASSERT_I64_EQ(g_udp_events,1,"one CEMON_DATA emitted");
  TEST_ASSERT_I64_EQ(g_udp_len,32,"the datagram keeps its length");
  TEST_ASSERT_PTR_EQ(g_udp_src,(const void *)sock->udp_recv->buf,
                     "delivered from the datagram buffer, not recv->buf");
  TEST_ASSERT_MEM_EQ(g_udp_copy,32,sock->udp_recv->buf,32,"the datagram's own bytes");
  cemon_destroy(loop);
  TEST_END();
}

/* The soft-error case: an ICMP port-unreachable from an earlier send is not a
   failure, but it used to leave no trace at all. */
static void test_udp_soft_error_counted(void){
  cemon *loop;
  cemon_socket *sock;
  cemon_stats st;
  TEST_BEGIN("udp: a soft receive error is counted and the socket survives");
  loop=cemon_create();
  TEST_ASSERT(loop!=0,"loop created");
  sock=cemon_udp_bind(loop,"127.0.0.1",0,udp_capture,0);
  TEST_ASSERT(sock!=0,"udp socket bound");
  TEST_ASSERT_I64_EQ(cemon_recvfrom(sock),0,"receive armed");
  TEST_ASSERT_PTR_EQ(sock->recv->op.sock,(void *)sock,"the receive was really posted");
  memset(&st,0,sizeof(st));
  TEST_ASSERT_I64_EQ(cemon_inspect(loop,0,&st),0,"inspect");
  TEST_ASSERT_I64_EQ((int)st.loop_udp_soft_errors,0,"counter starts at zero");
  g_udp_events=0;
  cemon_win_recv_done(sock->recv,0,WSAECONNRESET);
  memset(&st,0,sizeof(st));
  TEST_ASSERT_I64_EQ(cemon_inspect(loop,0,&st),0,"inspect after the soft error");
  TEST_ASSERT_I64_EQ((int)st.loop_udp_soft_errors,1,"the absorbed error is now counted");
  TEST_ASSERT_I64_EQ(cemon_socket_is_dead(sock),0,"the socket is still usable");
  TEST_ASSERT_I64_EQ(sock->recv_armed,1,"the receive was re-armed");
  TEST_ASSERT_I64_EQ(g_udp_events,0,"no datagram was invented for an error completion");
  cemon_destroy(loop);
  TEST_END();
}
#endif

int main(void){
#if defined(_WIN32)
  WSADATA wsa;
  WSAStartup(MAKEWORD(2,2),&wsa);
#endif
#if defined(_WIN32)
  TEST_PLAN(11);
#else
  TEST_PLAN(9);
#endif
  test_shutdown_write_open();
  test_shutdown_write_closed();
  test_destroy_bounded_drain();
  test_control_frame_priority();
  test_poll_fatal_reports_cause();
  test_owner_binding();
  test_udp_loopback_datagram();
#if defined(_WIN32)
  test_udp_datagram_buffer_when_address_unusable();
  test_udp_soft_error_counted();
#endif
  test_ingress_close_bounded();
  test_destroy_silent_peer_no_leak();
  TEST_SUMMARY();
  return TEST_EXIT_CODE();
}
