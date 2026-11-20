#include "proc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#ifdef _WIN32

#include <windows.h>

static void append_quoted(char *dst, size_t cap, const char *arg)
{
    cs_strlcat(dst, "\"", cap);
    for (const char *p = arg; *p; p++) {
        if (*p == '"') cs_strlcat(dst, "\\\"", cap);
        else {
            char one[2] = {*p, 0};
            cs_strlcat(dst, one, cap);
        }
    }
    cs_strlcat(dst, "\"", cap);
}

bool proc_run_in(const char *cwd, const char *const argv[], char *out, size_t out_cap,
                 int timeout_ms, int *exit_code, bool *timed_out, char *err, size_t err_cap)
{
    if (out && out_cap) out[0] = '\0';
    if (err && err_cap) err[0] = '\0';
    if (exit_code) *exit_code = -1;
    if (timed_out) *timed_out = false;
    if (!argv || !argv[0]) return false;

    char cmdline[8192] = {0};
    for (int i = 0; argv[i]; i++) {
        if (i) cs_strlcat(cmdline, " ", sizeof(cmdline));
        append_quoted(cmdline, sizeof(cmdline), argv[i]);
    }

    SECURITY_ATTRIBUTES sa = {0};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        snprintf(err, err_cap, "CreatePipe failed: %lu", GetLastError());
        return false;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {0};
    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL,
                             cwd && *cwd ? cwd : NULL, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        snprintf(err, err_cap, "CreateProcess failed: %lu", GetLastError());
        CloseHandle(rd);
        return false;
    }

    char buf[4096];
    DWORD total = 0;
    DWORD waited = 0;
    bool done = false;
    while (!done) {
        DWORD avail = 0;
        if (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            DWORD want = avail < sizeof(buf) ? avail : (DWORD)sizeof(buf);
            DWORD got = 0;
            if (ReadFile(rd, buf, want, &got, NULL) && got > 0) {
                if (out && total + 1 < out_cap) {
                    DWORD room = (DWORD)(out_cap - 1 - total);
                    DWORD take = got < room ? got : room;
                    memcpy(out + total, buf, take);
                    total += take;
                    out[total] = '\0';
                }
                continue;
            }
        }
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            /* Drain once more, then finish. */
            while (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
                DWORD got = 0;
                if (!ReadFile(rd, buf, sizeof(buf), &got, NULL) || got == 0) break;
                if (out && total + 1 < out_cap) {
                    DWORD room = (DWORD)(out_cap - 1 - total);
                    DWORD take = got < room ? got : room;
                    memcpy(out + total, buf, take);
                    total += take;
                    out[total] = '\0';
                }
            }
            done = true;
            break;
        }
        if (waited >= (DWORD)timeout_ms) {
            TerminateProcess(pi.hProcess, 124);
            if (timed_out) *timed_out = true;
            done = true;
            break;
        }
        Sleep(15);
        waited += 15;
    }

    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    if (exit_code) *exit_code = (int)code;
    CloseHandle(rd);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (timed_out && *timed_out) {
        snprintf(err, err_cap, "Process timed out after %d ms", timeout_ms);
    }
    return true;
}

bool proc_run(const char *const argv[], char *out, size_t out_cap, int timeout_ms,
              int *exit_code, bool *timed_out, char *err, size_t err_cap)
{
    return proc_run_in(NULL, argv, out, out_cap, timeout_ms, exit_code, timed_out, err,
                       err_cap);
}

