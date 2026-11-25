#include "backend.h"
#ifdef _WIN32
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static HANDLE model_in, model_out, model_process;

static void close_handle(HANDLE *handle) {
    if (*handle && *handle != INVALID_HANDLE_VALUE) CloseHandle(*handle);
    *handle = NULL;
}

void stop_model(void) {
    close_handle(&model_in);
    close_handle(&model_out);
    if (model_process) {
        if (WaitForSingleObject(model_process, 100) == WAIT_TIMEOUT) TerminateProcess(model_process, 1);
        close_handle(&model_process);
    }
}

static int start_model(void) {
    SECURITY_ATTRIBUTES security = {sizeof security, NULL, TRUE};
    HANDLE child_in = NULL, child_out = NULL, quiet = NULL;
    int started = 0;
    if (!CreatePipe(&child_in, &model_in, &security, 0) ||
        !CreatePipe(&model_out, &child_out, &security, 0) ||
        !SetHandleInformation(model_in, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(model_out, HANDLE_FLAG_INHERIT, 0)) goto cleanup;
    quiet = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &security,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (quiet == INVALID_HANDLE_VALUE) goto cleanup;
    STARTUPINFOA startup = {0};
    PROCESS_INFORMATION process;
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = child_in;
    startup.hStdOutput = child_out;
    startup.hStdError = quiet;
    char executable[4096], command[4160];
    DWORD length = GetEnvironmentVariableA("HARBORSHIFT_PYTHON", executable, sizeof executable);
    if (!length) {
        const char *local = ".venv\\Scripts\\python.exe";
        DWORD attributes = GetFileAttributesA(local);
        strcpy(executable, attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY) ?
               local : "python");
    }
    else if (length >= sizeof executable || strchr(executable, '"')) goto cleanup;
    snprintf(command, sizeof command, "\"%s\" -B -u -m harborshift.ml.worker", executable);
    if (CreateProcessA(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process)) {
        CloseHandle(process.hThread);
        model_process = process.hProcess;
        started = 1;
    }
cleanup:
    close_handle(&child_in);
    close_handle(&child_out);
    close_handle(&quiet);
    if (!started) stop_model();
    return started;
}

Json *predict(Json *features) {
    if (!model_process && !start_model()) return NULL;
    char *line = cJSON_PrintUnformatted(features);
    if (!line) return NULL;
    DWORD wrote, size = (DWORD)strlen(line);
    int sent = WriteFile(model_in, line, size, &wrote, NULL) && wrote == size &&
               WriteFile(model_in, "\n", 1, &wrote, NULL) && wrote == 1;
    cJSON_free(line);
    char response[4096];
    DWORD used = 0, start = GetTickCount();
    while (sent && used < sizeof response - 1 && GetTickCount() - start < 15000) {
        DWORD available, received;
        if (!PeekNamedPipe(model_out, NULL, 0, NULL, &available, NULL)) break;
        if (!available) { Sleep(5); continue; }
        if (available >= sizeof response - used) break;
        if (!ReadFile(model_out, response + used, available, &received, NULL) || !received) break;
        used += received;
        if (response[used - 1] == '\n') {
            response[used] = 0;
            return cJSON_ParseWithOpts(response, NULL, 1);
        }
    }
    stop_model();
    return NULL;
}

#else
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int model_in = -1, model_out = -1;
static pid_t model_pid = -1;

static void close_fd(int *fd) {
    if (*fd >= 0) close(*fd);
    *fd = -1;
}

void stop_model(void) {
    close_fd(&model_in);
    close_fd(&model_out);
    if (model_pid > 0) {
        kill(model_pid, SIGKILL);
        while (waitpid(model_pid, NULL, 0) < 0 && errno == EINTR) {}
        model_pid = -1;
    }
}

static int start_model(void) {
    const char *python = getenv("HARBORSHIFT_PYTHON");
    if (!python || !*python) python = access(".venv/bin/python", X_OK) == 0 ? ".venv/bin/python" : "python3";
    int input[2] = {-1, -1}, output[2] = {-1, -1};
    if (pipe(input) == 0 && pipe(output) == 0) {
        model_pid = fork();
        if (model_pid == 0) {
            close(input[1]);
            close(output[0]);
            if (dup2(input[0], STDIN_FILENO) < 0 || dup2(output[1], STDOUT_FILENO) < 0) _exit(126);
            if (input[0] != STDIN_FILENO) close(input[0]);
            if (output[1] != STDOUT_FILENO) close(output[1]);
            execlp(python, python, "-B", "-u", "-m", "harborshift.ml.worker", (char *)NULL);
            _exit(127);
        }
    }
    close_fd(&input[0]);
    close_fd(&output[1]);
    if (model_pid <= 0) {
        close_fd(&input[1]);
        close_fd(&output[0]);
        return 0;
    }
    signal(SIGPIPE, SIG_IGN);
    model_in = input[1];
    model_out = output[0];
    return 1;
}

static int write_all(const char *data, size_t length) {
    while (length) {
        ssize_t written = write(model_in, data, length);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return 0;
        data += written;
        length -= (size_t)written;
    }
    return 1;
}

static long long milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

Json *predict(Json *features) {
    if (model_pid < 0 && !start_model()) return NULL;
    char *line = cJSON_PrintUnformatted(features);
    if (!line) return NULL;
    int sent = write_all(line, strlen(line)) && write_all("\n", 1);
    cJSON_free(line);
    char response[4096];
    size_t used = 0;
    long long deadline = milliseconds() + 15000;
    while (sent && used < sizeof response - 1) {
        long long remaining = deadline - milliseconds();
        if (remaining <= 0) break;
        struct pollfd ready = {model_out, POLLIN, 0};
        int result = poll(&ready, 1, (int)remaining);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0 || !(ready.revents & POLLIN)) break;
        ssize_t received = read(model_out, response + used, sizeof response - 1 - used);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) break;
        used += (size_t)received;
        if (response[used - 1] == '\n') {
            response[used] = 0;
            return cJSON_ParseWithOpts(response, NULL, 1);
        }
    }
    stop_model();
    return NULL;
}
#endif
