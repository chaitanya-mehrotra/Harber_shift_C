#include "scenario.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const Scenario *scenario;
    const Dag *dag;
    int task, containers;
    Day deadline;
    double penalty_rate;
    Json *options;
} Recovery;

static Json *option(Recovery *r, const char *id, const char *kind, const char *status, const char *reason) {
    Json *opt = cJSON_CreateObject();
    cJSON_AddItemToArray(r->options, opt);
    cJSON_AddStringToObject(opt, "option_id", id);
    cJSON_AddStringToObject(opt, "kind", kind);
    cJSON_AddStringToObject(opt, "status", status);
    cJSON_AddStringToObject(opt, "reason", reason);
    return opt;
}

static void reject(Recovery *r, const char *id, const char *kind, const char *reason) {
    Json *opt = option(r, id, kind, "rejected", reason);
    const char *fields[] = {"eta","handoff_eta","freight_inr","purchase_inr","penalty_inr",
                            "total_future_inr","late_days","lead_days","required_containers"};
    for (size_t i = 0; i < sizeof fields / sizeof *fields; ++i) cJSON_AddNullToObject(opt, fields[i]);
    cJSON_AddArrayToObject(opt, "route_ids");
}

static void candidate(Recovery *r, const char *id, const char *kind, const char *status, const char *reason,
                      Day handoff, Json *routes, double freight, double purchase, int lead) {
    Day eta = handoff;
    if (r->dag) {
        TaskTiming *revised = schedule(r->scenario, r->dag, 0, r->task, handoff);
        if (!revised) return;
        eta = revised[r->dag->delivery].finish;
        free_schedule(r->dag, revised);
    }
    int late = eta > r->deadline ? eta - r->deadline : 0;
    double penalty = late * r->penalty_rate;
    Json *opt = option(r, id, kind, status, reason);
    set_day(opt, "eta", eta);
    set_day(opt, "handoff_eta", handoff);
    if (routes) put(opt, "route_ids", routes);
    else cJSON_AddArrayToObject(opt, "route_ids");
    cJSON_AddNumberToObject(opt, "freight_inr", freight);
    cJSON_AddNumberToObject(opt, "penalty_inr", penalty);
    if (isnan(purchase)) {
        cJSON_AddNullToObject(opt, "purchase_inr");
        cJSON_AddNullToObject(opt, "total_future_inr");
    } else {
        cJSON_AddNumberToObject(opt, "purchase_inr", purchase);
        cJSON_AddNumberToObject(opt, "total_future_inr", freight + purchase + penalty);
    }
    cJSON_AddNumberToObject(opt, "late_days", late);
    cJSON_AddNumberToObject(opt, "required_containers", r->containers);
    if (lead >= 0) cJSON_AddNumberToObject(opt, "lead_days", lead);
    else cJSON_AddNullToObject(opt, "lead_days");
}

static Json *path_for(Recovery *r, const char *from, const char *to, int cheapest, Json *blocked) {
    return shortest(r->scenario->net, from, to, r->containers, cheapest, NULL, blocked, NULL, NULL, r->scenario->error);
}

static int path_found(Json *path) { return cJSON_IsNumber(get(path, "total_days")); }

static void via_path(Recovery *r, const char *id, const char *kind, const char *status, const char *reason,
                     Day start, Json *path, double purchase, int lead) {
    Day handoff = advance(start, (int)num(path, "total_days"), r->scenario->error);
    candidate(r, id, kind, status, reason, handoff, get(path, "route_ids"), num(path, "freight_inr"), purchase, lead);
}

