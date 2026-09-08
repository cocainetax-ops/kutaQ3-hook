#pragma once

// =============================================================================================== //
// kutaQ3 hook tests - the little assertion harness the test binaries share
//
// Every check prints one line; the summary at the end and the process exit code are what
// `make -C tests check` looks at.
// =============================================================================================== //

#include <math.h>
#include <stdio.h>

static int g_checks = 0;
static int g_failed = 0;

static void Section(const char* name)
{
	printf("\n%s\n", name);
}

static void Pass(const char* what)
{
	printf("  ok    %s\n", what);
}

static void Fail(const char* what, const char* detail)
{
	++g_failed;
	printf("  FAIL  %s: %s\n", what, detail);
}

#define CHECK_TRUE(expr, what)                                                \
	do {                                                                      \
		++g_checks;                                                           \
		if (expr) Pass(what); else Fail(what, #expr);                         \
	} while (0)

#define CHECK_INT(actual, expected, what)                                     \
	do {                                                                      \
		++g_checks;                                                           \
		const long a_ = (long)(actual), e_ = (long)(expected);                \
		if (a_ == e_) Pass(what);                                             \
		else { char b_[160]; snprintf(b_, sizeof(b_),                         \
		        "%s -> %ld, expected %ld", #actual, a_, e_);                  \
		       Fail(what, b_); }                                              \
	} while (0)

#define CHECK_UINT(actual, expected, what)                                    \
	do {                                                                      \
		++g_checks;                                                           \
		const unsigned long a_ = (unsigned long)(actual);                     \
		const unsigned long e_ = (unsigned long)(expected);                   \
		if (a_ == e_) Pass(what);                                             \
		else { char b_[160]; snprintf(b_, sizeof(b_),                         \
		        "%s -> 0x%06lx, expected 0x%06lx", #actual, a_, e_);          \
		       Fail(what, b_); }                                              \
	} while (0)

#define CHECK_STR(actual, expected, what)                                     \
	do {                                                                      \
		++g_checks;                                                           \
		if (strcmp((actual), (expected)) == 0) Pass(what);                    \
		else { char b_[192]; snprintf(b_, sizeof(b_),                         \
		        "%s -> \"%s\", expected \"%s\"", #actual, (actual), (expected)); \
		       Fail(what, b_); }                                              \
	} while (0)

#define CHECK_NEAR(actual, expected, eps, what)                               \
	do {                                                                      \
		++g_checks;                                                           \
		const double a_ = (double)(actual), e_ = (double)(expected);          \
		if (fabs(a_ - e_) <= (eps)) Pass(what);                               \
		else { char b_[192]; snprintf(b_, sizeof(b_),                         \
		        "%s -> %.6f, expected %.6f (+/-%.6f)", #actual, a_, e_, (double)(eps)); \
		       Fail(what, b_); }                                              \
	} while (0)

#define CHECK_SUMMARY(label)                                                  \
	do {                                                                      \
		printf("\n%d checks, %d failed - %s\n", g_checks, g_failed,           \
		       g_failed ? "FAILED" : "all passed");                           \
	} while (0)
