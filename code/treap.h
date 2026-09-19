#ifndef TREAP_H
#define TREAP_H
#ifdef __cplusplus
extern "C" {
#endif
#ifndef TREAP_DEF
#ifdef TREAP_STATIC
#define TREAP_DEF static
#else
#define TREAP_DEF extern
#endif
#endif
#if defined(_MSC_VER)
typedef unsigned __int64 treap_u64;
#else
typedef unsigned long long treap_u64;
#endif
typedef struct treap treap;
TREAP_DEF treap *treap_create(treap_u64 seed);
TREAP_DEF void treap_free(treap *t);
TREAP_DEF int treap_set(treap *t,const unsigned char *key,unsigned int key_len,const unsigned char *value,unsigned int value_len);
TREAP_DEF int treap_delete(treap *t,const unsigned char *key,unsigned int key_len);
TREAP_DEF treap_u64 treap_delete_range(treap *t,const unsigned char *begin,unsigned int begin_len,const unsigned char *end,unsigned int end_len);
TREAP_DEF int treap_get(const treap *t,const unsigned char *key,unsigned int key_len,const unsigned char **value,unsigned int *value_len);
TREAP_DEF int treap_exists(const treap *t,const unsigned char *key,unsigned int key_len);
TREAP_DEF int treap_min(const treap *t,const unsigned char **key,unsigned int *key_len,const unsigned char **value,unsigned int *value_len);
TREAP_DEF int treap_max(const treap *t,const unsigned char **key,unsigned int *key_len,const unsigned char **value,unsigned int *value_len);
TREAP_DEF treap_u64 treap_count(const treap *t);
TREAP_DEF treap_u64 treap_rank(const treap *t,const unsigned char *key,unsigned int key_len);
TREAP_DEF treap_u64 treap_count_range(const treap *t,const unsigned char *begin,unsigned int begin_len,const unsigned char *end,unsigned int end_len);
TREAP_DEF int treap_select(const treap *t,treap_u64 index,const unsigned char **key,unsigned int *key_len,const unsigned char **value,unsigned int *value_len);
TREAP_DEF int treap_min_range(const treap *t,const unsigned char *begin,unsigned int begin_len,const unsigned char *end,unsigned int end_len,const unsigned char **key,unsigned int *key_len,const unsigned char **value,unsigned int *value_len);
TREAP_DEF int treap_max_range(const treap *t,const unsigned char *begin,unsigned int begin_len,const unsigned char *end,unsigned int end_len,const unsigned char **key,unsigned int *key_len,const unsigned char **value,unsigned int *value_len);
#define TREAP_ASC 0
#define TREAP_DESC 1
typedef int (*treap_visit_fn)(const unsigned char *key,unsigned int key_len,const unsigned char *value,unsigned int value_len,void *ud);
TREAP_DEF treap_u64 treap_scan(const treap *t,const unsigned char *begin,unsigned int begin_len,const unsigned char *end,unsigned int end_len,int direction,treap_visit_fn fn,void *ud);
TREAP_DEF void treap_capture(treap *t);
typedef int (*treap_write_fn)(void *ud,const unsigned char *data,unsigned int size);
TREAP_DEF int treap_save(treap *t,treap_write_fn write,void *ud);
/* Snapshot views are READ-ONLY off the owner thread.  treap_capture() and treap_save_finish() must
   run on the thread that owns the tree (the one doing treap_set/delete); treap_save() may then run
   on another thread because it only READS the captured view and its immutable nodes.
   Ownership rule, and the reason this split exists: the snapshot thread used to run the reclamation
   inside treap_save - freeing the view's retired nodes and rewriting the view and live byte
   counters - concurrently with the owner thread cloning nodes (which writes blob reference counts)
   and retiring nodes into the live list.  Reclamation must happen at the COMPLETION point, on the
   owner thread, after the save has reported its result. */
TREAP_DEF void treap_save_finish(treap *t);
typedef int (*treap_read_fn) (void *ud,unsigned char *buf,unsigned int size);
/* Replace the entire tree from a serialized stream (snapshot recovery).
   Only valid when no transaction and no active snapshot is present: load
   frees the current nodes, which would dangle shared or snapshot refs. */
TREAP_DEF int treap_load(treap *t,treap_read_fn read,void *ud);
/* Copy-on-write working copy for a custom-command transaction: treap_fork()
   makes a copy whose live tree SHARES the original's root (so treap_set/get/
   delete on the copy work exactly like on the original, and reads see the
   command's own writes).  treap_commit() swaps the copy's root into the
   original atomically; treap_abort() frees only the clones (rollback).
   The copy is a normal treap* - no second read/write API.  A transaction
   copy must NOT call treap_capture/save/load (snapshot and load APIs). */
TREAP_DEF treap *treap_fork(treap *t);
TREAP_DEF int treap_commit(treap *copy,treap *t);
TREAP_DEF void treap_abort(treap *copy);
typedef struct treap_info{
  treap_u64 count;
  treap_u64 pending_free_count;
  treap_u64 tree_bytes;
  treap_u64 pending_free_bytes;
  unsigned int height;
  unsigned int max_key_len;
  unsigned int max_value_len;
  double avg_key_len;
  double avg_value_len;
} treap_info;
TREAP_DEF int treap_inspect(const treap *t,treap_info *out);
#ifdef __cplusplus
}
#endif
#endif
#if defined(TREAP_IMPLEMENTATION)&&!defined(TREAP_IMPLEMENTATION_ONCE)
#define TREAP_IMPLEMENTATION_ONCE
#include <stdlib.h>
#include <string.h>
#ifndef TREAP_MALLOC
#define TREAP_MALLOC malloc
#endif
#ifndef TREAP_FREE
#define TREAP_FREE free

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
#define TREAP_MALLOC(n) k_dbg_malloc((size_t)(n),__LINE__,__FILE__)
#define TREAP_CALLOC(n,s) k_dbg_calloc((size_t)(n),(size_t)(s),__LINE__,__FILE__)
#define TREAP_FREE(p) k_dbg_free_site((void *)(p),__LINE__,__FILE__)
#endif
#endif
#ifndef TREAP_CALLOC
#define TREAP_CALLOC calloc
#endif
#ifndef TREAP_REALLOC
#define TREAP_REALLOC realloc
#endif
#if defined(_MSC_VER)
#define TREAP_U64_C(x) x##ui64
#else
#define TREAP_U64_C(x) x##ULL
#endif
/* Values live OUT OF LINE, in reference-counted blocks.  A node stores a pointer to the
   block's payload, so every read site (treap_get/scan/save/...) keeps working with plain
   bytes - but a COW clone copies only the node (its small inline key) and bumps the
   block's reference count instead of memcpy'ing the whole value once per cloned path
   node.  That per-clone value copy dominated writes with large values: measured on an
   in-memory backend, 4 KiB values lost more than half of the achievable throughput and
   64 KiB values more than four fifths, purely to cloning. */
