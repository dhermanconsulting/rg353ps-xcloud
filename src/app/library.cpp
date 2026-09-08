/*
 * The game library screen.
 *
 * A list with the box art on the left and the name beside it, sorted by
 * name, with the last few games played on top and a letter-picker search
 * on Y. Everything slow happens off the main thread:
 *
 *   - names and box-art URLs come from NameResolver (library_data.cpp),
 *     which works through the whole list in the background and caches to
 *     names.json, so the second launch is complete before the first paint;
 *   - box art comes from Thumbs (thumbs.cpp), decoded and scaled on its
 *     own thread and blitted here as NV12;
 *   - when the list came from catalog.json, CatalogRefresh below fetches
 *     the real one and the screen swaps it in when it differs.
 *
 * The main loop drains all three under their mutexes, repaints only when
 * something changed, and measures every repaint: the budget is ~5 ms
 * (the loop is polled at 60 Hz and the pad's auto-repeat needs it to come
 * round), and a paint over 10 ms is logged. Measured in the host simulator
 * with 60 titles and every visible poster decoded: 0.3-0.7 ms.
 */
#include "app.hpp"
#include "library_data.hpp"
#include "options.hpp"
#include "thumbs.hpp"

#include <map>
#include <memory>

namespace app {

namespace {

/* ---- layout, 640x480 ------------------------------------------------- */

constexpr int kHeaderH = 52;
constexpr int kTabTop = 52;
constexpr int kTabH = 28;
constexpr int kListTop = kTabTop + kTabH + 4;
/* The letter strip, shown only on the All games tab: see paint_letters(). It
 * costs one row there (five instead of six) and nothing on the other tabs. */
constexpr int kLetterH = 22;
constexpr int kFooterH = 24;
constexpr int kListBottom = kHeight - kFooterH;
constexpr int kRowH = 62;          /* six rows and a section title fit */
constexpr int kSectionH = 26;
constexpr int kArtX = 14;
constexpr int kArtW = 40, kArtH = 58;   /* a 2:3 poster fits as 38x58 */
constexpr int kTextX = kArtX + kArtW + 12;
constexpr int kSearchTop = 232;    /* the picker panel; two rows stay above */
constexpr size_t kArtBudget = 10u << 20;

/* The ageing metadata refresh (see wait_for_names). A fortnight is well
 * inside how fast a Store rating actually moves, and 40 a launch is two
 * batches -- about 3 MB, invisible next to the stream. */
constexpr int kRefreshAgeDays = 14;
constexpr int kRefreshPerLaunch = 40;

/* The picker: three rows of twelve. */
constexpr const char kGrid[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
constexpr int kGridCols = 12, kGridRows = 3;
constexpr int kQueryMax = 12;

/* ROW_NOTE is an unselectable line of explanatory text, used when a section
 * exists but has nothing in it yet. ROW_GENRE is a genre to step into. */
enum RowKind { ROW_SECTION, ROW_GAME, ROW_CONSOLE, ROW_NOTE, ROW_GENRE,
	       ROW_TOOL };
enum Section { SEC_CONSOLE, SEC_RECENT, SEC_ALL };

/*
 * Top-level tabs.
 *
 * Tabs rather than a sidebar because of the panel: 640x480, and a row already
 * carries box art plus a title plus a metadata line. A sidebar wide enough to
 * read costs ~150 px, a quarter of the width, out of exactly the space the
 * titles need; the tab strip costs 28 px of height and still leaves six rows.
 */
enum Tab { TAB_HOME, TAB_ALL, TAB_GENRES, TAB_SETTINGS, TAB_TOOLS, TAB_COUNT };

const char *const kTabName[TAB_COUNT] = { "Home", "All Games", "Genres",
					  "Settings", "Tools" };

/*
 * The Tools tab: things that measure the device rather than change it, which
 * is why they are not settings. Each is a row that opens a screen of its own.
 */
/*
 * The three that throw something away sit at the BOTTOM, below About, so
 * that scrolling down the list never lands on one by momentum. Each asks
 * first, and each says exactly what it is about to delete.
 */
enum Tool { TOOL_BUTTONS, TOOL_NETWORK, TOOL_ABOUT, TOOL_LOGOUT, TOOL_RESET,
	    TOOL_RESET_LOGOUT, TOOL_COUNT };

const char *const kToolName[TOOL_COUNT] = { "Button tester",
					    "Network test",
					    "About",
					    "Log out",
					    "Reset app",
					    "Reset and log out" };
const char *const kToolHelp[TOOL_COUNT] = {
	"Check what each button on this device really reports",
	"Measure the link to the streaming service",
	"Who wrote this, and which build it is",
	"Forget this Xbox account and pair again with a new code",
	"Clear the library cache and settings, but stay signed in",
	"Everything, as though it had just been installed",
};

/*
 * What lives in the state directory, and which of the two things a wipe can
 * throw away it belongs to. `art` is the thumbnail directory rather than a
 * file; everything else is a plain unlink.
 *
 * Listed here rather than hunted for at run time so that adding a cache
 * somewhere else in the client and forgetting it here is a visible omission
 * in one place, not a file that quietly survives a reset for ever.
 */
const char *const kAuthFiles[]  = { "tokens.json" };
const char *const kCacheFiles[] = { "catalog.json", "names.json",
				    "recent.json", "native.json",
				    "options.json" };

/*
 * Which horizontal band the D-pad is talking to.
 *
 * The screen is three bands stacked: the tabs, a strip belonging to the tab
 * showing (the letters on All games, Standard/Advanced on Settings), and the
 * body. Up and Down move BETWEEN bands; Left and Right move ALONG the one
 * that is lit, and change the tab only from the tab strip.
 *
 * It used to be that Left/Right changed the tab from anywhere in the list,
 * on the reasoning that they were the one D-pad axis the list did not use.
 * That reasoning was about which button was free, not about what the screen
 * felt like to hold: the tabs were a thing you could see and never point at,
 * a nudge of the stick while reading a row threw the whole list away, and
 * Settings -- which needs Left/Right for its values -- had to be left with B
 * because there was no gesture left to leave it with. Making the strips
 * places the cursor can actually stand costs one press to reach them and
 * gives every band the same two axes.
 */
enum Focus { FOCUS_TABS, FOCUS_STRIP, FOCUS_BODY };

struct Row {
	RowKind kind;
	Section section;
	int game;            /* ROW_GAME: index into Model::games.
			      * ROW_CONSOLE: index into Model::consoles.
			      * ROW_GENRE: index into Model::genres. */
	std::string label;   /* ROW_SECTION / ROW_NOTE: its text */
};

int row_height(const Row &r)
{
	return r.kind == ROW_SECTION || r.kind == ROW_NOTE ? kSectionH : kRowH;
}

/* -fake-consoles N: the remote-play rows with no account and no network. */
std::vector<gnx::HomeConsole> fake_consoles(int count)
{
	std::vector<gnx::HomeConsole> out;

	for (int i = 0; i < count; i++) {
		gnx::HomeConsole c;
		char buf[64];

		std::snprintf(buf, sizeof(buf), "FAKESERVERID%08d", i + 1);
		c.server_id = buf;
		/* The first has no name, which is the case a real account
		 * turned up (serverName came back empty on an Xbox One X):
		 * console_label() has to fall back to the model. */
		if (i) {
			std::snprintf(buf, sizeof(buf), "Xbox in room %d", i);
			c.name = buf;
		}
		c.console_type = i % 2 ? "XboxSeriesX" : "XboxOneX";
		c.power_state = i % 3 == 0 ? "ConnectedStandby" : "On";
		out.push_back(std::move(c));
	}
	return out;
}

/*
 * What to call a console on screen. serverName comes back EMPTY on at least
 * one real account (measured 2026-09-06 on an Xbox One X), so falling back to
 * the model is not defensive coding for its own sake -- without it the row
 * would be blank. The type reads as "XboxOneX", which is worth a space.
 */
std::string console_label(const gnx::HomeConsole &c)
{
	if (!c.name.empty())
		return c.name;
	std::string out;
	for (size_t i = 0; i < c.console_type.size(); i++) {
		if (i && std::isupper((unsigned char)c.console_type[i]) &&
		    !std::isupper((unsigned char)c.console_type[i - 1]))
			out += ' ';
		out += c.console_type[i];
	}
	return out.empty() ? "Xbox" : out;
}

/* 2249414 -> "2.2M". A rating count is only ever read as an order of
 * magnitude, and the row has no width to spare. */
std::string human_count(int n)
{
	char buf[24];

	if (n >= 1000000)
		std::snprintf(buf, sizeof(buf), "%.1fM", n / 1000000.0);
	else if (n >= 1000)
		std::snprintf(buf, sizeof(buf), "%.0fk", n / 1000.0);
	else
		std::snprintf(buf, sizeof(buf), "%d", n);
	return buf;
}

/*
 * The list and everything derived from it. `games` is kept sorted by key;
 * `rows` is what the screen walks: section titles and game rows, filtered
 * by the search query when there is one.
 */
struct Model {
	/*
	 * Consoles this account can remote-play. Usually none (the feature is
	 * off by default on the console) and usually one when it is on, which
	 * is why they are rows at the top of the list rather than a separate
	 * screen: a picker for a list of one is a button press wasted, and
	 * remote play streams the dashboard rather than a title, so there is
	 * nothing else to choose afterwards. See docs/REMOTE-PLAY.md.
	 */
	std::vector<gnx::HomeConsole> consoles;
	std::vector<gnx::Game> games;
	std::vector<std::string> keys;
	std::vector<char> letters;
	std::vector<std::string> labels;
	std::vector<std::string> recent;   /* launch ids, newest first */
	std::string query;                 /* typed letters, A-Z 0-9 */
	std::vector<Row> rows;
	int matches = 0;                   /* game rows in the ALL section */
	/* True while the console lookup is still in flight, so the empty
	 * Remote play section can say "looking" rather than "none". */
	bool consoles_pending = true;

	Tab tab = TAB_HOME;
	/* Distinct genres with their counts, rebuilt whenever names arrive
	 * (a genre only exists once the Store lookup has returned). */
	std::vector<std::pair<std::string, int> > genres;
	/* Non-empty while the reader has stepped into one genre from the
	 * Genres tab; B steps back out. kNativeGenre is the pseudo-genre for
	 * titles that encode at our size. */
	std::string genre_filter;
	/* Set by the screen so build_rows can mark and filter these. */
	const std::set<std::string> *native = nullptr;

	/*
	 * The All games tab is grouped by first letter, with '#' collecting
	 * digits and symbols exactly as the search picker does. Only letters
	 * that actually have titles are offered, so the strip never sends you
	 * to an empty list.
	 */
	std::vector<char> letter_tabs;
	char letter = 0;                   /* 0 until index_letters() runs */

	void index_genres();
	void index_letters();

	void sort();
	void build_rows();
	int find_game(const std::string &title_id) const;
};

void Model::sort()
{
	std::vector<int> order(games.size());
	std::vector<std::string> k(games.size());

	for (size_t i = 0; i < games.size(); i++) {
		order[i] = (int)i;
		k[i] = sort_key(games[i]);
	}
	std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
		if (k[a] != k[b])
			return k[a] < k[b];
		return games[a].title_id < games[b].title_id;
	});

