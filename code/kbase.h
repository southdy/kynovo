/* kbase.h -- pure byte primitives (zero deps beyond stdlib): fixed-width
   integers, injectable allocators, endian I/O, CRC32, a monotonic clock, and
   the k_buf / k_reader byte containers.  Single-header; every function is
   `static`. */
#ifndef KBASE_H
#define KBASE_H
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#define _GNU_SOURCE
#include <time.h>
#endif
typedef signed char k_i8;
typedef unsigned char k_u8;
typedef signed short k_i16;
typedef unsigned short k_u16;
typedef signed int k_i32;
typedef unsigned int k_u32;
#if defined(_MSC_VER)
typedef signed __int64 k_i64;
typedef unsigned __int64 k_u64;
#define K_I64_FMT "I64d"
#define K_U64_FMT "I64u"
#define K_X64_FMT "I64x"
#define K_I64_C(x) x##i64
#define K_U64_C(x) x##ui64
#else
typedef signed long long k_i64;
typedef unsigned long long k_u64;
#define K_I64_FMT "lld"
#define K_U64_FMT "llu"
#define K_X64_FMT "llx"
#define K_I64_C(x) x##LL
#define K_U64_C(x) x##ULL
#endif
/* ---- self-describing allocator (DEBUG BUILDS ONLY: compile with -DK_ALLOC_DEBUG) ----
   The platform heap checker reports "a freed block was modified" at some LATER, unrelated malloc,
   which names the detector and not the victim.  This wrapper keeps freed blocks in a quarantine
   with head/tail canaries and records WHO allocated each one, so k_dbg_verify() (called between
   operations by a stress driver) reports the victim's allocation site, its size and the exact
   offset of the first byte written after the free -- i.e. neither the writer nor the victim has to
   be guessed.  Inert unless K_ALLOC_DEBUG is defined. */
#ifdef K_ALLOC_DEBUG
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#if defined(_WIN32)
#include <windows.h>
#endif
#endif
#define K_DBG_QUARANTINE 8192
/* THE REGISTRIES BELOW ARE PROCESS-WIDE AND THE PROGRAM UNDER TEST IS MULTI-THREADED.  Without a
   lock, two threads freeing concurrently corrupt the live/quarantine registries, which produces
   bogus reports (a stale victim, and an invalid free from the quarantine's own eviction) that look
   exactly like the defect being hunted.  Every entry point therefore serialises on one lock. */
