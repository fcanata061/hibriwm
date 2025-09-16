// mywm_skeleton.cpp
// Skeleton C++ implementation for a configurable X11 Window Manager (mywm)
// - Uses xcb for X11 communication
// - Exposes a UNIX domain IPC protocol for scripts (text commands + JSON events)
// - Designed so all substantive functions are declared and documented; implementers
//   can fill in function bodies later and know exactly what each function must do.
//
// Build (example): g++ mywm_skeleton.cpp -o mywm -lxcb -lpthread -lstdc++fs
// NOTE: This file is a single compilation unit that sketches all modules. Many
// helper functions are left as TODO for clarity. Use this as the authoritative
// reference for function names, parameters, and expected behavior.

#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/xcb_event.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <fcntl.h>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <queue>
#include <atomic>
#include <iostream>
#include <sstream>
#include <functional>
#include <filesystem>
#include <nlohmann/json.hpp> // requires nlohmann/json single-header, used for event payloads

using json = nlohmann::json;
namespace fs = std::filesystem;

// -----------------------------
// Configuration constants
// -----------------------------
static const char *SOCK_PATH = "/tmp/mywm.sock"; // runtime socket
static const char *CONFIG_PATH = "/home/user/.config/mywm/config.sh"; // example

// -----------------------------
// Forward declarations & types
// -----------------------------
using WindowID = uint32_t;
struct Geometry { int x,y,w,h; };

enum BorderType { INNER_BORDER, OUTER_BORDER };

// Event types sent to bar/clients
struct WmEvent {
    std::string type; // e.g., "workspace", "focus", "bar-toggle"
    json payload;
};

// -----------------------------
// Utility functions
// -----------------------------
static std::string hex_color_sanitize(const std::string &c) {
    // TODO: validate/normalize colors (#rrggbb) and return a canonical form
    return c;
}

// -----------------------------
// X Connection wrapper
// -----------------------------
class XConnection {
public:
    XConnection();
    ~XConnection();

    // Connect / disconnect
    bool connect();
    void disconnect();

    // Accessors
    xcb_connection_t* conn() { return conn_; }
    int screen_number() const { return screen_num_; }

    // Event helpers
    xcb_window_t root() const { return root_; }

    // Grab keys / buttons
    void grab_key(uint16_t keycode, uint16_t modifiers);
    void grab_button(uint8_t button, uint16_t modifiers);

    // send simple client messages or EWMH updates (helpers)
    void set_wm_name(const std::string &name);

private:
    xcb_connection_t *conn_ = nullptr;
    const xcb_setup_t *setup_ = nullptr;
    xcb_screen_t *screen_ = nullptr;
    int screen_num_ = 0;
    xcb_window_t root_ = 0;
};

// -----------------------------
// Frame (decoration) handling
// -----------------------------
class Frame {
public:
    Frame(XConnection &xc, WindowID client);
    ~Frame();

    // Create/destroy frame around client window (reparent)
    void create();
    void destroy();

    // Draw borders/decoration using Cairo (TODO: integrate Cairo)
    void draw();

    // Geometry helpers
    void move_resize(const Geometry &g);

    WindowID client() const { return client_; }
    WindowID frame_win() const { return frame_win_; }

    // Border configuration
    void set_border_width(BorderType t, int w);
    void set_border_color(BorderType t, const std::string &hex);

private:
    XConnection &xc_;
    WindowID client_;
    WindowID frame_win_ = 0; // the window we create and parent the client to
    Geometry geom_ = {0,0,0,0};
    int inner_width_ = 2;
    int outer_width_ = 4;
    std::string inner_color_ = "#222222";
    std::string outer_color_ = "#111111";
};

// -----------------------------
// Window model
// -----------------------------
struct WmWindow {
    WindowID id;
    std::unique_ptr<Frame> frame;
    bool mapped = false;
    bool floating = false;
    bool scratch = false;
    int workspace = 0;
    Geometry geom_tiled{};
    Geometry geom_floating{};
    std::string title;
    std::string cls; // WM_CLASS
    bool fullscreen = false;
};

