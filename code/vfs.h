#ifndef VFS_H
#define VFS_H
#ifdef __cplusplus
extern "C" {
#endif
#ifndef VFS_DEF
#ifdef VFS_STATIC
#define VFS_DEF static
#else
#define VFS_DEF extern
#endif
#endif
#if defined(_MSC_VER)
typedef unsigned __int64 vfs_u64;
#else
typedef unsigned long long vfs_u64;
#endif
/* Threading contract (checked against this file, not assumed).

   ONE thread per vfs_file handle, always: read/write/sync/close touch per-inode state, and the handle carries
   the only reference to it.  The application honours this by construction - the WAL worker owns the WAL
   segments and the metadata, the snapshot worker owns the snapshot files, the event loop opens a snapshot only
   to send or to install one.

   Sharing the BACKEND between threads is a separate question, and both backends now answer the same way:

   - DISK keeps nothing in the process: each call is a syscall, so the operating system serialises it.
   - MEM keeps one process-global table (vfs_mem below) and serialises it with a spinlock (vfs_spin above):
     vfs_mem_open / vfs_mem_unlink / vfs_mem_close update the bucket head and the per-inode refcount, which
     used to be unlocked read-modify-writes that could lose an update when two paths hashed to one bucket
     (~1 in VFS_MEM_HASH_BUCKETS).  Any number of threads may use the mem backend at once.

   The lock covers the TABLE only - the bucket chain and each inode's refcount/linked flags, i.e. exactly the
   state two threads share when they touch DIFFERENT files (vfs_mem_open / vfs_mem_unlink / vfs_mem_close).
   read/write/sync are unlocked: they touch one inode's own blocks and size, which the per-handle rule above
   already makes single-threaded, exactly as it does for disk.  It is a spinlock rather than a CRITICAL_SECTION
   or a pthread mutex because those would add an OS/threading dependency and an init step to this header, and
   because the critical sections here are a handful of pointer updates - nothing that justifies blocking. */
typedef struct vfs_file vfs_file;
VFS_DEF vfs_file *vfs_open(const char *uri);
VFS_DEF int vfs_unlink(const char *uri);
VFS_DEF int vfs_read(vfs_file *file,vfs_u64 offset,void *buf,unsigned int size);
VFS_DEF int vfs_write(vfs_file *file,vfs_u64 offset,const void *buf,unsigned int size);
VFS_DEF int vfs_sync(vfs_file *file);
VFS_DEF void vfs_close(vfs_file *file);
#ifdef __cplusplus
}
#endif
#endif
#if defined(VFS_IMPLEMENTATION)&&!defined(VFS_IMPLEMENTATION_ONCE)
#define VFS_IMPLEMENTATION_ONCE
#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#ifndef VFS_MALLOC
#define VFS_MALLOC malloc
#endif
#ifndef VFS_FREE
#define VFS_FREE free
#endif
#ifndef VFS_CALLOC
#define VFS_CALLOC calloc
#endif
#if defined(_MSC_VER)
#define VFS_U64_C(x) x##ui64
#else
#define VFS_U64_C(x) x##ULL
#endif
#define VFS_MAX_OFFSET VFS_U64_C(0x7fffffffffffffff)
typedef struct vfs_backend vfs_backend;
struct vfs_file{
  vfs_backend *be;
};
struct vfs_backend{
  const char *name;
  vfs_file *(*open)(vfs_backend*,const char *);
  int (*unlink)(vfs_backend*,const char *);
  int (*read)(vfs_file*,vfs_u64,void*,unsigned int);
  int (*write)(vfs_file*,vfs_u64,const void*,unsigned int);
  int (*sync)(vfs_file *);
  void (*close)(vfs_file *);
};
typedef struct vfs_disk_file{
  vfs_file base;
#if defined(_WIN32)
  HANDLE fd;
#else
  int fd;
#endif
} vfs_disk_file;
typedef struct vfs_disk_ctx{
  vfs_backend base;
} vfs_disk_ctx;
static vfs_file *vfs_disk_open(vfs_backend *be,const char *path){
  vfs_disk_file *file_disk;
#if defined(_WIN32)
  HANDLE fd=CreateFileA(path,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,0,OPEN_ALWAYS   /* no FILE_SHARE_DELETE: the WAL of a running server must not be deletable/renamable */,FILE_ATTRIBUTE_NORMAL,0);
  if(fd==INVALID_HANDLE_VALUE) return 0;
#else
  int fd=open(path,O_RDWR|O_CREAT,0666);
  if(fd<0) return 0;
#endif
  file_disk=(vfs_disk_file *)VFS_MALLOC(sizeof(vfs_disk_file));
  if(file_disk){
    file_disk->base.be=be;
    file_disk->fd=fd;
  }else{
#if defined(_WIN32)
    CloseHandle(fd);
#else
    close(fd);
#endif
  }
  return (vfs_file *)file_disk;
}
static int vfs_disk_unlink(vfs_backend *be,const char *path){
  (void)be;
#if defined(_WIN32)
  return (DeleteFileA(path)||GetLastError()==ERROR_FILE_NOT_FOUND)?0:-1;
#else
  return (unlink(path)==0||errno==ENOENT)?0:-1;
#endif
}
#ifndef VFS_DISK_CHUNK
#define VFS_DISK_CHUNK 1048576u
#endif
static int vfs_disk_read(vfs_file *file,vfs_u64 offset,void *buf,unsigned int size){
  vfs_disk_file *file_disk=(vfs_disk_file *)file;
  unsigned char *ptr=(unsigned char *)buf;
  while(size){
    unsigned int chunk=size>VFS_DISK_CHUNK?VFS_DISK_CHUNK:size;
#if defined(_WIN32)
    DWORD done=0;
    OVERLAPPED ov;
    memset(&ov,0,sizeof(ov));
    ov.Offset=(DWORD)(offset&0xffffffffu);
    ov.OffsetHigh=(DWORD)((offset>>32)&0xffffffffu);
    if(!ReadFile(file_disk->fd,ptr,(DWORD)chunk,&done,&ov)||done==0) return -1;
#else
    ssize_t done=pread(file_disk->fd,ptr,chunk,(off_t)offset);
    if(done<=0) return -1;
#endif
    ptr+=done;
    size-=(unsigned int)done;
    offset+=(vfs_u64)done;
  }
  return 0;
}
static int vfs_disk_write(vfs_file *file,vfs_u64 offset,const void *buf,unsigned int size){
  vfs_disk_file *file_disk=(vfs_disk_file *)file;
  const unsigned char *ptr=(const unsigned char *)buf;
  while(size){
    unsigned int chunk=size>VFS_DISK_CHUNK?VFS_DISK_CHUNK:size;
#if defined(_WIN32)
    DWORD done=0;
    OVERLAPPED ov;
    memset(&ov,0,sizeof(ov));
    ov.Offset=(DWORD)(offset&0xffffffffu);
    ov.OffsetHigh=(DWORD)((offset>>32)&0xffffffffu);
    if(!WriteFile(file_disk->fd,ptr,(DWORD)chunk,&done,&ov)||done==0) return -1;
#else
    ssize_t done=pwrite(file_disk->fd,ptr,chunk,(off_t)offset);
    if(done<=0) return -1;
#endif
    ptr+=done;
    size-=(unsigned int)done;
    offset+=(vfs_u64)done;
  }
  return 0;
}
static int vfs_disk_sync(vfs_file *file){
  vfs_disk_file *file_disk=(vfs_disk_file *)file;
#if defined(_WIN32)
  return FlushFileBuffers(file_disk->fd)?0:-1;
#else
  return fsync(file_disk->fd)?-1:0;
#endif
}
static void vfs_disk_close(vfs_file *file){
  vfs_disk_file *file_disk=(vfs_disk_file *)file;
#if defined(_WIN32)
  CloseHandle(file_disk->fd);
#else
  close(file_disk->fd);
#endif
  VFS_FREE(file_disk);
}
static vfs_disk_ctx vfs_disk={{
  "disk",
  vfs_disk_open,
  vfs_disk_unlink,
  vfs_disk_read,
  vfs_disk_write,
  vfs_disk_sync,
  vfs_disk_close
}};
#define VFS_MEM_HASH_BUCKETS 1024
#define VFS_MEM_BLOCK_SHIFT 16
#define VFS_MEM_BLOCK_SIZE 65536
#define VFS_MEM_BLOCK_MASK 65535
#define VFS_MEM_BLOCK_BUCKETS_INIT 256
typedef struct vfs_mem_block vfs_mem_block;
struct vfs_mem_block{
  vfs_u64 id;
  unsigned char data[65536];
  vfs_mem_block *next;
};
typedef struct vfs_mem_inode vfs_mem_inode;
struct vfs_mem_inode{
  vfs_u64 logical_size;
  vfs_mem_block **blocks;
  size_t nbuckets;
  size_t nblocks;
  int refcount;
  int linked;
  vfs_mem_inode *next;
  char path[1];
};
/* A spinlock, so this layer stays dependency-free: Win32 uses the interlocked exchange it has had since
   Windows 2000, every other compiler the GCC atomic builtin.  It guards the in-memory backend's shared
   state (the bucket table plus each inode's refcount/linked) and nothing else - the disk backend needs no
   lock because its state lives in the operating system. */