bool proc_run_stream(const char *cwd, const char *const argv[], int *exit_code)
{
    if (exit_code) *exit_code = -1;
    if (!argv || !argv[0]) return false;
    char cmdline[8192] = {0};
    for (int i = 0; argv[i]; i++) {
        if (i) cs_strlcat(cmdline, " ", sizeof(cmdline));
        append_quoted(cmdline, sizeof(cmdline), argv[i]);
    }
    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {0};
    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL,
                             cwd && *cwd ? cwd : NULL, &si, &pi);
    if (!ok) {
        fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    if (exit_code) *exit_code = (int)code;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool proc_find_in_path(const char *name, char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    if (!name) return false;
    char env[32768];
    DWORD n = GetEnvironmentVariableA("PATH", env, sizeof(env));
    if (n == 0 || n >= sizeof(env)) return false;
    const char *exts = ".EXE;.CMD;.BAT;";
    char *p = env;
    while (p && *p) {
        char *semi = strchr(p, ';');
        if (semi) *semi = '\0';
        char candidate[CS_PATH_MAX];
        if (strchr(name, '.') || strchr(name, '\\') || strchr(name, '/')) {
            cs_path_join(candidate, sizeof(candidate), p, name);
            if (cs_file_exists(candidate)) {
                cs_strlcpy(out, candidate, cap);
                return true;
            }
        } else {
            char base[512];
            cs_strlcpy(base, name, sizeof(base));
            cs_strlcat(base, ".exe", sizeof(base));
            cs_path_join(candidate, sizeof(candidate), p, base);
            if (cs_file_exists(candidate)) {
                cs_strlcpy(out, candidate, cap);
                return true;
            }
        }
        p = semi ? semi + 1 : NULL;
    }
    (void)exts;
    return false;
}

#else

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

bool proc_run_in(const char *cwd, const char *const argv[], char *out, size_t out_cap,
                 int timeout_ms, int *exit_code, bool *timed_out, char *err, size_t err_cap)
{
    if (out && out_cap) out[0] = '\0';
    if (err && err_cap) err[0] = '\0';
    if (exit_code) *exit_code = -1;
    if (timed_out) *timed_out = false;
    if (!argv || !argv[0]) return false;

    int p[2];
    if (pipe(p) != 0) {
        snprintf(err, err_cap, "pipe failed: %s", strerror(errno));
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, err_cap, "fork failed: %s", strerror(errno));
        close(p[0]);
        close(p[1]);
        return false;
    }
    if (pid == 0) {
        close(p[0]);
        dup2(p[1], STDOUT_FILENO);
        dup2(p[1], STDERR_FILENO);
        close(p[1]);
        if (cwd && *cwd) chdir(cwd);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        execvp(argv[0], (char *const *)argv);
        fprintf(stderr, "exec %s failed: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    close(p[1]);
    int flags = fcntl(p[0], F_GETFL, 0);
    fcntl(p[0], F_SETFL, flags | O_NONBLOCK);

    size_t total = 0;
    int waited = 0;
    bool eof = false;
    bool reaped = false;
    int status = 0;
    char buf[4096];

    while (!eof || !reaped) {
        struct pollfd pfd = {p[0], POLLIN, 0};
        int pr = poll(&pfd, 1, 20);
        if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
            for (;;) {
                ssize_t got = read(p[0], buf, sizeof(buf));
                if (got > 0) {
                    if (out && total + 1 < out_cap) {
                        size_t room = out_cap - 1 - total;
                        size_t take = (size_t)got < room ? (size_t)got : room;
                        memcpy(out + total, buf, take);
                        total += take;
                        out[total] = '\0';
                    }
                } else if (got == 0) {
                    eof = true;
                    break;
                } else {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) eof = true;
                    break;
                }
            }
        } else if (pr == 0) {
            waited += 20;
        }

        if (!reaped) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) reaped = true;
        }
        if (waited >= timeout_ms) {
            if (!reaped) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                reaped = true;
            }
            eof = true;
            if (timed_out) *timed_out = true;
        }
        if (eof && reaped) break;
    }

    close(p[0]);
    if (exit_code) *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (timed_out && *timed_out) {
        snprintf(err, err_cap, "Process timed out after %d ms", timeout_ms);
    }
    return true;
}

bool proc_run(const char *const argv[], char *out, size_t out_cap, int timeout_ms,
              int *exit_code, bool *timed_out, char *err, size_t err_cap)
{
    return proc_run_in(NULL, argv, out, out_cap, timeout_ms, exit_code, timed_out, err,
                       err_cap);
}

bool proc_run_stream(const char *cwd, const char *const argv[], int *exit_code)
{
    if (exit_code) *exit_code = -1;
    if (!argv || !argv[0]) return false;
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        if (cwd && *cwd) chdir(cwd);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    if (exit_code) *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return true;
}

bool proc_find_in_path(const char *name, char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    if (!name) return false;
    if (strchr(name, '/')) {
        if (cs_file_exists(name)) {
            cs_strlcpy(out, name, cap);
            return true;
        }
        return false;
    }
    const char *path = getenv("PATH");
    if (!path) return false;
    char tmp[32768];
    cs_strlcpy(tmp, path, sizeof(tmp));
    char *p = tmp;
    while (p && *p) {
        char *colon = strchr(p, ':');
        if (colon) *colon = '\0';
        char candidate[CS_PATH_MAX];
        cs_path_join(candidate, sizeof(candidate), p, name);
        if (cs_file_exists(candidate)) {
            cs_strlcpy(out, candidate, cap);
            return true;
        }
        p = colon ? colon + 1 : NULL;
    }
    return false;
}

#endif
