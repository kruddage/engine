/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * krudd — the one command you run everything off.
 *
 * `krudd build` loads krudd/kruddmake/build.scm and lets s7 drive: C provides the `run`
 * primitive, Scheme renders build.ninja from the directory specs and drives
 * ninja(1) directly (no CMake). The no-arg dispatch resolves how many
 * "<name>.krudd-project" files live in the current directory and picks a mode
 * from that; the setup flow is destined to grow a GitHub-backed project
 * scaffolder. The point of this file is the front door (`krudd`); the insides
 * live in Scheme and are meant to be gutted from behind while it stays put.
 *
 * Built with the system compiler by krudd.sh; it owns no engine headers.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h> /* _spawnlp, _P_WAIT — route shell-outs through sh below */
#endif

#include "s7.h"

#define PROJECT_SUFFIX ".krudd-project"
#define MAX_PROJECTS   64
#define NAME_MAX_LEN   256

/*
 * Run a command through a POSIX shell and return its exit status (0 / -1).
 *
 * On POSIX this is plain system(). On Windows the CRT's system() shells out to
 * cmd.exe, whose builtins (mkdir, date) and redirection syntax don't match the
 * POSIX one-liners the build specs emit — `mkdir -p`, `date -u +…`,
 * `2>/dev/null` (see build.scm, ninja.scm, introspect.scm). The build always
 * runs under MSYS2 (see the Windows editor CI), so sh is on PATH: route through
 * it. Passing cmd as a single spawn argument — not embedded in another quoted
 * string — lets the MSYS2 runtime de-quote it the way sh expects, so nothing
 * here has to re-quote cmd's own quotes.
 */
static int shell_run(const char *cmd)
{
#ifdef _WIN32
	intptr_t rc = _spawnlp(_P_WAIT, "sh", "sh", "-c", cmd, (char *)NULL);

	return (rc == 0) ? 0 : -1;
#else
	int rc = system(cmd);

	return (rc == 0) ? 0 : -1;
#endif
}

/* Echo then run a shell command; return 0 on success, -1 otherwise. */
static int run(const char *cmd)
{
	printf("krudd: $ %s\n", cmd);
	fflush(stdout);
	return shell_run(cmd);
}

static const char *getenv_or(const char *key, const char *dflt)
{
	const char *v = getenv(key);

	return (v && *v) ? v : dflt;
}

/* setenv is POSIX; mingw/Windows only ships _putenv_s. Wrap the platform call
 * behind one helper that keeps setenv's (name, value, overwrite) contract —
 * overwrite == 0 leaves an existing value untouched — so cmd_editor reads the
 * same on every host (the native editor targets Windows too, see krudd_qt.cpp). */
static int krudd_setenv(const char *name, const char *value, int overwrite)
{
#ifdef _WIN32
	if (!overwrite && getenv(name))
		return 0;
	return _putenv_s(name, value);
#else
	return setenv(name, value, overwrite);
#endif
}

/* (run "cmd") from Scheme -> run the shell command, return its exit status. */
static s7_pointer krudd_run(s7_scheme *sc, s7_pointer args)
{
	s7_pointer cmd = s7_car(args);

	if (!s7_is_string(cmd))
		return s7_make_integer(sc, -1);
	return s7_make_integer(sc, run(s7_string(cmd)));
}

#ifdef _WIN32
/*
 * (system cmd [capture]) — override s7's built-in on Windows so Scheme's
 * `system` calls go through sh too, the same as `run`. Without this, the
 * build's `(system "mkdir -p …")` (ninja.scm) and `(system … #t)` metadata
 * probes (introspect.scm's git/date) would hit cmd.exe and fail. Matches s7's
 * contract: a true second argument captures stdout and returns it as a string;
 * otherwise return the exit status. Left as s7's own primitive on POSIX so
 * those builds are byte-for-byte unchanged.
 *
 * Capture redirects the child's stdout to a temp file (paths slash-normalized
 * so MSYS2 sh opens them) rather than _popen, which would itself relaunch
 * cmd.exe — the whole point is to avoid it.
 */
static s7_pointer krudd_system(s7_scheme *sc, s7_pointer args)
{
	s7_pointer  cmd = s7_car(args);
	const char *c;

	if (!s7_is_string(cmd))
		return s7_make_integer(sc, -1);
	c = s7_string(cmd);

	/* Capture requested when a second, non-#f argument is present. */
	if (s7_is_pair(s7_cdr(args)) && s7_boolean(sc, s7_cadr(args))) {
		char   *tmp = _tempnam(NULL, "kruddsys");
		char   *redir, *out;
		size_t  need, cap = 0, len = 0;
		FILE   *f;
		int     ch;
		s7_pointer result;

		if (!tmp)
			return s7_make_string(sc, "");
		for (char *p = tmp; *p; p++)
			if (*p == '\\')
				*p = '/';

		need  = strlen(c) + strlen(tmp) + 8;
		redir = (char *)malloc(need);
		if (!redir) {
			free(tmp);
			return s7_make_string(sc, "");
		}
		snprintf(redir, need, "%s > \"%s\"", c, tmp);
		shell_run(redir);
		free(redir);

		f = fopen(tmp, "rb");
		if (!f) {
			free(tmp);
			return s7_make_string(sc, "");
		}
		out = NULL;
		while ((ch = fgetc(f)) != EOF) {
			if (len + 1 >= cap) {
				size_t ncap = cap ? cap * 2 : 128;
				char  *n    = (char *)realloc(out, ncap);

				if (!n)
					break;
				out = n;
				cap = ncap;
			}
			out[len++] = (char)ch;
		}
		fclose(f);
		remove(tmp);
		free(tmp);
		if (!out)
			return s7_make_string(sc, "");
		out[len] = '\0';
		result = s7_make_string(sc, out);
		free(out);
		return result;
	}

	return s7_make_integer(sc, shell_run(c));
}
#endif

