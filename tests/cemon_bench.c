/* tests/cemon_bench.c -- cemon + runtime layered benchmark and stress test.

   Layered so it can answer three questions:
     Q1 mechanism limit : how fast is cemon itself?
     Q2 attribution     : of the real delay, how much is cemon vs the OS?
     Q3 stress          : does cemon degrade under load?

   Tests (each creates and destroys its own cemon loop):
     0  pure OS wake baseline   CreateEvent/SetEvent + WaitForSingleObject,
                                cemon is not involved at all (Q2 reference)
     1  cemon wake latency      no-cooperation: every post carries its own
                                timestamp, the worker never waits for that
                                specific post (only windowed backpressure),
                                so the measured delay is not helped by the
                                producer yielding on the consumer's behalf
     2  attribution             test1.median - test0.median = cemon overhead
     3  throughput decompose    producer rate | consumer drain rate
     4  steady-state throughput poll(10) blocking + windowed producer, 2s
     5  timer accuracy          cemon_after(1ms) vs raw Sleep(1) reference
     6  stress: queue full      cemon_post rejected at CEMON_POSTQ_LIMIT
     7  stress: multi-producer  runtime n_threads=4 under contention

   C89.  build: ./build.sh cemon-bench */

#define CEMON_IMPLEMENTATION
#include "../code/cemon.h"
#define RUNTIME_IMPLEMENTATION
#include "../code/runtime.h"
#include "../code/kbase.h"
#include "../tools/bench_env.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#define BENCH_YIELD() SwitchToThread()
#else
#include <sched.h>
#include <time.h>
#define BENCH_YIELD() sched_yield()
#endif

/* ========================= helpers ========================= */

static k_u64 bench_now(void){
  k_u64 t=0;
  k_monotonic_us(&t);
  return t;
}

static int bench_cmp_u64(const void *a,const void *b){
  k_u64 x=*(const k_u64 *)a,y=*(const k_u64 *)b;
  return x<y?-1:(x>y?1:0);
}

/* sorts in place, prints median/p99/min/avg, returns the median */
static k_u64 bench_report(const char *label,k_u64 *samples,int n){
  k_u64 minv,median,p99,total;
  int i;
  if(n<=0){ printf("%-28s no samples\n",label); return 0; }
  qsort(samples,(size_t)n,sizeof(samples[0]),bench_cmp_u64);
  minv=samples[0];
  median=samples[n/2];
  p99=samples[(int)(((k_u64)n*99u)/100u)];
  total=0;
  for(i=0;i<n;i++) total+=samples[i];
  printf("%-28s n=%-6d median=%-8" K_U64_FMT " p99=%-8" K_U64_FMT " min=%-8" K_U64_FMT " avg=%" K_U64_FMT " us\n",
         label,n,(k_u64)median,(k_u64)p99,
         (k_u64)minv,(k_u64)(total/(k_u64)n));
  return median;
}

/* run one latency test with a fresh loop */
static int bench_wake(int window,k_u64 *samples,int n);

/* ========================= test 0: pure OS wake ========================= */

#ifdef _WIN32
typedef struct{ HANDLE ev; HANDLE ack; } os_wake;
static void os_wake_init(os_wake *w){ w->ev=CreateEventA(0,0,0,0); w->ack=CreateEventA(0,0,0,0); }
static void os_wake_free(os_wake *w){ CloseHandle(w->ev); CloseHandle(w->ack); }
static void os_signal(os_wake *w){ SetEvent(w->ev); }
static void os_wait(os_wake *w){ WaitForSingleObject(w->ev,INFINITE); }
static void os_ack_signal(os_wake *w){ SetEvent(w->ack); }
static void os_ack_wait(os_wake *w){ WaitForSingleObject(w->ack,INFINITE); }
#else
typedef struct{ pthread_mutex_t m; pthread_cond_t ev; pthread_cond_t ack; int ev_f; int ack_f; } os_wake;
static void os_wake_init(os_wake *w){ pthread_mutex_init(&w->m,0); pthread_cond_init(&w->ev,0); pthread_cond_init(&w->ack,0); w->ev_f=0; w->ack_f=0; }
static void os_wake_free(os_wake *w){ pthread_mutex_destroy(&w->m); pthread_cond_destroy(&w->ev); pthread_cond_destroy(&w->ack); }
static void os_signal(os_wake *w){ pthread_mutex_lock(&w->m); w->ev_f=1; pthread_cond_signal(&w->ev); pthread_mutex_unlock(&w->m); }
static void os_wait(os_wake *w){ pthread_mutex_lock(&w->m); while(!w->ev_f) pthread_cond_wait(&w->ev,&w->m); w->ev_f=0; pthread_mutex_unlock(&w->m); }
static void os_ack_signal(os_wake *w){ pthread_mutex_lock(&w->m); w->ack_f=1; pthread_cond_signal(&w->ack); pthread_mutex_unlock(&w->m); }
static void os_ack_wait(os_wake *w){ pthread_mutex_lock(&w->m); while(!w->ack_f) pthread_cond_wait(&w->ack,&w->m); w->ack_f=0; pthread_mutex_unlock(&w->m); }
#endif