#ifdef _WIN32
static CRITICAL_SECTION k_dbg_lock;
static LONG k_dbg_lock_state;                /* 0 = uninitialised, 1 = ready */
static void k_dbg_lock_enter(void){
  LONG seen;
  if(k_dbg_lock_state==1) return;
  seen=InterlockedCompareExchange(&k_dbg_lock_state,2,0);   /* 2 = initialising */
  if(seen==0){ InitializeCriticalSection(&k_dbg_lock); k_dbg_lock_state=1; }
  else while(k_dbg_lock_state!=1) Sleep(0);
}
static void k_dbg_lock_leave(void){ (void)0; }
#define K_DBG_LOCK() EnterCriticalSection(&k_dbg_lock)
#define K_DBG_UNLOCK() LeaveCriticalSection(&k_dbg_lock)
#else
#define K_DBG_LOCK() do{}while(0)
#define K_DBG_UNLOCK() do{}while(0)
#endif
#define K_DBG_HEAD 0x4b444247u          /* 'KDBG' */
#define K_DBG_TAIL 0x5441494cu          /* 'TAIL' */
typedef struct k_dbg_hdr{
  k_u32 magic;
  k_u32 pad;
  size_t size;      /* payload bytes the caller asked for */
  int line;         /* allocation site */
  const char *file;
  int free_line;    /* free site: names the releaser as well as the allocator */
  const char *free_file;
  k_u32 freed;      /* already released?  a second release is a double free */
} k_dbg_hdr;
typedef struct k_dbg_block{
  void *ptr;        /* header address */
  size_t size;
  int line;
  const char *file;
} k_dbg_block;
static k_dbg_block k_dbg_q[K_DBG_QUARANTINE];
#define K_DBG_LIVE_MAX 65536
static k_dbg_block k_dbg_live[K_DBG_LIVE_MAX];   /* blocks currently handed out */
static unsigned int k_dbg_live_n;
static unsigned int k_dbg_qn;
static unsigned long long k_dbg_seq;
static unsigned long long k_dbg_checks;
static void k_dbg_abort(const char *what,const char *file,int line,size_t size,unsigned int off,
                        const char *ffile,int fline){
  fprintf(stderr,"debug-alloc: %s: block allocated at %s:%d size=%u, FREED at %s:%d, first bad byte at +%u\n",
          what,file?file:"?",line,(unsigned)size,ffile?ffile:"?",fline,(unsigned)off);
  fflush(stderr);
  abort();
}
static void *k_dbg_malloc(size_t size,int line,const char *file){
  k_dbg_hdr h;
#ifdef _WIN32
  k_dbg_lock_enter(); K_DBG_LOCK();
#endif
  SIZE_T need;
  k_u8 *blk;
  if(size==0u) size=1u;
  need=(SIZE_T)(sizeof(k_dbg_hdr)+size+8u);
  blk=(k_u8 *)malloc((size_t)need);
  if(!blk) return 0;
  h.magic=K_DBG_HEAD; h.pad=0u; h.size=size; h.line=line; h.file=file;
  h.free_line=0; h.free_file=0; h.freed=0u;
  memcpy(blk,&h,sizeof(h));
  memset(blk+sizeof(h),0xCD,size);
  { k_u32 tail=K_DBG_TAIL; memcpy(blk+sizeof(h)+size,&tail,4u); }
  k_dbg_seq++;
  if(k_dbg_live_n<K_DBG_LIVE_MAX){
    k_dbg_live[k_dbg_live_n].ptr=blk;
    k_dbg_live[k_dbg_live_n].size=size;
    k_dbg_live[k_dbg_live_n].line=line;
    k_dbg_live[k_dbg_live_n].file=file;
    k_dbg_live_n++;
  }
#ifdef _WIN32
  K_DBG_UNLOCK();
#endif
  return blk+sizeof(h);
}
static void k_dbg_free_site(void *p,int line,const char *file){
  k_dbg_hdr h;
  k_u8 *blk;
  if(!p) return;
#ifdef _WIN32
  k_dbg_lock_enter(); K_DBG_LOCK();
#endif
  blk=((k_u8 *)p)-sizeof(k_dbg_hdr);
  memcpy(&h,blk,sizeof(h));
  if(h.magic!=K_DBG_HEAD){
    fprintf(stderr,"debug-alloc: free of a pointer that is not one of ours (or a double free)\n");
    fflush(stderr);
    abort();
  }
  { /* drop it from the live registry */
    unsigned int li;
    for(li=0;li<k_dbg_live_n;li++) if(k_dbg_live[li].ptr==blk){
      k_dbg_live[li]=k_dbg_live[k_dbg_live_n-1];
      k_dbg_live_n--;
      break;
    }
  }
  if(h.freed){
    /* A double free: report both sites, since in this code base the interesting case is one object
       released by two different paths.  Without this check the second release looks legitimate (the
       header magic survives poisoning) and only surfaces later as an invalid free inside the
       quarantine's own eviction. */
    fprintf(stderr,"debug-alloc: DOUBLE FREE: block allocated at %s:%d size=%u, first freed at %s:%d, freed again at %s:%d\n",
            h.file?h.file:"?",h.line,(unsigned)h.size,h.free_file?h.free_file:"?",h.free_line,
            file?file:"?",line);
    fflush(stderr);
    abort();
  }
  h.freed=1u;
  h.free_line=line; h.free_file=file;      /* record the releaser before poisoning */
  memcpy(blk,&h,sizeof(h));
  memset(p,0xFD,h.size);
  { k_u32 tail=K_DBG_TAIL; memcpy((k_u8 *)p+h.size,&tail,4u); }
  if(k_dbg_qn>=K_DBG_QUARANTINE) free(k_dbg_q[k_dbg_qn%K_DBG_QUARANTINE].ptr);
  k_dbg_q[k_dbg_qn%K_DBG_QUARANTINE].ptr=blk;
  k_dbg_q[k_dbg_qn%K_DBG_QUARANTINE].size=h.size;
  k_dbg_q[k_dbg_qn%K_DBG_QUARANTINE].line=h.line;
  k_dbg_q[k_dbg_qn%K_DBG_QUARANTINE].file=h.file;
  k_dbg_qn++;
#ifdef _WIN32
  K_DBG_UNLOCK();
#endif
}
static void k_dbg_free(void *p){ k_dbg_free_site(p,0,0); }
static void *k_dbg_calloc(size_t n,size_t size,int line,const char *file){
  size_t total=n*size;
  void *p=k_dbg_malloc(total,line,file);
  if(p) memset(p,0,total);
  return p;
}
static void *k_dbg_realloc(void *p,size_t size,int line,const char *file){
  void *np;
  if(!p) return k_dbg_malloc(size,line,file);
  np=k_dbg_malloc(size,line,file);
  if(np){
    k_dbg_hdr h;
    memcpy(&h,((k_u8 *)p)-sizeof(k_dbg_hdr),sizeof(h));
    memcpy(np,p,h.size<size?h.size:size);
    k_dbg_free(p);
  }
  return np;
}
static void k_dbg_verify(const char *where){
  unsigned int i,count;
#ifdef _WIN32
  k_dbg_lock_enter(); K_DBG_LOCK();
#endif
  k_dbg_checks++;
  /* LIVE blocks first: an overrun past the end of a live allocation is the shape the platform
     heap reports later, at an unrelated free, naming the detector instead of the victim. */
  for(i=0;i<k_dbg_live_n;i++){
    k_u8 *blk=(k_u8 *)k_dbg_live[i].ptr;
    k_u32 tail;
    memcpy(&tail,blk+sizeof(k_dbg_hdr)+k_dbg_live[i].size,4u);
    if(tail!=K_DBG_TAIL){
      unsigned int off;
      for(off=0;off<(unsigned int)k_dbg_live[i].size;off++) if(blk[sizeof(k_dbg_hdr)+off]==0xCD) break;
      { k_dbg_hdr lh; memcpy(&lh,blk,sizeof(lh));
        k_dbg_abort(where?where:"overrun past a LIVE block",k_dbg_live[i].file,k_dbg_live[i].line,
                    k_dbg_live[i].size,off,lh.free_file,lh.free_line); }
    }
  }
  count=k_dbg_qn<K_DBG_QUARANTINE?k_dbg_qn:K_DBG_QUARANTINE;
  for(i=0;i<count;i++){
    k_u8 *blk=(k_u8 *)k_dbg_q[i].ptr;
    k_dbg_hdr h;
    k_u32 tail;
    unsigned int off;
    memcpy(&h,blk,sizeof(h));
    if(h.magic!=K_DBG_HEAD) k_dbg_abort("head overwritten in a freed block",k_dbg_q[i].file,k_dbg_q[i].line,k_dbg_q[i].size,0,h.free_file,h.free_line);
    memcpy(&tail,blk+sizeof(h)+h.size,4u);
    if(tail!=K_DBG_TAIL){
      for(off=0;off<(unsigned int)h.size;off++) if(blk[sizeof(h)+off]!=0xFD) break;
      k_dbg_abort(where?where:"post-free write",k_dbg_q[i].file,k_dbg_q[i].line,h.size,off,h.free_file,h.free_line);
    }
  }
#ifdef _WIN32
  K_DBG_UNLOCK();
#endif
}
#define K_MALLOC(n) k_dbg_malloc((size_t)(n),__LINE__,__FILE__)
#define K_CALLOC(n,s) k_dbg_calloc((size_t)(n),(size_t)(s),__LINE__,__FILE__)
#define K_REALLOC(p,n) k_dbg_realloc((p),(size_t)(n),__LINE__,__FILE__)
#define K_FREE(p) k_dbg_free_site((void *)(p),__LINE__,__FILE__)
#endif /* K_ALLOC_DEBUG */
#ifndef K_MALLOC
#define K_MALLOC malloc
#endif
#ifndef K_FREE
#define K_FREE free
#endif
#ifndef K_CALLOC
#define K_CALLOC calloc
#endif
#ifndef K_REALLOC
#define K_REALLOC realloc
#endif
k_i8 k_read_i8(const k_u8 *p){ return (k_i8)p[0]; }
k_u8 k_read_u8(const k_u8 *p){ return (k_u8)p[0]; }
k_i16 k_read_i16(const k_u8 *p){ return (k_i16)((k_u16)p[0]|((k_u16)p[1]<<8)); }
k_u16 k_read_u16(const k_u8 *p){ return (k_u16)p[0]|((k_u16)p[1]<<8); }
k_i32 k_read_i32(const k_u8 *p){ return (k_i32)((k_u32)p[0]|((k_u32)p[1]<<8)|((k_u32)p[2]<<16)|((k_u32)p[3]<<24)); }
k_u32 k_read_u32(const k_u8 *p){ return (k_u32)p[0]|((k_u32)p[1]<<8)|((k_u32)p[2]<<16)|((k_u32)p[3]<<24); }
k_i64 k_read_i64(const k_u8 *p){ return (k_i64)((k_u64)p[0]|((k_u64)p[1]<<8)|((k_u64)p[2]<<16)|((k_u64)p[3]<<24)|((k_u64)p[4]<<32)|((k_u64)p[5]<<40)|((k_u64)p[6]<<48)|((k_u64)p[7]<<56)); }
k_u64 k_read_u64(const k_u8 *p){ return (k_u64)p[0]|((k_u64)p[1]<<8)|((k_u64)p[2]<<16)|((k_u64)p[3]<<24)|((k_u64)p[4]<<32)|((k_u64)p[5]<<40)|((k_u64)p[6]<<48)|((k_u64)p[7]<<56); }
void k_write_i8(k_u8 *p,k_i8 v){ p[0]=(k_u8)v; }
void k_write_u8(k_u8 *p,k_u8 v){ p[0]=(k_u8)v; }
void k_write_i16(k_u8 *p,k_i16 v){ p[0]=(k_u8)v;p[1]=(k_u8)((k_u16)v>>8); }
void k_write_u16(k_u8 *p,k_u16 v){ p[0]=(k_u8)v;p[1]=(k_u8)(v>>8); }
void k_write_i32(k_u8 *p,k_i32 v){ p[0]=(k_u8)v;p[1]=(k_u8)((k_u32)v>>8);p[2]=(k_u8)((k_u32)v>>16);p[3]=(k_u8)((k_u32)v>>24); }
void k_write_u32(k_u8 *p,k_u32 v){ p[0]=(k_u8)v;p[1]=(k_u8)(v>>8);p[2]=(k_u8)(v>>16);p[3]=(k_u8)(v>>24); }
void k_write_i64(k_u8 *p,k_i64 v){ p[0]=(k_u8)v;p[1]=(k_u8)((k_u64)v>>8);p[2]=(k_u8)((k_u64)v>>16);p[3]=(k_u8)((k_u64)v>>24);p[4]=(k_u8)((k_u64)v>>32);p[5]=(k_u8)((k_u64)v>>40);p[6]=(k_u8)((k_u64)v>>48);p[7]=(k_u8)((k_u64)v>>56); }
void k_write_u64(k_u8 *p,k_u64 v){ p[0]=(k_u8)v;p[1]=(k_u8)(v>>8);p[2]=(k_u8)(v>>16);p[3]=(k_u8)(v>>24);p[4]=(k_u8)(v>>32);p[5]=(k_u8)(v>>40);p[6]=(k_u8)(v>>48);p[7]=(k_u8)(v>>56); }
static const k_u32 crc32_table[256]={
  0x00000000u,0x77073096u,0xee0e612cu,0x990951bau,0x076dc419u,0x706af48fu,0xe963a535u,0x9e6495a3u,
  0x0edb8832u,0x79dcb8a4u,0xe0d5e91eu,0x97d2d988u,0x09b64c2bu,0x7eb17cbdu,0xe7b82d07u,0x90bf1d91u,
  0x1db71064u,0x6ab020f2u,0xf3b97148u,0x84be41deu,0x1adad47du,0x6ddde4ebu,0xf4d4b551u,0x83d385c7u,
  0x136c9856u,0x646ba8c0u,0xfd62f97au,0x8a65c9ecu,0x14015c4fu,0x63066cd9u,0xfa0f3d63u,0x8d080df5u,
  0x3b6e20c8u,0x4c69105eu,0xd56041e4u,0xa2677172u,0x3c03e4d1u,0x4b04d447u,0xd20d85fdu,0xa50ab56bu,
  0x35b5a8fau,0x42b2986cu,0xdbbbc9d6u,0xacbcf940u,0x32d86ce3u,0x45df5c75u,0xdcd60dcfu,0xabd13d59u,
  0x26d930acu,0x51de003au,0xc8d75180u,0xbfd06116u,0x21b4f4b5u,0x56b3c423u,0xcfba9599u,0xb8bda50fu,
  0x2802b89eu,0x5f058808u,0xc60cd9b2u,0xb10be924u,0x2f6f7c87u,0x58684c11u,0xc1611dabu,0xb6662d3du,
  0x76dc4190u,0x01db7106u,0x98d220bcu,0xefd5102au,0x71b18589u,0x06b6b51fu,0x9fbfe4a5u,0xe8b8d433u,
  0x7807c9a2u,0x0f00f934u,0x9609a88eu,0xe10e9818u,0x7f6a0dbbu,0x086d3d2du,0x91646c97u,0xe6635c01u,
  0x6b6b51f4u,0x1c6c6162u,0x856530d8u,0xf262004eu,0x6c0695edu,0x1b01a57bu,0x8208f4c1u,0xf50fc457u,
  0x65b0d9c6u,0x12b7e950u,0x8bbeb8eau,0xfcb9887cu,0x62dd1ddfu,0x15da2d49u,0x8cd37cf3u,0xfbd44c65u,
  0x4db26158u,0x3ab551ceu,0xa3bc0074u,0xd4bb30e2u,0x4adfa541u,0x3dd895d7u,0xa4d1c46du,0xd3d6f4fbu,
  0x4369e96au,0x346ed9fcu,0xad678846u,0xda60b8d0u,0x44042d73u,0x33031de5u,0xaa0a4c5fu,0xdd0d7cc9u,
  0x5005713cu,0x270241aau,0xbe0b1010u,0xc90c2086u,0x5768b525u,0x206f85b3u,0xb966d409u,0xce61e49fu,
  0x5edef90eu,0x29d9c998u,0xb0d09822u,0xc7d7a8b4u,0x59b33d17u,0x2eb40d81u,0xb7bd5c3bu,0xc0ba6cadu,
  0xedb88320u,0x9abfb3b6u,0x03b6e20cu,0x74b1d29au,0xead54739u,0x9dd277afu,0x04db2615u,0x73dc1683u,
  0xe3630b12u,0x94643b84u,0x0d6d6a3eu,0x7a6a5aa8u,0xe40ecf0bu,0x9309ff9du,0x0a00ae27u,0x7d079eb1u,
  0xf00f9344u,0x8708a3d2u,0x1e01f268u,0x6906c2feu,0xf762575du,0x806567cbu,0x196c3671u,0x6e6b06e7u,
  0xfed41b76u,0x89d32be0u,0x10da7a5au,0x67dd4accu,0xf9b9df6fu,0x8ebeeff9u,0x17b7be43u,0x60b08ed5u,
  0xd6d6a3e8u,0xa1d1937eu,0x38d8c2c4u,0x4fdff252u,0xd1bb67f1u,0xa6bc5767u,0x3fb506ddu,0x48b2364bu,
  0xd80d2bdau,0xaf0a1b4cu,0x36034af6u,0x41047a60u,0xdf60efc3u,0xa867df55u,0x316e8eefu,0x4669be79u,
  0xcb61b38cu,0xbc66831au,0x256fd2a0u,0x5268e236u,0xcc0c7795u,0xbb0b4703u,0x220216b9u,0x5505262fu,
  0xc5ba3bbeu,0xb2bd0b28u,0x2bb45a92u,0x5cb36a04u,0xc2d7ffa7u,0xb5d0cf31u,0x2cd99e8bu,0x5bdeae1du,
  0x9b64c2b0u,0xec63f226u,0x756aa39cu,0x026d930au,0x9c0906a9u,0xeb0e363fu,0x72076785u,0x05005713u,
  0x95bf4a82u,0xe2b87a14u,0x7bb12baeu,0x0cb61b38u,0x92d28e9bu,0xe5d5be0du,0x7cdcefb7u,0x0bdbdf21u,
  0x86d3d2d4u,0xf1d4e242u,0x68ddb3f8u,0x1fda836eu,0x81be16cdu,0xf6b9265bu,0x6fb077e1u,0x18b74777u,
  0x88085ae6u,0xff0f6a70u,0x66063bcau,0x11010b5cu,0x8f659effu,0xf862ae69u,0x616bffd3u,0x166ccf45u,
  0xa00ae278u,0xd70dd2eeu,0x4e048354u,0x3903b3c2u,0xa7672661u,0xd06016f7u,0x4969474du,0x3e6e77dbu,
  0xaed16a4au,0xd9d65adcu,0x40df0b66u,0x37d83bf0u,0xa9bcae53u,0xdebb9ec5u,0x47b2cf7fu,0x30b5ffe9u,
  0xbdbdf21cu,0xcabac28au,0x53b39330u,0x24b4a3a6u,0xbad03605u,0xcdd70693u,0x54de5729u,0x23d967bfu,
  0xb3667a2eu,0xc4614ab8u,0x5d681b02u,0x2a6f2b94u,0xb40bbe37u,0xc30c8ea1u,0x5a05df1bu,0x2d02ef8du
};
typedef struct k_crc32_ctx{
  k_u32 reg;
} k_crc32_ctx;
static int k_crc32_init(k_crc32_ctx *ctx){
  if(!ctx) return -1;
  ctx->reg=0xffffffffu;
  return 0;
}
static int k_crc32_update(k_crc32_ctx *ctx,const void *data,k_u32 len){
  const k_u8 *p=(const k_u8 *)data;
  k_u32 v;
  if(!ctx||(len&&!data)) return -1;
  v=ctx->reg;
  while(len--) v=(v>>8)^crc32_table[(v^*p++)&0xffu];
  ctx->reg=v;
  return 0;
}
static int k_crc32_final(k_crc32_ctx *ctx,k_u32 *crc32){
  if(!ctx||!crc32) return -1;
  *crc32=ctx->reg^0xffffffffu;
  return 0;
}
static void k_crc32(const void *data,k_u32 len,k_u32 *crc32){
  k_crc32_ctx ctx;
  k_crc32_init(&ctx);
  k_crc32_update(&ctx,data,len);
  k_crc32_final(&ctx,crc32);
}
/* These live outside the K_ALLOC_DEBUG block above: that block is where <stdio.h>/<stdarg.h> used to
   come from, which is why the bounded-formatting helpers below failed to compile on Linux (unknown
   type name va_list) until they carried their own includes. */
