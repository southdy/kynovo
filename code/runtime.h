#ifndef RUNTIME_H
#define RUNTIME_H
#ifdef __cplusplus
extern "C" {
#endif
#ifndef RUNTIME_DEF
#ifdef RUNTIME_STATIC
#define RUNTIME_DEF static
#else
#define RUNTIME_DEF extern
#endif
#endif
typedef int (*runtime_fn)(void *arg);
typedef struct runtime_ctx runtime_ctx;
typedef struct runtime_backend runtime_backend;
RUNTIME_DEF runtime_ctx *runtime_create(const char *name,int n_threads,void (*entry)(runtime_ctx *rt,void *arg),void *entry_arg);
RUNTIME_DEF void runtime_destroy(runtime_ctx *rt);
RUNTIME_DEF void runtime_stop(runtime_ctx *rt);
RUNTIME_DEF int runtime_should_stop(runtime_ctx *rt);
RUNTIME_DEF void runtime_worker_ready(runtime_ctx *rt);
RUNTIME_DEF void runtime_wait_workers_ready(runtime_ctx *rt);
RUNTIME_DEF void runtime_worker_exit(runtime_ctx *rt);
RUNTIME_DEF void runtime_wait_workers_exit(runtime_ctx *rt);
RUNTIME_DEF int runtime_task_post(runtime_ctx *rt,runtime_fn fn,void *arg);
RUNTIME_DEF int runtime_task_poll(runtime_ctx *rt,int timeout_ms,runtime_fn *fn,void **arg);
RUNTIME_DEF int runtime_result_post(runtime_ctx *rt,runtime_fn fn,void *arg);
RUNTIME_DEF int runtime_result_poll(runtime_ctx *rt,int timeout_ms,runtime_fn *fn,void **arg);
RUNTIME_DEF void runtime_drain(runtime_ctx *rt);
typedef struct runtime_gate runtime_gate;
RUNTIME_DEF runtime_gate *runtime_gate_create(int n);
RUNTIME_DEF void runtime_gate_destroy(runtime_gate *g);
RUNTIME_DEF void runtime_gate_arrive(runtime_gate *g);
RUNTIME_DEF void runtime_gate_wait(runtime_gate *g);
RUNTIME_DEF void runtime_gate_open(runtime_gate *g);
RUNTIME_DEF void runtime_msleep(unsigned int ms);
#ifdef __cplusplus
}
#endif
#endif
#if defined(RUNTIME_IMPLEMENTATION)&&!defined(RUNTIME_IMPLEMENTATION_ONCE)
#define RUNTIME_IMPLEMENTATION_ONCE
#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#else
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <pthread.h>
#include <time.h>
#endif
#include <string.h>   /* strcmp in the backend lookup */
/* Bound for every worker handshake/exit wait: a contract-violating worker (never calls
   runtime_worker_ready/exit, or blocks in entry) would otherwise hang the whole process. */
#define RUNTIME_WAIT_MS 30000u
#include <stddef.h>
#include <stdio.h>   /* visible diagnostics for worker-contract violations */
#include <stdlib.h>
#ifndef RUNTIME_MALLOC
#define RUNTIME_MALLOC malloc
#endif
#ifndef RUNTIME_FREE
#define RUNTIME_FREE free

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
#define RUNTIME_MALLOC(n) k_dbg_malloc((size_t)(n),__LINE__,__FILE__)
#define RUNTIME_CALLOC(n,s) k_dbg_calloc((size_t)(n),(size_t)(s),__LINE__,__FILE__)
#define RUNTIME_FREE(p) k_dbg_free_site((void *)(p),__LINE__,__FILE__)
#endif
#endif
#ifndef RUNTIME_CALLOC
#define RUNTIME_CALLOC calloc
#endif

/* ====== backend abstraction (mirror of vfs.h: vtable + name + registry) ======
   A runtime_backend is a self-contained worker-pool implementation selected by
   name at create time, exactly as vfs selects "disk"/"mem" by URI scheme.  The
   runtime_ctx carries only a `be` pointer; the public API dispatches through
   `rt->be`, so the core never branches on WHICH backend is wired.  Two backends
   live side by side and know nothing of each other:
     - "thread" : real OS worker threads (the original implementation).
     - "sync"   : deterministic single-threaded FIFO driven by runtime_drain,
                  for the test harness. */
