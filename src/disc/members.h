// What is on a disc, and getting it off.
//
// An image is read through the bundled 7z. A directory - a mounted CD, or a
// disc somebody already extracted - is read directly, because 7z exits
// non-zero on one.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "../rt/env.h"
#include "../util/hash.h"

namespace kg::iso {

// The disc's contents, via the bundled 7z. Paths are as the disc spells them.
// A directory - a mounted CD, or a disc somebody already extracted - is walked
// directly instead: 7z exits non-zero on one, and a directory is the shape the
// headline "load game CD/DVD" path arrives in.
std::vector<std::string> list(const rt::Env& e, const std::filesystem::path& p);

// Case-insensitive lookup against a real listing. Joliet casing is not
// predictable - a disc's top-level directory may read `pc`, not `PC` - so
// nothing may trust a manifest's spelling.
std::string resolve(const std::vector<std::string>& listing, const std::string& want);

// Extracts one member to `out_dir`, keeping its name. Used for anchors and for
// the unzip recipe. A directory source is copied from, not extracted.
bool extract_member(const rt::Env& e, const std::filesystem::path& iso, const std::string& member,
                    const std::filesystem::path& out_dir, std::string* err = nullptr);

// Extracts a whole subtree of the disc into `out_dir`, flattening the subtree
// prefix away so `out_dir` becomes the game root. A directory source is copied
// recursively, into real files: what this leaves behind goes into a pack that
// has to outlive this machine.
bool extract_subtree(const rt::Env& e, const std::filesystem::path& iso, const std::string& subdir,
                     const std::filesystem::path& out_dir, std::string* err = nullptr);

// Hashes a member without writing it to disk, by reading 7z's stdout - or, for
// a directory source, the file itself.
bool hash_member(const rt::Env& e, const std::filesystem::path& iso, const std::string& member,
                 Hash* out, uint64_t* size);

}  // namespace kg::iso