	std::vector<gnx::Game> sorted;
	sorted.reserve(games.size());
	keys.clear();
	letters.clear();
	labels.clear();
	for (int i : order) {
		sorted.push_back(std::move(games[i]));
		keys.push_back(std::move(k[i]));
		letters.push_back(letter_of_key(keys.back()));
		labels.push_back(display_name(sorted.back()));
	}
	games.swap(sorted);
}

/*
 * The pseudo-genre for titles that encode at the size we ask for. It is not a
 * Store category, but it belongs in the same list: from the reader's side it
 * is one more way to narrow 585 titles, and on this panel it is the most
 * useful one there is (docs/RESOLUTION.md).
 */
const char kNativeGenre[] = "Runs at native size";

void Model::index_genres()
{
	std::map<std::string, int> counts;

	for (const gnx::Game &g : games)
		if (!g.genre.empty())
			counts[g.genre]++;

	genres.assign(counts.begin(), counts.end());
	/* Most populous first: a genre with four titles is not worth the same
	 * row as one with ninety, and alphabetical would bury the big ones. */
	std::stable_sort(genres.begin(), genres.end(),
			 [](const std::pair<std::string, int> &a,
			    const std::pair<std::string, int> &b) {
				 return a.second > b.second;
			 });

	int native_count = 0;
	if (native)
		for (const gnx::Game &g : games)
			if (native->count(g.title_id))
				native_count++;
	if (native_count)
		genres.insert(genres.begin(), { kNativeGenre, native_count });
}

void Model::index_letters()
{
	std::set<char> present(letters.begin(), letters.end());

	letter_tabs.assign(present.begin(), present.end());
	/* '#' sorts before 'A' in ASCII, which is where it belongs: the
	 * catch-all group reads as "before A", the same as in the search
	 * picker and in the sort itself. */
	if (letter_tabs.empty())
		letter = 0;
	else if (!letter || !present.count(letter))
		letter = letter_tabs.front();
}

/* Does this game belong under `genre`, which may be the pseudo-genre? */
bool in_genre(const gnx::Game &g, const std::string &genre,
	      const std::set<std::string> *native)
{
	if (genre == kNativeGenre)
		return native && native->count(g.title_id);
	return g.genre == genre;
}

int Model::find_game(const std::string &title_id) const
{
	for (size_t i = 0; i < games.size(); i++)
		if (games[i].title_id == title_id)
			return (int)i;
	return -1;
}

void Model::build_rows()
{
	const std::string qkey = gnx::search_key(query);
	char label[96];

	rows.clear();
	matches = 0;

	/*
	 * A search is answered from the whole library whatever tab is showing:
	 * the question "where is Wreckfest" should not depend on which shelf
	 * you happened to be standing at. So a query bypasses the tabs
	 * entirely and falls through to the flat list below.
	 */
	const bool searching = !qkey.empty();

	/* Settings draws its own body (options.cpp owns the page), so the
	 * row model has nothing to say about it. */
	if (!searching && tab == TAB_SETTINGS)
		return;

	/* The Tools tab: one row per tool, each opening a screen of its own.
	 * Rows rather than a settings-style page because a tool is a thing
	 * you run, not a value you set. */
	if (!searching && tab == TAB_TOOLS) {
		rows.push_back({ ROW_SECTION, SEC_ALL, -1, "Tools" });
		for (int i = 0; i < TOOL_COUNT; i++)
			rows.push_back({ ROW_TOOL, SEC_ALL, i, "" });
		return;
	}

	/* The Genres tab: a list of genres to step into, not games. */
	if (!searching && tab == TAB_GENRES && genre_filter.empty()) {
		rows.push_back({ ROW_SECTION, SEC_ALL, -1,
				 genres.empty()
					 ? "Genres"
					 : "Genres  \xC2\xB7  " +
						   std::to_string(genres.size()) });
		for (size_t i = 0; i < genres.size(); i++)
			rows.push_back({ ROW_GENRE, SEC_ALL, (int)i, "" });
		if (genres.empty())
			rows.push_back({ ROW_NOTE, SEC_ALL, -1,
					 "Genres arrive with the names" });
		return;
	}

	/*
	 * Your own Xbox, pinned to the top of Home and shown even when there
	 * is nothing in it yet. An absent section is ambiguous -- it could
	 * mean "no console" or "still looking" or "this build cannot do it" --
	 * and those need very different things from the reader, so the section
	 * is always there and says which. Not on the other tabs: it is not a
	 * game, and it is not an answer to a search.
	 */
	if (!searching && tab == TAB_HOME) {
		rows.push_back({ ROW_SECTION, SEC_CONSOLE, -1, "Remote play" });
		for (size_t i = 0; i < consoles.size(); i++)
			rows.push_back({ ROW_CONSOLE, SEC_CONSOLE, (int)i, "" });
		if (consoles.empty())
			rows.push_back({ ROW_NOTE, SEC_CONSOLE, -1,
					 consoles_pending
						 ? "Looking for your Xbox..."
						 : "No console found" });
	}

	/* Recently played, only when not searching: a search is a question
	 * about the whole library and a second copy of a hit would confuse. */
	if (!searching && tab == TAB_HOME && !recent.empty()) {
		std::vector<int> found;
		for (const std::string &id : recent) {
			int i = find_game(id);
			if (i >= 0)
				found.push_back(i);
		}
		if (!found.empty()) {
			rows.push_back({ ROW_SECTION, SEC_RECENT, -1,
					 "Recently played" });
			for (int i : found)
				rows.push_back({ ROW_GAME, SEC_RECENT, i, "" });
		}
	}

	/*
	 * Home is a landing page, not a catalogue: the console, what you were
	 * last playing, and a way on. Listing 585 titles under it as well
	 * would make the two tabs that do that redundant.
	 */
	if (!searching && tab == TAB_HOME) {
		rows.push_back({ ROW_SECTION, SEC_ALL, -1, "Browse" });
		/* Names the gesture that actually works from here. It used to
		 * say "Left and Right", which stopped being true the day the
		 * tab strip became somewhere the cursor has to stand first. */
		rows.push_back({ ROW_NOTE, SEC_ALL, -1,
				 "L1/R1, or Up to the tabs, for all games "
				 "and genres" });
		return;
	}

	/* Grouped by letter on the flat list, whole on every other path: a
	 * search spans the alphabet and a genre's list is already short. */
	const bool by_letter = !searching && tab == TAB_ALL &&
			       genre_filter.empty() && letter;

	size_t title_at = rows.size();
	rows.push_back({ ROW_SECTION, SEC_ALL, -1, "" });
	for (size_t i = 0; i < games.size(); i++) {
		if (searching && !gnx::matches_query(games[i], qkey))
			continue;
		if (!searching && !genre_filter.empty() &&
		    !in_genre(games[i], genre_filter, native))
			continue;
		if (by_letter && letters[i] != letter)
			continue;
		rows.push_back({ ROW_GAME, SEC_ALL, (int)i, "" });
		matches++;
	}
	if (by_letter)
		std::snprintf(label, sizeof(label), "%s  \xC2\xB7  %d",
			      letter == '#' ? "Symbols and digits"
					    : std::string(1, letter).c_str(),
			      matches);
	else if (!searching && !genre_filter.empty())
		std::snprintf(label, sizeof(label), "%s  \xC2\xB7  %d",
			      genre_filter.c_str(), matches);
	else if (!searching)
		std::snprintf(label, sizeof(label), "All games  \xC2\xB7  %d",
			      matches);
	else if (matches)
		std::snprintf(label, sizeof(label), "%d result%s for %s",
			      matches, matches == 1 ? "" : "s", query.c_str());
	else
		std::snprintf(label, sizeof(label), "Nothing matches %s",
			      query.c_str());
	rows[title_at].label = label;
}

/*
 * Auto-repeat for a stick axis, which has no press edge to work from.
 * `at` is the caller's next-tick timestamp; 0 means the stick is centred.
 */
bool stick_repeat(int16_t axis, int now_ms, int &at)
{
	const int kDeadzone = 12000;  /* well past the driver's flat=32 */

	if (axis > -kDeadzone && axis < kDeadzone) {
		at = 0;
		return false;
	}
	if (!at) {
		at = now_ms + 350;
		return true;
	}
	if (now_ms < at)
		return false;
	at = now_ms + 60;
	return true;
}

/*
 * The background catalog fetch behind a cached list. It uses `auth` (the
 * credential chain) from its thread, so the screen joins it before
 * stream_game touches the same object; in practice it is done in a few
 * seconds, long before anyone has picked a game.
 */
class CatalogRefresh {
public:
	/* want_catalog false still fetches the console list: see run(). */
	CatalogRefresh(gnx::XboxAuth &auth, bool want_catalog)
		: thread_([this, &auth, want_catalog] { run(auth, want_catalog); })
	{
	}
	~CatalogRefresh() { wait(); }

	bool running() const { return running_; }
	void wait()
	{
		if (thread_.joinable())
			thread_.join();
	}
	/* Main thread: the fetched list, once. */
	bool take(std::vector<gnx::Game> &out)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!ready_)
			return false;
		ready_ = false;
		out.swap(result_);
		return true;
	}

	/* Main thread: the console list, once. */
	bool take_consoles(std::vector<gnx::HomeConsole> &out)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!consoles_ready_)
			return false;
		consoles_ready_ = false;
		out.swap(consoles_);
		return true;
	}
	/* False while the console lookup is still in flight, so the screen can
	 * say "looking" rather than "none" -- the two mean very different
	 * things to someone who has just switched remote play on. Set on
	 * failure as well: a lookup that threw is finished, just fruitless. */
	bool consoles_done() const { return consoles_done_; }

private:
	void run(gnx::XboxAuth &auth, bool want_catalog)
	{
		pthread_setname_np(pthread_self(), "xc-catalog");
		gnx::Http http;
		http.set_abort_flag(&g_abort);
		if (want_catalog) {
			try {
				std::vector<gnx::Game> playable;
				fetch_playable(http, auth, playable);
				std::lock_guard<std::mutex> lock(mutex_);
				result_.swap(playable);
				ready_ = true;
			} catch (const std::exception &e) {
				if (!g_abort)
					std::fprintf(stderr,
						     "library: refresh: %s\n",
						     e.what());
			}
		}
		/*
		 * Consoles ride along on this thread whether or not the
		 * catalog is being refreshed, because the expensive part is
		 * the credential chain and fetch_streaming_credentials() has
		 * just run it. Most accounts have none -- remote features are
		 * off by default on the console -- so a failure here is
		 * ordinary and silent beyond the log.
		 */
		try {
			gnx::StreamingCredentials creds =
				auth.fetch_streaming_credentials();
			if (!creds.home.host.empty()) {
				std::vector<gnx::HomeConsole> found =
					gnx::fetch_home_consoles(http, creds.home);
				std::fprintf(stderr, "library: %zu console(s) "
						     "for remote play\n",
					     found.size());
				std::lock_guard<std::mutex> lock(mutex_);
				consoles_.swap(found);
				consoles_ready_ = true;
			}
		} catch (const std::exception &e) {
			if (!g_abort)
				std::fprintf(stderr, "library: consoles: %s\n",
					     e.what());
		}
		consoles_done_ = true;
		running_ = false;
	}

	std::mutex mutex_;
	std::vector<gnx::Game> result_;
	std::vector<gnx::HomeConsole> consoles_;
	bool ready_ = false;
	bool consoles_ready_ = false;
	std::atomic<bool> consoles_done_{false};
	std::atomic<bool> running_{true};
	std::thread thread_;
};

/* What matters for "did the catalog change": ids and how each is playable. */
std::string catalog_signature(std::vector<gnx::Game> v)
{
	std::sort(v.begin(), v.end(), [](const gnx::Game &a, const gnx::Game &b) {
		return a.title_id < b.title_id;
	});
	std::string s;
	for (const gnx::Game &g : v) {
		s += g.title_id;
		s += '/';
		s += g.product_id;
		s += g.owned ? 'o' : '-';
		s += g.in_subscription ? 's' : '-';
		s += g.ad_playable ? 'a' : '-';
		s += ';';
	}
	return s;
}

class LibraryScreen {
public:
	LibraryScreen(drm_out &out, Fonts &f, pad &p, gnx::XboxAuth &auth,
		      std::vector<gnx::Game> &games,
		      const gnx::XboxProfile &profile, bool refresh)
		: out_(out), f_(f), p_(p), auth_(auth),
		  thumbs_(g_opts.state_dir + "/art", kArtW, kArtH, kArtBudget)
	{
		title_ = profile.gamertag.empty() ? "Your library"
						  : profile.gamertag;
		m_.games = games;
		names_.apply_cached(m_.games);
		m_.recent = load_recent();
		native_ = load_native();
		if (g_opts.fake_consoles > 0)
			m_.consoles = fake_consoles(g_opts.fake_consoles);
		/* Nothing is going to look, so do not claim to be looking:
		 * the synthetic path has no worker and the real one starts
		 * below. */
		m_.consoles_pending = !g_opts.fake_catalog;
		m_.sort();
		reindex();
		rebuild("", SEC_ALL);
		names_.request(m_.games);
		/* Always started: even when the catalog came from a cache and
		 * needs no refresh, this is what finds the account's consoles
		 * (and it is the thread that has already paid for the
		 * credential chain). */
		if (!g_opts.fake_catalog)
			refresh_.reset(new CatalogRefresh(auth, refresh));
	}

	~LibraryScreen()
	{
		if (paints_)
			std::fprintf(stderr,
				     "library: %u repaints, avg %.2f ms, max %.2f ms; "
				     "art %zu KB in memory\n",
				     paints_, paint_sum_ / paints_, paint_max_,
				     thumbs_.bytes() / 1024);
	}

	void run();

private:
	/* ---- list bookkeeping ---- */
	int list_bottom() const { return search_ ? kSearchTop : kListBottom; }
	/* Where the rows start: below the letter strip when it is showing. */
	int list_top() const
	{
		return kListTop + (letters_showing() || genre_strip_showing()
					   ? kLetterH
					   : 0);
	}
	/* The letter strip belongs to the flat A-Z list and nothing else: not
	 * to a search (which spans every letter), not inside a genre (whose
	 * list is already short), not on Home or Genres. */
	bool letters_showing() const
	{
		return !search_ && m_.tab == TAB_ALL && m_.genre_filter.empty();
	}
	/*
	 * Inside a genre, the strip under the tabs names it and is the way
	 * back out.
	 *
	 * It used to take over the TAB bar instead, which made stepping into
	 * a genre feel like leaving the screen: the one row that tells you
	 * where you are in the whole client vanished, and no tab could be
	 * reached until you had backed out. A genre is a filter on the Genres
	 * tab, not a place of its own, so the tabs stay put and the strip --
	 * which is where per-tab things belong anyway -- carries the genre.
	 */
	bool genre_strip_showing() const
	{
		return !search_ && !m_.genre_filter.empty();
	}
	/*
	 * Whether the tab showing has a strip of its own between the tabs and
	 * the body. Home and Genres do not, so Up from their first row goes
	 * straight to the tabs rather than stopping on nothing.
	 */
	bool has_strip() const
	{
		return (letters_showing() && !m_.letter_tabs.empty()) ||
		       genre_strip_showing() || m_.tab == TAB_SETTINGS;
	}
	/* Move the cursor down a band, as far as there is somewhere to go. */
	void focus_down();
	/* Left/Right along whichever strip is lit. */
	void strip_step(int dir);
	/* The primary tab, from anywhere: the shoulders, or Left/Right up on
	 * the tab strip itself. */
	void tab_step(int dir);
	const gnx::Game *selected() const
	{
		if (sel_ < 0 || sel_ >= (int)m_.rows.size())
			return nullptr;
		const Row &r = m_.rows[sel_];
		return r.kind == ROW_GAME ? &m_.games[r.game] : nullptr;
	}
	/* Rows the cursor may land on: games and consoles, never headings. */
	int selectable_row(int from, int dir) const;
	void step(int dir, int count);
	void ensure_visible();
	void wait_for_names();
	void reindex();
	void rebuild(const std::string &keep_id, Section keep_sec);
	void resort();
	bool pump_refresh();
	void launch();
	void leave_genre();
	void run_button_tester();
	void draw_pad_graphic(Painter &pt);
	void run_network_test();
	void show_about();
	bool confirm(const char *title, const char *goes, const char *stays);
	void wipe(bool auth, bool caches);
	void show_info(const gnx::Game &g);

	/* ---- input ---- */
	void handle_list(int now);
	void handle_tabs(int now);
	void handle_strip(int now);
	void handle_body(int now);
	void handle_search();

	/* ---- painting ---- */
	void paint();
	void paint_header(Painter &pt);
	void paint_tabs(Painter &pt);
	void paint_letters(Painter &pt);
	void paint_genre_strip(Painter &pt);
	void paint_rows(Painter &pt);
	void paint_footer(Painter &pt);
	void paint_hints(Painter &pt, const char *hints, uint8_t level);
	void draw_title(Painter &pt, int row, int x, int y, const char *s,
			uint8_t level, int max_w);
	void paint_search(Painter &pt);
	std::string tag_of(const gnx::Game &g, bool recent) const;

	drm_out &out_;
	Fonts &f_;
	pad &p_;
	gnx::XboxAuth &auth_;
	std::string title_;
	Model m_;
	NameResolver names_;
	Thumbs thumbs_;
	std::unique_ptr<CatalogRefresh> refresh_;
	/* Title ids seen encoding below 720p (library_data.cpp). Loaded once
	 * and re-read after a stream, which is the only thing that changes
	 * it. */
	std::set<std::string> native_;

