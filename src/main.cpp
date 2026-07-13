#include <ncurses.h>
#include <string>
#include <vector>
#include <algorithm>
#include <locale.h>
#include <cstdio>
#include <memory>
#include <cstring>
#include <sstream>
#include <set>
#include <sys/time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/stat.h>

enum class PkgManager { NONE, PACMAN, DNF, APT };

struct Package {
    std::string name;
    std::string version;
    std::string description;
    std::string size;
    bool installed;
};

struct AppState {
    int selected;
    int scroll;
    std::string query;
    bool confirmOpen;
    bool confirmYes;
    bool loading;
    std::string statusMsg;
    int statusTimeout;
    PkgManager pkgMgr;
    bool dirty;
    long lastKeyTime;
    bool busy;
    int busyPipe[2];
    pid_t busyPid;
    std::string busyOutput;
    bool busyDone;
    bool busySuccess;
};

static std::string runCmd(const char* cmd) {
    std::string result;
    FILE* pipe = popen(cmd, "r");
    if (!pipe) return result;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe);
    return result;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\n\r");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\n\r");
    return s.substr(a, b - a + 1);
}

static PkgManager detectPkgMgr() {
    if (system("command -v pacman &>/dev/null") == 0) return PkgManager::PACMAN;
    if (system("command -v dnf &>/dev/null") == 0) return PkgManager::DNF;
    if (system("command -v apt &>/dev/null") == 0) return PkgManager::APT;
    return PkgManager::NONE;
}

static const char* pmName(PkgManager pm) {
    switch (pm) {
        case PkgManager::PACMAN: return "pacman";
        case PkgManager::DNF:    return "dnf";
        case PkgManager::APT:    return "apt";
        default:                 return "none";
    }
}

static std::set<std::string> getInstalledPackages(PkgManager pm) {
    std::set<std::string> installed;
    std::string out;
    switch (pm) {
        case PkgManager::PACMAN:
            out = runCmd("pacman -Q 2>/dev/null");
            {
                std::istringstream iss(out);
                std::string line;
                while (std::getline(iss, line)) {
                    size_t sp = line.find(' ');
                    if (sp != std::string::npos)
                        installed.insert(line.substr(0, sp));
                }
            }
            break;
        case PkgManager::DNF:
            out = runCmd("rpm -qa --queryformat '%{NAME}\\n' 2>/dev/null");
            {
                std::istringstream iss(out);
                std::string line;
                while (std::getline(iss, line)) {
                    line = trim(line);
                    if (!line.empty()) installed.insert(line);
                }
            }
            break;
        case PkgManager::APT:
            out = runCmd("dpkg --get-selections 2>/dev/null | grep 'install' | awk '{print $1}'");
            {
                std::istringstream iss(out);
                std::string line;
                while (std::getline(iss, line)) {
                    line = trim(line);
                    if (!line.empty()) installed.insert(line);
                }
            }
            break;
        default:
            break;
    }
    return installed;
}

