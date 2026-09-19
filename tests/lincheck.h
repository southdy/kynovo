/* lincheck.h -- register linearizability checker (C89, always-static).
   In-harness port of tests/lincheck.py for the kserver_cluster_fuzz
   client-consistency oracle.

   Model: one register (one key).  An op is:
     is_write : 1 = write (SET writes a value, DEL writes "absent"), 0 = read
     val      : write -> value written; read -> value returned
     inv/resp : logical invocation/response timestamps (resp >= inv)

   LIN_ABSENT is the register's "key absent" value.  lin_linearizable(ops, n,
   init) returns 1 iff the history admits a total order that (a) respects
   real-time order (j before i whenever resp[j] < inv[i]) and (b) is a valid
   register history (each read returns the most recent preceding write, or init
   if none).

   The check is the Wing & Gong backtracking search turned into a graph search
   by memoizing failed (used-mask, current-value) configurations; without the
   memo the tree search is exponential on wide/non-linearizable histories (see
   Lowe, "Testing for Linearizability", Sec. 3.1).  lin_oracle is the plain
   tree search, kept only so lincheck_selftest() can cross-check the memoized
   version on random histories and prove the memoization is sound. */
#ifndef LINCHECK_H
#define LINCHECK_H
#include <stdio.h>
#include <string.h>

#define LIN_MAX 64              /* op-mask width: at most 64 ops per key */
#define LIN_ABSENT (-1)
#define LIN_INCONCLUSIVE (-1)   /* history too long for the op mask: NOT a violation */

typedef struct lin_op{
  int is_write;
  int val;
  long inv;
  long resp;
} lin_op;

/* ---- plain backtracking oracle (exponential; ground truth for small n) ---- */
static int lin_search(const lin_op *ops,int n,const unsigned long long *pred,
                      unsigned long long used,int cur_val,int count){
  int i;
  if(count==n) return 1;
  for(i=0;i<n;i++){
    if(used & (1ULL<<i)) continue;
    if(pred[i] & ~used) continue;      /* a real-time predecessor is unplaced */
    if(ops[i].is_write){
      used |= (1ULL<<i);
      if(lin_search(ops,n,pred,used,ops[i].val,count+1)) return 1;
      used &= ~(1ULL<<i);
    }else if(cur_val==ops[i].val){
      used |= (1ULL<<i);
      if(lin_search(ops,n,pred,used,cur_val,count+1)) return 1;
      used &= ~(1ULL<<i);
    }
  }
  return 0;
}

static int lin_oracle(const lin_op *ops,int n,int init){
  unsigned long long pred[LIN_MAX];
  int i,j;
  if(n<0||n>LIN_MAX) return LIN_INCONCLUSIVE;
  for(i=0;i<n;i++){
    pred[i]=0;
    for(j=0;j<n;j++)
      if(j!=i && ops[j].resp < ops[i].inv) pred[i] |= (1ULL<<j);
  }
  return lin_search(ops,n,pred,0ULL,init,0);
}

/* ---- memoized (graph-search) version: the production checker ----
   Key = the exact (used, cur) configuration.  The failed-configuration cache is
   the ONLY thing memoized, so an exact match is required: an earlier version
   keyed on (used+1, cur&3) and therefore conflated values that are congruent
   mod 4 (2 vs 6, -1 vs 3, 0/4/8...), i.e. it reported "already proven to fail"
   for configurations it had never explored and turned LINEARIZABLE histories
   into reported violations.  Values in real histories are LIN_ABSENT and 0..9,
   so those collisions are routine, not exotic. */
#define LIN_MEMO_BITS 16
#define LIN_MEMO_SIZE (1u<<LIN_MEMO_BITS)
typedef struct lin_memo_slot{
  unsigned long long used;
  int cur;
  int valid;
} lin_memo_slot;
static lin_memo_slot lin_memo[LIN_MEMO_SIZE];

static unsigned int lin_memo_hash(unsigned long long used,int cur){
  unsigned long long k=used*0x9E3779B97F4A7C15ULL;
  k^=(unsigned long long)(unsigned int)cur*0xC2B2AE3D27D4EB4FULL;
  k^=k>>29;
  k*=0xBF58476D1CE4E5B9ULL;
  k^=k>>32;
  return (unsigned int)(k>>(64-LIN_MEMO_BITS));
}
static int lin_memo_get(unsigned long long used,int cur){
  unsigned int i=lin_memo_hash(used,cur),probe;
  for(probe=0;probe<LIN_MEMO_SIZE;probe++){
    if(!lin_memo[i].valid) return 0;                       /* empty slot: not memoized */
    if(lin_memo[i].used==used&&lin_memo[i].cur==cur) return 1;
    i=(i+1u)&(LIN_MEMO_SIZE-1u);
  }
  return 0;                                                /* full: treat as a miss */
}
static void lin_memo_put(unsigned long long used,int cur){
  unsigned int i=lin_memo_hash(used,cur),probe;
  for(probe=0;probe<LIN_MEMO_SIZE;probe++){
    if(!lin_memo[i].valid||(lin_memo[i].used==used&&lin_memo[i].cur==cur)){
      lin_memo[i].used=used;
      lin_memo[i].cur=cur;
      lin_memo[i].valid=1;
      return;
    }
    i=(i+1u)&(LIN_MEMO_SIZE-1u);
  }
  /* Table full: drop the cache and take the home slot instead of spinning.  Only
     FAILED configurations are cached, so forgetting them costs search time but
     can never change the answer (a miss just re-explores). */
  memset(lin_memo,0,sizeof(lin_memo));
  i=lin_memo_hash(used,cur);
  lin_memo[i].used=used;
  lin_memo[i].cur=cur;
  lin_memo[i].valid=1;
}