	int sel_ = -1;         /* row index; -1 = nothing selectable */
	int top_ = 0;          /* first row painted */
	/*
	 * Marquee on the selected row's title. Names like "Baldur's Gate and
	 * Baldur's Gate II: Enhanced Edition" are wider than the row, and an
	 * ellipsis in the middle of a series is exactly the part you needed.
	 * It waits before moving so a name that fits your eye in one go is
	 * never in motion while you read it.
	 */
	int marq_row_ = -1;    /* which row the timer belongs to */
	int marq_at_ = 0;      /* ms the cursor landed there */
	int marq_paint_ = 0;   /* ms of the last marquee repaint */
	bool marq_live_ = false; /* the last paint drew a moving title */
	/* Starts in the body: the common thing to want is a game, and having
	 * to press Down before you could scroll would tax every launch to
	 * make the tabs one press cheaper. */
	Focus focus_ = FOCUS_BODY;
	bool search_ = false;  /* the picker is open */
	int cursor_ = 0;       /* picker cell */
	std::string search_from_;             /* selection when Y was pressed */
	Section search_from_sec_ = SEC_ALL;
	bool dirty_ = true;
	int stick_at_ = 0;
	int last_pending_ = -1;

	/* Repaint timing. */
	unsigned paints_ = 0;
	double paint_sum_ = 0, paint_max_ = 0;
};

/* The nearest game row at or after (dir > 0) / before `from`; -1 if none. */
int LibraryScreen::selectable_row(int from, int dir) const
{
	for (int i = from; i >= 0 && i < (int)m_.rows.size(); i += dir)
		if (m_.rows[i].kind == ROW_GAME ||
		    m_.rows[i].kind == ROW_CONSOLE ||
		    m_.rows[i].kind == ROW_GENRE ||
		    m_.rows[i].kind == ROW_TOOL)
			return i;
	return -1;
}

void LibraryScreen::step(int dir, int count)
{
	if (sel_ < 0)
		return;
	int at = sel_;
	for (int n = 0; n < count; n++) {
		int next = selectable_row(at + dir, dir);
		if (next < 0)
			break;
		at = next;
	}
	sel_ = at;
}

void LibraryScreen::ensure_visible()
{
	const std::vector<Row> &rows = m_.rows;

	if (sel_ < 0) {
		top_ = 0;
		return;
	}
	if (sel_ < top_)
		top_ = sel_;
	/* Bring a section title along when it sits right above. */
	if (top_ > 0 && rows[top_ - 1].kind == ROW_SECTION)
		top_--;
	for (;;) {
		int y = list_top();
		for (int i = top_; i < sel_; i++)
			y += row_height(rows[i]);
		if (y + row_height(rows[sel_]) <= list_bottom() || top_ >= sel_)
			break;
		top_++;
	}
}

/* Rebuild the rows and put the selection back on the same title. */
/*
 * Re-derive the genre and letter indexes. Called only when the DATA behind
 * them changes -- the games list, or the set of native titles -- and never
 * from rebuild(), which runs on every tab and letter change.
 *
 * That distinction is not academic: indexing walks ~585 titles into a map of
 * strings, and with it on the rebuild path the device logged an 11.1 ms
 * repaint against a 10 ms warning threshold. On the host it was invisible.
 */
void LibraryScreen::reindex()
{
	m_.native = &native_;
	m_.index_genres();
	m_.index_letters();
}

void LibraryScreen::rebuild(const std::string &keep_id, Section keep_sec)
{
	m_.build_rows();
	sel_ = -1;
	if (!keep_id.empty()) {
		int any = -1;
		for (int i = 0; i < (int)m_.rows.size(); i++) {
			const Row &r = m_.rows[i];
			if (r.kind != ROW_GAME ||
			    m_.games[r.game].title_id != keep_id)
				continue;
			if (r.section == keep_sec) {
				sel_ = i;
				break;
			}
			if (any < 0)
				any = i;
		}
		if (sel_ < 0)
			sel_ = any;
	}
	if (sel_ < 0)
		sel_ = selectable_row(0, +1);
	ensure_visible();
	dirty_ = true;
}

void LibraryScreen::resort()
{
	std::string id;
	Section sec = SEC_ALL;

	if (const gnx::Game *g = selected()) {
		id = g->title_id;
		sec = m_.rows[sel_].section;
	}
	m_.sort();
	reindex();
	rebuild(id, sec);
}

bool LibraryScreen::pump_refresh()
{
	std::vector<gnx::Game> fresh;
	bool changed = false;

	if (!refresh_)
		return false;
	/* Consoles arrive on the same worker and are independent of whether
	 * the catalog changed, so they are taken first and on their own. */
	{
		std::vector<gnx::HomeConsole> consoles;
		bool rebuild_rows = false;

		if (refresh_->take_consoles(consoles) &&
		    consoles.size() != m_.consoles.size()) {
			m_.consoles.swap(consoles);
			rebuild_rows = true;
		}
		/* The lookup finishing is itself a change worth repainting for:
		 * it turns "Looking for your Xbox..." into "No console found". */
		const bool pending = !refresh_->consoles_done();
		if (pending != m_.consoles_pending) {
			m_.consoles_pending = pending;
			rebuild_rows = true;
		}
		if (rebuild_rows) {
			m_.build_rows();
			changed = true;
		}
	}
	if (!refresh_->take(fresh))
		return changed;
	if (catalog_signature(fresh) == catalog_signature(m_.games)) {
		std::fprintf(stderr, "library: catalog unchanged (%zu titles)\n",
			     fresh.size());
		return false;
	}
	std::fprintf(stderr, "library: catalog refreshed: %zu titles (was %zu)\n",
		     fresh.size(), m_.games.size());
	/* Keep the names we have: the cache covers most, and the ones
	 * resolved this session are not in it yet. */
	names_.apply_cached(fresh);
	for (gnx::Game &g : fresh) {
		if (!g.name.empty())
			continue;
		int i = m_.find_game(g.title_id);
		if (i >= 0) {
			g.name = m_.games[i].name;
			g.box_art_url = m_.games[i].box_art_url;
			/* Carried for the same reason as the name: the
			 * refreshed catalog has none of it, and dropping it
			 * would empty every genre until the whole library had
			 * been resolved again. */
			g.genre = m_.games[i].genre;
			g.rating = m_.games[i].rating;
			g.rating_count = m_.games[i].rating_count;
			g.year = m_.games[i].year;
		}
	}
	m_.games.swap(fresh);
	resort();
	names_.request(m_.games);
	return true;
}

/* Step back from a genre's games to the list of genres, landing the cursor
 * on the genre just left rather than at the top. */
void LibraryScreen::leave_genre()
{
	const std::string was = m_.genre_filter;

	m_.genre_filter.clear();
	rebuild("", SEC_ALL);
	sel_ = selectable_row(0, +1);
	for (size_t i = 0; i < m_.rows.size(); i++)
		if (m_.rows[i].kind == ROW_GENRE &&
		    m_.genres[m_.rows[i].game].first == was) {
			sel_ = (int)i;
			break;
		}
}

/*
 * The button tester.
 *
 * This exists because of a gap in what is known about this hardware. From a
 * Linux event code onwards everything is deterministic -- face_map() picks
 * the Xbox button, the engine puts it in the pad packet -- but the step
 * BEFORE that, which physical button emits which code, was taken from the
 * device tree and never pressed (KNOWN-ISSUES 5). So the screen shows both
 * halves: the raw code the driver sent, and what this build does with it.
 * Press the button marked A; if the top line does not say A, that is the
 * answer, and "Face buttons" in Settings is the fix.
 *
 * Exit is a HELD B rather than a tap, because every button here is under
 * test and one that quits cannot also be demonstrated.
 */
/*
 * A gamepad, in Xbox positions, with everything currently held lit.
 *
 * Body and grips are discs and rectangles rather than anything clever: at
 * 640x480 the silhouette is what makes it recognisable, and the detail that
 * matters is which of the coloured things is green.
 *
 * The sticks show deflection as well as clicks -- the inner disc is offset by
 * the live axis reading -- because a stick that drifts is a fault this screen
 * should catch, and a number alone does not show a slow wander.
 */
void LibraryScreen::draw_pad_graphic(Painter &pt)
{
	const uint8_t kBody = 46, kBtn = 96, kEdge = 128;
	auto held = [&](enum pad_button b) { return p_.down[b] != 0; };
	auto button = [&](int cx, int cy, int r, enum pad_button b,
			  const char *label) {
		if (held(b))
			pt.sel_disc(cx, cy, r);
		else
			pt.disc(cx, cy, r, kBtn);
		if (label) {
			const int lw = text_measure(f_.small, label);
			pt.line(f_.small, cx - lw / 2, cy + 5, label,
				held(b) ? 255 : 40);
		}
	};

	/* Body: a slab with rounded ends, then the two grips under it. */
	pt.rect(198, 150, 244, 140, kBody);
	pt.disc(198, 220, 70, kBody);
	pt.disc(442, 220, 70, kBody);
	pt.disc(212, 286, 44, kBody);
	pt.disc(428, 286, 44, kBody);

	/* Triggers above the bumpers, both at the top edge where they are on
	 * the real thing -- LT/RT are what a thumb looks for first. */
	auto shoulder = [&](int x, int y, int w, int h, enum pad_button b,
			    const char *label) {
		if (held(b))
			pt.sel_rect(x, y, w, h);
		else
			pt.rect(x, y, w, h, kBtn);
		pt.line(f_.small, x + (w - text_measure(f_.small, label)) / 2,
			y + h - 4, label, held(b) ? 255 : 40);
	};
	shoulder(176, 124, 52, 18, PAD_L2, "LT");
	shoulder(412, 124, 52, 18, PAD_R2, "RT");
	shoulder(176, 146, 52, 18, PAD_L1, "LB");
	shoulder(412, 146, 52, 18, PAD_R1, "RB");

	/* Left stick, D-pad below it; face diamond right, right stick under
	 * it -- the Xbox arrangement, which is the one being checked. */
	auto stick = [&](int cx, int cy, enum pad_button click, int ax,
			 int ay) {
		pt.disc(cx, cy, 26, kEdge);
		pt.disc(cx, cy, 23, 60);
		/* Full deflection moves the cap to the rim, so a centred
		 * stick sits centred and a drifting one visibly does not. */
		const int dx = ax * 11 / 32768, dy = ay * 11 / 32768;
		if (held(click))
			pt.sel_disc(cx + dx, cy + dy, 14);
		else
			pt.disc(cx + dx, cy + dy, 14, kBtn);
	};
	stick(238, 196, PAD_L3, p_.lx, p_.ly);
	stick(368, 254, PAD_R3, p_.rx, p_.ry);

	/* D-pad as four arms, so a dead direction shows as a dead arm. */
	const int dx = 286, dy = 254, arm = 15, wide = 13;
	auto pad_arm = [&](int x, int y, int w, int h, enum pad_button b) {
		if (held(b))
			pt.sel_rect(x, y, w, h);
		else
			pt.rect(x, y, w, h, kBtn);
	};
	pad_arm(dx - wide / 2, dy - arm - wide / 2, wide, arm, PAD_UP);
	pad_arm(dx - wide / 2, dy + wide / 2, wide, arm, PAD_DOWN);
	pad_arm(dx - arm - wide / 2, dy - wide / 2, arm, wide, PAD_LEFT);
	pad_arm(dx + wide / 2, dy - wide / 2, arm, wide, PAD_RIGHT);
	pt.rect(dx - wide / 2, dy - wide / 2, wide, wide, kBtn);

	/* The diamond. Positions are the Xbox ones on purpose. */
	const int fx = 400, fy = 196, off = 28, r = 15;
	button(fx, fy - off, r, PAD_Y, "Y");
	button(fx - off, fy, r, PAD_X, "X");
	button(fx + off, fy, r, PAD_B, "B");
	button(fx, fy + off, r, PAD_A, "A");

	/* View, Xbox and Menu across the middle, unlabelled: at ten pixels
	 * across there is no room for a word inside one, and a word beside
	 * one lands on the stick. The "Acts as" line names whatever is held,
	 * which is the labelling that actually answers the question. */
	button(300, 208, 10, PAD_SELECT, nullptr);
	button(340, 208, 10, PAD_START, nullptr);
	button(320, 176, 13, PAD_MODE, nullptr);
}

void LibraryScreen::run_button_tester()
{
	constexpr int kHoldMs = 900;
	int b_since = 0;

	/* Start clean: whatever opened this screen is not a test result. */
	for (int b = 0; b < PAD_COUNT; b++)
		pad_take_press(&p_, (enum pad_button)b);
	p_.last_code = 0;

	while (!g_stop && !pad_script_done(&p_)) {
		const int now = pad_now_ms();
		int held = 0;

		if (p_.down[PAD_B]) {
			if (!b_since)
				b_since = now;
			held = now - b_since;
			if (held >= kHoldMs)
				return;
		} else {
			b_since = 0;
		}

		Painter pt(out_, f_);
		char line[128];

		pt.rect(0, 0, kWidth, kHeaderH, 40);
		pt.line(f_.big, 18, 37, "Button tester", 235);

		/* What the driver last said, and what we made of it. The two
		 * lines are the whole point, so they go at the top. */
		const char *raw = pad_evdev_name(p_.last_code);
		if (p_.last_code)
			std::snprintf(line, sizeof(line), "%s  (code %d)",
				      raw ? raw : "unknown code",
				      p_.last_code);
		else
			std::snprintf(line, sizeof(line),
				      "press any button");
		pt.line(f_.mid, 18, 84, "Driver sent", 150);
		pt.line(f_.mid, 150, 84, line, raw ? 245 : 170);

		std::string acts;
		for (int b = 0; b < PAD_COUNT; b++)
			if (p_.down[b]) {
				if (!acts.empty())
					acts += " + ";
				acts += pad_button_name((enum pad_button)b);
			}
		pt.line(f_.mid, 18, 112, "Acts as", 150);
		pt.line(f_.mid, 150, 112, acts.empty() ? "-" : acts.c_str(),
			acts.empty() ? 150 : 245);

		/*
		 * The controller, with whatever is held lit up on it.
		 *
		 * Drawn in XBOX positions, not this device's -- A at the
		 * bottom, B right, X left, Y top -- because the question is
		 * "what does the service think I pressed", and the answer is
		 * only legible against the shape the service expects. Press
		 * the button marked A on the shell; if the graphic lights
		 * anything but the bottom one, that is the bug, and "Face
		 * buttons" in Settings is the fix.
		 */
		draw_pad_graphic(pt);

		std::snprintf(line, sizeof(line),
			      "Sticks   L %6d,%6d      R %6d,%6d",
			      p_.lx, p_.ly, p_.rx, p_.ry);
		pt.line(f_.small, 20, kListBottom - 42, line, 175);
		std::snprintf(line, sizeof(line),
			      "Face buttons: %s%s   (Settings changes this)",
			      p_.layout == PAD_LAYOUT_POSITIONAL
				      ? "Xbox positions"
				      : "printed labels",
			      p_.swap_xy ? ", X and Y swapped" : "");
		pt.line(f_.small, 20, kListBottom - 20, line, 150);

		pt.rect(0, kListBottom, kWidth, kFooterH, 28);
		if (held)
			pt.rect(0, kListBottom, kWidth * held / kHoldMs, 3,
				200);
		pt.centred(f_.small, kHeight - 8,
			   held ? "keep holding to leave"
				: "hold B to leave", held ? 220 : 150);
		pt.present();

		pad_poll(&p_, 16);
	}
}

