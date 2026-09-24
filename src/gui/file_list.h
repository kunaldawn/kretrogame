// A plain list over std::filesystem, drawn inside a page: where it is looking,
// "..", the folders in it and the files it offers.
//
// There is no file dialog in kretro and there should not be one: a dialog is
// another toolkit, another theme, and nothing a gamepad can drive. The Import
// page, the wizard's first step and the Bundles page each drew their own copy
// of this list; what differed between them is FileListSpec.
#pragma once

#include <filesystem>
#include <functional>

namespace kg::gui {

struct FileListSpec {
  // The child window's ID, and its height.
  const char* child_id = "";
  float height = 0;
  // Leave out every entry whose name starts with a dot.
  bool skip_hidden = false;
  // Which regular files are listed. Empty lists them all.
  std::function<bool(const std::filesystem::path&)> show_file;
  // A small button at the end of every folder's row, for picking the folder
  // itself rather than going into it.
  const char* dir_button = nullptr;
  // Said inside the list when it has neither a folder nor a file to show.
  const char* empty_text = nullptr;
};

struct FilePick {
  enum Kind {
    None,
    File,
    DirButton,
  };
  Kind kind = None;
  std::filesystem::path path;
};

// Draws the list over `dir`. Going into a folder, or up with "..", changes
// `dir`. Returns what was picked this frame, for the caller to act on once the
// list is drawn.
FilePick file_list(std::filesystem::path& dir, const FileListSpec& spec);

}  // namespace kg::gui