typedef struct treap_blob{
  treap_u64 refs;
  unsigned int len;
  treap_u64 bytes;         /* sizeof(treap_blob)+len, for the tree byte accounting */
} treap_blob;
static treap_blob *treap_blob_create(const unsigned char *data,unsigned int len){
  treap_blob *b;
  if(!data||!len) return 0;
  b=(treap_blob *)TREAP_MALLOC(sizeof(treap_blob)+(treap_u64)len);
  if(!b) return 0;
  b->refs=1;
  b->len=len;
  b->bytes=(treap_u64)sizeof(treap_blob)+(treap_u64)len;
  memcpy((unsigned char *)(b+1),data,len);
  return b;
}
static unsigned char *treap_blob_value(const treap_blob *b){
  return b?(unsigned char *)(b+1):0;
}
static void treap_blob_ref(unsigned char *value){
  treap_blob *b;
  if(!value) return;
  b=(treap_blob *)(value-sizeof(treap_blob));
  b->refs++;
}
/* Returns the bytes reclaimed (0 while other nodes still reference the block). */
static treap_u64 treap_blob_unref(unsigned char *value){
  treap_blob *b;
  treap_u64 bytes;
  if(!value) return 0;
  b=(treap_blob *)(value-sizeof(treap_blob));
  if(b->refs>0) b->refs--;
  if(b->refs) return 0;
  bytes=b->bytes;
  TREAP_FREE(b);
  return bytes;
}
typedef struct treap_node treap_node;
struct treap_node{
  treap_u64 priority;
  treap_u64 size;          /* subtree node count (order-statistic augmentation) */
  unsigned char *key;
  unsigned char *value;
  treap_node *left;
  treap_node *right;
  treap_node *next_free;
  treap_node *alloc_next;
  unsigned int key_len;
  unsigned int value_len;
};
typedef struct treap_op{
  treap_node *alloc_head;
  treap_node *alloc_tail;   /* last clone in alloc chain (O(1) merge) */
  treap_node *free_head;
  treap_node *free_tail;
  treap_u64 free_count;
  treap_u64 alloc_bytes;
  treap_u64 free_bytes;
} treap_op;
typedef struct treap_state{
  treap_node *root;
  treap_u64 count;
  treap_u64 tree_bytes;
  treap_node *pending_free_head;
  treap_node *pending_free_tail;
  treap_u64 pending_free_count;
  treap_u64 pending_free_bytes;
} treap_state;
struct treap{
  treap_state live;
  treap_state snap;
  treap_u64 seed;
  int txn;         /* 1 = COW working copy (custom-command transaction) */
  treap *txn_owner; /* for a fork: the original tree it was forked from */
  treap_op txn_op; /* working-copy COW: alloc=clones (freed on discard), free=retired nodes (merged to live on publish) */
};
/* Release a node and the value block it holds.  Every free site goes through here, so a
   block is freed exactly when its last referencing node is. */
static void treap_node_release(treap_node *n,treap_u64 *value_bytes){
  if(!n) return;
  if(value_bytes) *value_bytes+=treap_blob_unref(n->value);
  TREAP_FREE(n);
}
static treap_u64 treap_free_chain(treap_node *head){
  treap_u64 reclaimed=0;
  while(head){
    treap_node *next=head->next_free;
    treap_node_release(head,&reclaimed);
    head=next;
  }
  return reclaimed;
}
static treap_u64 treap_free_tree(treap_node *root){
  treap_node *r,*cur=root;
  treap_u64 reclaimed=0;
  if(!root) return 0;
  while(cur){
    if(cur->left){
      treap_node *l=cur->left;
      cur->left=l->right;
      l->right=cur;
      cur=l;
    }else{
      r=cur->right;
      treap_node_release(cur,&reclaimed);
      cur=r;
    }
  }
  return reclaimed;
}
static void treap_op_push_alloc(treap_op *op,treap_node *n){
  n->alloc_next=op->alloc_head;
  op->alloc_head=n;
  if(!op->alloc_tail) op->alloc_tail=n;
}
static void treap_op_push_free(treap_op *op,treap_node *n){
  n->next_free=0;
  if(op->free_tail) op->free_tail->next_free=n;
  else op->free_head=n;
  op->free_tail=n;
  op->free_count++;
}
static void treap_op_discard(treap_op *op){
  treap_node *cur=op->alloc_head;
  while(cur){
    treap_node *next=cur->alloc_next;
    treap_node_release(cur,0);
    cur=next;
  }
  op->alloc_head=0;
  op->alloc_tail=0;
  op->free_head=0;
  op->free_tail=0;
  op->free_count=0;
  op->alloc_bytes=0;
  op->free_bytes=0;
}
static void treap_op_merge(treap_op *dst,treap_op *src){
  if(!dst||!src) return;
  if(src->alloc_head){
    src->alloc_tail->alloc_next=dst->alloc_head;
    dst->alloc_head=src->alloc_head;
    if(!dst->alloc_tail) dst->alloc_tail=src->alloc_tail;
  }
  if(src->free_head){
    if(dst->free_tail) dst->free_tail->next_free=src->free_head;
    else dst->free_head=src->free_head;
    dst->free_tail=src->free_tail;
  }
  dst->free_count+=src->free_count;
  dst->alloc_bytes+=src->alloc_bytes;
  dst->free_bytes+=src->free_bytes;
  src->alloc_head=0;
  src->alloc_tail=0;
  src->free_head=0;
  src->free_tail=0;
  src->free_count=0;
  src->alloc_bytes=0;
  src->free_bytes=0;
}
static treap_u64 treap_hash(treap_u64 seed,const void *data,unsigned int len){
  const unsigned char *p=(const unsigned char *)data;
  treap_u64 h=seed^TREAP_U64_C(14695981039346656037);
  unsigned int i;
  for(i=0;i<len;i++) h=(h^p[i])*TREAP_U64_C(1099511628211);
  h^=h>>33;
  h*=TREAP_U64_C(0xff51afd7ed558ccd);
  h^=h>>33;
  h*=TREAP_U64_C(0xc4ceb9fe1a85ec53);
  h^=h>>33;
  return h;
}
/* Node bytes are the struct plus the inline key; the value is attached separately and
   LIVES ELSEWHERE (treap_blob). */
