#include "backend.h"
#include <stdlib.h>
#include <stdio.h>
#include <float.h>
#include <string.h>

int port_index(const Network *network, const char *id) {
    for (int i = 0; i < network->np; ++i)
        if (eq(str(network->ports[i], "port_id"), id)) return i;
    return -1;
}

int edge_index(const Network *network, const char *id) {
    for (int i = 0; i < network->ne; ++i)
        if (eq(str(network->edges[i], "route_id"), id)) return i;
    return -1;
}

int network_init(Network *network, Json *data, Error *error) {
    network->root = data;
    Json *ports = get(data, "ports"), *edges = get(data, "routes");
    network->np = cJSON_GetArraySize(ports);
    network->ne = cJSON_GetArraySize(edges);
    int np = network->np, ne = network->ne;
    if (np <= 0 || ne <= 0 || np > 4096 || ne > 65536) {
        fail(error, "Invalid port or route count in network data");
        return 0;
    }
    network->ports = calloc(np, sizeof *network->ports);
    network->edges = calloc(ne, sizeof *network->edges);
    network->offset = calloc(np + 1, sizeof *network->offset);
    network->adjacent = calloc(ne, sizeof *network->adjacent);
    network->from = calloc(ne, sizeof *network->from);
    network->to = calloc(ne, sizeof *network->to);
    int *cursor = calloc(np, sizeof *cursor);
    if (!network->ports || !network->edges || !network->offset || !network->adjacent ||
        !network->from || !network->to || !cursor) {
        free(cursor);
        fail(error, "Could not allocate the transport graph");
        return 0;
    }
    int index = 0;
    each(port, ports) network->ports[index++] = port;
    index = 0;
    each(edge, edges) {
        int from = port_index(network, str(edge, "from_port_id"));
        int to = port_index(network, str(edge, "to_port_id"));
        if (from < 0 || to < 0 || num(edge, "transit_days") < 0 || num(edge, "cost_inr_per_container") < 0) {
            free(cursor);
            fail(error, "Invalid route: %s", str(edge, "route_id"));
            return 0;
        }
        network->edges[index] = edge;
        network->from[index] = from;
        network->to[index++] = to;
        ++network->offset[from + 1];
    }
    for (int i = 0; i < np; ++i) {
        network->offset[i + 1] += network->offset[i];
        cursor[i] = network->offset[i];
    }
    for (int i = 0; i < ne; ++i) network->adjacent[cursor[network->from[i]]++] = i;
    free(cursor);
    return 1;
}

void network_destroy(Network *network) {
    free(network->ports); free(network->edges); free(network->offset);
    free(network->adjacent); free(network->from); free(network->to);
    cJSON_Delete(network->root);
}

typedef struct { long double weight; int port; } Entry;
typedef struct { Entry *entries; int size; const Network *network; } Heap;

static int less(const Heap *heap, Entry a, Entry b) {
    if (a.weight != b.weight) return a.weight < b.weight;
    return strcmp(str(heap->network->ports[a.port], "port_id"),
                  str(heap->network->ports[b.port], "port_id")) < 0;
}

static void enqueue(Heap *heap, Entry entry) {
    int index = heap->size++;
    while (index > 0) {
        int parent = (index - 1) / 2;
        if (!less(heap, entry, heap->entries[parent])) break;
        heap->entries[index] = heap->entries[parent];
        index = parent;
    }
    heap->entries[index] = entry;
}

static Entry dequeue(Heap *heap) {
    Entry result = heap->entries[0], last = heap->entries[--heap->size];
    int index = 0;
    while (2 * index + 1 < heap->size) {
        int child = 2 * index + 1;
        if (child + 1 < heap->size && less(heap, heap->entries[child + 1], heap->entries[child])) ++child;
        if (!less(heap, heap->entries[child], last)) break;
        heap->entries[index] = heap->entries[child];
        index = child;
    }
    heap->entries[index] = last;
    return result;
}

