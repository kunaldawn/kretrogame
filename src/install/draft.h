// What the wizard has decided so far, and the two conversions it lives
// between: a Draft becomes the Meta that gets packed, and a Meta - a manifest or
// a recipe somebody sent - becomes the Draft the wizard opens on.
//
// The recipe's method is written into every pack as one of four strings. They
// are an on-disk format, so they are named once here and every reader and
// writer spells them through these constants.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../disc/disc.h"
#include "../pack/kgpack.h"
#include "keys.h"

namespace kg::install {

// What the wizard has decided so far. It is not a Meta: a Meta is what comes
// out at the end, with fingerprints and a tree in it, and half of that does not
// exist while a person is still choosing.
struct Draft {
  std::vector<std::filesystem::path> sources;
  // `serial` is stored in the key vault under `id` and goes into no pack;
  // the comment at the top of keys.h states that policy and draft_to_meta
  // obeys it.
  std::string id, name, serial;
  uint32_t year = 0;

  // The four values are the four strings install.cpp's method branch accepts:
  // wine_setup, installer_exe, copy, unzip. A fifth would be a method the
  // engine cannot run.
  enum class Method { Installer, InstallerExe, Copy, Unzip } method = Method::Installer;
  // Installer: "<n>/<path on that disc>", n counting from 1 in the order
  // open_sources assembled the set - because "which exe" is not an answer
  // without "on which disc". InstallerExe: an absolute path to the file.
  std::filesystem::path setup;
  std::string member, subdir;        // unzip / copy

  std::string windows_version = "winxp";   // asked before the install, not after

  std::filesystem::path install_dir; // chosen from the candidates, under drive_c
  std::filesystem::path exe;         // chosen from that directory
  std::string args;
  // Filled on the way out of step 6 by verify_list, from the directory the
  // user confirmed at step 5 and the executable they picked at step 6. It is
  // what the pack carries: draft_to_meta copies it and Build::write keeps it,
  // computing a list of its own only for a draft that arrived without one.
  std::vector<std::string> verify;

  uint32_t width = 640, height = 480;
  bool dgvoodoo = false;
  bool embed_discs = true;
};

// recipe.method, as it is written into a pack and a manifest. One string per
// Draft::Method, in the same order.
inline constexpr std::string_view kMethodWineSetup = "wine_setup";
inline constexpr std::string_view kMethodInstallerExe = "installer_exe";
inline constexpr std::string_view kMethodCopy = "copy";
inline constexpr std::string_view kMethodUnzip = "unzip";

// The recipe string a method is written as.
std::string_view recipe_method(Draft::Method m);

// The method a recipe string names. Empty when the recipe names none at all,
// and Installer for any string that is not one of the other three - including
// wine_setup itself, and anything this build does not know.
std::optional<Draft::Method> method_from_recipe(std::string_view s);

// Draft plus the assembled discs -> the Meta that gets packed.
Meta draft_to_meta(const Draft& d, const std::vector<disc::Disc>& discs);

// Everything a manifest or a recipe already knows, as a Draft the wizard can
// open on. Disc references resolve by name first and then by fingerprint, the
// same two-step install::run does; a reference that resolves to nothing is
// left out of the sources and named in `missing`, so the page can say which
// disc is wanted rather than failing a step later.
struct Prefill {
  Draft draft;
  std::vector<std::string> missing;
  // Set when the pack named an installer it had no business naming - an
  // absolute path, or one climbing out with "..". The draft's setup is cleared
  // and this holds what was asked for, so the wizard can say why it is asking
  // rather than silently starting somewhere else.
  std::string unsafe_setup;
};
Prefill draft_from_meta(const Meta& m);

// What the vault remembers, put into a draft that arrived without a serial.
// Returns true when it filled something in.
//
// A serial goes into no pack (keys.h says why), so a recipe knows everything
// about a game except the one thing its installer will ask for out loud. The
// wizard's identity page looks the key up while you are looking at that page -
// but a rebuild does not stop there, it goes straight to the install, and the
// step that shows the serial beside the installer had nothing to show for
// exactly the case that most needed it. Prefilling is not typing it in: the
// number is put on screen next to the box asking for it, and the person types.
bool prefill_serial(Draft& d, const std::vector<StoredKey>& keys);

}  // namespace kg::install
