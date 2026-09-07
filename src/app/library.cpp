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
enum RowKind { ROW_SECTION, ROW_GAME, ROW_CONSOLE, ROW_NOTE, ROW_GENRE };
enum Section { SEC_CONSOLE, SEC_RECENT, SEC_ALL };

/*
 * Top-level tabs, changed with Left/Right (the only D-pad axis the list was
 * not already using).
 *
 * Tabs rather than a sidebar because of the panel: 640x480, and a row already
 * carries box art plus a title plus a metadata line. A sidebar wide enough to
 * read costs ~150 px, a quarter of the width, out of exactly the space the
 * titles need; the tab strip costs 28 px of height and still leaves six rows.
 */
enum Tab { TAB_HOME, TAB_GENRES, TAB_ALL, TAB_SETTINGS, TAB_COUNT };

const char *const kTabName[TAB_COUNT] = { "Home", "Genres", "All games",
					  "Settings" };

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
		rows.push_back({ ROW_NOTE, SEC_ALL, -1,
				 "Left and Right for genres and all games" });
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
		return kListTop + (letters_showing() ? kLetterH : 0);
	}
	/* The letter strip belongs to the flat A-Z list and nothing else: not
	 * to a search (which spans every letter), not inside a genre (whose
	 * list is already short), not on Home or Genres. */
	bool letters_showing() const
	{
		return !search_ && m_.tab == TAB_ALL && m_.genre_filter.empty();
	}
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
	int next_letter_row(int dir) const;
	void ensure_visible();
	void wait_for_names();
	void reindex();
	void rebuild(const std::string &keep_id, Section keep_sec);
	void resort();
	bool pump_refresh();
	void launch();
	void leave_genre();

	/* ---- input ---- */
	void handle_list(int now);
	void handle_search();

	/* ---- painting ---- */
	void paint();
	void paint_header(Painter &pt);
	void paint_tabs(Painter &pt);
	void paint_letters(Painter &pt);
	void paint_rows(Painter &pt);
	void paint_footer(Painter &pt);
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
		    m_.rows[i].kind == ROW_GENRE)
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

/*
 * First game row of the next (dir=+1) or previous (dir=-1) letter group,
 * over the ALL section. From the recents, R1 goes to the top of the list.
 * Going back means the START of the previous group, not its end, so step
 * over this group first and then to the top of that one.
 */