struct runtime_backend{
  const char *name;
  runtime_ctx *(*create)(runtime_backend *be,int n_threads,void (*entry)(runtime_ctx *rt,void *arg),void *entry_arg);
  int  (*task_post)  (runtime_ctx *rt,runtime_fn fn,void *arg);
  int  (*task_poll)  (runtime_ctx *rt,int timeout_ms,runtime_fn *fn,void **arg);
  int  (*result_post)(runtime_ctx *rt,runtime_fn fn,void *arg);
  int  (*result_poll)(runtime_ctx *rt,int timeout_ms,runtime_fn *fn,void **arg);
  void (*stop)       (runtime_ctx *rt);
  int  (*should_stop)(runtime_ctx *rt);
  void (*worker_ready)(runtime_ctx *rt);
  void (*worker_exit) (runtime_ctx *rt);
  void (*wait_ready)  (runtime_ctx *rt);
  void (*wait_exit)   (runtime_ctx *rt);
  void (*drain)       (runtime_ctx *rt);   /* sync: run entry inline; thread: no-op */
  void (*destroy)     (runtime_ctx *rt);
};

struct runtime_ctx{
  runtime_backend *be;
  int stop;
  void (*entry)(runtime_ctx *rt,void *arg);
  void *entry_arg;
};

/* ================= "thread" backend (the original implementation) ================= */
typedef struct runtime_task runtime_task;
typedef struct runtime_queue runtime_queue;
struct runtime_task{
  runtime_task *next;
  runtime_fn fn;
  void *arg;
};
struct runtime_queue{
  runtime_task *head;
  runtime_task *tail;
  runtime_task *free;
#if defined(_WIN32)
  CRITICAL_SECTION lock;
  HANDLE wake;
#else
  pthread_mutex_t lock;
  pthread_cond_t wake;
#endif
};
typedef struct runtime_thread_ctx runtime_thread_ctx;
struct runtime_thread_ctx{
  runtime_ctx base;
  int n_threads;
  runtime_queue task;
  runtime_queue result;
#if defined(_WIN32)
  HANDLE *threads;
  HANDLE ready_event;
  HANDLE exit_event;
#else
  pthread_t *threads;
  pthread_cond_t ready_cond;
  pthread_cond_t exit_cond;
#endif
  int ready_count;
  int exit_count;
};
#if !defined(_WIN32)
static int queue_poll_wait(runtime_queue *q,int *stop,int timeout_ms){
  if(!*stop&&!q->head){
    if(timeout_ms<0){
      while(!*stop&&!q->head) pthread_cond_wait(&q->wake,&q->lock);
    }else if(timeout_ms>0){
      struct timespec ts;
#if defined(__APPLE__)
      ts.tv_sec=timeout_ms/1000;
      ts.tv_nsec=(timeout_ms%1000)*1000000L;
      while(!*stop&&!q->head){
        if(pthread_cond_timedwait_relative_np(&q->wake,&q->lock,&ts)==ETIMEDOUT) break;
      }
#else
      if(clock_gettime(CLOCK_MONOTONIC,&ts)!=0) return -1;
      ts.tv_sec+=timeout_ms/1000;
      ts.tv_nsec+=(timeout_ms%1000)*1000000L;
      if(ts.tv_nsec>=1000000000L){
        ts.tv_sec+=1;
        ts.tv_nsec-=1000000000L;
      }
      while(!*stop&&!q->head){
        if(pthread_cond_timedwait(&q->wake,&q->lock,&ts)==ETIMEDOUT) break;
      }
#endif
    }
  }
  return 0;
}
#endif
static int queue_post(runtime_queue *q,int *stop,runtime_fn fn,void *arg){
  runtime_task *t;
#if defined(_WIN32)
  EnterCriticalSection(&q->lock);
  if(*stop){ LeaveCriticalSection(&q->lock); return -1; }
#else
  pthread_mutex_lock(&q->lock);
  if(*stop){ pthread_mutex_unlock(&q->lock); return -1; }
#endif
  if(q->free){
    t=q->free;
    q->free=t->next;
  }else{
    t=(runtime_task *)RUNTIME_MALLOC(sizeof(*t));
    if(!t){
#if defined(_WIN32)
      LeaveCriticalSection(&q->lock);
#else
      pthread_mutex_unlock(&q->lock);
#endif
      return -1;
    }
  }
  t->fn=fn;
  t->arg=arg;
  t->next=0;
  if(q->tail) q->tail->next=t;
  else q->head=t;
  q->tail=t;
#if defined(_WIN32)
  LeaveCriticalSection(&q->lock);
  ReleaseSemaphore(q->wake,1,0);
#else
  pthread_cond_signal(&q->wake);
  pthread_mutex_unlock(&q->lock);
#endif
  return 0;
}
static int queue_poll(runtime_queue *q,int *stop,int timeout_ms,runtime_fn *fn,void **arg){
  runtime_task *t;
#if defined(_WIN32)
  if(WaitForSingleObject(q->wake,(timeout_ms<0)?INFINITE:(DWORD)timeout_ms)==WAIT_FAILED) return -1;
  EnterCriticalSection(&q->lock);
  if(*stop&&!q->head){
    LeaveCriticalSection(&q->lock);
    return -1;
  }
#else
  pthread_mutex_lock(&q->lock);
  if(queue_poll_wait(q,stop,timeout_ms)<0){
    pthread_mutex_unlock(&q->lock);
    return -1;
  }
  if(*stop&&!q->head){
    pthread_mutex_unlock(&q->lock);
    return -1;
  }
#endif
  t=q->head;
  if(t){
    q->head=t->next;
    if(!q->head) q->tail=0;
    if(fn) *fn=t->fn;
    if(arg) *arg=t->arg;
    t->next=q->free;
    q->free=t;
  }
#if defined(_WIN32)
  LeaveCriticalSection(&q->lock);
#else
  pthread_mutex_unlock(&q->lock);
#endif
  return t?1:0;
}
static void queue_free(runtime_queue *q){
  while(q->free){
    runtime_task *t=q->free;
    q->free=t->next;
    RUNTIME_FREE(t);
  }
  while(q->head){
    runtime_task *t=q->head;
    q->head=t->next;
    RUNTIME_FREE(t);
  }
}
#if defined(_WIN32)
static void queue_init(runtime_queue *q){
  InitializeCriticalSection(&q->lock);
  q->wake=CreateSemaphoreA(0,0,0x7fffffff,0);
}
static void queue_destroy(runtime_queue *q){
  if(q->wake) CloseHandle(q->wake);
  DeleteCriticalSection(&q->lock);
}
#else
static void queue_init(runtime_queue *q){
  pthread_mutex_init(&q->lock,0);
  pthread_cond_init(&q->wake,0);
}
static void queue_destroy(runtime_queue *q){
  pthread_cond_destroy(&q->wake);
  pthread_mutex_destroy(&q->lock);
}
#endif
static int thread_task_post(runtime_ctx *rt,runtime_fn fn,void *arg){
  runtime_thread_ctx *t=(runtime_thread_ctx *)rt;
  return queue_post(&t->task,&rt->stop,fn,arg);
}
static int thread_task_poll(runtime_ctx *rt,int timeout_ms,runtime_fn *fn,void **arg){
  runtime_thread_ctx *t=(runtime_thread_ctx *)rt;
  return queue_poll(&t->task,&rt->stop,timeout_ms,fn,arg);
}
static int thread_result_post(runtime_ctx *rt,runtime_fn fn,void *arg){
  runtime_thread_ctx *t=(runtime_thread_ctx *)rt;
  return queue_post(&t->result,&rt->stop,fn,arg);
}
static int thread_result_poll(runtime_ctx *rt,int timeout_ms,runtime_fn *fn,void **arg){
  runtime_thread_ctx *t=(runtime_thread_ctx *)rt;
  return queue_poll(&t->result,&rt->stop,timeout_ms,fn,arg);
}
static void thread_stop(runtime_ctx *rt){
  runtime_thread_ctx *t=(runtime_thread_ctx *)rt;
#if defined(_WIN32)
  EnterCriticalSection(&t->task.lock);
  rt->stop=1;
  LeaveCriticalSection(&t->task.lock);
  ReleaseSemaphore(t->task.wake,t->n_threads,0);
  EnterCriticalSection(&t->result.lock);
  rt->stop=1;
  LeaveCriticalSection(&t->result.lock);
  ReleaseSemaphore(t->result.wake,1,0);
#else
  pthread_mutex_lock(&t->task.lock);
  rt->stop=1;
  pthread_cond_broadcast(&t->task.wake);
  pthread_mutex_unlock(&t->task.lock);
  pthread_mutex_lock(&t->result.lock);
  rt->stop=1;
  pthread_cond_signal(&t->result.wake);
  pthread_mutex_unlock(&t->result.lock);
#endif
}
static int thread_should_stop(runtime_ctx *rt){
  runtime_thread_ctx *t=(runtime_thread_ctx *)rt;
  int s;
#if defined(_WIN32)
  EnterCriticalSection(&t->task.lock);
  s=rt->stop;
  LeaveCriticalSection(&t->task.lock);
#else
  pthread_mutex_lock(&t->task.lock);
  s=rt->stop;
  pthread_mutex_unlock(&t->task.lock);
#endif
  return s;
}
#if defined(_WIN32)
static unsigned __stdcall runtime_worker(void *p)
#else
static void *runtime_worker(void *p)
#endif
{
  runtime_ctx *rt=(runtime_ctx *)p;
  rt->entry(rt,rt->entry_arg);
  return 0;
}
static void thread_worker_ready(runtime_ctx *rt){
  runtime_thread_ctx *t=(runtime_thread_ctx *)rt;
#if defined(_WIN32)
  EnterCriticalSection(&t->task.lock);
  t->ready_count++;
  if(t->ready_count==t->n_threads) SetEvent(t->ready_event);
  LeaveCriticalSection(&t->task.lock);
#else
  pthread_mutex_lock(&t->task.lock);
  t->ready_count++;
  if(t->ready_count==t->n_threads) pthread_cond_broadcast(&t->ready_cond);
  pthread_mutex_unlock(&t->task.lock);
#endif
}
static void thread_wait_ready(runtime_ctx *rt){
  runtime_thread_ctx *t=(runtime_thread_ctx *)rt;
#if defined(_WIN32)
  if(WaitForSingleObject(t->ready_event,RUNTIME_WAIT_MS)!=WAIT_OBJECT_0){ fprintf(stderr,"warning: a worker did not report ready within %u ms (worker contract violated)\n",(unsigned)RUNTIME_WAIT_MS); return; }
#else
  pthread_mutex_lock(&t->task.lock);
  while(t->ready_count<t->n_threads) pthread_cond_wait(&t->ready_cond,&t->task.lock);
  pthread_mutex_unlock(&t->task.lock);
#endif
}
static void thread_worker_exit(runtime_ctx *rt){
  runtime_thread_ctx *t=(runtime_thread_ctx *)rt;
#if defined(_WIN32)
  EnterCriticalSection(&t->task.lock);
  t->exit_count++;
  if(t->exit_count==t->n_threads) SetEvent(t->exit_event);
  LeaveCriticalSection(&t->task.lock);
#else
  pthread_mutex_lock(&t->task.lock);
  t->exit_count++;
  if(t->exit_count==t->n_threads) pthread_cond_broadcast(&t->exit_cond);
  pthread_mutex_unlock(&t->task.lock);
#endif
}
static void thread_wait_exit(runtime_ctx *rt){
  runtime_thread_ctx *t=(runtime_thread_ctx *)rt;
#if defined(_WIN32)
  if(WaitForSingleObject(t->exit_event,RUNTIME_WAIT_MS)!=WAIT_OBJECT_0){ fprintf(stderr,"warning: a worker did not exit within %u ms\n",(unsigned)RUNTIME_WAIT_MS); return; }
#else
  pthread_mutex_lock(&t->task.lock);
  while(t->exit_count<t->n_threads) pthread_cond_wait(&t->exit_cond,&t->task.lock);
  pthread_mutex_unlock(&t->task.lock);
#endif
}
static void thread_drain(runtime_ctx *rt){ (void)rt; }  /* a live thread pool has no synchronous turn */
static void thread_destroy(runtime_ctx *rt){
  runtime_thread_ctx *t=(runtime_thread_ctx *)rt;
  thread_stop(rt);
  if(t->threads){
    int i;
    for(i=0;i<t->n_threads;i++){
#if defined(_WIN32)
      if(t->threads[i]){
        if(WaitForSingleObject(t->threads[i],RUNTIME_WAIT_MS)!=WAIT_OBJECT_0){ fprintf(stderr,"warning: worker thread %d did not join within %u ms\n",i,(unsigned)RUNTIME_WAIT_MS); break; }
        CloseHandle(t->threads[i]);
      }
#else
      pthread_join(t->threads[i],0);
#endif
    }
    RUNTIME_FREE(t->threads);
  }
  queue_free(&t->task);
  queue_free(&t->result);
  queue_destroy(&t->task);
  queue_destroy(&t->result);
#if defined(_WIN32)
  if(t->ready_event) CloseHandle(t->ready_event);
  if(t->exit_event) CloseHandle(t->exit_event);
#else
  pthread_cond_destroy(&t->ready_cond);
  pthread_cond_destroy(&t->exit_cond);
#endif
  RUNTIME_FREE(t);
}
static runtime_ctx *thread_create(runtime_backend *be,int n_threads,void (*entry)(runtime_ctx *rt,void *arg),void *entry_arg){
  runtime_thread_ctx *t;
  int i;
  if(n_threads<=0||!entry) return 0;
  t=(runtime_thread_ctx *)RUNTIME_CALLOC(1,sizeof(*t));
  if(!t) return 0;
  t->base.be=be;
  t->base.entry=entry;
  t->base.entry_arg=entry_arg;
  t->n_threads=n_threads;
  queue_init(&t->task);
  queue_init(&t->result);
  t->ready_count=0;
  t->exit_count=0;
#if defined(_WIN32)
  t->threads=(HANDLE *)RUNTIME_CALLOC(n_threads,sizeof(HANDLE));
  t->ready_event=CreateEventA(0,1,0,0);
  t->exit_event=CreateEventA(0,1,0,0);
  if(!t->task.wake||!t->result.wake||!t->ready_event||!t->exit_event){
    thread_destroy((runtime_ctx *)t);
    return 0;
  }
#else
#if !defined(__APPLE__)
  {
    pthread_condattr_t attr;
    if(pthread_condattr_init(&attr)!=0) return -1;
    if(pthread_condattr_setclock(&attr,CLOCK_MONOTONIC)!=0){ pthread_condattr_destroy(&attr); return -1; }   /* a realtime-clock cond would be fed absolute monotonic deadlines */
    pthread_cond_destroy(&t->task.wake);
    pthread_cond_destroy(&t->result.wake);
    pthread_cond_init(&t->task.wake,&attr);
    pthread_cond_init(&t->result.wake,&attr);
    pthread_condattr_destroy(&attr);
  }
#endif
  t->threads=(pthread_t *)RUNTIME_CALLOC(n_threads,sizeof(pthread_t));
  pthread_cond_init(&t->ready_cond,0);
  pthread_cond_init(&t->exit_cond,0);
#endif
  if(!t->threads){
    thread_destroy((runtime_ctx *)t);
    return 0;
  }
  for(i=0;i<n_threads;i++){
#if defined(_WIN32)
    t->threads[i]=(HANDLE)_beginthreadex(0,0,runtime_worker,t,0,0);
    if(!t->threads[i]){
      t->n_threads=i;
      thread_destroy((runtime_ctx *)t);
      return 0;
    }
#else
    if(pthread_create(&t->threads[i],0,runtime_worker,t)){
      t->n_threads=i;
      thread_destroy((runtime_ctx *)t);
      return 0;
    }
#endif
  }
  return (runtime_ctx *)t;
}
static runtime_backend runtime_thread_backend={
  "thread",
  thread_create,
  thread_task_post,thread_task_poll,thread_result_post,thread_result_poll,
  thread_stop,thread_should_stop,
  thread_worker_ready,thread_worker_exit,thread_wait_ready,thread_wait_exit,
  thread_drain,
  thread_destroy
};