// -----------------------------
// Monitor & Workspace
// -----------------------------
struct Monitor {
    int x,y,w,h;
    int id;
    std::vector<int> workspaces; // indices
};

struct Workspace {
    int index;
    std::vector<WindowID> tiled;  // layout-managed windows
    std::vector<WindowID> floating; // floating windows
    int monitor_id = 0;
    bool visible = false;
};

// -----------------------------
// Layout interface and BSP implementation (skeleton)
// -----------------------------
class Layout {
public:
    virtual ~Layout() = default;
    // Apply layout to workspace, adjusting window geometries in wm_windows map
    virtual void apply(Workspace &ws, std::map<WindowID, WmWindow> &wm_windows, const Monitor &m) = 0;

    // Optional helpers
    virtual void focus_next(Workspace &ws) {}
    virtual void focus_prev(Workspace &ws) {}
};

class BSPLayout : public Layout {
public:
    BSPLayout();
    ~BSPLayout() override;
    void apply(Workspace &ws, std::map<WindowID, WmWindow> &wm_windows, const Monitor &m) override;
    // Provide swap/move operations
    void promote(WindowID id, Workspace &ws, std::map<WindowID, WmWindow> &wm_windows);
    void swap(WindowID a, WindowID b, Workspace &ws);
};

// -----------------------------
// Rules engine (matchers -> actions)
// -----------------------------
struct Rule {
    std::string match_class; // e.g., "Firefox"
    std::optional<int> workspace;
    std::optional<int> monitor_id;
    std::optional<bool> floating;
    std::optional<std::string> area; // relative geometry string
};

class RulesEngine {
public:
    void add_rule(const Rule &r);
    std::optional<Rule> match(WindowID id, const WmWindow &w);
private:
    std::vector<Rule> rules_;
};

// -----------------------------
// IPC Server: accepts commands, pushes them to the main loop
// -----------------------------
class IPCServer {
public:
    using CommandHandler = std::function<void(const std::string&)>;

    IPCServer(const std::string &sockpath);
    ~IPCServer();

    // Start listening in a background thread. Commands will be forwarded to handler.
    void start(CommandHandler handler);
    void stop();

    // Emit events to subscribed clients (bar). This writes JSON lines to connected clients.
    void emit_event(const WmEvent &ev);

private:
    std::string sockpath_;
    int server_fd_ = -1;
    std::thread accept_thread_;
    std::atomic<bool> running_{false};

    std::mutex clients_mtx_;
    std::vector<int> client_fds_; // simple list of connected clients for events

    // internal helpers
    void accept_loop(CommandHandler handler);
    void handle_client(int client_fd, CommandHandler handler);
    void send_to_clients(const std::string &s);
};

// -----------------------------
// Input manager: handles key & mouse binds, and converts into commands
// -----------------------------
class InputManager {
public:
    InputManager(XConnection &xc, IPCServer &ipc);
    ~InputManager();

    void register_default_bindings();
    void bind_key(const std::string &keycombo, const std::string &cmd);
    void bind_button(const std::string &btncombo, const std::string &cmd);

    // Called by main loop on KeyPress/ButtonPress events so we can route them
    void handle_key_event(xcb_key_press_event_t *ev);
    void handle_button_event(xcb_button_press_event_t *ev);

private:
    XConnection &xc_;
    IPCServer &ipc_;
    std::map<std::string, std::string> keymap_; // simple mapping
    std::map<std::string, std::string> btnmap_;
};

// -----------------------------
// Config loader: runs a shell script or reads a simple config and emits commands
// -----------------------------
class ConfigLoader {
public:
    ConfigLoader(const std::string &path, IPCServer &ipc);
    ~ConfigLoader();

    // Run once (synchronous) to load config
    void run_once();

