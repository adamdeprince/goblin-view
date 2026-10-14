/*
 * The configuration file: ~/.config/goblin-view/config.
 *
 *     # a comment
 *     shell = /bin/zsh
 *     scrollback = 5000
 *     [host "kindle"]
 *     mono = yes
 *
 * `key = value` lines, and [host "name"] sections that apply only when
 * the terminal the client is attached from names itself that way in its
 * XTVERSION reply (a case-insensitive substring: "kitty" matches
 * "kitty(0.48.2)"). Booleans take yes/no, true/false, on/off, 1/0.
 * Values may be quoted. Precedence, highest first: a command-line
 * option, an environment variable, this file, the built-in default.
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dv.hpp"

void
dv_config_defaults(struct dv_config *c)
{
	memset(c, 0, sizeof *c);
	c->scrollback = 1000;
	c->backdrop = 0x2591;		/* light shade */
	c->click_raises = 1;
	c->menu_key = DV_MENU_KEY_ALT | DV_MENU_KEY_CTRL_BACKSLASH;
	c->mono = 0;
}

const char *
dv_config_path(void)
{
	static char	path[DV_PATH_MAX];
	const char	*e = getenv("GOBLIN_VIEW_CONFIG"), *xdg, *home;

	if (e != NULL && e[0] != '\0')
		return (e);
	xdg = getenv("XDG_CONFIG_HOME");
	home = getenv("HOME");
	if (xdg != NULL && xdg[0] == '/')
		snprintf(path, sizeof path, "%s/goblin-view/config", xdg);
	else if (home != NULL && home[0] != '\0')
		snprintf(path, sizeof path, "%s/.config/goblin-view/config", home);
	else
		return ("/etc/goblin-view/config");
	return (path);
}