/*
 * The info panel: everything known about one title, on X.
 *
 * All of it is already in hand -- the catalogue call fetches genre, rating,
 * year and the entitlement flags, and the row has room for about two of
 * them. This is where the rest goes, so a title can be looked at before it is
 * launched rather than only after.
 *
 * Deliberately local: no request is made when this opens, so it is instant
 * and works with the Wi-Fi off. The Store's own description, publisher and
 * developer ride in the SAME response the catalogue already parses and
 * discards, so they could join this -- see the note in catalog.cpp.
 */
void LibraryScreen::show_info(const gnx::Game &g)
{
	const std::string label = m_.labels[m_.rows[sel_].game];

	for (int b = 0; b < PAD_COUNT; b++)
		pad_take_press(&p_, (enum pad_button)b);

	while (!g_stop && !pad_script_done(&p_)) {
		Painter pt(out_, f_);
		char buf[160];
		int y = kHeaderH + 24;

		pt.rect(0, 0, kWidth, kHeaderH, 40);
		pt.line_fit(f_.big, 18, 37, label.c_str(), 235, kWidth - 36);

		/* The poster at the size the panel has room for, which is
		 * four times the area of the row's thumbnail. */
		const Thumb *t = thumbs_.get(g.product_id, g.box_art_url);
		const int ax = 20, aw = 108, ah = 156;
		if (t && t->ok())
			pt.blit_nv12(ax + ((aw - t->w) / 2 & ~1),
				     y + ((ah - t->h) / 2 & ~1),
				     t->luma.data(), t->chroma.data(), t->w,
				     t->w, t->h);
		else
			pt.rect(ax, y, aw, ah, 34);

		const int tx = ax + aw + 20;
		auto row = [&](const char *k, const std::string &v) {
			if (v.empty())
				return;
			pt.line(f_.small, tx, y + 14, k, 140);
			pt.line_fit(f_.small, tx + 96, y + 14, v.c_str(), 215,
				    kWidth - tx - 96 - 16);
			y += 24;
		};

		/* How this account can actually play it comes first: it is
		 * the difference between a title you can start and one the
		 * Store would like to sell you. */
		std::string how;
		if (g.owned)
			how = "Owned";
		if (g.in_subscription)
			how += how.empty() ? "Game Pass" : ", Game Pass";
		if (g.ad_playable)
			how += how.empty() ? "Free with ads"
					   : ", free with ads";
		row("Play", how.empty() ? "Not available on this account" : how);
		if (g.max_session_secs > 0) {
			std::snprintf(buf, sizeof(buf), "%d minutes a session",
				      g.max_session_secs / 60);
			row("Ad limit", buf);
		}
		row("Genre", g.genre);
		if (g.year) {
			std::snprintf(buf, sizeof(buf), "%d", g.year);
			row("Released", buf);
		}
		if (g.rating > 0 && g.rating_count > 0) {
			std::snprintf(buf, sizeof(buf), "%.1f out of 5  (%s)",
				      g.rating,
				      human_count(g.rating_count).c_str());
			row("Rating", buf);
		}
		/* The one fact here that is ours rather than the Store's, and
		 * the one that changes how the game looks on this panel. */
		row("Encode", native_.count(g.title_id)
				      ? "640x360 native -- no downscale"
				      : "1280x720, halved to fit");
		row("Launch id", g.title_id);
		row("Store id", g.product_id);

		pt.rect(0, kListBottom, kWidth, kFooterH, 28);
		std::snprintf(buf, sizeof(buf), "%s Play     %s Back",
			      pad_button_label(&p_, PAD_A),
			      pad_button_label(&p_, PAD_B));
		pt.centred(f_.small, kHeight - 8, buf, 150);
		pt.present();

		pad_poll(&p_, 16);
		if (pad_take_press(&p_, PAD_B) || pad_take_press(&p_, PAD_X))
			return;
		if (pad_take_press(&p_, PAD_A)) {
			/* Straight into the game from here: having just read
			 * about it is the likeliest moment to want it. */
			for (int b = 0; b < PAD_COUNT; b++)
				pad_take_press(&p_, (enum pad_button)b);
			launch();
			return;
		}
	}
}

/*
 * The network test: four measurements and a verdict.
 *
 * The Xbox console's own "test network speed & statistics" is a wizard in the
 * console OS, not an API anyone can call -- checked 2026-09-08, and the only
 * streaming-related thing Microsoft publishes for developers is the Cloud
 * Aware API a TITLE uses to ask whether it is being streamed. So this is
 * assembled from two public sources instead:
 *
 *   - LATENCY against uks.gssv-play-prod.xboxlive.com, the actual xCloud
 *     edge this client streams from. Any HTTP answer proves the round trip;
 *     the status does not matter, the timing does. The first sample is
 *     discarded because it pays for the TLS handshake.
 *   - THROUGHPUT against speed.cloudflare.com/__down and /__up, the public
 *     endpoints behind Cloudflare's own speed test. Not Microsoft, but a
 *     large anycast network that is nearly always closer than the game
 *     stream is, so it measures the local link rather than a distant server.
 *
 * Latency and jitter come from the path that matters; bandwidth comes from
 * the one that can actually saturate. Neither number alone answers the
 * question, which is why both are here -- and why the verdict at the end
 * weighs them together. 720p wants about 4.5 Mbps and 720p high about 7
 * (docs/RESOLUTION.md), and jitter is what turns a link that has the
 * bandwidth into one that still stutters (KNOWN-ISSUES 0).
 */
namespace {

enum NetStage { NET_LATENCY, NET_DOWN, NET_UP, NET_DONE };

/* Latency targets, best first. See the note in the worker. */
const struct { const char *url, *label; } kNetTarget[] = {
	{ "https://uks.gssv-play-prod.xboxlive.com/", "xCloud edge" },
	{ "https://xsts.auth.xboxlive.com/",          "Xbox Live" },
};
constexpr int kNetTargetCount =
	(int)(sizeof(kNetTarget) / sizeof(kNetTarget[0]));

struct NetResult {
	std::atomic<int> stage{NET_LATENCY};
	std::atomic<int> lat_ms{0};     /* median round trip to the edge */
	std::atomic<int> lat_jit{0};    /* spread across the samples */
	std::atomic<int> lat_n{0};      /* how many answered */
	std::atomic<int> lat_target{-1}; /* index into kNetTarget, -1 none */
	std::atomic<int> down_kbps{0};
	std::atomic<int> up_kbps{0};
	std::atomic<bool> failed{false};
	std::mutex lock;
	std::string err;
};

/*
 * Signal strength, from /proc/net/wireless -- present on any Linux with a
 * wireless interface and needing no privileges, unlike the nl80211 socket
 * wifi_tune.c opens for the setting side. Column 4 is the level in dBm.
 * Returns 0 when it cannot be read, which includes the host simulator.
 */
int wifi_dbm()
{
	FILE *f = std::fopen("/proc/net/wireless", "r");
	char line[256];
	int dbm = 0;

	if (!f)
		return 0;
	/* Two header lines, then one per interface. */
	while (std::fgets(line, sizeof(line), f)) {
		char iface[64];
		float link, level;

		if (std::sscanf(line, " %63[^:]: %*x %f %f", iface, &link,
				&level) == 3) {
			dbm = (int)level;
			break;
		}
	}
	std::fclose(f);
	return dbm;
}

}  // namespace

/*
 * "Are you sure", for the three tools that delete something.
 *
 * Defaults to cancelling in the sense that matters: nothing happens until A
 * is pressed, B is offered first in the hint, and the panel spells out what
 * goes and what stays rather than saying "this cannot be undone" and leaving
 * the reader to guess the scope.
 */
bool LibraryScreen::confirm(const char *title, const char *goes,
			    const char *stays)
{
	for (int b = 0; b < PAD_COUNT; b++)
		pad_take_press(&p_, (enum pad_button)b);

	while (!g_stop && !pad_script_done(&p_)) {
		Painter pt(out_, f_);
		char buf[96];

		pt.rect(0, 0, kWidth, kHeaderH, 40);
		pt.line(f_.big, 18, 37, title, 235);

		pt.line(f_.mid, 24, 110, "This will:", 150);
		pt.line_fit(f_.mid, 24, 146, goes, 240, kWidth - 48);
		if (stays) {
			pt.line(f_.mid, 24, 200, "This will not:", 150);
			pt.line_fit(f_.mid, 24, 236, stays, 200, kWidth - 48);
		}
		pt.line_fit(f_.small, 24, 300,
			    "The app closes afterwards; open it again from "
			    "Ports.", 150, kWidth - 48);

		pt.rect(0, kListBottom, kWidth, kFooterH, 28);
		std::snprintf(buf, sizeof(buf), "%s Cancel      %s Go ahead",
			      pad_button_label(&p_, PAD_B),
			      pad_button_label(&p_, PAD_A));
		pt.centred(f_.small, kHeight - 8, buf, 170);
		pt.present();

		pad_poll(&p_, 16);
		if (pad_take_press(&p_, PAD_B))
			return false;
		if (pad_take_press(&p_, PAD_A))
			return true;
	}
	return false;
}

/*
 * Delete the chosen halves of the state directory, then stop the client.
 *
 * Stopping is not tidiness: the catalogue, the thumbnails and the auth token
 * are all held in memory by objects that outlive this call, so carrying on
 * would mean running against state that has just been deleted underneath --
 * and, after a log out, still holding the token that was supposedly
 * forgotten. A clean exit makes the next launch the reset one.
 */
void LibraryScreen::wipe(bool auth, bool caches)
{
	const std::string dir = g_opts.state_dir;
	int gone = 0;

	if (auth)
		for (const char *f : kAuthFiles)
			gone += std::remove((dir + "/" + f).c_str()) == 0;
	if (caches) {
		for (const char *f : kCacheFiles)
			gone += std::remove((dir + "/" + f).c_str()) == 0;
		/* The art cache is a directory of files named by product id;
		 * empty it rather than removing the directory, which the
		 * thumbnail worker expects to exist. */
		const std::string art = dir + "/art";
		if (DIR *d = opendir(art.c_str())) {
			while (struct dirent *e = readdir(d)) {
				if (e->d_name[0] == '.')
					continue;
				gone += std::remove((art + "/" + e->d_name)
							    .c_str()) == 0;
			}
			closedir(d);
		}
	}
	std::fprintf(stderr, "tools: wiped %d file%s from %s (auth=%d "
			     "caches=%d)\n",
		     gone, gone == 1 ? "" : "s", dir.c_str(), auth, caches);

	/* Say what happened before going: a screen that vanishes on a button
	 * press is indistinguishable from a crash. */
	const int until = pad_now_ms() + 1400;
	while (!g_stop && pad_now_ms() < until) {
		Painter pt(out_, f_);
		char buf[96];

		pt.rect(0, 0, kWidth, kHeaderH, 40);
		pt.line(f_.big, 18, 37, "Done", 235);
		std::snprintf(buf, sizeof(buf), "%d file%s removed", gone,
			      gone == 1 ? "" : "s");
		pt.centred(f_.mid, 200, buf, 235);
		pt.centred(f_.small, 240, "Closing. Open it again from Ports.",
			   165);
		pt.present();
		pad_poll(&p_, 30);
	}
	g_stop = 1;
}

/* Who wrote it and which build this is. The build line is the same string
 * the footer carries; it is here too because this is where someone looks. */
void LibraryScreen::show_about()
{
	for (int b = 0; b < PAD_COUNT; b++)
		pad_take_press(&p_, (enum pad_button)b);

	while (!g_stop && !pad_script_done(&p_)) {
		Painter pt(out_, f_);
		char buf[96];

		pt.rect(0, 0, kWidth, kHeaderH, 40);
		pt.line(f_.big, 18, 37, "About", 235);

		pt.centred(f_.big, 170, "Xbox Cloud Gaming", 235);
		pt.centred(f_.mid, 206, "for the Anbernic RG353", 165);
		pt.centred(f_.big, 280, "Dan Herman", 245);

		std::snprintf(buf, sizeof(buf), "%s   built %s", g_version,
			      g_build_time);
		pt.centred(f_.small, 330, buf, 140);
		pt.centred(f_.small, 356, "GPL-3.0.  Not affiliated with "
					  "Microsoft or Anbernic.", 120);

		pt.rect(0, kListBottom, kWidth, kFooterH, 28);
		std::snprintf(buf, sizeof(buf), "%s Back",
			      pad_button_label(&p_, PAD_B));
		pt.centred(f_.small, kHeight - 8, buf, 150);
		pt.present();

		pad_poll(&p_, 16);
		if (pad_take_press(&p_, PAD_B) || pad_take_press(&p_, PAD_A))
			return;
	}
}

