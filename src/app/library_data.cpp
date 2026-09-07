/*
 * The library's data side. See library_data.hpp.
 */
#include "library_data.hpp"

#include <set>

#include <fstream>
#include <sstream>

#include "../../third_party/nlohmann/json.hpp"

using nlohmann::json;

namespace app {

namespace {

/* How long a "the Store has never heard of this" answer is trusted before we
 * ask again: some products are delisted for good, some are simply not out
 * yet, and asking every launch costs the loading screen a wait for titles
 * that will never resolve. */
constexpr int kMissRetryDays = 30;

/* Days since the epoch. Coarse on purpose: the ageing refresh only ever
 * asks "is this a couple of weeks old", and a day number survives a clock
 * that jumps (this device has no RTC battery) far better than a timestamp. */
int today()
{
	return (int)(std::time(nullptr) / 86400);
}

std::string state_path(const char *leaf)
{
	return g_opts.state_dir + "/" + leaf;
}

/*
 * Write a cache file atomically: a crash or a pulled battery mid-write must
 * not leave a truncated JSON file that the next launch then trusts. The SD
 * card's FAT rename is atomic enough for this.
 */
bool write_file(const std::string &path, const std::string &body)
{
	std::string tmp = path + ".tmp";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		if (!out)
			return false;
		out.write(body.data(), (std::streamsize)body.size());
		if (!out)
			return false;
	}
	if (std::rename(tmp.c_str(), path.c_str())) {
		std::remove(tmp.c_str());
		return false;
	}
	return true;
}

json read_json(const std::string &path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return json();
	return json::parse(in, nullptr, false);
}

/*
 * Fold one code point to the lowercase ASCII letter it sorts as, ' ' for a
 * separator, or 0 to drop it. Covers what game names actually contain:
 * ASCII, Latin-1 (é, ñ, ü...), Latin Extended-A (Ō, ł, ş...). Symbols
 * (™ ® :) are separators so "Hi-Fi RUSH" keys as "hi fi rush"; anything
 * else (kana, the U+FFFD from bad bytes) is dropped.
 */
char fold(uint32_t cp)
{
	if (cp < 128) {
		if (std::isalnum((int)cp))
			return (char)std::tolower((int)cp);
		return ' ';
	}
	if (cp >= 0xC0 && cp <= 0xFF) {
		/* U+00C0..U+00FF; '.' marks × and ÷. */
		static const char latin1[65] =
			"aaaaaaaceeeeiiiidnooooo.ouuuuyts"
			"aaaaaaaceeeeiiiidnooooo.ouuuuyty";
		char c = latin1[cp - 0xC0];
		return c == '.' ? ' ' : c;
	}
	if (cp >= 0x100 && cp <= 0x17F) {
		static const struct { uint32_t last; char c; } ext[] = {
			{ 0x105, 'a' }, { 0x10D, 'c' }, { 0x111, 'd' },
			{ 0x11B, 'e' }, { 0x123, 'g' }, { 0x127, 'h' },
			{ 0x133, 'i' }, { 0x135, 'j' }, { 0x138, 'k' },
			{ 0x142, 'l' }, { 0x14B, 'n' }, { 0x153, 'o' },
			{ 0x159, 'r' }, { 0x161, 's' }, { 0x167, 't' },
			{ 0x173, 'u' }, { 0x175, 'w' }, { 0x178, 'y' },
			{ 0x17E, 'z' }, { 0x17F, 's' },
		};
		for (const auto &e : ext)
			if (cp <= e.last)
				return e.c;
	}
	if (cp >= 0x2000 && cp <= 0x206F)   /* dashes, quotes, ellipsis */
		return ' ';
	return 0;
}

std::string fold_string(const std::string &s)
{
	std::string key;
	const char *p = s.c_str();

	key.reserve(s.size());
	while (*p) {
		char c = fold(text_utf8_next(&p));

		if (!c)
			continue;
		if (c == ' ') {
			if (!key.empty() && key.back() != ' ')
				key += ' ';
			continue;
		}
		key += c;
	}
	while (!key.empty() && key.back() == ' ')
		key.pop_back();
	return key;
}

json game_to_json(const gnx::Game &g)
{
	return json{
		{ "id", g.title_id },
		{ "product", g.product_id },
		{ "owned", g.owned },
		{ "sub", g.subscription },
		{ "insub", g.in_subscription },
		{ "ads", g.ad_supported },
		{ "adg", g.ad_granted },
		{ "adp", g.ad_playable },
		{ "cap", g.max_session_secs },
	};
}

gnx::Game game_from_json(const json &j)
{
	gnx::Game g;

	g.title_id = j.value("id", "");
	g.product_id = j.value("product", "");
	g.owned = j.value("owned", false);
	g.subscription = j.value("sub", false);
	g.in_subscription = j.value("insub", false);
	g.ad_supported = j.value("ads", false);
	g.ad_granted = j.value("adg", false);
	g.ad_playable = j.value("adp", false);
	g.max_session_secs = j.value("cap", 0);
	return g;
}

}  // namespace

