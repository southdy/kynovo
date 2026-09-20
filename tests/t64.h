#ifndef T64_H
#define T64_H
/* Per-compiler 64-bit layer shared by every test program.
   MSVC 6 has no `long long`, no `LL`/`ULL` literals, no `%llu` and neither `strtoull` nor
   `_strtoui64`, so test sources must not spell any of those directly.  MinGW-w64 accepts all of
   them, which is exactly why this class of defect only ever surfaces on the legacy guest; the
   principles ratchet counts the raw spellings under tests/ to keep them from coming back.
   Also note MSVC 6 cannot convert `unsigned __int64` to `double` (error C2520) - go through the
   signed type first where a test needs a double. */
#if defined(_MSC_VER)
typedef __int64 test_i64;
typedef unsigned __int64 test_u64;
#define TEST_I64_FMT "I64d"
#define TEST_U64_FMT "I64u"
#define TEST_I64_C(x) x##i64
#define TEST_U64_C(x) x##ui64
#else
typedef long long test_i64;
typedef unsigned long long test_u64;
#define TEST_I64_FMT "lld"
#define TEST_U64_FMT "llu"
#define TEST_I64_C(x) x##LL
#define TEST_U64_C(x) x##ULL
#endif
/* Decimal parse with no runtime-library call at all.  MSVC 6 declares neither strtoull nor
   _strtoui64 (C4013 "undefined", and the linker then wants __strtoui64, which the CRT does not
   export), and the name differs across CRTs.  The tests only ever parse a decimal count or seed,
   so parse it here. */
static test_u64 test_strtoull(const char *s){
  test_u64 v=0;
  if(s==0) return 0;
  while(*s==' '||*s=='\t') s++;
  while(*s>='0'&&*s<='9'){ v=v*10u+(test_u64)(*s-'0'); s++; }
  return v;
}
#endif
