/*
 * Screens that are not the library: the one-line message and sign-in.
 */
#include "app.hpp"

namespace app {

void show_message(drm_out &out, Fonts &f, const char *title, const char *l1,
		  const char *l2)
{
	Painter p(out, f);
	p.header("Xbox Cloud Gaming");
	p.centred(f.mid, 200, title, 235);
	if (l1)
		p.centred(f.small, 245, l1, 170);
	if (l2)
		p.centred(f.small, 275, l2, 170);
	p.present();
}

bool sign_in(drm_out &out, Fonts &f, pad &p, gnx::XboxAuth &auth)
{
	gnx::DeviceCode code;

	show_message(out, f, "Connecting to Microsoft...", nullptr);
	try {
		code = auth.request_device_code();
	} catch (const std::exception &e) {
		show_message(out, f, "Could not reach Microsoft", e.what(),
			     "Check wifi, then press B");
		while (!g_stop && !pad_take_press(&p, PAD_B))
			pad_poll(&p, 200);
		return false;
	}

	std::fprintf(stderr, "xcloud: device code %s at %s (valid %ds)\n",
		     code.user_code.c_str(), code.verification_uri.c_str(),
		     code.expires_in_secs);

	/* Strip the scheme so the URL fits and reads cleanly on a small panel. */
	std::string uri = code.verification_uri;
	for (const char *pfx : { "https://", "http://" })
		if (uri.rfind(pfx, 0) == 0)
			uri = uri.substr(std::strlen(pfx));

	auto draw = [&](const char *status) {
		Painter pt(out, f);
		pt.header("Sign in");
		pt.centred(f.small, 105, "On a phone or PC, go to", 175);
		pt.centred(f.mid, 140, uri.c_str(), 235);
		pt.centred(f.small, 190, "and enter this code", 175);
		pt.rect(90, 212, kWidth - 180, 66, 45);
		pt.centred(f.mono, 262, code.user_code.c_str(), 235);
		pt.centred(f.small, 330, status, 150);
		pt.footer("B to cancel");
		pt.present();
	};
	draw("Waiting for you to sign in...");

	const int interval_ms = code.interval_secs > 0 ? code.interval_secs * 1000
						      : 5000;
	int since_poll = interval_ms;   /* poll immediately */
	int elapsed = 0;

	while (!g_stop && elapsed < code.expires_in_secs * 1000) {
		pad_poll(&p, 100);
		if (pad_take_press(&p, PAD_B))
			return false;
		elapsed += 100;
		since_poll += 100;
		if (since_poll < interval_ms)
			continue;
		since_poll = 0;

		gnx::PollResult r;
		try {
			r = auth.poll_device_code(code);
		} catch (const std::exception &e) {
			draw(e.what());
			continue;
		}
		if (r == gnx::PollResult::Authorized)
			return true;
		if (r == gnx::PollResult::Expired) {
			show_message(out, f, "That code expired",
				     "Press A to get a new one, B to quit");
			while (!g_stop) {
				pad_poll(&p, 200);
				if (pad_take_press(&p, PAD_A))
					return sign_in(out, f, p, auth);
				if (pad_take_press(&p, PAD_B))
					return false;
			}
			return false;
		}
	}
	return false;
}

}  // namespace app