std::string sort_key(const gnx::Game &g)
{
	std::string key = fold_string(g.name.empty() ? g.title_id : g.name);

	if (key.size() > 4 && key.compare(0, 4, "the ") == 0)
		key.erase(0, 4);
	return key;
}

char letter_of_key(const std::string &key)
{
	if (key.empty())
		return '#';
	unsigned char c = (unsigned char)key[0];
	return (c >= 'a' && c <= 'z') ? (char)std::toupper(c) : '#';
}

std::string display_name(const gnx::Game &g)
{
	if (!g.name.empty())
		return g.name;
	std::string s = g.title_id;
	for (size_t i = 0; i < s.size(); i++) {
		unsigned char c = (unsigned char)s[i];
		s[i] = (char)(i ? std::tolower(c) : std::toupper(c));
		if (s[i] == '_')
			s[i] = ' ';
	}
	return s;
}

/* ---- catalog.json -------------------------------------------------------- */

/*
 * catalog.json: { "version": 1, "games": [ { "id", "product", "owned",
 * "sub", "insub", "ads", "adg", "adp", "cap" }, ... ] } -- the playable
 * list with every Game flag the launch path reads (needs_ad_offering()
 * decides which offering the session is created against). Names live in
 * names.json, so a catalog refresh never loses them.
 */
bool load_catalog_cache(std::vector<gnx::Game> &games)
{
	json j = read_json(state_path("catalog.json"));

	if (j.is_discarded() || !j.is_object() || j.value("version", 0) != 1)
		return false;
	std::vector<gnx::Game> out;
	for (const json &e : j.value("games", json::array())) {
		gnx::Game g = game_from_json(e);
		if (!g.title_id.empty())
			out.push_back(std::move(g));
	}
	if (out.empty())
		return false;
	games = std::move(out);
	std::fprintf(stderr, "library: %zu titles from catalog.json\n",
		     games.size());
	return true;
}

void save_catalog_cache(const std::vector<gnx::Game> &games)
{
	json list = json::array();
	for (const gnx::Game &g : games)
		list.push_back(game_to_json(g));
	json j{ { "version", 1 }, { "games", std::move(list) } };
	if (!write_file(state_path("catalog.json"), j.dump()))
		std::fprintf(stderr, "library: could not write catalog.json\n");
}

void fetch_playable(gnx::Http &http, gnx::XboxAuth &auth,
		    std::vector<gnx::Game> &playable)
{
	gnx::StreamingCredentials creds = auth.fetch_streaming_credentials();
	std::vector<gnx::Game> all = gnx::fetch_catalog(http, creds.cloud);

	if (creds.cloud_f2p) {
		try {
			gnx::merge_ad_supported(
				all, gnx::fetch_catalog(http, *creds.cloud_f2p));
		} catch (const std::exception &e) {
			std::fprintf(stderr, "xcloud: f2p catalog: %s\n", e.what());
		}
	}

	playable.clear();
	for (auto &g : all)
		if (g.playable())
			playable.push_back(g);
	std::fprintf(stderr, "xcloud: %zu of %zu titles playable\n",
		     playable.size(), all.size());
	save_catalog_cache(playable);
}

/* ---- recent.json --------------------------------------------------------- */

std::vector<std::string> load_recent()
{
	std::vector<std::string> recent;
	json j = read_json(state_path("recent.json"));

	if (j.is_discarded() || !j.is_array())
		return recent;
	for (const json &e : j)
		if (e.is_string() && (int)recent.size() < kRecentMax)
			recent.push_back(e.get<std::string>());
	return recent;
}

void note_recent(std::vector<std::string> &recent, const std::string &title_id)
{
	recent.erase(std::remove(recent.begin(), recent.end(), title_id),
		     recent.end());
	recent.insert(recent.begin(), title_id);
	if ((int)recent.size() > kRecentMax)
		recent.resize(kRecentMax);
	write_file(state_path("recent.json"), json(recent).dump());
}

