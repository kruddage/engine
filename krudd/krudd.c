/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * krudd — the one command you run everything off.
 *
 * Phase 1 is a proof-of-life seam, nothing clever: `krudd build` shells out to
 * cmake to build the engine (the exact thing CI does today), and the no-arg
 * dispatch resolves how many "<name>.krudd-project" files live in the current
 * directory and picks a mode from that. The cmake syscall is destined to become
 * s7-driven codegen (`s7 build.scm | cmake`) and the setup flow is destined to
 * grow a GitHub-backed project scaffolder — but the point of this file is the
 * seam, not the insides. Everything here is meant to be gutted from behind
 * while the front door (`krudd`) stays put.
 *
 * Built with the system compiler by krudd.sh; it owns no engine headers.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "s7.h"

#define PROJECT_SUFFIX ".krudd-project"
#define MAX_PROJECTS   64
#define NAME_MAX_LEN   256

/* Echo then run a shell command; return 0 on success, -1 otherwise. */
static int run(const char *cmd)
{
	int rc;

	printf("krudd: $ %s\n", cmd);
	fflush(stdout);
	rc = system(cmd);
	return (rc == 0) ? 0 : -1;
}

static const char *getenv_or(const char *key, const char *dflt)
{
	const char *v = getenv(key);

	return (v && *v) ? v : dflt;
}

/*
 * KRUDD_CONFIGURE / KRUDD_BUILD let CI inject the WASM toolchain (`emcmake
 * cmake ...`) while a plain checkout falls back to native cmake, so the seam is
 * verifiable both ways without baking a toolchain choice into this file.
 */
static const char *configure_cmd(void)
{
	return getenv_or("KRUDD_CONFIGURE", "cmake -B build");
}

static const char *build_cmd(void)
{
	return getenv_or("KRUDD_BUILD", "cmake --build build");
}

/* Direct fallback when the Scheme build description is unreachable. */
static int direct_cmake(void)
{
	if (run(configure_cmd()) != 0)
		return -1;
	return run(build_cmd());
}

/* (run "cmd") from Scheme -> run the shell command, return its exit status. */
static s7_pointer krudd_run(s7_scheme *sc, s7_pointer args)
{
	s7_pointer cmd = s7_car(args);

	if (!s7_is_string(cmd))
		return s7_make_integer(sc, -1);
	return s7_make_integer(sc, run(s7_string(cmd)));
}

/*
 * The payoff: `krudd build` loads build.scm and lets s7 drive. C provides the
 * primitives (`run`, the command strings); Scheme orchestrates. If s7 or the
 * script is unreachable we fall back to driving cmake directly, so a bare krudd
 * binary still builds.
 */
static int cmd_build(void)
{
	char        path[1024];
	s7_scheme  *s7;
	s7_pointer  port, saved;
	const char *errtext;
	FILE       *probe;
	int         failed;

	snprintf(path, sizeof path, "%s/krudd/build.scm",
		 getenv_or("KRUDD_ROOT", "."));
	probe = fopen(path, "r");
	if (!probe) {
		fprintf(stderr, "krudd: %s not found — driving cmake directly\n",
			path);
		return direct_cmake();
	}
	fclose(probe);

	s7 = s7_init();
	if (!s7)
		return direct_cmake();

	s7_define_function(s7, "run", krudd_run, 1, 0, false,
			   "(run cmd) run a shell command, return its exit status");
	s7_define_variable(s7, "*configure*",
			   s7_make_string(s7, configure_cmd()));
	s7_define_variable(s7, "*build*", s7_make_string(s7, build_cmd()));

	/* Capture Scheme errors instead of letting them scribble on stderr. */
	port  = s7_open_output_string(s7);
	saved = s7_set_current_error_port(s7, port);
	s7_load(s7, path);
	errtext = s7_get_output_string(s7, port);
	failed  = (errtext && errtext[0] != '\0');
	if (failed)
		fprintf(stderr, "krudd: build.scm failed:\n%s\n", errtext);
	s7_set_current_error_port(s7, saved);
	s7_close_output_port(s7, port);
	s7_free(s7);
	return failed ? -1 : 0;
}

/* Serve the built site. Blocks until Ctrl-C — the interactive "run" tail. */
static int cmd_serve(void)
{
	printf("krudd: serving build/ at http://localhost:8000 "
	       "(Ctrl-C to stop)\n");
	return run("cd build && python3 -m http.server 8000");
}