typedef struct{
  os_wake w;
  k_u64 signal_us;
  k_u64 *samples;
  volatile int i;
  volatile int n;
} os_ctx;

static void os_worker(runtime_ctx *rt,void *arg){
  os_ctx *ctx=(os_ctx *)arg;
  runtime_worker_ready(rt);
  while(!runtime_should_stop(rt)&&ctx->i<ctx->n){
    ctx->signal_us=bench_now();
    os_signal(&ctx->w);
    os_ack_wait(&ctx->w);
  }
  runtime_worker_exit(rt);
}

static int bench_os_wake(k_u64 *samples,int n){
  os_ctx ctx;
  runtime_ctx *rt;
  memset(&ctx,0,sizeof(ctx));
  ctx.samples=samples; ctx.n=n;
  os_wake_init(&ctx.w);
  rt=runtime_create("thread",1,os_worker,&ctx);
  if(!rt){ os_wake_free(&ctx.w); return 0; }
  runtime_wait_workers_ready(rt);
  while(ctx.i<n){
    os_wait(&ctx.w);
    samples[ctx.i]=bench_now()-ctx.signal_us;
    ctx.i++;
    os_ack_signal(&ctx.w);
  }
  runtime_stop(rt);
  runtime_wait_workers_exit(rt);
  runtime_destroy(rt);
  os_wake_free(&ctx.w);
  return n;
}

/* ========================= test 1: cemon wake, no cooperation ========================= */

typedef struct{
  cemon *loop;
  volatile int posted;
  volatile int handled;
  k_u64 *samples;
  int n;
  int window;
} lat_ctx;

typedef struct{
  k_u64 post_us;
  void *ctx;
} lat_node;

static void lat_cb(cemon *loop,void *ud){
  lat_node *node=(lat_node *)ud;
  lat_ctx *ctx=(lat_ctx *)node->ctx;
  (void)loop;
  if(ctx->handled<ctx->n){
    k_u64 now;
    k_monotonic_us(&now);
    ctx->samples[ctx->handled]=now>node->post_us?now-node->post_us:0u;
    ctx->handled++;
  }
  free(node);
}

static void lat_worker(runtime_ctx *rt,void *arg){
  lat_ctx *ctx=(lat_ctx *)arg;
  runtime_worker_ready(rt);
  while(!runtime_should_stop(rt)&&ctx->posted<ctx->n){
    lat_node *node;
    if(ctx->posted-ctx->handled>=ctx->window){ BENCH_YIELD(); continue; }
    node=(lat_node *)malloc(sizeof(*node));
    if(!node) break;
    node->ctx=ctx;
    node->post_us=bench_now();
    if(cemon_post(ctx->loop,lat_cb,node)!=0){ free(node); break; }
    ctx->posted++;
  }
  runtime_worker_exit(rt);
}

static int bench_wake(int window,k_u64 *samples,int n){
  lat_ctx ctx;
  runtime_ctx *rt;
  cemon *loop;
  k_u64 t0;
  memset(&ctx,0,sizeof(ctx));
  ctx.samples=samples; ctx.n=n; ctx.window=window;
  loop=cemon_create();
  if(!loop) return 0;
  ctx.loop=loop;
  rt=runtime_create("thread",1,lat_worker,&ctx);
  if(!rt){ cemon_destroy(loop); return 0; }
  runtime_wait_workers_ready(rt);
  /* bounded: if a worker exits early without posting, never spin forever */
  t0=bench_now();
  while(ctx.handled<n&&(bench_now()-t0)<30000000u) cemon_poll(loop,10);
  if(ctx.handled<n) printf("   [warn] latency test stopped early: handled=%d of n=%d after 30s\n",ctx.handled,n);
  runtime_stop(rt);
  runtime_wait_workers_exit(rt);
  runtime_destroy(rt);
  cemon_destroy(loop);
  return ctx.handled;
}

