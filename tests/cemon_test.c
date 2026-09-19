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
#if defined(_WIN32)
#include <winsock2.h>
#endif
#include <pthread.h>
#if defined(_WIN32)
#include <windows.h>
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
static void test_destroy_bounded_drain(void){
  cemon *loop;
  cemon_socket *sock;
  probe_fd fd;
  TEST_BEGIN("cemon_destroy returns with a never-completing socket ref (bounded drain)");
  loop=cemon_create();
  TEST_ASSERT(loop!=0,"loop created");
  fd=socket(AF_INET,SOCK_STREAM,0);
  TEST_ASSERT(fd!=PROBE_FD_INVALID,"tcp fd");
  sock=cemon_sock_new(loop,(cemon_fd)fd,CEMON_TCP_SOCK,0,0);
  TEST_ASSERT(sock!=0,"socket created");
  cemon_sock_hold(sock);                      /* the "holder that never completes" */
  cemon_socket_die(sock,0,0);                 /* unlinked + fd closed, but not freed */
  cemon_destroy(loop);                        /* must return instead of hanging */
  TEST_ASSERT(1,"destroy returned");
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
static void test_owner_binding(void){
  pthread_t th;
  cemon *loop;
  TEST_BEGIN("cemon: owner is explicitly bindable and violations are reported");
  loop=cemon_create();
  TEST_ASSERT(loop!=0,"loop created");
  g_own_loop=loop;
  g_own_bind_rc=99; g_own_is_owner=99; g_own_poll_rc=99; g_own_destroy_rc=99;
  TEST_ASSERT_I64_EQ(cemon_is_owner(loop),1,"an unbound loop accepts the caller as owner");
  TEST_ASSERT_I64_EQ(cemon_bind_owner(loop),0,"explicit bind from this thread succeeds");
  TEST_ASSERT_I64_EQ(cemon_is_owner(loop),1,"this thread is the owner");
  TEST_ASSERT(pthread_create(&th,0,owner_probe_thread,0)==0,"probe thread started");
  pthread_join(th,0);
  TEST_ASSERT_I64_EQ(g_own_bind_rc,-1,"another thread cannot bind an owned loop");
  TEST_ASSERT_I64_EQ(g_own_is_owner,0,"another thread is not the owner");
  TEST_ASSERT_I64_EQ(g_own_poll_rc,-1,"another thread cannot poll");
  TEST_ASSERT_I64_EQ(g_own_destroy_rc,-1,"another thread cannot destroy (was a silent no-op)");
  TEST_ASSERT_I64_EQ(cemon_destroy(loop),0,"the owner can destroy");
  TEST_END();
}

int main(void){
#if defined(_WIN32)
  WSADATA wsa;
  WSAStartup(MAKEWORD(2,2),&wsa);
#endif
  TEST_PLAN(5);
  test_shutdown_write_open();
  test_shutdown_write_closed();
  test_destroy_bounded_drain();
  test_control_frame_priority();
  test_owner_binding();
  TEST_SUMMARY();
  return TEST_EXIT_CODE();
}
