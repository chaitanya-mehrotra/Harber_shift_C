#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define SD_SEND SHUT_WR
#define closesocket close
#define strnicmp strncasecmp
#endif
#include "backend.h"
#include "cli.h"
#include "data.h"
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { HEADER_LIMIT = 16384, BODY_LIMIT = 65536, FILE_LIMIT = 8 * 1024 * 1024 };
static SOCKET listener = INVALID_SOCKET;

static int send_all(SOCKET client, const char *data, size_t size) {
    while (size) {
        int bytes = send(client, data, (int)size, 0);
        if (bytes <= 0) return 0;
        data += bytes;
        size -= (size_t)bytes;
    }
    return 1;
}

static void response(SOCKET client, int code, const char *type, const char *body, size_t size) {
    const char *message = code == 200 ? "OK" : code == 400 ? "Bad Request" :
                          code == 404 ? "Not Found" : code == 405 ? "Method Not Allowed" :
                          code == 413 ? "Content Too Large" : code == 422 ? "Unprocessable Content" :
                          "Internal Server Error";
    char header[256];
    int length = snprintf(header, sizeof header,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\n"
        "X-Content-Type-Options: nosniff\r\n\r\n", code, message, type, (unsigned long)size);
    if (length > 0 && (size_t)length < sizeof header && send_all(client, header, (size_t)length))
        send_all(client, body, size);
}

static void json_response(SOCKET client, int code, Json *obj) {
    char *body = cJSON_PrintUnformatted(obj);
    const char *type = "application/json; charset=utf-8";
    if (body) {
        response(client, code, type, body, strlen(body));
        cJSON_free(body);
    } else {
        const char *message = "{\"detail\":\"Could not serialize response\"}";
        response(client, 500, type, message, strlen(message));
    }
    cJSON_Delete(obj);
}

static void problem(SOCKET client, int code, const char *detail) {
    Json *error = cJSON_CreateObject();
    cJSON_AddStringToObject(error, "detail", detail);
    json_response(client, code, error);
}

static int unhex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (unsigned char)tolower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static int decode_id(char *id) {
    char *out = id;
    for (char *in = id; *in; ++in) {
        unsigned char ch = (unsigned char)*in;
        if (ch == '%') {
            if (!in[1] || !in[2]) return 0;
            int high = unhex(in[1]), low = unhex(in[2]);
            if (high < 0 || low < 0) return 0;
            ch = (unsigned char)(16 * high + low);
            in += 2;
        }
        if (ch < 32 || ch == '/' || ch == '\\') return 0;
        *out++ = (char)ch;
    }
    *out = 0;
    return 1;
}

static Json *shipment_rows(Json *root) {
    Json *result = cJSON_CreateArray(), *contracts = get(root, "contracts");
    each(shipment, get(root, "shipments")) {
        Json *obj = cJSON_CreateObject();
        put(obj, "shipment", shipment);
        put(obj, "contract", lookup(contracts, "contract_id", str(shipment, "contract_id")));
        cJSON_AddItemToArray(result, obj);
    }
    return result;
}

static int static_asset(SOCKET client, const char *path) {
    const char *type = "text/html; charset=utf-8";
    const char *name = "frontend/dist/index.html";
    char file[1200];
    if (strncmp(path, "/assets/", 8) == 0 && path[8]) {
        const char *asset = path + 8;
        for (const unsigned char *c = (const unsigned char *)asset; *c; ++c)
            if (!isalnum(*c) && *c != '_' && *c != '-' && *c != '.') return 0;
        if (strstr(asset, "..")) return 0;
        const char *ext = strrchr(asset, '.');
        if (!ext) return 0;
        if (eq(ext, ".js")) type = "text/javascript; charset=utf-8";
        else if (eq(ext, ".css")) type = "text/css; charset=utf-8";
        else return 0;
        snprintf(file, sizeof file, "frontend/dist/assets/%s", asset);
        name = file;
    } else if (!eq(path, "/") && !eq(path, "/index.html")) return 0;
    size_t size;
    char *data = read_file(name, FILE_LIMIT, &size);
    if (!data) return 0;
    response(client, 200, type, data, size);
    free(data);
    return 1;
}

