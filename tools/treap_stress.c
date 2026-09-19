/* treap_stress.c -- targeted apply-path stress for the page-heap harness.
   The server's apply path (k_server_apply_command -> treap_set) is where the heap checker reports
   corruption, so this drives the SAME treap operations directly: no sockets, no event loop, seconds
   instead of tens of rounds.  Runs under Application Verifier Heaps to catch the write itself.

   Usage: treap_stress.exe [ops] [keyspace] [value_len]
   Build: gcc -std=c89 -O0 -g -Wall -Wextra -Wno-unused-function -o build/treap_stress.exe tools/treap_stress.c */
#define KBASE_IMPLEMENTATION
#include "../code/kbase.h"
#define TREAP_IMPLEMENTATION
#include "../code/treap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int g_rng=12345u;
static unsigned int rnd(void){
  g_rng=g_rng*1103515245u+12345u;
  return (g_rng>>16)&0x7fffu;
}

int main(int argc,char **argv){
  treap *t;
  unsigned long long ops=2000000ull,keyspace=4096ull,i;
  unsigned int value_len=7u,extra=0;
  unsigned char key[32],value[4096];
  const unsigned char *got;
  unsigned int got_len;
  int rc;
  if(argc>1) ops=strtoull(argv[1],0,10);
  if(argc>2) keyspace=strtoull(argv[2],0,10);
  if(argc>3) value_len=(unsigned int)strtoul(argv[3],0,10);
  if(!value_len||value_len>sizeof(value)){ fprintf(stderr,"bad value_len\n"); return 2; }
  memset(value,'v',sizeof(value));
  t=treap_create(0x5eed5eedu);
  if(!t){ fprintf(stderr,"treap_create failed\n"); return 1; }
  printf("treap_stress: ops=%llu keyspace=%llu value_len=%u\n",ops,keyspace,(unsigned)value_len);
  for(i=0;i<ops;i++){
    unsigned long long n=rnd()%(keyspace?keyspace:1ull);
    int key_len=sprintf((char *)key,"ph_%llu",n);
    unsigned int len=value_len;
    /* Mostly overwrite the same keys: that is what exercises the COW paths and the deferred frees
       that the real workload hits (the apply path rewrites hot keys constantly). */
    if((rnd()&7u)==0u) len=1u+(rnd()%value_len);
    rc=treap_set(t,key,(unsigned int)key_len,value,len);
    if(rc!=0){ printf("set failed at %llu rc=%d\n",i,rc); return 1; }
    if((rnd()&63u)==0u){
      rc=treap_get(t,key,(unsigned int)key_len,&got,&got_len);
      if(rc<0){ printf("get failed at %llu\n",i); return 1; }
      if(rc>0&&got_len!=len){ printf("value length mismatch at %llu: %u != %u\n",i,got_len,len); return 1; }
    }
    if((rnd()&255u)==0u){
      unsigned long long d=rnd()%(keyspace?keyspace:1ull);
      int dlen=sprintf((char *)key,"ph_%llu",d);
      treap_delete(t,key,(unsigned int)dlen);
    }
    if((rnd()&4095u)==0u){
      key[0]=(unsigned char)('a'+(rnd()%26));
      key[1]=(unsigned char)('a'+(rnd()%26));
      extra=treap_count(t);
      (void)extra;
    }
  }
  printf("treap_stress: done (%llu ops), count=%llu\n",ops,(unsigned long long)treap_count(t));
  treap_free(t);
  return 0;
}
