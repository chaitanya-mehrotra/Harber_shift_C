#include "cli.h"
#include "data.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { const char *flag, *key; char kind; } Flag;

static void print_json(Json *result) {
    char *text = cJSON_Print(result);
    if (text) { puts(text); cJSON_free(text); }
    cJSON_Delete(result);
}

int validate_csv(const char *directory, int export_json) {
    Error error = {{0}};
    Json *data = read_dataset(directory, &error);
    if (!data) { fprintf(stderr, "%s\n", error.message); return 1; }
    if (export_json) print_json(data);
    else {
        puts("Validated CSV dataset.");
        each(table, data)
            if (!eq(table->string, "warnings")) printf("  %s: %d\n", table->string, cJSON_GetArraySize(table));
        each(warning, get(data, "warnings")) printf("  WARNING: %s\n", string(warning));
        cJSON_Delete(data);
    }
    return 0;
}

static const Flag routing_flags[] = {
    {"--quantity", "quantity_units", 'n'}, {"--units-per-container", "units_per_container", 'n'},
    {"--objective", "objective", 's'}, {"--via", "required_stops", 'a'},
    {"--close-port", "closed_ports", 'a'}, {"--close-route", "closed_routes", 'a'}, {NULL}
};
static const Flag scenario_flags[] = {
    {"--as-of", "as_of", 's'}, {"--event", "sample_ids", 'a'}, {"--port", "port_id", 'e'},
    {"--event-type", "event_type", 'e'}, {"--duration", "duration_days", 'i'}, {"--severity", "severity", 'i'},
    {"--status", "port_status", 'e'}, {"--start", "start", 'e'}, {"--manufacturing", "manufacturing", 'b'},
    {"--recover", "recover", 'b'}, {"--shipment", "shipment_id", 's'},
    {"--units-per-container", "units_per_container", 'n'}, {"--original-purchase-inr", "original_purchase_inr", 's'},
    {"--use-ml", "use_ml", 'b'}, {"--congestion", "congestion", 'n'}, {"--buffer-days", "buffer_days", 'n'}, {NULL}
};

static Json *parse_flags(int count, char **args, int route, Error *error) {
    Json *query = cJSON_CreateObject();
    int start = 1;
    if (route) {
        if (count < 3) {
            fail(error, "Usage: route ORIGIN DESTINATION --quantity N --units-per-container N");
            return query;
        }
        cJSON_AddStringToObject(query, "origin", args[1]);
        cJSON_AddStringToObject(query, "destination", args[2]);
        start = 3;
    }
    for (int i = start; i < count && !error->message[0]; ++i) {
        const Flag *flag = route ? routing_flags : scenario_flags;
        while (flag->flag && !eq(flag->flag, args[i])) ++flag;
        if (!flag->flag) { fail(error, "Unknown option: %s", args[i]); break; }
        Json *target = query;
        if (flag->kind == 'e' || flag->kind == 'i') {
            target = get(query, "event");
            if (!target) target = cJSON_AddObjectToObject(query, "event");
        }
        if (flag->kind == 'b') { cJSON_AddTrueToObject(target, flag->key); continue; }
        if (++i >= count) { fail(error, "Missing value for %s", flag->flag); break; }
        if (flag->kind == 'a') {
            Json *array = get(target, flag->key);
            if (!array) array = cJSON_AddArrayToObject(target, flag->key);
            push(array, args[i]);
        } else if (flag->kind == 'i' || flag->kind == 'n') {
            char *end;
            errno = 0;
            long value = strtol(args[i], &end, 10);
            if (end == args[i] || *end || errno == ERANGE || value < INT_MIN || value > INT_MAX)
                fail(error, "Expected an integer for %s", flag->flag);
            else cJSON_AddNumberToObject(target, flag->key, value);
        } else cJSON_AddStringToObject(target, flag->key, args[i]);
    }
    return query;
}

int cli(const Network *net, int count, char **args) {
    Error error = {{0}};
    if (eq(args[0], "validate")) {
        if (count != 1) { fprintf(stderr, "Usage: validate [--data CSV_DIRECTORY]\n"); return 1; }
        puts("Loaded the validated network dataset.");
        each(table, net->root) {
            if (cJSON_IsArray(table) && !eq(table->string, "warnings"))
                printf("  %s: %d\n", table->string, cJSON_GetArraySize(table));
        }
        each(warning, get(net->root, "warnings")) printf("  WARNING: %s\n", string(warning));
        return 0;
    }
    if (eq(args[0], "neighbors")) {
        if (count != 2) { fprintf(stderr, "Usage: neighbors PORT_ID\n"); return 1; }
        int port = port_index(net, args[1]);
        if (port < 0) { fprintf(stderr, "Unknown port ID: %s\n", args[1]); return 1; }
        printf("Graph: %d ports, %d routes\n", net->np, net->ne);
        printf("Outgoing routes from %s (%s)\n", str(net->ports[port], "port_name"), args[1]);
        for (int i = net->offset[port]; i < net->offset[port + 1]; ++i) {
            int edge = net->adjacent[i];
            Json *route = net->edges[edge], *next = net->ports[net->to[edge]];
            printf("  %s -> %s (%s): %.0f days, %.2f INR/container, capacity %.0f, %s\n",
                   str(route, "route_id"), str(next, "port_name"), str(next, "port_id"),
                   num(route, "transit_days"), num(route, "cost_inr_per_container"),
                   num(route, "capacity_containers"), str(route, "status"));
        }
        return 0;
    }
    int route = eq(args[0], "route");
    Json *query = parse_flags(count, args, route, &error), *result = NULL;
    if (!error.message[0]) {
        result = route ? route_request(net, query, &error) : simulate(net, query, &error);
        if (route && result && (get(query, "closed_ports") || get(query, "closed_routes"))) {
            cJSON_DeleteItemFromObjectCaseSensitive(query, "closed_ports");
            cJSON_DeleteItemFromObjectCaseSensitive(query, "closed_routes");
            Json *comparison = cJSON_CreateObject();
            cJSON_AddItemToObject(comparison, "baseline", route_request(net, query, &error));
            cJSON_AddItemToObject(comparison, "scenario", result);
            result = comparison;
        }
    }
    cJSON_Delete(query);
    if (error.message[0]) { cJSON_Delete(result); fprintf(stderr, "%s\n", error.message); return 1; }
    print_json(result);
    return 0;
}
