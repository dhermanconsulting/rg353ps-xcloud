#include "wifi_tune.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---- generic netlink, by hand ------------------------------------------ */

/*
 * Just enough of libnl to send one nl80211 command: resolve the family id
 * through the nlctrl family, send a request with two attributes, and read
 * the ACK. Everything is a fixed-size buffer; no allocation.
 */

/*
 * Big enough for the nlctrl GETFAMILY reply for nl80211, which lists every
 * one of the family's ~120 operations and its multicast groups: about 3 KB
 * on the 4.19 kernel. A 512-byte reply buffer here silently dropped that
 * reply and the lookup reported "not found" (seen on the device 2026-09-05
 * as "nl80211 power save refused: No such file or directory").
 */
#define NLBUF_SIZE 8192

struct nlbuf {
	unsigned char data[NLBUF_SIZE];
	int len;
};

static void nl_put_attr(struct nlbuf *b, int type, const void *v, int vlen)
{
	struct nlattr *a = (struct nlattr *)(b->data + b->len);
	int alen = NLA_HDRLEN + vlen;

	a->nla_type = (unsigned short)type;
	a->nla_len = (unsigned short)alen;
	memcpy((unsigned char *)a + NLA_HDRLEN, v, vlen);
	b->len += NLA_ALIGN(alen);
}

static void nl_begin(struct nlbuf *b, int family, int cmd, unsigned seq)
{
	struct nlmsghdr *h = (struct nlmsghdr *)b->data;
	struct genlmsghdr *g;

	memset(b, 0, sizeof(*b));
	h->nlmsg_type = (unsigned short)family;
	h->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	h->nlmsg_seq = seq;
	h->nlmsg_pid = 0;
	g = (struct genlmsghdr *)(b->data + NLMSG_HDRLEN);
	g->cmd = (unsigned char)cmd;
	g->version = 1;
	b->len = NLMSG_HDRLEN + NLMSG_ALIGN(GENL_HDRLEN);
}

/* Send and read replies until the ACK/error for our sequence. Returns the
 * kernel's error (0 ok, negative errno) and leaves the first non-ACK reply
 * in `reply` if given. */
static int nl_transact(int fd, struct nlbuf *b, unsigned seq,
		       struct nlbuf *reply)
{
	struct nlmsghdr *h = (struct nlmsghdr *)b->data;
	struct sockaddr_nl kernel;
	unsigned char rbuf[NLBUF_SIZE];

	h->nlmsg_len = (unsigned)b->len;
	memset(&kernel, 0, sizeof(kernel));
	kernel.nl_family = AF_NETLINK;
	if (sendto(fd, b->data, (size_t)b->len, 0, (struct sockaddr *)&kernel,
		   sizeof(kernel)) < 0)
		return -errno;

	for (int guard = 0; guard < 8; guard++) {
		ssize_t n = recv(fd, rbuf, sizeof(rbuf), 0);
		if (n < 0)
			return -errno;
		for (struct nlmsghdr *r = (struct nlmsghdr *)rbuf;
		     NLMSG_OK(r, (unsigned)n); r = NLMSG_NEXT(r, n)) {
			if (r->nlmsg_seq != seq)
				continue;
			if (r->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *e = (struct nlmsgerr *)NLMSG_DATA(r);
				return e->error;  /* 0 is the ACK */
			}
			if (r->nlmsg_type == NLMSG_DONE)
				return 0;
			if (reply && !reply->len && r->nlmsg_len <= sizeof(reply->data)) {
				memcpy(reply->data, r, r->nlmsg_len);
				reply->len = (int)r->nlmsg_len;
			}
		}
	}
	return -ETIMEDOUT;
}