/* ---- native.json ---------------------------------------------------------
 *
 * Titles observed encoding below 720p, i.e. the ones that acted on the
 * display details we send. 720p is the platform floor for everything that
 * ignores XGameStreamingSetResolution (docs/RESOLUTION.md), so a smaller
 * stream is proof the game itself chose the size.
 *
 * Learned from actual play rather than shipped as a list: a hand-maintained
 * list would be wrong the moment a title adopts the API, and a sweep of 585
 * titles costs hours of cloud time to answer a question that answers itself
 * the first time you play one.
 */

std::set<std::string> load_native()
{
	std::set<std::string> ids;
	json j = read_json(state_path("native.json"));

	if (j.is_discarded() || !j.is_array())
		return ids;
	for (const json &e : j)
		if (e.is_string())
			ids.insert(e.get<std::string>());
	return ids;
}

void note_native(const std::string &title_id, bool native)
{
	std::set<std::string> ids = load_native();

	/* Recorded in both directions: a title that stops honouring it (or
	 * that we wrongly credited) must be able to lose the mark. */
	if (native ? !ids.insert(title_id).second : !ids.erase(title_id))
		return;  /* already in the state we were about to write */
	write_file(state_path("native.json"),
		   json(std::vector<std::string>(ids.begin(), ids.end())).dump());
}

/* ---- names.json and the resolver ---------------------------------------- */

NameResolver::NameResolver() : path_(state_path("names.json"))
{
	load();
	thread_ = std::thread(&NameResolver::worker, this);
}

NameResolver::~NameResolver()
{
	quit_ = true;   /* also the HTTP abort flag: a batch in flight stops */
	wake_.notify_all();
	if (thread_.joinable())
		thread_.join();
	if (total_)
		std::fprintf(stderr,
			     "names: %d looked up this session, %d named, %d not "
			     "in the Store, %d left\n",
			     total_.load(), hits_.load(), misses_.load(),
			     pending_.load());
}

void NameResolver::load()
{
	json j = read_json(path_);

	if (j.is_discarded() || !j.is_object())
		return;
	for (auto it = j.begin(); it != j.end(); ++it) {
		if (!it.value().is_object())
			continue;
		Entry e;
		e.name = it.value().value("name", "");
		e.art = it.value().value("art", "");
		e.genre = it.value().value("genre", "");
		e.rating = it.value().value("rating", 0.0f);
		e.rating_count = it.value().value("ratings", 0);
		e.year = it.value().value("year", 0);
		e.fetched_day = it.value().value("day", 0);
		e.miss = it.value().value("miss", false);
		/* A recorded miss has no name by definition and is still
		 * worth keeping: it is what stops us asking again. */
		if (e.name.empty() && !e.miss)
			continue;
		/*
		 * Entries written before genre existed are KEPT, so the
		 * library still shows real names immediately, and separately
		 * marked stale so request() asks for them again. Dropping
		 * them (an earlier version of this) meant a library of raw
		 * launch ids until the whole catalogue had been re-resolved,
		 * which is a bad trade for a field the reader may not even
		 * look at.
		 */
		if (it.value()["genre"].is_null())
			stale_.insert(it.key());
		cache_[it.key()] = std::move(e);
	}
	std::fprintf(stderr, "library: %zu names from names.json\n",
		     cache_.size());
}

void NameResolver::save(const std::unordered_map<std::string, Entry> &snapshot) const
{
	json j = json::object();
	for (const auto &[id, e] : snapshot)
		j[id] = json{ { "name", e.name },   { "art", e.art },
			      { "genre", e.genre }, { "rating", e.rating },
			      { "ratings", e.rating_count },
			      { "year", e.year },   { "day", e.fetched_day },
			      { "miss", e.miss } };
	if (!write_file(path_, j.dump()))
		std::fprintf(stderr, "library: could not write names.json\n");
}

void NameResolver::apply_cached(std::vector<gnx::Game> &games) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (gnx::Game &g : games) {
		if (!g.name.empty())
			continue;
		auto found = cache_.find(g.title_id);
		if (found == cache_.end() || found->second.miss)
			continue;
		g.name = found->second.name;
		g.box_art_url = found->second.art;
		g.genre = found->second.genre;
		g.rating = found->second.rating;
		g.rating_count = found->second.rating_count;
		g.year = found->second.year;
	}
}

