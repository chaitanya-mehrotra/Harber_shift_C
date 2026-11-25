#ifndef HARBORSHIFT_SCENARIO_H
#define HARBORSHIFT_SCENARIO_H
#include "backend.h"

typedef struct {
    const char *id, *port, *type, *status, *source, *headline;
    Day start, end;
    int duration, severity;
} Event;

typedef struct {
    const Network *net;
    Json *tables, *demo;
    Error *error;
    Event events[MAX_EVENTS];
    int event_count, use_ml, congestion, buffer, packing;
    Day as_of;
} Scenario;

typedef struct {
    Json *scheduled, *calls;
    Day eta;
    int waited;
} Itinerary;

typedef struct {
    Json *spec;
    int *order, *dependencies;
    Json **tasks;
    int count, raw, finished, delivery;
} Dag;

typedef struct { Day start, finish; Itinerary itinerary; int direct; } TaskTiming;

Json *table(const Scenario *scenario, const char *name);
Json *legs_for(const Scenario *scenario, const char *id);
int wait_at(const Scenario *scenario, const char *port, Day on, int sailing, int completed, Json *ids, Json *estimates);
Itinerary replay(const Scenario *scenario, Json *shipment, Json *legs, Day shifted_start);
void free_itinerary(Itinerary itinerary);
int dag_init(Dag *dag, Json *spec, Error *error);
void dag_free(Dag *dag);
TaskTiming *schedule(const Scenario *scenario, const Dag *dag, int baseline, int replaced, Day handoff);
void free_schedule(const Dag *dag, TaskTiming *timings);
Json *cascade_report(const Dag *dag, const TaskTiming *baseline, const TaskTiming *revised);
Json *recover(const Scenario *scenario, Json *shipment, Itinerary itinerary, double purchase,
              const Dag *dag, int task, Json *final_contract, int supplier_allowed);
#endif
