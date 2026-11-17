/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 */

#include "dv.hpp"

#include <errno.h>
#include <getopt.h>
#include <string.h>
#include <unistd.h>

int	dv_debug;
int	dv_mono;
float	dv_focus_px;

static void
usage(void)
{
	fprintf(stderr,
	    "Odin Seat — a desktop in your terminal\n"
	    "usage: odin-seat [-ADmv] [--3d-focus=X] [-L name] [-S path] [command [args]]\n"
	    "  -A  allow the local SSH_AUTH_SOCK when the terminal has no agent\n"
	    "  -m  paint this terminal in strict black and white (e-paper);\n"
	    "      also ODIN_SEAT_MONO=1 in the environment\n"
	    "  --3d-focus=X  on a terminal that shows Silkie depth, the focused\n"
	    "      window and all it shows X pixels nearer (negative: farther)\n"
	    "commands:\n"
	    "  (none)          attach, creating a session if needed\n"
	    "  new-session     [-d] [-x cols] [-y rows] [-s name] [cmd...]\n"
	    "  new-window      [-s name] [cmd...]   (open a window in a session)\n"
	    "  split-window    [-h|-v] [-s name] [-w N] [-- cmd...]\n"
	    "                  -h opens to the right (default); -v opens below\n"
	    "  kill-window     [-f] [-s name] [-w N]  (-f kills hung programs)\n"
	    "  attach          [-s name]\n"
	    "  list-windows    [-s name]\n"
	    "  list-sessions\n"
	    "  kill-session    [name]\n"
	    "  kill-server\n"
	    "  list-clients\n"
	    "  agent-socket    [-s name]   (stable SSH_AUTH_SOCK for this session)\n"
	    "  send-keys       [-s name] [-w N] [-l] text...   (C escapes unless -l)\n"
	    "  capture-window  [-s name] [-w N] [-S lines|-]   (the window's text)\n"
	    "  pipe-window     [-s name] [-w N] [path]         (log output; no path stops)\n"
	    "  save            [-f file]   (every session's windows, cwd and command)\n"
	    "  restore         [-f file]   (recreate them; default ~/.local/state/odin-seat/sessions)\n"
	    "  upgrade         [binary]    (the server becomes this program, in place;\n"
	    "                               windows and attached terminals carry on)\n"
	    "  menu            <key>      (O/C/E/K/R/Z/Q/1-9, M/P/H, arrows)\n"
	    "config file keys (0 means no limit; see odin-seat(1)):\n"
	    "  csi-limit       max bytes of one CSI (ESC [); also ODIN_SEAT_CSI_LIMIT\n"
	    "  escape-limit    max bytes of one OSC, APC or DCS; also ODIN_SEAT_ESCAPE_LIMIT\n"
	    "see odin-seat(1) and odin-seat-protocols(7)\n");
	exit(1);
}

