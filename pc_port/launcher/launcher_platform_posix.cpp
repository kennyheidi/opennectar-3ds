// Implementación POSIX de la interfaz de plataforma del launcher.
// Los diálogos gráficos se delegan en zenity o kdialog; si no hay ninguno,
// el launcher recurre a su instalador en modo texto.

#include "launcher_platform.h"

#include <cstdint>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#if defined(__APPLE__)
#include <climits>
#include <mach-o/dyld.h>
#endif
#include <fstream>
#include <iostream>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace pikmin {
namespace launcher {
namespace platform {

namespace {

bool commandExists(const char* command)
{
    const char* pathValue = std::getenv("PATH");
    if (!pathValue) return false;
    std::string paths(pathValue);
    std::size_t start = 0;
    while (start <= paths.size()) {
        const std::size_t end = paths.find(':', start);
        const fs::path candidate = fs::path(paths.substr(start, end - start)) / command;
        if (access(candidate.c_str(), X_OK) == 0) return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

std::string runDialog(const char* program, const std::vector<std::string>& args)
{
    int pipeFds[2];
    if (pipe(pipeFds) != 0) return {};
    const pid_t child = fork();
    if (child == 0) {
        dup2(pipeFds[1], STDOUT_FILENO);
        // GTK/libadwaita diagnostics and unsupported cosmetic Zenity options
        // belong to the dialog process. Do not mix them with the launcher's
        // extraction progress or with the selected path read from stdout.
        const int nullFd = open("/dev/null", O_WRONLY);
        if (nullFd >= 0) {
            dup2(nullFd, STDERR_FILENO);
            close(nullFd);
        }
        close(pipeFds[0]); close(pipeFds[1]);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(program));
        for (const std::string& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execvp(program, argv.data());
        _exit(127);
    }
    close(pipeFds[1]);
    std::string result;
    char buffer[1024];
    ssize_t count;
    while ((count = read(pipeFds[0], buffer, sizeof(buffer))) > 0) result.append(buffer, count);
    close(pipeFds[0]);
    int status = 0;
    waitpid(child, &status, 0);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return {};
    return result;
}

} // namespace

fs::path executablePath()
{
    // The self-contained package wrappers exec through ld-linux, so
    // /proc/self/exe would name the loader rather than the launcher. They pass
    // the real path in an environment variable instead. Two names are accepted:
    // NECTAR_ is what package-standalone.sh writes today, PIKMIN_ is what
    // launcher_main.cpp generates and what older packages carry.
    const char* env = std::getenv("NECTAR_EXECUTABLE_PATH");
    if (env == nullptr) env = std::getenv("PIKMIN_EXECUTABLE_PATH");
    if (env != nullptr) {
        return fs::path(env);
    }
#if defined(__APPLE__)
    // macOS has no /proc. _NSGetExecutablePath returns the path as it was
    // invoked, which can be relative, so resolve it to an absolute path.
    std::vector<char> invoked(4096);
    std::uint32_t invokedSize = static_cast<std::uint32_t>(invoked.size());
    if (_NSGetExecutablePath(invoked.data(), &invokedSize) != 0) return {};
    char resolved[PATH_MAX];
    if (realpath(invoked.data(), resolved) == nullptr) return {};
    return fs::path(resolved);
#else
    std::vector<char> path(4096);
    const ssize_t count = readlink("/proc/self/exe", path.data(), path.size() - 1);
    if (count <= 0) return {};
    path[static_cast<std::size_t>(count)] = '\0';
    return fs::path(path.data());
#endif
}

fs::path defaultDataRoot()
{
    if (const char* xdg = std::getenv("XDG_DATA_HOME")) return fs::path(xdg) / "pikmin-native";
    if (const char* home = std::getenv("HOME")) return fs::path(home) / ".local/share/pikmin-native";
    return fs::current_path() / "pikmin-native-data";
}

unsigned long currentProcessId()
{
    return static_cast<unsigned long>(getpid());
}

bool hasGraphicalDialogs()
{
#if defined(__APPLE__)
    // osascript ships with macOS, so the native choosers are always available.
    return true;
#else
    return commandExists("zenity") || commandExists("kdialog");
#endif
}

bool stdinIsTerminal()
{
    return isatty(STDIN_FILENO) != 0;
}

void showMessage(const std::string& title, const std::string& message, bool error)
{
    if (commandExists("zenity")) {
        runDialog("zenity", { error ? "--error" : "--info", "--title=" + title, "--text=" + message,
                               "--width=480" });
        return;
    }
    else if (commandExists("kdialog")) {
        runDialog("kdialog", { error ? "--error" : "--msgbox", message, "--title", title });
        return;
    }
    std::cerr << title << ": " << message << '\n';
}

fs::path askForInstallDirectory(const std::string& title)
{
    if (commandExists("zenity")) {
        std::string initial;
        if (const char* home = std::getenv("HOME")) initial = fs::path(home).string() + "/";
        const std::string selected = runDialog("zenity", {
            "--file-selection", "--directory",
            "--title=Open Nectar - " + title,
            "--filename=" + initial
        });
        if (!selected.empty()) return selected;
    } else if (commandExists("kdialog")) {
        const std::string initial = std::getenv("HOME") ? std::getenv("HOME") : ".";
        const std::string selected = runDialog("kdialog", {
            "--getexistingdirectory", initial,
            "--title", "Open Nectar - " + title
        });
        if (!selected.empty()) return selected;
    }
#if defined(__APPLE__)
    else {
        // POSIX path returns a trailing slash. Drop it, but keep a bare "/".
        std::string selected = runDialog("osascript", { "-e",
            "POSIX path of (choose folder with prompt \"Open Nectar - " + title + "\")" });
        while (selected.size() > 1 && selected.back() == '/') selected.pop_back();
        if (!selected.empty()) return selected;
    }
#endif
    return {};
}

fs::path askForImage()
{
    if (commandExists("zenity")) {
        const std::string selected = runDialog("zenity", {
            "--file-selection", "--title=Open Nectar - Choose your disc image",
            "--file-filter=GameCube disc image | *.iso *.ISO *.gcm *.GCM *.rvz *.RVZ *.wia *.WIA *.gcz *.GCZ",
            "--file-filter=All files | *"
        });
        if (!selected.empty()) return selected;
    }
    else if (commandExists("kdialog")) {
        const std::string selected = runDialog("kdialog", {
            "--getopenfilename", ".", "*.iso *.ISO *.gcm *.GCM *.rvz *.RVZ *.wia *.WIA *.gcz *.GCZ|GameCube disc image"
        });
        if (!selected.empty()) return selected;
    }
#if defined(__APPLE__)
    else {
        // No type filter: osascript's "of type" needs file-type identifiers, not
        // extensions. The launcher checks the image format after selection.
        const std::string selected = runDialog("osascript", { "-e",
            "POSIX path of (choose file with prompt \"Open Nectar - Choose your disc image\")" });
        if (!selected.empty()) return selected;
    }
#endif
    return {};
}

fs::path askForFile(const std::string& title, const std::string& filterName, const std::string& patterns)
{
    if (commandExists("zenity")) {
        return runDialog("zenity", { "--file-selection", "--title=Open Nectar - " + title,
                                     "--file-filter=" + filterName + " | " + patterns, "--file-filter=All files | *" });
    }
    if (commandExists("kdialog")) {
        return runDialog("kdialog", { "--getopenfilename", ".", patterns + "|" + filterName });
    }
    return {};
}

fs::path findConverter()
{
    const auto beside = executablePath().parent_path() / "dolphin-tool";
    if (fs::is_regular_file(beside) && access(beside.c_str(), X_OK) == 0) return beside;
    const char* value = std::getenv("PATH");
    if (!value) return {};
    const std::string paths(value);
    std::size_t start = 0;
    while (start <= paths.size()) {
        const auto end = paths.find(':', start);
        const auto candidate = fs::path(paths.substr(start, end - start)) / "dolphin-tool";
        if (fs::is_regular_file(candidate) && access(candidate.c_str(), X_OK) == 0)
            return fs::absolute(candidate);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}

fs::path askForConverter()
{
    if (commandExists("zenity"))
        return runDialog("zenity", { "--file-selection", "--title=Choose dolphin-tool from your Dolphin installation" });
    if (commandExists("kdialog"))
        return runDialog("kdialog", { "--getopenfilename", ".", "dolphin-tool", "--title", "Choose dolphin-tool" });
    return {};
}

fs::path cacheDirectory()
{
    fs::path dir;
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) dir = fs::path(xdg) / "open-nectar";
    else if (const char* home = std::getenv("HOME")) dir = fs::path(home) / ".cache/open-nectar";
    else dir = fs::temp_directory_path() / "open-nectar-cache";
    std::error_code ignored;
    fs::create_directories(dir, ignored);
    return dir;
}

bool downloadFile(const std::string& url, const fs::path& destination, std::string& error)
{
    const fs::path partial = destination.string() + ".part";
    const pid_t child = fork();
    if (child < 0) {
        error = std::strerror(errno);
        return false;
    }
    if (child == 0) {
        // Sin salida: el launcher no tiene dónde mostrarla.
        const int null = open("/dev/null", O_WRONLY);
        if (null >= 0) { dup2(null, STDOUT_FILENO); dup2(null, STDERR_FILENO); }
        execlp("curl", "curl", "-fsSL", "--max-time", "30", "-o", partial.c_str(), url.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    std::error_code ec;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fs::remove(partial, ec);
        error = WIFEXITED(status) && WEXITSTATUS(status) == 127 ? "curl is not installed" : "download failed";
        return false;
    }
    fs::rename(partial, destination, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    return true;
}

bool convertImage(const fs::path& converter, const fs::path& source,
                  const fs::path& destination, const std::function<void()>& pump, std::string& error)
{
    const pid_t child = fork();
    if (child == 0) {
        execl(converter.c_str(), converter.c_str(), "convert", "-i", source.c_str(),
              "-o", destination.c_str(), "-f", "iso", static_cast<char*>(nullptr));
        _exit(127);
    }
    if (child < 0) { error = "Could not start dolphin-tool: " + std::string(std::strerror(errno)); return false; }
    int status = 0;
    for (;;) {
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) break;
        if (result < 0) {
            if (errno == EINTR) continue;
            error = "Could not wait for dolphin-tool: " + std::string(std::strerror(errno));
            return false;
        }
        if (pump) pump();
        usleep(50000);
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
    error = "Disc conversion failed. Check that dolphin-tool runs, the temporary folder has free space, "
            "and the disc image opens in Dolphin.";
    return false;
}

// When the launcher is started by double-clicking it in a file manager there
// is no terminal to show errors on. Re-run itself inside a terminal emulator
// so the text-mode installer and any error message become visible.
bool respawnInTerminal()
{
    if (std::getenv("PIKMIN_LAUNCHER_TERMINAL")) return false;
    const fs::path self = executablePath();
    if (self.empty()) return false;
    static const char* const terminals[] = {
        "x-terminal-emulator", "gnome-terminal", "konsole",
        "xfce4-terminal", "mate-terminal", "lxterminal", "xterm"
    };
    for (const char* terminal : terminals) {
        if (!commandExists(terminal)) continue;
        const pid_t child = fork();
        if (child == 0) {
            setenv("PIKMIN_LAUNCHER_TERMINAL", "1", 1);
            execlp(terminal, terminal, "-e", self.c_str(),
                   static_cast<char*>(nullptr));
            _exit(127);
        }
        if (child > 0) {
            int status = 0;
            waitpid(child, &status, 0);
            return true;
        }
    }
    return false;
}

bool runAndCapture(const fs::path& program, const std::vector<std::string>& arguments,
                   const fs::path& workingDirectory, std::string& output, std::string& error)
{
    int pipeFds[2];
    if (pipe(pipeFds) != 0) {
        error = std::strerror(errno);
        return false;
    }
    const pid_t child = fork();
    if (child < 0) {
        error = std::strerror(errno);
        close(pipeFds[0]);
        close(pipeFds[1]);
        return false;
    }
    if (child == 0) {
        dup2(pipeFds[1], STDOUT_FILENO);
        const int null = open("/dev/null", O_WRONLY);
        if (null >= 0) dup2(null, STDERR_FILENO);
        close(pipeFds[0]);
        close(pipeFds[1]);
        if (chdir(workingDirectory.c_str()) != 0) _exit(126);
        std::vector<std::string> storage;
        storage.push_back(program.string());
        storage.insert(storage.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        for (std::string& argument : storage) argv.push_back(argument.data());
        argv.push_back(nullptr);
        execvp(program.c_str(), argv.data()); // con ruta, igual que execv; sin ella, busca en PATH (tar)
        _exit(127);
    }
    close(pipeFds[1]);
    output.clear();
    char buffer[4096];
    for (;;) {
        const ssize_t got = read(pipeFds[0], buffer, sizeof(buffer));
        if (got > 0) output.append(buffer, size_t(got));
        else if (got == 0 || errno != EINTR) break;
    }
    close(pipeFds[0]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error = "exit status " + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return false;
    }
    return true;
}

void relaunch(const fs::path& program, const std::vector<std::string>& arguments)
{
    std::vector<std::string> storage;
    storage.push_back(program.string());
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (std::string& argument : storage) argv.push_back(argument.data());
    argv.push_back(nullptr);
    execv(program.c_str(), argv.data());
}

[[noreturn]] void launchGame(const fs::path& dataRoot, const fs::path& gameBinary,
                             const std::vector<std::string>& arguments)
{
    if (chdir(dataRoot.c_str()) != 0) {
        std::cerr << "No se pudo entrar en " << dataRoot << ": " << std::strerror(errno) << '\n';
        std::exit(1);
    }
    std::vector<std::string> storage;
    storage.push_back(gameBinary.string());
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (std::string& argument : storage) argv.push_back(argument.data());
    argv.push_back(nullptr);
    execv(gameBinary.c_str(), argv.data());
    std::cerr << "No se pudo iniciar " << gameBinary << ": " << std::strerror(errno) << '\n';
    std::exit(1);
}

bool spawnGame(const fs::path& dataRoot, const fs::path& gameBinary, const std::vector<std::string>& arguments,
               GameProcess& process, std::string& error)
{
    std::vector<std::string> storage;
    storage.push_back(gameBinary.string());
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (std::string& argument : storage) argv.push_back(argument.data());
    argv.push_back(nullptr);
    const pid_t child = fork();
    if (child < 0) {
        error = std::string("fork failed: ") + std::strerror(errno);
        return false;
    }
    if (child == 0) {
        if (chdir(dataRoot.c_str()) != 0) _exit(127);
        execv(gameBinary.c_str(), argv.data());
        _exit(127);
    }
    process.handle = static_cast<unsigned long long>(child);
    process.pid    = static_cast<unsigned long>(child);
    return true;
}

bool createDesktopShortcut(const fs::path& launcher)
{
    const char* home = std::getenv("HOME");
    if (!home || !*home) return false;
    std::error_code ec;
#if defined(__APPLE__)
    const fs::path link = fs::path(home) / "Desktop" / "Open Nectar";
    fs::remove(link, ec);
    fs::create_symlink(launcher, link, ec);
    return !ec;
#else
    // La carpeta del escritorio va traducida ("Escritorio"): la dice
    // user-dirs.dirs, con la línea XDG_DESKTOP_DIR="$HOME/Escritorio".
    fs::path desktop = fs::path(home) / "Desktop";
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::ifstream dirs((xdg && *xdg ? fs::path(xdg) : fs::path(home) / ".config") / "user-dirs.dirs");
    for (std::string line; std::getline(dirs, line);) {
        const std::string key = "XDG_DESKTOP_DIR=\"";
        if (line.compare(0, key.size(), key) != 0) continue;
        std::string value = line.substr(key.size());
        if (!value.empty() && value.back() == '"') value.pop_back();
        if (value.compare(0, 5, "$HOME") == 0) value = std::string(home) + value.substr(5);
        if (!value.empty()) desktop = value;
    }
    const fs::path icon = launcher.parent_path() / "open_nectar.png";
    const std::string entry = "[Desktop Entry]\nType=Application\nName=Open Nectar\n"
                              "Comment=Pikmin 1 and Pikmin 2\nExec=\"" + launcher.string() + "\"\n"
                              "Path=" + launcher.parent_path().string() + "\n"
                            + (fs::is_regular_file(icon, ec) ? "Icon=" + icon.string() + "\n" : std::string())
                            + "Terminal=false\nCategories=Game;\n";
    bool written = false;
    // Escritorio y menú de aplicaciones.
    for (const fs::path& file : { desktop / "open-nectar.desktop",
                                  fs::path(home) / ".local/share/applications/open-nectar.desktop" }) {
        fs::create_directories(file.parent_path(), ec);
        std::ofstream out(file, std::ios::trunc);
        if (!(out << entry)) continue;
        out.close();
        fs::permissions(file, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                        fs::perm_options::add, ec);
        if (!written) {
            // GNOME no abre un .desktop del escritorio hasta marcarlo de confianza.
            std::string ignored, error;
            runAndCapture("gio", { "set", file.string(), "metadata::trusted", "true" }, desktop, ignored, error);
        }
        written = true;
    }
    return written;
#endif
}

bool gameRunning(GameProcess& process, int& exitCode)
{
    if (process.handle == 0) return false;
    int status = 0;
    const pid_t got = waitpid(static_cast<pid_t>(process.handle), &status, WNOHANG);
    if (got == 0) return true;
    process.handle = 0;
    exitCode = got > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return false;
}

} // namespace platform
} // namespace launcher
} // namespace pikmin
