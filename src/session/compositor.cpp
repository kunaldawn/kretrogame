#include "compositor.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
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

using detail::pointer_capture_on;
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

// A descriptor closed when this goes, and with it the flock held on it.
struct Claim {
  int fd = -1;
  Claim() = default;
  explicit Claim(int f) : fd(f) {}
  Claim(Claim&& o) noexcept : fd(o.fd) { o.fd = -1; }
  Claim(const Claim&) = delete;
  Claim& operator=(const Claim&) = delete;
  Claim& operator=(Claim&&) = delete;
  ~Claim() {
    if (fd >= 0) close(fd);
  }
};

// A running Weston, and the name of the Wayland socket it answers on. The
// claim on the name is declared first so it is let go last, once the
// compositor is gone.
struct Weston {
  Claim claim;
  ScopedChild child;
  std::string socket;
};

}  // namespace

// libwayland holds an flock on <socket>.lock for as long as the compositor
// that made the socket lives, so a lock nobody holds is a socket left by a
// Weston that was killed. That one is removed: left there, we would see the
// socket, believe our compositor is up, and then watch Xwayland fail to
// connect to a dead endpoint. A held lock is another session's live screen -
// the same game played from another state directory, or a player and kretro
// side by side - and removing its socket took that session's display away
// from everything that looks it up by name.
//
// That lock is only taken once a Weston is up, and two sessions starting
// together would both see the name free, and the second would remove the
// first one's socket as it appeared. So a name is first claimed with an flock
// of our own on <socket>.kretro, taken before anything is looked at and held
// until the compositor is gone: whoever holds it has the name, and another
// kretro, in this process or another, goes on to the next.
SocketName claim_socket_name(const fs::path& dir, const std::string& suffix) {
  std::error_code ec;
  const std::string stem = "kretro-" + suffix;
  for (int n = 1; n < 100; ++n) {
    const std::string name = n == 1 ? stem : stem + "-" + std::to_string(n);
    Claim claim(open((dir / (name + ".kretro")).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
    if (claim.fd < 0 || flock(claim.fd, LOCK_EX | LOCK_NB) != 0) continue;
    const fs::path lock = dir / (name + ".lock");
    const int fd = open(lock.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
      const bool live = flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK;
      close(fd);
      if (live) continue;
    }
    fs::remove(dir / name, ec);
    fs::remove(lock, ec);
    SocketName out;
    out.name = name;
    out.claim_fd = claim.fd;
    claim.fd = -1;
    return out;
  }
  throw std::runtime_error("no free Wayland socket name for " + stem);
}

namespace {

// `close_fd`, when not -1, is Weston's end of the socket a close of the
// window is reported on (CompositorOptions::close_image), and is handed to it
// open.
Weston start_weston(const rt::Env& e, const rt::Env& wine_env, const CompositorOptions& opt,
                    uint32_t w, uint32_t h, uint32_t s, int close_fd,
                    const std::function<void(const std::string&)>& say) {
  std::error_code ec;
  const fs::path& home = opt.home;
  const char* xdg = env_nonempty("XDG_RUNTIME_DIR");
  const fs::path run_dir = xdg ? fs::path(xdg) : fs::path("/tmp");
  SocketName claimed = claim_socket_name(run_dir, opt.socket_suffix);
  Claim claim(claimed.claim_fd);
  const std::string& socket = claimed.name;
  std::vector<std::string> wargs;
  const bool wayland_host = env_nonempty("WAYLAND_DISPLAY") != nullptr;
  const bool pointer_capture = pointer_capture_on(opt.pointer_capture, env_nonempty("KRETRO_WESTON_CAPTURE"));
  // Weston runs as an ordinary client of whatever the host has, which is what
  // keeps this on the well-trodden path on both Wayland and X11 hosts - unless
  // nobody is meant to see it, in which case it renders into a buffer and no
  // window appears anywhere. Our own Xwayland is a client of this Weston
  // either way, for the reason given above the weston.ini: we want a root
  // window that is one surface, and no X window manager in the picture.
  wargs.push_back(opt.headless
                      ? "--backend=headless"
                      : (wayland_host ? "--backend=wayland" : "--backend=x11"));
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
  } else if (opt.fullscreen && !wayland_host) {
    // The x11 backend is stock: fullscreen is the whole X screen at 1x.
    wargs.push_back("--fullscreen");
    say("display: fullscreen, the game renders at " + std::to_string(w) + "x" + std::to_string(h));
  } else {
    // The window's size and scale, fullscreen or not. The runtime's wayland
    // backend is patched (runtime/weston/) to treat --fullscreen as the state
    // the window starts in: it keeps the output at the game's size and scales
    // it by the largest whole number that fits the host's screen, and Alt+F11
    // or Ctrl+Alt+F goes back to exactly this window. Stock Weston ignores
    // these with --fullscreen and puts the game in a corner at 1x.
    const uint32_t ws = opt.window_scale ? opt.window_scale : s;
    wargs.push_back("--width=" + std::to_string(w));
    wargs.push_back("--height=" + std::to_string(h));
    wargs.push_back("--scale=" + std::to_string(ws));
    if (opt.fullscreen) {
      wargs.push_back("--fullscreen");
      say("display: fullscreen, the game renders at " + std::to_string(w) + "x" +
          std::to_string(h) + ", at " + std::to_string(ws) + "x as a window");
    } else {
      say("display: " + std::to_string(w) + "x" + std::to_string(h) + " at " + std::to_string(ws) +
          "x -> " + std::to_string(w * ws) + "x" + std::to_string(h * ws) + " window");
    }
    // Capture is a promise only a game session makes, so only it hears about
    // the click and Ctrl+Alt.
    if (wayland_host)
      say(pointer_capture ? "keys: Alt+F11 fullscreen, click to capture the mouse, Ctrl+Alt frees it"
                          : "keys: Alt+F11 fullscreen");
  }

  fs::path weston = rt::which(e, "weston");
  if (weston.empty()) throw std::runtime_error("no weston in this runtime");
  fs::path weston_log = home / "weston.log";
  fs::remove(weston_log, ec);
  rt::Env wenv = wine_env;
  wenv.set("WESTON_CONFIG_FILE", (home / ".config" / "weston.ini").string());
  if (!opt.headless && wayland_host) {
    // What the patched wayland backend needs to know (runtime/weston/): the
    // game's screen, which fullscreen letterboxes, and the window's scale,
    // which it restores. Only the wayland backend is patched; an X11 host's
    // Weston runs the stock x11 backend, which has no use for them.
    const uint32_t ws = opt.window_scale ? opt.window_scale : s;
    wenv.set("KRETRO_WESTON_GAME", std::to_string(w) + "x" + std::to_string(h));
    wenv.set("KRETRO_WESTON_WINDOW_SCALE", std::to_string(ws));
    // A click locks the host pointer to the window, so the game's pointer
    // stops at its edges and the game gets every movement. Somebody for whom
    // that is wrong - a desktop that refuses pointer locks, a tablet - sets
    // KRETRO_WESTON_CAPTURE=0 and it is left as they set it. Set either way,
    // because Weston inherits kretro's environment: an installer must not be
    // captured just because the person exported KRETRO_WESTON_CAPTURE=1.
    wenv.set("KRETRO_WESTON_CAPTURE", pointer_capture ? "1" : "0");
    // Set either way too: a close only asks when this session can answer.
    wenv.set("KRETRO_WESTON_CLOSE_FD", close_fd >= 0 ? std::to_string(close_fd) : "");
  }

  pid_t wpid = kg::fork_tied(SIGTERM);
  if (wpid < 0) throw std::runtime_error("cannot fork");
  if (wpid == 0) {
    int fd = open(weston_log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }
    setsid();
    if (close_fd >= 0) fcntl(close_fd, F_SETFD, 0);  // made close-on-exec; this one exec keeps it
    rt::exec(wenv, weston, wargs);
  }

  ScopedChild child(wpid, SIGTERM, false);

  const fs::path sock = run_dir / socket;
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
  return Weston{std::move(claim), std::move(child), socket};
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
//
// With `fullscreen` it asks the nested shell for the whole output, and that is
// what lets it emulate a RandR mode: a program that sets 640x480 gets an X
// screen of 640x480, which Xwayland scales up to the output it was given.
ScopedChild start_xwayland(const rt::Env& e, const rt::Env& wine_env, const fs::path& home,
                           const std::string& socket, const std::string& display, int dispnum,
                           uint32_t w, uint32_t h, bool fullscreen) {
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
    std::vector<std::string> xargs = {display, "-geometry", std::to_string(w) + "x" + std::to_string(h),
                                      "-noreset"};
    if (fullscreen) xargs.push_back("-fullscreen");
    rt::exec(xenv, xwayland, xargs);
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

// How long one photograph of the nested screen may take.
constexpr int kCaptureTimeoutSec = 5;

// A helper that photographs the nested screen while the game runs. The first
// stable frame becomes the game's tile art - unless an install left a
// stand-in there, which the first play replaces; the most recent one is what
// you are shown when you come back months later and cannot remember where
// you were. Neither needs anything from the game itself, because we own the
// compositor it is drawing into.
//
// The child is forked and never execs, and takes a session of its own, so it
// is ended with SIGKILL to its whole group - the screenshooter it may be
// waiting on with it - and the session's end never waits for a photograph.
// The frames directory is made whether or not there is anything to capture.
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
          // weston-screenshooter waits for a frame, and a host that has
          // stopped drawing the window - minimised, on another workspace -
          // sends none. A photograph that does not come in seconds is not
          // coming; the next one is a minute away.
          po2.timeout_sec = kCaptureTimeoutSec;
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
// is stopped once that many seconds have gone by. `close_fd`, when not -1, is
// where the window's close button is heard (see CompositorOptions::close_image),
// and `on_close` is told each time, with how many closes there have been.
int wait_program(pid_t gpid, int stop_after, int close_fd, const std::function<void(int)>& on_close) {
  int gstatus = 0;
  if (stop_after <= 0 && close_fd < 0) {
    waitpid(gpid, &gstatus, 0);
    return gstatus;
  }
  const std::time_t deadline = stop_after > 0 ? std::time(nullptr) + stop_after : 0;
  int closes = 0;
  while (true) {
    pid_t w = waitpid(gpid, &gstatus, WNOHANG);
    if (w == gpid) break;
    if (deadline && std::time(nullptr) >= deadline) {
      kill(gpid, SIGCONT);  // a stopped process cannot act on SIGTERM
      kill(gpid, SIGTERM);
      waitpid(gpid, &gstatus, 0);
      break;
    }
    if (close_fd < 0) {
      usleep(100000);
      continue;
    }
    pollfd pfd{close_fd, POLLIN, 0};
    if (poll(&pfd, 1, 100) <= 0) continue;
    char buf[16];
    const ssize_t n = recv(close_fd, buf, sizeof buf, MSG_DONTWAIT);
    if (n > 0) {
      // Clicks that came in together are one close: a second is somebody
      // who has seen the first go unanswered.
      on_close(++closes);
    } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
      close_fd = -1;  // Weston has gone, and the program will follow it
    }
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
  // The socket a close of the window is heard on, when this session has a
  // program to ask and a window to close: the patched backend is the wayland
  // one, and headless has no window. Ours is `listen`; Weston's end is closed
  // here once Weston has its own copy.
  Claim listen;
  Claim theirs;
  if (int sv[2]; !opt.close_image.empty() && !opt.headless && env_nonempty("WAYLAND_DISPLAY") &&
                 socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0) {
    listen.fd = sv[0];
    theirs.fd = sv[1];
  }
  Weston weston = start_weston(e, wine_env, opt, w, h, s, theirs.fd, say);
  // Before anything else is started, so Weston holds the only other end and
  // its going is an end of file here.
  if (theirs.fd >= 0) {
    close(theirs.fd);
    theirs.fd = -1;
  }

  const int dispnum = free_x_display();
  const std::string display = ":" + std::to_string(dispnum);
  ScopedChild xwayland =
      start_xwayland(e, wine_env, home, weston.socket, display, dispnum, w, h, opt.emulate_modes);
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

  // The first close asks the program to quit, as closing its window on
  // Windows would, and a game that saves as it exits gets to. The second is
  // somebody for whom that did not work - a game that ignores WM_CLOSE, or
  // one that has hung - and it ends everything in the prefix at once.
  const auto on_close = [&](int closes) {
    if (closes == 1) {
      say("the window was closed: asking " + opt.close_image +
          " to quit (close it again to end it at once)");
      rt::run(genv, prog, {"taskkill", "/im", opt.close_image});
    } else {
      say("the window was closed again: ending " + opt.close_image);
      kill_wineserver(e, wine_env);
    }
  };
  const int gstatus = wait_program(gpid, opt.stop_after, listen.fd, on_close);
  if (ipid > 0) { kill(ipid, SIGTERM); int st; waitpid(ipid, &st, 0); }
  result.status = WIFEXITED(gstatus) ? WEXITSTATUS(gstatus) : -1;

  if (opt.wait_for_processes) wait_for_prefix_idle(e, wine_env, say);

  kill_wineserver(e, wine_env);
  return result;
}

}  // namespace kg::session
