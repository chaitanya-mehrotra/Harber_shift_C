#include "data.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_COLUMNS = 64, CSV_LIMIT = 16 * 1024 * 1024 };
typedef struct { const char *name; char type; const char *constraint; } Column;
typedef struct { const char *name; int optional; Column columns[12]; } Table;

#define S(name) {name, 's', NULL}
#define N(name) {name, 'n', NULL}
#define I(name) {name, 'i', NULL}
#define P(name) {name, 'p', NULL}
#define C(name) {name, 'c', NULL}
#define D(name) {name, 'd', NULL}
#define E(name, choices) {name, 's', choices}
#define R(name, table) {name, 'r', table}
#define EVENTS "congestion|equipment_failure|severe_weather|strike"
#define PORT_STATUS "closed|restricted"

static const Table schema[] = {
    {"ports", 0, {S("port_id"), S("port_name"), S("country")}},
    {"products", 0, {S("product_id"), S("product_name"), E("category", "raw_material|finished_part")}},
    {"routes", 0, {S("route_id"), R("from_port_id", "ports"), R("to_port_id", "ports"),
        I("transit_days"), N("cost_inr_per_container"), I("capacity_containers"), E("status", "open|" PORT_STATUS)}},
    {"suppliers", 0, {S("supplier_id"), S("supplier_name"), R("port_id", "ports"), R("product_id", "products"),
        I("available_units"), I("lead_time_days"), N("unit_price_inr"), E("qualification_status", "qualified|unqualified|pending")}},
    {"contracts", 0, {S("contract_id"), S("customer_name"), R("product_id", "products"), I("quantity_units"),
        R("destination_port_id", "ports"), D("deadline"), N("late_penalty_inr_per_day")}},
    {"shipments", 0, {S("shipment_id"), R("product_id", "products"), I("quantity_units"), R("origin_port_id", "ports"),
        R("destination_port_id", "ports"), D("departure_date"), D("planned_arrival_date"), R("contract_id", "contracts"),
        E("status", "planned|in_transit|delivered|cancelled")}},
    {"shipment_legs", 0, {R("shipment_id", "shipments"), P("leg_sequence"), R("route_id", "routes"),
        D("planned_departure_date"), D("planned_arrival_date")}},
    {"disruptions", 0, {S("event_id"), D("reported_date"), R("port_id", "ports"), E("event_type", EVENTS),
        I("expected_duration_days"), {"severity_1_to_5", 'v', NULL}, E("port_status", PORT_STATUS), S("headline")}},
    {"production_dependencies", 1, {S("dependency_id"), R("input_port_id", "ports"), R("input_product_id", "products"),
        R("manufacturing_port_id", "ports"), R("output_product_id", "products"), I("production_days"),
        R("output_shipment_id", "shipments")}},
    {"historical_delays_synthetic", 1, {S("record_id"), R("port_id", "ports"), E("event_type", EVENTS "|none"),
        I("expected_duration_days"), C("severity_1_to_5"), C("congestion_0_to_5"), I("planned_transit_days"),
        I("schedule_buffer_days"), N("actual_delay_days"), E("data_origin", "synthetic")}}
};

static const char *id_column(const char *table) {
    for (size_t i = 0; i < sizeof schema / sizeof *schema; ++i)
        if (eq(table, schema[i].name)) return schema[i].columns[0].name;
    return "";
}

static int choices(const char *allowed, const char *value) {
    size_t length = strlen(value);
    for (const char *start = allowed; start; ) {
        const char *end = strchr(start, '|');
        size_t size = end ? (size_t)(end - start) : strlen(start);
        if (size == length && !strncmp(start, value, size)) return 1;
        start = end ? end + 1 : NULL;
    }
    return 0;
}

static char *strip(char *value) {
    while (isspace((unsigned char)*value)) ++value;
    size_t n = strlen(value);
    while (n && isspace((unsigned char)value[n-1])) value[--n] = 0;
    return value;
}

/* Decode one RFC 4180-style CSV record in place. Embedded newlines inside quotes are preserved. */
static int csv_row(char **input, char **cells) {
    char *read = *input, *write = read;
    if (!*read) return 0;
    int count = 0;
    for (;;) {
        if (count == MAX_COLUMNS) return -1;
        cells[count++] = write;
        if (*read == '"') {
            ++read;
            for (;;) {
                if (!*read) return -1;
                char ch = *read++;
                if (ch == '"') {
                    if (*read != '"') break;
                    ++read;
                }
                *write++ = ch;
            }
            if (*read && *read != ',' && *read != '\r' && *read != '\n') return -1;
        } else while (*read && *read != ',' && *read != '\r' && *read != '\n') *write++ = *read++;
        char separator = *read;
        if (separator) ++read;
        *write++ = 0;
        if (separator == ',') continue;
        if (separator == '\r' && *read == '\n') ++read;
        *input = read;
        return count;
    }
}