static std::vector<Package> searchPackages(PkgManager pm, const std::string& query) {
    std::vector<Package> result;
    if (query.empty()) return result;

    std::set<std::string> installed = getInstalledPackages(pm);

    std::string cmd;
    switch (pm) {
        case PkgManager::PACMAN:
            cmd = "pacman -Ss '" + query + "' 2>/dev/null";
            break;
        case PkgManager::DNF:
            cmd = "dnf search '" + query + "' 2>/dev/null";
            break;
        case PkgManager::APT:
            cmd = "apt-cache search --names-only '" + query + "' 2>/dev/null";
            break;
        default:
            return result;
    }

    std::string out = runCmd(cmd.c_str());
    if (out.empty()) return result;

    std::istringstream iss(out);
    std::string line;

    if (pm == PkgManager::PACMAN) {
        while (std::getline(iss, line)) {
            if (line.empty() || line[0] == ' ' || line[0] == '\t') continue;
            size_t slash = line.find('/');
            if (slash == std::string::npos) continue;
            std::string repo = line.substr(0, slash);
            std::string rest = trim(line.substr(slash + 1));
            size_t sp = rest.find(' ');
            std::string name = (sp != std::string::npos) ? rest.substr(0, sp) : rest;
            std::string version = "";
            size_t paren = rest.find('(');
            size_t parenEnd = rest.find(')');
            if (paren != std::string::npos && parenEnd != std::string::npos)
                version = rest.substr(paren + 1, parenEnd - paren - 1);
            std::string desc = "";
            if (parenEnd != std::string::npos) {
                size_t dash = rest.find(" - ", parenEnd);
                if (dash != std::string::npos) desc = trim(rest.substr(dash + 3));
            }
            result.push_back({name, version, desc, "", installed.count(name) > 0});
        }
    } else if (pm == PkgManager::DNF) {
        bool inResults = false;
        while (std::getline(iss, line)) {
            if (line.find("====") != std::string::npos || line.find("Last metadata") != std::string::npos) {
                inResults = true;
                continue;
            }
            if (!inResults || line.empty()) continue;
            size_t sp1 = line.find(' ');
            if (sp1 == std::string::npos) continue;
            std::string name = trim(line.substr(0, sp1));
            std::string rest = trim(line.substr(sp1 + 1));
            std::string version = "";
            std::string desc = rest;
            size_t colon = rest.find(':');
            if (colon != std::string::npos) {
                version = trim(rest.substr(0, colon));
                desc = trim(rest.substr(colon + 1));
            }
            result.push_back({name, version, desc, "", installed.count(name) > 0});
        }
    } else if (pm == PkgManager::APT) {
        while (std::getline(iss, line)) {
            line = trim(line);
            if (line.empty()) continue;
            size_t dash = line.find(" - ");
            if (dash == std::string::npos) continue;
            std::string name = trim(line.substr(0, dash));
            std::string desc = trim(line.substr(dash + 3));
            result.push_back({name, "", desc, "", installed.count(name) > 0});
        }
    }

    return result;
}

static std::string askPassword(WINDOW* win) {
    int maxy, maxx;
    getmaxyx(win, maxy, maxx);

    int dw = 44;
    int dh = 7;
    int dy = (maxy - dh) / 2;
    int dx = (maxx - dw) / 2;

    WINDOW* dlg = newwin(dh, dw, dy, dx);
    wattron(dlg, COLOR_PAIR(16));
    wborder(dlg, '|', '|', '-', '-', '+', '+', '+', '+');
    wattroff(dlg, COLOR_PAIR(16));

    wattron(dlg, COLOR_PAIR(19) | A_BOLD);
    mvwprintw(dlg, 0, 2, " SUDO PASSWORD ");
    wattroff(dlg, COLOR_PAIR(19) | A_BOLD);

    wattron(dlg, COLOR_PAIR(7));
    mvwprintw(dlg, 2, 3, "Enter your password:");
    wattroff(dlg, COLOR_PAIR(7));

    wattron(dlg, COLOR_PAIR(14));
    mvwhline(dlg, 4, 3, ' ', dw - 6);
    wattroff(dlg, COLOR_PAIR(14));

    wrefresh(dlg);

    echo();
    curs_set(1);

    // Read password character by character, show asterisks
    std::string pass;
    wmove(dlg, 4, 3);
    wrefresh(dlg);
    while (true) {
        int c = wgetch(dlg);
        if (c == '\n' || c == KEY_ENTER) break;
        if (c == KEY_BACKSPACE || c == 127 || c == '\b') {
            if (!pass.empty()) {
                pass.pop_back();
                int cy, cx;
                getyx(dlg, cy, cx);
                mvwaddch(dlg, cy, cx - 1, ' ');
                wmove(dlg, cy, cx - 1);
                wrefresh(dlg);
            }
        } else if (isprint(c)) {
            pass += (char)c;
            waddch(dlg, '*');
            wrefresh(dlg);
        }
    }

    noecho();
    curs_set(0);
    delwin(dlg);
    return pass;
}

