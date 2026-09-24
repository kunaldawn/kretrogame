// kgpack: one envelope, three uses.
//
//   state/runtimes/<id>.kgpack   a pinned runtime capsule
//   state/games/<id>.kgpack      the installed game itself
//   <id>.recipe.kgpack           shareable; rebuilds from the recipient's disc
//
// A capsule carries a DwarFS body; a recipe pack carries none. Both carry the
// same tree Merkle root, which is what makes a rebuilt install provable.
//
// This header gathers the pieces: header.h (the fixed 96-byte header),
// pack_meta.h (the metadata), pack.h (reading and writing a pack) and the
// checks on the names a pack carries.
#pragma once

#include "../util/safe_names.h"
#include "header.h"
#include "pack.h"
#include "pack_meta.h"