static int typed_field(Json *row, const Column *column, char *raw) {
    if (!*raw) return 0;
    if (column->type == 's' || column->type == 'r' || column->type == 'd') {
        if (column->type == 's' && column->constraint && !choices(column->constraint, raw)) return 0;
        if (column->type == 'd' && day(raw) < 0) return 0;
        cJSON_AddStringToObject(row, column->name, raw);
        return 1;
    }
    char *end;
    errno = 0;
    double value = column->type == 'n' ? strtod(raw, &end) : (double)strtol(raw, &end, 10);
    double low = column->type == 'p' || column->type == 'v' ? 1 : 0;
    double high = column->type == 'c' || column->type == 'v' ? 5 : column->type == 'n' ? HUGE_VAL : INT_MAX;
    if (end == raw || *end || errno == ERANGE || !isfinite(value) || value < low || value > high ||
        (column->type == 'n' && strpbrk(raw, "xXpP"))) return 0;
    cJSON_AddNumberToObject(row, column->name, value);
    return 1;
}

static void finding(Json *root, const Table *table, const char *id, const char *issue, Error *error) {
    char message[1024];
    snprintf(message, sizeof message, "%s.csv [%s]: %s", table->name, id, issue);
    if (table->optional) push(get(root, "warnings"), message);
    else fail(error, "%s", message);
}

static void read_table(Json *root, const char *directory, const Table *table, Error *error) {
    Json *rows = cJSON_AddArrayToObject(root, table->name);
    char path[4096];
    int length = snprintf(path, sizeof path, "%s/%s.csv", directory, table->name);
    size_t size;
    char *file = length > 0 && (size_t)length < sizeof path ? read_file(path, CSV_LIMIT, &size) : NULL;
    if (!file) { finding(root, table, "", "CSV missing or unreadable", error); return; }
    char *cursor = file;
    if (size >= 3 && !memcmp(file, "\xef\xbb\xbf", 3)) cursor += 3;
    char *header[MAX_COLUMNS], *values[MAX_COLUMNS];
    int header_count = csv_row(&cursor, header);
    int columns = 0, indices[12];
    for (int i = 0; i < header_count; ++i) for (int j = 0; j < i; ++j) if (eq(header[i], header[j])) {
        finding(root, table, "", "duplicate column names", error);
        goto cleanup;
    }
    for (; table->columns[columns].name; ++columns) {
        const char *field = table->columns[columns].name;
        indices[columns] = -1;
        for (int j = 0; j < header_count; ++j) if (eq(header[j], field)) indices[columns] = j;
        if (indices[columns] < 0) { finding(root, table, "", field, error); goto cleanup; }
    }
    int count;
    while (!error->message[0] && (count = csv_row(&cursor, values)) > 0) {
        if (count == 1 && !*values[0]) continue;
        char *id = indices[0] < count ? strip(values[indices[0]]) : "";
        if (count != header_count) { finding(root, table, id, "wrong number of values", error); continue; }
        Json *row = cJSON_CreateObject();
        int valid = 1;
        for (int i = 0; i < columns; ++i) {
            const Column *column = &table->columns[i];
            if (!typed_field(row, column, strip(values[indices[i]]))) {
                char issue[256];
                snprintf(issue, sizeof issue, "invalid %s", column->name);
                finding(root, table, id, issue, error);
                valid = 0;
            }
        }
        if (valid) {
            const char *key = table->columns[0].name;
            int duplicate = 0;
            each(old, rows) if (eq(str(old, key), id) &&
                (!eq(table->name, "shipment_legs") || num(old, "leg_sequence") == num(row, "leg_sequence"))) {
                duplicate = 1;
                break;
            }
            if (duplicate) finding(root, table, id, "duplicate ID", error);
            else { cJSON_AddItemToArray(rows, row); continue; }
        }
        cJSON_Delete(row);
    }
    if (!error->message[0] && count < 0) finding(root, table, "", "malformed CSV record", error);
cleanup:
    free(file);
}

static void validate_references(Json *root, const Table *table, Error *error) {
    Json *rows = get(root, table->name), *row = rows->child;
    while (row) {
        Json *next = row->next;
        int valid = 1;
        for (const Column *column = table->columns; column->name; ++column) {
            if (column->type != 'r') continue;
            const char *id = str(row, column->name), *target = column->constraint;
            if (!lookup(get(root, target), id_column(target), id)) {
                char issue[512];
                snprintf(issue, sizeof issue, "'%s' has no matching record in %s.csv", id, target);
                finding(root, table, str(row, table->columns[0].name), issue, error);
                valid = 0;
            }
        }
        if (!valid && table->optional) cJSON_Delete(cJSON_DetachItemViaPointer(rows, row));
        row = next;
    }
}

