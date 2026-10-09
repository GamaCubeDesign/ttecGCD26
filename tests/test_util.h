/*
 * test_util.h — minimal assertion harness.
 *
 * Deliberately not a framework. The codec under test has zero dependencies
 * so that it compiles unchanged on the ESP32; its tests keeping the same
 * property means they can be cross-compiled and run on the target when we
 * want to prove the two ends really do agree.
 */

#ifndef TTEC_TEST_UTIL_H
#define TTEC_TEST_UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        tests_run++;                                                         \
        if (!(cond)) {                                                       \
            tests_failed++;                                                  \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

#define CHECK_EQ_INT(actual, expected)                                       \
    do {                                                                     \
        tests_run++;                                                         \
        long long a_ = (long long)(actual);                                  \
        long long e_ = (long long)(expected);                                \
        if (a_ != e_) {                                                      \
            tests_failed++;                                                  \
            printf("  FAIL %s:%d: %s == %lld, expected %lld\n",              \
                   __FILE__, __LINE__, #actual, a_, e_);                     \
        }                                                                    \
    } while (0)

#define CHECK_NEAR(actual, expected, tol)                                    \
    do {                                                                     \
        tests_run++;                                                         \
        double a_ = (double)(actual);                                        \
        double e_ = (double)(expected);                                      \
        double d_ = a_ > e_ ? a_ - e_ : e_ - a_;                             \
        if (!(d_ <= (double)(tol))) {                                        \
            tests_failed++;                                                  \
            printf("  FAIL %s:%d: %s == %.9f, expected %.9f +/- %.9f\n",     \
                   __FILE__, __LINE__, #actual, a_, e_, (double)(tol));      \
        }                                                                    \
    } while (0)

#define CHECK_STR_EQ(actual, expected)                                       \
    do {                                                                     \
        tests_run++;                                                         \
        if (strcmp((actual), (expected)) != 0) {                             \
            tests_failed++;                                                  \
            printf("  FAIL %s:%d: %s == \"%s\", expected \"%s\"\n",          \
                   __FILE__, __LINE__, #actual, (actual), (expected));       \
        }                                                                    \
    } while (0)

#define CHECK_MEM_EQ(actual, expected, len)                                  \
    do {                                                                     \
        tests_run++;                                                         \
        if (memcmp((actual), (expected), (len)) != 0) {                      \
            tests_failed++;                                                  \
            printf("  FAIL %s:%d: %s differs from %s\n",                     \
                   __FILE__, __LINE__, #actual, #expected);                  \
            printf("        got:      ");                                    \
            for (size_t i_ = 0; i_ < (size_t)(len); i_++)                    \
                printf("%02X ", ((const unsigned char *)(actual))[i_]);      \
            printf("\n        expected: ");                                  \
            for (size_t i_ = 0; i_ < (size_t)(len); i_++)                    \
                printf("%02X ", ((const unsigned char *)(expected))[i_]);    \
            printf("\n");                                                    \
        }                                                                    \
    } while (0)

#define TEST_GROUP(name) printf("%s\n", (name))

#define TEST_SUMMARY(suite)                                                  \
    do {                                                                     \
        printf("%s: %d checks, %d failed\n",                                 \
               (suite), tests_run, tests_failed);                            \
        return tests_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;              \
    } while (0)

#endif /* TTEC_TEST_UTIL_H */