    // Start inotify watcher to reload on change and call reload_callback
    void watch(std::function<void()> reload_callback);

private:
    std::string path_;
    IPCServer &ipc_;
    int inotify_fd_ = -1;
    std::thread watch_thread_;
    std::atomic<bool> watching_{false};
};

// -----------------------------
// Bar publisher: publishes events/state so an external script can render the bar
// -----------------------------
class BarPublisher {
public:
    BarPublisher(IPCServer &ipc);
    ~BarPublisher();

    // push state events
    void publish_workspace(int current, const std::vector<int> &occupied);
    void publish_focus(WindowID id, const std::string &title);
    void publish_bar_visible(bool visible);

private:
    IPCServer &ipc_;
};

// -----------------------------
// WindowManager (core) - orchestrates everything
// -----------------------------
class WindowManager {
public:
    WindowManager();
    ~WindowManager();

    bool init();
    void run(); // main event loop. Blocks until quit
    void stop();

    // Core operations (called from IPC command handlers or input manager)
    void cmd_spawn(const std::string &cmdline, std::optional<int> workspace_area = std::nullopt);
    void cmd_focus_direction(const std::string &dir);
    void cmd_move_direction(const std::string &dir);
    void cmd_resize_rel(int dx, int dy);
    void cmd_toggle_float(WindowID id);
    void cmd_swap(WindowID a, WindowID b);
    void cmd_send_to_ws(WindowID id, int ws, bool follow);
    void cmd_view_ws(int ws);
    void cmd_toggle_bar();
    void cmd_scratch_toggle(const std::string &name);
    void cmd_set_border(BorderType type, int width);
    void cmd_set_color(BorderType type, const std::string &hex);
    void cmd_reload_config();
    void cmd_quit();

    // Event handlers from X
    void handle_map_request(xcb_map_request_event_t *ev);
    void handle_unmap_notify(xcb_unmap_notify_event_t *ev);
    void handle_configure_request(xcb_configure_request_event_t *ev);
    void handle_key_press(xcb_key_press_event_t *ev);
    void handle_button_press(xcb_button_press_event_t *ev);

private:
    XConnection xc_;
    IPCServer ipc_ {SOCK_PATH};
    InputManager *input_ = nullptr;
    ConfigLoader *cfg_ = nullptr;
    BarPublisher *bar_ = nullptr;
    RulesEngine rules_;

    std::atomic<bool> running_{false};

    // State
    std::map<WindowID, WmWindow> windows_;
    std::map<int, Workspace> workspaces_;
    std::map<int, Monitor> monitors_;
    int current_ws_ = 1;
    std::unique_ptr<Layout> layout_; // e.g., BSPLayout

    // Thread-safety primitives
    std::shared_mutex state_mtx_; // protects windows_/workspaces_/monitors_

    // Helpers
    void adopt_new_window(WindowID id);
    void reparent_to_frame(WindowID id);
    void remove_window(WindowID id);
    void update_struts_and_area();
    void notify_workspace_change();
};

// -----------------------------
// Implementation skeletons
// -----------------------------

// XConnection implementation
XConnection::XConnection() {}
XConnection::~XConnection() { disconnect(); }

bool XConnection::connect() {
    conn_ = xcb_connect(nullptr, &screen_num_);
    if (xcb_connection_has_error(conn_)) return false;
    setup_ = xcb_get_setup(conn_);
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup_);
    for (int i=0;i<screen_num_;++i) xcb_screen_next(&iter);
    screen_ = iter.data;
    if (!screen_) return false;
    root_ = screen_->root;
    return true;
}

void XConnection::disconnect() {
    if (conn_) { xcb_disconnect(conn_); conn_ = nullptr; }
}

