/* tst.h - minimal test harness: CHECK records a failure and carries on. */
#ifndef TST_H
#define TST_H
#include <stdio.h>
extern int tst_fail, tst_pass;
#define CHECK(c) do { if (c) tst_pass++; else { tst_fail++; \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); \
    if (_a == _b) tst_pass++; else { tst_fail++; \
    printf("  FAIL %s:%d  %s == %s  (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)
#define RUN(fn) do { printf("%s\n", #fn); fn(); } while (0)
#endif
