#include "scenario.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <rpc.h>

static void add_event(Scenario *s, Json *item, int sample) {
    Event event = {0};
    event.id = sample ? str(item, "event_id") : "manual";
    event.port = str(item, "port_id");
    event.type = str(item, "event_type");
    event.status = str(item, "port_status");
    event.headline = sample ? str(item, "headline") : "";
    const char *types[] = {"strike", "congestion", "equipment_failure", "severe_weather"};
    int valid_type = 0;
    for (int i = 0; i < 4; ++i) valid_type |= eq(event.type, types[i]);
    if (!valid_type) fail(s->error, "Unknown event type: %s", event.type);
    if (!eq(event.status, "closed") && !eq(event.status, "restricted"))
        fail(s->error, "Port status must be closed or restricted");
    if (port_index(s->net, event.port) < 0) fail(s->error, "Unknown port ID: %s", event.port);
    if (sample) event.source = "synthetic CSV";
    else event.source = read_bool(item, "advisory_replay", s->error) ?
                       "advisory replay with user assumptions" : "manual simulation";
    event.duration = integer(item, sample ? "expected_duration_days" : "duration_days", 0, LAST_DAY, -1, s->error);
    event.severity = integer(item, sample ? "severity_1_to_5" : "severity", 1, 5, 0, s->error);
    Json *start = get(item, sample ? "reported_date" : "start");
    event.start = !start || cJSON_IsNull(start) ? s->as_of : day(string(start));
    if (event.start < 0) fail(s->error, "Event start must be a valid YYYY-MM-DD date");
    event.end = advance(event.start, event.duration, s->error);
    for (int i = 0; i < s->event_count; ++i) if (eq(event.id, s->events[i].id)) return;
    if (!s->error->message[0]) s->events[s->event_count++] = event;
}

static Json *event_record(Event event) {
    Json *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "event_id", event.id);
    cJSON_AddStringToObject(obj, "port_id", event.port);
    cJSON_AddStringToObject(obj, "event_type", event.type);
    set_day(obj, "start", event.start);
    cJSON_AddNumberToObject(obj, "duration_days", event.duration);
    cJSON_AddNumberToObject(obj, "severity", event.severity);
    cJSON_AddStringToObject(obj, "port_status", event.status);
    cJSON_AddStringToObject(obj, "source", event.source);
    cJSON_AddStringToObject(obj, "confidence", "assumed");
    cJSON_AddStringToObject(obj, "headline", event.headline);
    return obj;
}

static Json *score(Json *shipment, Json *contract, Itinerary itinerary) {
    Json *impact = cJSON_CreateObject();
    int slack = day(str(contract, "deadline")) - itinerary.eta;
    put(impact, "shipment_id", get(shipment, "shipment_id"));
    put(impact, "contract_id", get(contract, "contract_id"));
    put(impact, "baseline_eta", get(shipment, "planned_arrival_date"));
    set_day(impact, "revised_eta", itinerary.eta);
    put(impact, "deadline", get(contract, "deadline"));
    cJSON_AddNumberToObject(impact, "slack_days", slack);
    cJSON_AddNumberToObject(impact, "late_days", slack < 0 ? -slack : 0);
    put(impact, "penalty_per_day", get(contract, "late_penalty_inr_per_day"));
    put(impact, "calls", itinerary.calls);
    return impact;
}