static int compare_leg(const void *a, const void *b) {
    double x = num(*(Json *const *)a, "leg_sequence"), y = num(*(Json *const *)b, "leg_sequence");
    return (x > y) - (x < y);
}

static void sort_legs(Json *legs, Error *error) {
    int n = cJSON_GetArraySize(legs), count = 0;
    Json **items = calloc(n + 1, sizeof *items);
    if (!items) { fail(error, "Could not allocate itinerary index"); return; }
    while (legs->child) items[count++] = cJSON_DetachItemFromArray(legs, 0);
    qsort(items, count, sizeof *items, compare_leg);
    for (int i = 0; i < count; ++i) cJSON_AddItemToArray(legs, items[i]);
    free(items);
}

static void validate_shipments(Json *root, Error *error) {
    Json *shipments = get(root, "shipments"), *legs = get(root, "shipment_legs");
    Json *contracts = get(root, "contracts"), *routes = get(root, "routes");
    each(shipment, shipments) {
        const char *id = str(shipment, "shipment_id");
        Json *contract = lookup(contracts, "contract_id", str(shipment, "contract_id"));
        if (!eq(str(shipment, "product_id"), str(contract, "product_id")) ||
            !eq(str(shipment, "destination_port_id"), str(contract, "destination_port_id")) ||
            num(shipment, "quantity_units") != num(contract, "quantity_units"))
            fail(error, "Shipment %s disagrees with its contract", id);
        int count = 0;
        const char *port = str(shipment, "origin_port_id");
        Day previous = day(str(shipment, "departure_date"));
        each(leg, legs) {
            if (!eq(id, str(leg, "shipment_id"))) continue;
            Json *route = lookup(routes, "route_id", str(leg, "route_id"));
            Day start = day(str(leg, "planned_departure_date")), end = day(str(leg, "planned_arrival_date"));
            ++count;
            if (num(leg, "leg_sequence") != count || !eq(str(route, "from_port_id"), port) ||
                start < previous || (count == 1 && start != previous) || end < start)
                fail(error, "Shipment %s has an invalid itinerary at leg %d", id, count);
            port = str(route, "to_port_id");
            previous = end;
        }
        if (!count || !eq(port, str(shipment, "destination_port_id")) ||
            previous != day(str(shipment, "planned_arrival_date")))
            fail(error, "Shipment %s has an incomplete or inconsistent itinerary", id);
    }
}

static void validate_production(Json *root) {
    Json *items = get(root, "production_dependencies"), *item = items->child;
    const char *dependency_field[] = {"output_product_id", "manufacturing_port_id"};
    const char *shipment_field[] = {"product_id", "origin_port_id"};
    while (item) {
        Json *next = item->next;
        const char *shipment_id = str(item, "output_shipment_id");
        Json *shipment = lookup(get(root, "shipments"), "shipment_id", shipment_id);
        int valid = 1;
        for (int i = 0; i < 2; ++i) {
            const char *expected = str(item, dependency_field[i]), *actual = str(shipment, shipment_field[i]);
            if (eq(expected, actual)) continue;
            char message[512];
            snprintf(message, sizeof message, "production_dependencies.csv [%s]: shipment %s %s %s, expected %s",
                     str(item, "dependency_id"), shipment_id, i == 0 ? "carries" : "starts at", actual, expected);
            push(get(root, "warnings"), message);
            valid = 0;
        }
        if (!valid) cJSON_Delete(cJSON_DetachItemViaPointer(items, item));
        item = next;
    }
}

Json *read_dataset(const char *directory, Error *error) {
    Json *root = cJSON_CreateObject();
    cJSON_AddArrayToObject(root, "warnings");
    for (size_t i = 0; i < sizeof schema / sizeof *schema && !error->message[0]; ++i)
        read_table(root, directory, &schema[i], error);
    for (size_t i = 0; i < sizeof schema / sizeof *schema && !error->message[0]; ++i)
        validate_references(root, &schema[i], error);
    if (!error->message[0]) {
        sort_legs(get(root, "shipment_legs"), error);
        validate_shipments(root, error);
        validate_production(root);
    }
    if (error->message[0]) { cJSON_Delete(root); return NULL; }
    cJSON_Delete(cJSON_DetachItemFromObjectCaseSensitive(root, "historical_delays_synthetic"));
    return root;
}