static int
is_command(const char *s)
{
	return (strcmp(s, "new-session") == 0 ||
	    strcmp(s, "new-window") == 0 ||
	    strcmp(s, "split-window") == 0 ||
	    strcmp(s, "kill-window") == 0 ||
	    strcmp(s, "attach") == 0 ||
	    strcmp(s, "refresh") == 0 ||
	    strcmp(s, "mouse") == 0 ||
	    strcmp(s, "list-windows") == 0 ||
	    strcmp(s, "list-clients") == 0 ||
	    strcmp(s, "agent-socket") == 0 ||
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
	fprintf(stderr, "odin-seat: %s:%d: %s\n", path, line, msg);
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
	const char	*dv_sock = getenv("ODIN_SEAT_SOCK");
	int		 have_label = 0;
    bool local_agent = false;
	char		*sock;
	char		**cmdv;
	std::vector<char *>	scoped_command;
	char		*upgrade_path = nullptr;
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
		const char	*m = getenv("ODIN_SEAT_MONO");

		if (m != NULL && m[0] != '\0' && strcmp(m, "0") != 0)
			dv_mono = 1;
	}
	static const struct option longopts[] = {
		{ "3d-focus", required_argument, nullptr, 'F' },
		{ nullptr, 0, nullptr, 0 }
	};

	while ((opt = getopt_long(argc, argv, "+ADL:S:mvhV", longopts,
	    nullptr)) != -1) {
		switch (opt) {
		case 'F':
			if (!dv_parse_px(optarg, &dv_focus_px)) {
				fprintf(stderr, "odin-seat: --3d-focus: not a number "
				    "of pixels: %s\n", optarg);
				return (1);
			}
			break;
		case 'A': local_agent = true; break;
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
			printf("Odin Seat %s\n", DV_VERSION);
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
		fprintf(stderr, "odin-seat: attach needs a tty\n");
		return (1);
	}
	if (display) {
		if (dv_tty_size(&sz) < 0) {
			fprintf(stderr, "odin-seat: tty size: %s\n", strerror(errno));
			return (1);
		}
	}
	term = getenv("TERM");
	if (term == NULL)
		term = "";

	/*
	 * Inside a odin-seat window ($ODIN_SEAT_SOCK set): talk to THIS server, and never
	 * nest a display -- a recursive attach is a terminal in a terminal.
	 * Window-manager commands still work; they act on this desktop.
	 */
	if (dv_sock != NULL && dv_sock[0] != '\0') {
		if (display) {
			fprintf(stderr,
			    "odin-seat: already inside a odin-seat session.\n"
			    "    Tap Alt (or Ctrl-\\) for the menu, or run "
			    "a command, e.g.:\n"
			    "      odin-seat new-window            open another window\n"
			    "      odin-seat list-windows          list this desktop\n"
			    "      odin-seat menu o                open  (also c/z/1-9)\n");
			return (1);
		}
		if (!have_label && path_override == nullptr)
			path_override = dv_sock;
		const char *session = getenv("ODIN_SEAT_SESSION");
		if (!have_label && path_override != nullptr &&
		    strcmp(path_override, dv_sock) == 0 && session != nullptr &&
		    session[0] != '\0' && (strcmp(cmdv[0], "split-window") == 0 ||
		    strcmp(cmdv[0], "kill-window") == 0 || strcmp(cmdv[0], "agent-socket") == 0)) {
			int explicit_session = 0;

			for (int i = 1; cmdv[i] != nullptr; i++) {
				if (strcmp(cmdv[i], "--") == 0 || cmdv[i][0] != '-')
					break;
				if (strcmp(cmdv[i], "-s") == 0) {
					explicit_session = 1;
					break;
				}
				if (strcmp(cmdv[i], "-w") == 0 && cmdv[i + 1] != nullptr)
					i++;
			}
			if (!explicit_session) {
				scoped_command = {cmdv[0], const_cast<char *>("-s"),
				    const_cast<char *>(session)};
				for (int i = 1; cmdv[i] != nullptr; i++)
					scoped_command.push_back(cmdv[i]);
				scoped_command.push_back(nullptr);
				cmdv = scoped_command.data();
			}
		}
	}

	sock = dv_socket_path(label, path_override);
	if (strcmp(cmdv[0], "takeover") == 0) {	/* we are the new server */
		if (argc < 2)
			usage();
		return (dv_server_takeover(cmdv[1], sock, dv_debug));
	}
	if (strcmp(cmdv[0], "upgrade") == 0 && argc == 1) {
		/* to this program, by default */
		static char	*upgrade_cmd[3];

		if ((upgrade_path = dv_exe_path()) != nullptr) {
			upgrade_cmd[0] = cmdv[0];
			upgrade_cmd[1] = upgrade_path;
			upgrade_cmd[2] = NULL;
			cmdv = upgrade_cmd;
		}
	}
	opt = dv_client_run(sock, display, sz, term, cmdv, local_agent);
	free(upgrade_path);
	free(sock);
	return (opt);
}
