/*
 * check.h - minimal unit-test helpers for the host-side tests (make test).
 */
#ifndef TESTS_CHECK_H
#define TESTS_CHECK_H

#include <stdio.h>
#include <string.h>

static int check_failures, check_count;

#define CHECK(cond) do { \
	check_count++; \
	if (!(cond)) { \
		check_failures++; \
		fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

#define CHECK_EQ(a, b) do { \
	long long a_ = (long long)(a), b_ = (long long)(b); \
	check_count++; \
	if (a_ != b_) { \
		check_failures++; \
		fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %lld, expected %s == %lld\n", \
		        __FILE__, __LINE__, #a, a_, #b, b_); \
	} \
} while (0)

#define CHECK_STR(a, b) do { \
	const char *a_ = (a), *b_ = (b); \
	check_count++; \
	if (strcmp(a_, b_) != 0) { \
		check_failures++; \
		fprintf(stderr, "%s:%d: CHECK_STR failed: \"%s\" != \"%s\"\n", \
		        __FILE__, __LINE__, a_, b_); \
	} \
} while (0)

#define CHECK_DONE() do { \
	printf("%s: %d checks, %d failed\n", __FILE__, check_count, check_failures); \
	return check_failures ? 1 : 0; \
} while (0)

#endif