static void startBackgroundOp(AppState& state, PkgManager pm, const std::string& name, bool install) {
    pipe(state.busyPipe);

    pid_t pid = fork();
    if (pid == 0) {
        // child
        close(state.busyPipe[0]);
        dup2(state.busyPipe[1], STDOUT_FILENO);
        dup2(state.busyPipe[1], STDERR_FILENO);
        close(state.busyPipe[1]);

        std::string cmd;
        if (install) {
            switch (pm) {
                case PkgManager::PACMAN: cmd = "sudo pacman -S --noconfirm '" + name + "'"; break;
                case PkgManager::DNF:    cmd = "sudo dnf install -y '" + name + "'"; break;
                case PkgManager::APT:    cmd = "sudo apt install -y '" + name + "'"; break;
                default: _exit(1);
            }
        } else {
            switch (pm) {
                case PkgManager::PACMAN: cmd = "sudo pacman -R --noconfirm '" + name + "'"; break;
                case PkgManager::DNF:    cmd = "sudo dnf remove -y '" + name + "'"; break;
                case PkgManager::APT:    cmd = "sudo apt remove -y '" + name + "'"; break;
                default: _exit(1);
            }
        }
        execl("/bin/sh", "sh", "-c", cmd.c_str(), NULL);
        _exit(1);
    } else {
        // parent
        close(state.busyPipe[1]);
        fcntl(state.busyPipe[0], F_SETFL, O_NONBLOCK);
        state.busyPid = pid;
        state.busy = true;
        state.busyDone = false;
        state.busySuccess = false;
        state.busyOutput = "";
    }
}

