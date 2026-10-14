/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 */

#include "dv.hpp"

#include <errno.h>
#include <string.h>
#include <unistd.h>

int	dv_debug;
int	dv_mono;

static void
usage(void)
{
	fprintf(stderr,
	    "usage: goblin-view [-Dmv] [-L name] [-S path] [command [args]]\n"
	    "  -m  paint this terminal in strict black and white (e-paper);\n"
	    "      also GOBLIN_VIEW_MONO=1 in the environment\n"
	    "commands:\n"
	    "  (none)          attach, creating a session if needed\n"
	    "  new-session     [-d] [-x cols] [-y rows] [-s name] [cmd...]\n"
	    "  new-window      [-s name] [cmd...]   (open a window in a session)\n"
	    "  attach          [-s name]\n"
	    "  list-windows    [-s name]\n"
	    "  list-sessions\n"
	    "  kill-session    [name]\n"
	    "  kill-server\n"
	    "  list-clients\n"
	    "  send-keys       [-s name] [-w N] [-l] text...   (C escapes unless -l)\n"
	    "  capture-window  [-s name] [-w N] [-S lines|-]   (the window's text)\n"
	    "  pipe-window     [-s name] [-w N] [path]         (log output; no path stops)\n"
	    "  save            [-f file]   (every session's windows, cwd and command)\n"
	    "  restore         [-f file]   (recreate them; default ~/.local/state/goblin-view/sessions)\n"
	    "  upgrade         [binary]    (the server becomes this program, in place;\n"
	    "                               windows and attached terminals carry on)\n"
	    "  menu            <key>      (O/C/R/Z/Q/1-9, M/P/H, arrows)\n"
	    "config file keys (0 means no limit; see goblin-view(1)):\n"
	    "  csi-limit       max bytes of one CSI (ESC [); also GOBLIN_VIEW_CSI_LIMIT\n"
	    "  escape-limit    max bytes of one OSC, APC or DCS; also GOBLIN_VIEW_ESCAPE_LIMIT\n"
	    "see goblin-view(1) and goblin-view-protocols(7)\n");
	exit(1);
}

static int
is_command(const char *s)
{
	return (strcmp(s, "new-session") == 0 ||
	    strcmp(s, "new-window") == 0 ||
	    strcmp(s, "attach") == 0 ||
	    strcmp(s, "refresh") == 0 ||
	    strcmp(s, "mouse") == 0 ||
	    strcmp(s, "list-windows") == 0 ||
	    strcmp(s, "list-clients") == 0 ||
	    strcmp(s, "list-sessions") == 0 ||
	    strcmp(s, "kill-session") == 0 ||
	    strcmp(s, "kill-server") == 0 ||
	    strcmp(s, "upgrade") == 0 ||
	    strcmp(s, "takeover") == 0 ||
	    strcmp(s, "save") == 0 ||
	    strcmp(s, "restore") == 0 ||
	    strcmp(s, "send-keys") == 0 ||
	    strcmp(s, "capture-window") == 0 ||
	    strcmp(s, "pipe-window") == 0 ||
	    strcmp(s, "menu") == 0);
}

static void
config_warn(const char *path, int line, const char *msg)
{
	fprintf(stderr, "goblin-view: %s:%d: %s\n", path, line, msg);
}

static int
command_is_display(int argc, char **argv)
{
	int	i;

	if (argc < 1)
		return (1);
	if (strcmp(argv[0], "attach") == 0)
		return (1);
	if (strcmp(argv[0], "new-session") == 0) {
		for (i = 1; i < argc; i++) {
			if (strcmp(argv[i], "-d") == 0)
				return (0);
		}
		return (1);
	}
	return (0);
}

int
main(int argc, char **argv)
{
	int		 opt;
	const char	*label = "default";
	const char	*path_override = nullptr;
	const char	*dv_sock = getenv("GOBLIN_VIEW_SOCK");
	int		 have_label = 0;
	char		*sock;
	char		**cmdv;
	char		 attach_lit[] = "attach";
	char		*attach_cmd[] = { attach_lit, nullptr };
	int		 display;
	struct dv_size	 sz;
	const char	*term;

	/*
	 * Stop at the subcommand so GNU getopt does not steal -d/-x/-s
	 * from new-session.
	 */
	{
		const char	*m = getenv("GOBLIN_VIEW_MONO");

		if (m != NULL && m[0] != '\0' && strcmp(m, "0") != 0)
			dv_mono = 1;
	}
	while ((opt = getopt(argc, argv, "+DL:S:mvhV")) != -1) {
		switch (opt) {
		case 'D':
			dv_debug = 1;
			dv_verbose = 1;
			break;
		case 'm':
			dv_mono = 1;
			break;
		case 'L':
			label = optarg;
			have_label = 1;
			break;
		case 'S':
			path_override = optarg;
			have_label = 1;
			break;
		case 'v':
			dv_verbose = 1;
			break;
		case 'V':
			printf("goblin-view %s\n", DV_VERSION);
			printf("data directory: %s\n", dv_im_datadir());
			return (0);
		case 'h':
			usage();
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc > 0 && !is_command(argv[0]))
		usage();

	{
		/* Complain about the config file now, while stderr is readable. */
		struct dv_config	cfg;

		dv_config_defaults(&cfg);
		dv_config_load(&cfg, dv_config_path(), NULL, config_warn);
	}

	if (argc == 0)
		cmdv = attach_cmd;
	else
		cmdv = argv;

	memset(&sz, 0, sizeof sz);
	display = command_is_display(argc == 0 ? 1 : argc, cmdv);
	if (display && !isatty(STDIN_FILENO)) {
		fprintf(stderr, "goblin-view: attach needs a tty\n");
		return (1);
	}
	if (display) {
		if (dv_tty_size(&sz) < 0) {
			fprintf(stderr, "goblin-view: tty size: %s\n", strerror(errno));
			return (1);
		}
	}
	term = getenv("TERM");
	if (term == NULL)
		term = "";

	/*
	 * Inside a goblin-view window ($GOBLIN_VIEW_SOCK set): talk to THIS server, and never
	 * nest a display -- a recursive attach is a terminal in a terminal.
	 * Window-manager commands still work; they act on this desktop.
	 */
	if (dv_sock != NULL && dv_sock[0] != '\0') {
		if (display) {
			fprintf(stderr,
			    "goblin-view: already inside a goblin-view session.\n"
			    "    Tap Alt (or Ctrl-\\) for the menu, or run "
			    "a command, e.g.:\n"
			    "      goblin-view new-window            open another window\n"
			    "      goblin-view list-windows          list this desktop\n"
			    "      goblin-view menu o                open  (also c/z/1-9)\n");
			return (1);
		}
		if (!have_label && path_override == nullptr)
			path_override = dv_sock;
	}

	sock = dv_socket_path(label, path_override);
	if (strcmp(cmdv[0], "takeover") == 0) {	/* we are the new server */
		if (argc < 2)
			usage();
		return (dv_server_takeover(cmdv[1], sock, dv_debug));
	}
	if (strcmp(cmdv[0], "upgrade") == 0 && argc == 1) {
		/* to this program, by default */
		static char	exe[DV_PATH_MAX], real[DV_PATH_MAX];
		static char	*upgrade_cmd[3];

		if (dv_exe_path(exe, sizeof exe) && realpath(exe, real) != NULL) {
			upgrade_cmd[0] = cmdv[0];
			upgrade_cmd[1] = real;
			upgrade_cmd[2] = NULL;
			cmdv = upgrade_cmd;
		}
	}
	opt = dv_client_run(sock, display, sz, term, cmdv);
	free(sock);
	return (opt);
}
