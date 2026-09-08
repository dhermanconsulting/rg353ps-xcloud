/*
 * The in-app options menu.
 *
 * One table of switches, reachable two ways: as an overlay drawn over the
 * running stream (SELECT + X), so a filter can be changed while watching
 * the same scene, and as a full screen from the library (Y is search, so
 * this is on START). Values are saved to <state dir>/options.json and
 * reloaded at launch.
 *
 * Command-line flags still win: options_load() runs BEFORE the flags are
 * parsed and options_capture() runs after, so the menu opens showing
 * whatever is actually in force.
 *
 * Some switches take effect on the running stream (the downscale filter,
 * the late latch); the rest are read when a stream starts, and the menu
 * says which is which rather than pretending otherwise.
 */
#pragma once

#include "app.hpp"

namespace app {

/* <state dir>/options.json -> g_opts. Before the command line is parsed. */
void options_load();
/* g_opts -> the menu's own values. After the command line is parsed. */
void options_capture();
/* Written atomically, as the library's caches are. */
void options_save();

/*
 * The overlay, driven from whichever thread polls the pad -- the stream's
 * input thread, or the replay loop.
 *
 * While it is open the caller MUST send a neutral pad frame to the engine
 * instead of the real one, or navigating the menu also drives the game.
 * options_menu_input() consumes the presses it uses, so anything it does
 * not use still reaches the game.
 */
bool options_menu_open();
void options_menu_input(pad &p, int now);
/*
 * Draw over the frame already written to the back buffer. Write-only, like
 * every other blit here (see the note in text.c), so it must be called
 * after the frame is in the buffer and before the commit.
 */
void options_menu_draw(drm_out &out, Fonts &f);

/*
 * The settings PAGE: the library's Settings tab. Unlike the overlay it edits
 * a staged copy and commits only on its Save row, because a page you sit and
 * configure on should not act on a value you merely cycled past -- see the
 * note by g_edit in options.cpp.
 *
 * The library owns the chrome (header, primary tabs) and calls in for the
 * body; options.cpp owns the sub-tabs, the rows, the staging and the actions.
 */

/*
 * Which of the page's two bands the library has lit.
 *
 * The library owns the cursor's vertical position across the whole screen --
 * primary tabs, then sub-tabs, then rows -- so the page cannot decide for
 * itself which of its bands the D-pad is talking to; it has to be told. NONE
 * means the cursor is up on the primary tabs and the whole page is inert but
 * still drawn, so you can see what you are about to step into.
 */
enum OptPageFocus { OPT_FOCUS_NONE, OPT_FOCUS_GROUP, OPT_FOCUS_BODY };

void options_page_enter();              /* stage from the committed values */
bool options_page_unsaved();            /* something staged but not saved */
bool options_page_explaining();         /* the Explain panel is open */
void options_page_group_step(int dir);  /* Standard <-> Advanced */
bool options_page_at_top();             /* the cursor is on the first row */
void options_page_to_top();             /* put it there */
/*
 * True when the page consumed the press. Called only while the rows have the
 * cursor, and the library keeps Up at the first row for itself so the cursor
 * can leave this band upwards.
 */
bool options_page_input(pad &p, int now);
void options_page_draw(Painter &pt, Fonts &f, int top, int bottom,
		       OptPageFocus focus);

}  // namespace app
