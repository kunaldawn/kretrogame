// What a Windows executable says it needs: the DLLs in its import table.
//
// This is how "auto" picks a graphics backend. A game that imports d3d9.dll
// draws with Direct3D 9, one that imports ddraw.dll draws with DirectDraw, one
// that imports opengl32.dll draws with OpenGL, and the best way to run each is
// different. Reading it takes a few kilobytes of the file and no Wine at all.
//
// The file comes out of a pack somebody else built, so nothing in it is
// trusted: every offset is checked against the buffer before it is followed,
// every count is capped, and a header that points outside the file ends the
// parse with a reason rather than a read past the end.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace kg::pe {

struct Imports {
  bool ok = false;
  std::string error;               // why ok is false, in words
  bool is64 = false;               // PE32+ rather than PE32
  std::vector<std::string> dlls;   // lower case, in table order, each once

  // Whether the program asks DirectDraw for a Direct3D interface: the IID of
  // IDirect3D, 2, 3 or 7 is somewhere in its bytes. A Direct3D 5-7 game never
  // imports d3dim.dll - ddraw.dll loads it on the game's behalf - so its
  // import table looks exactly like a 2D DirectDraw game's. The interface ID it
  // passes to QueryInterface is linked into it from dxguid.lib, and that is
  // the one place the difference shows.
  bool direct3d_im = false;

  // Whether "opengl32" is written anywhere in it. The id Tech 3 games and
  // their like import no OpenGL at all: they LoadLibrary it by name, so that a
  // 3Dfx MiniGL can stand in, and the name is the only trace in the file.
  bool names_opengl = false;

  bool imports(const std::string& dll) const;  // "d3d9" or "d3d9.dll", any case
};

// Parses a whole file held in memory.
Imports parse(const uint8_t* data, size_t size);
Imports parse(const std::vector<uint8_t>& bytes);

// Reads the file and parses it. Anything over 512 MiB is refused unread; no
// executable of this era is a tenth of that, and a pack is not a reason to
// allocate whatever size a stranger wrote down.
Imports parse_file(const std::filesystem::path& p);

}  // namespace kg::pe