/* ========================= test 3: producer / consumer rates ========================= */

typedef struct{
  cemon *loop;
  volatile int posted;
  volatile int handled;
  k_u64 t0,t1;
  int n;
} rate_ctx;

static void noop_cb(cemon *loop,void *ud){
  rate_ctx *ctx=(rate_ctx *)ud;
  (void)loop;
  ctx->handled++;
}

static void rate_worker(runtime_ctx *rt,void *arg){
  rate_ctx *ctx=(rate_ctx *)arg;
  int i;
  runtime_worker_ready(rt);
  ctx->t0=bench_now();
  for(i=0;i<ctx->n;i++){
    if(cemon_post(ctx->loop,noop_cb,ctx)!=0) break;
    ctx->posted++;
  }
  ctx->t1=bench_now();
  runtime_worker_exit(rt);
}

/* ========================= test 4: steady-state throughput ========================= */

typedef struct{
  cemon *loop;
  volatile int posted;
  volatile int handled;
  int window;
  volatile int stop;
} steady_ctx;

static void steady_cb(cemon *loop,void *ud){
  steady_ctx *ctx=(steady_ctx *)ud;
  (void)loop;
  ctx->handled++;
}

static void steady_worker(runtime_ctx *rt,void *arg){
  steady_ctx *ctx=(steady_ctx *)arg;
  runtime_worker_ready(rt);
  while(!runtime_should_stop(rt)&&!ctx->stop){
    if(ctx->posted-ctx->handled>=ctx->window){ BENCH_YIELD(); continue; }
    if(cemon_post(ctx->loop,steady_cb,ctx)!=0) continue;
    ctx->posted++;
  }
  runtime_worker_exit(rt);
}

/* ========================= test 5: timer ========================= */

typedef struct{
  volatile int fired;
  k_u64 fire_us;
} timer_ctx;

static void timer_cb(cemon *loop,void *ud){
  timer_ctx *ctx=(timer_ctx *)ud;
  (void)loop;
  k_monotonic_us(&ctx->fire_us);
  ctx->fired=1;
}

/* ========================= test 6: queue full ========================= */

typedef struct{
  cemon *loop;
  volatile int posted;
  volatile int rejects;
  int attempts;
} qfull_ctx;

static void qfull_cb(cemon *loop,void *ud){
  (void)loop; (void)ud;
}

static void qfull_worker(runtime_ctx *rt,void *arg){
  qfull_ctx *ctx=(qfull_ctx *)arg;
  int i;
  runtime_worker_ready(rt);
  for(i=0;i<ctx->attempts;i++){
    if(cemon_post(ctx->loop,qfull_cb,0)!=0) ctx->rejects++;
    else ctx->posted++;
  }
  runtime_worker_exit(rt);
}

/* ========================= test 7: multi producer ========================= */

typedef struct{
  cemon *loop;
  volatile int posted;
  volatile int handled;
  k_u64 *samples;
  int n;
  int window;
} mp_ctx;

static void mp_cb(cemon *loop,void *ud){
  lat_node *node=(lat_node *)ud;
  mp_ctx *ctx=(mp_ctx *)node->ctx;
  (void)loop;
  if(ctx->handled<ctx->n){
    k_u64 now;
    k_monotonic_us(&now);
    ctx->samples[ctx->handled]=now>node->post_us?now-node->post_us:0u;
    ctx->handled++;
  }
  free(node);
}

static void mp_worker(runtime_ctx *rt,void *arg){
  mp_ctx *ctx=(mp_ctx *)arg;
  runtime_worker_ready(rt);
  while(!runtime_should_stop(rt)&&ctx->posted<ctx->n){
    lat_node *node;
    if(ctx->posted-ctx->handled>=ctx->window){ BENCH_YIELD(); continue; }
    node=(lat_node *)malloc(sizeof(*node));
    if(!node) break;
    node->ctx=ctx;
    node->post_us=bench_now();
    if(cemon_post(ctx->loop,mp_cb,node)!=0){ free(node); break; }
    ctx->posted++;
  }
  runtime_worker_exit(rt);
}

