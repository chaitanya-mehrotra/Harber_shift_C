#ifndef HARBORSHIFT_BACKEND_H
#define HARBORSHIFT_BACKEND_H

#include "vendor/cJSON.h"
#include <limits.h>
#include <stddef.h>

typedef cJSON Json;
typedef int Day;
enum { NO_DAY = -1, LAST_DAY = 3652058, MAX_EVENTS = 11 };
typedef struct { Json *root; Json **ports, **edges; int *offset, *adjacent, *from, *to; int np, ne; } Network;
typedef struct { char message[512]; } Error;

#define each(item, array) for (Json *item = (array) ? (array)->child : NULL; item; item = item->next)
Json *get(const Json *obj, const char *key);
const char *str(const Json *obj, const char *key);
const char *string(const Json *item);
double num(const Json *obj, const char *key);
int eq(const char *a, const char *b);
int has(const Json *array, const char *value);
Json *lookup(const Json *array, const char *key, const char *value);
Json *clone(const Json *value);
void put(Json *obj, const char *key, const Json *value);
void push(Json *array, const char *value);
void fail(Error *error, const char *fmt, ...);
int integer(const Json *obj, const char *key, int minimum, int maximum, int fallback, Error *error);
int list(const Json *obj, const char *key, int max, Error *error);
int read_bool(const Json *obj, const char *key, Error *error);
char *read_file(const char *name, size_t limit, size_t *size);
Day day(const char *iso);
Day advance(Day date, int days, Error *error);
void set_day(Json *obj, const char *key, Day date);
int network_init(Network *network, Json *data, Error *error);
void network_destroy(Network *network);
int port_index(const Network *network, const char *id);
int edge_index(const Network *network, const char *id);
Json *shortest(const Network *network, const char *origin, const char *destination,
               int containers, int cheapest, Json *stops, Json *closed_ports, Json *closed_routes, Json *trace, Error *error);
Json *route_request(const Network *network, Json *query, Error *error);
Json *simulate(const Network *network, Json *query, Error *error);
Json *news(const Network *network);
void clear_news(void);
Json *parse_advisory_feed(const Network *network, const char *source, const char *data, size_t size);
Json *predict(Json *features);
void stop_model(void);
#endif
