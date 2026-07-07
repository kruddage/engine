/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "scheme.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const struct scheme_api *s;

static void test_arithmetic(void)
{
	int64_t v = 0;

	assert(s->eval_int("(+ 1 2)", &v) == 0 && v == 3);
	assert(s->eval_int("(* 6 7)", &v) == 0 && v == 42);
	assert(s->eval_int("(- (expt 2 10) 1)", &v) == 0 && v == 1023);
}

static void test_strings(void)
{
	char buf[64];

	assert(s->eval_string("(string-append \"foo\" \"bar\")",
			      buf, sizeof buf) == 0);
	assert(strcmp(buf, "foobar") == 0);

	assert(s->eval_string("(symbol->string 'hello)", buf, sizeof buf) == 0);
	assert(strcmp(buf, "hello") == 0);

	/* A too-small buffer is rejected, never overflowed. */
	assert(s->eval_string("(string-append \"aaaa\" \"bbbb\")", buf, 4) == -1);
}

static void test_error_path(void)
{
	int64_t v = 0;

	/* Scheme-level errors return -1 and must not crash the interpreter. */
	assert(s->eval_ok("(/ 1 0)") == -1);
	assert(s->eval_ok("(this-is-not-bound 5)") == -1);
	assert(s->eval_ok("(car '())") == -1);
	assert(s->eval_ok("(+ 1") == -1); /* unbalanced parens */

	/* A well-formed expression after the errors still succeeds. */
	assert(s->eval_ok("(+ 2 2)") == 0);

	/* Result-type mismatch: an integer accessor on a string result. */
	assert(s->eval_int("(string-append \"x\" \"y\")", &v) == -1);

	/* The interpreter survived every error above. */
	assert(s->eval_int("(* 6 7)", &v) == 0 && v == 42);
}

int main(void)
{
	s = scheme_native_api();
	assert(s != NULL);

	test_arithmetic();
	test_strings();
	test_error_path();

	printf("scheme tests passed\n");
	return 0;
}