/* ========================= main ========================= */

int main(void){
  k_u64 *samples;
  k_u64 med_os,med_cemon,med_cemon_big;
  k_u64 t0,gran_before;
  int n,i,in_window,late;
  char params[160];
  n=2000;
  gran_before=bench_timer_granularity_us();
#ifdef _WIN32
  /* Pin the timer granularity. Windows defaults to a ~15.6ms tick, under which
     a "1ms" timer measures as 8-15ms and hides real jitter; a timing benchmark
     must report the granularity it measured under. kdbsvr raises it too, since
     its flush_timeout_ms / poll_ms are meaningless at the default tick. */
  timeBeginPeriod(1);
#endif
  sprintf(params,"tests=0..7 n=%d window=1/64/256 sample=per-callback timestamp",n);
  bench_env_banner("cemon_bench",params);
  printf("   timer    : before timeBeginPeriod(1) = %" K_U64_FMT " us, after = %" K_U64_FMT " us (measured)\n",
         gran_before,bench_timer_granularity_us());

  samples=(k_u64 *)malloc((size_t)n*sizeof(k_u64));
  if(!samples){ printf("oom\n"); return 1; }

  /* ---- test 0: OS baseline ---- */
  i=bench_os_wake(samples,n);
  med_os=bench_report("[0] OS wake baseline",samples,i);

  /* ---- test 1: cemon wake, two backlog windows ---- */
  i=bench_wake(1,samples,n);
  med_cemon=bench_report("[1] cemon wake (win=1)",samples,i);

  i=bench_wake(256,samples,n);
  med_cemon_big=bench_report("[1] cemon wake (win=256)",samples,i);

  /* ---- test 2: attribution ---- */
  printf("[2] cemon overhead (median)     win=1: %" K_U64_FMT " us   win=256: %" K_U64_FMT " us  (cemon - OS)\n",
         (k_u64)(med_cemon>med_os?med_cemon-med_os:0),
         (k_u64)(med_cemon_big>med_os?med_cemon_big-med_os:0));

  free(samples);

  /* ---- test 3: producer vs consumer ---- */
  {
    cemon *loop;
    runtime_ctx *rt;
    rate_ctx ctx;
    k_u64 prod_us,cons_us,t0;
    int m=4000;
    memset(&ctx,0,sizeof(ctx));
    ctx.n=m;
    loop=cemon_create();
    if(loop){
      ctx.loop=loop;
      rt=runtime_create("thread",1,rate_worker,&ctx);
      if(rt){
        runtime_wait_workers_ready(rt);
        runtime_wait_workers_exit(rt);   /* worker finished posting */
        prod_us=ctx.t1>ctx.t0?ctx.t1-ctx.t0:0u;
        printf("[3] producer rate              n=%d  total=%" K_U64_FMT " us  %" K_U64_FMT " posts/s\n",
               ctx.posted,(k_u64)prod_us,
               prod_us?(k_u64)((k_u64)ctx.posted*1000000u/prod_us):0u);
        /* drain what the producer queued */
        t0=bench_now();
        while(ctx.handled<ctx.posted) cemon_poll(loop,0);
        cons_us=bench_now()-t0;
        printf("[3] consumer drain rate        n=%d  total=%" K_U64_FMT " us  %" K_U64_FMT " cbs/s\n",
               ctx.handled,(k_u64)cons_us,
               cons_us?(k_u64)((k_u64)ctx.handled*1000000u/cons_us):0u);
        runtime_stop(rt);
        runtime_destroy(rt);
      }
      cemon_destroy(loop);
    }
  }

  /* ---- test 4: steady-state throughput (2s) ---- */
  {
    cemon *loop;
    runtime_ctx *rt;
    steady_ctx ctx;
    k_u64 t0,t1,dur;
    memset(&ctx,0,sizeof(ctx));
    ctx.window=64;
    loop=cemon_create();
    if(loop){
      ctx.loop=loop;
      rt=runtime_create("thread",1,steady_worker,&ctx);
      if(rt){
        runtime_wait_workers_ready(rt);
        t0=bench_now();
        do{ cemon_poll(loop,10); t1=bench_now(); }while(t1-t0<2000000u);
        dur=t1-t0;
        in_window=ctx.handled;      /* numerator: only callbacks completed in the window */
        ctx.stop=1;
        while(ctx.posted>ctx.handled) cemon_poll(loop,0);
        late=ctx.handled-in_window; /* drained after the window: reported separately, not counted */
        printf("[4] steady throughput          n=%d  dur=%" K_U64_FMT " us  %" K_U64_FMT " posts/s  (in-window=%d, drained-after-window=%d, poll(10), win=64)\n",
               in_window,(k_u64)dur,
               dur?(k_u64)((k_u64)in_window*1000000u/dur):0u,in_window,late);
        runtime_stop(rt);
        runtime_wait_workers_exit(rt);
        runtime_destroy(rt);
      }
      cemon_destroy(loop);
    }
  }

  /* ---- test 5: timer accuracy: cemon_after vs raw Sleep ---- */
  {
    cemon *loop;
    timer_ctx tc;
    cemon_timer *tm;
    k_u64 prev,d;
    int m=500;
    samples=(k_u64 *)malloc((size_t)m*sizeof(k_u64));
    if(samples){
      loop=cemon_create();
      if(loop){
        memset(&tc,0,sizeof(tc));
        tm=cemon_after(loop,1,1,timer_cb,&tc);
        if(tm){
          prev=0; i=0;
          while(i<m-1){
            cemon_poll(loop,100);
            if(tc.fired){
              k_u64 now=tc.fire_us;
              if(prev){ d=now>prev?now-prev:0u; samples[i++]=d; }
              prev=now; tc.fired=0;
            }
          }
          cemon_timer_stop(tm);
          bench_report("[5] cemon_after(1ms)",samples,i);
        }
        cemon_destroy(loop);
      }
      /* raw Sleep(1) reference on the same machine */
      prev=0; i=0;
      while(i<m-1){
#ifdef _WIN32
        Sleep(1);
#else
        { struct timespec ts; ts.tv_sec=0; ts.tv_nsec=1000000L; nanosleep(&ts,0); }
#endif
        { k_u64 now=bench_now(); if(prev){ d=now>prev?now-prev:0u; samples[i++]=d; } prev=now; }
      }
      bench_report("[5] raw Sleep(1) reference",samples,i);
      free(samples);
    }
  }

  /* ---- test 6: queue full ---- */
  {
    cemon *loop;
    runtime_ctx *rt;
    qfull_ctx ctx;
    memset(&ctx,0,sizeof(ctx));
    ctx.attempts=6000;
    loop=cemon_create();
    if(loop){
      ctx.loop=loop;
      rt=runtime_create("thread",1,qfull_worker,&ctx);
      if(rt){
        runtime_wait_workers_ready(rt);
        runtime_wait_workers_exit(rt);
        printf("[6] queue full                 attempts=%d  accepted=%d  rejected=%d\n",
               ctx.attempts,ctx.posted,ctx.rejects);
        runtime_destroy(rt);
      }
      cemon_destroy(loop);
    }
  }

  /* ---- test 7: multi-producer ---- */
  {
    cemon *loop;
    runtime_ctx *rt;
    mp_ctx ctx;
    int nthreads=4;
    n=4000;
    samples=(k_u64 *)malloc((size_t)n*sizeof(k_u64));
    if(samples){
      memset(&ctx,0,sizeof(ctx));
      ctx.samples=samples; ctx.n=n; ctx.window=64;
      loop=cemon_create();
      if(loop){
        ctx.loop=loop;
        rt=runtime_create("thread",nthreads,mp_worker,&ctx);
        if(rt){
          runtime_wait_workers_ready(rt);
          /* bounded: a worker that dies early must not hang the benchmark */
          t0=bench_now();
          while(ctx.handled<n&&(bench_now()-t0)<60000000u) cemon_poll(loop,10);
          if(ctx.handled<n) printf("   [warn] multi-producer stopped early: handled=%d of n=%d after 60s\n",ctx.handled,n);
          runtime_stop(rt);
          runtime_wait_workers_exit(rt);
          runtime_destroy(rt);
          bench_report("[7] multi-producer (4)",samples,ctx.handled);
        }
        cemon_destroy(loop);
      }
      free(samples);
    }
  }

#ifdef _WIN32
  timeEndPeriod(1);
#endif
  return 0;
}