static void reroutes(Recovery *r, Json *shipment, Itinerary itinerary, Json *baseline, Json *blocked, double purchase) {
    const Scenario *s = r->scenario;
    Day first = day(str(cJSON_GetArrayItem(itinerary.scheduled, 0), "planned_departure_date"));
    if (first < 0) first = day(str(shipment, "departure_date"));
    int transit = first < s->as_of || eq(str(shipment, "status"), "in_transit");
    const char *dest = str(shipment, "destination_port_id"), *decision = str(shipment, "origin_port_id");
    Day decision_date = first > s->as_of ? first : s->as_of;
    if (transit) {
        Json *next_leg = NULL;
        each(leg, itinerary.scheduled) {
            if (day(str(leg, "planned_arrival_date")) >= s->as_of) { next_leg = leg; break; }
        }
        int edge = edge_index(s->net, str(next_leg, "route_id"));
        if (edge >= 0) {
            decision = str(s->net->edges[edge], "to_port_id");
            decision_date = day(str(next_leg, "planned_arrival_date"));
        }
        if (edge < 0 || eq(decision, dest)) {
            reject(r, "reroute", "reroute", "No intermediate decision point remains in the modeled itinerary.");
            return;
        }
    }
    const char *ids[] = {"reroute-fastest", "reroute-cheapest"};
    const char *names[] = {"Fastest", "Cheapest"};
    Json *previous = NULL;
    int distinct = 0;
    for (int i = 0; i < 2; ++i) {
        Json *path = path_for(r, decision, dest, i, blocked);
        if (!path_found(path)) {
            reject(r, ids[i], "reroute", str(path, "no_route_reason"));
            cJSON_Delete(path);
            distinct = 1;
            break;
        }
        Json *routes = get(path, "route_ids");
        if ((!transit && cJSON_Compare(routes, baseline, 1)) ||
            (previous && cJSON_Compare(routes, previous, 1))) {
            cJSON_Delete(path);
            continue;
        }
        char reason[512];
        if (transit)
            snprintf(reason, sizeof reason, "Conditional on reaching %s as scheduled and confirming vessel position.", decision);
        else snprintf(reason, sizeof reason, "%s distinct path from %s.", names[i], decision);
        via_path(r, ids[i], "reroute", transit ? "conditional" : "feasible", reason, decision_date, path, purchase, -1);
        cJSON_Delete(previous);
        previous = clone(routes);
        distinct = 1;
        cJSON_Delete(path);
    }
    cJSON_Delete(previous);
    if (!transit && !distinct) reject(r, "reroute", "reroute", "The fastest and cheapest feasible paths match the baseline.");
}

static void supplier_option(Recovery *r, Json *shipment, Json *supplier, Json *blocked) {
    const char *id = str(supplier, "supplier_id"), *name = str(supplier, "supplier_name");
    const char *destination = str(shipment, "destination_port_id"), *origin = str(supplier, "port_id");
    double quantity = num(shipment, "quantity_units");
    const char *issues[3];
    int n = 0;
    if (!eq(str(supplier, "product_id"), str(shipment, "product_id"))) issues[n++] = "wrong product";
    if (!eq(str(supplier, "qualification_status"), "qualified")) issues[n++] = "supplier is unqualified";
    if (num(supplier, "available_units") < quantity) issues[n++] = "insufficient stock";
    char reason[768];
    if (n) {
        snprintf(reason, sizeof reason, "%s: %s%s%s%s%s", name, issues[0],
                 n > 1 ? ", " : "", n > 1 ? issues[1] : "", n > 2 ? ", " : "", n > 2 ? issues[2] : "");
        reject(r, id, "supplier", reason);
        return;
    }
    int lead = (int)num(supplier, "lead_time_days");
    double purchase = num(supplier, "unit_price_inr") * quantity;
    Day ready = advance(r->scenario->as_of, lead, r->scenario->error);
    if (eq(origin, destination)) {
        int wait = wait_at(r->scenario, origin, ready, 0, 0, NULL, NULL);
        snprintf(reason, sizeof reason, "%s: local, qualified and full quantity.", name);
        candidate(r, id, "supplier", "feasible", reason, advance(ready, wait, r->scenario->error), NULL, 0, purchase, lead);
        return;
    }
    Json *cheap = path_for(r, origin, destination, 1, blocked);
    if (!path_found(cheap)) {
        snprintf(reason, sizeof reason, "%s: %s", name, str(cheap, "no_route_reason"));
        reject(r, id, "supplier", reason);
    } else {
        snprintf(reason, sizeof reason, "%s: qualified for the full quantity.", name);
        via_path(r, id, "supplier", "feasible", reason, ready, cheap, purchase, lead);
        Json *fast = path_for(r, origin, destination, 0, blocked);
        if (path_found(fast) && !cJSON_Compare(get(fast, "route_ids"), get(cheap, "route_ids"), 1)) {
            char variant[256];
            snprintf(variant, sizeof variant, "%s-fastest", id);
            via_path(r, variant, "supplier", "feasible", reason, ready, fast, purchase, lead);
        }
        cJSON_Delete(fast);
    }
    cJSON_Delete(cheap);
}

static double compare_cost(Json *opt) {
    if (cJSON_IsNull(get(opt, "total_future_inr"))) return num(opt, "freight_inr") + num(opt, "penalty_inr");
    return num(opt, "total_future_inr");
}

static int before(Json *a, Json *b, int mixed) {
    int late_a = num(a, "late_days") > 0, late_b = num(b, "late_days") > 0;
    if (late_a != late_b) return late_a < late_b;
    double penalty_a = num(a, "penalty_inr"), penalty_b = num(b, "penalty_inr");
    if (penalty_a != penalty_b) return penalty_a < penalty_b;
    if (!mixed && compare_cost(a) != compare_cost(b)) return compare_cost(a) < compare_cost(b);
    int eta = strcmp(str(a, "eta"), str(b, "eta"));
    if (eta) return eta < 0;
    int wait_a = eq(str(a, "kind"), "wait"), wait_b = eq(str(b, "kind"), "wait");
    if (wait_a != wait_b) return wait_a > wait_b;
    return strcmp(str(a, "option_id"), str(b, "option_id")) < 0;
}