static int segment(const Network *net, const char *origin, const char *destination, int cheapest,
                   const unsigned char *blocked, Json *ports, Json *routes, double *days, double *cost,
                   Json *trace, int leg, Error *error) {
    int start = port_index(net, origin), end = port_index(net, destination);
    long double *best = malloc(net->np * sizeof *best);
    int *previous = malloc(net->np * sizeof *previous), *path = malloc(net->np * sizeof *path);
    Heap heap = {malloc((net->ne + 1) * sizeof(Entry)), 0, net};
    int found = 0;
    if (!best || !previous || !path || !heap.entries) {
        fail(error, "Could not allocate Dijkstra workspace");
        goto cleanup;
    }
    for (int i = 0; i < net->np; ++i) best[i] = LDBL_MAX;
    best[start] = 0;
    enqueue(&heap, (Entry){0, start});
    while (heap.size) {
        Entry head = dequeue(&heap);
        int port = head.port;
        if (head.weight != best[port]) continue;
        Json *updates = NULL;
        if (trace) {
            Json *step = cJSON_CreateObject();
            cJSON_AddItemToArray(trace, step);
            cJSON_AddNumberToObject(step, "segment", leg);
            cJSON_AddStringToObject(step, "port_id", str(net->ports[port], "port_id"));
            cJSON_AddNumberToObject(step, "weight", (double)head.weight);
            cJSON_AddStringToObject(step, "target_port_id", destination);
            updates = cJSON_AddArrayToObject(step, "updates");
        }
        if (port == end) { found = 1; break; }
        for (int i = net->offset[port]; i < net->offset[port + 1]; ++i) {
            int edge = net->adjacent[i], next = net->to[edge];
            if (blocked[edge]) continue;
            long double weight = num(net->edges[edge], cheapest ? "cost_inr_per_container" : "transit_days");
            long double candidate = head.weight + weight;
            if (candidate >= best[next]) continue;
            best[next] = candidate;
            previous[next] = edge;
            enqueue(&heap, (Entry){candidate, next});
            if (updates) {
                Json *update = cJSON_CreateObject();
                cJSON_AddItemToArray(updates, update);
                cJSON_AddStringToObject(update, "route_id", str(net->edges[edge], "route_id"));
                cJSON_AddStringToObject(update, "port_id", str(net->ports[next], "port_id"));
                cJSON_AddNumberToObject(update, "weight", (double)candidate);
            }
        }
    }
    if (found) {
        int count = 0;
        for (int port = end; port != start; port = net->from[previous[port]]) path[count++] = previous[port];
        while (count) {
            Json *edge = net->edges[path[--count]];
            push(routes, str(edge, "route_id"));
            push(ports, str(edge, "to_port_id"));
            *days += num(edge, "transit_days");
            *cost += num(edge, "cost_inr_per_container");
        }
    }
cleanup:
    free(best); free(previous); free(path); free(heap.entries);
    return found;
}