/* ================= "sync" backend (deterministic single-threaded FIFO) ================= */
typedef struct runtime_sync_ctx runtime_sync_ctx;
struct runtime_sync_ctx{
  runtime_ctx base;
  runtime_task *task_head;
  runtime_task *task_tail;
  runtime_task *result_head;
  runtime_task *result_tail;
};
static runtime_task *sync_pop(runtime_task **head,runtime_task **tail){
  runtime_task *t=*head;
  if(t){
    *head=t->next;
    if(!*head) *tail=0;
    t->next=0;
  }
  return t;
}
static void sync_push(runtime_task **head,runtime_task **tail,runtime_task *t){
  t->next=0;
  if(*tail) (*tail)->next=t;
  else *head=t;
  *tail=t;
}
static int sync_task_post(runtime_ctx *rt,runtime_fn fn,void *arg){
  runtime_sync_ctx *s=(runtime_sync_ctx *)rt;
  runtime_task *t;
  if(rt->stop) return -1;
  t=(runtime_task *)RUNTIME_MALLOC(sizeof(*t));
  if(!t) return -1;
  t->fn=fn;
  t->arg=arg;
  sync_push(&s->task_head,&s->task_tail,t);
  return 0;
}
/* Non-blocking: pop one task (1), or -1 when empty so the worker entry's loop
   drains the batch and returns.  timeout_ms is irrelevant here and ignored. */