static void dispatch(SOCKET client, const Network *net, char *method, char *path, char *body, size_t length) {
    char *query_string = strchr(path, '?');
    if (query_string) *query_string = 0;
    if (!strncmp(path, "/api/", 5)) path += 4;
    int is_get = eq(method, "GET"), is_post = eq(method, "POST");
    if (eq(path, "/route") || eq(path, "/simulate")) {
        if (!is_post) { problem(client, 405, "This endpoint requires POST"); return; }
        Json *query = memchr(body, 0, length) ? NULL : cJSON_ParseWithOpts(body, NULL, 1);
        if (!cJSON_IsObject(query)) {
            cJSON_Delete(query);
            problem(client, 422, "Expected a JSON object");
            return;
        }
        Error error = {{0}};
        Json *result = eq(path, "/route") ? route_request(net, query, &error) : simulate(net, query, &error);
        cJSON_Delete(query);
        if (!result) problem(client, 422, error.message);
        else json_response(client, 200, result);
        return;
    }
    if (!is_get) { problem(client, 405, "This endpoint requires GET"); return; }
    if (eq(path, "/health")) {
        Json *status = cJSON_CreateObject();
        cJSON_AddStringToObject(status, "status", "ready");
        cJSON_AddStringToObject(status, "data_origin", "synthetic");
        cJSON_AddNumberToObject(status, "shipment_count", cJSON_GetArraySize(get(net->root, "shipments")));
        json_response(client, 200, status);
        return;
    }
    if (eq(path, "/shipments")) { json_response(client, 200, shipment_rows(net->root)); return; }
    if (eq(path, "/news")) { json_response(client, 200, news(net)); return; }
    if (eq(path, "/ports") || eq(path, "/routes") || eq(path, "/disruptions")) {
        json_response(client, 200, clone(get(net->root, path + 1)));
        return;
    }
    const char *prefix = "/shipments/", *suffix = "/itinerary";
    size_t path_size = strlen(path), prefix_size = strlen(prefix), suffix_size = strlen(suffix);
    if (path_size > prefix_size + suffix_size && !strncmp(path, prefix, prefix_size) &&
        eq(path + path_size - suffix_size, suffix)) {
        path[path_size - suffix_size] = 0;
        char *id = path + prefix_size;
        if (!decode_id(id)) { problem(client, 400, "Invalid shipment ID"); return; }
        if (!lookup(get(net->root, "shipments"), "shipment_id", id)) {
            char message[1200];
            snprintf(message, sizeof message, "Unknown shipment ID: %s", id);
            problem(client, 404, message);
        } else {
            Json *legs = cJSON_CreateArray();
            each(leg, get(net->root, "shipment_legs"))
                if (eq(str(leg, "shipment_id"), id)) cJSON_AddItemToArray(legs, clone(leg));
            json_response(client, 200, legs);
        }
        return;
    }
    if (!static_asset(client, path)) problem(client, 404, "Not Found");
}

static int parse_size(char *headers, size_t *size) {
    int seen = 0;
    char *line = headers;
    while (line && *line) {
        char *end = strstr(line, "\r\n");
        if (end) *end = 0;
        if (!strnicmp(line, "Transfer-Encoding:", 18)) return 400;
        if (!strnicmp(line, "Content-Length:", 15)) {
            if (seen++) return 400;
            char *value = line + 15;
            while (*value == ' ' || *value == '\t') ++value;
            if (!isdigit((unsigned char)*value)) return 400;
            size_t total = 0;
            while (isdigit((unsigned char)*value)) {
                total = 10 * total + (unsigned)(*value++ - '0');
                if (total > BODY_LIMIT) return 413;
            }
            while (*value == ' ' || *value == '\t') ++value;
            if (*value) return 400;
            *size = total;
        }
        line = end ? end + 2 : NULL;
    }
    return 200;
}

static void handle(SOCKET client, const Network *net) {
    char *request = malloc(HEADER_LIMIT + BODY_LIMIT + 1);
    if (!request) { problem(client, 500, "Could not allocate request"); return; }
    size_t used = 0;
    char *header_end = NULL;
    while (used < HEADER_LIMIT) {
        int count = recv(client, request + used, HEADER_LIMIT - (int)used, 0);
        if (count <= 0) goto cleanup;
        used += (size_t)count;
        request[used] = 0;
        header_end = strstr(request, "\r\n\r\n");
        if (header_end) break;
    }
    if (!header_end) { problem(client, 400, "Malformed or oversized request headers"); goto cleanup; }
    char *body = header_end + 4;
    char *line_end = strstr(request, "\r\n");
    if (!line_end || line_end > header_end) { problem(client, 400, "Invalid request line"); goto cleanup; }
    *header_end = 0;
    *line_end = 0;
    char method[16], path[1024], protocol[16], extra[2];
    if (sscanf(request, "%15s %1023s %15s %1s", method, path, protocol, extra) != 3 || path[0] != '/' ||
        (!eq(protocol, "HTTP/1.1") && !eq(protocol, "HTTP/1.0"))) {
        problem(client, 400, "Invalid request line");
        goto cleanup;
    }
    size_t length = 0;
    int code = parse_size(line_end == header_end ? NULL : line_end + 2, &length);
    if (code != 200) { problem(client, code, "Invalid HTTP message length"); goto cleanup; }
    size_t current = used - (size_t)(body - request);
    while (current < length) {
        int count = recv(client, body + current, (int)(length - current), 0);
        if (count <= 0) { problem(client, 400, "Incomplete request body"); goto cleanup; }
        current += (size_t)count;
    }
    body[length] = 0;
    dispatch(client, net, method, path, body, length);
cleanup:
    free(request);
}