void XConnection::grab_key(uint16_t keycode, uint16_t modifiers) {
    // TODO: call xcb_grab_key with proper mapping
}
void XConnection::grab_button(uint8_t button, uint16_t modifiers) {
    // TODO: call xcb_grab_button
}
void XConnection::set_wm_name(const std::string &name) {
    // TODO: set _NET_WM_NAME or X11 name
}

// Frame implementation (skeleton)
Frame::Frame(XConnection &xc, WindowID client): xc_(xc), client_(client) {}
Frame::~Frame() { destroy(); }
void Frame::create() {
    // TODO: create a new window (frame_win_), reparent client_ into it, set event masks
}
void Frame::destroy() {
    // TODO: unparent, destroy frame if exists
}
void Frame::draw() {
    // TODO: draw borders using cairo or XCB poly functions
}
void Frame::move_resize(const Geometry &g) {
    geom_ = g;
    // TODO: xcb configure window positions for frame and client
}
void Frame::set_border_width(BorderType t, int w) {
    if (t==INNER_BORDER) inner_width_ = w; else outer_width_ = w;
}
void Frame::set_border_color(BorderType t, const std::string &hex) {
    if (t==INNER_BORDER) inner_color_ = hex_color_sanitize(hex); else outer_color_ = hex_color_sanitize(hex);
}

// BSPLayout skeleton
BSPLayout::BSPLayout() {}
BSPLayout::~BSPLayout() {}
void BSPLayout::apply(Workspace &ws, std::map<WindowID, WmWindow> &wm_windows, const Monitor &m) {
    // TODO: compute tree partitions and set wm_windows[id].geom_tiled accordingly
}
void BSPLayout::promote(WindowID id, Workspace &ws, std::map<WindowID, WmWindow> &wm_windows) {
    // TODO: move id to master area
}
void BSPLayout::swap(WindowID a, WindowID b, Workspace &ws) {
    // TODO: swap positions of two windows in the layout
}

// RulesEngine skeleton
void RulesEngine::add_rule(const Rule &r) { rules_.push_back(r); }
std::optional<Rule> RulesEngine::match(WindowID id, const WmWindow &w) {
    // TODO: match rule by class/title/exe etc. For now simple class match
    for (auto &r : rules_) if (!r.match_class.empty() && r.match_class==w.cls) return r;
    return std::nullopt;
}

// IPCServer implementation (skeleton)
IPCServer::IPCServer(const std::string &sockpath): sockpath_(sockpath) {}
IPCServer::~IPCServer() { stop(); }

void IPCServer::start(CommandHandler handler) {
    running_ = true;
    // Create server socket
    server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_<0) return;
    sockaddr_un addr; memset(&addr,0,sizeof(addr)); addr.sun_family = AF_UNIX; strncpy(addr.sun_path, sockpath_.c_str(), sizeof(addr.sun_path)-1);
    unlink(sockpath_.c_str());
    bind(server_fd_, (sockaddr*)&addr, sizeof(addr));
    listen(server_fd_, 8);
    accept_thread_ = std::thread(&IPCServer::accept_loop, this, handler);
}

void IPCServer::stop() {
    running_ = false;
    if (server_fd_>=0) close(server_fd_);
    if (accept_thread_.joinable()) accept_thread_.join();
    // close all client fds
    std::lock_guard<std::mutex> lk(clients_mtx_);
    for (int fd : client_fds_) close(fd);
    client_fds_.clear();
}

void IPCServer::accept_loop(CommandHandler handler) {
    while (running_) {
        sockaddr_un client_addr; socklen_t len = sizeof(client_addr);
        int client = accept(server_fd_, (sockaddr*)&client_addr, &len);
        if (client < 0) { usleep(10000); continue; }
        // Spawn a thread per client to read one-line commands; also add to event clients
        {
            std::lock_guard<std::mutex> lk(clients_mtx_);
            client_fds_.push_back(client);
        }
        std::thread(&IPCServer::handle_client, this, client, handler).detach();
    }
}