static int lin_search_memo(const lin_op *ops,int n,const unsigned long long *pred,
                           unsigned long long used,int cur_val,int count){
  int i;
  if(count==n) return 1;
  if(lin_memo_get(used,cur_val)) return 0;      /* already proven to fail */
  for(i=0;i<n;i++){
    if(used & (1ULL<<i)) continue;
    if(pred[i] & ~used) continue;
    if(ops[i].is_write){
      used |= (1ULL<<i);
      if(lin_search_memo(ops,n,pred,used,ops[i].val,count+1)) return 1;
      used &= ~(1ULL<<i);
    }else if(cur_val==ops[i].val){
      used |= (1ULL<<i);
      if(lin_search_memo(ops,n,pred,used,cur_val,count+1)) return 1;
      used &= ~(1ULL<<i);
    }
  }
  lin_memo_put(used,cur_val);
  return 0;
}

/* 1 = linearizable, 0 = not, LIN_INCONCLUSIVE = the history is longer than the
   op mask can hold, so it was NOT checked.  Callers must treat INCONCLUSIVE as
   "skip", never as a violation: reporting an unchecked history as non-
   linearizable is a fabricated safety violation.  (tests/lincheck.py has no
   such limit; the cap exists only because the C version packs `used` in a
   fixed-width bitmask.) */
static int lin_linearizable(const lin_op *ops,int n,int init){
  unsigned long long pred[LIN_MAX];
  int i,j;
  if(n<0||n>LIN_MAX) return LIN_INCONCLUSIVE;
  for(i=0;i<n;i++){
    pred[i]=0;
    for(j=0;j<n;j++)
      if(j!=i && ops[j].resp < ops[i].inv) pred[i] |= (1ULL<<j);
  }
  memset(lin_memo,0,sizeof(lin_memo));
  return lin_search_memo(ops,n,pred,0ULL,init,0);
}

static void lin_dump(const lin_op *ops,int n){
  int i;
  for(i=0;i<n;i++)
    fprintf(stderr,"  %c%d [%ld,%ld]\n",ops[i].is_write?'w':'r',ops[i].val,
            (long)ops[i].inv,(long)ops[i].resp);
}

static int lc_case(const char *name,const lin_op *ops,int n,int init,int expect){
  int got=lin_linearizable(ops,n,init);
  if(got!=expect){
    fprintf(stderr,"lincheck selftest FAIL: %s (got %d want %d)\n",name,got,expect);
    return 1;
  }
  return 0;
}

/* splitmix64 (self-contained, for the randomized cross-check) */
static unsigned long long lc_seed;
static unsigned long long lc_rand(void){
  unsigned long long z=(lc_seed+=0x9E3779B97F4A7C15ULL);
  z=(z^(z>>30))*0xBF58476D1CE4E5B9ULL;
  z=(z^(z>>27))*0x94D049BB133111EBULL;
  return z^(z>>31);
}