static int sync_task_poll(runtime_ctx *rt,int timeout_ms,runtime_fn *fn,void **arg){
  runtime_sync_ctx *s=(runtime_sync_ctx *)rt;
  runtime_task *t;
  (void)timeout_ms;
  if(rt->stop&&!s->task_head) return -1;
  t=sync_pop(&s->task_head,&s->task_tail);
  if(!t) return -1;
  if(fn) *fn=t->fn;
  if(arg) *arg=t->arg;
  RUNTIME_FREE(t);
  return 1;
}
static int sync_result_post(runtime_ctx *rt,runtime_fn fn,void *arg){
  runtime_sync_ctx *s=(runtime_sync_ctx *)rt;
  runtime_task *t;
  if(rt->stop) return -1;
  t=(runtime_task *)RUNTIME_MALLOC(sizeof(*t));
  if(!t) return -1;
  t->fn=fn;
  t->arg=arg;
  sync_push(&s->result_head,&s->result_tail,t);
  return 0;
}
static int sync_result_poll(runtime_ctx *rt,int timeout_ms,runtime_fn *fn,void **arg){
  runtime_sync_ctx *s=(runtime_sync_ctx *)rt;
  runtime_task *t;
  (void)timeout_ms;
  if(rt->stop&&!s->result_head) return -1;
  t=sync_pop(&s->result_head,&s->result_tail);
  if(!t) return 0;
  if(fn) *fn=t->fn;
  if(arg) *arg=t->arg;
  RUNTIME_FREE(t);
  return 1;
}
static void sync_stop(runtime_ctx *rt){ rt->stop=1; }
static int sync_should_stop(runtime_ctx *rt){ return rt->stop; }
static void sync_worker_ready(runtime_ctx *rt){ (void)rt; }
static void sync_worker_exit(runtime_ctx *rt){ (void)rt; }
static void sync_wait_ready(runtime_ctx *rt){ (void)rt; }
static void sync_wait_exit(runtime_ctx *rt){ (void)rt; }
/* Run the worker entry inline over the current task batch: the entry drains the
   queue (task_poll returns -1 when empty) and posts results, all on the caller's
   thread: the deterministic counterpart of one background-worker turn. */
