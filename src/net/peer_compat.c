/*
 * The one symbol libpeer expects the application to provide.
 *
 * The deps are built with -DLOG_REDIRECT=1 (see deps/build-deps.sh), which
 * makes every LOGI/LOGW/LOGE inside libpeer call peer_log() instead of
 * writing to stdout. Routing it into the engine's stream log is what makes
 * ICE and DTLS failures debuggable at all -- on the handheld there is no
 * console to print to.
 *
 * Ported from green-nx's switch_compat.c. Its other three jobs (ifaddrs,
 * if_nametoindex, mbedtls_hardware_poll) exist only because devkitA64's newlib
 * lacks them; glibc has all three.
 */
#include <stdarg.h>
#include <stdio.h>

static void (*g_peer_log_cb)(const char *line);

void gnx_peer_log_set(void (*cb)(const char *line)) { g_peer_log_cb = cb; }

void peer_log(char *level_tag, const char *file, int line, const char *fmt, ...)
{
	char msg[384];
	char full[512];
	va_list ap;

	if (!g_peer_log_cb)
		return;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	snprintf(full, sizeof(full), "%s %s:%d %s", level_tag ? level_tag : "?",
		 file ? file : "?", line, msg);
	g_peer_log_cb(full);
}