static treap_u64 treap_node_bytes(unsigned int key_len){
  return (treap_u64)sizeof(treap_node)+(treap_u64)key_len;
}
static treap_node *treap_node_init(treap_node *n,unsigned int key_len,const unsigned char *key,treap_u64 priority){
  n->key_len=key_len;
  n->value_len=0;
  n->value=0;
  if(!key_len) n->key=0;
  else{
    n->key=(unsigned char *)(n+1);
    memcpy(n->key,key,key_len);
  }
  n->priority=priority;
  n->left=0;
  n->right=0;
  n->size=1u;
  return n;
}
/* Attach a freshly copied value block (reference count 1).  Reports the block's bytes so
   the caller can account them once, no matter how many nodes end up sharing it. */
static int treap_node_attach_value(treap_node *n,unsigned int value_len,const unsigned char *value,treap_u64 *value_bytes){
  treap_blob *b;
  if(!value_len){
    n->value=0;
    n->value_len=0;
    return 0;
  }
  b=treap_blob_create(value,value_len);
  if(!b) return -1;
  n->value=treap_blob_value(b);
  n->value_len=value_len;
  if(value_bytes) *value_bytes+=b->bytes;
  return 0;
}
static treap_node *treap_clone_node(treap_op *op,treap_node *src){
  treap_node *n;
  treap_u64 sz=treap_node_bytes(src->key_len);
  unsigned int total;
  if(sz>0xffffffffu) return 0;
  total=(unsigned int)sz;
  n=(treap_node *)TREAP_MALLOC(total);
  if(!n) return 0;
  op->alloc_bytes+=total;
  op->free_bytes+=total;
  treap_node_init(n,src->key_len,src->key,src->priority);
  /* SHARE the value: this is the whole point - no value bytes are copied per clone. */
  n->value=src->value;
  n->value_len=src->value_len;
  if(n->value) treap_blob_ref(n->value);
  n->left=src->left;
  n->right=src->right;
  n->size=src->size;   /* stale until the split/merge fixup recomputes it */
  treap_op_push_alloc(op,n);
  treap_op_push_free(op,src);
  return n;
}
static int treap_key_cmp(const unsigned char *a,unsigned int alen,const unsigned char *b,unsigned int blen){
  if(alen!=0&&blen!=0){
    int cmp=memcmp(a,b,alen<blen?alen:blen);
    if(cmp!=0) return cmp;
  }
  if(alen<blen) return -1;
  if(alen>blen) return 1;
  return 0;
}
/* Total order on (priority, key): tie-break equal priorities by key so that
   merge and load build the same canonical tree even on hash collision. */
static int treap_priority_cmp(const treap_node *a,const treap_node *b){
  if(a->priority>b->priority) return 1;
  if(a->priority<b->priority) return -1;
  return treap_key_cmp(a->key,a->key_len,b->key,b->key_len);
}
static const treap_node *treap_find(const treap_node *root,const unsigned char *key,unsigned int key_len){
  while(root){
    int cmp=treap_key_cmp(key,key_len,root->key,root->key_len);
    if(cmp==0) return root;
    root=(cmp<0)?root->left:root->right;
  }
  return 0;
}
/* Recompute one node's subtree size from its (already-correct) children. */
static void treap_node_fix_size(treap_node *n){
  n->size=1u+(n->left?n->left->size:0u)+(n->right?n->right->size:0u);
}
/* Recompute sizes of all nodes cloned by an op, bottom-up.  The alloc chain
   is PREPENDED per clone, so head-to-tail is exactly reverse clone order
   (deepest first) - the order sizes must be fixed in. */
