// tests/vocab_test.h - tiny shared assertion helpers for the Word Buddy host tests.
//
// Usage: include once per test program (it defines a file-local failure counter), call
// CHECK(cond, "printf format", ...) anywhere, and end main() with
// `return vocab_test_report("vocab xyz tests");`. Header-only, C11, no dependencies.
// Included with quotes, so it resolves from tests/ without an extra -I flag.
//
// Owner: lead developer. Other roles may use it; ask the lead for changes.
#pragma once

#include <stdio.h>
#include <stdlib.h>

static int vocab_test_failures;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            vocab_test_failures++;                                         \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                                  \
            fputc('\n', stderr);                                           \
        }                                                                  \
    } while (0)

// Prints "<name>: PASS" and returns EXIT_SUCCESS, or reports the failure count.
static inline int vocab_test_report(const char *name)
{
    if (vocab_test_failures) {
        fprintf(stderr, "%s: %d failure(s)\n", name, vocab_test_failures);
        return EXIT_FAILURE;
    }
    printf("%s: PASS\n", name);
    return EXIT_SUCCESS;
}
