// kretro export and import: a game for another kretro.
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "handlers.h"
#include "../install/share.h"
#include "../rt/env.h"
#include "../util/format.h"
#include "../util/paths.h"

namespace fs = std::filesystem;

namespace kg::cli {

int cmd_export(const rt::Env& /*e*/, std::vector<std::string>& a) {
  const std::string& id = a[0];
  bool recipe = true;
  fs::path out;
  for (size_t i = 1; i < a.size(); ++i) {
    if (a[i] == "--recipe") recipe = true;
    else if (a[i] == "--capsule") recipe = false;
    else if (a[i] == "--standalone") {
      // Gone: it copied all of kretro, authoring tools and all, to carry
      // one game. A one-game player is the same one file for somebody
      // else to run, at half the size, and it is what gets shipped now.
      std::fprintf(stderr,
                   "kretro: export --standalone is gone. A player is what you send now:\n"
                   "  kretro bundle build -o %s.run --id %s --rights %s\n"
                   "or the Bundles page on the shelf.\n",
                   id.c_str(), id.c_str(), id.c_str());
      return 2;
    }
    else if (a[i] == "-o" && i + 1 < a.size()) out = a[++i];
  }
  if (recipe) {
    fs::path f = install::export_recipe(id, out);
    std::printf("%s\n", f.c_str());
    std::printf("  %s - the recipe, not the game. Anyone with the same disc rebuilds it\n",
                fmt::bytes_iec(fs::file_size(f)).c_str());
    std::printf("  by clicking through the installer themselves, and the tree they get is\n");
    std::printf("  compared with this one and the difference reported.\n");
  } else {
    fs::path src = game_pack(id);
    fs::path dst = out.empty() ? fs::path(id + ".kgpack") : out;
    std::error_code ec;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) { std::fprintf(stderr, "kretro: %s\n", ec.message().c_str()); return 1; }
    std::printf("%s\n  %s - the whole game in one file\n", dst.c_str(),
                fmt::bytes_iec(fs::file_size(dst)).c_str());
  }
  return 0;
}

int cmd_import(const rt::Env& e, std::vector<std::string>& a) {
  install::ImportResult r = install::import_pack(e, a[0], /*replace=*/false, log_line);
  std::printf("\n%s\n", r.name.c_str());
  if (r.rebuilt) {
    std::printf("  rebuilt from your own disc, and %s\n",
                r.root_matched ? "it matches the recipe exactly" : "NOT verified");
  } else {
    std::printf("  installed from the capsule\n");
  }
  std::printf("  %s\n", r.pack.c_str());
  return 0;
}

}  // namespace kg::cli