static void treap_op_fix_sizes(treap_op *op){
  treap_node *cur;
  for(cur=op->alloc_head;cur;cur=cur->alloc_next) treap_node_fix_size(cur);
}
static int treap_split(treap_op *op,treap_node *root,const unsigned char *key,unsigned int key_len,treap_node **L,treap_node **R){
  treap_node **hook_L=L,**hook_R=R;
  *hook_L=*hook_R=0;
  while(root){
    int cmp=treap_key_cmp(key,key_len,root->key,root->key_len);
    treap_node *n=treap_clone_node(op,root);
    if(!n) return -1;
    if(cmp<=0){
      *hook_R=n;
      hook_R=&n->left;
      root=root->left;
    }else{
      *hook_L=n;
      hook_L=&n->right;
      root=root->right;
    }
  }
  *hook_L=*hook_R=0;
  treap_op_fix_sizes(op);
  return 0;
}
static treap_node *treap_merge(treap_op *op,treap_node *L,treap_node *R){
  treap_node *result=0,**hook=&result;
  while(L&&R){
    if(treap_priority_cmp(L,R)>0){
      treap_node *n=treap_clone_node(op,L);
      if(!n) return 0;
      *hook=n;
      hook=&n->right;
      L=L->right;
    }else{
      treap_node *n=treap_clone_node(op,R);
      if(!n) return 0;
      *hook=n;
      hook=&n->left;
      R=R->left;
    }
  }
  *hook=L?L:R;
  treap_op_fix_sizes(op);
  return result;
}
static treap_node *treap_remove_min(treap_op *op,treap_node *root,treap_node **out_min){
  treap_node *new_root=0,**hook=&new_root;
  *out_min=0;
  while(root){
    treap_node *n;
    if(root->left==0){
      *out_min=root;
      *hook=root->right;
      op->free_bytes+=treap_node_bytes(root->key_len);
      break;
    }
    n=treap_clone_node(op,root);
    if(!n) return 0;
    *hook=n;
    hook=&n->left;
    root=root->left;
  }
  treap_op_fix_sizes(op);
  return new_root;
}
static int treap_stack_push(treap_node ***stack,unsigned int *cap,unsigned int *sp,treap_node *node){
  if(*sp>=*cap){
    treap_node **ns;
    if(*cap>0xffffffffu/2) return -1;
    *cap*=2;
    ns=(treap_node **)TREAP_REALLOC(*stack,*cap*sizeof(treap_node *));
    if(!ns) return -1;
    *stack=ns;
  }
  (*stack)[(*sp)++]=node;
  return 0;
}
TREAP_DEF treap *treap_create(treap_u64 seed){
  treap *t=(treap *)TREAP_CALLOC(1,sizeof(treap));
  if(t) t->seed=seed;
  return t;
}
TREAP_DEF void treap_free(treap *t){
  if(!t) return;
  if(t->txn){ treap_abort(t); return; }  /* a fork ends only via abort/commit */
  treap_free_chain(t->snap.pending_free_head);
  treap_free_chain(t->live.pending_free_head);
  treap_free_tree(t->live.root);
  TREAP_FREE(t);
}
/* Publish one completed op into t: normal mode merges retired nodes into
   pending_free; working-copy (txn) mode accumulates the whole op into txn_op
   instead (commit later merges free, abort frees alloc). */
static void treap_publish_op(treap *t,treap_op *op,treap_node *result,int delta){
  if(delta>0) t->live.count++;
  else if(delta<0) t->live.count--;
  t->live.tree_bytes+=op->alloc_bytes;
  t->live.tree_bytes-=op->free_bytes;
  if(t->txn){
    treap_op_merge(&t->txn_op,op);
  }else{
    if(op->free_head){
      op->free_tail->next_free=t->live.pending_free_head;
      if(!t->live.pending_free_head) t->live.pending_free_tail=op->free_tail;
      t->live.pending_free_head=op->free_head;
      t->live.pending_free_count+=op->free_count;
    }
    t->live.pending_free_bytes+=op->free_bytes;
  }
  t->live.root=result;
}
TREAP_DEF treap *treap_fork(treap *t){
  treap *copy;
  if(!t||t->txn) return 0;   /* no nested transactions */
  copy=(treap *)TREAP_CALLOC(1,sizeof(treap));
  if(!copy) return 0;
  copy->seed=t->seed;
  copy->live.root=t->live.root;
  copy->live.count=t->live.count;
  copy->live.tree_bytes=t->live.tree_bytes;
  copy->txn=1;
  copy->txn_owner=t;
  return copy;
}
TREAP_DEF int treap_commit(treap *copy,treap *t){
  if(!copy||!t||!copy->txn||copy->txn_owner!=t) return -1;
  if(copy->txn_op.free_head){
    copy->txn_op.free_tail->next_free=t->live.pending_free_head;
    if(!t->live.pending_free_head) t->live.pending_free_tail=copy->txn_op.free_tail;
    t->live.pending_free_head=copy->txn_op.free_head;
    t->live.pending_free_count+=copy->txn_op.free_count;
    t->live.pending_free_bytes+=copy->txn_op.free_bytes;
  }
  t->live.root=copy->live.root;
  t->live.count=copy->live.count;
  t->live.tree_bytes=copy->live.tree_bytes;
  TREAP_FREE(copy);
  return 0;
}
TREAP_DEF void treap_abort(treap *copy){
  treap_node *cur;
  if(!copy||!copy->txn) return;
  cur=copy->txn_op.alloc_head;
  while(cur){
    treap_node *next=cur->alloc_next;
    treap_node_release(cur,0);
    cur=next;
  }
  TREAP_FREE(copy);
}
TREAP_DEF int treap_set(treap *t,const unsigned char *key,unsigned int key_len,const unsigned char *value,unsigned int value_len){
  treap_node *result,*mid,*new_node,*old_node=0,*L=0,*R=0,*R_rest=0;
  int exists=0;
  unsigned int total;
  treap_u64 sz;
  treap_op op={0};
  if(!t||!key||!key_len||(value_len&&!value)) return -1;
  if(treap_split(&op,t->live.root,key,key_len,&L,&R)<0) goto fail;
  if(R!=0){
    const treap_node *min_in_R=R;
    while(min_in_R->left!=0) min_in_R=min_in_R->left;
    if(treap_key_cmp(min_in_R->key,min_in_R->key_len,key,key_len)==0) exists=1;
  }
  if(!exists) R_rest=R;
  else{
    R_rest=treap_remove_min(&op,R,&old_node);
    if(!R_rest&&!old_node) goto fail;
    if(old_node) treap_op_push_free(&op,old_node);
  }
  sz=treap_node_bytes(key_len);
  if(sz>0xffffffffu) goto fail;
  total=(unsigned int)sz;
  new_node=(treap_node *)TREAP_MALLOC(total);
  if(!new_node) goto fail;
  op.alloc_bytes+=total;
  treap_node_init(new_node,key_len,key,treap_hash(t->seed,key,key_len));
  new_node->left=0;
  new_node->right=0;
  treap_op_push_alloc(&op,new_node);
  {
    treap_u64 value_bytes=0;
    if(treap_node_attach_value(new_node,value_len,value,&value_bytes)!=0) goto fail;
    op.alloc_bytes+=value_bytes;    /* the value block is charged once, however many
                                       clones end up sharing it */
  }
  mid=treap_merge(&op,new_node,R_rest);
  if(!mid) goto fail;
  result=treap_merge(&op,L,mid);
  if(!result) goto fail;
  treap_publish_op(t,&op,result,exists?0:1);
  return 0;
fail:
  treap_op_discard(&op);
  return -1;
}
TREAP_DEF int treap_delete(treap *t,const unsigned char *key,unsigned int key_len){
  treap_node *result,*old_node=0,*L=0,*R=0,*R_rest=0;
  int exists=0;
  treap_op op={0};
  if(!t||!key||!key_len) return -1;
  if(treap_split(&op,t->live.root,key,key_len,&L,&R)<0) goto fail;
  if(R!=0){
    const treap_node *min_in_R=R;
    while(min_in_R->left!=0) min_in_R=min_in_R->left;
    if(treap_key_cmp(min_in_R->key,min_in_R->key_len,key,key_len)==0) exists=1;
  }
  if(!exists){ treap_op_discard(&op);return 0; }
  R_rest=treap_remove_min(&op,R,&old_node);
  if(!R_rest&&!old_node) goto fail;
  if(old_node) treap_op_push_free(&op,old_node);
  result=treap_merge(&op,L,R_rest);
  if(!result&&L&&R_rest) goto fail;
  treap_publish_op(t,&op,result,-1);
  return 1;
fail:
  treap_op_discard(&op);
  return -1;
}
/* Retire an entire subtree into op.free, counting nodes. Used by range delete
   in BOTH normal and txn modes: retire-before-publish keeps the COW ownership
   model intact (nodes stay reachable by snapshot/abort until publish+reclaim).
   Must use explicit-stack preorder, not Morris -- Morris corrupts left/right,
   and these nodes may still be referenced by a snapshot or the original tree. */