int LibraryScreen::next_letter_row(int dir) const
{
	const std::vector<Row> &rows = m_.rows;
	auto letter = [&](int i) { return m_.letters[rows[i].game]; };
	auto in_all = [&](int i) {
		return rows[i].kind == ROW_GAME && rows[i].section == SEC_ALL;
	};

	if (sel_ < 0)
		return sel_;
	if (!in_all(sel_)) {
		for (int i = 0; i < (int)rows.size(); i++)
			if (in_all(i))
				return dir > 0 ? i : sel_;
		return sel_;
	}
	char here = letter(sel_);
	if (dir > 0) {
		int last = sel_;
		for (int i = sel_ + 1; i < (int)rows.size(); i++) {
			if (!in_all(i))
				continue;
			if (letter(i) != here)
				return i;
			last = i;
		}
		return last;
	}
	int i = sel_;
	while (i > 0 && in_all(i - 1) && letter(i - 1) == here)
		i--;
	if (i == 0 || !in_all(i - 1))
		return i;
	char prev = letter(i - 1);
	i--;
	while (i > 0 && in_all(i - 1) && letter(i - 1) == prev)
		i--;
	return i;
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

void LibraryScreen::launch()
{
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

void LibraryScreen::handle_list(int now)
{
	pad &p = p_;
	int prev = sel_;

	/*
	 * Settings owns the whole body while it is showing, including Left
	 * and Right (which cycle a value there, as they do in every settings
	 * list) and the shoulders (its two sub-tabs). That leaves no gesture
	 * for changing the primary tab, which is why B leaves this one --
	 * the same unwind that leaves a genre.
	 */
	if (m_.tab == TAB_SETTINGS) {
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
	if (down)
		step(+1, 1);
	if (up)
		step(-1, 1);

	/*
	 * Shoulders move a whole letter; with 585 entries that is the
	 * difference between finding a game and scrolling for a minute.
	 * Triggers page.
	 *
	 * On the flat list that now means changing the letter GROUP rather
	 * than jumping within one long list -- the same gesture doing the
	 * same thing, with the strip above finally showing where it lands.
	 * Everywhere else (a genre, a search) there are no groups, so it
	 * falls back to the jump.
	 */
	if (letters_showing() && !m_.letter_tabs.empty()) {
		int step = 0;
		if (pad_repeat(&p, PAD_R1, now))
			step = +1;
		if (pad_repeat(&p, PAD_L1, now))
			step = -1;
		if (step) {
			const auto &tabs = m_.letter_tabs;
			int at = 0;
			for (size_t i = 0; i < tabs.size(); i++)
				if (tabs[i] == m_.letter)
					at = (int)i;
			m_.letter = tabs[(at + (int)tabs.size() + step) %
					 tabs.size()];
			rebuild("", SEC_ALL);
			sel_ = selectable_row(0, +1);
			top_ = 0;
			ensure_visible();
			dirty_ = true;
			return;
		}
	} else {
		if (pad_repeat(&p, PAD_R1, now))
			sel_ = next_letter_row(+1);
		if (pad_repeat(&p, PAD_L1, now))
			sel_ = next_letter_row(-1);
	}
	if (pad_repeat(&p, PAD_R2, now))
		step(+1, page);
	if (pad_repeat(&p, PAD_L2, now))
		step(-1, page);

	/*
	 * Left/Right change tab. They were the only D-pad axis the list did
	 * not already use, which is what makes tabs cheap here: no existing
	 * gesture had to move.
	 *
	 * Inside a genre they step back out instead, so Left is always "less
	 * deep" and the reader never has to remember which button undoes what.
	 */
	int tab_step = 0;
	if (pad_repeat(&p, PAD_LEFT, now))
		tab_step = -1;
	if (pad_repeat(&p, PAD_RIGHT, now))
		tab_step = +1;
	if (tab_step) {
		if (!m_.genre_filter.empty()) {
			leave_genre();
		} else {
			m_.tab = (Tab)((m_.tab + TAB_COUNT + tab_step) %
					TAB_COUNT);
			if (m_.tab == TAB_SETTINGS)
				options_page_enter();
			rebuild("", SEC_ALL);
			sel_ = selectable_row(0, +1);
			top_ = 0;
		}
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
		dirty_ = true;
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
		 * are jumping through 585 entries with L1/R1. */
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
}

/*
 * The letter strip: every first letter that has titles, the current one lit.
 *
 * Laid out across the full width by count rather than at a fixed pitch, so a
 * catalogue missing X and Z spreads the rest out instead of leaving a gap.
 * Twenty-seven slots in 640 px is about 23 px each, which the small face fits
 * comfortably.
 */
void LibraryScreen::paint_letters(Painter &pt)
{
	const auto &tabs = m_.letter_tabs;
	const int y = kListTop;

	pt.rect(0, y, kWidth, kLetterH, 20);
	if (tabs.empty())
		return;

	const int slot = kWidth / (int)tabs.size();
	for (size_t i = 0; i < tabs.size(); i++) {
		const bool on = tabs[i] == m_.letter;
		const char text[2] = { tabs[i], 0 };
		const int x = (int)i * slot + (slot - text_measure(f_.small, text)) / 2;

		if (on)
			pt.rect((int)i * slot + 1, y + 1, slot - 2,
				kLetterH - 2, 62);
		pt.line(f_.small, x, y + 16, text, on ? 240 : 120);
	}
}

/*
 * The tab strip: the three tabs spread across the width, the current one
 * underlined and brightened.
 *
 * While the reader has stepped into a genre the strip shows that genre and a
 * way back instead of the tabs, because Left/Right mean "back out" there --
 * a strip still offering three tabs would be advertising a gesture that does
 * something else.
 */
void LibraryScreen::paint_tabs(Painter &pt)
{
	pt.rect(0, kTabTop, kWidth, kTabH, 26);

	if (!m_.genre_filter.empty()) {
		pt.line(f_.small, 18, kTabTop + 19,
			("\xE2\x97\x82  " + m_.genre_filter).c_str(), 215);
		const char *hint = "B or Left to go back";
		pt.line(f_.small,
			kWidth - 18 - text_measure(f_.small, hint),
			kTabTop + 19, hint, 120);
		return;
	}

	const int slot = kWidth / TAB_COUNT;
	for (int i = 0; i < TAB_COUNT; i++) {
		const bool on = i == m_.tab;
		const int tw = text_measure(f_.small, kTabName[i]);
		const int x = i * slot + (slot - tw) / 2;

		if (on) {
			/* Underline rather than a filled pill: the strip sits
			 * directly under the header bar and a second block of
			 * colour there reads as a second header. */
			pt.rect(i * slot + 8, kTabTop + kTabH - 3,
				slot - 16, 2, 200);
		}
		pt.line(f_.small, x, kTabTop + 19, kTabName[i], on ? 235 : 120);
	}
}

void LibraryScreen::paint_rows(Painter &pt)
{
	const int bottom = list_bottom();
	int y = list_top();
	uint8_t hy, hcb, hcr;

	/* Selection bar: a dark green, so it reads as "chosen" and not as a
	 * grey band the art sits on. */
	screen_rgb_to_ycc(18, 74, 44, &hy, &hcb, &hcr);

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

		pt.line_fit(f_.mid, kTextX, y + 26, m_.labels[r.game].c_str(),
			    on ? 235 : 190, kWidth - kTextX - 16);
		std::string tag = tag_of(g, r.section == SEC_RECENT);
		if (!tag.empty())
			pt.line_fit(f_.small, kTextX, y + 48, tag.c_str(),
				    on ? 175 : 115, kWidth - kTextX - 16);
		y += h;
	}
}

void LibraryScreen::paint_footer(Painter &pt)
{
	char hints[200];

	/* The letters are what is PRINTED on the shell for each function
	 * (pad_button_label), so the hint matches the thumb. */
	if (m_.tab == TAB_SETTINGS) {
		/* Nothing here searches or jumps a letter, and offering it
		 * would be advertising buttons that do nothing. */
		if (options_page_explaining())
			std::snprintf(hints, sizeof(hints), "%s Close",
				      pad_button_label(&p_, PAD_B));
		else
			std::snprintf(hints, sizeof(hints),
				      "Left/Right Change   %s Select   %s Explain"
				      "   %s %s   L1/R1 %s",
				      pad_button_label(&p_, PAD_A),
				      pad_button_label(&p_, PAD_Y),
				      pad_button_label(&p_, PAD_B),
				      options_page_unsaved() ? "Discard" : "Back",
				      options_page_other_group());
		pt.rect(0, kListBottom, kWidth, kFooterH, 28);
		pt.centred(f_.small, kHeight - 8, hints,
			   options_page_unsaved() ? 200 : 150);
		return;
	}
	std::snprintf(hints, sizeof(hints),
		      "%s Play     %s %s     %s Search     L1/R1 Letter     "
		      "L2/R2 Page",
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
		      pad_button_label(&p_, PAD_Y));
	pt.rect(0, kListBottom, kWidth, kFooterH, 28);
	pt.centred(f_.small, kHeight - 8, hints, 150);
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
		options_page_draw(pt, f_, kListTop, kListBottom);
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

		if (dirty_) {
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
