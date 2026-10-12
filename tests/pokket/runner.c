#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "runner.h"

static jmp_buf *s_jmp;
static char s_why[512];
static int s_pass, s_fail, s_np;

void pokket_assert_fail(const char *expr, const char *file, int line) {
    snprintf(s_why, sizeof(s_why), "%s:%d: assert(%s)", file, line, expr);
    if (!s_jmp) {
        printf("FATAL outside a case: %s\n", s_why);
        exit(1);
    }
    longjmp(*s_jmp, 1);
}

void pokket_not_ported(const char *what) {
    snprintf(s_why, sizeof(s_why), "needs %s", what);
    if (!s_jmp) {
        printf("FATAL outside a case: %s\n", s_why);
        exit(1);
    }
    longjmp(*s_jmp, 2);
}

void pokket_run(const char *name, void (*fn)(void *), void *arg) {
    jmp_buf jb;
    jmp_buf *prev = s_jmp;
    int r;
    fflush(stdout);
    s_jmp = &jb;
    r = setjmp(jb);
    if (r == 0) {
        fn(arg);
        printf("RESULT PASS %s\n", name);
        s_pass++;
    } else if (r == 1) {
        printf("RESULT FAIL %s: %s\n", name, s_why);
        s_fail++;
    } else {
        printf("RESULT NOT-PORTABLE %s: %s\n", name, s_why);
        s_np++;
    }
    s_jmp = prev;
}

int pokket_summary(const char *suite) {
    printf("SUMMARY %s pass=%d fail=%d not-portable=%d\n", suite, s_pass, s_fail, s_np);
    return s_fail ? 1 : 0;
}