static void sync_drain(runtime_ctx *rt){
  rt->entry(rt,rt->entry_arg);
}
static void sync_destroy(runtime_ctx *rt){
  runtime_sync_ctx *s=(runtime_sync_ctx *)rt;
  runtime_task *t;
  while((t=sync_pop(&s->task_head,&s->task_tail))) RUNTIME_FREE(t);
  while((t=sync_pop(&s->result_head,&s->result_tail))) RUNTIME_FREE(t);
  RUNTIME_FREE(s);
}
static runtime_ctx *sync_create(runtime_backend *be,int n_threads,void (*entry)(runtime_ctx *rt,void *arg),void *entry_arg){
  runtime_sync_ctx *s;
  (void)n_threads;  /* a deterministic backend has no worker threads */
  if(!entry) return 0;
  s=(runtime_sync_ctx *)RUNTIME_CALLOC(1,sizeof(*s));
  if(!s) return 0;
  s->base.be=be;
  s->base.entry=entry;
  s->base.entry_arg=entry_arg;
  return (runtime_ctx *)s;
}
static runtime_backend runtime_sync_backend={
  "sync",
  sync_create,
  sync_task_post,sync_task_poll,sync_result_post,sync_result_poll,
  sync_stop,sync_should_stop,
  sync_worker_ready,sync_worker_exit,sync_wait_ready,sync_wait_exit,
  sync_drain,
  sync_destroy
};

