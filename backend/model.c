#include "backend.h"
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