Json *shortest(const Network *net, const char *origin, const char *destination,
               int containers, int cheapest, Json *stops, Json *closed_ports, Json *closed_routes, Json *trace, Error *error) {
    const char *ordered[22] = {origin};
    int count = 1;
    each(stop, stops) ordered[count++] = string(stop);
    ordered[count++] = destination;
    for (int i = 0; i < count; ++i)
        if (port_index(net, ordered[i]) < 0) fail(error, "Unknown port ID: %s", ordered[i]);
    each(port, closed_ports)
        if (port_index(net, string(port)) < 0) fail(error, "Unknown port ID: %s", string(port));
    each(edge, closed_routes)
        if (edge_index(net, string(edge)) < 0) fail(error, "Unknown route ID: %s", string(edge));
    if (error->message[0]) return NULL;

    unsigned char *blocked = calloc(net->ne, 1);
    if (!blocked) { fail(error, "Could not allocate route filters"); return NULL; }
    Json *result = cJSON_CreateObject(), *exclusions = cJSON_AddArrayToObject(result, "excluded_edges");
    char reason[512] = "";
    for (int i = 0; i < net->ne; ++i) {
        Json *edge = net->edges[i], *reasons = cJSON_CreateArray();
        const char *id = str(edge, "route_id");
        if (eq(str(edge, "status"), "closed")) push(reasons, "marked closed in route data");
        if (has(closed_routes, id)) push(reasons, "closed in this snapshot");
        const char *ends[] = {str(edge, "from_port_id"), str(edge, "to_port_id")};
        for (int j = 0; j < 2; ++j) if (has(closed_ports, ends[j])) {
            snprintf(reason, sizeof reason, "port %s is closed", ends[j]);
            push(reasons, reason);
        }
        int capacity = (int)num(edge, "capacity_containers");
        if (capacity < containers) {
            snprintf(reason, sizeof reason, "capacity %d below %d required", capacity, containers);
            push(reasons, reason);
        }
        if (reasons->child) {
            blocked[i] = 1;
            Json *excluded = cJSON_CreateObject();
            cJSON_AddStringToObject(excluded, "route_id", id);
            cJSON_AddItemToObject(excluded, "reasons", reasons);
            cJSON_AddItemToArray(exclusions, excluded);
        } else cJSON_Delete(reasons);
    }
    reason[0] = 0;
    Json *ports = cJSON_CreateArray(), *routes = cJSON_CreateArray();
    double days = 0, cost = 0;
    for (int i = 0; i < count; ++i) if (has(closed_ports, ordered[i])) {
        snprintf(reason, sizeof reason, "Required port %s is closed", ordered[i]);
        break;
    }
    push(ports, origin);
    for (int i = 1; i < count && !reason[0]; ++i) {
        if (!segment(net, ordered[i-1], ordered[i], cheapest, blocked, ports, routes, &days, &cost, trace, i, error))
            snprintf(reason, sizeof reason, "No feasible path from %s to %s", ordered[i-1], ordered[i]);
    }
    free(blocked);
    cJSON_AddNumberToObject(result, "required_containers", containers);
    if (reason[0]) {
        cJSON_Delete(ports); cJSON_Delete(routes);
        ports = cJSON_CreateArray(); routes = cJSON_CreateArray();
        cJSON_AddNullToObject(result, "total_days");
        cJSON_AddNullToObject(result, "freight_inr");
        cJSON_AddStringToObject(result, "no_route_reason", reason);
    } else {
        cJSON_AddNumberToObject(result, "total_days", days);
        cJSON_AddNumberToObject(result, "freight_inr", cost * containers);
        cJSON_AddNullToObject(result, "no_route_reason");
    }
    cJSON_AddItemToObject(result, "port_ids", ports);
    cJSON_AddItemToObject(result, "route_ids", routes);
    if (error->message[0]) { cJSON_Delete(result); return NULL; }
    return result;
}

Json *route_request(const Network *net, Json *query, Error *error) {
    int quantity = integer(query, "quantity_units", 1, INT_MAX, 0, error);
    int packing = integer(query, "units_per_container", 1, INT_MAX, 0, error);
    const char *objective = get(query, "objective") ? str(query, "objective") : "fastest";
    if (!eq(objective, "fastest") && !eq(objective, "cheapest")) fail(error, "Unknown routing objective: %s", objective);
    list(query, "required_stops", 20, error);
    list(query, "closed_ports", 20, error);
    list(query, "closed_routes", 100, error);
    int include_trace = read_bool(query, "include_trace", error);
    if (error->message[0]) return NULL;
    Json *trace = include_trace ? cJSON_CreateArray() : NULL;
    Json *result = shortest(net, str(query, "origin"), str(query, "destination"), 1 + (quantity - 1) / packing,
                    eq(objective, "cheapest"), get(query, "required_stops"), get(query, "closed_ports"),
                    get(query, "closed_routes"), trace, error);
    if (result && trace) cJSON_AddItemToObject(result, "search_trace", trace);
    else cJSON_Delete(trace);
    return result;
}