static int compare_string(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int compare_shipment(const void *a, const void *b) {
    return strcmp(str(*(Json *const *)a, "shipment_id"), str(*(Json *const *)b, "shipment_id"));
}

static int compare_urgent(const void *ap, const void *bp) {
    Json *a = *(Json *const *)ap, *b = *(Json *const *)bp;
    double sa = num(a, "slack_days"), sb = num(b, "slack_days");
    if (sa != sb) return sa < sb ? -1 : 1;
    double pa = num(a, "penalty_per_day"), pb = num(b, "penalty_per_day");
    if (pa != pb) return pa > pb ? -1 : 1;
    int contract = strcmp(str(a, "contract_id"), str(b, "contract_id"));
    return contract ? contract : compare_shipment(ap, bp);
}

static int live(Itinerary itinerary) {
    each(call, itinerary.calls) if (!eq(str(call, "status"), "already completed")) return 1;
    return 0;
}

static double standard(Scenario *s, const char *selected, double purchase, Json *out) {
    Json *shipments = table(s, "shipments"), *contracts = table(s, "contracts");
    Json *impacts = get(out, "impacts"), *recovery = get(out, "recovery");
    Json *affected = get(out, "affected_shipment_ids"), *urgent = get(out, "urgent_contract_ids");
    int count = cJSON_GetArraySize(shipments), ordered_count = 0, urgent_count = 0;
    Json **ordered = malloc((count + 1) * sizeof *ordered), **ranked = malloc((count + 1) * sizeof *ranked);
    double penalties = 0;
    if (!ordered || !ranked) {
        fail(s->error, "Could not allocate shipment ranking");
        goto cleanup;
    }
    each(shipment, shipments) ordered[ordered_count++] = shipment;
    qsort(ordered, ordered_count, sizeof *ordered, compare_shipment);
    for (int i = 0; i < ordered_count && !s->error->message[0]; ++i) {
        Json *shipment = ordered[i];
        const char *id = str(shipment, "shipment_id");
        if ((selected && !eq(selected, id)) || eq(str(shipment, "status"), "cancelled")) continue;
        Json *legs = legs_for(s, id);
        Itinerary itin = replay(s, shipment, legs, NO_DAY);
        cJSON_Delete(legs);
        if (itin.calls->child) {
            Json *contract = lookup(contracts, "contract_id", str(shipment, "contract_id"));
            Json *impact = score(shipment, contract, itin);
            cJSON_AddItemToArray(impacts, impact);
            if (num(impact, "slack_days") <= 0 && itin.waited) ranked[urgent_count++] = impact;
            if (live(itin)) {
                push(affected, id);
                cJSON_AddItemToArray(recovery, recover(s, shipment, itin, purchase, NULL, -1, contract, 1));
            }
        }
        free_itinerary(itin);
    }
    qsort(ranked, urgent_count, sizeof *ranked, compare_urgent);
    for (int i = 0; i < urgent_count; ++i) {
        const char *id = str(ranked[i], "contract_id");
        if (has(urgent, id)) continue;
        push(urgent, id);
        penalties += num(ranked[i], "late_days") * num(ranked[i], "penalty_per_day");
    }
cleanup:
    free(ordered); free(ranked);
    cJSON_AddArrayToObject(out, "cascade");
    set_day(out, "customer_deadline", NO_DAY);
    return penalties;
}

static double manufacturing(Scenario *s, double purchase, Json *out) {
    Dag dag = {0};
    TaskTiming *baseline = NULL, *revised = NULL;
    double penalties = 0;
    if (!dag_init(&dag, s->demo, s->error)) goto cleanup;
    baseline = schedule(s, &dag, 1, -1, NO_DAY);
    revised = schedule(s, &dag, 0, -1, NO_DAY);
    if (s->error->message[0]) goto cleanup;
    cJSON_AddItemToObject(out, "cascade", cascade_report(&dag, baseline, revised));
    Json *shipments = table(s, "shipments"), *contracts = table(s, "contracts");
    Json *contract = lookup(contracts, "contract_id", str(dag.tasks[dag.delivery], "contract_id"));
    Day deadline = day(str(contract, "deadline"));
    set_day(out, "customer_deadline", deadline);
    Day delivery = revised[dag.delivery].finish;
    if (delivery > baseline[dag.delivery].finish && delivery >= deadline) {
        push(get(out, "urgent_contract_ids"), str(contract, "contract_id"));
        penalties = (delivery - deadline) * num(contract, "late_penalty_inr_per_day");
    }

    Json *ordered[128];
    int count = 0;
    for (int i = 0; i < dag.count; ++i) {
        const char *id = str(dag.tasks[i], "shipment_id");
        if (*id) ordered[count++] = lookup(shipments, "shipment_id", id);
    }
    qsort(ordered, count, sizeof *ordered, compare_shipment);
    for (int i = 0; i < count; ++i) {
        Json *shipment = ordered[i];
        const char *id = str(shipment, "shipment_id");
        int task = 0;
        while (task < dag.count && !eq(id, str(dag.tasks[task], "shipment_id"))) ++task;
        if (task == dag.count) continue;
        Itinerary itin = revised[task].itinerary;
        int calls = itin.calls && itin.calls->child;
        if (calls || revised[task].finish > baseline[task].finish) push(get(out, "affected_shipment_ids"), id);
        if (calls) {
            Json *own = lookup(contracts, "contract_id", str(shipment, "contract_id"));
            cJSON_AddItemToArray(get(out, "impacts"), score(shipment, own, itin));
        }
    }
    int targets[] = {dag.raw, dag.finished};
    for (int i = 0; i < 2; ++i) {
        int task = targets[i];
        Itinerary itin = revised[task].itinerary;
        if (!itin.calls || !itin.calls->child || itin.eta < s->as_of) continue;
        Json *shipment = lookup(shipments, "shipment_id", str(dag.tasks[task], "shipment_id"));
        if (i == 0 && isnan(purchase)) purchase = num(s->demo, "purchase");
        Json *result = recover(s, shipment, itin, purchase, &dag, task, contract, i == 0);
        cJSON_AddItemToArray(get(out, "recovery"), result);
        break;
    }
cleanup:
    free_schedule(&dag, baseline);
    free_schedule(&dag, revised);
    dag_free(&dag);
    return penalties;
}

static void affected_network(Scenario *s, Json *out, Json *events) {
    const char *ids[MAX_EVENTS];
    int count = 0, unique = 0;
    for (int i = 0; i < s->event_count; ++i) {
        Event event = s->events[i];
        cJSON_AddItemToArray(events, event_record(event));
        if (event.duration && event.end > s->as_of) ids[count++] = event.port;
    }
    qsort(ids, count, sizeof *ids, compare_string);
    Json *ports = cJSON_AddArrayToObject(out, "affected_port_ids");
    Json *edges = cJSON_AddArrayToObject(out, "affected_edge_ids");
    for (int i = 0; i < count; ++i)
        if (!i || !eq(ids[i], ids[i - 1])) { push(ports, ids[i]); ++unique; }
    if (unique) {
        each(edge, get(s->net->root, "routes"))
            if (has(ports, str(edge, "from_port_id")) || has(ports, str(edge, "to_port_id")))
                push(edges, str(edge, "route_id"));
    }
}

static void prediction(Scenario *s, Json *out) {
    Json *record = cJSON_AddObjectToObject(out, "prediction");
    Json *sources = cJSON_AddArrayToObject(record, "sources");
    Json *versions = cJSON_AddArrayToObject(record, "versions");
    const char *version = NULL;
    int rule = 0;
    each(impact, get(out, "impacts")) {
        each(call, get(impact, "calls")) {
            each(estimate, get(call, "model_estimates")) version = str(estimate, "version");
        }
    }
    for (int i = 0; i < s->event_count; ++i) rule |= eq(s->events[i].status, "closed");
    if (rule || !version) { push(sources, "rule_fallback"); push(versions, "rule-v1"); }
    if (version) { push(sources, "synthetic_model"); push(versions, version); }
    cJSON_AddStringToObject(record, "requested", s->use_ml ? "synthetic_model" : "rule_fallback");
    cJSON_AddNumberToObject(record, "congestion", s->congestion);
    cJSON_AddNumberToObject(record, "buffer_days", s->buffer);
}

Json *simulate(const Network *net, Json *query, Error *error) {
    Scenario s = {0};
    s.net = net;
    s.error = error;
    int chain = read_bool(query, "manufacturing", error);
    s.demo = chain ? get(net->root, "demo") : NULL;
    if (chain && !s.demo) fail(error, "%s", str(net->root, "demo_error"));
    s.tables = s.demo ? s.demo : net->root;
    s.use_ml = read_bool(query, "use_ml", error);
    s.as_of = day(str(query, "as_of"));
    if (s.as_of < 0) fail(error, "as_of must be a valid YYYY-MM-DD date");
    s.congestion = integer(query, "congestion", 0, 5, 0, error);
    s.buffer = integer(query, "buffer_days", 0, INT_MAX, 0, error);
    Json *packing = get(query, "units_per_container");
    s.packing = !packing || cJSON_IsNull(packing) ? (s.demo ? (int)num(s.demo, "packing") : 10) :
                integer(query, "units_per_container", 1, INT_MAX, 0, error);
    Json *selected_value = get(query, "shipment_id");
    const char *selected = NULL;
    if (selected_value && !cJSON_IsNull(selected_value)) {
        selected = string(selected_value);
        if (!lookup(table(&s, "shipments"), "shipment_id", selected)) fail(error, "Unknown shipment ID: %s", selected);
        if (s.demo) fail(error, "Manufacturing scenarios evaluate the complete chain");
    }
    Json *price = get(query, "original_purchase_inr");
    double purchase = NAN;
    if (price && !cJSON_IsNull(price)) {
        if (cJSON_IsNumber(price)) purchase = price->valuedouble;
        else if (cJSON_IsString(price)) {
            char *end;
            purchase = strtod(price->valuestring, &end);
            if (end == price->valuestring || *end) purchase = NAN;
        }
        if (!isfinite(purchase) || purchase < 0) fail(error, "Original purchase cost must be finite and nonnegative");
    }
    if (list(query, "sample_ids", 10, error)) {
        each(id, get(query, "sample_ids")) {
            Json *sample = lookup(get(net->root, "disruptions"), "event_id", string(id));
            if (!sample) fail(error, "Unknown sample event ID: %s", string(id));
            else add_event(&s, sample, 1);
        }
    }
    Json *manual = get(query, "event");
    if (manual && !cJSON_IsNull(manual)) {
        if (!cJSON_IsObject(manual)) fail(error, "event must be an object");
        else add_event(&s, manual, 0);
    }
    if (error->message[0]) return NULL;

    Json *out = cJSON_CreateObject();
    UUID uuid;
    unsigned char *guid = NULL;
    UuidCreate(&uuid);
    if (UuidToStringA(&uuid, &guid) == RPC_S_OK) {
        cJSON_AddStringToObject(out, "scenario_id", (char *)guid);
        RpcStringFreeA(&guid);
    }
    set_day(out, "as_of", s.as_of);
    cJSON_AddBoolToObject(out, "manufacturing", !!s.demo);
    cJSON_AddNumberToObject(out, "total_shipments", cJSON_GetArraySize(table(&s, "shipments")));
    affected_network(&s, out, cJSON_AddArrayToObject(out, "events"));
    cJSON_AddArrayToObject(out, "affected_shipment_ids");
    cJSON_AddArrayToObject(out, "urgent_contract_ids");
    cJSON_AddArrayToObject(out, "impacts");
    cJSON_AddArrayToObject(out, "recovery");
    double penalties = s.demo ? manufacturing(&s, purchase, out) : standard(&s, selected, purchase, out);
    cJSON_AddNumberToObject(out, "estimated_penalties_inr", penalties);
    prediction(&s, out);
    Json *warnings = clone(get(net->root, "warnings"));
    cJSON_AddItemToObject(out, "warnings", warnings);
    push(warnings, "Synthetic schedules, costs, and event assumptions. Closures use the reopening rule.");
    if (s.use_ml) push(warnings, "Only restricted calls can use the synthetic model. Congestion and buffer are scenario assumptions.");
    if (error->message[0]) { cJSON_Delete(out); return NULL; }
    return out;
}