static int treap_op_push_free_tree(treap_op *op,treap_node *root,treap_u64 *count){
  treap_node **stack;
  unsigned int cap=64,sp=0;
  if(!root) return 0;
  stack=(treap_node **)TREAP_MALLOC(cap*sizeof(treap_node*));
  if(!stack) return -1;
  stack[0]=root; sp=1;
  while(sp>0){
    treap_node *n=stack[--sp];
    (*count)++;
    op->free_bytes+=treap_node_bytes(n->key_len);
    treap_op_push_free(op,n);
    if(n->right){
      if(sp>=cap){
        treap_node **ns;
        unsigned int newcap;
        if(cap>0xffffffffu/2u){ TREAP_FREE(stack); return -1; }
        newcap=cap*2u;
        ns=(treap_node **)TREAP_REALLOC(stack,newcap*sizeof(treap_node*));
        if(!ns){ TREAP_FREE(stack); return -1; }
        stack=ns; cap=newcap;
      }
      stack[sp++]=n->right;
    }
    if(n->left){
      if(sp>=cap){
        treap_node **ns;
        unsigned int newcap;
        if(cap>0xffffffffu/2u){ TREAP_FREE(stack); return -1; }
        newcap=cap*2u;
        ns=(treap_node **)TREAP_REALLOC(stack,newcap*sizeof(treap_node*));
        if(!ns){ TREAP_FREE(stack); return -1; }
        stack=ns; cap=newcap;
      }
      stack[sp++]=n->left;
    }
  }
  TREAP_FREE(stack);
  return 0;
}
/* Delete range [begin, end) (empty begin/end = unbounded). Returns number of
   removed keys, or (treap_u64)-1 on failure. */