/*
 * `krudd build` loads build.scm and lets s7 drive. C provides the `run`
 * primitive; Scheme orchestrates. build.scm is part of the checkout, so a
 * missing script or a dead interpreter is a hard error, not a fallback.
 */
static int cmd_build(void)
{
	char        path[1024];
	s7_scheme  *s7;
	s7_pointer  port, saved;
	const char *errtext;
	FILE       *probe;
	int         failed;

	snprintf(path, sizeof path, "%s/krudd/kruddmake/build.scm",
		 getenv_or("KRUDD_ROOT", "."));
	probe = fopen(path, "r");
	if (!probe) {
		fprintf(stderr, "krudd: %s not found\n", path);
		return -1;
	}
	fclose(probe);

	s7 = s7_init();
	if (!s7) {
		fprintf(stderr, "krudd: could not start s7\n");
		return -1;
	}

	s7_define_function(s7, "run", krudd_run, 1, 0, false,
			   "(run cmd) run a shell command, return its exit status");

#ifdef _WIN32
	/* Override s7's built-in `system` so the build's Scheme-level shell-outs
	 * (mkdir, git, date — see krudd_system) go through sh rather than cmd.exe. */
	s7_define_function(s7, "system", krudd_system, 1, 1, false,
			   "(system cmd [capture]) run a shell command via sh");
#endif

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

/*
 * `krudd editor` — build the Qt editor shell and run it. The native editor:
 * the engine's Vulkan backend, presenting into a QWindow embedded in real Qt
 * chrome (menu bar, toolbar, Scene/Inspector/Assets/Console docks) on the
 * desktop (SteamOS / the Steam Deck / Windows), no browser in the path. See
 * docs/qt-editor-shell.md.
 *
 * The window target carries (vulkan) and (qt). This sets KRUDD_VULKAN and
 * KRUDD_QT for the build so the ordinary `krudd build` stays Vulkan- and
 * Qt-free — both targets are opt-in, like the old (dawn) gate. The Vulkan
 * loader and headers are an ordinary system package (setup.sh installs them
 * with the validation layers); Qt, unlike a system library on a default include
 * path, needs KRUDD_QT_CFLAGS set (normally from pkg-config) before this runs.
 *
 * `editor-qt` is kept as a back-compat alias for this command.
 */
static int cmd_editor(void)
{
	char path[1024];

	if (!getenv("KRUDD_QT_CFLAGS")) {
		fprintf(stderr,
			"krudd: editor needs Qt6 — set KRUDD_QT_CFLAGS "
			"(and usually KRUDD_QT_LIBS), e.g.:\n"
			"krudd:   KRUDD_QT_CFLAGS=\"$(pkg-config --cflags "
			"Qt6Widgets Qt6Gui Qt6Core)\"\n"
			"krudd:   KRUDD_QT_LIBS=\"$(pkg-config --libs "
			"Qt6Widgets Qt6Gui Qt6Core)\"\n"
			"krudd: see docs/qt-editor-shell.md for the recipe.\n");
		return -1;
	}

	/* Pull the (vulkan) backend and (qt) window targets into the native
	 * graph for this build. Do not clobber an explicit value. */
	krudd_setenv("KRUDD_VULKAN", "1", 0);
	krudd_setenv("KRUDD_QT", "1", 0);

	if (cmd_build() != 0)
		return -1;

	snprintf(path, sizeof path, "%s/build/bin/krudd_qt",
		 getenv_or("KRUDD_ROOT", "."));
	return run(path);
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
		"  editor        build + run the Qt editor shell (Vulkan; needs "
		"KRUDD_QT_CFLAGS)\n"
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
	/* `editor-qt` is a back-compat alias; the editor is the Qt shell now. */
	if (strcmp(argv[1], "editor") == 0 || strcmp(argv[1], "editor-qt") == 0)
		return cmd_editor() == 0 ? 0 : 1;
	if (strcmp(argv[1], "new-project") == 0)
		return scaffold_project() == 0 ? 0 : 1;

	fprintf(stderr, "krudd: unknown command '%s'\n", argv[1]);
	usage();
	return 2;
}