void NameResolver::request(const std::vector<gnx::Game> &games)
{
	int queued = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (const gnx::Game &g : games) {
			if (g.product_id.empty())
				continue;
			auto have = cache_.find(g.title_id);
			/* A product the Store has no record of. Retried, but
			 * on the ageing clock rather than every launch: some
			 * are delisted for good, some are simply not out yet. */
			if (have != cache_.end() && have->second.miss &&
			    today() - have->second.fetched_day < kMissRetryDays)
				continue;
			/* Named already and not stale: nothing to ask. A
			 * stale entry has a name but predates genre. */
			if (!g.name.empty() && !stale_.count(g.title_id))
				continue;
			if (!asked_.insert(g.title_id).second)
				continue;
			gnx::Game stub;
			stub.title_id = g.title_id;
			stub.product_id = g.product_id;
			jobs_.push_back(std::move(stub));
			queued++;
		}
	}
	if (queued) {
		pending_ += queued;
		total_ += queued;
		wake_.notify_one();
	}
}

void NameResolver::request_refresh(const std::vector<gnx::Game> &games,
				   int max_age_days, int budget)
{
	int queued = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const int now = today();
		for (const gnx::Game &g : games) {
			if (queued >= budget)
				break;
			if (g.product_id.empty())
				continue;
			auto have = cache_.find(g.title_id);
			if (have == cache_.end())
				continue;   /* request() has this one */
			/* fetched_day 0 is an entry from before the day was
			 * recorded: due, but it will get a real day the first
			 * time round and then age normally. */
			if (now - have->second.fetched_day < max_age_days)
				continue;
			if (!asked_.insert(g.title_id).second)
				continue;
			gnx::Game stub;
			stub.title_id = g.title_id;
			stub.product_id = g.product_id;
			jobs_.push_back(std::move(stub));
			queued++;
		}
	}
	if (queued) {
		pending_ += queued;
		total_ += queued;
		wake_.notify_one();
		std::fprintf(stderr,
			     "names: %d entries queued for refresh\n", queued);
	}
}

bool NameResolver::pump(std::vector<gnx::Game> &games)
{
	std::vector<gnx::Game> resolved;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (done_.empty())
			return false;
		resolved.swap(done_);
	}
	bool changed = false;
	for (const gnx::Game &r : resolved) {
		if (r.name.empty())
			continue;
		for (gnx::Game &g : games) {
			if (g.title_id != r.title_id)
				continue;
			/* A game that already has a name may still be waiting
			 * for a genre (a cache entry that predates it), so
			 * "has a name" is not enough to skip on. */
			if (!g.name.empty() && !g.genre.empty())
				continue;
			g.name = r.name;
			g.box_art_url = r.box_art_url;
			g.genre = r.genre;
			g.rating = r.rating;
			g.rating_count = r.rating_count;
			g.year = r.year;
			changed = true;
		}
	}
	return changed;
}

void NameResolver::worker()
{
	pthread_setname_np(pthread_self(), "xc-names");
	gnx::Http http;
	http.set_abort_flag(&quit_);

	while (!quit_) {
		std::vector<gnx::Game> batch;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait(lock, [&] { return quit_ || !jobs_.empty(); });
			if (quit_)
				return;
			while (!jobs_.empty() && batch.size() < 20) {
				batch.push_back(std::move(jobs_.front()));
				jobs_.pop_front();
			}
		}

		try {
			gnx::fetch_names(http, batch);
		} catch (const std::exception &e) {
			/* Best effort: the batch goes back unnamed and is not
			 * retried this session, so a dead network does not
			 * loop; the next launch asks again. */
			if (!quit_)
				std::fprintf(stderr, "library: names: %s\n",
					     e.what());
		}

		std::unordered_map<std::string, Entry> snapshot;
		int hits = 0;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			for (gnx::Game &g : batch) {
				if (!g.name.empty()) {
					cache_[g.title_id] = { g.name,
							       g.box_art_url,
							       g.genre, g.rating,
							       g.rating_count,
							       g.year, today(),
							       false };
					stale_.erase(g.title_id);
					hits++;
				} else {
					/* Remember the miss so the next launch
					 * does not ask again -- the loading
					 * screen would otherwise wait on it
					 * every time, forever. */
					Entry e;
					e.fetched_day = today();
					e.miss = true;
					cache_[g.title_id] = std::move(e);
					stale_.erase(g.title_id);
				}
				done_.push_back(std::move(g));
			}
			snapshot = cache_;
		}
		hits_ += hits;
		misses_ += (int)batch.size() - hits;
		pending_ -= (int)batch.size();
		/* The file write is here, on the worker, and from a copy: the
		 * main thread must never wait on the SD card, and holding the
		 * lock through a write would make it. */
		save(snapshot);
	}
}

}  // namespace app