#include <stdarg.h>
#include <stdio.h>
#if defined(_WIN32) && !defined(_MSC_VER)
/* MinGW declares vsnprintf `static` under -std=c89 and does not declare _vsnprintf at all, so the
   msvcrt entry point is declared here rather than depending on the header's mood.  It exists in every
   CRT this project targets. */
__declspec(dllimport) int _vsnprintf(char *dst,size_t cap,const char *fmt,va_list ap);
#endif
/* Bounded formatting that is always NUL-terminated and reports what it could not fit.
   MSVC 6's _vsnprintf does not terminate on truncation, so both branches finish by hand; a caller that
   keeps appending (the INFO/STATS line does) therefore cannot run off the end of its buffer no matter
   how many fields are added.  Returns the number of characters written, excluding the terminator. */
static int k_snprintf(char *dst,size_t cap,const char *fmt,...){
  va_list ap;
  int need;
  if(!dst||cap==0) return 0;
  dst[0]='\0';
  if(!fmt) return 0;
  va_start(ap,fmt);
#if defined(_WIN32)
  /* _vsnprintf: MinGW declares vsnprintf static under -std=c89, so the msvcrt name is used on Windows
     for BOTH compilers.  It returns -1 on truncation (MSVC) or the needed length (modern MinGW) - the
     check below covers both by trusting strlen() afterwards. */
  need=_vsnprintf(dst,cap,fmt,ap);
#else
  need=vsnprintf(dst,cap,fmt,ap);
#endif
  va_end(ap);
  dst[cap-1u]='\0';
  if(need<0||(size_t)need>=cap) return (int)strlen(dst);
  return need;
}
/* Append to a growing text: never overflows, never leaves it unterminated, and reports truncation
   through *over instead of silently shortening the record. */