TREAP_DEF treap_u64 treap_delete_range(treap *t,const unsigned char *begin,unsigned int begin_len,const unsigned char *end,unsigned int end_len){
  treap_op op;
  treap_node *L=0,*M=0,*R=0,*merged;
  treap_u64 removed_count=0;
  if(!t) return (treap_u64)-1;
  if(begin_len&&!begin) return (treap_u64)-1;
  if(end_len&&!end) return (treap_u64)-1;
  memset(&op,0,sizeof(op));
  M=t->live.root;
  if(begin_len){
    if(treap_split(&op,M,begin,begin_len,&L,&M)<0) goto fail;
  }
  if(end_len){
    if(treap_split(&op,M,end,end_len,&M,&R)<0) goto fail;
  }
  /* M = [begin, end): retire into op.free, never free before publish. */
  if(treap_op_push_free_tree(&op,M,&removed_count)<0) goto fail;
  merged=treap_merge(&op,L,R);
  if(!merged&&(L||R)) goto fail;
  treap_publish_op(t,&op,merged,0);
  t->live.count-=removed_count;
  return removed_count;
fail:
  treap_op_discard(&op);
  return (treap_u64)-1;
}
TREAP_DEF int treap_get(const treap *t,const unsigned char *key,unsigned int key_len,const unsigned char **value,unsigned int *value_len){
  const treap_node *n;
  if(!t||!key||!key_len||!value||!value_len) return -1;
  n=treap_find(t->live.root,key,key_len);
  if(!n) return 0;
  *value=n->value;
  *value_len=n->value_len;
  return 1;
}
TREAP_DEF int treap_exists(const treap *t,const unsigned char *key,unsigned int key_len){
  if(!t||!key||!key_len) return -1;
  return treap_find(t->live.root,key,key_len)?1:0;
}
TREAP_DEF int treap_min(const treap *t,const unsigned char **key,unsigned int *key_len,const unsigned char **value,unsigned int *value_len){
  const treap_node *n;
  if(!t||!key||!key_len||!value||!value_len) return -1;
  n=t->live.root;
  if(!n) return 0;
  while(n->left) n=n->left;
  *key=n->key;
  *key_len=n->key_len;
  *value=n->value;
  *value_len=n->value_len;
  return 1;
}
TREAP_DEF int treap_max(const treap *t,const unsigned char **key,unsigned int *key_len,const unsigned char **value,unsigned int *value_len){
  const treap_node *n;
  if(!t||!key||!key_len||!value||!value_len) return -1;
  n=t->live.root;
  if(!n) return 0;
  while(n->right) n=n->right;
  *key=n->key;
  *key_len=n->key_len;
  *value=n->value;
  *value_len=n->value_len;
  return 1;
}
TREAP_DEF treap_u64 treap_count(const treap *t){
  return t?t->live.count:0;
}
TREAP_DEF treap_u64 treap_rank(const treap *t,const unsigned char *key,unsigned int key_len){
  const treap_node *cur;
  treap_u64 rank=0;
  if(!t||!key||!key_len) return 0;
  cur=t->live.root;
  while(cur){
    if(treap_key_cmp(cur->key,cur->key_len,key,key_len)<0){
      rank+=(cur->left?cur->left->size:0u)+1u;
      cur=cur->right;
    }else cur=cur->left;
  }
  return rank;
}
TREAP_DEF treap_u64 treap_count_range(const treap *t,const unsigned char *begin,unsigned int begin_len,const unsigned char *end,unsigned int end_len){
  treap_u64 rb,re;
  if(!t) return 0;
  rb=begin_len?treap_rank(t,begin,begin_len):0u;
  re=end_len?treap_rank(t,end,end_len):t->live.count;
  return re>=rb?re-rb:0u;
}
TREAP_DEF int treap_select(const treap *t,treap_u64 index,const unsigned char **key,unsigned int *key_len,const unsigned char **value,unsigned int *value_len){
  const treap_node *cur;
  if(!t||!key||!key_len||!value||!value_len) return -1;
  cur=t->live.root;
  while(cur){
    treap_u64 left_size=cur->left?cur->left->size:0u;
    if(index<left_size) cur=cur->left;
    else if(index==left_size){
      *key=cur->key; *key_len=cur->key_len;
      *value=cur->value; *value_len=cur->value_len;
      return 1;
    }else{ index-=left_size+1u; cur=cur->right; }
  }
  return 0;
}
TREAP_DEF int treap_min_range(const treap *t,const unsigned char *begin,unsigned int begin_len,const unsigned char *end,unsigned int end_len,const unsigned char **key,unsigned int *key_len,const unsigned char **value,unsigned int *value_len){
  const treap_node *cur,*best;
  if(!t||!key||!key_len||!value||!value_len) return -1;
  cur=t->live.root;
  best=0;
  while(cur){
    if(begin_len&&treap_key_cmp(cur->key,cur->key_len,begin,begin_len)<0) cur=cur->right;
    else{ best=cur; cur=cur->left; }
  }
  if(!best) return 0;
  if(end_len&&treap_key_cmp(best->key,best->key_len,end,end_len)>=0) return 0;
  *key=best->key; *key_len=best->key_len;
  *value=best->value; *value_len=best->value_len;
  return 1;
}
TREAP_DEF int treap_max_range(const treap *t,const unsigned char *begin,unsigned int begin_len,const unsigned char *end,unsigned int end_len,const unsigned char **key,unsigned int *key_len,const unsigned char **value,unsigned int *value_len){
  const treap_node *cur,*best;
  if(!t||!key||!key_len||!value||!value_len) return -1;
  cur=t->live.root;
  best=0;
  while(cur){
    if(end_len&&treap_key_cmp(cur->key,cur->key_len,end,end_len)>=0) cur=cur->left;
    else{ best=cur; cur=cur->right; }
  }
  if(!best) return 0;
  if(begin_len&&treap_key_cmp(best->key,best->key_len,begin,begin_len)<0) return 0;
  *key=best->key; *key_len=best->key_len;
  *value=best->value; *value_len=best->value_len;
  return 1;
}
TREAP_DEF treap_u64 treap_scan(const treap *t,const unsigned char *begin,unsigned int begin_len,const unsigned char *end,unsigned int end_len,int direction,treap_visit_fn fn,void *ud){
  treap_u64 count=0;
  unsigned int cap=64,sp=0;
  treap_node **stack,*cur;
  if(!t||!t->live.root||!fn) return 0;
  if((begin_len&&!begin)||(end_len&&!end)) return (treap_u64)-1;
  if(direction!=TREAP_ASC&&direction!=TREAP_DESC) return (treap_u64)-1;
  stack=(treap_node **)TREAP_MALLOC(cap*sizeof(treap_node *));
  if(!stack) return -1;
  if(direction==TREAP_DESC){
    cur=t->live.root;
    if(end_len){
      while(cur){
        if(treap_key_cmp(cur->key,cur->key_len,end,end_len)<0){
          if(treap_stack_push(&stack,&cap,&sp,cur)){ TREAP_FREE(stack);return -1; }
          cur=cur->right;
        }else cur=cur->left;
      }
    }else{
      while(cur){
        if(treap_stack_push(&stack,&cap,&sp,cur)){ TREAP_FREE(stack);return -1; }
        cur=cur->right;
      }
    }
    while(sp>0){
      cur=stack[--sp];
      if(begin_len&&treap_key_cmp(cur->key,cur->key_len,begin,begin_len)<0) break;
      count++;
      if(fn(cur->key,cur->key_len,cur->value,cur->value_len,ud)) break;
      for(cur=cur->left;cur;cur=cur->right){
        if(treap_stack_push(&stack,&cap,&sp,cur)){ TREAP_FREE(stack);return -1; }
      }
    }
  }else{
    cur=t->live.root;
    if(begin_len){
      while(cur){
        if(treap_key_cmp(cur->key,cur->key_len,begin,begin_len)>=0){
          if(treap_stack_push(&stack,&cap,&sp,cur)){ TREAP_FREE(stack);return -1; }
          cur=cur->left;
        }else cur=cur->right;
      }
    }else{
      while(cur){
        if(treap_stack_push(&stack,&cap,&sp,cur)){ TREAP_FREE(stack);return -1; }
        cur=cur->left;
      }
    }
    while(sp>0){
      cur=stack[--sp];
      if(end_len&&treap_key_cmp(cur->key,cur->key_len,end,end_len)>=0) break;
      count++;
      if(fn(cur->key,cur->key_len,cur->value,cur->value_len,ud)) break;
      for(cur=cur->right;cur;cur=cur->left){
        if(treap_stack_push(&stack,&cap,&sp,cur)){ TREAP_FREE(stack);return -1; }
      }
    }
  }
  TREAP_FREE(stack);
  return count;
}
TREAP_DEF void treap_capture(treap *t){
  if(!t) return;
  t->snap.root=t->live.root;
  t->snap.count=t->live.count;
  t->snap.tree_bytes=t->live.tree_bytes;
  if(t->live.pending_free_head){
    if(t->snap.pending_free_head) t->snap.pending_free_tail->next_free=t->live.pending_free_head;
    else t->snap.pending_free_head=t->live.pending_free_head;
    t->snap.pending_free_tail=t->live.pending_free_tail;
  }
  t->snap.pending_free_count+=t->live.pending_free_count;
  t->snap.pending_free_bytes+=t->live.pending_free_bytes;
  t->live.pending_free_head=0;
  t->live.pending_free_tail=0;
  t->live.pending_free_count=0;
  t->live.pending_free_bytes=0;
}
TREAP_DEF int treap_save(treap *t,treap_write_fn write,void *ud){
  treap_node **stack,*cur;
  int err=0;
  unsigned int cap=64,sp=0;
  unsigned char b[4];
  if(!t) return -1;
  if(write&&t->snap.root){
    stack=(treap_node **)TREAP_MALLOC(cap*sizeof(treap_node*));
    if(!stack){ err=1;goto done; }
    cur=t->snap.root;
    while(cur||sp>0){
      while(cur){
        if(treap_stack_push(&stack,&cap,&sp,cur)){ TREAP_FREE(stack);err=1;goto done; }
        cur=cur->left;
      }
      cur=stack[--sp];
      if(!err){
        b[0]=(unsigned char)cur->key_len;b[1]=(unsigned char)(cur->key_len>>8);b[2]=(unsigned char)(cur->key_len>>16);b[3]=(unsigned char)(cur->key_len>>24);
        if(write(ud,b,4)!=0) err=1;
        if(!err&&cur->key_len&&write(ud,cur->key,cur->key_len)!=0) err=1;
        b[0]=(unsigned char)cur->value_len;b[1]=(unsigned char)(cur->value_len>>8);b[2]=(unsigned char)(cur->value_len>>16);b[3]=(unsigned char)(cur->value_len>>24);
        if(!err&&write(ud,b,4)!=0) err=1;
        if(!err&&cur->value_len&&write(ud,cur->value,cur->value_len)!=0) err=1;
        if(err){ TREAP_FREE(stack);goto done; }
      }
      cur=cur->right;
    }
    TREAP_FREE(stack);
  }
done:
  if(!err){
    b[0]=0;b[1]=0;b[2]=0;b[3]=0;
    if(write&&write(ud,b,4)!=0) err=1;
  }
  /* NO reclamation here: this function may run on another thread, which must only READ the view.
     The owner thread calls treap_save_finish() once the save has reported its result. */
  return err?-1:0;
}
/* Release a captured snapshot view: reclaim its retired nodes and reset the view.  OWNER THREAD
   ONLY (see the contract above) - it frees memory and rewrites live/snapshot bookkeeping, so it
   must not run concurrently with tree mutation.  Also used to discard a capture (`treap_save(t,0,0)`
   used to be the discard path; that conflation was the bug). */
