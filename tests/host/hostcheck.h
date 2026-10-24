/*
 * hostcheck.h -- a minimal C test runner for host tests (wii-homebrew-kit).
 *
 * Host tests build project code for the PC and run it against simulated
 * hardware. This header gives them:
 *  - CHECK(cond, fmt, ...): fail the current test with a message, and return;
 *  - known_bug: a strict expected failure. The test describes the correct
 *    behaviour; while it fails it is reported as "known" and does not fail the
 *    run; once it passes, the run fails with "FIXED?" so the mark is removed
 *    with the fix;
 *  - a hang guard: a simulation that runs out of (simulated) time calls
 *    hc_hang(), which ends the current test as failed instead of hanging.
 *
 * Use: in exactly one file of the test program,
 *
 *     #define HOSTCHECK_MAIN
 *     #include "hostcheck.h"
 *
 *     static void test_roundtrip(void) { CHECK(1 + 1 == 2, "math"); }
 *     static const struct hc_test tests[] = {
 *         { "roundtrip", test_roundtrip, false },
 *     };
 *     int main(int argc, char **argv) { return hc_main(tests, HC_COUNT(tests), argc, argv, NULL); }
 *
 * Other files of the program include it without HOSTCHECK_MAIN to call hc_hang().
 * `./test NAME` runs only the tests whose name contains NAME.
 */
#ifndef HOSTCHECK_H
#define HOSTCHECK_H

#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

struct hc_test {
	const char *name;
	void (*fn)(void);
	bool known_bug;
};

#define HC_COUNT(a) (sizeof(a) / sizeof((a)[0]))

extern const char *hc_failure;
extern char hc_message[512];

/* End the current test as failed with a message (for simulations and helpers
 * deep in a call chain, where CHECK cannot return to the test). */
void hc_hang(const char *fmt, ...);

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		snprintf(hc_message, sizeof(hc_message), __VA_ARGS__); \
		hc_failure = hc_message; \
		return; \
	} \
} while (0)

/* After a helper that may CHECK-fail: stop the test too. */
#define CHECK_OK() do { if (hc_failure) return; } while (0)

#ifdef HOSTCHECK_MAIN
#include <stdarg.h>

const char *hc_failure;
char hc_message[512];
static jmp_buf hc_jump;

void hc_hang(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(hc_message, sizeof(hc_message), fmt, ap);
	va_end(ap);
	hc_failure = hc_message;
	longjmp(hc_jump, 1);
}

/* Run the tests; setup (may be NULL) runs before each one. Returns the exit
 * status: 0 when nothing failed and no known_bug test passed. */
static int hc_main(const struct hc_test *tests, size_t n, int argc, char **argv,
		   void (*setup)(void))
{
	/* static: they must survive the longjmp */
	static int passed, known, bad;
	static size_t i;
	for (i = 0; i < n; i++) {
		const struct hc_test *t = &tests[i];
		if (argc > 1 && !strstr(t->name, argv[1]))
			continue;
		hc_failure = NULL;
		if (setjmp(hc_jump) == 0) {
			if (setup)
				setup();
			t->fn();
		}
		if (!hc_failure && !t->known_bug) {
			printf("ok      %s\n", t->name);
			passed++;
		} else if (hc_failure && t->known_bug) {
			printf("known   %s: %s\n", t->name, hc_failure);
			known++;
		} else if (!hc_failure) {
			printf("FIXED?  %s passes: remove its known_bug mark\n", t->name);
			bad++;
		} else {
			printf("FAIL    %s: %s\n", t->name, hc_failure);
			bad++;
		}
	}
	printf("%d passed, %d known bugs, %d failed\n", passed, known, bad);
	return bad ? 1 : 0;
}
#endif /* HOSTCHECK_MAIN */

#endif /* HOSTCHECK_H */