static void checkBackgroundOp(AppState& state) {
    if (!state.busy) return;

    char buf[4096];
    ssize_t n;
    while ((n = read(state.busyPipe[0], buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        state.busyOutput += buf;
    }

    int status;
    pid_t res = waitpid(state.busyPid, &status, WNOHANG);
    if (res > 0) {
        close(state.busyPipe[0]);
        state.busy = false;
        state.busyDone = true;
        state.busySuccess = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
}

static void initColors() {
    start_color();
    use_default_colors();
    init_pair(1,  COLOR_CYAN,   -1);
    init_pair(3,  COLOR_GREEN,  -1);
    init_pair(4,  COLOR_YELLOW, -1);
    init_pair(5,  COLOR_WHITE,  COLOR_BLUE);
    init_pair(7,  COLOR_WHITE,  -1);
    init_pair(8,  COLOR_CYAN,   -1);
    init_pair(9,  COLOR_WHITE,  COLOR_GREEN);
    init_pair(10, COLOR_BLUE,   -1);
    init_pair(12, COLOR_WHITE,  COLOR_BLACK);
    init_pair(13, COLOR_WHITE,  COLOR_CYAN);
    init_pair(14, COLOR_WHITE,  COLOR_BLACK);
    init_pair(15, COLOR_WHITE,  COLOR_RED);
    init_pair(16, COLOR_WHITE,  COLOR_BLACK);
    init_pair(17, COLOR_GREEN,  COLOR_BLACK);
    init_pair(18, COLOR_WHITE,  COLOR_GREEN);
    init_pair(19, COLOR_WHITE,  COLOR_YELLOW);
    init_pair(20, COLOR_CYAN,   COLOR_BLACK);
    init_pair(21, COLOR_GREEN,  -1);
    init_pair(22, COLOR_RED,    -1);
}

static void drawConfirmDialog(WINDOW* win, const Package& pkg, bool installAction, bool cursorYes) {
    int maxy, maxx;
    getmaxyx(win, maxy, maxx);

    int dw = 52;
    int dh = 8;
    int dy = (maxy - dh) / 2;
    int dx = (maxx - dw) / 2;

    WINDOW* dlg = newwin(dh, dw, dy, dx);
    wattron(dlg, COLOR_PAIR(16));
    wborder(dlg, '|', '|', '-', '-', '+', '+', '+', '+');
    wattroff(dlg, COLOR_PAIR(16));

    wattron(dlg, COLOR_PAIR(19) | A_BOLD);
    mvwprintw(dlg, 0, 2, " CONFIRM ");
    wattroff(dlg, COLOR_PAIR(19) | A_BOLD);

    wattron(dlg, COLOR_PAIR(7));
    if (installAction) {
        mvwprintw(dlg, 2, 3, "Install \"%s\"?", pkg.name.c_str());
    } else {
        mvwprintw(dlg, 2, 3, "Remove \"%s\"?", pkg.name.c_str());
    }
    wattroff(dlg, COLOR_PAIR(7));

    if (!pkg.version.empty()) {
        wattron(dlg, COLOR_PAIR(10));
        mvwprintw(dlg, 3, 3, "Version: %s", pkg.version.c_str());
        wattroff(dlg, COLOR_PAIR(10));
    }

    if (!pkg.description.empty()) {
        wattron(dlg, COLOR_PAIR(10));
        std::string d = pkg.description;
        if ((int)d.length() > dw - 6) d = d.substr(0, dw - 9) + "...";
        mvwprintw(dlg, 4, 3, "%s", d.c_str());
        wattroff(dlg, COLOR_PAIR(10));
    }

    int btnYesX = dw / 2 - 12;
    int btnNoX = dw / 2 + 3;

    if (cursorYes) {
        wattron(dlg, COLOR_PAIR(18) | A_BOLD);
        mvwprintw(dlg, 6, btnYesX, " [ Yes ] ");
        wattroff(dlg, COLOR_PAIR(18) | A_BOLD);
        wattron(dlg, COLOR_PAIR(16));
        mvwprintw(dlg, 6, btnNoX, " [ No ]  ");
        wattroff(dlg, COLOR_PAIR(16));
    } else {
        wattron(dlg, COLOR_PAIR(16));
        mvwprintw(dlg, 6, btnYesX, " [ Yes ] ");
        wattroff(dlg, COLOR_PAIR(16));
        wattron(dlg, COLOR_PAIR(15) | A_BOLD);
        mvwprintw(dlg, 6, btnNoX, " [ No ]  ");
        wattroff(dlg, COLOR_PAIR(15) | A_BOLD);
    }

    wrefresh(dlg);
    delwin(dlg);
}

static void ensureDesktopEntry() {
    const char* home = getenv("HOME");
    if (!home) return;

    std::string appsDir = std::string(home) + "/.local/share/applications";
    std::string iconsDir = std::string(home) + "/.local/share/icons";
    std::string desktopFile = appsDir + "/tupac.desktop";
    std::string iconFile = iconsDir + "/tupac.svg";

    // check if already exists
    struct stat st;
    if (stat(desktopFile.c_str(), &st) == 0) return;

    // create dirs
    system(("mkdir -p '" + appsDir + "'").c_str());
    system(("mkdir -p '" + iconsDir + "'").c_str());

    // write SVG icon
    FILE* f = fopen(iconFile.c_str(), "w");
    if (f) {
        fprintf(f,
            "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'>\n"
            "  <rect width='64' height='64' rx='12' fill='#1a1a2e'/>\n"
            "  <text x='32' y='28' text-anchor='middle' font-family='monospace'\n"
            "        font-size='16' font-weight='bold' fill='#00d4ff'>TP</text>\n"
            "  <text x='32' y='48' text-anchor='middle' font-family='monospace'\n"
            "        font-size='10' fill='#888'>pkg</text>\n"
            "</svg>\n"
        );
        fclose(f);
    }

    // get executable path
    char exePath[4096] = {};
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len <= 0) {
        strncpy(exePath, "tupac", sizeof(exePath));
    }

    // write .desktop file
    f = fopen(desktopFile.c_str(), "w");
    if (f) {
        fprintf(f,
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=TUPAC\n"
            "Comment=Terminal Package Manager\n"
            "Exec=%s\n"
            "Icon=%s\n"
            "Terminal=true\n"
            "Categories=System;Utility;\n"
            "Keywords=package;manager;install;pacman;dnf;apt;\n"
            "StartupNotify=false\n",
            exePath, iconFile.c_str()
        );
        fclose(f);
    }
}

static void drawUI(WINDOW* win, AppState& state, const std::vector<Package>& pkgs) {
    int maxy, maxx;
    getmaxyx(win, maxy, maxx);
    werase(win);

    int mid = maxy / 2 + 1;

    // ── Title ──
    wattron(win, COLOR_PAIR(12) | A_BOLD);
    mvwhline(win, 0, 0, ' ', maxx);
    mvwprintw(win, 0, 2, " TUPAC ");
    wattroff(win, COLOR_PAIR(12) | A_BOLD);
    wattron(win, COLOR_PAIR(12));
    mvwprintw(win, 0, 11, "Package Manager [%s]", pmName(state.pkgMgr));
    wattroff(win, COLOR_PAIR(12));
    wattron(win, COLOR_PAIR(10));
    mvwprintw(win, 0, maxx - 20, "[q] quit");
    wattroff(win, COLOR_PAIR(10));

    mvwhline(win, 1, 0, ACS_HLINE, maxx);

    // ── Package list ──
    int listStart = 2;
    int listEnd = mid - 1;
    int listH = listEnd - listStart;

    if (state.selected >= (int)pkgs.size()) state.selected = std::max(0, (int)pkgs.size() - 1);
    if (state.selected < 0) state.selected = 0;
    if (state.selected < state.scroll) state.scroll = state.selected;
    if (state.selected >= state.scroll + listH) state.scroll = state.selected - listH + 1;

    wattron(win, COLOR_PAIR(1) | A_BOLD);
    mvwprintw(win, listStart, 2, " PACKAGE");
    mvwprintw(win, listStart, 28, "VERSION");
    mvwprintw(win, listStart, 44, "STATUS");
    wattroff(win, COLOR_PAIR(1) | A_BOLD);
    mvwhline(win, listStart + 1, 0, ACS_HLINE, maxx);

    if (state.loading) {
        wattron(win, COLOR_PAIR(4) | A_BOLD);
        mvwprintw(win, listStart + 3, maxx / 2 - 10, "Loading packages...");
        wattroff(win, COLOR_PAIR(4) | A_BOLD);
    } else if (pkgs.empty()) {
        wattron(win, COLOR_PAIR(4) | A_BOLD);
        if (state.query.empty()) {
            mvwprintw(win, listStart + 3, maxx / 2 - 20, "Type to search packages...");
        } else {
            mvwprintw(win, listStart + 3, maxx / 2 - 20, "No packages found for \"%s\"", state.query.c_str());
        }
        wattroff(win, COLOR_PAIR(4) | A_BOLD);
    } else {
        for (int i = state.scroll; i < std::min(state.scroll + listH, (int)pkgs.size()); i++) {
            int y = listStart + 2 + (i - state.scroll);
            const Package& p = pkgs[i];
            bool sel = (i == state.selected);

            if (sel) {
                wattron(win, COLOR_PAIR(13));
                mvwhline(win, y, 0, ' ', maxx);
                wattroff(win, COLOR_PAIR(13));
            }

            if (sel) {
                wattron(win, COLOR_PAIR(13) | A_BOLD);
                mvwprintw(win, y, 2, "> %s", p.name.c_str());
                wattroff(win, COLOR_PAIR(13) | A_BOLD);
            } else {
                wattron(win, COLOR_PAIR(7));
                mvwprintw(win, y, 2, "  %s", p.name.c_str());
                wattroff(win, COLOR_PAIR(7));
            }

            wattron(win, sel ? COLOR_PAIR(13) : COLOR_PAIR(10));
            mvwprintw(win, y, 28, "%s", p.version.c_str());
            wattroff(win, sel ? COLOR_PAIR(13) : COLOR_PAIR(10));

            if (p.installed) {
                wattron(win, sel ? COLOR_PAIR(13) : COLOR_PAIR(3) | A_BOLD);
                mvwprintw(win, y, 44, "[installed]");
                wattroff(win, sel ? COLOR_PAIR(13) : COLOR_PAIR(3) | A_BOLD);
            } else {
                wattron(win, sel ? COLOR_PAIR(13) : COLOR_PAIR(10) | A_DIM);
                mvwprintw(win, y, 44, "available");
                wattroff(win, sel ? COLOR_PAIR(13) : COLOR_PAIR(10) | A_DIM);
            }
        }
    }

    // ── Search bar ──
    int searchY = mid;
    mvwhline(win, searchY, 0, ' ', maxx);
    wattron(win, COLOR_PAIR(1) | A_BOLD);
    mvwprintw(win, searchY, 2, "\u2315");
    wattroff(win, COLOR_PAIR(1) | A_BOLD);

    int searchX = 5;
    int searchW = maxx - 7;

    wattron(win, COLOR_PAIR(14));
    mvwhline(win, searchY, searchX, ' ', searchW);
    wattroff(win, COLOR_PAIR(14));

    if (!state.query.empty()) {
        wattron(win, COLOR_PAIR(7) | A_BOLD);
        mvwprintw(win, searchY, searchX + 1, "%s", state.query.c_str());
        wattroff(win, COLOR_PAIR(7) | A_BOLD);
    }

    wattron(win, COLOR_PAIR(20) | A_BLINK);
    mvwaddch(win, searchY, searchX + 1 + (int)state.query.length(), '|');
    wattroff(win, COLOR_PAIR(20) | A_BLINK);

    mvwhline(win, searchY + 1, 0, ACS_HLINE, maxx);

    // ── Detail panel ──
    int detY = searchY + 2;

    wattron(win, COLOR_PAIR(1) | A_BOLD);
    mvwprintw(win, detY, 2, "\u25B6 PACKAGE DETAILS");
    wattroff(win, COLOR_PAIR(1) | A_BOLD);

    if (!pkgs.empty() && state.selected < (int)pkgs.size()) {
        const Package& p = pkgs[state.selected];
        int line = detY + 1;

        wattron(win, COLOR_PAIR(8));
        mvwprintw(win, line, 4, "Name:");
        wattroff(win, COLOR_PAIR(8));
        wattron(win, COLOR_PAIR(7) | A_BOLD);
        mvwprintw(win, line, 20, "%s", p.name.c_str());
        wattroff(win, COLOR_PAIR(7) | A_BOLD);

        wattron(win, COLOR_PAIR(8));
        mvwprintw(win, line, maxx / 2, "Version:");
        wattroff(win, COLOR_PAIR(8));
        wattron(win, COLOR_PAIR(3) | A_BOLD);
        mvwprintw(win, line, maxx / 2 + 16, "%s", p.version.c_str());
        wattroff(win, COLOR_PAIR(3) | A_BOLD);
        line++;

        wattron(win, COLOR_PAIR(8));
        mvwprintw(win, line, 4, "Description:");
        wattroff(win, COLOR_PAIR(8));
        wattron(win, COLOR_PAIR(7));
        std::string desc = p.description;
        int maxDesc = maxx - 20;
        if ((int)desc.length() > maxDesc) desc = desc.substr(0, maxDesc - 3) + "...";
        mvwprintw(win, line, 20, "%s", desc.c_str());
        wattroff(win, COLOR_PAIR(7));
        line++;

        wattron(win, COLOR_PAIR(8));
        mvwprintw(win, line, 4, "Status:");
        wattroff(win, COLOR_PAIR(8));
        if (p.installed) {
            wattron(win, COLOR_PAIR(9) | A_BOLD);
            mvwprintw(win, line, 20, "[INSTALLED]");
            wattroff(win, COLOR_PAIR(9) | A_BOLD);
        } else {
            wattron(win, COLOR_PAIR(10) | A_DIM);
            mvwprintw(win, line, 20, "[NOT INSTALLED]");
            wattroff(win, COLOR_PAIR(10) | A_DIM);
        }

        wattron(win, COLOR_PAIR(8));
        mvwprintw(win, line, maxx / 2, "Action:");
        wattroff(win, COLOR_PAIR(8));
        wattron(win, COLOR_PAIR(19));
        mvwprintw(win, line, maxx / 2 + 16, "Enter to %s", p.installed ? "remove" : "install");
        wattroff(win, COLOR_PAIR(19));
    }

    // ── Status / Footer ──
    if (state.statusTimeout > 0) {
        wattron(win, COLOR_PAIR(17) | A_BOLD);
        mvwprintw(win, maxy - 1, 2, "%s", state.statusMsg.c_str());
        wattroff(win, COLOR_PAIR(17) | A_BOLD);
    } else {
        wattron(win, COLOR_PAIR(10));
        mvwprintw(win, maxy - 1, 2, "\u2191\u2193 move  type to search  Enter install/remove  q quit");
        wattroff(win, COLOR_PAIR(10));
    }

    wrefresh(win);
}

int main() {
    setlocale(LC_ALL, "");
    ensureDesktopEntry();

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(-1);

    initColors();

    PkgManager pm = detectPkgMgr();
    if (pm == PkgManager::NONE) {
        endwin();
        printf("Error: No supported package manager found.\n");
        printf("Supported: pacman (Arch), dnf (Fedora), apt (Debian/Ubuntu)\n");
        return 1;
    }

    AppState state = {0, 0, "", false, true, false, "", 0, pm, false, 0,
                      false, {-1,-1}, -1, "", false, false};
    std::vector<Package> pkgs;

    while (true) {
        // Check background operation
        checkBackgroundOp(state);
        if (state.busyDone) {
            state.busyDone = false;
            if (state.busySuccess) {
                state.statusMsg = pkgs[state.selected].name + " - operation successful.";
                // update installed status
                auto refreshed = searchPackages(pm, state.query);
                for (auto& p : refreshed) {
                    for (auto& orig : pkgs) {
                        if (p.name == orig.name) {
                            orig.installed = p.installed;
                            break;
                        }
                    }
                }
            } else {
                std::string lastLine;
                std::istringstream iss(state.busyOutput);
                std::string line;
                while (std::getline(iss, line)) {
                    if (!line.empty()) lastLine = line;
                }
                if (lastLine.empty()) lastLine = "Operation failed.";
                if (lastLine.length() > 60) lastLine = lastLine.substr(0, 57) + "...";
                state.statusMsg = lastLine;
            }
            state.statusTimeout = 4;
        }

        drawUI(stdscr, state, pkgs);

        if (state.confirmOpen) {
            drawConfirmDialog(stdscr, pkgs[state.selected],
                pkgs[state.selected].installed ? false : true, state.confirmYes);
        }

        if (state.busy) {
            // show spinner
            int maxy, maxx;
            getmaxyx(stdscr, maxy, maxx);
            static int spin = 0;
            const char* spins[] = {"\u25D4", "\u25D1", "\u25D5", "\u25CE"};
            wattron(stdscr, COLOR_PAIR(4) | A_BOLD);
            mvwprintw(stdscr, maxy - 1, 2, "%s Processing...", spins[spin % 4]);
            wattroff(stdscr, COLOR_PAIR(4) | A_BOLD);
            wrefresh(stdscr);
            spin++;
            timeout(200);
            getch();
            timeout(-1);
            continue;
        }

        // Debounce: search after 250ms of no typing
        int waitMs = -1;
        if (state.dirty) {
            struct timeval tv;
            gettimeofday(&tv, NULL);
            long now = tv.tv_sec * 1000 + tv.tv_usec / 1000;
            long elapsed = now - state.lastKeyTime;
            if (elapsed >= 250) {
                if (state.query.empty()) {
                    pkgs.clear();
                } else {
                    state.loading = true;
                    drawUI(stdscr, state, pkgs);
                    pkgs = searchPackages(pm, state.query);
                    state.loading = false;
                    drawUI(stdscr, state, pkgs);
                }
                state.dirty = false;
                state.selected = 0;
                state.scroll = 0;
            } else {
                waitMs = 250 - elapsed;
            }
        }

        if (state.statusTimeout > 0) {
            timeout(1000);
        } else if (waitMs > 0) {
            timeout(waitMs);
        } else {
            timeout(-1);
        }

        int ch = getch();

        if (state.statusTimeout > 0) {
            state.statusTimeout--;
            if (state.statusTimeout <= 0) {
                state.statusMsg = "";
            }
            continue;
        }

        if (state.confirmOpen) {
            switch (ch) {
                case KEY_LEFT:
                case 'h':
                    state.confirmYes = true;
                    break;
                case KEY_RIGHT:
                case 'l':
                    state.confirmYes = false;
                    break;
                case '\n':
                    if (state.confirmYes) {
                        bool install = !pkgs[state.selected].installed;

                        // ask password via popup
                        std::string pass = askPassword(stdscr);
                        if (pass.empty()) {
                            state.statusMsg = "Cancelled - no password entered.";
                            state.statusTimeout = 3;
                            state.confirmOpen = false;
                            break;
                        }

                        // cache sudo with the password
                        std::string cacheCmd = "echo '" + pass + "' | sudo -S true 2>/dev/null";
                        int cacheOk = system(cacheCmd.c_str());
                        // overwrite password in memory
                        for (auto& c : pass) c = 'X';

                        if (cacheOk != 0) {
                            state.statusMsg = "Wrong password.";
                            state.statusTimeout = 3;
                            state.confirmOpen = false;
                            break;
                        }

                        state.statusMsg = install ?
                            "Installing " + pkgs[state.selected].name + "..." :
                            "Removing " + pkgs[state.selected].name + "...";
                        state.statusTimeout = 0;

                        startBackgroundOp(state, pm, pkgs[state.selected].name, install);
                    }
                    state.confirmOpen = false;
                    break;
                case 27:
                    state.confirmOpen = false;
                    break;
            }
            continue;
        }

        if (ch == 'q' || ch == 'Q') break;

        switch (ch) {
            case KEY_UP:
            case 'k':
                state.selected--;
                if (state.selected < 0) state.selected = std::max(0, (int)pkgs.size() - 1);
                break;
            case KEY_DOWN:
            case 'j':
                state.selected++;
                if (state.selected >= (int)pkgs.size()) state.selected = (int)pkgs.size() - 1;
                break;
            case '\n':
                if (!pkgs.empty() && state.selected < (int)pkgs.size() && !state.busy) {
                    state.confirmOpen = true;
                    state.confirmYes = true;
                }
                break;
            case KEY_BACKSPACE:
            case 127:
            case '\b':
                if (!state.query.empty()) {
                    state.query.pop_back();
                    struct timeval tv;
                    gettimeofday(&tv, NULL);
                    state.lastKeyTime = tv.tv_sec * 1000 + tv.tv_usec / 1000;
                    state.dirty = true;
                }
                break;
            default:
                if (isprint(ch) && !state.busy) {
                    state.query += (char)ch;
                    struct timeval tv;
                    gettimeofday(&tv, NULL);
                    state.lastKeyTime = tv.tv_sec * 1000 + tv.tv_usec / 1000;
                    state.dirty = true;
                }
                break;
        }
    }

    endwin();
    return 0;
}
