// What the Bundles page decides, with the page taken away.
//
// The page is ImGui and SDL, and a test binary cannot link either; everything
// on it that could be quietly wrong - the id a title turns into, what is
// remembered and what comes back, which checks a game raises, how big the
// file will be, which backend "auto" means, what a preview is allowed to
// inherit - is here instead, as functions of plain data. The page draws a
// Draft, edits it, and hands it to build_from_draft on a worker thread; that
// is the same call the tests make.
//
// A Draft is the author's bundle as the author left it: every field of every
// step, the checks already acknowledged, where the last build went. It is
// remembered in <state>/bundles/<id>.cbor, one file per bundle, so building
// the next version is opening the page and pressing Build. The pictures and
// author-supplied files are held as bytes, not as paths: a banner that moved
// on the author's disk must not turn a one-click rebuild into a search.
//
// Each concern has a header of its own under builder/, all of it in namespace
// kg::bundle; this one includes them all, for the page, the command line and
// the tests.
#pragma once

#include "builder/checks.h"
#include "builder/draft.h"
#include "builder/draft_build.h"
#include "builder/draft_store.h"
#include "builder/exe_probe.h"
#include "builder/key_fragment.h"
#include "builder/pack_facts.h"
#include "builder/player_base.h"
#include "builder/preview.h"
#include "builder/repack.h"
#include "builder/size_report.h"
