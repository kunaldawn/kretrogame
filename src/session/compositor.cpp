#include "compositor.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "../util/env.h"
#include "../util/paths.h"
#include "../util/proc.h"
#include "input_helper.h"
#include "internal.h"
#include "saves_layout.h"

namespace kg::session {
namespace fs = std::filesystem;

using detail::wait_for;

namespace {

// A child this process started and has to see gone however run_in_compositor
// leaves it, thrown out of or returned from: `sig` is sent to the child, or
// to its whole process group when `group` is set, and it is reaped. release()
// is for a child that is already gone and reaped, so nothing is signalled.
struct ScopedChild {
  pid_t pid = 0;
  int sig = SIGTERM;
  bool group = false;

  ScopedChild(pid_t p, int s, bool g) : pid(p), sig(s), group(g) {}
  ScopedChild(ScopedChild&& o) noexcept : pid(o.pid), sig(o.sig), group(o.group) { o.release(); }
  ScopedChild(const ScopedChild&) = delete;
  ScopedChild& operator=(const ScopedChild&) = delete;
  ScopedChild& operator=(ScopedChild&&) = delete;
  ~ScopedChild() {
    if (pid > 0) {
      kill(group ? -pid : pid, sig);
      int st;
      waitpid(pid, &st, 0);
    }
  }
  void release() { pid = 0; }
};

// The desktop shell, with its helper clients pointed at our copies, and
// Weston's own Xwayland switched off - we run our own, below.
//
// kiosk-shell would be the natural choice for a single fullscreen game, and
// it is what the design called for, but Weston 14's Xwayland window manager
// asserts on a map request from a shell that creates no frames
// (weston_wm_handle_map_request: window->frame_id != XCB_WINDOW_NONE) and
// takes the session down with it. The desktop shell is stable with X
// clients; its only problem was that it spawns helpers from paths compiled
// in at build time, and those are configurable.
void write_weston_ini(const rt::Env& e, const fs::path& home) {
  std::ofstream(home / ".config" / "weston.ini")
      << "[core]\nshell=desktop-shell.so\nxwayland=false\nidle-time=0\nrequire-input=false\n"
      << "[shell]\n"
      << "client=" << (e.root / "usr/libexec/weston-desktop-shell").string() << "\n"
      << "panel-position=none\nbackground-color=0xff000000\nanimation=none\n"
      << "[input-method]\n"
      << "path=" << (e.root / "usr/libexec/weston-keyboard").string() << "\n";
}

// A running Weston, and the name of the Wayland socket it answers on.
struct Weston {
  ScopedChild child;
  std::string socket;
};

Weston start_weston(const rt::Env& e, const rt::Env& wine_env, const CompositorOptions& opt,
                    uint32_t w, uint32_t h, uint32_t s,
                    const std::function<void(const std::string&)>& say) {
  std::error_code ec;
  const fs::path& home = opt.home;
  std::string socket = "kretro-" + opt.socket_suffix;
  std::vector<std::string> wargs;
  // Weston runs as an ordinary client of whatever the host has, which is what
  // keeps this on the well-trodden path on both Wayland and X11 hosts - unless
  // nobody is meant to see it, in which case it renders into a buffer and no
  // window appears anywhere. Our own Xwayland is a client of this Weston
  // either way, for the reason given above the weston.ini: we want a root
  // window that is one surface, and no X window manager in the picture.
  wargs.push_back(opt.headless
                      ? "--backend=headless"
                      : (env_nonempty("WAYLAND_DISPLAY") ? "--backend=wayland" : "--backend=x11"));
  wargs.push_back("--socket=" + socket);
  wargs.push_back("--debug");  // enables the capture protocol the journal uses
  if (opt.headless) {
    // No --scale here. A headless output is read back pixel for pixel by
    // whoever is drawing it, and scaling it in the compositor would only make
    // the same picture cost more to copy across the X connection.
    wargs.push_back("--width=" + std::to_string(w));
    wargs.push_back("--height=" + std::to_string(h));
    say("display: headless " + std::to_string(w) + "x" + std::to_string(h) +
        " - drawn inside our own window");
  } else if (opt.fullscreen) {
    wargs.push_back("--fullscreen");
    say("display: fullscreen, the game renders at " + std::to_string(w) + "x" + std::to_string(h));
  } else {
    wargs.push_back("--width=" + std::to_string(w));
    wargs.push_back("--height=" + std::to_string(h));
    wargs.push_back("--scale=" + std::to_string(s));
    say("display: " + std::to_string(w) + "x" + std::to_string(h) + " at " + std::to_string(s) +
        "x -> " + std::to_string(w * s) + "x" + std::to_string(h * s) + " window");
  }

  fs::path weston = rt::which(e, "weston");
  if (weston.empty()) throw std::runtime_error("no weston in this runtime");
  fs::path weston_log = home / "weston.log";
  fs::remove(weston_log, ec);
  // A Weston that was killed rather than shut down leaves its socket behind.
  // Left there, we would see the socket, believe the compositor is up, and
  // then watch Xwayland fail to connect to a dead endpoint.
  {
    const char* xr = env_nonempty("XDG_RUNTIME_DIR");
    fs::path base = xr ? fs::path(xr) : fs::path("/tmp");
    fs::remove(base / socket, ec);
    fs::remove(base / (socket + ".lock"), ec);
  }
  rt::Env wenv = wine_env;
  wenv.set("WESTON_CONFIG_FILE", (home / ".config" / "weston.ini").string());

  pid_t wpid = kg::fork_tied(SIGTERM);
  if (wpid < 0) throw std::runtime_error("cannot fork");
  if (wpid == 0) {
    int fd = open(weston_log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }
    setsid();
    rt::exec(wenv, weston, wargs);
  }

  ScopedChild child(wpid, SIGTERM, false);

  const char* xdg = env_nonempty("XDG_RUNTIME_DIR");
  fs::path sock = fs::path(xdg ? xdg : "/tmp") / socket;
  // The socket appearing is necessary but not sufficient: check the process is
  // still alive, so a compositor that started and immediately died is reported
  // as such rather than as a puzzling failure two steps later.
  bool up = wait_for([&] {
    int st = 0;
    if (waitpid(wpid, &st, WNOHANG) == wpid) { child.release(); return true; }
    return fs::exists(sock, ec);
  }, 120, 250);
  if (!up || child.pid == 0) {
    std::ifstream log(weston_log);
    std::stringstream ss; ss << log.rdbuf();
    throw std::runtime_error("Weston did not start:\n" + ss.str().substr(0, 2000));
  }
  return Weston{std::move(child), socket};
}

// The first X display number from :8 up that nobody has a socket for.
int free_x_display() {
  std::error_code ec;
  int dispnum = -1;
  for (int n = 8; n < 100; ++n) {
    if (!fs::exists("/tmp/.X11-unix/X" + std::to_string(n), ec)) { dispnum = n; break; }
  }
  if (dispnum < 0) throw std::runtime_error("no free X display number");
  return dispnum;
}

// Our own Xwayland, run as an ordinary client of that Weston and *not*
// rootless, so its root window is a single surface: the game's private
// screen, integer-scaled by Weston.
//
// Weston's built-in Xwayland is not used, because its window manager asserts
// on a map request from a window it did not frame
// (weston_wm_handle_map_request: window->frame_id != XCB_WINDOW_NONE) and
// takes the whole session down. Running the X server ourselves means there is
// no X window manager in the picture at all, which is exactly right for a
// game that owns its whole screen.
ScopedChild start_xwayland(const rt::Env& e, const rt::Env& wine_env, const fs::path& home,
                           const std::string& socket, const std::string& display, int dispnum,
                           uint32_t w, uint32_t h) {
  std::error_code ec;
  rt::Env xenv = wine_env;
  xenv.set("WAYLAND_DISPLAY", socket);
  fs::path xwayland = rt::which(e, "Xwayland");
  if (xwayland.empty()) throw std::runtime_error("no Xwayland in this runtime");

  pid_t xpid = kg::fork_tied(SIGTERM);
  if (xpid < 0) throw std::runtime_error("cannot fork");
  if (xpid == 0) {
    int fd = open((home / "xwayland.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }
    setsid();
    rt::exec(xenv, xwayland,
             {display, "-geometry", std::to_string(w) + "x" + std::to_string(h), "-noreset"});
  }
  ScopedChild child(xpid, SIGTERM, false);

  if (!wait_for([&] { return fs::exists("/tmp/.X11-unix/X" + std::to_string(dispnum), ec); }, 120, 250)) {
    std::ifstream log(home / "xwayland.log");
    std::stringstream ss; ss << log.rdbuf();
    throw std::runtime_error("Xwayland did not start:\n" + ss.str().substr(0, 2000));
  }
  return child;
}

// Says which GL renderer Weston came up on, from its log, and warns when it
// is software rendering.
void log_gl_renderer(const fs::path& weston_log, const std::function<void(const std::string&)>& say) {
  std::ifstream log(weston_log);
  std::string line;
  while (std::getline(log, line)) {
    size_t p = line.find("GL renderer:");
    if (p == std::string::npos) continue;
    std::string r = line.substr(p + 12);
    while (!r.empty() && r.front() == ' ') r.erase(r.begin());
    if (r.rfind("llvmpipe", 0) == 0) {
      say("warning: software rendering (" + r + ") - run kretro doctor");
    } else {
      say("renderer: " + r);
    }
    break;
  }
}

// A helper that photographs the nested screen while the game runs. The first
// stable frame becomes the game's tile art - unless an install left a
// stand-in there, which the first play replaces; the most recent one is what
// you are shown when you come back months later and cannot remember where
// you were. Neither needs anything from the game itself, because we own the
// compositor it is drawing into.
//
// The child is forked and never execs, and takes a session of its own, so it
// is ended with SIGKILL to its whole group. The frames directory is made
// whether or not there is anything to capture.
ScopedChild start_frame_capture(const rt::Env& e, const rt::Env& wine_env, const std::string& socket,
                                const CompositorOptions& opt) {
  std::error_code ec;
  const fs::path& frames = opt.capture_dir;
  fs::create_directories(frames, ec);
  pid_t cpid = -1;
  if (opt.capture && !frames.empty()) {
    fs::path shooter = rt::which(e, "weston-screenshooter");
    if (!shooter.empty()) {
      cpid = kg::fork_tied(SIGKILL);
      if (cpid == 0) {
        setsid();
        rt::Env se = wine_env;
        se.set("WAYLAND_DISPLAY", socket);
        fs::path scratch = frames / ".capture";
        for (int i = 0;; ++i) {
          sleep(i == 0 ? static_cast<unsigned>(opt.first_capture_after) : 60);
          fs::remove_all(scratch, ec);
          fs::create_directories(scratch, ec);
          ProcOptions po2;
          po2.capture = true;
          po2.cwd = scratch.string();
          if (!rt::run(se, shooter, {}, po2).ok()) continue;
          for (const fs::directory_entry& de : fs::directory_iterator(scratch, ec)) {
            if (de.path().extension() != ".png") continue;
            fs::copy_file(de.path(), last_frame_file(frames),
                          fs::copy_options::overwrite_existing, ec);
            if (opt.keep_every_frame) {
              char stamp[32];
              std::snprintf(stamp, sizeof(stamp), "frame-%03d.png", i);
              fs::copy_file(de.path(), frames / stamp, fs::copy_options::overwrite_existing, ec);
            }
            // Write-once, except over a stand-in. The install leaves a marker
            // beside the frame it took of the installer; the first play finds
            // it, replaces the tile with a frame of the actual game and takes
            // the marker away, and every play after that leaves both alone.
            //
            // This is set_title_art's rule written out again, not a call to
            // it, and it stays that way: the marker says "the installer" here
            // and a longer sentence there, and a copy that fails here still
            // writes the marker. Making them one changes bytes on disk.
            const fs::path title = title_art_file(frames);
            const fs::path standin = title_marker_file(frames);
            const bool take_it = !fs::exists(title, ec) ||
                                 (!opt.title_is_provisional && fs::exists(standin, ec));
            if (take_it) {
              fs::copy_file(de.path(), title, fs::copy_options::overwrite_existing, ec);
              if (opt.title_is_provisional) std::ofstream(standin) << "the installer\n";
              else fs::remove(standin, ec);
            }
          }
        }
        _exit(0);
      }
    }
  }
  return ScopedChild(cpid, SIGKILL, true);
}

// Starts the program itself, in `cwd`, and returns its pid.
//
// The game is forked here rather than through the process helper, because a
// helper beside it needs its pid: that is what pause-on-focus-loss stops and
// what tells the gamepad mapper when to go away.
//
// A group of its own only for the caller that has to kill one. `on_pgid` is
// that caller and the only one: "abandon this install" cannot wait for an
// installer that is waiting for a person, so it signals the whole group.
// Playing a game asks for no such thing, and must not be given it - a game
// in a group of its own is no longer in the terminal's foreground group, so
// Ctrl-C during `kretro play` reaches kretro and never the game. That is the
// one key everybody uses to stop a program, and it stopped working the day
// this became unconditional.
pid_t launch_program(const rt::Env& genv, const fs::path& prog, const std::vector<std::string>& args,
                     const fs::path& cwd, const CompositorOptions& opt,
                     const std::function<void(const std::string&)>& say) {
  say("launching " + prog.filename().string());
  const std::vector<std::string>& gargs = args;
  const bool own_group = static_cast<bool>(opt.on_pgid);
  pid_t gpid = fork();
  if (gpid < 0) throw std::runtime_error("cannot fork");
  if (gpid == 0) {
    // setpgid rather than setsid: the pid is unchanged, which is what waitpid
    // below and the input helper's --pid both address, and a new session would
    // give away the controlling terminal a CLI install still prints to.
    if (own_group) setpgid(0, 0);
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
    rt::exec(genv, prog, gargs);
  }
  if (own_group) {
    // Raced with the child on purpose: whichever runs first, the group exists.
    setpgid(gpid, gpid);
    opt.on_pgid(gpid);
  }
  return gpid;
}

// The gamepad helper, `kretro input` or the player's own, for the program
// `gpid` on `display`. Its pid, or -1 when there is none.
pid_t start_input_helper(const rt::Env& genv, const fs::path& home, const std::string& display,
                         pid_t gpid, const CompositorOptions& opt) {
  pid_t ipid = -1;
  // Not during an install. The stage is already injecting XTest events into
  // this display from the GUI process, which is also the process holding the
  // gamepad, and two writers on one pointer is a cursor that fights the hand
  // moving it.
  if (const char* app = env_nonempty("KRETRO_APP"); app && opt.input_helper) {
    ipid = kg::fork_tied(SIGTERM);
    if (ipid == 0) {
      setsid();
      int fd = open((home / "input.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }
      rt::Env ie = genv;
      // The state this process resolved, handed down: the helper is kretro or
      // the player again, started with the game's HOME, and a state chosen
      // afresh from that HOME is an empty one under the game's home - with no
      // pack in it for kretro's helper to read the bindings from, and no
      // settings.toml for the player's.
      ie.set("KRETRO_STATE", state_dir().string());
      InputHelperArgs ia;
      ia.display = display;
      ia.pid = gpid;
      ia.game = opt.socket_suffix;
      ia.pause = opt.pause_on_blur;
      rt::exec(ie, app, input_helper_argv(ia));
    }
  }
  return ipid;
}

// Waits for the program and returns its wait status. With `stop_after` set it
// is stopped once that many seconds have gone by.
int wait_program(pid_t gpid, int stop_after) {
  int gstatus = 0;
  if (stop_after > 0) {
    std::time_t deadline = std::time(nullptr) + stop_after;
    while (true) {
      pid_t w = waitpid(gpid, &gstatus, WNOHANG);
      if (w == gpid) break;
      if (std::time(nullptr) >= deadline) {
        kill(gpid, SIGCONT);  // a stopped process cannot act on SIGTERM
        kill(gpid, SIGTERM);
        waitpid(gpid, &gstatus, 0);
        break;
      }
      usleep(100000);
    }
  } else {
    waitpid(gpid, &gstatus, 0);
  }
  return gstatus;
}

// The program we launched may have been a stub that handed off to another
// Windows process. wineserver -w blocks until every process in the prefix
// has finished, which is the only reliable "the installer is done" signal.
//
// It is forked rather than run, because this is where an InstallShield
// install actually spends its time - and therefore where a stalled one has
// to be noticed - and it is waited for by asking waitpid, without blocking,
// every half second until it has gone.
void wait_for_prefix_idle(const rt::Env& e, const rt::Env& wine_env,
                          const std::function<void(const std::string&)>& say) {
  fs::path ws = rt::which(e, "wineserver");
  if (ws.empty()) return;
  say("waiting for the installer to finish");
  pid_t wp = kg::fork_tied(SIGTERM);
  if (wp == 0) {
    setsid();
    rt::exec(wine_env, ws, {"-w"});
  }
  if (wp > 0) {
    int status = 0;
    while (true) {
      if (waitpid(wp, &status, WNOHANG) == wp) break;
      usleep(500000);
    }
  }
}

// Wine keeps running after the game exits unless its server is told to stop.
void kill_wineserver(const rt::Env& e, const rt::Env& wine_env) {
  fs::path ws = rt::which(e, "wineserver");
  if (!ws.empty()) rt::run(wine_env, ws, {"-k"});
}

}  // namespace

bool set_title_art(const fs::path& frames, const fs::path& frame, bool provisional) {
  std::error_code ec;
  const fs::path title = title_art_file(frames);
  // The install's own mark, and the only thing that lets a later frame in.
  const fs::path standin = title_marker_file(frames);
  const bool mine = !fs::exists(title, ec) || (!provisional && fs::exists(standin, ec));
  if (!mine) return false;
  fs::create_directories(frames, ec);
  fs::copy_file(frame, title, fs::copy_options::overwrite_existing, ec);
  if (ec) return false;
  if (provisional) {
    std::ofstream f(standin);
    f << "this frame is of the installer, not the game\n";
  } else {
    fs::remove(standin, ec);
  }
  return true;
}

// Each step below is a function of its own, called in the order the steps
// have always run in: the compositor, then the X server on it, then the
// camera, the program and its gamepad helper. The children are ScopedChild
// locals declared in that order, so however this leaves they are ended in the
// reverse one - the camera, then Xwayland, then Weston.
CompositorResult run_in_compositor(const rt::Env& e, const rt::Env& wine_env,
                                  const fs::path& prog, const std::vector<std::string>& args,
                                  const fs::path& cwd, const CompositorOptions& opt,
                                  const std::function<void(const std::string&)>& say) {
  CompositorResult result;
  const fs::path& home = opt.home;

  const uint32_t w = opt.width ? opt.width : 800;
  const uint32_t h = opt.height ? opt.height : 600;
  const uint32_t s = opt.scale ? opt.scale : 1;

  write_weston_ini(e, home);
  Weston weston = start_weston(e, wine_env, opt, w, h, s, say);

  const int dispnum = free_x_display();
  const std::string display = ":" + std::to_string(dispnum);
  ScopedChild xwayland = start_xwayland(e, wine_env, home, weston.socket, display, dispnum, w, h);
  say("private display " + display);

  // The earliest moment anyone else may open this display, and therefore the
  // right one to hand it out. Not after the program starts: the stage wants to
  // be showing a black nested screen before the installer paints its first
  // window, because a person looking at an empty panel for four seconds
  // concludes it is broken and reaches for the mouse.
  //
  // This runs on whatever thread called run_in_compositor - the worker - and
  // the callback's only job is to hand the string to the UI thread.
  if (opt.on_display_ready) opt.on_display_ready(display);

  log_gl_renderer(home / "weston.log", say);

  rt::Env genv = wine_env;
  genv.set("DISPLAY", display);
  // Wine must not find a Wayland display, or it may talk to the host
  // compositor directly and escape the nested screen entirely.
  genv.set("WAYLAND_DISPLAY", "");

  ScopedChild capture = start_frame_capture(e, wine_env, weston.socket, opt);

  const pid_t gpid = launch_program(genv, prog, args, cwd, opt, say);
  const pid_t ipid = start_input_helper(genv, home, display, gpid, opt);

  const int gstatus = wait_program(gpid, opt.stop_after);
  if (ipid > 0) { kill(ipid, SIGTERM); int st; waitpid(ipid, &st, 0); }
  result.status = WIFEXITED(gstatus) ? WEXITSTATUS(gstatus) : -1;

  if (opt.wait_for_processes) wait_for_prefix_idle(e, wine_env, say);

  kill_wineserver(e, wine_env);
  return result;
}

}  // namespace kg::session