void LibraryScreen::run_network_test()
{
	NetResult r;
	const int dbm = wifi_dbm();

	std::thread worker([&r]() {
		gnx::Http http;   /* one handle: connections stay warm */
		std::vector<int> samples;

		/*
		 * Http::get THROWS on a curl-level failure -- no route, DNS
		 * gone, TLS refused -- which on a network test is not an
		 * exceptional case but one of the results. Every call here is
		 * wrapped: an unreachable host is a measurement of zero, not
		 * a crash. (It was a crash first: the exception escaped the
		 * thread and took the client with it.)
		 */
		auto timed = [&http](const char *url, int *ms,
				     std::string *body) {
			const double t0 = now_ms();

			try {
				gnx::HttpResponse res = http.get(url);
				*ms = (int)(now_ms() - t0);
				if (body)
					*body = std::move(res.body);
				return res.status;
			} catch (const std::exception &) {
				*ms = (int)(now_ms() - t0);
				return 0L;
			}
		};

		/*
		 * --- latency ---
		 *
		 * The streaming edge first, then Xbox Live's auth host, which
		 * is the next thing along the same chain and answers a bare
		 * GET with a 404. The edge is the better target when it is
		 * reachable, but its name does not resolve everywhere outside
		 * a live session, and a latency figure from a host that never
		 * replied is worse than one that names where it went -- so
		 * the screen says which was used.
		 */
		for (int t = 0; t < kNetTargetCount && r.lat_target < 0; t++) {
			int ms = 0;

			if (timed(kNetTarget[t].url, &ms, nullptr))
				r.lat_target = t;
		}
		if (r.lat_target >= 0) {
			const char *url = kNetTarget[r.lat_target].url;

			/* The probe above already paid for DNS and the TLS
			 * handshake, so these are warm round trips. */
			for (int i = 0; i < 5; i++) {
				int ms = 0;

				/* Status is irrelevant -- an unauthenticated
				 * GET is meant to be refused. A reply at all
				 * is the measurement. */
				if (timed(url, &ms, nullptr))
					samples.push_back(ms);
			}
		}
		if (!samples.empty()) {
			std::sort(samples.begin(), samples.end());
			r.lat_ms = samples[samples.size() / 2];
			r.lat_jit = samples.back() - samples.front();
			r.lat_n = (int)samples.size();
		}

		/* --- download --- */
		r.stage = NET_DOWN;
		{
			int ms = 0;
			std::string body;
			long status = timed("https://speed.cloudflare.com/"
					    "__down?bytes=4000000",
					    &ms, &body);

			if (status && body.size() > 100000 && ms > 0)
				r.down_kbps = (int)(body.size() * 8 /
						    (size_t)ms);
		}

		/* --- upload, which only has to be adequate: the pad packets
		 * going the other way are a few kbit/s, and a link that
		 * cannot manage this cannot hold a session open either. --- */
		r.stage = NET_UP;
		try {
			const std::string payload(512 * 1024, 'x');
			const double t0 = now_ms();
			gnx::HttpResponse res = http.post(
				"https://speed.cloudflare.com/__up", payload);
			const int ms = (int)(now_ms() - t0);

			if (res.status && ms > 0)
				r.up_kbps = (int)(payload.size() * 8 /
						  (size_t)ms);
		} catch (const std::exception &) {
			/* Left at zero, which the screen reports as failed. */
		}

		if (!r.lat_n && !r.down_kbps) {
			std::lock_guard<std::mutex> g(r.lock);
			r.err = "nothing answered -- no Wi-Fi, or no route out";
			r.failed = true;
		}
		r.stage = NET_DONE;
	});

	for (int b = 0; b < PAD_COUNT; b++)
		pad_take_press(&p_, (enum pad_button)b);

	bool leave = false;
	while (!g_stop && !leave && !pad_script_done(&p_)) {
		Painter pt(out_, f_);
		char buf[160];
		int y = kHeaderH + 34;

		pt.rect(0, 0, kWidth, kHeaderH, 40);
		pt.line(f_.big, 18, 37, "Network test", 235);

		auto row = [&](const char *k, const char *v, uint8_t lv) {
			pt.line(f_.small, 20, y, k, 140);
			pt.line(f_.mid, 190, y, v, lv);
			y += 34;
		};

		if (dbm) {
			std::snprintf(buf, sizeof(buf), "%d dBm  (%s)", dbm,
				      dbm > -55   ? "strong"
				      : dbm > -67 ? "workable"
						  : "weak");
			row("Wi-Fi signal", buf, dbm > -67 ? 235 : 215);
		} else {
			row("Wi-Fi signal", "not readable here", 150);
		}

		const int stage = r.stage;

		if (stage == NET_LATENCY) {
			row("Latency", "measuring\xE2\x80\xA6", 200);
		} else if (!r.lat_n) {
			row("Latency", "no answer from the edge", 215);
		} else {
			std::snprintf(buf, sizeof(buf),
				      "%d ms   jitter %d ms   (%s)", (int)r.lat_ms,
				      (int)r.lat_jit,
				      kNetTarget[r.lat_target].label);
			row("Latency", buf, 235);
		}

		if (stage < NET_DOWN)
			row("Download", "waiting", 130);
		else if (stage == NET_DOWN)
			row("Download", "measuring\xE2\x80\xA6", 200);
		else if (!r.down_kbps)
			row("Download", "failed", 215);
		else {
			std::snprintf(buf, sizeof(buf), "%.1f Mbps",
				      r.down_kbps / 1000.0);
			row("Download", buf, 235);
		}

		if (stage < NET_UP)
			row("Upload", "waiting", 130);
		else if (stage == NET_UP)
			row("Upload", "measuring\xE2\x80\xA6", 200);
		else if (!r.up_kbps)
			row("Upload", "failed", 215);
		else {
			std::snprintf(buf, sizeof(buf), "%.1f Mbps",
				      r.up_kbps / 1000.0);
			row("Upload", buf, 235);
		}

		if (stage == NET_DONE && !r.failed) {
			const double mbps = r.down_kbps / 1000.0;
			const int jit = r.lat_jit;

			/*
			 * The verdict, which is the reason anyone opened
			 * this. Bandwidth sets the ceiling and jitter decides
			 * whether the ceiling is reachable -- a link with
			 * plenty of headroom and a wandering round trip is
			 * exactly the one that stutters every ten seconds.
			 */
			y += 12;
			pt.line(f_.small, 20, y,
				"720p wants about 4.5 Mbps, 720p high about 7.",
				150);
			y += 26;
			const char *verdict =
				mbps < 4    ? "Below what 720p wants. Expect "
					      "dropped frames."
				: jit > 120 ? "Bandwidth is fine, but the "
					      "round trip wanders badly. "
					      "Expect stutter."
				: mbps >= 9 ? "Comfortable for either. 720p "
					      "high is worth trying."
				: mbps >= 6 ? "720p is comfortable. 720p high "
					      "may struggle on a dip."
					    : "720p should hold. 720p high is "
					      "out of reach here.";
			pt.line_fit(f_.small, 20, y, verdict, 235, kWidth - 40);
			y += 26;
			if (dbm && dbm <= -67)
				pt.line_fit(f_.small, 20, y,
					    "The signal is the limit here, not "
					    "the service -- move closer to the "
					    "access point.", 200, kWidth - 40);
		} else if (stage == NET_DONE) {
			std::lock_guard<std::mutex> g(r.lock);
			y += 12;
			pt.line_fit(f_.small, 20, y, r.err.c_str(), 215,
				    kWidth - 40);
		}

		pt.rect(0, kListBottom, kWidth, kFooterH, 28);
		std::snprintf(buf, sizeof(buf), "%s Back",
			      pad_button_label(&p_, PAD_B));
		pt.centred(f_.small, kHeight - 8, buf, 150);
		pt.present();

		pad_poll(&p_, 16);
		if (pad_take_press(&p_, PAD_B) && r.stage == NET_DONE)
			leave = true;
	}
	if (worker.joinable())
		worker.join();
}

void LibraryScreen::launch()
{
	/* A tool opens its own screen and comes back here when it is done. */
	if (sel_ >= 0 && sel_ < (int)m_.rows.size() &&
	    m_.rows[sel_].kind == ROW_TOOL) {
		switch (m_.rows[sel_].game) {
		case TOOL_BUTTONS:
			run_button_tester();
			break;
		case TOOL_NETWORK:
			run_network_test();
			break;
		case TOOL_ABOUT:
			show_about();
			break;
		case TOOL_LOGOUT:
			if (confirm("Log out",
				    "Forget the Xbox account signed in here",
				    "Touch your library cache or settings"))
				wipe(true, false);
			break;
		case TOOL_RESET:
			if (confirm("Reset app",
				    "Clear the library cache, box art and "
				    "settings",
				    "Sign you out -- you stay paired"))
				wipe(false, true);
			break;
		case TOOL_RESET_LOGOUT:
			if (confirm("Reset and log out",
				    "Clear everything: account, cache, box "
				    "art and settings", nullptr))
				wipe(true, true);
			break;
		}
		/* The tool owned the pad: drop any edge it left behind so
		 * releasing its exit combo does not act on this list. */
		for (int b = 0; b < PAD_COUNT; b++)
			pad_take_press(&p_, (enum pad_button)b);
		dirty_ = true;
		return;
	}
	/* A genre row steps in rather than starting anything. */
	if (sel_ >= 0 && sel_ < (int)m_.rows.size() &&
	    m_.rows[sel_].kind == ROW_GENRE) {
		m_.genre_filter = m_.genres[m_.rows[sel_].game].first;
		rebuild("", SEC_ALL);
		sel_ = selectable_row(0, +1);
		top_ = 0;
		ensure_visible();
		dirty_ = true;
		return;
	}
	/* Remote play: no title to pick, so this goes straight to the
	 * session. A console asleep in standby fails its first request and
	 * wakes doing it; the engine waits for it and asks again, which is
	 * why this can sit on "Waking your Xbox..." for half a minute. */
	if (sel_ >= 0 && sel_ < (int)m_.rows.size() &&
	    m_.rows[sel_].kind == ROW_CONSOLE) {
		const gnx::HomeConsole console = m_.consoles[m_.rows[sel_].game];

		if (g_opts.fake_catalog) {
			show_message(out_, f_, "Simulator",
				     ("Would stream " + console_label(console))
					     .c_str(),
				     "no session is created here");
			int until = pad_now_ms() + 600;
			while (!g_stop && pad_now_ms() < until)
				pad_poll(&p_, 50);
		} else {
			StreamRequest request;

			request.target = gnx::stream::StreamTarget::Console;
			request.id = console.server_id;
			request.name = console_label(console);
			stream_session(out_, f_, p_, auth_, request);
		}
		for (int b = 0; b < PAD_COUNT; b++)
			pad_take_press(&p_, (enum pad_button)b);
		dirty_ = true;
		return;
	}

	const gnx::Game *sel = selected();
	if (!sel)
		return;
	const gnx::Game game = *sel;
	const Section sec = m_.rows[sel_].section;

	note_recent(m_.recent, game.title_id);
	if (g_opts.fake_catalog) {
		/* Test bench: no session. Show what would have started and
		 * come back, so the recents can be exercised too. */
		std::string l1 = "Would stream " + display_name(game);
		show_message(out_, f_, "Simulator", l1.c_str(),
			     "no session is created here");
		int until = pad_now_ms() + 600;
		while (!g_stop && pad_now_ms() < until)
			pad_poll(&p_, 50);
	} else {
		if (refresh_ && refresh_->running()) {
			show_message(out_, f_, "One moment...",
				     "Finishing the library refresh");
			refresh_->wait();
			pump_refresh();
		}
		stream_game(out_, f_, p_, auth_, game);
		/* The stream may have just discovered this title encodes at
		 * our size, which changes its row. */
		native_ = load_native();
		reindex();
	}
	/* The stream owned the plane and the pad: repaint the list, and
	 * drop any press edge that leaked out of it so releasing the quit
	 * combo does not select a game. */
	for (int b = 0; b < PAD_COUNT; b++)
		pad_take_press(&p_, (enum pad_button)b);
	rebuild(game.title_id, sec);
}

/* ---- input ---------------------------------------------------------- */

/*
 * Down a band. Skips a strip this tab does not have, and refuses to leave the
 * tabs at all when there is nothing below to stand on -- an empty Genres tab
 * before the names have resolved -- because a cursor parked on an empty list
 * is a cursor nobody can see.
 */
void LibraryScreen::focus_down()
{
	if (focus_ == FOCUS_TABS && has_strip()) {
		focus_ = FOCUS_STRIP;
		return;
	}
	if (m_.tab == TAB_SETTINGS) {
		options_page_to_top();
		focus_ = FOCUS_BODY;
		return;
	}
	if (sel_ < 0)
		sel_ = selectable_row(0, +1);
	if (sel_ >= 0) {
		focus_ = FOCUS_BODY;
		ensure_visible();
	}
}

/* Left/Right along whichever strip is lit: the letters, or the settings
 * pages. Both mean the same thing -- change what the body below is showing. */
void LibraryScreen::strip_step(int dir)
{
	/* A genre strip has one move on it and it is Left: out. */
	if (genre_strip_showing()) {
		if (dir < 0) {
			leave_genre();
			focus_ = FOCUS_BODY;
			ensure_visible();
		}
		return;
	}
	if (m_.tab == TAB_SETTINGS) {
		options_page_group_step(dir);
		return;
	}
	const std::vector<char> &tabs = m_.letter_tabs;
	int at = 0;

	if (tabs.empty())
		return;
	for (size_t i = 0; i < tabs.size(); i++)
		if (tabs[i] == m_.letter)
			at = (int)i;
	m_.letter = tabs[(at + (int)tabs.size() + dir) % tabs.size()];
	rebuild("", SEC_ALL);
	sel_ = selectable_row(0, +1);
	top_ = 0;
	ensure_visible();
}

/*
 * The primary tab strip. Left/Right choose the tab, Down or A steps into it.
 * This is the only place the tab changes: a tab you have to point at first is
 * a tab you cannot lose your place to with a stray nudge of the stick.
 */
void LibraryScreen::handle_tabs(int now)
{
	pad &p = p_;
	int step_ = 0;

	if (pad_repeat(&p, PAD_LEFT, now))
		step_ = -1;
	if (pad_repeat(&p, PAD_RIGHT, now))
		step_ = +1;
	if (step_) {
		tab_step(step_);
		return;
	}
	/* A steps in as well as Down: "select the thing that is lit" should
	 * not stop meaning anything just because the lit thing is a tab. */
	if (pad_repeat(&p, PAD_DOWN, now) || pad_take_press(&p, PAD_A)) {
		focus_down();
		dirty_ = true;
	}
}

/*
 * The strip belonging to the tab showing: the letter index on All games, the
 * Standard/Advanced pages on Settings. Same two axes as the band above it.
 */
void LibraryScreen::handle_strip(int now)
{
	pad &p = p_;

	/*
	 * The strip can go away under the cursor -- the letters empty out
	 * while the catalogue is still arriving, a tab changed underneath.
	 * Fall back to the tabs rather than standing on nothing.
	 */
	if (!has_strip()) {
		focus_ = FOCUS_TABS;
		dirty_ = true;
		return;
	}
	if (pad_repeat(&p, PAD_LEFT, now)) {
		strip_step(-1);
		dirty_ = true;
		return;
	}
	if (pad_repeat(&p, PAD_RIGHT, now)) {
		strip_step(+1);
		dirty_ = true;
		return;
	}
	if (pad_repeat(&p, PAD_UP, now)) {
		focus_ = FOCUS_TABS;
		dirty_ = true;
		return;
	}
	if (pad_repeat(&p, PAD_DOWN, now) || pad_take_press(&p, PAD_A)) {
		focus_down();
		dirty_ = true;
	}
}

/*
 * Change the primary tab, from wherever the cursor happens to be.
 *
 * The four headers and nothing else. Folding the settings sub-pages onto this
 * loop as two extra stops was tried and taken back out: the shoulders are the
 * one gesture whose meaning never depends on where the cursor is, and "cycles
 * the headers, except on Settings where it also walks the sub-pages" is
 * exactly the kind of exception that costs that. Standard and Advanced are
 * reached the way every other strip is -- Up onto the strip, then Left/Right.
 *
 * A tab is the whole screen, so arriving at one should not leave a genre or a
 * half-typed search still filtering it: picking Genres and getting the games
 * of the genre you were in is not the tab you picked.
 */