static void fixture_shipment(Network *network, Json *demo, Json *item, Error *error) {
    int index = edge_index(network, str(item, "route_id"));
    if (index < 0) { fail(error, "Manufacturing demo requires source route %s", str(item, "route_id")); return; }
    Json *route = network->edges[index];
    const char *origin = str(item, "origin_port_id"), *dest = str(item, "destination_port_id");
    if (!eq(origin, str(route, "from_port_id")) || !eq(dest, str(route, "to_port_id")))
        fail(error, "Manufacturing demo shipment has an inconsistent route: %s", str(item, "shipment_id"));

    Day start = day(str(item, "date"));
    Day end = advance(start, (int)num(route, "transit_days"), error);
    Json *contract = cJSON_CreateObject(), *leg = cJSON_CreateObject(), *shipment = cJSON_CreateObject();
    cJSON_AddItemToArray(get(demo, "shipments"), shipment);
    cJSON_AddItemToArray(get(demo, "contracts"), contract);
    cJSON_AddItemToArray(get(demo, "shipment_legs"), leg);
    const char *common[] = {"contract_id", "product_id", "quantity_units", "destination_port_id"};
    for (size_t i = 0; i < sizeof common / sizeof *common; ++i) {
        put(shipment, common[i], get(item, common[i]));
        put(contract, common[i], get(item, common[i]));
    }
    put(shipment, "shipment_id", get(item, "shipment_id"));
    put(shipment, "origin_port_id", get(item, "origin_port_id"));
    set_day(shipment, "departure_date", start);
    set_day(shipment, "planned_arrival_date", end);
    cJSON_AddStringToObject(shipment, "status", "planned");
    put(contract, "deadline", get(item, "deadline"));
    put(contract, "late_penalty_inr_per_day", get(item, "late_penalty_inr_per_day"));
    cJSON_AddStringToObject(contract, "customer_name", num(item, "late_penalty_inr_per_day") ? "Demo customer" : "Durban factory");
    put(leg, "shipment_id", get(item, "shipment_id"));
    put(leg, "route_id", get(item, "route_id"));
    cJSON_AddNumberToObject(leg, "leg_sequence", 1);
    set_day(leg, "planned_departure_date", start);
    set_day(leg, "planned_arrival_date", end);
}

int attach_manufacturing(Network *network, Error *error) {
    char *text = read_file("data/fixtures/manufacturing.json", CSV_LIMIT, NULL);
    Json *fixture = text ? cJSON_ParseWithOpts(text, NULL, 1) : NULL;
    free(text);
    if (!cJSON_IsObject(fixture)) {
        cJSON_Delete(fixture);
        fail(error, "Could not read the manufacturing fixture");
        return 0;
    }
    Json *demo = cJSON_AddObjectToObject(network->root, "demo");
    cJSON_AddArrayToObject(demo, "contracts");
    cJSON_AddArrayToObject(demo, "shipments");
    cJSON_AddArrayToObject(demo, "shipment_legs");
    put(demo, "suppliers", get(network->root, "suppliers"));
    Json *local = get(fixture, "local_supplier");
    if (!cJSON_IsObject(local) || lookup(get(demo, "suppliers"), "supplier_id", str(local, "supplier_id")))
        fail(error, "Invalid or conflicting manufacturing supplier");
    cJSON_AddItemToArray(get(demo, "suppliers"), clone(local));
    each(item, get(fixture, "shipments")) fixture_shipment(network, demo, item, error);
    put(demo, "packing", get(fixture, "packing_units_per_container"));
    put(demo, "purchase", get(fixture, "original_raw_purchase_inr"));
    put(demo, "tasks", get(fixture, "tasks"));
    Json *production = get(fixture, "production"), *shipments = get(demo, "shipments");
    const char *raw = str(production, "input_shipment_id"), *finished = str(production, "output_shipment_id");
    Json *final = lookup(shipments, "shipment_id", finished);
    each(task, get(demo, "tasks")) {
        const char *id = str(task, "shipment_id");
        if (*id) {
            Json *shipment = lookup(shipments, "shipment_id", id);
            Day start = day(str(shipment, "departure_date")), end = day(str(shipment, "planned_arrival_date"));
            if (!shipment || !eq(str(task, "planned_start"), str(shipment, "departure_date")) ||
                num(task, "duration_days") != end - start)
                fail(error, "Manufacturing task schedule disagrees with shipment %s", id);
        }
        if (eq(id, raw)) put(demo, "raw", get(task, "task_id"));
        if (eq(id, finished)) put(demo, "finished", get(task, "task_id"));
        if (eq(str(task, "contract_id"), str(final, "contract_id"))) put(demo, "delivery", get(task, "task_id"));
    }
    if (!get(demo, "raw") || !get(demo, "finished") || !get(demo, "delivery") ||
        !cJSON_IsNumber(get(demo, "packing")) || num(demo, "packing") < 1)
        fail(error, "Manufacturing fixture is missing required tasks or packing");
    cJSON_Delete(fixture);
    return !error->message[0];
}