void IPCServer::handle_client(int client_fd, CommandHandler handler) {
    // Simple loop: read lines and forward to handler
    constexpr size_t BUF_SZ = 1024;
    char buf[BUF_SZ];
    ssize_t r;
    std::string acc;
    while ((r = read(client_fd, buf, BUF_SZ-1))>0) {
        buf[r]=0; acc += buf;
        size_t pos;
        while ((pos = acc.find('\n'))!=std::string::npos) {
            std::string line = acc.substr(0,pos);
            acc.erase(0,pos+1);
            // Trim
            while(!line.empty() && (line.back()=='\r' || line.back()==' ')) line.pop_back();
            if (!line.empty()) handler(line);
            // reply quick OK
            write(client_fd, "OK\n", 3);
        }
    }
    // client disconnected -> remove from client list
    {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        client_fds_.erase(std::remove(client_fds_.begin(), client_fds_.end(), client_fd), client_fds_.end());
    }
    close(client_fd);
}

void IPCServer::emit_event(const WmEvent &ev) {
    json j; j["event"] = ev.type; j["payload"] = ev.payload;
    send_to_clients(j.dump()+"\n");
}

void IPCServer::send_to_clients(const std::string &s) {
    std::lock_guard<std::mutex> lk(clients_mtx_);
    for (int fd : client_fds_) {
        ssize_t r = write(fd, s.c_str(), s.size()); (void)r;
    }
}

// InputManager skeleton
InputManager::InputManager(XConnection &xc, IPCServer &ipc): xc_(xc), ipc_(ipc) {}
InputManager::~InputManager() {}
void InputManager::register_default_bindings() {
    // Example
    bind_key("Mod4-Return","spawn st");
}
void InputManager::bind_key(const std::string &keycombo, const std::string &cmd){ keymap_[keycombo]=cmd; }
void InputManager::bind_button(const std::string &btncombo, const std::string &cmd){ btnmap_[btncombo]=cmd; }
void InputManager::handle_key_event(xcb_key_press_event_t *ev) {
    // TODO: translate ev to keycombo string then lookup in keymap_ and forward to ipc via a synthetic command
}
void InputManager::handle_button_event(xcb_button_press_event_t *ev) {
    // TODO: translate button -> btncombo and handle
}

// ConfigLoader skeleton
ConfigLoader::ConfigLoader(const std::string &path, IPCServer &ipc): path_(path), ipc_(ipc) {}
ConfigLoader::~ConfigLoader() { stop(); }
void ConfigLoader::run_once() {
    // Execute the shell config script and pipe lines to the IPC socket (or directly call handler)
    if (!fs::exists(path_)) return;
    std::string cmd = "/bin/sh '" + path_ + "'";
    // For simplicity we'll run the script and read its stdout which should contain "COMMAND lines"
    FILE *p = popen(cmd.c_str(), "r");
    if (!p) return;
    char buf[512];
    while (fgets(buf, sizeof(buf), p)) {
        std::string line(buf);
        while (!line.empty() && (line.back()=='\n' || line.back()=='\r')) line.pop_back();
        if (!line.empty()) {
            // naive: open a one-shot client to local socket and send the command
            int sock = socket(AF_UNIX, SOCK_STREAM, 0);
            if (sock>=0) {
                sockaddr_un addr; memset(&addr,0,sizeof(addr)); addr.sun_family=AF_UNIX; strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path)-1);
                if (connect(sock, (sockaddr*)&addr, sizeof(addr))==0) {
                    std::string out = line+"\n"; write(sock, out.c_str(), out.size()); close(sock);
                } else close(sock);
            }
        }
    }
    pclose(p);
}

