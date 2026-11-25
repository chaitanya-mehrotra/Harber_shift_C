#include "scenario.h"
#include <stdlib.h>

static int task_index(const Dag *dag, const char *id) {
    for (int i = 0; i < dag->count; ++i)
        if (eq(str(dag->tasks[i], "task_id"), id)) return i;
    return -1;
}

int dag_init(Dag *dag, Json *spec, Error *error) {
    Json *tasks = get(spec, "tasks");
    int count = cJSON_GetArraySize(tasks);
    dag->count = count;
    dag->spec = spec;
    if (count < 1 || count > 128) { fail(error, "Invalid manufacturing task count"); return 0; }
    dag->tasks = calloc(count, sizeof *dag->tasks);
    dag->dependencies = calloc(count * count, sizeof *dag->dependencies);
    dag->order = calloc(count, sizeof *dag->order);
    if (!dag->tasks || !dag->dependencies || !dag->order) {
        fail(error, "Could not allocate manufacturing graph");
        return 0;
    }
    int index = 0, indegree[128] = {0};
    each(task, tasks) dag->tasks[index++] = task;
    for (int child = 0; child < count; ++child) {
        each(parent, get(dag->tasks[child], "predecessors")) {
            int p = task_index(dag, string(parent));
            if (p < 0 || dag->dependencies[p * count + child]) {
                fail(error, "Invalid manufacturing predecessor: %s", string(parent));
                return 0;
            }
            dag->dependencies[p * count + child] = 1;
            ++indegree[child];
        }
    }

    /* Kahn's algorithm: the topological order also serves as the FIFO queue. */
    int tail = 0;
    for (int i = 0; i < count; ++i) if (!indegree[i]) dag->order[tail++] = i;
    for (int head = 0; head < tail; ++head) {
        int parent = dag->order[head];
        for (int child = 0; child < count; ++child)
            if (dag->dependencies[parent * count + child] && --indegree[child] == 0) dag->order[tail++] = child;
    }
    dag->raw = task_index(dag, str(spec, "raw"));
    dag->finished = task_index(dag, str(spec, "finished"));
    dag->delivery = task_index(dag, str(spec, "delivery"));
    if (tail != count || dag->raw < 0 || dag->finished < 0 || dag->delivery < 0) {
        fail(error, "Manufacturing graph contains a cycle or a missing task");
        return 0;
    }
    return 1;
}

void dag_free(Dag *dag) { free(dag->tasks); free(dag->dependencies); free(dag->order); }

TaskTiming *schedule(const Scenario *scenario, const Dag *dag, int baseline, int replaced, Day handoff) {
    int count = dag->count;
    TaskTiming *timing = calloc(count, sizeof *timing);
    if (!timing) { fail(scenario->error, "Could not allocate manufacturing schedule"); return NULL; }
    for (int i = 0; i < count && !scenario->error->message[0]; ++i) {
        int index = dag->order[i];
        Json *task = dag->tasks[index];
        Day start = day(str(task, "planned_start"));
        for (int p = 0; p < count; ++p)
            if (dag->dependencies[p * count + index] && timing[p].finish > start) start = timing[p].finish;
        timing[index].start = start;
        Day planned = advance(start, (int)num(task, "duration_days"), scenario->error);
        timing[index].finish = planned;
        const char *shipment_id = str(task, "shipment_id");
        if (baseline) continue;
        if (index == replaced) {
            timing[index].finish = handoff > start ? handoff : start;
        } else if (*shipment_id) {
            Json *shipment = lookup(table(scenario, "shipments"), "shipment_id", shipment_id);
            Json *legs = legs_for(scenario, shipment_id);
            timing[index].itinerary = replay(scenario, shipment, legs, start);
            cJSON_Delete(legs);
            timing[index].finish = timing[index].itinerary.eta;
            timing[index].direct = timing[index].finish - planned;
        }
    }
    return timing;
}

void free_schedule(const Dag *dag, TaskTiming *timing) {
    if (!timing) return;
    for (int i = 0; i < dag->count; ++i) free_itinerary(timing[i].itinerary);
    free(timing);
}

static Json *timing_record(TaskTiming timing) {
    Json *record = cJSON_CreateObject();
    set_day(record, "start", timing.start);
    set_day(record, "finish", timing.finish);
    return record;
}

static void bfs_parents(const Dag *dag, int root, int *parents) {
    int queue[128], tail = 0;
    for (int i = 0; i < dag->count; ++i) parents[i] = -1;
    parents[root] = root;
    queue[tail++] = root;
    for (int head = 0; head < tail; ++head) {
        int parent = queue[head];
        for (int child = 0; child < dag->count; ++child) {
            if (parents[child] < 0 && dag->dependencies[parent * dag->count + child]) {
                parents[child] = parent;
                queue[tail++] = child;
            }
        }
    }
}

Json *cascade_report(const Dag *dag, const TaskTiming *baseline, const TaskTiming *revised) {
    Json *result = cJSON_CreateArray(), *reason_lists[128];
    for (int i = 0; i < dag->count; ++i) {
        int index = dag->order[i];
        Json *record = cJSON_CreateObject();
        cJSON_AddItemToArray(result, record);
        put(record, "task_id", get(dag->tasks[index], "task_id"));
        put(record, "label", get(dag->tasks[index], "label"));
        cJSON_AddItemToObject(record, "baseline", timing_record(baseline[index]));
        cJSON_AddItemToObject(record, "revised", timing_record(revised[index]));
        cJSON_AddNumberToObject(record, "direct_delay_days", revised[index].direct);
        reason_lists[index] = cJSON_AddArrayToObject(record, "reason_paths");
        if (revised[index].itinerary.calls) put(record, "calls", revised[index].itinerary.calls);
        else cJSON_AddArrayToObject(record, "calls");
    }
    for (int i = 0; i < dag->count; ++i) {
        int root = dag->order[i];
        if (revised[root].direct <= 0) continue;
        int parents[128];
        bfs_parents(dag, root, parents);
        for (int end = 0; end < dag->count; ++end) {
            if (parents[end] < 0) continue;
            int reversed[128], n = 0;
            for (int next = end; next != root; next = parents[next]) reversed[n++] = next;
            Json *path = cJSON_CreateArray();
            push(path, str(dag->tasks[root], "task_id"));
            while (n) push(path, str(dag->tasks[reversed[--n]], "task_id"));
            cJSON_AddItemToArray(reason_lists[end], path);
        }
    }
    return result;
}
