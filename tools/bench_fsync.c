/* tools/bench_fsync.c -- fsync microbenchmark: measure the real vfs_sync cost
   (empty and per-batch) to derive the flush batching window (flush_timeout_ms /
   flush_item_limit).  Single file, C89, MSVC 6.0 compatible.

   build: gcc -std=c89 -O2 -Wall -Wextra -Wdeclaration-after-statement \
          -Wno-unused-function tools/bench_fsync.c -o build/bench_fsync.exe

   Method (the old version scanned the batch sizes in ONE fixed descending pass,
   so cache/thermal drift between batch sizes was indistinguishable from the
   batch-size effect - and that is exactly the number the flush window is derived
   from):
     - BENCH_ROUNDS rounds, each running every batch size once;
     - the batch order is reshuffled per round (deterministic PRNG), so drift is
       spread across all batch sizes instead of being charged to the later ones;
     - each cell is the MEDIAN of BENCH_N samples (fsync has heavy jitter tails,
       so the median is the planning number and p99 is the tail to budget for);
     - the reported row is the median ACROSS rounds with the per-round spread, so
       an unstable cell is visible rather than averaged away. */

#define VFS_IMPLEMENTATION
#include "../code/vfs.h"
#include "../code/kbase.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../tools/bench_env.h"

#define BENCH_N 200
#define BENCH_ROUNDS 5
#define BENCH_BATCHES 5
#define BENCH_ITEM 100u

static int bench_batches[BENCH_BATCHES]={1024,100,10,1,0};

static int bench_cmp_u64(const void *a,const void *b){
  k_u64 x=*(const k_u64 *)a,y=*(const k_u64 *)b;
  return x<y?-1:(x>y?1:0);
}
static unsigned int bench_rng=0x1f123bb5u;
static unsigned int bench_rand(void){
  bench_rng^=bench_rng<<13;
  bench_rng^=bench_rng>>17;
  bench_rng^=bench_rng<<5;
  return bench_rng;
}
static void bench_shuffle(int *a,int n){
  int i,j,t;
  for(i=n-1;i>0;i--){
    j=(int)(bench_rand()%(unsigned int)(i+1));
    t=a[i];
    a[i]=a[j];
    a[j]=t;
  }
}

int main(void){
  vfs_file *f;
  static k_u64 samples[BENCH_N];
  static k_u64 round_median[BENCH_ROUNDS][BENCH_BATCHES];
  static k_u64 round_p99[BENCH_ROUNDS][BENCH_BATCHES];
  static k_u64 across[BENCH_ROUNDS];
  k_u64 t0,t1,median,p99,minv,maxv;
  unsigned char buf[BENCH_ITEM];
  int order[BENCH_BATCHES];
  char params[160];
  int r,b,i,j,nb,bi,slot;
  vfs_u64 off;
  memset(buf,0x5a,sizeof(buf));
  sprintf(params,"file=disk://bench-fsync.tmp item=%u B samples=%d per cell rounds=%d order=shuffled-per-round",
          (unsigned)BENCH_ITEM,BENCH_N,BENCH_ROUNDS);
  bench_env_banner("bench_fsync",params);
  f=vfs_open("disk://bench-fsync.tmp");
  if(!f){ printf("open failed\n"); return 1; }
  /* warm-up: one write+sync so first-sample file-create cost is excluded */
  vfs_write(f,0,buf,BENCH_ITEM);
  vfs_sync(f);
  for(r=0;r<BENCH_ROUNDS;r++){
    for(b=0;b<BENCH_BATCHES;b++) order[b]=bench_batches[b];
    bench_shuffle(order,BENCH_BATCHES);
    for(b=0;b<BENCH_BATCHES;b++){
      nb=order[b];
      slot=-1;
      for(bi=0;bi<BENCH_BATCHES;bi++) if(bench_batches[bi]==nb) slot=bi;
      for(i=0;i<BENCH_N;i++){
        off=0;
        for(j=0;j<nb;j++){
          if(vfs_write(f,off,buf,BENCH_ITEM)!=0){ printf("write failed\n"); vfs_close(f); return 1; }
          off+=(vfs_u64)BENCH_ITEM;
        }
        k_monotonic_us(&t0);
        if(vfs_sync(f)!=0){ printf("sync failed\n"); vfs_close(f); return 1; }
        k_monotonic_us(&t1);
        samples[i]=t1>t0?t1-t0:0;
      }
      qsort(samples,BENCH_N,sizeof(samples[0]),bench_cmp_u64);
      round_median[r][slot]=samples[BENCH_N/2];
      round_p99[r][slot]=samples[(BENCH_N*99)/100];
    }
  }
  printf("\nper batch size: median across %d rounds (and the per-round spread)\n",BENCH_ROUNDS);
  for(slot=0;slot<BENCH_BATCHES;slot++){
    nb=bench_batches[slot];
    p99=0;
    for(r=0;r<BENCH_ROUNDS;r++){
      across[r]=round_median[r][slot];
      if(round_p99[r][slot]>p99) p99=round_p99[r][slot];
    }
    qsort(across,BENCH_ROUNDS,sizeof(across[0]),bench_cmp_u64);
    median=across[BENCH_ROUNDS/2];
    minv=across[0];
    maxv=across[BENCH_ROUNDS-1];
    if(nb==0){
      printf("batch=%-5d (empty sync): median=%" K_U64_FMT " us  spread=[%" K_U64_FMT "..%" K_U64_FMT "] us  p99max=%" K_U64_FMT " us\n",
             nb,median,minv,maxv,p99);
    }else{
      printf("batch=%-5d (%6u B): median=%" K_U64_FMT " us  spread=[%" K_U64_FMT "..%" K_U64_FMT "] us  p99max=%" K_U64_FMT " us  per-item=%" K_U64_FMT " us\n",
             nb,(unsigned)((unsigned)nb*BENCH_ITEM),median,minv,maxv,p99,median/(k_u64)nb);
    }
  }
  vfs_close(f);
  vfs_unlink("disk://bench-fsync.tmp");
  return 0;
}
