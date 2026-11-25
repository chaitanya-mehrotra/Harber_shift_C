#include "backend.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Json *get(const Json *obj, const char *key) { return cJSON_GetObjectItemCaseSensitive(obj, key); }
const char *string(const Json *item) { return cJSON_IsString(item) ? item->valuestring : ""; }
const char *str(const Json *obj, const char *key) { return string(get(obj, key)); }
double num(const Json *obj, const char *key) { return cJSON_GetNumberValue(get(obj, key)); }
int eq(const char *a, const char *b) { return strcmp(a, b) == 0; }
int has(const Json *array, const char *value) {
    each(item, array) if (eq(string(item), value)) return 1;
    return 0;
}
Json *lookup(const Json *array, const char *key, const char *value) {
    each(item, array) if (eq(str(item, key), value)) return item;
    return NULL;
}
Json *clone(const Json *value) { return value ? cJSON_Duplicate(value, 1) : cJSON_CreateNull(); }
void put(Json *obj, const char *key, const Json *value) { cJSON_AddItemToObject(obj, key, clone(value)); }
void push(Json *array, const char *value) { cJSON_AddItemToArray(array, cJSON_CreateString(value)); }

void fail(Error *error, const char *fmt, ...) {
    if (error->message[0]) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(error->message, sizeof error->message, fmt, args);
    va_end(args);
}

int integer(const Json *obj, const char *key, int minimum, int maximum, int fallback, Error *error) {
    Json *item = get(obj, key);
    if (!item && fallback >= minimum) return fallback;
    double value = cJSON_GetNumberValue(item);
    if (!isfinite(value) || value < minimum || value > maximum || floor(value) != value) {
        fail(error, "%s must be an integer from %d to %d", key, minimum, maximum);
        return minimum;
    }
    return (int)value;
}

int list(const Json *obj, const char *key, int max, Error *error) {
    Json *array = get(obj, key);
    if (!array) return 1;
    if (cJSON_IsArray(array) && cJSON_GetArraySize(array) <= max) {
        each(item, array) {
            if (!cJSON_IsString(item)) {
                fail(error, "%s must contain strings", key);
                return 0;
            }
        }
        return 1;
    }
    fail(error, "%s must be an array of at most %d strings", key, max);
    return 0;
}

int read_bool(const Json *obj, const char *key, Error *error) {
    Json *item = get(obj, key);
    if (item && !cJSON_IsBool(item)) fail(error, "%s must be a boolean", key);
    return cJSON_IsTrue(item);
}

char *read_file(const char *name, size_t limit, size_t *size) {
    FILE *file = fopen(name, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END)) { fclose(file); return NULL; }
    long length = ftell(file);
    if (length < 0 || (size_t)length > limit) { fclose(file); return NULL; }
    rewind(file);
    char *data = malloc((size_t)length + 1);
    if (data && fread(data, 1, (size_t)length, file) == (size_t)length) {
        data[length] = 0;
        if (size) *size = (size_t)length;
    } else {
        free(data);
        data = NULL;
    }
    fclose(file);
    return data;
}

static int days_before_year(int year) {
    int y = year - 1;
    return y * 365 + y / 4 - y / 100 + y / 400;
}

static int month_length(int year, int month) {
    static const int lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return lengths[month - 1] + (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

Day day(const char *iso) {
    if (strlen(iso) != 10 || iso[4] != '-' || iso[7] != '-') return NO_DAY;
    for (int i = 0; i < 10; ++i) {
        if (i != 4 && i != 7 && (iso[i] < '0' || iso[i] > '9')) return NO_DAY;
    }
    int year, month, date;
    if (sscanf(iso, "%4d-%2d-%2d", &year, &month, &date) != 3 ||
        year < 1 || month < 1 || month > 12 || date < 1 || date > month_length(year, month)) return NO_DAY;
    Day result = days_before_year(year) + date - 1;
    for (int m = 1; m < month; ++m) result += month_length(year, m);
    return result;
}

Day advance(Day date, int days, Error *error) {
    if (date < 0 || days < 0 || days > LAST_DAY - date) {
        fail(error, "Date arithmetic is outside the supported range");
        return LAST_DAY;
    }
    return date + days;
}

void set_day(Json *obj, const char *key, Day date) {
    if (date < 0) { cJSON_AddNullToObject(obj, key); return; }
    int low = 1, high = 10000;
    while (low + 1 < high) {
        int middle = (low + high) / 2;
        if (days_before_year(middle) <= date) low = middle;
        else high = middle;
    }
    int month = 1;
    int within = date - days_before_year(low);
    while (within >= month_length(low, month)) within -= month_length(low, month++);
    char iso[16];
    snprintf(iso, sizeof iso, "%04d-%02d-%02d", low, month, within + 1);
    cJSON_AddStringToObject(obj, key, iso);
}
