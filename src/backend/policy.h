// How a game is drawn, heard and shown on this machine: decided, not guessed.
//
// Everything here is a pure function of three things - what the author said,
// what the game's executable imports, and what the host can do - so every
// decision is a row in a table test rather than a thing somebody finds out on
// a stranger's laptop. Nothing here runs a program, reads a file or looks at
// the environment. The answer is a plain struct; session::play applies it.
//
// The table, which is the player's design for drawing:
//
//   D3D8/9            DXVK 3 on Vulkan 1.4, DXVK 2.7 on 1.3; else WineD3D-Vulkan,
//                     then WineD3D-GL
//   D3D5-7, 3D ddraw  WineD3D with renderer=vulkan; else WineD3D-GL
//   2D DirectDraw     cnc-ddraw
//   OpenGL            native, through the bundled Mesa or the host's NVIDIA
//
// Vulkan paths are preferred because 32-bit OpenGL under new WoW64 maps
// buffers slowly without Vulkan interop, and every one of these games is
// 32-bit.
//
// No usable GPU - NVIDIA's libraries missing or not matching the kernel, and
// nothing else in the machine - means software rendering, said out loud. A game
// the author marked as needing a GPU is refused instead: it would start, and be
// unplayable, and the person would blame the game.
#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "../gpu/caps.h"
#include "../util/pe.h"

namespace kg::backend {

// What the author picked in the builder, as bundle.meta spells it.
enum class AuthorBackend { Auto, Dxvk, WineD3DVulkan, WineD3DGL, CncDdraw };
AuthorBackend parse_author_backend(const std::string& s);  // unknown -> Auto
const char* author_backend_name(AuthorBackend b);

enum class Backend {
  Dxvk3,
  Dxvk2,
  WineD3DVulkan,
  WineD3DGL,
  CncDdraw,
  NativeGL,
  Software,
  Refuse,
};
const char* backend_name(Backend b);

struct Decision {
  Backend backend = Backend::Refuse;
  // One sentence a person can read: why this backend, or why not a better one.
  std::string reason;
};

Decision choose_backend(AuthorBackend author, const pe::Imports& exe, const gpu::HostCaps& caps,
                        bool needs_gpu);

// Whether a game draws with OpenGL itself: it imports opengl32, or it names
// opengl32 (to load it by hand) and imports no Direct3D or DirectDraw that a
// Direct3D path might be drawing with instead.
bool draws_with_opengl(const pe::Imports& exe);

// The environment that keeps an OpenGL game of this era from overflowing on
// the driver's extension list. id Tech 3 prints GL_EXTENSIONS through a fixed
// buffer on its stack, and today's lists are several times what one held in
// 1999: the game dies writing past its stack a second after it starts. Both
// drivers can be asked for a list of the length an old game expects - NVIDIA's
// as of its driver 177, Mesa's without extensions past 2003 - and only a game
// that draws with OpenGL itself is asked for it, since WineD3D wants the whole
// list. A value the person set is theirs, and is left alone by the caller.
std::vector<std::pair<std::string, std::string>> gl_extension_cap();

enum class DisplayPath {
  NestedWeston,     // our Weston and Xwayland, integer scaling everywhere
  GamescopeDirect,  // Steam Deck Game Mode: gamescope already is the compositor
};
const char* display_path_name(DisplayPath d);
DisplayPath display_path(const gpu::HostCaps& caps);

// Wine's audio driver list. pulse first, which pipewire-pulse also answers;
// alsa after it, against the runtime's own configuration.
std::string audio_drivers();

struct RegValue {
  std::string key;     // HKEY_CURRENT_USER\Software\Wine\Direct3D
  std::string name;    // renderer
  bool dword = false;  // REG_DWORD rather than REG_SZ
  std::string data;    // "vulkan", or "1" for a DWORD
};

// Everything the decision implies for Wine, and nothing else.
struct Plan {
  Decision decision;
  DisplayPath display = DisplayPath::NestedWeston;
  std::vector<std::pair<std::string, std::string>> env;
  std::vector<RegValue> registry;
  // Appended to whatever WINEDLLOVERRIDES the pack already carries.
  std::string dll_overrides;
  // Where the DLLs come from, relative to the runtime root. The first that
  // exists is used; the later ones are older layouts of the same thing.
  std::vector<std::string> dll_dirs;
  // Which DLLs, by name without .dll, to put into the prefix's system
  // directories from that source.
  std::vector<std::string> dlls;

  bool refused() const { return decision.backend == Backend::Refuse; }
};

// The Wine side of a decision: DXVK's DLLs and overrides, WineD3D's renderer,
// software rendering's Mesa variables, and what every choice shares - the
// audio driver list, and the input settings Proton uses (SDL for gamepads,
// hidraw off so one pad does not appear twice).
Plan settings_for(const Decision& d, const gpu::HostCaps& caps);

// The two together, which is what a caller wants.
Plan plan(AuthorBackend author, const pe::Imports& exe, const gpu::HostCaps& caps,
          bool needs_gpu);

// The .reg file that applies `registry`, in the REGEDIT4 form `wine regedit`
// imports. One file and one Wine start, rather than a Wine start per value.
std::string registry_file(const std::vector<RegValue>& values);

}  // namespace kg::backend
