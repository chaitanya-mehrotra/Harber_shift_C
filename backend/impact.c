#include "scenario.h"
#include <math.h>
#include <stdlib.h>

Json *table(const Scenario *scenario, const char *name) { return get(scenario->tables, name); }

Json *legs_for(const Scenario *scenario, const char *id) {
    Json *legs = cJSON_CreateArray();
    each(leg, table(scenario, "shipment_legs"))
        if (eq(id, str(leg, "shipment_id"))) cJSON_AddItemToArray(legs, clone(leg));
    return legs;
}

static int active(const Event *event, const char *port, Day on) {
    return eq(event->port, port) && event->start <= on && on < event->end;
}

static int restriction(const Scenario *scenario, const Event *event, Day on, int sailing, Json *estimates) {
    if (scenario->use_ml) {
        Json *features = cJSON_CreateObject();
        cJSON_AddStringToObject(features, "port_id", event->port);
        cJSON_AddStringToObject(features, "event_type", event->type);
        cJSON_AddNumberToObject(features, "expected_duration_days", event->duration);
        cJSON_AddNumberToObject(features, "severity_1_to_5", event->severity);
        cJSON_AddNumberToObject(features, "congestion_0_to_5", scenario->congestion);
        cJSON_AddNumberToObject(features, "planned_transit_days", sailing);
        cJSON_AddNumberToObject(features, "schedule_buffer_days", scenario->buffer);
        Json *prediction = predict(features);
        double raw = num(prediction, "days");
        if (eq(str(prediction, "source"), "synthetic_model") && raw >= 0 && raw <= LAST_DAY) {
            int rounded = (int)ceil(raw);
            if (estimates) {
                Json *estimate = cJSON_CreateObject();
                cJSON_AddItemToArray(estimates, estimate);
                cJSON_AddStringToObject(estimate, "event_id", event->id);
                set_day(estimate, "evaluated_on", on);
                cJSON_AddItemToObject(estimate, "inputs", features);
                cJSON_AddNumberToObject(estimate, "predicted_days", raw);
                cJSON_AddNumberToObject(estimate, "rounded_days", rounded);
                put(estimate, "version", get(prediction, "version"));
            } else cJSON_Delete(features);
            cJSON_Delete(prediction);
            return rounded;
        }
        cJSON_Delete(prediction);
        cJSON_Delete(features);
    }
    int regular = (event->severity + 1) / 2;
    return event->end - on < regular ? event->end - on : regular;
}

int wait_at(const Scenario *scenario, const char *port, Day on, int sailing, int completed,
            Json *ids, Json *estimates) {
    unsigned applied = 0;
    Day ready = on;
    if (completed) {
        for (int i = 0; i < scenario->event_count; ++i)
            if (active(&scenario->events[i], port, on)) push(ids, scenario->events[i].id);
        return 0;
    }
    while (!scenario->error->message[0]) {
        Day reopen = ready;
        unsigned closures = 0;
        for (int i = 0; i < scenario->event_count; ++i) {
            const Event *event = &scenario->events[i];
            if (active(event, port, ready) && eq(event->status, "closed")) {
                if (event->end > reopen) reopen = event->end;
                closures |= 1u << i;
                if (ids && !(applied & (1u << i))) push(ids, event->id);
            }
        }
        if (closures) {
            applied |= closures;
            ready = reopen;
            continue;
        }
        unsigned restrictions = 0;
        int maximum = 0;
        for (int i = 0; i < scenario->event_count; ++i) {
            const Event *event = &scenario->events[i];
            if (!(applied & (1u << i)) && active(event, port, ready)) {
                int wait = restriction(scenario, event, ready, sailing, estimates);
                if (wait > maximum) maximum = wait;
                restrictions |= 1u << i;
                if (ids) push(ids, event->id);
            }
        }
        if (!restrictions) break;
        applied |= restrictions;
        ready = advance(ready, maximum, scenario->error);
    }
    return ready - on;
}

static int call(const Scenario *scenario, Itinerary *itinerary, Json *leg, const char *port,
                const char *type, Day on, int sailing, int completed) {
    Json *ids = cJSON_CreateArray(), *estimates = cJSON_CreateArray();
    int wait = wait_at(scenario, port, on, sailing, completed, ids, estimates);
    if (!ids->child) {
        cJSON_Delete(ids); cJSON_Delete(estimates);
        return 0;
    }
    Json *record = cJSON_CreateObject();
    cJSON_AddItemToArray(itinerary->calls, record);
    cJSON_AddStringToObject(record, "port_id", port);
    put(record, "route_id", get(leg, "route_id"));
    put(record, "leg_sequence", get(leg, "leg_sequence"));
    cJSON_AddStringToObject(record, "call_type", type);
    set_day(record, "call_date", on);
    cJSON_AddStringToObject(record, "status", completed ? "already completed" :
                            on <= scenario->as_of ? "modeled impact" : "possible future exposure");
    cJSON_AddItemToObject(record, "event_ids", ids);
    cJSON_AddNumberToObject(record, "wait_days", wait);
    cJSON_AddItemToObject(record, "model_estimates", estimates);
    if (wait) itinerary->waited = 1;
    return wait;
}

Itinerary replay(const Scenario *scenario, Json *shipment, Json *legs, Day shifted_start) {
    Itinerary itinerary = {cJSON_CreateArray(), cJSON_CreateArray(), day(str(shipment, "planned_arrival_date")), 0};
    Day previous = NO_DAY;
    int completed = eq(str(shipment, "status"), "delivered");
    each(leg, legs) {
        Day depart = day(str(leg, "planned_departure_date"));
        int sailing = day(str(leg, "planned_arrival_date")) - depart;
        if (shifted_start >= 0 && previous == NO_DAY) depart = shifted_start;
        if (previous > depart) depart = previous;
        int edge = edge_index(scenario->net, str(leg, "route_id"));
        if (edge < 0) { fail(scenario->error, "Invalid itinerary route: %s", str(leg, "route_id")); break; }
        Json *route = scenario->net->edges[edge];
        const char *from = str(route, "from_port_id"), *to = str(route, "to_port_id");
        if (depart != previous)
            depart = advance(depart, call(scenario, &itinerary, leg, from, "departure", depart, sailing, completed), scenario->error);
        Day arrive = advance(depart, sailing, scenario->error);
        if (!eq(from, to) || sailing)
            arrive = advance(arrive, call(scenario, &itinerary, leg, to, "arrival", arrive, sailing, completed), scenario->error);
        Json *scheduled = cJSON_CreateObject();
        put(scheduled, "route_id", get(leg, "route_id"));
        set_day(scheduled, "planned_departure_date", depart);
        set_day(scheduled, "planned_arrival_date", arrive);
        cJSON_AddItemToArray(itinerary.scheduled, scheduled);
        itinerary.eta = previous = arrive;
    }
    return itinerary;
}

void free_itinerary(Itinerary itinerary) {
    cJSON_Delete(itinerary.scheduled);
    cJSON_Delete(itinerary.calls);
}