/* Collect the "<name>.krudd-project" files in the current directory. */
static int list_projects(char names[][NAME_MAX_LEN], int max)
{
	DIR           *d = opendir(".");
	struct dirent *e;
	size_t         suffix = strlen(PROJECT_SUFFIX);
	int            n = 0;

	if (!d)
		return 0;
	while ((e = readdir(d)) && n < max) {
		size_t len = strlen(e->d_name);

		if (len > suffix &&
		    strcmp(e->d_name + len - suffix, PROJECT_SUFFIX) == 0)
			snprintf(names[n++], NAME_MAX_LEN, "%s", e->d_name);
	}
	closedir(d);
	return n;
}

/* Read one line from stdin into buf (newline stripped). -1 on EOF. */
static int prompt(const char *label, char *buf, size_t size)
{
	printf("%s", label);
	fflush(stdout);
	if (!fgets(buf, (int)size, stdin))
		return -1;
	buf[strcspn(buf, "\n")] = '\0';
	return 0;
}

/* Scaffold a "<name>.krudd-project" from a name + description prompt. */
static int scaffold_project(void)
{
	char name[NAME_MAX_LEN];
	char desc[512];
	char path[NAME_MAX_LEN + 32];
	FILE *f;

	if (prompt("Project name: ", name, sizeof name) != 0 || !name[0]) {
		fprintf(stderr, "krudd: no name given\n");
		return -1;
	}
	if (prompt("Description: ", desc, sizeof desc) != 0)
		desc[0] = '\0';

	snprintf(path, sizeof path, "%s%s", name, PROJECT_SUFFIX);
	f = fopen(path, "w");
	if (!f) {
		perror("krudd: fopen");
		return -1;
	}
	fprintf(f, "name = %s\ndescription = %s\n", name, desc);
	fclose(f);
	printf("krudd: created %s\n", path);
	return 0;
}

/* Load a single project: build it, then serve. */
static int run_project(const char *project)
{
	printf("krudd: loading project %s\n", project);
	if (cmd_build() != 0)
		return -1;
	return cmd_serve();
}

/* More than one project: list and let the user pick. */
static int pick_and_run(char names[][NAME_MAX_LEN], int n)
{
	char choice[16];
	int  i, sel;

	printf("krudd: multiple projects here:\n");
	for (i = 0; i < n; i++)
		printf("  [%d] %s\n", i + 1, names[i]);
	if (prompt("load which? ", choice, sizeof choice) != 0)
		return -1;
	sel = atoi(choice);
	if (sel < 1 || sel > n) {
		fprintf(stderr, "krudd: no such project\n");
		return -1;
	}
	return run_project(names[sel - 1]);
}

/* No-arg dispatch: 0 → setup, 1 → run it, many → pick. */
static int dispatch(void)
{
	char names[MAX_PROJECTS][NAME_MAX_LEN];
	int  n = list_projects(names, MAX_PROJECTS);

	if (n == 0) {
		printf("krudd: no %s here — this is the bare krudd repo.\n",
		       PROJECT_SUFFIX);
		if (cmd_build() != 0)
			return -1;
		printf("krudd: let's set up a project.\n");
		return scaffold_project();
	}
	if (n == 1)
		return run_project(names[0]);
	return pick_and_run(names, n);
}

static void usage(void)
{
	fprintf(stderr,
		"usage: krudd [command]\n"
		"  (no args)     resolve projects here: setup / run / pick\n"
		"  build         configure + build (no prompts; used by CI)\n"
		"  run           build, then serve the site\n"
		"  new-project   scaffold a <name>.krudd-project\n");
}

int main(int argc, char **argv)
{
	if (argc < 2)
		return dispatch() == 0 ? 0 : 1;

	if (strcmp(argv[1], "build") == 0)
		return cmd_build() == 0 ? 0 : 1;
	if (strcmp(argv[1], "run") == 0)
		return (cmd_build() == 0 && cmd_serve() == 0) ? 0 : 1;
	if (strcmp(argv[1], "new-project") == 0)
		return scaffold_project() == 0 ? 0 : 1;

	fprintf(stderr, "krudd: unknown command '%s'\n", argv[1]);
	usage();
	return 2;
}