void LibraryScreen::tab_step(int dir)
{
	m_.tab = (Tab)((m_.tab + TAB_COUNT + dir) % TAB_COUNT);
	m_.genre_filter.clear();
	m_.query.clear();
	if (m_.tab == TAB_SETTINGS)
		options_page_enter();
	rebuild("", SEC_ALL);
	sel_ = selectable_row(0, +1);
	top_ = 0;
	/* The band the cursor is in may not exist on the tab it just landed
	 * on -- Home has no strip. Fall back rather than float. */
	if (focus_ == FOCUS_STRIP && !has_strip())
		focus_ = FOCUS_TABS;
	if (focus_ == FOCUS_BODY && sel_ < 0 && m_.tab != TAB_SETTINGS)
		focus_ = FOCUS_TABS;
	ensure_visible();
	dirty_ = true;
}

void LibraryScreen::handle_list(int now)
{
	pad &p = p_;

	/*
	 * The shoulders cycle the primary tabs and do nothing else, from
	 * every band and every depth -- the one gesture on this screen whose
	 * meaning never depends on where the cursor is.
	 *
	 * They used to move a letter group here and switch the settings
	 * sub-tabs there, which made them the third way of doing what two
	 * other things already did and the only way of doing neither
	 * consistently. The strips are reachable with the D-pad now, so the
	 * shoulders are free to be the one shortcut that always means the
	 * same thing.
	 */
	if (pad_repeat(&p, PAD_R1, now)) {
		tab_step(+1);
		return;
	}
	if (pad_repeat(&p, PAD_L1, now)) {
		tab_step(-1);
		return;
	}

	switch (focus_) {
	case FOCUS_TABS:
		handle_tabs(now);
		return;
	case FOCUS_STRIP:
		handle_strip(now);
		return;
	case FOCUS_BODY:
		handle_body(now);
		return;
	}
}

void LibraryScreen::handle_body(int now)
{
	pad &p = p_;
	int prev = sel_;

	/*
	 * The settings rows belong to options.cpp, with one press held back:
	 * Up on the first row is how the cursor climbs to the sub-tabs, so it
	 * has to be taken before the page sees it. Left and Right stay the
	 * page's own -- down here they cycle the value on the row, which is
	 * editing rather than navigating, and is why the sub-tabs needed a
	 * band of their own in the first place.
	 */
	if (m_.tab == TAB_SETTINGS) {
		if (options_page_at_top() && !options_page_explaining() &&
		    pad_repeat(&p, PAD_UP, now)) {
			focus_ = FOCUS_STRIP;
			dirty_ = true;
			return;
		}
		if (options_page_input(p, now))
			dirty_ = true;
		return;
	}
	int page = (list_bottom() - list_top()) / kRowH;

	/* D-pad and left stick both scroll, with auto-repeat. */
	bool up = pad_repeat(&p, PAD_UP, now);
	bool down = pad_repeat(&p, PAD_DOWN, now);
	if (!up && !down && stick_repeat(p.ly, now, stick_at_)) {
		up = p.ly < 0;
		down = p.ly > 0;
	}
	/* Up off the top of the list leaves it for the band above -- the
	 * letters if this tab has them, the tabs otherwise. This is the only
	 * way out of the body, so it has to work from an empty list too. */
	if (up && (sel_ < 0 || selectable_row(sel_ - 1, -1) < 0)) {
		focus_ = has_strip() ? FOCUS_STRIP : FOCUS_TABS;
		dirty_ = true;
		return;
	}
	if (down)
		step(+1, 1);
	if (up)
		step(-1, 1);

	/* Triggers page, which is the only thing the shoulders leave them.
	 * The letter groups moved up to the strip, where they are visible. */
	if (pad_repeat(&p, PAD_R2, now))
		step(+1, page);
	if (pad_repeat(&p, PAD_L2, now))
		step(-1, page);

	/*
	 * Left and Right do not change the tab down here -- that belongs to
	 * the bands above, reached with Up.
	 *
	 * On a genre row Right steps INTO the genre, which is not the same
	 * thing: it is descending, the direction B already comes back from,
	 * and it means a genre can be opened without moving the thumb to A.
	 */
	if (sel_ >= 0 && m_.rows[sel_].kind == ROW_GENRE &&
	    pad_repeat(&p, PAD_RIGHT, now)) {
		launch();
		return;
	}
	/* And Left comes back out of one, so the pair reads as one axis:
	 * Right goes deeper, Left goes back. B still does it too. */
	if (!m_.genre_filter.empty() && pad_repeat(&p, PAD_LEFT, now)) {
		leave_genre();
		ensure_visible();
		dirty_ = true;
		return;
	}

	if (sel_ != prev) {
		ensure_visible();
		dirty_ = true;
	}

	if (pad_take_press(&p, PAD_Y)) {
		/* Remember where we were: a search abandoned empty comes
		 * back here rather than to the top of the list. */
		search_from_.clear();
		if (const gnx::Game *g = selected()) {
			search_from_ = g->title_id;
			search_from_sec_ = m_.rows[sel_].section;
		}
		search_ = true;
		ensure_visible();
		dirty_ = true;
	}
	if (pad_take_press(&p, PAD_START)) {
		/* START is now a shortcut to the Settings tab rather than a
		 * second, separate settings screen. Two settings UIs would
		 * drift apart, and only one of them would have the staging,
		 * the defaults and the sub-tabs. */
		m_.tab = TAB_SETTINGS;
		options_page_enter();
		rebuild("", SEC_ALL);
		/* Straight to the rows: START is a shortcut for someone who
		 * already knows what they want to change, and landing them on
		 * the tab strip would charge them two presses for it. */
		focus_ = FOCUS_BODY;
		dirty_ = true;
	}
	/* X opens the info panel on a game. Nothing else on this screen uses
	 * X, and "what actually is this" is the question a list of 585 names
	 * raises most often. */
	if (pad_take_press(&p, PAD_X)) {
		if (const gnx::Game *g = selected()) {
			show_info(*g);
			for (int b = 0; b < PAD_COUNT; b++)
				pad_take_press(&p, (enum pad_button)b);
			dirty_ = true;
			return;
		}
	}
	if (pad_take_press(&p, PAD_A))
		launch();
}

void LibraryScreen::handle_search()
{
	pad &p = p_;
	int now = pad_now_ms();
	int prev = cursor_;
	bool changed = false;

	if (pad_repeat(&p, PAD_UP, now))
		cursor_ = (cursor_ + kGridCols * (kGridRows - 1)) %
			  (kGridCols * kGridRows);
	if (pad_repeat(&p, PAD_DOWN, now))
		cursor_ = (cursor_ + kGridCols) % (kGridCols * kGridRows);
	if (pad_repeat(&p, PAD_LEFT, now))
		cursor_ = cursor_ % kGridCols ? cursor_ - 1 : cursor_ + kGridCols - 1;
	if (pad_repeat(&p, PAD_RIGHT, now))
		cursor_ = cursor_ % kGridCols == kGridCols - 1 ? cursor_ - kGridCols + 1
							     : cursor_ + 1;
	bool close = false;

	if (pad_take_press(&p, PAD_A) && (int)m_.query.size() < kQueryMax) {
		m_.query += kGrid[cursor_];
		changed = true;
	}
	if (pad_take_press(&p, PAD_B)) {
		if (m_.query.empty())
			close = true;
		else
			m_.query.pop_back();
		changed = true;
	}
	if (pad_take_press(&p, PAD_START) || pad_take_press(&p, PAD_Y))
		close = true;

	if (changed && !close)
		rebuild("", SEC_ALL);   /* a new result set: start at the top */
	if (close) {
		search_ = false;
		if (m_.query.empty())
			rebuild(search_from_, search_from_sec_);
		else
			ensure_visible();   /* keep the result that is lit */
		dirty_ = true;
	} else if (cursor_ != prev) {
		dirty_ = true;
	}
}

/* ---- painting ------------------------------------------------------- */

std::string LibraryScreen::tag_of(const gnx::Game &g, bool recent) const
{
	std::string tag;

	if (g.owned)
		tag = "Owned";
	else if (g.in_subscription)
		tag = "Game Pass";
	else if (g.ad_playable)
		tag = "Free with ads";
	/*
	 * Titles that encode at the size we ask for. On this panel that means
	 * no downscale at all, a third of the bitrate and a quarter of the
	 * decode -- a different class of experience -- and it cannot be
	 * guessed from the genre or the publisher, only learned by playing it
	 * once. See docs/RESOLUTION.md.
	 */
	if (native_.count(g.title_id))
		tag += tag.empty() ? "Native 360p" : "  \xC2\xB7  Native 360p";
	/* Not while filtered to that genre: every row would say the same
	 * thing, and the strip above already says it once. */
	if (!g.genre.empty() && g.genre != m_.genre_filter)
		tag += tag.empty() ? g.genre : "  \xC2\xB7  " + g.genre;
	/* A 4.8 from two million and a 4.4 from twenty are different claims,
	 * so the count goes with the score rather than the score alone. */
	if (g.rating > 0 && g.rating_count > 0) {
		char stars[48];
		std::snprintf(stars, sizeof(stars), "%.1f\xE2\x98\x85 (%s)",
			      g.rating, human_count(g.rating_count).c_str());
		tag += tag.empty() ? stars : std::string("  \xC2\xB7  ") + stars;
	}
	if (g.name.empty() && !g.product_id.empty() && names_.pending())
		tag += tag.empty() ? "Looking up the name\xE2\x80\xA6"
				   : "  \xC2\xB7  looking up the name\xE2\x80\xA6";
	(void)recent;
	return tag;
}

void LibraryScreen::paint_header(Painter &pt)
{
	char right[64];

	pt.rect(0, 0, kWidth, kHeaderH, 40);
	pt.line_fit(f_.big, 18, 37, title_.c_str(), 235, 340);

	int pending = names_.pending(), total = names_.total();
	if (pending > 0) {
		std::snprintf(right, sizeof(right), "Naming %d/%d",
			      total - pending, total);
		/* A thin bar along the header's foot, so progress is visible
		 * without a line of text for it. */
		pt.rect(0, kHeaderH - 3, kWidth, 3, 60);
		pt.rect(0, kHeaderH - 3, kWidth * (total - pending) / total, 3,
			200);
	} else if (!m_.query.empty()) {
		std::snprintf(right, sizeof(right), "%d of %zu", m_.matches,
			      m_.games.size());
	} else {
		/* The current letter matters more than the total when you
		 * are jumping through 585 entries by letter group. */
		int pos = 0;
		char letter = ' ';
		if (sel_ >= 0 && m_.rows[sel_].section == SEC_ALL) {
			for (int i = 0; i <= sel_; i++)
				if (m_.rows[i].kind == ROW_GAME &&
				    m_.rows[i].section == SEC_ALL)
					pos++;
			letter = m_.letters[m_.rows[sel_].game];
		}
		if (pos)
			std::snprintf(right, sizeof(right), "%c   %d/%zu", letter,
				      pos, m_.games.size());
		else
			std::snprintf(right, sizeof(right), "%zu games",
				      m_.games.size());
	}
	int w = text_measure(f_.small, right);
	pt.line(f_.small, kWidth - w - 18, 34, right, 170);
	paint_tabs(pt);
	if (letters_showing())
		paint_letters(pt);
	else if (genre_strip_showing())
		paint_genre_strip(pt);
}

/*
 * The letter strip: every first letter that has titles, the current one lit.
 *
 * Laid out across the full width by count rather than at a fixed pitch, so a
 * catalogue missing X and Z spreads the rest out instead of leaving a gap.
 * Twenty-seven slots in 640 px is about 23 px each, which the small face fits
 * comfortably.
 */
/*
 * The genre strip: which genre is filtering the list, and the way out of it.
 * Same band, same highlight and the same Left/Right as every other strip --
 * there is only one direction to go, and it is Left, which is the shallower
 * one everywhere else too.
 */
void LibraryScreen::paint_genre_strip(Painter &pt)
{
	const bool lit = focus_ == FOCUS_STRIP;
	const int y = kListTop;
	const std::string label = "\xE2\x97\x82  " + m_.genre_filter;
	const char *hint = lit ? "Left to leave the genre"
			       : "Up, then Left, to leave";

	pt.rect(0, y, kWidth, kLetterH, lit ? 34 : 20);
	if (lit)
		pt.sel_rect(4, y + 1, text_measure(f_.small, label.c_str()) + 16,
			    kLetterH - 2);
	pt.line(f_.small, 12, y + 16, label.c_str(), lit ? 255 : 205);
	pt.line(f_.small, kWidth - 12 - text_measure(f_.small, hint), y + 16,
		hint, lit ? 190 : 105);
}

void LibraryScreen::paint_letters(Painter &pt)
{
	const auto &tabs = m_.letter_tabs;
	const bool lit = focus_ == FOCUS_STRIP;
	const int y = kListTop;

	pt.rect(0, y, kWidth, kLetterH, lit ? 34 : 20);
	if (tabs.empty())
		return;

	const int slot = kWidth / (int)tabs.size();
	for (size_t i = 0; i < tabs.size(); i++) {
		const bool on = tabs[i] == m_.letter;
		const char text[2] = { tabs[i], 0 };
		const int x = (int)i * slot + (slot - text_measure(f_.small, text)) / 2;

		if (on && lit)
			pt.sel_rect((int)i * slot + 1, y + 1, slot - 2,
				    kLetterH - 2);
		else if (on)
			pt.rect((int)i * slot + 1, y + 1, slot - 2,
				kLetterH - 2, 44);
		pt.line(f_.small, x, y + 16, text,
			on ? (lit ? 255 : 205) : (lit ? 150 : 120));
	}
}

/*
 * The tab strip: the four tabs spread across the width, the current one
 * underlined and brightened, and the whole band lifted while it holds the
 * cursor -- which is the only state in which Left and Right do anything to
 * it, so it had better be visible at a glance.
 *
 * While the reader has stepped into a genre the strip shows that genre and
 * the way back instead of the tabs, because there is nothing to the left or
 * right of where they are standing.
 */
