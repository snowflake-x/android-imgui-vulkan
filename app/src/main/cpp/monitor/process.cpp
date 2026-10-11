#include "monitor/process.h"
#include "monitor/model.h"
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
namespace vulkanscope::monitor {
CommandResult RunCommand(const std::vector<std::string> &args, const std::atomic<bool> &cancelled,
                         int timeoutMs, std::size_t limit) {
    CommandResult result;
    if (args.empty() || cancelled) {
        result.error = "Cancelled";
        return result;
    }
    int pipeFd[2];
    if (pipe2(pipeFd, O_CLOEXEC) != 0) {
        result.error = "Cannot create output pipe";
        return result;
    }
    std::vector<char *> argv;
    for (const auto &arg : args)
        argv.push_back(const_cast<char *>(arg.c_str()));
    argv.push_back(nullptr);
    std::string executable = args[0];
    if (executable.find('/') == std::string::npos) {
        const char *path = std::getenv("PATH");
        std::istringstream paths(path ? path : "/system/bin:/system/xbin:/usr/bin:/bin");
        std::string directory;
        executable.clear();
        while (std::getline(paths, directory, ':')) {
            if (directory.empty())
                continue;
            const auto candidate = directory + "/" + args[0];
            if (access(candidate.c_str(), X_OK) == 0) {
                executable = candidate;
                break;
            }
        }
        if (executable.empty()) {
            close(pipeFd[0]);
            close(pipeFd[1]);
            result.error = "Command unavailable";
            return result;
        }
    }
    /** @brief Android 26 无 posix_spawn；子进程仅调用异步信号安全操作后立即 execve。 */
    const char *executablePath = executable.c_str();
    char *const *childArgv = argv.data();
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        dup2(pipeFd[1], STDOUT_FILENO);
        dup2(pipeFd[1], STDERR_FILENO);
        close(pipeFd[0]);
        close(pipeFd[1]);
        execve(executablePath, childArgv, environ);
        _exit(127);
    }
    close(pipeFd[1]);
    if (pid < 0) {
        close(pipeFd[0]);
        result.error = "Cannot start command";
        return result;
    }
    setpgid(pid, pid);
    fcntl(pipeFd[0], F_SETFL, O_NONBLOCK);
    auto deadline = MonotonicNs() + timeoutMs * 1000000LL;
    bool exited = false, eof = false;
    int status = 0;
    while (!exited || !eof) {
        if (cancelled || MonotonicNs() >= deadline) {
            result.error = cancelled ? "Cancelled" : "Command timed out";
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            break;
        }
        char bytes[4096];
        for (int chunk = 0; chunk < 32; ++chunk) {
            ssize_t n = read(pipeFd[0], bytes, sizeof(bytes));
            if (n > 0) {
                if (result.output.size() < limit)
                    result.output.append(bytes, std::min<std::size_t>(n, limit - result.output.size()));
            } else {
                if (n == 0)
                    eof = true;
                break;
            }
        }
        if (!exited) {
            auto waited = waitpid(pid, &status, WNOHANG);
            exited = waited == pid;
            if (waited < 0 && errno != EINTR) {
                exited = true;
                status = 1;
            }
        }
        if (!exited || !eof) {
            pollfd p{pipeFd[0], POLLIN, 0};
            poll(eof ? nullptr : &p, eof ? 0 : 1, 25);
        }
    }
    close(pipeFd[0]);
    if (!exited)
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    result.success = result.error.empty() && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!result.success && result.error.empty())
        result.error = "Command denied or failed";
    return result;
}
}
