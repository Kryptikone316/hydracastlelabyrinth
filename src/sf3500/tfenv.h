/* Device settings from TreeFrogUI.
 *
 * TreeFrogUI writes /tmp/tfdevice.env at boot describing the device it runs on
 * (TF_DEVICE, TF_PANEL_W/H, TF_ROTATE, TF_PRESENT, and TF_DRIVER, the driver
 * that matches the hardware, e.g. /mnt/sdcard/cubegm/driver_sf3000.so). Look
 * keys up there so the same binary works on every supported device. A variable
 * of the same name in the environment wins. Header-only. */
#ifndef TFENV_H
#define TFENV_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TFENV_FILE     "/tmp/tfdevice.env"
#define TFENV_DEFAULT_DRIVER "/mnt/sdcard/cubegm/driver_sf3500.so"

/* Value of KEY from the environment or tfdevice.env, or NULL if not set.
 * The result is copied into OUT (OUTSZ bytes). */
static const char *tfenv_get(const char *key, char *out, size_t outsz)
{
	const char *e = getenv(key);
	size_t n = strlen(key);
	char line[320];
	FILE *f;

	if (e && *e) {
		snprintf(out, outsz, "%s", e);
		return out;
	}
	f = fopen(TFENV_FILE, "r");
	if (!f)
		return NULL;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, key, n) != 0 || line[n] != '=')
			continue;
		line[strcspn(line, "\r\n")] = '\0';
		fclose(f);
		if (!line[n + 1])
			return NULL;
		snprintf(out, outsz, "%s", line + n + 1);
		return out;
	}
	fclose(f);
	return NULL;
}

/* Driver to dlopen: TF_DRIVER, else the SF3500 driver this port was developed on. */
static const char *tfdriver_path(void)
{
	static char path[256];
	static int resolved = 0;

	if (!resolved) {
		resolved = 1;
		if (!tfenv_get("TF_DRIVER", path, sizeof(path)))
			snprintf(path, sizeof(path), "%s", TFENV_DEFAULT_DRIVER);
	}
	return path;
}

#endif
