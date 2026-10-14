/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 */

#include "dv.hpp"

#include <sys/ioctl.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

static void
fill_winsize(struct winsize *ws, unsigned cols, unsigned rows,
    unsigned xpixel, unsigned ypixel)
{
	memset(ws, 0, sizeof *ws);
	ws->ws_col = cols < 1 ? 1 : cols;
	ws->ws_row = rows < 1 ? 1 : rows;
	/* PTY pixels are the window, not the cell — same as tmux. */
	ws->ws_xpixel = (unsigned short)(ws->ws_col * xpixel);
	ws->ws_ypixel = (unsigned short)(ws->ws_row * ypixel);
}

void
dv_pty_resize(struct dv_window *w)
{
	struct winsize		 ws;
	struct dv_desktop	*d;

	if (w->pty_fd < 0)
		return;
	d = &w->session->desktop;
	fill_winsize(&ws, w->lcols, w->lrows, d->xpixel, d->ypixel);
	if (ioctl(w->pty_fd, TIOCSWINSZ, &ws) < 0)
		dv_log("TIOCSWINSZ: %s", strerror(errno));
}

int
dv_pty_spawn(struct dv_window *w, const char *cmd, char *const *cmd_argv)
{
	struct winsize	 ws;
	struct dv_desktop *d = &w->session->desktop;
	pid_t		 pid;
	int		 fd;
	const char	*shell;
	char		*argv_shell[3];
	char *const	*av;

	fill_winsize(&ws, w->lcols, w->lrows, d->xpixel, d->ypixel);
	pid = forkpty(&fd, NULL, NULL, &ws);
	if (pid < 0) {
		dv_log("forkpty: %s", strerror(errno));
		return (-1);
	}
	if (pid == 0) {
		{
			const char	*term = getenv("GOBLIN_VIEW_TERM");

			/*
			 * xterm-256color until the inner emulator answers
			 * DA/kitty queries. TERM=xterm-ghostty makes zsh/fish
			 * wait for replies we currently passthrough to the
			 * host instead of the PTY.
			 */
			if (term == NULL || term[0] == '\0')
				term = "xterm-256color";
			setenv("TERM", term, 1);
			setenv("COLORTERM", "truecolor", 1);
		}
		/*
		 * Tell programs (and a nested goblin-view) they are inside a session,
		 * and which one, so `goblin-view` here manages this desktop instead of
		 * starting another. Mirrors tmux's $TMUX.
		 */
		if (dv_srv != NULL && dv_srv->sock_path != NULL)
			setenv("GOBLIN_VIEW_SOCK", dv_srv->sock_path, 1);
		if (w->session != NULL)
			setenv("GOBLIN_VIEW_SESSION", w->session->name, 1);
		/*
		 * The server daemonised into "/", so without this every
		 * window would start there. Prefer the client's cwd, then
		 * $HOME; a vanished directory is not fatal.
		 */
		if (w->cwd[0] == '\0' || chdir(w->cwd) < 0) {
			const char	*home = getenv("HOME");

			if (home != NULL && home[0] != '\0')
				(void)chdir(home);
		}
		signal(SIGPIPE, SIG_DFL);
		if (cmd_argv != NULL && cmd_argv[0] != NULL) {
			execvp(cmd_argv[0], cmd_argv);
			_exit(127);
		}
		if (cmd != NULL && cmd[0] != '\0') {
			execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
			_exit(127);
		}
		shell = dv_cfg.shell[0] != '\0' ? dv_cfg.shell : getenv("SHELL");
		if (shell == NULL || shell[0] == '\0')
			shell = "/bin/sh";
		argv_shell[0] = (char *)shell;
		argv_shell[1] = NULL;
		av = argv_shell;
		execvp(av[0], av);
		_exit(127);
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	w->pty_fd = fd;
	w->pid = pid;
	return (0);
}

void
dv_pty_close(struct dv_window *w)
{
	int	status;

	if (w->pty_fd >= 0) {
		close(w->pty_fd);
		w->pty_fd = -1;
	}
	if (w->pid > 0) {
		/*
		 * Hang up the whole foreground process group, not just the
		 * shell: a child still running from the shell's startup (a
		 * pyenv rehash holding its lock file, say) then gets to run
		 * its exit trap instead of being orphaned or left holding
		 * state that stalls the next shell. The process is its own
		 * session and group leader (forkpty), so -pid is the group.
		 */
		kill(-w->pid, SIGHUP);
		kill(w->pid, SIGHUP);
		waitpid(w->pid, &status, WNOHANG);
		w->pid = 0;
	}
}
