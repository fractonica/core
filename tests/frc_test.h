/*
 * frc_test.h - the test harness of fractonica-core. One header, libc only.
 *
 * A test program is a plain C file tests/test_NAME.c (the Makefile finds it):
 *
 *     #include "frc_test.h"
 *     #include "fractonica.h"
 *
 *     static void test_factors(void)
 *     {
 *         TEST_SECTION("factors");
 *         CHECK(frc_factor(0) == 5);
 *         CHECK_EQ_U64(frc_prefix(6), 125125);
 *     }
 *
 *     int main(void)
 *     {
 *         test_factors();
 *         return TEST_SUMMARY("NAME");   // prints one line, non-zero on failure
 *     }
 *
 * Macros
 *     TEST_SECTION(name)               names the checks that follow (shown on failure)
 *     CHECK(condition)
 *     CHECK_EQ_I64(actual, expected)   any signed integers; prints both on failure
 *     CHECK_EQ_U64(actual, expected)   any unsigned integers
 *     CHECK_STR(actual, expected)      NUL-terminated strings, byte for byte
 *     CHECK_NOTE(format, ...)          printf a line of context after a failure:
 *                                      it prints only if the check just above failed
 *     TEST_SUMMARY(name)               "name: N checks, M failed"; the exit code
 *
 * A failing check never stops the program: every check runs, the first
 * FRC_TEST_MAX_REPORTS failures are printed, all are counted.
 */
#ifndef FRC_TEST_H
#define FRC_TEST_H

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef FRC_TEST_MAX_REPORTS
#  define FRC_TEST_MAX_REPORTS 25
#endif

static int frc_test_checks;
static int frc_test_failures;
static int frc_test_last_failed; /* 1 when the most recent check failed AND was printed */
static const char *frc_test_section = "";

/* Count a failure; returns 1 when it should be printed. */
static inline int frc_test_fail(const char *file, int line)
{
    frc_test_failures++;
    frc_test_last_failed = frc_test_failures <= FRC_TEST_MAX_REPORTS;
    if (frc_test_last_failed) {
        printf("FAIL %s:%d [%s] ", file, line, frc_test_section);
    } else if (frc_test_failures == FRC_TEST_MAX_REPORTS + 1) {
        printf("... further failures are counted but not printed\n");
    }
    return frc_test_last_failed;
}

static inline void frc_test_pass(void)
{
    frc_test_last_failed = 0;
}

#define TEST_SECTION(name) (frc_test_section = (name))

#define CHECK(condition)                                                        \
    do {                                                                        \
        frc_test_checks++;                                                      \
        if (condition) {                                                        \
            frc_test_pass();                                                    \
        } else if (frc_test_fail(__FILE__, __LINE__)) {                         \
            printf("%s\n", #condition);                                         \
        }                                                                       \
    } while (0)

#define CHECK_EQ_I64(actual, expected)                                          \
    do {                                                                        \
        int64_t frc_test_a = (int64_t)(actual);                                 \
        int64_t frc_test_e = (int64_t)(expected);                               \
        frc_test_checks++;                                                      \
        if (frc_test_a == frc_test_e) {                                         \
            frc_test_pass();                                                    \
        } else if (frc_test_fail(__FILE__, __LINE__)) {                         \
            printf("%s == %" PRId64 ", expected %" PRId64 " (%s)\n",            \
                   #actual, frc_test_a, frc_test_e, #expected);                 \
        }                                                                       \
    } while (0)

#define CHECK_EQ_U64(actual, expected)                                          \
    do {                                                                        \
        uint64_t frc_test_a = (uint64_t)(actual);                               \
        uint64_t frc_test_e = (uint64_t)(expected);                             \
        frc_test_checks++;                                                      \
        if (frc_test_a == frc_test_e) {                                         \
            frc_test_pass();                                                    \
        } else if (frc_test_fail(__FILE__, __LINE__)) {                         \
            printf("%s == %" PRIu64 ", expected %" PRIu64 " (%s)\n",            \
                   #actual, frc_test_a, frc_test_e, #expected);                 \
        }                                                                       \
    } while (0)

#define CHECK_STR(actual, expected)                                             \
    do {                                                                        \
        const char *frc_test_a = (actual);                                      \
        const char *frc_test_e = (expected);                                    \
        frc_test_checks++;                                                      \
        if (strcmp(frc_test_a, frc_test_e) == 0) {                              \
            frc_test_pass();                                                    \
        } else if (frc_test_fail(__FILE__, __LINE__)) {                         \
            printf("%s == \"%s\", expected \"%s\"\n", #actual, frc_test_a,      \
                   frc_test_e);                                                 \
        }                                                                       \
    } while (0)

#define CHECK_NOTE(...)                                                         \
    do {                                                                        \
        if (frc_test_last_failed) {                                             \
            printf("     ");                                                    \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
        }                                                                       \
    } while (0)

/* Print the one-line verdict; the value is the program's exit code. */
static inline int frc_test_summary(const char *name)
{
    printf("%s: %d checks, %d failed%s\n", name, frc_test_checks, frc_test_failures,
           frc_test_failures == 0 ? "" : "  <-- FAILED");
    return frc_test_failures == 0 ? 0 : 1;
}
#define TEST_SUMMARY(name) frc_test_summary(name)

#endif /* FRC_TEST_H */