void LibraryScreen::paint_tabs(Painter &pt)
{
	const bool lit = focus_ == FOCUS_TABS;

	pt.rect(0, kTabTop, kWidth, kTabH, lit ? 40 : 26);

	const int slot = kWidth / TAB_COUNT;
	for (int i = 0; i < TAB_COUNT; i++) {
		const bool on = i == m_.tab;
		const int tw = text_measure(f_.small, kTabName[i]);
		const int x = i * slot + (slot - tw) / 2;

		/*
		 * The green selection bar itself when the cursor is up here,
		 * a flat grey block when it is not. Two earlier attempts got
		 * this wrong: an underline, which reads as a mark under a
		 * word rather than as the cursor standing on it, and then a
		 * grey block, which is a different marking rather than the
		 * same one moved. What someone holding this expects is the
		 * green bar off the top of the list arriving on Home, so
		 * that is what it does.
		 */
		if (on && lit)
			pt.sel_rect(i * slot + 6, kTabTop + 3, slot - 12,
				    kTabH - 6);
		else if (on)
			pt.rect(i * slot + 6, kTabTop + 3, slot - 12,
				kTabH - 6, 44);
		pt.line(f_.small, x, kTabTop + 19, kTabName[i],
			on ? (lit ? 255 : 205) : (lit ? 150 : 110));
	}
}

/*
 * A row's title, scrolled if it is the selected one and does not fit.
 *
 * Dwell, slide, dwell, snap back. The dwell at each end is what makes it
 * readable rather than annoying: a name that fits in a glance is never moving
 * while you glance at it, and the end of a long one stays still long enough
 * to read before it returns. Only the selected row ever moves -- six titles
 * crawling at once would be a screen nobody can read anything on.
 */
void LibraryScreen::draw_title(Painter &pt, int row, int x, int y,
			       const char *s, uint8_t level, int max_w)
{
	constexpr int kDwellMs = 1000;    /* before it starts, and at the end */
	constexpr int kPxPerSec = 34;
	const int width = text_measure(f_.mid, s);
	const int over = width - max_w;

	if (row != sel_ || focus_ != FOCUS_BODY || over <= 0) {
		pt.line_fit(f_.mid, x, y, s, level, max_w);
		return;
	}
	if (marq_row_ != row) {
		marq_row_ = row;
		marq_at_ = pad_now_ms();
	}

	const int slide_ms = over * 1000 / kPxPerSec;
	const int cycle = kDwellMs + slide_ms + kDwellMs;
	int t = pad_now_ms() - marq_at_;
	int shift;

	if (t < 0 || t > cycle)          /* wrapped, or the clock jumped */
		marq_at_ = pad_now_ms(), t = 0;
	if (t < kDwellMs)
		shift = 0;
	else if (t < kDwellMs + slide_ms)
		shift = (t - kDwellMs) * kPxPerSec / 1000;
	else
		shift = over;
	if (shift > over)
		shift = over;
	if (t >= cycle)
		marq_at_ = pad_now_ms();

	/* Repainting this at 60 Hz to move a pixel and a half is not worth a
	 * quarter of a core on a device this size; the loop is told to come
	 * back, but only every kTickMs. */
	marq_live_ = true;
	pt.line_window(f_.mid, x - shift, y, s, level, x, max_w);
}

void LibraryScreen::paint_rows(Painter &pt)
{
	marq_live_ = false;
	const int bottom = list_bottom();
	int y = list_top();
	uint8_t hy, hcb, hcr;

	/*
	 * The green bar is only ever here while the cursor is. When it has
	 * gone up to a strip, what is left behind is a faint breadcrumb of
	 * where Down will put you back -- dark enough to read as "this is
	 * remembered" rather than as a second, competing selection.
	 */
	if (focus_ == FOCUS_BODY)
		screen_rgb_to_ycc(18, 74, 44, &hy, &hcb, &hcr);
	else
		screen_rgb_to_ycc(24, 30, 27, &hy, &hcb, &hcr);

	for (int i = top_; i < (int)m_.rows.size(); i++) {
		const Row &r = m_.rows[i];
		int h = row_height(r);

		if (y + h > bottom)
			break;
		if (r.kind == ROW_SECTION) {
			pt.line(f_.small, 16, y + 18, r.label.c_str(), 150);
			pt.rect(16, y + 23, kWidth - 32, 1, 70);
			y += h;
			continue;
		}
		if (r.kind == ROW_TOOL) {
			const bool on = i == sel_ && focus_ == FOCUS_BODY;

			if (i == sel_)
				pt.rect_colour(6, y, kWidth - 12, kRowH, hy,
					       hcb, hcr);
			pt.line(f_.mid, kTextX, y + 24, kToolName[r.game],
				on ? 235 : 190);
			pt.line_fit(f_.small, kTextX, y + 46,
				    kToolHelp[r.game], on ? 175 : 115,
				    kWidth - kTextX - 16);
			y += h;
			continue;
		}
		if (r.kind == ROW_GENRE) {
			const auto &g = m_.genres[r.game];
			const bool on = i == sel_;
			char count[24];

			if (on)
				pt.rect_colour(6, y, kWidth - 12, kRowH, hy,
					       hcb, hcr);
			pt.line_fit(f_.mid, kTextX, y + 26, g.first.c_str(),
				    on ? 235 : 190, kWidth - kTextX - 90);
			std::snprintf(count, sizeof(count), "%d", g.second);
			pt.line(f_.small, kWidth - 16 -
						  text_measure(f_.small, count),
				y + 30, count, on ? 175 : 115);
			y += h;
			continue;
		}
		if (r.kind == ROW_NOTE) {
			/* Indented to the text column so it reads as content
			 * of the section above rather than another heading,
			 * and dimmer because it is not selectable. */
			pt.line(f_.small, kTextX, y + 17, r.label.c_str(), 105);
			y += h;
			continue;
		}

		const bool on = i == sel_;

		if (r.kind == ROW_CONSOLE) {
			const gnx::HomeConsole &c = m_.consoles[r.game];

			if (on)
				pt.rect_colour(6, y, kWidth - 12, kRowH, hy,
					       hcb, hcr);
			/* No box art for a console; a slab keeps the text
			 * column aligned with the games below it. */
			pt.rect(kArtX + 1, y + 2, kArtW - 2, kArtH, on ? 70 : 34);
			pt.line_fit(f_.mid, kTextX, y + 26,
				    console_label(c).c_str(), on ? 235 : 190,
				    kWidth - kTextX - 16);
			/* Power state is worth showing: from standby the
			 * first connection has to wake the console and takes
			 * appreciably longer. */
			std::string tag =
				c.power_state == "On"      ? "Ready"
				: c.power_state.empty()    ? "Remote play"
							   : "Asleep, will wake";
			pt.line_fit(f_.small, kTextX, y + 48, tag.c_str(),
				    on ? 175 : 115, kWidth - kTextX - 16);
			y += h;
			continue;
		}

		const gnx::Game &g = m_.games[r.game];

		if (on)
			pt.rect_colour(6, y, kWidth - 12, kRowH, hy, hcb, hcr);

		/* Art, or a slab where it will go; the blit carries chroma. */
		const Thumb *t = thumbs_.get(g.product_id, g.box_art_url);
		const int ay = y + 2;
		if (t && t->ok()) {
			int ox = ((kArtW - t->w) / 2) & ~1;
			int oy = ((kArtH - t->h) / 2) & ~1;
			pt.blit_nv12(kArtX + ox, ay + oy, t->luma.data(),
				     t->chroma.data(), t->w, t->w, t->h);
		} else {
			pt.rect(kArtX + 1, ay, kArtW - 2, kArtH, on ? 70 : 34);
		}

		draw_title(pt, i, kTextX, y + 26, m_.labels[r.game].c_str(),
			   on ? 235 : 190, kWidth - kTextX - 16);
		std::string tag = tag_of(g, r.section == SEC_RECENT);
		if (!tag.empty())
			pt.line_fit(f_.small, kTextX, y + 48, tag.c_str(),
				    on ? 175 : 115, kWidth - kTextX - 16);
		y += h;
	}
}

/*
 * The footer bar: the hints, and the build in the corner.
 *
 * The version is there because "am I even running the thing you just sent
 * me" is otherwise unanswerable with the handheld in your hands and the log
 * on another machine. It is right-aligned and the hints are centred in what
 * is left over rather than across the whole width, so a long hint line
 * cannot run underneath it -- on a 640 px panel the settings hints already
 * reach within ~60 px of both edges.
 */
void LibraryScreen::paint_hints(Painter &pt, const char *hints, uint8_t level)
{
	const int hw = text_measure(f_.small, hints);
	std::string ver = g_version;
	int vw = text_measure(f_.small, ver.c_str());

	pt.rect(0, kListBottom, kWidth, kFooterH, 28);

	/*
	 * The hints win the width; the version is omitted rather than drawn
	 * over them. Measured in the small face: the list hints are 538 px of
	 * a 640 px panel and the settings hints 557, so there is room for
	 * about 70 px in the corner and no more.
	 *
	 * Which is why a working build shows its BUILD TIME instead of its
	 * tag. `git describe --dirty` gives every uncommitted build the same
	 * "v1.0.0-rc1-dirty" -- 123 px that does not fit and, worse, is
	 * identical across every push in an afternoon, so it cannot answer
	 * the only question it is there for: is this the binary I was just
	 * sent. The compile time changes every build and is a third of the
	 * width. A clean build keeps its tag, which is what a release needs
	 * to say.
	 *
	 * Trimming the string was tried and is wrong: chopping "-dirty" off
	 * the end makes a working build claim to be the release.
	 */
	if (ver.find("-dirty") != std::string::npos) {
		const std::string t = g_build_time;   /* "HH:MM:SS" */

		ver = "dev " + (t.size() >= 5 ? t.substr(0, 5) : t);
		vw = text_measure(f_.small, ver.c_str());
	}
	if (hw + vw + 14 > kWidth) {
		ver.clear();
		vw = 0;
	}
	if (!ver.empty())
		pt.line(f_.small, kWidth - vw - 6, kHeight - 8, ver.c_str(),
			95);

	const int avail = vw ? kWidth - (vw + 12) : kWidth;
	pt.line(f_.small, hw < avail ? (avail - hw) / 2 : 4, kHeight - 8,
		hints, level);
}

void LibraryScreen::paint_footer(Painter &pt)
{
	char hints[200];

	/*
	 * The hints follow the cursor, not the tab. A footer that lists what
	 * the list does while the cursor is up on the tabs is worse than no
	 * footer: it names buttons that will not do that from here.
	 *
	 * The letters are what is PRINTED on the shell for each function
	 * (pad_button_label), so the hint matches the thumb.
	 */
	if (focus_ == FOCUS_TABS) {
		if (!m_.genre_filter.empty())
			std::snprintf(hints, sizeof(hints),
				      "Left Back to genres     Down Into the list"
				      "     %s Back",
				      pad_button_label(&p_, PAD_B));
		else
			std::snprintf(hints, sizeof(hints),
				      "Left/Right Choose     Down or %s Open"
				      "     %s %s",
				      pad_button_label(&p_, PAD_A),
				      pad_button_label(&p_, PAD_B),
				      m_.tab == TAB_SETTINGS
					      ? (options_page_unsaved()
							 ? "Discard"
							 : "Back")
				      : m_.query.empty() ? "Quit"
							 : "Clear search");
		paint_hints(pt, hints, 170);
		return;
	}
	if (focus_ == FOCUS_STRIP) {
		std::snprintf(hints, sizeof(hints),
			      "Left/Right %s     Down or %s Into the list"
			      "     Up Tabs",
			      m_.tab == TAB_SETTINGS ? "Page" : "Letter",
			      pad_button_label(&p_, PAD_A));
		paint_hints(pt, hints, 170);
		return;
	}
	if (m_.tab == TAB_SETTINGS) {
		/* Nothing here searches or jumps a letter, and offering it
		 * would be advertising buttons that do nothing. */
		if (options_page_explaining())
			std::snprintf(hints, sizeof(hints), "%s Close",
				      pad_button_label(&p_, PAD_B));
		else
			std::snprintf(hints, sizeof(hints),
				      "Left/Right Change  %s Select  %s Explain"
				      "  %s %s  L1/R1 - Tab L/R",
				      pad_button_label(&p_, PAD_A),
				      pad_button_label(&p_, PAD_Y),
				      pad_button_label(&p_, PAD_B),
				      options_page_unsaved() ? "Discard" : "Back");
		paint_hints(pt, hints, options_page_unsaved() ? 200 : 150);
		return;
	}
	std::snprintf(hints, sizeof(hints),
		      "%s Play  %s %s  %s Search  %s Info  L1/R1 - Tab L/R  "
		      "L2/R2 Page Up/Down",
		      pad_button_label(&p_, PAD_A), pad_button_label(&p_, PAD_B),
		      /* Say what B does HERE. It unwinds one level at a time,
		       * so promising "Quit" while inside a genre or a search
		       * is simply wrong -- and on Settings it throws away
		       * staged edits, which is worth naming. */
		      m_.tab == TAB_SETTINGS
			      ? (options_page_unsaved() ? "Discard" : "Back")
		      : !m_.genre_filter.empty() ? "Back"
		      : m_.query.empty()	 ? "Quit"
					   : "Clear search",
		      pad_button_label(&p_, PAD_Y),
		      pad_button_label(&p_, PAD_X));
	paint_hints(pt, hints, 150);
}

void LibraryScreen::paint_search(Painter &pt)
{
	const int y0 = kSearchTop;
	const int cell_w = 42, cell_h = 36;
	const int gx = (kWidth - kGridCols * cell_w) / 2;
	const int gy = y0 + 50;
	char buf[64];
	uint8_t hy, hcb, hcr;

	screen_rgb_to_ycc(18, 74, 44, &hy, &hcb, &hcr);
	pt.rect(0, y0, kWidth, kHeight - y0, 30);
	pt.rect(0, y0, kWidth, 2, 120);

	std::string q = "Search: " + m_.query + "_";
	pt.line(f_.mid, 20, y0 + 32, q.c_str(), 235);
	std::snprintf(buf, sizeof(buf), "%d of %zu", m_.matches,
		      m_.games.size());
	int w = text_measure(f_.small, buf);
	pt.line(f_.small, kWidth - w - 20, y0 + 32, buf, 170);

	for (int i = 0; i < kGridCols * kGridRows; i++) {
		int cx = gx + (i % kGridCols) * cell_w;
		int cy = gy + (i / kGridCols) * cell_h;
		char s[2] = { kGrid[i], 0 };
		int cw = text_measure(f_.mid, s);

		if (i == cursor_)
			pt.rect_colour(cx + 2, cy + 2, cell_w - 4, cell_h - 4, hy,
				       hcb, hcr);
		pt.line(f_.mid, cx + (cell_w - cw) / 2, cy + 26, s,
			i == cursor_ ? 235 : 180);
	}

	std::snprintf(buf, sizeof(buf), "%s Add     %s %s     Start Close",
		      pad_button_label(&p_, PAD_A), pad_button_label(&p_, PAD_B),
		      m_.query.empty() ? "Close" : "Delete");
	pt.centred(f_.small, kHeight - 8, buf, 150);
}