TREAP_DEF void treap_save_finish(treap *t){
  treap_u64 reclaimed;
  if(!t) return;
  reclaimed=treap_free_chain(t->snap.pending_free_head);
  if(reclaimed) t->live.tree_bytes=t->live.tree_bytes>reclaimed?t->live.tree_bytes-reclaimed:0;
  t->snap.root=0;
  t->snap.count=0;
  t->snap.tree_bytes=0;
  t->snap.pending_free_head=0;
  t->snap.pending_free_tail=0;
  t->snap.pending_free_count=0;
  t->snap.pending_free_bytes=0;
}
TREAP_DEF int treap_load(treap *t,treap_read_fn read,void *ud){
  treap_node **stack=0,*new_root=0;
  unsigned char *kbuf=0,*vbuf=0;
  unsigned int stack_cap=64,stack_top=0,kcap=0,vcap=0;
  treap_u64 node_count=0;
  treap_u64 loaded_bytes=0;
  if(!t||!read) return -1;
  /* load replaces the entire state: refuse when a transaction or an active
     snapshot still references the current nodes (both would dangle). */
  if(t->txn) return -1;
  if(t->snap.root||t->snap.pending_free_head) return -1;
  stack=(treap_node**)TREAP_MALLOC(stack_cap*sizeof(treap_node*));
  if(!stack) return -1;
  for(;;){
    unsigned int klen,vlen,total;
    treap_u64 sz;
    unsigned char b[4];
    treap_node *n,*last;
    if(read(ud,b,4)!=0) goto fail;
    klen=(unsigned int)b[0]|((unsigned int)b[1]<<8)|((unsigned int)b[2]<<16)|((unsigned int)b[3]<<24);
    if(klen==0) break;
    if(klen>kcap){
      unsigned char *nk=(unsigned char*)TREAP_REALLOC(kbuf,klen);
      if(!nk) goto fail;
      kbuf=nk;
      kcap=klen;
    }
    if(read(ud,kbuf,klen)!=0) goto fail;
    if(read(ud,b,4)!=0) goto fail;
    vlen=(unsigned int)b[0]|((unsigned int)b[1]<<8)|((unsigned int)b[2]<<16)|((unsigned int)b[3]<<24);
    if(vlen>vcap){
      unsigned char *nv=(unsigned char*)TREAP_REALLOC(vbuf,vlen);
      if(!nv) goto fail;
      vbuf=nv;
      vcap=vlen;
    }
    if(read(ud,vbuf,vlen)!=0) goto fail;
    sz=treap_node_bytes(klen);
    if(sz>0xffffffffu) goto fail;
    total=(unsigned int)sz;
    n=(treap_node*)TREAP_MALLOC(total);
    if(!n) goto fail;
    treap_node_init(n,klen,kbuf,treap_hash(t->seed,kbuf,klen));
    {
      treap_u64 value_bytes=0;
      if(treap_node_attach_value(n,vlen,vbuf,&value_bytes)!=0){
        TREAP_FREE(n);
        goto fail;
      }
      total+=(unsigned int)value_bytes;
    }
    n->left=n->right=n->next_free=0;
    last=0;
    while(stack_top>0&&treap_priority_cmp(stack[stack_top-1],n)<0){ treap_node_fix_size(stack[stack_top-1]); last=stack[--stack_top]; }
    n->left=last;
    if(stack_top>0) stack[stack_top-1]->right=n;
    if(treap_stack_push(&stack,&stack_cap,&stack_top,n)){
      if(stack_top==0) treap_free_tree(n);
      goto fail;
    }
    node_count++;
    loaded_bytes+=total;
  }
  new_root=stack_top>0?stack[0]:0;
  while(stack_top>0){ treap_node_fix_size(stack[stack_top-1]); stack_top--; }
  treap_free_chain(t->snap.pending_free_head);
  t->snap.root=0;
  t->snap.count=0;
  t->snap.tree_bytes=0;
  t->snap.pending_free_head=0;
  t->snap.pending_free_tail=0;
  t->snap.pending_free_count=0;
  t->snap.pending_free_bytes=0;
  treap_free_chain(t->live.pending_free_head);
  treap_free_tree(t->live.root);
  t->live.root=new_root;
  t->live.count=node_count;
  t->live.tree_bytes=loaded_bytes;
  t->live.pending_free_head=0;
  t->live.pending_free_tail=0;
  t->live.pending_free_count=0;
  t->live.pending_free_bytes=0;
  TREAP_FREE(stack);
  TREAP_FREE(kbuf);
  TREAP_FREE(vbuf);
  return 0;
fail:
  if(stack_top>0) treap_free_tree(stack[0]);
  TREAP_FREE(stack);
  TREAP_FREE(kbuf);
  TREAP_FREE(vbuf);
  return -1;
}
typedef struct{
  const treap_node *node;
  unsigned int depth;
} treap_inspect_frame;
static int treap_inspect_push(treap_inspect_frame **stack,unsigned int *cap,unsigned int *sp,const treap_node *node,unsigned int depth){
  treap_inspect_frame *ns;
  unsigned int newcap;
  if(*sp>=*cap){
    if(*cap>0xffffffffu/2u) return -1;
    newcap=*cap*2u;
    ns=(treap_inspect_frame *)TREAP_REALLOC(*stack,newcap*sizeof(treap_inspect_frame));
    if(!ns) return -1;
    *stack=ns;
    *cap=newcap;
  }
  (*stack)[*sp].node=node;
  (*stack)[*sp].depth=depth;
  (*sp)++;
  return 0;
}
TREAP_DEF int treap_inspect(const treap *t,treap_info *out){
  treap_inspect_frame *stack;
  unsigned int cap=64u,sp=0u;
  unsigned int max_klen=0u,max_vlen=0u,height=0u;
  treap_u64 sum_klen=0,sum_vlen=0,node_count=0;
  if(!t||!out) return -1;
  out->count=t->live.count;
  out->tree_bytes=t->live.tree_bytes;
  out->pending_free_count=t->live.pending_free_count;
  out->pending_free_bytes=t->live.pending_free_bytes;
  stack=(treap_inspect_frame *)TREAP_MALLOC(cap*sizeof(treap_inspect_frame));
  if(!stack) return -1;
  if(t->live.root){
    treap_inspect_frame f;
    stack[0].node=t->live.root;
    stack[0].depth=1u;
    sp=1u;
    while(sp>0){
      f=stack[--sp];
      if(f.depth>height) height=f.depth;
      if(f.node->key_len>max_klen) max_klen=f.node->key_len;
      if(f.node->value_len>max_vlen) max_vlen=f.node->value_len;
      sum_klen+=f.node->key_len;
      sum_vlen+=f.node->value_len;
      node_count++;
      if(f.node->right){
        if(treap_inspect_push(&stack,&cap,&sp,f.node->right,f.depth+1u)){ TREAP_FREE(stack);return -1; }
      }
      if(f.node->left){
        if(treap_inspect_push(&stack,&cap,&sp,f.node->left,f.depth+1u)){ TREAP_FREE(stack);return -1; }
      }
    }
  }
  TREAP_FREE(stack);
  out->height=height;
  out->max_key_len=max_klen;
  out->max_value_len=max_vlen;
  out->avg_key_len=node_count>0?(double)sum_klen/(double)node_count:0.0;
  out->avg_value_len=node_count>0?(double)sum_vlen/(double)node_count:0.0;
  return 0;
}
#endif
