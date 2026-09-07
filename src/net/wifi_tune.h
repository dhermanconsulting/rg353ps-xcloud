/*
 * Wi-Fi tuning for the duration of a stream.
 *
 * Measured on the RG353 (rtl8821cs, connman + wpa_supplicant): the driver
 * runs in maximum leisure power save (ps_info: "LPS mode: MAX", entered
 * 222 times in one session; the driver leaves LPS only above 12 Mbps of
 * traffic, so a stream hovering there toggles it) and connman configures a
 * background scan every 30 s while the signal is below -65 dBm, which at
 * -63 to -67 dBm it is about half the time. A single-radio
 * SDIO chip that is dozing between beacons or off scanning other channels
 * cannot receive a 1000-packet-per-second video stream; a five-minute race
 * logged 64 retransmit requests, 25 lost frames and gaps up to 2 s.
 *
 * Two best-effort knobs, each logged, each undone on the way out:
 *   - nl80211 NL80211_CMD_SET_POWER_SAVE = disabled, sent over a raw
 *     generic-netlink socket (no libnl on the device). In the Realtek rtw
 *     driver this is cfg80211_rtw_set_power_mgmt(): it leaves LPS at once
 *     and PS_RDY_CHECK() refuses to re-enter while the flag is clear. The
 *     wireless-extensions SIOCSIWPOWER path is a stub in that driver
 *     (returns EPERM), which is why it is not used.
 *   - the driver's scan_deny switch (/proc/net/rtl8821cs/<if>/scan_deny),
 *     which makes cfg80211_rtw_scan refuse every scan request, so the
 *     supplicant's background scan (connman sets "simple:30:-65:300")
 *     cannot take the radio off-channel while it is raised.
 *     wpa_supplicant runs D-Bus-only on this firmware (`-u`, no control
 *     socket), so the wpa_cli route that clears bgscan directly can never
 *     work here; it is kept only as a fallback for an OS build that has
 *     a control socket.
 * Neither needs a persistent change to the read-only root. If either is
 * refused the stream still runs; the log says which. Verify on the device
 * with /proc/net/rtl8821cs/wlan0/ps_info ("LPS mode") and dmesg ("scan
 * deny"). A crash lowers scan_deny again from the fatal-signal handler; a
 * SIGKILL does not, and `echo 0 > .../scan_deny` (or a reboot) puts it back.
 */
#ifndef XCLOUD_WIFI_TUNE_H
#define XCLOUD_WIFI_TUNE_H

#ifdef __cplusplus
extern "C" {
#endif

struct wifi_tune {
	char ifname[16];
	int ps_disabled;        /* we switched power save off via nl80211 */
	int scan_denied;        /* driver scan_deny raised (proc knob) */
	int net_id;             /* wpa_supplicant network id we changed */
	/* Previous bgscan value, quoted, to restore on exit. Sized to match the
	 * wpa_cli reply buffer it is copied from: anything smaller truncates,
	 * and a truncated value is not a cosmetic problem -- it is written back
	 * verbatim on restore, so it would leave the user's network configured
	 * with half a bgscan expression. */
	char bgscan_saved[160];
	int bgscan_cleared;
};

/* Apply both knobs to `ifname` (NULL = "wlan0"). Returns the number applied. */
int wifi_tune_apply(struct wifi_tune *w, const char *ifname);
/* Put back what was changed. */
void wifi_tune_restore(struct wifi_tune *w);
/* Async-signal-safe: lower the driver's scan_deny if this process raised
 * it. For the fatal-signal handler, so a crash mid-stream does not leave
 * the device unable to scan (and so to roam or reconnect) until reboot. */
void wifi_tune_panic(void);

#ifdef __cplusplus
}
#endif

#endif /* XCLOUD_WIFI_TUNE_H */