void LibraryScreen::paint()
{
	double t0 = now_ms();

	thumbs_.tick();
	Painter pt(out_, f_);
	paint_header(pt);
	if (m_.tab == TAB_SETTINGS)
		options_page_draw(pt, f_, kListTop, kListBottom,
				  focus_ == FOCUS_BODY    ? OPT_FOCUS_BODY
				  : focus_ == FOCUS_STRIP ? OPT_FOCUS_GROUP
							  : OPT_FOCUS_NONE);
	else
		paint_rows(pt);
	if (search_)
		paint_search(pt);
	else
		paint_footer(pt);

	/* Measured before the flip: the flip waits for vblank and would
	 * hide the painting time behind up to 16 ms of sleep. */
	double ms = now_ms() - t0;
	paints_++;
	paint_sum_ += ms;
	if (ms > paint_max_)
		paint_max_ = ms;
	if (ms > 10)
		std::fprintf(stderr, "library: slow repaint, %.1f ms\n", ms);
	pt.present();
}

/*
 * Resolve the whole catalogue before showing it, with a progress screen.
 *
 * A library that fills in over two minutes while you are reading it is worse
 * than one that makes you wait for it once: rows reorder under the cursor as
 * names arrive, and the genre tab is a lie until the last batch lands. So the
 * wait is taken up front.
 *
 * It is NOT taken on every launch. The Store's Details template is the only
 * one carrying Category, and it weighs about 80 KB a title -- roughly 47 MB
 * and two minutes for 585 of them on this Wi-Fi. A title's genre does not
 * change, so paying that again on a launch where nothing is missing would be
 * pure waste; names.json makes the second launch instant.
 *
 * B skips it. Two minutes is a long time to stand between someone and the
 * game they came to play, and everything still resolves in the background
 * afterwards exactly as it used to.
 */
void LibraryScreen::wait_for_names()
{
	bool waited = false;

	while (!g_stop && !pad_script_done(&p_) && names_.pending() > 0) {
		const int total = names_.total();
		const int done = total - names_.pending();
		char line[96];

		waited = true;
		{
			Painter pt(out_, f_);
			pt.header(title_.c_str());
			pt.centred(f_.mid, 200, "Loading your library", 235);
			std::snprintf(line, sizeof(line), "%d of %d", done,
				      total);
			pt.centred(f_.small, 232, line, 175);
			/* A bar, because "271 of 585" says nothing about how
			 * long is left until you have watched it move. */
			const int w = kWidth - 160;
			pt.rect(80, 250, w, 6, 40);
			if (total > 0)
				pt.rect(80, 250, w * done / total, 6, 180);
			pt.footer("B to skip and let it finish in the background");
			pt.present();
		}
		pad_poll(&p_, 100);
		if (pad_take_press(&p_, PAD_B))
			break;
		if (quit_combo(p_))
			return;
		names_.pump(m_.games);
	}
	/*
	 * Now that nothing is missing, keep what we have honest. Ratings move
	 * and names occasionally change, so a slice of the oldest entries is
	 * queued for a background re-fetch: 40 a launch against a fortnight's
	 * age walks the whole 585 through in a couple of weeks of use, at
	 * about 3 MB a launch instead of 47. Nothing waits on it.
	 */
	names_.request_refresh(m_.games, kRefreshAgeDays, kRefreshPerLaunch);

	if (!waited)
		return;
	/* Names arrived while we watched: sort into the new order and index
	 * the genres they brought before the first paint. */
	names_.pump(m_.games);
	m_.sort();
	reindex();
	rebuild("", SEC_ALL);
}

void LibraryScreen::run()
{
	wait_for_names();
	while (!g_stop && !pad_script_done(&p_)) {
		/* Drain the background workers. Each hand-off is a swap
		 * under a mutex; none of them blocks on I/O. */
		if (names_.pump(m_.games))
			resort();
		if (pump_refresh())
			dirty_ = true;
		if (thumbs_.pump())
			dirty_ = true;
		int pending = names_.pending();
		if (pending != last_pending_) {
			last_pending_ = pending;
			dirty_ = true;
		}

		/*
		 * A sliding title is the one thing here that changes without
		 * anyone pressing anything, so it asks for its own repaints
		 * -- at 25 Hz, not the loop's 60. Moving a pixel and a half
		 * more often than that is not worth the extra repaints on a
		 * device where one costs a couple of milliseconds.
		 */
		constexpr int kTickMs = 40;
		if (marq_live_ && pad_now_ms() - marq_paint_ >= kTickMs)
			dirty_ = true;

		if (dirty_) {
			marq_paint_ = pad_now_ms();
			paint();
			dirty_ = false;
		}

		/* Short timeout, not 200 ms: pad_repeat is driven off the
		 * wall clock, so the loop has to come round often enough to
		 * notice a repeat tick is due. */
		pad_poll(&p_, 16);
		int now = pad_now_ms();

		if (search_) {
			handle_search();
			continue;
		}
		if (pad_take_press(&p_, PAD_B)) {
			/* B unwinds one level at a time: out of a genre
			 * first, then out of a search, and only then out of
			 * the screen. Handled here rather than in
			 * handle_list() because this is where B is consumed;
			 * a second handler further down would never see it. */
			if (m_.tab == TAB_SETTINGS) {
				/* Discards whatever was staged; the footer
				 * says so while there is anything to lose. */
				m_.tab = TAB_HOME;
				rebuild("", SEC_ALL);
				/* Onto the tabs, not back into a list: B here
				 * means "out of this", and landing on the
				 * strip shows where "out" put you. It also
				 * keeps the cursor off a band Home does not
				 * have -- the settings sub-tabs. */
				focus_ = FOCUS_TABS;
				dirty_ = true;
				continue;
			}
			if (!m_.genre_filter.empty()) {
				leave_genre();
				ensure_visible();
				dirty_ = true;
				continue;
			}
			if (m_.query.empty())
				return;
			/* Clear the search but stay on the game that was lit,
			 * now in its place in the full list. */
			std::string keep;
			if (const gnx::Game *g = selected())
				keep = g->title_id;
			m_.query.clear();
			rebuild(keep, SEC_ALL);
			continue;
		}
		handle_list(now);
	}
}

}  // namespace

void library(drm_out &out, Fonts &f, pad &p, gnx::Http &http,
	     gnx::XboxAuth &auth, std::vector<gnx::Game> &games,
	     const gnx::XboxProfile &profile, bool refresh)
{
	(void)http;   /* every fetch runs on a worker with its own Http */
	LibraryScreen screen(out, f, p, auth, games, profile, refresh);
	screen.run();
}

/* ---- -fake-catalog ----------------------------------------------------- */

namespace {

/*
 * Synthetic titles for the test bench: real product ids so name and art
 * resolution can be watched (the first six resolve; the seventh is a Store
 * miss and the eighth has no product id at all), names with the marks and
 * accents the Store hands back, "The " prefixes, digits, and a few owned or
 * ad-supported so every tag has a case.
 */
struct FakeTitle {
	const char *id, *name, *product;
	int flags;   /* 1 owned, 2 ad-supported only */
};

const FakeTitle kFake[] = {
	{ "FORZAHORIZON5", "", "9NKX70BBCDRN", 1 },
	{ "HALOINFINITE", "", "9PP5G1F0C2B6", 0 },
	{ "SEAOFTHIEVES", "", "9P2N57MC619K", 0 },
	{ "MINECRAFT", "", "9NBLGGH2JHXJ", 1 },
	{ "ORIANDTHEWILLOFTHEWISPS", "", "9N8CD0XZKLP4", 0 },
	{ "MICROSOFTFLIGHTSIMULATOR", "", "9PMQDM08SNK9", 0 },
	{ "ASSASSINSCREEDSHADOWS", "", "9ZZZZZZZZZZ1", 0 },
	{ "WATCH_DOGS", "", "", 0 },
	{ "THEELDERSCROLLSVSKYRIMSPECIALEDITION",
	  "The Elder Scrolls V: Skyrim Special Edition", "", 1 },
	{ "THEWITCHER3WILDHUNT",
	  "The Witcher 3: Wild Hunt \xE2\x80\x93 Complete Edition", "", 0 },
	{ "OKAMIHD", "\xC5\x8Ckami HD \xE5\xA4\xA7\xE7\xA5\x9E", "", 0 },
	{ "ABZU", "ABZ\xC3\x9B", "", 0 },
	{ "SENUASSAGAHELLBLADEII", "Senua's Saga: Hellblade II", "", 0 },
	{ "NIERAUTOMATA", "NieR:Automata\xE2\x84\xA2", "", 0 },
	{ "DRAGONQUESTXIS",
	  "DRAGON QUEST\xC2\xAE XI S: Echoes of an Elusive Age \xE2\x80\x93 "
	  "Definitive Edition", "", 0 },
	{ "TETRISEFFECTCONNECTED", "Tetris\xC2\xAE Effect: Connected", "", 0 },
	{ "LEGOSTARWARSTHESKYWALKERSAGA",
	  "LEGO\xC2\xAE Star Wars\xE2\x84\xA2: The Skywalker Saga", "", 0 },
	{ "CRASHBANDICOOTNSANETRILOGY",
	  "Crash Bandicoot\xE2\x84\xA2 N. Sane Trilogy", "", 1 },
	{ "DISHONORED2", "Dishonored\xC2\xAE 2", "", 0 },
	{ "INDIANAJONESANDTHEGREATCIRCLE",
	  "Indiana Jones and the Great Circle\xE2\x84\xA2", "", 0 },
	{ "STARWARSJEDISURVIVOR", "STAR WARS Jedi: Survivor\xE2\x84\xA2", "", 0 },
	{ "KUNITSUGAMI", "Kunitsu-Gami: Path of the Goddess", "", 0 },
	{ "HIFIRUSH", "Hi-Fi RUSH", "", 0 },
	{ "CLAIROBSCUREXPEDITION33", "Clair Obscur: Expedition 33", "", 0 },
	{ "7DAYSTODIE", "7 Days to Die", "", 0 },
	{ "112OPERATOR", "112 Operator", "", 0 },
	{ "BALATRO", "Balatro", "", 0 },
	{ "PALWORLD", "Palworld", "", 2 },
	{ "NINJAGAIDEN2BLACK", "NINJA GAIDEN 2 Black", "", 0 },
	{ "PERSONA5ROYAL", "Persona 5 Royal", "", 0 },
	{ "YAKUZA0", "Yakuza 0", "", 0 },
	{ "WRECKFEST", "Wreckfest", "", 1 },
	{ "GEARS5", "Gears 5", "", 0 },
	{ "PSYCHONAUTS2", "Psychonauts 2", "", 0 },
	{ "GROUNDED", "Grounded", "", 0 },
	{ "STATEOFDECAY2", "State of Decay 2: Juggernaut Edition", "", 0 },
	{ "STARFIELD", "Starfield", "", 0 },
	{ "FALLOUT76", "Fallout 76", "", 2 },
	{ "DOOMETERNAL", "DOOM Eternal", "", 0 },
	{ "QUAKEII", "Quake II", "", 0 },
	{ "PREY", "Prey", "", 0 },
	{ "AVOWED", "Avowed", "", 0 },
	{ "SOUTHOFMIDNIGHT", "South of Midnight", "", 0 },
	{ "THEGUNK", "The Gunk", "", 0 },
	{ "THEMEDIUM", "The Medium", "", 0 },
	{ "THEASCENT", "The Ascent", "", 0 },
	{ "THEOUTERWORLDS", "The Outer Worlds: Spacer's Choice Edition", "", 0 },
	{ "AMONGUS", "Among Us", "", 2 },
	{ "CELESTE", "Celeste", "", 0 },
	{ "HADES", "Hades", "", 0 },
	{ "PENTIMENT", "Pentiment", "", 0 },
	{ "GRIS", "GRIS", "", 0 },
	{ "MONSTERHUNTERRISE", "MONSTER HUNTER RISE", "", 0 },
	{ "OCTOPATHTRAVELERII", "OCTOPATH TRAVELER II", "", 0 },
	{ "LIESOFP", "Lies of P", "", 0 },
	{ "ZOOTYCOONULTIMATEANIMALCOLLECTION",
	  "Zoo Tycoon: Ultimate Animal Collection", "", 0 },
	{ "CITIESSKYLINES", "Cities: Skylines - Remastered", "", 0 },
	{ "AGEOFEMPIRESIV", "Age of Empires IV: Anniversary Edition", "", 0 },
	{ "FINALFANTASYXIV", "FINAL FANTASY XIV Online", "", 0 },
	{ "ELDENRING", "ELDEN RING", "", 0 },
};

}  // namespace

void fake_library(drm_out &out, Fonts &f, pad &p, int count)
{
	const int known = (int)(sizeof(kFake) / sizeof(kFake[0]));
	std::vector<gnx::Game> games;

	for (int i = 0; i < count; i++) {
		gnx::Game g;
		int flags = 0;

		if (i < known) {
			g.title_id = kFake[i].id;
			g.name = kFake[i].name;
			g.product_id = kFake[i].product;
			flags = kFake[i].flags;
		} else {
			char buf[64];
			std::snprintf(buf, sizeof(buf), "SYNTHETICTITLE%d", i + 1);
			g.title_id = buf;
			std::snprintf(buf, sizeof(buf), "Synthetic Title %d", i + 1);
			g.name = buf;
		}
		g.owned = flags & 1;
		if (flags & 2) {
			g.ad_supported = g.ad_granted = g.ad_playable = true;
			g.max_session_secs = 3600;
		} else {
			g.subscription = g.in_subscription = true;
		}
		games.push_back(std::move(g));
	}

	/* Own caches: a synthetic catalog.json must never be what a real
	 * session loads, and a real names.json is no use to these ids. */
	g_opts.state_dir += "/fake";
	mkdir(g_opts.state_dir.c_str(), 0755);

	/* The account path cannot run here, so the catalog cache gets its
	 * round trip in the test bench instead: what is written must read
	 * back as the same list. */
	{
		std::vector<gnx::Game> back;
		save_catalog_cache(games);
		bool same = load_catalog_cache(back) &&
			    catalog_signature(back) == catalog_signature(games);
		std::fprintf(stderr, "library: catalog.json round trip %s\n",
			     same ? "OK" : "FAILED");
	}

	gnx::Http http;
	gnx::XboxAuth auth(g_opts.state_dir + "/tokens.json");
	gnx::XboxProfile profile;

	http.set_abort_flag(&g_abort);
	profile.gamertag = "Simulator";
	std::fprintf(stderr, "library: fake catalog of %d titles\n", count);
	library(out, f, p, http, auth, games, profile, false);
}

}  // namespace app