static int lincheck_selftest(void){
  lin_op a[4],h[8];
  int bad=0,iter,i,n,init,r,oracle;
  unsigned int mi;
  /* seq write->read (linearizable) */
  a[0].is_write=1; a[0].val=1; a[0].inv=0; a[0].resp=1;
  a[1].is_write=0; a[1].val=1; a[1].inv=2; a[1].resp=3;
  bad |= lc_case("seq write->read",a,2,LIN_ABSENT,1);
  /* stale read (NON-linearizable): write completes, then read returns absent */
  a[0].is_write=1; a[0].val=1; a[0].inv=0; a[0].resp=1;
  a[1].is_write=0; a[1].val=LIN_ABSENT; a[1].inv=2; a[1].resp=3;
  bad |= lc_case("stale read",a,2,LIN_ABSENT,0);
  /* concurrent read-old (linearizable): read linearizes before the write */
  a[0].is_write=1; a[0].val=1; a[0].inv=0; a[0].resp=5;
  a[1].is_write=0; a[1].val=LIN_ABSENT; a[1].inv=1; a[1].resp=2;
  bad |= lc_case("concurrent read-old",a,2,LIN_ABSENT,1);
  /* concurrent read-new (linearizable): read linearizes after the write */
  a[0].is_write=1; a[0].val=1; a[0].inv=0; a[0].resp=5;
  a[1].is_write=0; a[1].val=1; a[1].inv=1; a[1].resp=2;
  bad |= lc_case("concurrent read-new",a,2,LIN_ABSENT,1);
  /* phantom value (NON-linearizable): read returns a never-written value */
  a[0].is_write=1; a[0].val=0; a[0].inv=0; a[0].resp=1;
  a[1].is_write=0; a[1].val=1; a[1].inv=2; a[1].resp=3;
  bad |= lc_case("phantom value",a,2,LIN_ABSENT,0);
  /* two overlapping writers, read sees the later (linearizable) */
  a[0].is_write=1; a[0].val=1; a[0].inv=0; a[0].resp=3;
  a[1].is_write=1; a[1].val=2; a[1].inv=0; a[1].resp=3;
  a[2].is_write=0; a[2].val=2; a[2].inv=4; a[2].resp=5;
  bad |= lc_case("two writers",a,3,LIN_ABSENT,1);
  /* read-after-delete (NON-linearizable): del completes, then read returns old */
  a[0].is_write=1; a[0].val=LIN_ABSENT; a[0].inv=0; a[0].resp=1;
  a[1].is_write=0; a[1].val=1; a[1].inv=2; a[1].resp=3;
  bad |= lc_case("read after delete",a,2,1,0);
  /* read-before-any-write (linearizable) */
  a[0].is_write=0; a[0].val=LIN_ABSENT; a[0].inv=0; a[0].resp=1;
  a[1].is_write=1; a[1].val=1; a[1].inv=2; a[1].resp=3;
  bad |= lc_case("read absent",a,2,LIN_ABSENT,1);

  /* memo-key collision regression.  These four ops need the states (used, 2) and
     (used, 6) to stay distinct; a key that drops the high bits of cur (e.g.
     used+1 with a cur&3 table) conflates them and reports this LINEARIZABLE
     history (init 2; oracle, lincheck.py and an exhaustive permutation search all
     agree) as a violation. */
  a[0].is_write=1; a[0].val=4; a[0].inv=2; a[0].resp=6;
  a[1].is_write=1; a[1].val=2; a[1].inv=1; a[1].resp=2;
  a[2].is_write=1; a[2].val=0; a[2].inv=2; a[2].resp=2;
  a[3].is_write=0; a[3].val=4; a[3].inv=3; a[3].resp=6;
  bad |= lc_case("values 2 and 6 must not share a memo key",a,4,2,1);

  /* randomized cross-check: memoized checker must agree with the plain oracle.
     Values span -1..8 (10 distinct) so that values congruent mod 4 necessarily
     appear in the same history: a memo keyed on (used, cur&3) is guaranteed to
     be caught here, while a -1..2 domain (the four residues exactly once) could
     never expose the collision. */
  lc_seed=0x123456789ABCDEF0ULL;
  for(iter=0;iter<2000;iter++){
    n=1+(int)(lc_rand()%7u);              /* 1..7 ops */
    init=(int)(lc_rand()%5u)-1;           /* -1..3 */
    for(i=0;i<n;i++){
      long inv=(long)(lc_rand()%9u);
      h[i].inv=inv;
      h[i].resp=inv+(long)(lc_rand()%(9u-(unsigned long)inv+1u));
      h[i].is_write=(int)(lc_rand()%2u);
      h[i].val=(int)(lc_rand()%10u)-1;    /* -1..8 */
    }
    r=lin_linearizable(h,n,init);
    oracle=lin_oracle(h,n,init);
    if(r!=oracle){
      fprintf(stderr,"lincheck selftest MISMATCH (iter %d): memo=%d oracle=%d\n",
              iter,r,oracle);
      lin_dump(h,n);
      return 1;
    }
  }
  /* Capacity / termination: the open-addressed cache must not spin when a table
     is full (the previous probe loop had no capacity check and hung once all
     65536 slots of one cur&3 table were taken).  Fill past capacity with distinct
     keys, then look up on the full table: both must terminate. */
  for(mi=0;mi<LIN_MEMO_SIZE+8u;mi++) lin_memo_put((unsigned long long)mi,(int)(mi&1u));
  r=lin_memo_get((unsigned long long)(LIN_MEMO_SIZE-1u),0);
  if(r!=0&&r!=1){ fprintf(stderr,"lincheck selftest FAIL: memo lookup returned %d\n",r); return 1; }
  memset(lin_memo,0,sizeof(lin_memo));

  return bad;
}

#endif /* LINCHECK_H */