/* ================= registry + route (mirror of vfs_list / vfs_route) ================= */
static runtime_backend *runtime_list[]={(runtime_backend *)&runtime_thread_backend,(runtime_backend *)&runtime_sync_backend,0};
static runtime_backend *runtime_route(const char *name){
  runtime_backend **bp;
  if(!name) return 0;
  for(bp=runtime_list;*bp;bp++){
    if(strcmp((*bp)->name,name)==0) return *bp;
  }
  return 0;
}

/* ================= public API: route + dispatch via rt->be ================= */
RUNTIME_DEF runtime_ctx *runtime_create(const char *name,int n_threads,void (*entry)(runtime_ctx *rt,void *arg),void *entry_arg){
  runtime_backend *be=runtime_route(name);
  return be?be->create(be,n_threads,entry,entry_arg):0;
}
RUNTIME_DEF int runtime_task_post(runtime_ctx *rt,runtime_fn fn,void *arg){
  if(!rt||!rt->be) return -1;
  return rt->be->task_post(rt,fn,arg);
}
RUNTIME_DEF int runtime_task_poll(runtime_ctx *rt,int timeout_ms,runtime_fn *fn,void **arg){
  if(!rt||!rt->be) return -1;
  return rt->be->task_poll(rt,timeout_ms,fn,arg);
}
RUNTIME_DEF int runtime_result_post(runtime_ctx *rt,runtime_fn fn,void *arg){
  if(!rt||!rt->be) return -1;
  return rt->be->result_post(rt,fn,arg);
}
RUNTIME_DEF int runtime_result_poll(runtime_ctx *rt,int timeout_ms,runtime_fn *fn,void **arg){
  if(!rt||!rt->be) return -1;
  return rt->be->result_poll(rt,timeout_ms,fn,arg);
}
RUNTIME_DEF void runtime_stop(runtime_ctx *rt){
  if(!rt||!rt->be) return;
  rt->be->stop(rt);
}
RUNTIME_DEF int runtime_should_stop(runtime_ctx *rt){
  if(!rt||!rt->be) return 1;
  return rt->be->should_stop(rt);
}
RUNTIME_DEF void runtime_worker_ready(runtime_ctx *rt){
  if(!rt||!rt->be) return;
  rt->be->worker_ready(rt);
}
RUNTIME_DEF void runtime_worker_exit(runtime_ctx *rt){
  if(!rt||!rt->be) return;
  rt->be->worker_exit(rt);
}
RUNTIME_DEF void runtime_wait_workers_ready(runtime_ctx *rt){
  if(!rt||!rt->be) return;
  rt->be->wait_ready(rt);
}
RUNTIME_DEF void runtime_wait_workers_exit(runtime_ctx *rt){
  if(!rt||!rt->be) return;
  rt->be->wait_exit(rt);
}
RUNTIME_DEF void runtime_drain(runtime_ctx *rt){
  if(!rt||!rt->be) return;
  rt->be->drain(rt);
}
RUNTIME_DEF void runtime_destroy(runtime_ctx *rt){
  if(!rt||!rt->be) return;
  rt->be->destroy(rt);
}

