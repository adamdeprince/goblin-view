/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 */

#include "dv.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static uid_t
dv_uid(void)
{
	return (getuid());
}

char *
dv_socket_path(const char *label, const char *path_override)
{
	const char	*base;
	char		*dir, *path;

	if (path_override != nullptr && path_override[0] != '\0')
		return (xstrdup(path_override));
	if (label == NULL || label[0] == '\0')
		label = "default";
	base = getenv("XDG_RUNTIME_DIR");
	if (base != NULL && base[0] != '\0')
		dir = xasprintf("%s/goblin-view", base);
	else
		dir = xasprintf("/tmp/goblin-view-%u", (unsigned)dv_uid());
	path = xasprintf("%s/%s", dir, label);
	free(dir);
	return (path);
}

static int
ensure_dir(const char *path)
{
	struct stat	st;

	if (stat(path, &st) == 0) {
		if (!S_ISDIR(st.st_mode)) {
			errno = ENOTDIR;
			return (-1);
		}
		return (0);
	}
	if (mkdir(path, 0700) < 0 && errno != EEXIST)
		return (-1);
	chmod(path, 0700);
	return (0);
}

int
dv_socket_listen(const char *sock_path)
{
	struct sockaddr_un	 sa;
	int			 fd;
	char			*slash;

	slash = const_cast<char *>(strrchr(sock_path, '/'));
	if (slash != nullptr) {
		char	*dir = xstrdup(sock_path);

		dir[slash - sock_path] = '\0';
		if (ensure_dir(dir) < 0) {
			free(dir);
			return (-1);
		}
		free(dir);
	}
	unlink(sock_path);
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return (-1);
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	if (dv_strlcpy(sa.sun_path, sock_path, sizeof sa.sun_path) >=
	    sizeof sa.sun_path) {
		close(fd);
		errno = ENAMETOOLONG;
		return (-1);
	}
	if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		close(fd);
		return (-1);
	}
	if (listen(fd, 16) < 0) {
		close(fd);
		unlink(sock_path);
		return (-1);
	}
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	chmod(sock_path, 0600);
	return (fd);
}

int
dv_socket_connect(const char *sock_path)
{
	struct sockaddr_un	 sa;
	int			 fd;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return (-1);
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	if (dv_strlcpy(sa.sun_path, sock_path, sizeof sa.sun_path) >=
	    sizeof sa.sun_path) {
		close(fd);
		errno = ENAMETOOLONG;
		return (-1);
	}
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		close(fd);
		return (-1);
	}
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	return (fd);
}

int
dv_socket_start_server(const char *sock_path, int debug, char **argv0)
{
	pid_t	 pid;
	int	 tries;
	int	 nfd;

	(void)argv0;
	pid = fork();
	if (pid < 0)
		return (-1);
	if (pid == 0) {
		if (!debug) {
			if (fork() > 0)
				_exit(0);
			setsid();
			if (chdir("/") < 0)
				_exit(1);
			nfd = open("/dev/null", O_RDWR);
			if (nfd >= 0) {
				dup2(nfd, 0);
				dup2(nfd, 1);
				dup2(nfd, 2);
				if (nfd > 2)
					close(nfd);
			}
		}
		dv_server_main(sock_path, debug);
		_exit(0);
	}
	if (!debug)
		waitpid(pid, NULL, 0);

	for (tries = 0; tries < 50; tries++) {
		int	fd = dv_socket_connect(sock_path);

		if (fd >= 0)
			return (fd);
		usleep(20000);
	}
	errno = ECONNREFUSED;
	return (-1);
}
