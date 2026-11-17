/* The configuration file: keys, sections matched to the terminal, and
 * what is complained about. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dv.hpp"

static int	failed;
static int	warnings;
static char	last_warning[128];

static void
fail(const char *n)
{
	fprintf(stderr, "FAIL %s\n", n);
	failed = 1;
}

static void
warn(const char *path, int line, const char *msg)
{
	(void)path;
	warnings++;
	snprintf(last_warning, sizeof last_warning, "%d: %s", line, msg);
}

int
main(void)
{
	struct dv_config	c;
	char			path[] = "/tmp/gvcfgXXXXXX";
	int			fd = mkstemp(path);
	FILE			*f;

	if (fd < 0) {
		fail("mkstemp");
		return (1);
	}
	f = fdopen(fd, "w");
	fputs("# odin-seat config\n"
	    "shell = /bin/zsh   # trailing comment\n"
	    "scrollback = 5000\n"
	    "backdrop = \"·\"\n"
	    "click-raises = no\n"
	    "kitty-medium = direct\n"
	    "download-dir = '/tmp/dl'\n"
	    "menu-key = ctrl-backslash\n"
	    "csi-limit = 1024\n"
	    "escape-limit = 0\n"
	    "\n"
	    "[host \"kindle\"]\n"
	    "mono = yes\n"
	    "menu-key = none\n"
	    "[host kitty]\n"
	    "mono = off\n"
	    "scrollback = 20\n"
	    "[other]\n"
	    "mono = yes\n", f);
	fclose(f);

	dv_config_defaults(&c);
	if (c.scrollback != 1000 || c.backdrop != 0x2591 || !c.click_raises ||
	    c.menu_key != (DV_MENU_KEY_ALT | DV_MENU_KEY_CTRL_BACKSLASH) || c.mono)
		fail("defaults");

	/* No host: only the top-level keys apply. */
	dv_config_defaults(&c);
	warnings = 0;
	if (dv_config_load(&c, path, NULL, warn) != 1 || warnings != 1 ||
	    strstr(last_warning, "unknown section") == NULL)
		fail("an unknown section is reported once");
	if (strcmp(c.shell, "/bin/zsh") != 0)
		fail("shell, with a trailing comment stripped");
	if (c.scrollback != 5000)
		fail("scrollback");
	if (c.backdrop != 0xb7)
		fail("backdrop takes a quoted UTF-8 glyph");
	if (c.click_raises)
		fail("click-raises = no");
	if (strcmp(c.kitty_medium, "direct") != 0 || strcmp(c.download_dir, "/tmp/dl") != 0)
		fail("kitty-medium and a single-quoted download-dir");
	if (c.menu_key != DV_MENU_KEY_CTRL_BACKSLASH)
		fail("menu-key = ctrl-backslash");
	if (c.csi_limit != 1024 || c.escape_limit != 0)
		fail("csi-limit and escape-limit");
	setenv("ODIN_SEAT_CSI_LIMIT", "8", 1);
	dv_limits_apply(&c);
	if (dv_csi_limit != 8 || dv_escape_limit != 0)
		fail("ODIN_SEAT_CSI_LIMIT overrides the file");
	unsetenv("ODIN_SEAT_CSI_LIMIT");
	dv_limits_apply(&c);
	if (dv_csi_limit != 1024)
		fail("csi-limit returns when the environment is unset");
	if (c.mono)
		fail("a host section does not apply without a host");

	/* The kindle terminal: its section wins over the top level. */
	dv_config_defaults(&c);
	dv_config_load(&c, path, "Kindle Terminal 0.3", warn);
	if (!c.mono || c.menu_key != 0 || c.scrollback != 5000)
		fail("the [host kindle] section applies to a matching terminal");

	/* kitty: an unquoted section name, later keys override earlier. */
	dv_config_defaults(&c);
	dv_config_load(&c, path, "kitty(0.48.2)", warn);
	if (c.mono || c.scrollback != 20 || c.menu_key != DV_MENU_KEY_CTRL_BACKSLASH)
		fail("the [host kitty] section applies and overrides");

	/* Bad values and unknown keys are reported, and do not stop parsing. */
	f = fopen(path, "w");
	fputs("scrollback = lots\nfoo = 1\nkitty-medium = telepathy\nmenu-key = alt, ctrl-backslash\nshell = /bin/sh\n", f);
	fclose(f);
	dv_config_defaults(&c);
	warnings = 0;
	if (dv_config_load(&c, path, NULL, warn) != 3 || warnings != 3)
		fail("three problems are reported");
	if (c.scrollback != 1000 || strcmp(c.shell, "/bin/sh") != 0 ||
	    c.menu_key != (DV_MENU_KEY_ALT | DV_MENU_KEY_CTRL_BACKSLASH))
		fail("good lines after bad ones still apply");
	if (dv_config_load(&c, "/nonexistent/odin-seat/config", NULL, warn) != -1)
		fail("a missing file is -1, not an error");
	unlink(path);
	if (failed)
		return (1);
	printf("test-config: ok\n");
	return (0);
}