static void rank_options(Recovery *r, Json *result, Json *warnings) {
    int max = cJSON_GetArraySize(r->options), count = 0, known = 0, deadline_feasible = 0;
    Json **ranked = malloc((max + 1) * sizeof *ranked);
    if (!ranked) { fail(r->scenario->error, "Could not allocate recovery ranking"); return; }
    each(opt, r->options) {
        if (!eq(str(opt, "status"), "feasible")) continue;
        ranked[count++] = opt;
        known += !cJSON_IsNull(get(opt, "total_future_inr"));
        deadline_feasible |= num(opt, "late_days") == 0;
    }
    int mixed = known && known < count;
    if (mixed) push(warnings, "Purchase costs are incomplete. ETA replaces cost for cross-option ranking.");
    else if (count && !known) push(warnings, "Purchase cost is unknown. Common purchase cost is excluded from transport comparison.");

    /* Stable insertion sort keeps the small set of feasible options in ranking order. */
    for (int i = 1; i < count; ++i) {
        Json *value = ranked[i];
        int j = i;
        while (j > 0 && before(value, ranked[j - 1], mixed)) { ranked[j] = ranked[j - 1]; --j; }
        ranked[j] = value;
    }
    Json *out = cJSON_AddArrayToObject(result, "options");
    for (int i = 0; i < count; ++i) cJSON_AddItemToArray(out, clone(ranked[i]));
    each(opt, r->options) if (!eq(str(opt, "status"), "feasible")) cJSON_AddItemToArray(out, clone(opt));
    put(result, "recommended_id", count ? get(ranked[0], "option_id") : NULL);
    cJSON_AddBoolToObject(result, "deadline_feasible", deadline_feasible);
    free(ranked);
}

Json *recover(const Scenario *s, Json *shipment, Itinerary itinerary, double purchase,
              const Dag *dag, int task, Json *contract, int supplier_allowed) {
    int quantity = (int)num(shipment, "quantity_units");
    Recovery r = {s, dag, task, 1 + (quantity - 1) / s->packing, day(str(contract, "deadline")),
                  num(contract, "late_penalty_inr_per_day"), cJSON_CreateArray()};
    Json *result = cJSON_CreateObject(), *warnings = cJSON_AddArrayToObject(result, "warnings");
    put(result, "shipment_id", get(shipment, "shipment_id"));
    put(result, "contract_id", get(contract, "contract_id"));
    const char *status = str(shipment, "status");
    if (eq(status, "cancelled") || eq(status, "delivered") || itinerary.eta < s->as_of) {
        push(warnings, "Shipment has no remaining modeled transport.");
    } else {
        push(warnings, "Totals are estimated future costs. Existing commitments and cancellation fees are unknown.");
        push(warnings, "New routes conservatively avoid closed or restricted ports during the comparison horizon.");
        if (dag) push(warnings, "All options retain production and finished delivery. Common downstream freight is excluded.");
        Json *baseline = cJSON_CreateArray(), *blocked = cJSON_CreateArray();
        int cannot_wait = 0;
        double cost = 0;
        each(leg, itinerary.scheduled) {
            const char *id = str(leg, "route_id");
            push(baseline, id);
            if (day(str(leg, "planned_departure_date")) < s->as_of) continue;
            int edge = edge_index(s->net, id);
            if (edge < 0) continue;
            Json *route = s->net->edges[edge];
            cannot_wait |= eq(str(route, "status"), "closed") || num(route, "capacity_containers") < r.containers;
            cost += num(route, "cost_inr_per_container");
        }
        if (cannot_wait) reject(&r, "wait", "wait", "A remaining baseline route is closed or lacks capacity in source data.");
        else candidate(&r, "wait", "wait", "feasible", "Keep the itinerary and use the modeled wait at affected port calls.",
                       itinerary.eta, baseline, cost * r.containers, purchase, -1);
        Day horizon = r.deadline > itinerary.eta ? r.deadline : itinerary.eta;
        if (s->as_of > horizon) horizon = s->as_of;
        for (int i = 0; i < s->event_count; ++i) {
            const Event *e = &s->events[i];
            if (e->duration && e->start <= horizon && e->end > s->as_of && !has(blocked, e->port)) push(blocked, e->port);
        }
        reroutes(&r, shipment, itinerary, baseline, blocked, purchase);
        if (supplier_allowed) {
            each(supplier, table(s, "suppliers")) supplier_option(&r, shipment, supplier, blocked);
        } else reject(&r, "supplier", "supplier", "Replacing finished parts would bypass mandatory manufacturing.");
        cJSON_Delete(baseline); cJSON_Delete(blocked);
    }
    rank_options(&r, result, warnings);
    cJSON_Delete(r.options);
    return result;
}
