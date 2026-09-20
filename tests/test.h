/* ============================================================================
   TEST.H -- Minimal single-header C89 test framework
   ===================================================
   Principles: Clean, Simple, Small, Cohesive, Decoupled, Uniform

   Usage:
     #include "test.h"

     TEST_PLAN(3);

     static void test_example(void){
       TEST_BEGIN("example test");
       TEST_ASSERT(1+1==2, "math is broken");
       TEST_ASSERT_I64_EQ(2*3, 6, "multiplication");
       TEST_END();
     }

     int main(void){
       test_example();
       TEST_SUMMARY();
       return TEST_EXIT_CODE();
     }

   Crash-localization output:
     BEGIN [1/72] test_name            <-- progress: crashed at test 1 of 72
     PASS  test_name  123 us
     BEGIN [45/72] test_45             <-- last line before crash: test 45 of 72
     PROGRESS  12345 us  batch         <-- long test heartbeat
     PASS  long_test  10234567 us      <-- 10-second test, AI won't mistake for hang
     SUMMARY: 70/72 passed  12345678 us
     (process terminates with exit code > 1 on crash)

   Macros:
     TEST_PLAN(n)                 set total test count for progress display
     TEST_BEGIN(name)             start test, emit "BEGIN [N/T] name" + fflush
     TEST_ASSERT(cond, msg)       boolean assertion
     TEST_ASSERT_I64_EQ(a,b,msg) signed 64-bit equality (raft_i64, int, etc.)
     TEST_ASSERT_I64_NE(a,b,msg) signed 64-bit inequality
     TEST_ASSERT_U64_EQ(a,b,msg) unsigned 64-bit equality (raft_u64, treap_u64, vfs_u64)
     TEST_ASSERT_U64_NE(a,b,msg) unsigned 64-bit inequality
     TEST_ASSERT_PTR_EQ(a,b,msg) pointer equality (%p format)
     TEST_ASSERT_STR_EQ(a,b,msg) string equality (null-safe, prints values)
     TEST_LOG(msg)                context buffer (printed on failure)
     TEST_PROGRESS(tag)           heartbeat for long tests (elapsed + tag + fflush)
     TEST_END()                   emit "PASS name  us", clear log
     TEST_SUMMARY()               emit "SUMMARY: N/M passed  us"
     TEST_EXIT_CODE()             0=all pass, 1=some fail  (a crash exits via signal, not this code)

   AI contract (machine-parseable output):
     - Every line to stdout is prefixed with a fixed keyword:
         BEGIN     test started, name follows
         PASS      test succeeded, name + elapsed-us follow
         PROGRESS  long-test heartbeat, elapsed-us + tag follow
         SUMMARY   final tally, N/M passed + total-us follow
     - Every failure goes to stderr, delimited by "-- FAIL name ----------"
     - Crash detected by: last BEGIN has no matching PASS/FAIL, or exit code > 1
     - Hang detection: if BEGIN appears and no PASS/PROGRESS follows
       within 5 seconds, the test is likely hung.  Long tests MUST emit
       periodic PROGRESS lines to signal liveness.

   Example long test with heartbeat:
     TEST_BEGIN("bulk insert 100k");
     for(i=0;i<100000;i++){
       treap_set(t,k,kl,v,vl);
       if(i%10000==0) TEST_PROGRESS("batch");
     }
     TEST_END();

   Regression-test discipline (negative control):
     A regression test that passes BOTH with and without the fix proves nothing.
     Before declaring a fix complete: revert the fix in a COPY, confirm the new
     test FAILS against the unpatched code, then restore and confirm it passes.
     That proves the test actually catches the bug it claims to. This is a
     standing rule for every new regression case, not a one-off audit step.
*/
#ifndef TEST_H
#define TEST_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "t64.h"   /* per-compiler 64-bit types, format/fmt macros and a decimal parse */
static int _test_run;
static int _test_fail;
static int _test_total;
static const char *_test_name;
static unsigned long _test_elapsed;
static unsigned long _test_t0;
static char _test_log_buf[1024];
#define TEST_PLAN(n) (_test_total=(n))
#define TEST_BEGIN(name) do{ \
  _test_name=name; \
  _test_run++; \
  printf("BEGIN [%d/%d] %s\n",_test_run,_test_total,name); fflush(stdout); \
  _test_t0=_test_now_us(); \
}while(0)
#define TEST_ASSERT(cond, msg) do{ \
  if(!(cond)){ _test_fail_report(__FILE__,__LINE__,#cond,msg); _test_fail++; return; } \
}while(0)
#define TEST_ASSERT_I64_EQ(a, b, msg) do{ \
  test_i64 _ae=(test_i64)(a); test_i64 _be=(test_i64)(b); \
  if(_ae!=_be){ _test_fail_i64_eq(__FILE__,__LINE__,#a,#b,_ae,_be,msg); _test_fail++; return; } \
}while(0)
#define TEST_ASSERT_I64_NE(a, b, msg) do{ \
  test_i64 _an=(test_i64)(a); test_i64 _bn=(test_i64)(b); \
  if(_an==_bn){ _test_fail_i64_ne(__FILE__,__LINE__,#a,#b,_an,msg); _test_fail++; return; } \
}while(0)
#define TEST_ASSERT_U64_EQ(a, b, msg) do{ \
  test_u64 _au=(test_u64)(a); test_u64 _bu=(test_u64)(b); \
  if(_au!=_bu){ _test_fail_u64_eq(__FILE__,__LINE__,#a,#b,_au,_bu,msg); _test_fail++; return; } \
}while(0)
#define TEST_ASSERT_U64_NE(a, b, msg) do{ \
  test_u64 _au=(test_u64)(a); test_u64 _bu=(test_u64)(b); \
  if(_au==_bu){ _test_fail_u64_ne(__FILE__,__LINE__,#a,#b,_au,msg); _test_fail++; return; } \
}while(0)
#define TEST_ASSERT_PTR_EQ(a, b, msg) do{ \
  const void *_pa=(const void*)(a); const void *_pb=(const void*)(b); \
  if(_pa!=_pb){ _test_fail_ptr(__FILE__,__LINE__,#a,#b,_pa,_pb,msg); _test_fail++; return; } \
}while(0)
#define TEST_ASSERT_STR_EQ(a, b, msg) do{ \
  const char *_sa=(const char*)(a); const char *_sb=(const char*)(b); \
  if(!_sa||!_sb||strcmp(_sa,_sb)!=0){ _test_fail_str(__FILE__,__LINE__,#a,#b,_sa,_sb,msg); _test_fail++; return; } \
}while(0)
#define TEST_ASSERT_MEM_EQ(a, a_len, b, b_len, msg) do{ \
  const void *_ma=(const void*)(a); const void *_mb=(const void*)(b); \
  size_t _mla=(size_t)(a_len); size_t _mlb=(size_t)(b_len); \
  if(!_ma||!_mb||_mla!=_mlb||memcmp(_ma,_mb,_mla)!=0){ \
    _test_fail_mem(__FILE__,__LINE__,#a,#b,(const unsigned char*)_ma,_mla,(const unsigned char*)_mb,_mlb,msg); _test_fail++; return; \
  } \
}while(0)
#define TEST_LOG(msg) do{ \
  size_t _ln=strlen(msg); \
  if(_ln>sizeof(_test_log_buf)-1) _ln=sizeof(_test_log_buf)-1; \
  memcpy(_test_log_buf,msg,_ln); _test_log_buf[_ln]='\0'; \
}while(0)
/* heartbeat for long-running tests: print elapsed + tag, fflush.
   AI contract: PROGRESS lines between BEGIN and PASS mean the test
   is still alive.  Absence of any output for >5 seconds without
   PROGRESS indicates a likely hang. */
#define TEST_PROGRESS(tag) do{ \
  printf("PROGRESS  %lu us  %s\n",(unsigned long)(_test_now_us()-_test_t0),tag); \
  fflush(stdout); \
}while(0)
#define TEST_END() do{ \
  _test_elapsed+=_test_now_us()-_test_t0; \
  printf("PASS  %s  %lu us\n",_test_name,(unsigned long)(_test_now_us()-_test_t0)); \
  _test_log_buf[0]='\0'; \
}while(0)
#define TEST_SUMMARY() do{ \
  printf("SUMMARY: %d/%d passed",_test_run-_test_fail,_test_run); \
  if(_test_elapsed>0) printf("  %lu us",(unsigned long)_test_elapsed); \
  printf("\n"); \
}while(0)
#define TEST_EXIT_CODE() (_test_fail>0?1:0)
static void _test_dashes(void){
  int i;
  for(i=0;i<45-(int)strlen(_test_name);i++) fputc('-',stderr);
}
static void _test_fail_report(const char *file,int line,const char *cond,const char *msg){
  fprintf(stderr,"\n-- FAIL %s ", _test_name);
  _test_dashes();
  fprintf(stderr,"\n%s:%d  assertion:  %s\n",file,line,cond);
  if(msg&&msg[0]) fprintf(stderr,"  message:   %s\n",msg);
  if(_test_log_buf[0]) fprintf(stderr,"  log:       %s\n",_test_log_buf);
  fprintf(stderr,"--------------------------------------------------\n\n");
}
static void _test_fail_i64_eq(const char *file,int line,const char *a_str,const char *b_str,test_i64 a_val,test_i64 b_val,const char *msg){
  fprintf(stderr,"\n-- FAIL %s ", _test_name);
  _test_dashes();
  fprintf(stderr,"\n%s:%d  assertion:  %s == %s\n",file,line,a_str,b_str);
  fprintf(stderr,"  expected:  %" TEST_I64_FMT "\n",b_val);
  fprintf(stderr,"  actual:    %" TEST_I64_FMT "\n",a_val);
  if(msg&&msg[0]) fprintf(stderr,"  message:   %s\n",msg);
  if(_test_log_buf[0]) fprintf(stderr,"  log:       %s\n",_test_log_buf);
  fprintf(stderr,"--------------------------------------------------\n\n");
}
static void _test_fail_i64_ne(const char *file,int line,const char *a_str,const char *b_str,test_i64 a_val,const char *msg){
  fprintf(stderr,"\n-- FAIL %s ", _test_name);
  _test_dashes();
  fprintf(stderr,"\n%s:%d  assertion:  %s != %s\n",file,line,a_str,b_str);
  fprintf(stderr,"  both:      %" TEST_I64_FMT "  (should differ)\n",a_val);
  if(msg&&msg[0]) fprintf(stderr,"  message:   %s\n",msg);
  if(_test_log_buf[0]) fprintf(stderr,"  log:       %s\n",_test_log_buf);
  fprintf(stderr,"--------------------------------------------------\n\n");
}
static void _test_fail_u64_eq(const char *file,int line,const char *a_str,const char *b_str,test_u64 a_val,test_u64 b_val,const char *msg){
  fprintf(stderr,"\n-- FAIL %s ", _test_name);
  _test_dashes();
  fprintf(stderr,"\n%s:%d  assertion:  %s == %s\n",file,line,a_str,b_str);
  fprintf(stderr,"  expected:  %" TEST_U64_FMT "\n",b_val);
  fprintf(stderr,"  actual:    %" TEST_U64_FMT "\n",a_val);
  if(msg&&msg[0]) fprintf(stderr,"  message:   %s\n",msg);
  if(_test_log_buf[0]) fprintf(stderr,"  log:       %s\n",_test_log_buf);
  fprintf(stderr,"--------------------------------------------------\n\n");
}
static void _test_fail_u64_ne(const char *file,int line,const char *a_str,const char *b_str,test_u64 a_val,const char *msg){
  fprintf(stderr,"\n-- FAIL %s ", _test_name);
  _test_dashes();
  fprintf(stderr,"\n%s:%d  assertion:  %s != %s\n",file,line,a_str,b_str);
  fprintf(stderr,"  both:      %" TEST_U64_FMT "  (should differ)\n",a_val);
  if(msg&&msg[0]) fprintf(stderr,"  message:   %s\n",msg);
  if(_test_log_buf[0]) fprintf(stderr,"  log:       %s\n",_test_log_buf);
  fprintf(stderr,"--------------------------------------------------\n\n");
}
static void _test_fail_ptr(const char *file,int line,const char *a_str,const char *b_str,const void *a_val,const void *b_val,const char *msg){
  fprintf(stderr,"\n-- FAIL %s ", _test_name);
  _test_dashes();
  fprintf(stderr,"\n%s:%d  assertion:  %s == %s\n",file,line,a_str,b_str);
  fprintf(stderr,"  expected:  %p\n",(const void*)b_val);
  fprintf(stderr,"  actual:    %p\n",(const void*)a_val);
  if(msg&&msg[0]) fprintf(stderr,"  message:   %s\n",msg);
  if(_test_log_buf[0]) fprintf(stderr,"  log:       %s\n",_test_log_buf);
  fprintf(stderr,"--------------------------------------------------\n\n");
}
static void _test_fail_str(const char *file,int line,const char *a_str,const char *b_str,const char *a_val,const char *b_val,const char *msg){
  fprintf(stderr,"\n-- FAIL %s ", _test_name);
  _test_dashes();
  fprintf(stderr,"\n%s:%d  assertion:  %s == %s\n",file,line,a_str,b_str);
  fprintf(stderr,"  expected:  \"%s\"\n",b_val?b_val:"(null)");
  fprintf(stderr,"  actual:    \"%s\"\n",a_val?a_val:"(null)");
  if(msg&&msg[0]) fprintf(stderr,"  message:   %s\n",msg);
  if(_test_log_buf[0]) fprintf(stderr,"  log:       %s\n",_test_log_buf);
  fprintf(stderr,"--------------------------------------------------\n\n");
}
static void _test_fail_mem(const char *file,int line,const char *a_str,const char *b_str,const unsigned char *a_val,size_t a_len,const unsigned char *b_val,size_t b_len,const char *msg){
  size_t i,lim;
  fprintf(stderr,"\n-- FAIL %s ", _test_name);
  _test_dashes();
  fprintf(stderr,"\n%s:%d  assertion:  memcmp(%s, %s)\n",file,line,a_str,b_str);
  fprintf(stderr,"  len:       a=%u b=%u\n",(unsigned)a_len,(unsigned)b_len);
  if(a_val){
    lim=a_len<32?a_len:32;
    fprintf(stderr,"  a bytes:  ");
    for(i=0;i<lim;i++) fprintf(stderr," %02x",a_val[i]);
    if(a_len>lim) fprintf(stderr," ...");
    fprintf(stderr,"\n");
  }else fprintf(stderr,"  a bytes:  (null)\n");
  if(b_val){
    lim=b_len<32?b_len:32;
    fprintf(stderr,"  b bytes:  ");
    for(i=0;i<lim;i++) fprintf(stderr," %02x",b_val[i]);
    if(b_len>lim) fprintf(stderr," ...");
    fprintf(stderr,"\n");
  }else fprintf(stderr,"  b bytes:  (null)\n");
  if(msg&&msg[0]) fprintf(stderr,"  message:   %s\n",msg);
  if(_test_log_buf[0]) fprintf(stderr,"  log:       %s\n",_test_log_buf);
  fprintf(stderr,"--------------------------------------------------\n\n");
}
#if defined(_WIN32)
#if defined(_WIN32)
#include <windows.h>
#endif
static unsigned long _test_now_us(void){
  LARGE_INTEGER f,c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  /* c.QuadPart*1e6 overflows signed 64-bit once the QPC tick count grows large
     (long uptime / high-frequency TSC). Divide first so the intermediate stays
     in range: quotient = whole seconds, remainder = sub-second fraction. */
  return (unsigned long)((c.QuadPart/f.QuadPart)*1000000 + (c.QuadPart%f.QuadPart)*1000000/f.QuadPart);
}
#else
#include <time.h>
static unsigned long _test_now_us(void){
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC,&ts);
  return (unsigned long)(ts.tv_sec*1000000+ts.tv_nsec/1000);
}
#endif
#endif