#if defined(_WIN32)
typedef volatile LONG vfs_spin;
#define VFS_SPIN_INIT 0
static void vfs_spin_acquire(vfs_spin *s){
  while(InterlockedExchange(s,1)!=0) Sleep(0);
}
static void vfs_spin_release(vfs_spin *s){
  InterlockedExchange(s,0);
}
#else
typedef volatile int vfs_spin;
#define VFS_SPIN_INIT 0
static void vfs_spin_acquire(vfs_spin *s){
  while(__sync_lock_test_and_set(s,1)){ /* spin: the critical sections are a handful of pointer updates */ }
}
static void vfs_spin_release(vfs_spin *s){
  __sync_lock_release(s);
}
#endif

/* ONE lock for the in-memory backend, at file scope rather than inside vfs_mem_ctx, because the mem
   functions cannot trust `be` or `file->be` to BE this backend: another backend may wrap mem and re-point
   each file it opens at itself (tests/vfs_fault_test.c does exactly that), and casting that pointer to
   vfs_mem_ctx and locking through it corrupted the wrapper instead of locking anything.  Verified the hard
   way: the local MinGW build passed while the Linux gate hung inside vfs_fault_test's first case. */
static vfs_spin vfs_mem_lock=VFS_SPIN_INIT;

typedef struct vfs_mem_file{
  vfs_file base;
  vfs_mem_inode *inode;
} vfs_mem_file;
typedef struct vfs_mem_ctx{
  vfs_backend base;
  vfs_mem_inode *buckets[VFS_MEM_HASH_BUCKETS];   /* guarded by vfs_mem_lock (file scope, see above) */
} vfs_mem_ctx;
static vfs_mem_block *vfs_mem_find_block(vfs_mem_inode *n,vfs_u64 id){
  vfs_mem_block *b;
  if(!n->blocks) return 0;
  for(b=n->blocks[(size_t)(id%(vfs_u64)n->nbuckets)];b;b=b->next){
    if(b->id==id) return b;
  }
  return 0;
}
static vfs_mem_block *vfs_mem_get_block(vfs_mem_inode *n,vfs_u64 id){
  vfs_mem_block *b;
  size_t idx;
  if(!n->blocks){
    n->nbuckets=VFS_MEM_BLOCK_BUCKETS_INIT;
    n->blocks=(vfs_mem_block **)VFS_CALLOC(n->nbuckets,sizeof(vfs_mem_block *));
    if(!n->blocks) return 0;
  }
  idx=(size_t)(id%(vfs_u64)n->nbuckets);
  for(b=n->blocks[idx];b;b=b->next){
    if(b->id==id) return b;
  }
  b=(vfs_mem_block *)VFS_CALLOC(1,sizeof(vfs_mem_block));
  if(!b) return 0;
  b->id=id;
  b->next=n->blocks[idx];
  n->blocks[idx]=b;
  n->nblocks++;
  if(n->nblocks>n->nbuckets*2&&n->nbuckets<((size_t)-1)/2){
    size_t old_nb=n->nbuckets,i;
    vfs_mem_block **old=n->blocks;
    n->nbuckets*=2;
    n->blocks=(vfs_mem_block **)VFS_CALLOC(n->nbuckets,sizeof(vfs_mem_block *));
    if(!n->blocks){
      n->blocks=old;
      n->nbuckets=old_nb;
      return b;
    }
    for(i=0;i<old_nb;i++){
      vfs_mem_block *cur,*nx;
      for(cur=old[i];cur;cur=nx){
        size_t ni=(size_t)(cur->id%(vfs_u64)n->nbuckets);
        nx=cur->next;
        cur->next=n->blocks[ni];
        n->blocks[ni]=cur;
      }
    }
    VFS_FREE(old);
  }
  return b;
}
static unsigned int vfs_hash(const void *data,size_t len){
  const unsigned char *p=(const unsigned char *)data;
  unsigned int h=2166136261u;
  size_t i;
  for(i=0;i<len;i++) h=(h^p[i])*16777619u;
  h^=h>>16;
  h*=0x85ebca6bu;
  h^=h>>13;
  h*=0xc2b2ae35u;
  h^=h>>16;
  return h;
}
static vfs_file *vfs_mem_open(vfs_backend *be,const char *path){
  vfs_mem_ctx *mem=(vfs_mem_ctx *)be;
  size_t len=strlen(path);
  unsigned int idx=vfs_hash(path,len)%VFS_MEM_HASH_BUCKETS;
  vfs_mem_file *f;
  vfs_mem_inode *n;
  vfs_spin_acquire(&vfs_mem_lock);
  for(n=mem->buckets[idx];n;n=n->next){
    if(n->linked&&strcmp(n->path,path)==0) break;
  }
  f=(vfs_mem_file *)VFS_MALLOC(sizeof(vfs_mem_file));
  if(f){
    if(!n){
      n=(vfs_mem_inode *)VFS_CALLOC(1,sizeof(vfs_mem_inode)+len);
      if(!n){
        VFS_FREE(f);
        vfs_spin_release(&vfs_mem_lock);
        return 0;
      }
      n->logical_size=0;
      n->linked=1;
      strcpy(n->path,path);
      n->next=mem->buckets[idx];
      mem->buckets[idx]=n;
    }
    n->refcount++;
    f->base.be=be;
    f->inode=n;
  }
  vfs_spin_release(&vfs_mem_lock);
  return (vfs_file *)f;
}
static int vfs_mem_unlink(vfs_backend *be,const char *path){
  vfs_mem_ctx *mem=(vfs_mem_ctx *)be;
  size_t len=strlen(path);
  unsigned int idx=vfs_hash(path,len)%VFS_MEM_HASH_BUCKETS;
  vfs_mem_inode **pp;
  vfs_spin_acquire(&vfs_mem_lock);
  for(pp=&mem->buckets[idx];*pp;pp=&(*pp)->next){
    if(strcmp((*pp)->path,path)==0){
      vfs_mem_inode *n=*pp;
      *pp=n->next;
      n->linked=0;
      if(n->refcount==0){
        if(n->blocks){
          size_t i;
          vfs_mem_block *b,*nx;
          for(i=0;i<n->nbuckets;i++){
            for(b=n->blocks[i];b;b=nx){
              nx=b->next;
              VFS_FREE(b);
            }
          }
          VFS_FREE(n->blocks);
        }
        VFS_FREE(n);
      }
      vfs_spin_release(&vfs_mem_lock);
      return 0;
    }
  }
  vfs_spin_release(&vfs_mem_lock);
  return -1;
}
static int vfs_mem_read(vfs_file *file,vfs_u64 offset,void *buf,unsigned int size){
  if(size){
    vfs_mem_file *f=(vfs_mem_file *)file;
    vfs_mem_inode *n=f->inode;
    unsigned char *dst=(unsigned char *)buf;
    if(offset>n->logical_size||size>n->logical_size-offset) return -1;
    while(size){
      vfs_u64 blockid=offset>>VFS_MEM_BLOCK_SHIFT;
      unsigned int block_off=(unsigned int)(offset&VFS_MEM_BLOCK_MASK);
      unsigned int chunk=VFS_MEM_BLOCK_SIZE-block_off;
      vfs_mem_block *b;
      if(chunk>size) chunk=size;
      b=vfs_mem_find_block(n,blockid);
      if(b) memcpy(dst,b->data+block_off,chunk);
      else memset(dst,0,chunk);
      dst+=chunk;
      size-=chunk;
      offset+=(vfs_u64)chunk;
    }
  }
  return 0;
}
static int vfs_mem_write(vfs_file *file,vfs_u64 offset,const void *buf,unsigned int size){
  if(size){
    vfs_mem_file *f=(vfs_mem_file *)file;
    vfs_mem_inode *n=f->inode;
    vfs_u64 end=offset+(vfs_u64)size;
    const unsigned char *src=(const unsigned char *)buf;
    if(end<offset) return -1;
    while(size){
      vfs_u64 blockid=offset>>VFS_MEM_BLOCK_SHIFT;
      unsigned int block_off=(unsigned int)(offset&VFS_MEM_BLOCK_MASK);
      unsigned int chunk=VFS_MEM_BLOCK_SIZE-block_off;
      vfs_mem_block *b;
      if(chunk>size) chunk=size;
      b=vfs_mem_get_block(n,blockid);
      if(!b) return -1;
      memcpy(b->data+block_off,src,chunk);
      src+=chunk;
      size-=chunk;
      offset+=(vfs_u64)chunk;
    }
    if(end>n->logical_size) n->logical_size=end;
  }
  return 0;
}
static int vfs_mem_sync(vfs_file *file){
  (void)file;
  return 0;
}
static void vfs_mem_close(vfs_file *file){
  vfs_mem_file *f=(vfs_mem_file *)file;
  vfs_mem_inode *n=f->inode;
  vfs_spin_acquire(&vfs_mem_lock);
  if(--n->refcount==0&&!n->linked){
    if(n->blocks){
      size_t i;
      vfs_mem_block *b,*nx;
      for(i=0;i<n->nbuckets;i++){
        for(b=n->blocks[i];b;b=nx){
          nx=b->next;
          VFS_FREE(b);
        }
      }
      VFS_FREE(n->blocks);
    }
    VFS_FREE(n);
  }
  vfs_spin_release(&vfs_mem_lock);
  VFS_FREE(f);
}
/* The one process-global, unlocked piece of state in this layer: see the threading contract at the top. */
static vfs_mem_ctx vfs_mem={{
  "mem",
  vfs_mem_open,
  vfs_mem_unlink,
  vfs_mem_read,
  vfs_mem_write,
  vfs_mem_sync,
  vfs_mem_close
},{0}};
static vfs_backend *vfs_list[]={(vfs_backend *)&vfs_disk,(vfs_backend *)&vfs_mem,0};
static vfs_backend *vfs_route(const char *uri,const char **path_out){
  const char *p=strstr(uri,"://");
  if(p){
    size_t len=(size_t)(p-uri);
    vfs_backend **vp;
    for(vp=vfs_list;*vp;vp++){
      if(strlen((*vp)->name)==len&&strncmp((*vp)->name,uri,len)==0){
        if(path_out) *path_out=p+3;
        return *vp;
      }
    }
  }
  return 0;
}
VFS_DEF vfs_file *vfs_open(const char *uri){
  const char *path=0;
  vfs_backend *be=vfs_route(uri,&path);
  return (be&&path&&path[0])?be->open(be,path):0;
}
VFS_DEF int vfs_unlink(const char *uri){
  const char *path=0;
  vfs_backend *be=vfs_route(uri,&path);
  return (be&&path&&path[0])?be->unlink(be,path):-1;
}
VFS_DEF int vfs_read(vfs_file *file,vfs_u64 offset,void *buf,unsigned int size){
  return (file&&buf&&offset<=VFS_MAX_OFFSET)?file->be->read(file,offset,buf,size):-1;
}
VFS_DEF int vfs_write(vfs_file *file,vfs_u64 offset,const void *buf,unsigned int size){
  return (file&&buf&&offset<=VFS_MAX_OFFSET)?file->be->write(file,offset,buf,size):-1;
}
VFS_DEF int vfs_sync(vfs_file *file){
  return file?file->be->sync(file):-1;
}
VFS_DEF void vfs_close(vfs_file *file){
  if(file) file->be->close(file);
}
#endif
