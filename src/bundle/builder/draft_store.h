// Where drafts are remembered: <state>/bundles/<id>.cbor, one file per bundle.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "checks.h"
#include "draft.h"

namespace kg::bundle {

// <state>/bundles. A parameter everywhere below, so a test has its own.
std::filesystem::path bundles_dir();

// Every remembered bundle in `dir`, by title. A file that does not decode is
// skipped and named in `unreadable`, rather than hiding the rest.
std::vector<Draft> load_drafts(const std::filesystem::path& dir,
                               std::vector<std::string>* unreadable = nullptr);
// Written beside and renamed over, so a crash mid-write leaves the old draft.
// `was` is the id it was remembered under before, when the author renamed it
// before publishing: that file goes, or one bundle would be two. A file
// already there under d.id that is not `was` is another bundle, and is
// refused rather than written over: a new bundle left at its default title,
// or a title that happens to slug to an older bundle's id, would otherwise
// silently replace that bundle and everything the author set on it.
void save_draft(const std::filesystem::path& dir, const Draft& d, const std::string& was = "");
void forget_draft(const std::filesystem::path& dir, const std::string& id);

// `base`, or base-2, base-3... : the first no bundle in `dir` is remembered
// under. What a new bundle starts as, so it never starts as an old one.
std::string unused_id(const std::filesystem::path& dir, const std::string& base);

// Records a finished build on the bundle remembered as `id`, read fresh from
// `dir`: the page may be holding an older copy, or the author may have renamed
// the bundle while it built. False, and nothing written, when no bundle is
// remembered under that id any more - writing one back would bring a renamed
// bundle back as a second one.
bool stamp_built(const std::filesystem::path& dir, const std::string& id, const std::string& path,
                 uint64_t size, const std::string& when);

// Remembers a bundle `kretro bundle build` made, as the page would hold it
// after its Build: each check the build printed acknowledged - --rights and
// those notes are the acknowledgement a script makes, and `rebuild` is held
// to them - and the build stamped. A bundle already remembered under that id
// is the page's, with everything the author set on it, and is left as it is:
// false, and nothing written.
bool remember_built(const std::filesystem::path& dir, Draft d, const std::vector<Check>& checks,
                    const std::string& path, uint64_t size, const std::string& when);

}  // namespace kg::bundle