/* nlctrl: CTRL_CMD_GETFAMILY "nl80211" -> family id, or negative. */
static int nl80211_family(int fd, unsigned *seq)
{
	struct nlbuf req, rep;
	int rc;

	nl_begin(&req, GENL_ID_CTRL, CTRL_CMD_GETFAMILY, ++*seq);
	nl_put_attr(&req, CTRL_ATTR_FAMILY_NAME, "nl80211", 8);
	memset(&rep, 0, sizeof(rep));
	rc = nl_transact(fd, &req, *seq, &rep);
	if (rc < 0)
		return rc;
	if (!rep.len)
		return -ENOMSG;  /* ACKed but no family description captured */
	{
		struct nlmsghdr *h = (struct nlmsghdr *)rep.data;
		unsigned char *p = rep.data + NLMSG_HDRLEN + NLMSG_ALIGN(GENL_HDRLEN);
		unsigned char *end = rep.data + h->nlmsg_len;
		while (p + NLA_HDRLEN <= end) {
			struct nlattr *a = (struct nlattr *)p;
			if (a->nla_len < NLA_HDRLEN || p + a->nla_len > end)
				break;
			if ((a->nla_type & NLA_TYPE_MASK) == CTRL_ATTR_FAMILY_ID)
				return *(unsigned short *)(p + NLA_HDRLEN);
			p += NLA_ALIGN(a->nla_len);
		}
	}
	return -ENOENT;
}

/* NL80211_CMD_SET_POWER_SAVE on the interface. */
static int nl80211_set_ps(const char *ifname, int enabled)
{
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
	struct sockaddr_nl local;
	unsigned seq = (unsigned)getpid();
	int family, rc;
	unsigned ifindex = if_nametoindex(ifname);
	unsigned state = enabled ? NL80211_PS_ENABLED : NL80211_PS_DISABLED;
	struct nlbuf req;

	if (fd < 0)
		return -errno;
	if (!ifindex) {
		close(fd);
		return -ENODEV;
	}
	memset(&local, 0, sizeof(local));
	local.nl_family = AF_NETLINK;
	if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
		rc = -errno;
		close(fd);
		return rc;
	}
	family = nl80211_family(fd, &seq);
	if (family < 0) {
		close(fd);
		return family;
	}
	nl_begin(&req, family, NL80211_CMD_SET_POWER_SAVE, ++seq);
	nl_put_attr(&req, NL80211_ATTR_IFINDEX, &ifindex, sizeof(ifindex));
	nl_put_attr(&req, NL80211_ATTR_PS_STATE, &state, sizeof(state));
	rc = nl_transact(fd, &req, seq, NULL);
	close(fd);
	return rc;
}

/* ---- driver scan_deny -------------------------------------------------- */

/*
 * The Realtek driver exposes a boolean in procfs that makes it refuse every
 * scan request (cfg80211_rtw_scan logs "scan deny" and returns busy). Raised
 * for the stream, it stops the supplicant's background scan at the one point
 * that matters, whatever the supplicant is configured to do and however it
 * is controlled. The fd stays open so the fatal-signal handler can lower it
 * again with a single write().
 */
static int g_scan_deny_fd = -1;

static int scan_deny_set(const char *ifname, int deny)
{
	char path[96];

	if (g_scan_deny_fd < 0) {
		snprintf(path, sizeof(path), "/proc/net/rtl8821cs/%s/scan_deny",
			 ifname);
		g_scan_deny_fd = open(path, O_WRONLY | O_CLOEXEC);
		if (g_scan_deny_fd < 0)
			return -errno;
	}
	if (write(g_scan_deny_fd, deny ? "1\n" : "0\n", 2) != 2)
		return -errno;
	return 0;
}

void wifi_tune_panic(void)
{
	if (g_scan_deny_fd >= 0) {
		ssize_t ignored = write(g_scan_deny_fd, "0\n", 2);
		(void)ignored;
	}
}

/* ---- wpa_supplicant --------------------------------------------------- */

/* Run wpa_cli for the interface and return its first output line. */
static int wpa(const char *ifname, const char *args, char *out, size_t n)
{
	char cmd[256];
	FILE *fp;

	snprintf(cmd, sizeof(cmd), "wpa_cli -i %s %s 2>&1", ifname, args);
	fp = popen(cmd, "r");
	if (!fp)
		return -1;
	if (!fgets(out, (int)n, fp))
		out[0] = 0;
	pclose(fp);
	out[strcspn(out, "\r\n")] = 0;
	return 0;
}