/* ================= gates (unchanged thread utility) ================= */
struct runtime_gate{
  int n;
  int count;
#if defined(_WIN32)
  CRITICAL_SECTION lock;
  HANDLE arrived_event;
  unsigned int gen;
  unsigned int parity;
  HANDLE go[2];
#else
  pthread_mutex_t lock;
  pthread_cond_t arrived_cond;
  unsigned int gen;
#endif
};
RUNTIME_DEF runtime_gate *runtime_gate_create(int n){
  runtime_gate *g;
  if(n<=0) return 0;
  g=(runtime_gate *)RUNTIME_CALLOC(1,sizeof(*g));
  if(!g) return 0;
  g->n=n;
#if defined(_WIN32)
  InitializeCriticalSection(&g->lock);
  g->arrived_event=CreateEventA(0,1,0,0);
  g->go[0]=CreateEventA(0,1,0,0);
  g->go[1]=CreateEventA(0,1,0,0);
  if(!g->arrived_event||!g->go[0]||!g->go[1]){
    runtime_gate_destroy(g);
    return 0;
  }
#else
  pthread_mutex_init(&g->lock,0);
  pthread_cond_init(&g->arrived_cond,0);
#endif
  return g;
}
RUNTIME_DEF void runtime_gate_destroy(runtime_gate *g){
  if(!g) return;
#if defined(_WIN32)
  if(g->arrived_event) CloseHandle(g->arrived_event);
  if(g->go[0]) CloseHandle(g->go[0]);
  if(g->go[1]) CloseHandle(g->go[1]);
  DeleteCriticalSection(&g->lock);
#else
  pthread_mutex_destroy(&g->lock);
  pthread_cond_destroy(&g->arrived_cond);
#endif
  RUNTIME_FREE(g);
}
RUNTIME_DEF void runtime_gate_arrive(runtime_gate *g){
  unsigned int gen;
#if defined(_WIN32)
  unsigned int p;
  if(!g) return;
  EnterCriticalSection(&g->lock);
  gen=g->gen;
  p=g->parity;
  g->count++;
  if(g->count==g->n) SetEvent(g->arrived_event);
  LeaveCriticalSection(&g->lock);
  for(;;){
    WaitForSingleObject(g->go[p],INFINITE);
    EnterCriticalSection(&g->lock);
    if(g->gen!=gen){ LeaveCriticalSection(&g->lock); break; }
    LeaveCriticalSection(&g->lock);
  }
#else
  if(!g) return;
  pthread_mutex_lock(&g->lock);
  gen=g->gen;
  g->count++;
  if(g->count==g->n) pthread_cond_broadcast(&g->arrived_cond);
  while(g->gen==gen) pthread_cond_wait(&g->arrived_cond,&g->lock);
  pthread_mutex_unlock(&g->lock);
#endif
}
RUNTIME_DEF void runtime_gate_wait(runtime_gate *g){
  if(!g) return;
#if defined(_WIN32)
  WaitForSingleObject(g->arrived_event,INFINITE);
#else
  pthread_mutex_lock(&g->lock);
  while(g->count<g->n) pthread_cond_wait(&g->arrived_cond,&g->lock);
  pthread_mutex_unlock(&g->lock);
#endif
}
RUNTIME_DEF void runtime_gate_open(runtime_gate *g){
#if defined(_WIN32)
  unsigned int p;
  if(!g) return;
  EnterCriticalSection(&g->lock);
  p=g->parity;
  ResetEvent(g->arrived_event);
  ResetEvent(g->go[1-p]);
  g->gen++;
  g->count=0;
  SetEvent(g->go[p]);
  g->parity=1-p;
  LeaveCriticalSection(&g->lock);
#else
  if(!g) return;
  pthread_mutex_lock(&g->lock);
  g->gen++;
  g->count=0;
  pthread_cond_broadcast(&g->arrived_cond);
  pthread_mutex_unlock(&g->lock);
#endif
}
RUNTIME_DEF void runtime_msleep(unsigned int ms){
#if defined(_WIN32)
  Sleep((DWORD)ms);
#else
  struct timespec ts;
  ts.tv_sec=(time_t)(ms/1000);
  ts.tv_nsec=(long)(ms%1000)*1000000L;
  nanosleep(&ts,0);
#endif
}
#endif