static char *
trim(char *s)
{
	char	*e;

	while (*s != '\0' && isspace((unsigned char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = '\0';
	return (s);
}

/* Strip a quoted value's quotes, in place. */
static char *
unquote(char *v)
{
	size_t	n = strlen(v);

	if (n >= 2 && ((v[0] == '"' && v[n - 1] == '"') ||
	    (v[0] == '\'' && v[n - 1] == '\''))) {
		v[n - 1] = '\0';
		v++;
	}
	return (v);
}

static int
parse_bool(const char *v, int *out)
{
	if (strcasecmp(v, "yes") == 0 || strcasecmp(v, "true") == 0 ||
	    strcasecmp(v, "on") == 0 || strcmp(v, "1") == 0) {
		*out = 1;
		return (1);
	}
	if (strcasecmp(v, "no") == 0 || strcasecmp(v, "false") == 0 ||
	    strcasecmp(v, "off") == 0 || strcmp(v, "0") == 0) {
		*out = 0;
		return (1);
	}
	return (0);
}

/* The first UTF-8 character of v as a code point; 0 if none. */
static uint32_t
first_glyph(const char *v)
{
	const unsigned char	*u = (const unsigned char *)v;

	if (u[0] < 0x80)
		return (u[0]);
	if ((u[0] & 0xe0) == 0xc0 && u[1])
		return (((uint32_t)(u[0] & 0x1f) << 6) | (u[1] & 0x3f));
	if ((u[0] & 0xf0) == 0xe0 && u[1] && u[2])
		return (((uint32_t)(u[0] & 0x0f) << 12) | ((uint32_t)(u[1] & 0x3f) << 6) |
		    (u[2] & 0x3f));
	if ((u[0] & 0xf8) == 0xf0 && u[1] && u[2] && u[3])
		return (((uint32_t)(u[0] & 0x07) << 18) | ((uint32_t)(u[1] & 0x3f) << 12) |
		    ((uint32_t)(u[2] & 0x3f) << 6) | (u[3] & 0x3f));
	return (0);
}

/* Apply one key; -1 for an unknown key, -2 for a bad value. */
static int
apply(struct dv_config *c, const char *key, char *value)
{
	int	b;

	if (strcmp(key, "shell") == 0)
		dv_strlcpy(c->shell, value, sizeof c->shell);
	else if (strcmp(key, "scrollback") == 0) {
		char	*end;
		long	n = strtol(value, &end, 10);

		if (*value == '\0' || *end != '\0' || n < 0 || n > 1000000)
			return (-2);
		c->scrollback = (int)n;
	} else if (strcmp(key, "download-dir") == 0)
		dv_strlcpy(c->download_dir, value, sizeof c->download_dir);
	else if (strcmp(key, "kitty-medium") == 0) {
		if (strcmp(value, "shm") != 0 && strcmp(value, "tempfile") != 0 &&
		    strcmp(value, "file") != 0 && strcmp(value, "direct") != 0)
			return (-2);
		dv_strlcpy(c->kitty_medium, value, sizeof c->kitty_medium);
	} else if (strcmp(key, "term") == 0)
		dv_strlcpy(c->term, value, sizeof c->term);
	else if (strcmp(key, "backdrop") == 0) {
		uint32_t	g = first_glyph(value);

		if (g == 0)
			return (-2);
		c->backdrop = g;
	} else if (strcmp(key, "click-raises") == 0) {
		if (!parse_bool(value, &b))
			return (-2);
		c->click_raises = b;
	} else if (strcmp(key, "mono") == 0) {
		if (!parse_bool(value, &b))
			return (-2);
		c->mono = b;
	} else if (strcmp(key, "csi-limit") == 0 || strcmp(key, "escape-limit") == 0) {
		char			*end;
		unsigned long long	 n;

		errno = 0;
		n = strtoull(value, &end, 10);
		if (*value == '\0' || *end != '\0' || errno != 0)
			return (-2);
		if (key[0] == 'c')
			c->csi_limit = (size_t)n;
		else
			c->escape_limit = (size_t)n;
	} else if (strcmp(key, "menu-key") == 0) {
		/* a comma list: alt, ctrl-backslash, none */
		char	*tok, *save, buf[64];
		int	mask = 0;

		dv_strlcpy(buf, value, sizeof buf);
		for (tok = strtok_r(buf, ", ", &save); tok != NULL;
		    tok = strtok_r(NULL, ", ", &save)) {
			if (strcasecmp(tok, "alt") == 0)
				mask |= DV_MENU_KEY_ALT;
			else if (strcasecmp(tok, "ctrl-backslash") == 0 ||
			    strcasecmp(tok, "ctrl-\\") == 0)
				mask |= DV_MENU_KEY_CTRL_BACKSLASH;
			else if (strcasecmp(tok, "none") == 0)
				mask |= 0;
			else
				return (-2);
		}
		c->menu_key = mask;
	} else
		return (-1);
	return (0);
}

static size_t
env_size(const char *name, size_t fallback)
{
	const char		*v = getenv(name);
	char			*end;
	unsigned long long	 n;

	if (v == NULL || v[0] == '\0')
		return (fallback);
	errno = 0;
	n = strtoull(v, &end, 10);
	if (*end != '\0' || errno != 0)
		return (fallback);
	return ((size_t)n);
}

/* File values, then GOBLIN_VIEW_CSI_LIMIT and GOBLIN_VIEW_ESCAPE_LIMIT. */
void
dv_limits_apply(const struct dv_config *c)
{
	dv_csi_limit = env_size("GOBLIN_VIEW_CSI_LIMIT", c->csi_limit);
	dv_escape_limit = env_size("GOBLIN_VIEW_ESCAPE_LIMIT", c->escape_limit);
}

/*
 * Load the file at path into c. Lines in a [host "name"] section apply
 * only when host names that terminal (host may be NULL: no section
 * applies). Returns the number of problems reported through warn (which
 * may be NULL), or -1 when the file does not exist.
 */
int
dv_config_load(struct dv_config *c, const char *path, const char *host,
    void (*warn)(const char *, int, const char *))
{
	FILE	*f = fopen(path, "r");
	char	 line[2048];
	int	 lineno = 0, problems = 0, active = 1;

	if (f == NULL)
		return (-1);
	while (fgets(line, sizeof line, f) != NULL) {
		char	*s, *hash, *eq, *key, *value;
		int	 rc;

		lineno++;
		s = line;
		/* a comment: # at the start or after whitespace */
		for (hash = s; (hash = strchr(hash, '#')) != NULL; hash++)
			if (hash == s || isspace((unsigned char)hash[-1])) {
				*hash = '\0';
				break;
			}
		s = trim(s);
		if (*s == '\0')
			continue;
		if (*s == '[') {
			char	*e = strchr(s, ']'), *nm;

			if (e == NULL) {
				if (warn)
					warn(path, lineno, "unterminated section");
				problems++;
				active = 0;
				continue;
			}
			*e = '\0';
			nm = trim(s + 1);
			if (strncmp(nm, "host", 4) == 0 && (nm[4] == ' ' || nm[4] == '\t')) {
				nm = unquote(trim(nm + 4));
				active = host != NULL && dv_term_is(host, nm);
			} else {
				if (warn)
					warn(path, lineno, "unknown section");
				problems++;
				active = 0;
			}
			continue;
		}
		eq = strchr(s, '=');
		if (eq == NULL) {
			if (warn)
				warn(path, lineno, "expected key = value");
			problems++;
			continue;
		}
		*eq = '\0';
		key = trim(s);
		value = unquote(trim(eq + 1));
		if (!active)
			continue;
		rc = apply(c, key, value);
		if (rc == -1) {
			if (warn)
				warn(path, lineno, "unknown key");
			problems++;
		} else if (rc == -2) {
			if (warn)
				warn(path, lineno, "bad value");
			problems++;
		}
	}
	fclose(f);
	return (problems);
}