static void k_text_append(char *dst,size_t cap,int *len,int *over,const char *fmt,...){
  va_list ap;
  int need;
  size_t left;
  if(!dst||cap==0||!len) return;
  if((size_t)*len>=cap-1u){ if(over) *over=1; return; }
  left=cap-1u-(size_t)*len;
  va_start(ap,fmt);
#if defined(_WIN32)
  need=_vsnprintf(dst+*len,left+1u,fmt,ap);
#else
  need=vsnprintf(dst+*len,left+1u,fmt,ap);
#endif
  va_end(ap);
  dst[cap-1u]='\0';
  if(need<0||(size_t)need>left){
    *len=(int)strlen(dst);
    if(over) *over=1;
    return;
  }
  *len+=need;
}
static int k_monotonic_us(k_u64 *out_us){
#if defined(_WIN32)
  LARGE_INTEGER freq,counter;
  k_u64 whole,part;
  if(!out_us) return -1;
  if(!QueryPerformanceFrequency(&freq)) return -1;
  if(!QueryPerformanceCounter(&counter)) return -1;
  whole=(k_u64)(counter.QuadPart/freq.QuadPart);
  part=(k_u64)(counter.QuadPart%freq.QuadPart);
  *out_us=whole*(k_u64)K_U64_C(1000000)+(part*(k_u64)K_U64_C(1000000))/(k_u64)freq.QuadPart;
#else
  struct timespec ts;
  if(!out_us) return -1;
  if(clock_gettime(CLOCK_MONOTONIC,&ts)!=0) return -1;
  *out_us=(k_u64)ts.tv_sec*(k_u64)K_U64_C(1000000)+(k_u64)(ts.tv_nsec/(k_u64)K_U64_C(1000));
#endif
  return 0;
}
typedef struct k_buf{
  k_u8 *data;
  k_u32 len;
  k_u32 cap;
  int err;
} k_buf;
typedef struct k_reader{
  const k_u8 *data;
  k_u32 len;
  k_u32 off;
  int err;
} k_reader;
static void k_buf_free(k_buf *b){
  if(!b) return;
  K_FREE(b->data);
  memset(b,0,sizeof(*b));
}
static int k_buf_reserve(k_buf *b,k_u32 add){
  k_u32 need,cap;
  k_u8 *data;
  if(!b||b->err) return -1;
  if(add>0xffffffffu-b->len){
    b->err=1;
    return -1;
  }
  need=b->len+add;
  if(need<=b->cap) return 0;
  cap=b->cap?b->cap:128u;
  while(cap<need){
    if(cap>0x7fffffffu){
      cap=need;
      break;
    }
    cap*=2u;
  }
  data=(k_u8 *)K_REALLOC(b->data,cap);
  if(!data){
    b->err=1;
    return -1;
  }
  b->data=data;
  b->cap=cap;
  return 0;
}
static void k_buf_bytes(k_buf *b,const void *data,k_u32 size){
  if(!b||b->err||(size&&!data)||k_buf_reserve(b,size)!=0){
    if(b) b->err=1;
    return;
  }
  if(size) memcpy(b->data+b->len,data,size);
  b->len+=size;
}
static void k_buf_u8(k_buf *b,k_u8 value){
  if(k_buf_reserve(b,1u)!=0) return;
  k_write_u8(b->data+b->len,value);
  b->len++;
}
static void k_buf_u16(k_buf *b,k_u16 value){
  if(k_buf_reserve(b,2u)!=0) return;
  k_write_u16(b->data+b->len,value);
  b->len+=2u;
}
static void k_buf_u32(k_buf *b,k_u32 value){
  if(k_buf_reserve(b,4u)!=0) return;
  k_write_u32(b->data+b->len,value);
  b->len+=4u;
}
static void k_buf_i32(k_buf *b,k_i32 value){
  if(k_buf_reserve(b,4u)!=0) return;
  k_write_i32(b->data+b->len,value);
  b->len+=4u;
}
static void k_buf_i64(k_buf *b,k_i64 value){
  if(k_buf_reserve(b,8u)!=0) return;
  k_write_i64(b->data+b->len,value);
  b->len+=8u;
}
static int k_reader_need(k_reader *r,k_u32 size){
  if(!r||r->err||size>r->len-r->off){
    if(r) r->err=1;
    return 0;
  }
  return 1;
}
static k_u8 k_reader_u8(k_reader *r){
  k_u8 value=0;
  if(k_reader_need(r,1u)){
    value=k_read_u8(r->data+r->off);
    r->off++;
  }
  return value;
}
static k_u16 k_reader_u16(k_reader *r){
  k_u16 value=0;
  if(k_reader_need(r,2u)){
    value=k_read_u16(r->data+r->off);
    r->off+=2u;
  }
  return value;
}
static k_u32 k_reader_u32(k_reader *r){
  k_u32 value=0;
  if(k_reader_need(r,4u)){
    value=k_read_u32(r->data+r->off);
    r->off+=4u;
  }
  return value;
}
static k_i32 k_reader_i32(k_reader *r){
  k_i32 value=0;
  if(k_reader_need(r,4u)){
    value=k_read_i32(r->data+r->off);
    r->off+=4u;
  }
  return value;
}
static k_i64 k_reader_i64(k_reader *r){
  k_i64 value=0;
  if(k_reader_need(r,8u)){
    value=k_read_i64(r->data+r->off);
    r->off+=8u;
  }
  return value;
}
static const k_u8 *k_reader_bytes(k_reader *r,k_u32 size){
  const k_u8 *data=0;
  if(k_reader_need(r,size)){
    data=r->data+r->off;
    r->off+=size;
  }
  return data;
}
#endif
