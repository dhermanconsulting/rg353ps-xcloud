/*
 * The library's data side: sort keys and letter buckets, the on-disk caches
 * (catalog.json, names.json, recent.json under g_opts.state_dir) and the
 * background name resolver.
 *
 * Nothing here paints. library.cpp does that on the main thread and only
 * ever talks to the resolver through request() and pump(), the same
 * hand-off green-nx's Names uses: the worker owns its HTTP and its file, the
 * main thread takes finished batches from under a mutex.
 */
#pragma once

#include "app.hpp"

#include <condition_variable>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace app {

/*
 * What the list sorts by: the name casefolded with accents stripped to their
 * base letter, punctuation dropped and a leading "The " removed, so "The
 * Witcher 3" files under W and "Ōkami" under O. Falls back to the launch id
 * (lowercased) while the name is still unresolved -- the id is the squashed
 * name, so an unresolved title still lands roughly where it will end up.
 */
std::string sort_key(const gnx::Game &g);

/* 'A'..'Z' for a key starting with a letter, '#' for digits and the rest. */
char letter_of_key(const std::string &key);

/*
 * What the row shows: the name, or a readable form of the launch id
 * ("Assassinscreedshadows" rather than "ASSASSINSCREEDSHADOWS") while it
 * has none.
 */
std::string display_name(const gnx::Game &g);

/* ---- catalog.json -------------------------------------------------------- */

/* load_catalog_cache is declared in app.hpp (main.cpp calls it). */
void save_catalog_cache(const std::vector<gnx::Game> &games);

/* ---- recent.json --------------------------------------------------------- */

constexpr int kRecentMax = 8;

/* Launch ids, most recent first. */
std::vector<std::string> load_recent();

/* Move `title_id` to the front, trim to kRecentMax, write recent.json. */
void note_recent(std::vector<std::string> &recent, const std::string &title_id);

/*
 * Titles seen encoding below 720p, i.e. those that honour a custom
 * resolution. See library_data.cpp; note_native() is a no-op when the mark is
 * already in the state asked for.
 */
std::set<std::string> load_native();
void note_native(const std::string &title_id, bool native);

/* ---- names.json and the resolver ---------------------------------------- */

/*
 * Resolves names and box-art URLs for every title that lacks one, in
 * batches of 20 (one displaycatalog round trip is ~1 MB for 20 products,
 * which bounds the worker's memory on a 1 GB device), and writes
 * names.json after every batch so the next launch is complete instantly
 * even if this one is cut short.
 *
 * names.json: { "<title_id>": { "name": "...", "art": "<url>" }, ... }
 * Only hits are stored; a product the Store did not answer for is left out
 * and asked again next launch (a batch of misses is a tiny response).
 */
class NameResolver {
public:
	NameResolver();
	~NameResolver();

	/* Main thread, before the first paint: names the cache already has. */
	void apply_cached(std::vector<gnx::Game> &games) const;

	/* Main thread: queue every game that is missing something the library
	 * needs -- a name, or a genre from a cache written before genres. A
	 * known miss is not queued again until it ages out. These are what
	 * the loading screen waits for. */
	void request(const std::vector<gnx::Game> &games);

	/*
	 * Main thread, after the loading screen: queue up to `budget` entries
	 * that are complete but older than `max_age_days`, so ratings and
	 * names follow the Store over time. Bounded because a full re-pull is
	 * ~47 MB; a slice per launch ages the whole catalogue through in a
	 * few weeks without anyone noticing. Nothing waits on these.
	 */
	void request_refresh(const std::vector<gnx::Game> &games,
			     int max_age_days, int budget);

	/* Main thread: take finished lookups into `games`. True if any name
	 * changed, which means a re-sort. */
	bool pump(std::vector<gnx::Game> &games);

	/* Progress for the indicator: lookups still queued or in flight, and
	 * how many were queued in total this session. */
	int pending() const { return pending_.load(); }
	int total() const { return total_.load(); }

private:
	struct Entry {
		std::string name;
		std::string art;
		std::string genre;
		float rating = 0;
		int rating_count = 0;
		int year = 0;
		/* Days since the epoch when this was last fetched, for the
		 * ageing refresh; 0 for entries written before it existed. */
		int fetched_day = 0;
		/* The Store had nothing for this product id. Recorded rather
		 * than left out, or every launch would ask again for the ~21
		 * titles that will never answer -- which is invisible in a
		 * background trickle and two seconds of a waiting screen. */
		bool miss = false;
	};

	void worker();
	void load();
	void save(const std::unordered_map<std::string, Entry> &snapshot) const;

	std::string path_;
	mutable std::mutex mutex_;
	std::condition_variable wake_;
	std::unordered_map<std::string, Entry> cache_;  /* title_id -> entry */
	std::deque<gnx::Game> jobs_;                    /* stubs: ids only */
	std::vector<gnx::Game> done_;                   /* stubs with names */
	std::unordered_set<std::string> asked_;         /* title_ids, session */
	/* Cached entries that predate a field we now want (genre): usable for
	 * display, but re-requested so the cache fills in. */
	std::unordered_set<std::string> stale_;
	std::atomic<bool> quit_{false};
	std::atomic<int> pending_{0};
	std::atomic<int> total_{0};
	std::atomic<int> hits_{0}, misses_{0};   /* this session, for the log */
	std::thread thread_;
};

}  // namespace app