void ConfigLoader::watch(std::function<void()> reload_callback) {
    watching_ = true;
    inotify_fd_ = inotify_init1(IN_NONBLOCK);
    if (inotify_fd_ < 0) return;
    int wd = inotify_add_watch(inotify_fd_, path_.c_str(), IN_MODIFY | IN_CLOSE_WRITE);
    watch_thread_ = std::thread([this, reload_callback, wd]() {
        constexpr size_t BUF=1024;
        char buf[BUF];
        while (watching_) {
            ssize_t len = read(inotify_fd_, buf, BUF);
            if (len>0) {
                // naive: any event triggers reload
                reload_callback();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (wd>=0) inotify_rm_watch(inotify_fd_, wd);
        close(inotify_fd_);
    });
}

// BarPublisher skeleton
BarPublisher::BarPublisher(IPCServer &ipc): ipc_(ipc) {}
BarPublisher::~BarPublisher() {}
void BarPublisher::publish_workspace(int current, const std::vector<int> &occupied) {
    WmEvent e; e.type = "workspace"; e.payload["index"] = current; e.payload["occupied"] = occupied;
    ipc_.emit_event(e);
}
void BarPublisher::publish_focus(WindowID id, const std::string &title) {
    WmEvent e; e.type = "focus"; e.payload["win"] = id; e.payload["title"] = title;
    ipc_.emit_event(e);
}
void BarPublisher::publish_bar_visible(bool visible) {
    WmEvent e; e.type = "bar-toggle"; e.payload["visible"] = visible;
    ipc_.emit_event(e);
}

// WindowManager implementation skeleton
WindowManager::WindowManager() : ipc_(SOCK_PATH) {
    layout_ = std::make_unique<BSPLayout>();
}
WindowManager::~WindowManager() { stop(); }

bool WindowManager::init() {
    if (!xc_.connect()) return false;
    // start IPC server and hand it a handler that parses commands -> methods
    ipc_.start([this](const std::string &cmdline){
        // VERY simple parsing: split by spaces; production should use quoted parsing
        std::istringstream iss(cmdline);
        std::string cmd; iss >> cmd;
        if (cmd=="spawn") { std::string rest; getline(iss, rest); cmd_spawn(rest); }
        else if (cmd=="view") { int ws; iss >> ws; cmd_view_ws(ws); }
        else if (cmd=="togglebar") cmd_toggle_bar();
        else if (cmd=="set-border") { std::string which; int w; iss>>which>>w; cmd_set_border(which=="inner"?INNER_BORDER:OUTER_BORDER,w); }
        else if (cmd=="set-color") { std::string which, col; iss>>which>>col; cmd_set_color(which=="inner"?INNER_BORDER:OUTER_BORDER,col); }
        else if (cmd=="reload-config") cmd_reload_config();
        else if (cmd=="quit") cmd_quit();
        // TODO: many more commands
    });

    input_ = new InputManager(xc_, ipc_);
    input_->register_default_bindings();

    cfg_ = new ConfigLoader(CONFIG_PATH, ipc_);
    cfg_->run_once();
    cfg_->watch([this](){ this->cmd_reload_config(); });

    bar_ = new BarPublisher(ipc_);

    running_ = true;
    return true;
}

void WindowManager::run() {
    // Main event loop
    xcb_connection_t *c = xc_.conn();
    xcb_generic_event_t *ev;
    while (running_) {
        ev = xcb_wait_for_event(c);
        if (!ev) break;
        uint8_t type = ev->response_type & ~0x80;
        switch (type) {
            case XCB_MAP_REQUEST: handle_map_request((xcb_map_request_event_t*)ev); break;
            case XCB_UNMAP_NOTIFY: handle_unmap_notify((xcb_unmap_notify_event_t*)ev); break;
            case XCB_CONFIGURE_REQUEST: handle_configure_request((xcb_configure_request_event_t*)ev); break;
            case XCB_KEY_PRESS: handle_key_press((xcb_key_press_event_t*)ev); break;
            case XCB_BUTTON_PRESS: handle_button_press((xcb_button_press_event_t*)ev); break;
            default: break;
        }
        free(ev);
    }
}

void WindowManager::stop() {
    running_ = false;
    ipc_.stop();
    if (cfg_) { delete cfg_; cfg_ = nullptr; }
    if (input_) { delete input_; input_ = nullptr; }
}

// Command stubs
void WindowManager::cmd_spawn(const std::string &cmdline, std::optional<int> workspace_area) { 
    // TODO: fork/exec; use rules to place on workspace/area
}
void WindowManager::cmd_focus_direction(const std::string &dir) { /* TODO */ }
void WindowManager::cmd_move_direction(const std::string &dir) { /* TODO */ }
void WindowManager::cmd_resize_rel(int dx, int dy) { /* TODO */ }
void WindowManager::cmd_toggle_float(WindowID id) { /* TODO */ }
void WindowManager::cmd_swap(WindowID a, WindowID b) { /* TODO */ }
void WindowManager::cmd_send_to_ws(WindowID id, int ws, bool follow) { /* TODO */ }
void WindowManager::cmd_view_ws(int ws) { current_ws_ = ws; notify_workspace_change(); }
void WindowManager::cmd_toggle_bar() { bar_->publish_bar_visible(false); /* TODO: toggle */ }
void WindowManager::cmd_scratch_toggle(const std::string &name) { /* TODO */ }
void WindowManager::cmd_set_border(BorderType type, int width) { /* TODO: update frames */ }
void WindowManager::cmd_set_color(BorderType type, const std::string &hex) { /* TODO: update frames */ }
void WindowManager::cmd_reload_config() { cfg_->run_once(); }
void WindowManager::cmd_quit() { stop(); }

// Event handlers
void WindowManager::handle_map_request(xcb_map_request_event_t *ev) {
    WindowID id = ev->window;
    // adopt window and map
    adopt_new_window(id);
}
void WindowManager::handle_unmap_notify(xcb_unmap_notify_event_t *ev) {
    remove_window(ev->window);
}
void WindowManager::handle_configure_request(xcb_configure_request_event_t *ev) {
    // respond to client's configure requests appropriately
}
void WindowManager::handle_key_press(xcb_key_press_event_t *ev) {
    // forward to InputManager
    input_->handle_key_event(ev);
}
void WindowManager::handle_button_press(xcb_button_press_event_t *ev) {
    input_->handle_button_event(ev);
}

// Helpers
void WindowManager::adopt_new_window(WindowID id) {
    std::unique_lock<std::shared_mutex> lk(state_mtx_);
    WmWindow w; w.id = id;
    // TODO: query WM_CLASS/title and apply RulesEngine; reparent by creating Frame
    windows_[id] = std::move(w);
}
void WindowManager::reparent_to_frame(WindowID id) { /* TODO */ }
void WindowManager::remove_window(WindowID id) { std::unique_lock<std::shared_mutex> lk(state_mtx_); windows_.erase(id); }
void WindowManager::update_struts_and_area() { /* TODO: update EWMH _NET_WM_STRUT etc */ }
void WindowManager::notify_workspace_change() { 
    // compute occupied
    std::vector<int> occ; 
    for (auto &p : workspaces_) if (!p.second.tiled.empty() || !p.second.floating.empty()) occ.push_back(p.first);
    bar_->publish_workspace(current_ws_, occ);
}

// -----------------------------
// main()
// -----------------------------
int main(int argc, char **argv) {
    WindowManager wm;
    if (!wm.init()) { std::cerr << "Failed to init WM\n"; return 1; }
    wm.run();
    return 0;
}

// -----------------------------
// End of skeleton
// -----------------------------

// Implementation notes:
// - Every TODO marks a place where implementers should add XCB calls, Cairo draws, or
//   more robust parsing/logic.
// - IPC commands are intentionally simple text lines; production should use robust
//   parsing for quoted args and escaping.
// - The file defines every public method, class and function that will be required.
//   When you implement the bodies, keep the same function names and signatures so I
//   (and your tooling) can reason about them in future conversations.