static void interrupt(int signal_number) {
    (void)signal_number;
    if (listener != INVALID_SOCKET) {
        closesocket(listener);
        listener = INVALID_SOCKET;
    }
}

static void prevent_inherit(SOCKET fd) {
#ifdef _WIN32
    SetHandleInformation((HANDLE)fd, HANDLE_FLAG_INHERIT, 0);
#else
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
}

static void set_socket_timeouts(SOCKET fd) {
#ifdef _WIN32
    DWORD timeout = 5000;
#else
    struct timeval timeout = {5, 0};
#endif
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof timeout);
}

int main(int argc, char **argv) {
    int port = 8000;
    const char *source = NULL;
    if (argc == 3 && eq(argv[1], "import")) return validate_csv(argv[2], 1);
    for (int i = 2; i + 1 < argc; ++i) {
        if (!eq(argv[i], "--data")) continue;
        source = argv[i + 1];
        memmove(argv + i, argv + i + 2, (argc - i - 1) * sizeof *argv);
        argc -= 2;
        break;
    }
    int command = argc >= 2 && (eq(argv[1], "route") || eq(argv[1], "simulate") ||
                                eq(argv[1], "neighbors") || eq(argv[1], "validate"));
    if (command && source && eq(argv[1], "validate")) return validate_csv(source, 0);
    const char *port_value = argc == 2 ? argv[1] : getenv("PORT");
    if (!command && argc <= 2 && port_value && *port_value) {
        char *end;
        long value = strtol(port_value, &end, 10);
        if (end == port_value || *end || value < 1 || value > 65535) {
            fprintf(stderr, "Port must be an integer from 1 to 65535.\n");
            return 1;
        }
        port = (int)value;
    } else if (!command && argc != 1) {
        fprintf(stderr, "Usage: harborshift.exe [PORT | validate | neighbors | route | simulate | import]\n");
        return 1;
    }
    Error error = {{0}};
    Json *data;
    if (source) data = read_dataset(source, &error);
    else {
        char *text = read_file("data/network.json", FILE_LIMIT, NULL);
        data = text ? cJSON_ParseWithOpts(text, NULL, 1) : NULL;
        free(text);
    }
    Network net = {0};
    if (!network_init(&net, data, &error)) {
        fprintf(stderr, "Could not load data/network.json: %s\nRun from the project folder.\n", error.message);
        network_destroy(&net);
        return 1;
    }
    Error fixture_error = {{0}};
    if (!attach_manufacturing(&net, &fixture_error)) {
        cJSON_DeleteItemFromObjectCaseSensitive(data, "demo");
        cJSON_AddStringToObject(data, "demo_error", fixture_error.message);
    }
    if (command) {
        int result = cli(&net, argc - 1, argv + 1);
        stop_model();
        network_destroy(&net);
        return result;
    }
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) { network_destroy(&net); return 1; }
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener != INVALID_SOCKET) prevent_inherit(listener);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    const char *bind_all = getenv("HARBORSHIFT_BIND_ALL");
    int public_bind = bind_all && eq(bind_all, "1");
    addr.sin_addr.s_addr = htonl(public_bind ? INADDR_ANY : INADDR_LOOPBACK);
    const char *host = public_bind ? "0.0.0.0" : "127.0.0.1";
    int exit_code = 0;
    if (listener == INVALID_SOCKET || bind(listener, (struct sockaddr *)&addr, sizeof addr) == SOCKET_ERROR ||
        listen(listener, SOMAXCONN) == SOCKET_ERROR) {
#ifdef _WIN32
        fprintf(stderr, "Could not listen on %s:%d (Winsock error %d).\n", host, port, WSAGetLastError());
#else
        fprintf(stderr, "Could not listen on %s:%d: %s\n", host, port, strerror(errno));
#endif
        exit_code = 1;
    } else {
        signal(SIGINT, interrupt);
        signal(SIGTERM, interrupt);
        printf("HarborShift C server ready at http://%s:%d/\n", host, port);
        fflush(stdout);
        for (;;) {
            SOCKET client = accept(listener, NULL, NULL);
            if (client == INVALID_SOCKET) break;
            prevent_inherit(client);
            set_socket_timeouts(client);
            handle(client, &net);
            shutdown(client, SD_SEND);
            closesocket(client);
        }
    }
    stop_model();
    clear_news();
    if (listener != INVALID_SOCKET) closesocket(listener);
#ifdef _WIN32
    WSACleanup();
#endif
    network_destroy(&net);
    return exit_code;
}