/* The id of the network wpa_supplicant is currently using, from `status`
 * ("id=N" line). connman numbers networks as it adds them, so 0 is usual
 * but not guaranteed; the bgscan reset only applies to the current one. */
static int wpa_current_id(const char *ifname)
{
	char cmd[128];
	char line[160];
	FILE *fp;
	int id = -1;

	snprintf(cmd, sizeof(cmd), "wpa_cli -i %s status 2>&1", ifname);
	fp = popen(cmd, "r");
	if (!fp)
		return -1;
	while (fgets(line, sizeof(line), fp)) {
		if (!strncmp(line, "id=", 3)) {
			id = atoi(line + 3);
			break;
		}
		if (strstr(line, "Failed to connect") || strstr(line, "Could not"))
			break;
	}
	pclose(fp);
	return id;
}

int wifi_tune_apply(struct wifi_tune *w, const char *ifname)
{
	int applied = 0;
	int rc;
	char line[160];

	memset(w, 0, sizeof(*w));
	w->net_id = -1;
	snprintf(w->ifname, sizeof(w->ifname), "%s", ifname ? ifname : "wlan0");

	rc = nl80211_set_ps(w->ifname, 0);
	if (rc == 0) {
		w->ps_disabled = 1;
		applied++;
		fprintf(stderr, "wifi: power save off on %s (nl80211)\n",
			w->ifname);
	} else {
		fprintf(stderr, "wifi: nl80211 power save refused on %s: %s\n",
			w->ifname, strerror(-rc));
	}

	rc = scan_deny_set(w->ifname, 1);
	if (rc == 0) {
		w->scan_denied = 1;
		applied++;
		fprintf(stderr, "wifi: scans denied at the driver for the stream "
				"(/proc/net/rtl8821cs/%s/scan_deny)\n", w->ifname);
		return applied;
	}
	fprintf(stderr, "wifi: driver scan_deny unavailable (%s); trying "
			"wpa_cli\n", strerror(-rc));

	w->net_id = wpa_current_id(w->ifname);
	if (w->net_id < 0) {
		fprintf(stderr, "wifi: wpa_cli unavailable or not associated; "
				"background scan left alone\n");
		return applied;
	}
	{
		char args[96];
		snprintf(args, sizeof(args), "get_network %d bgscan", w->net_id);
		/* The value comes back quoted; anything else is an error
		 * message (FAIL, or "Failed to connect to ..."). */
		if (wpa(w->ifname, args, line, sizeof(line)) == 0 && line[0] == '"') {
			snprintf(w->bgscan_saved, sizeof(w->bgscan_saved), "%s", line);
			snprintf(args, sizeof(args), "set_network %d bgscan '\"\"'",
				 w->net_id);
			if (wpa(w->ifname, args, line, sizeof(line)) == 0 &&
			    strstr(line, "OK")) {
				w->bgscan_cleared = 1;
				applied++;
				fprintf(stderr, "wifi: background scan off for "
						"network %d (was %s)\n",
					w->net_id, w->bgscan_saved);
			} else {
				fprintf(stderr, "wifi: could not clear bgscan: %s\n",
					line);
			}
		} else {
			fprintf(stderr, "wifi: no bgscan set on network %d (%s)\n",
				w->net_id, line[0] ? line : "no output");
		}
	}
	return applied;
}

void wifi_tune_restore(struct wifi_tune *w)
{
	char line[160];

	if (w->scan_denied) {
		if (scan_deny_set(w->ifname, 0) == 0)
			fprintf(stderr, "wifi: scans allowed again\n");
		else
			fprintf(stderr, "wifi: could not lower scan_deny: %s\n",
				strerror(errno));
		w->scan_denied = 0;
	}
	if (w->bgscan_cleared) {
		char args[224];
		/* The saved value is quoted the way get_network returned it. */
		snprintf(args, sizeof(args), "set_network %d bgscan '%s'",
			 w->net_id, w->bgscan_saved);
		wpa(w->ifname, args, line, sizeof(line));
		w->bgscan_cleared = 0;
	}
	if (w->ps_disabled) {
		nl80211_set_ps(w->ifname, 1);
		w->ps_disabled = 0;
	}
}
