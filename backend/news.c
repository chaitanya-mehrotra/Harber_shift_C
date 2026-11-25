#include "backend.h"
#include "vendor/yxml.h"
#include <windows.h>
#include <wininet.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_FEED = 1000000, MAX_ITEMS = 100 };
static const struct { const char *name, *url; } feeds[] = {
    {"UK Maritime and Coastguard Agency",
     "https://www.gov.uk/search/news-and-communications.atom?organisations%5B%5D=maritime-and-coastguard-agency"},
    {"NOAA National Hurricane Center · Atlantic", "https://www.nhc.noaa.gov/index-at.xml"}
};

typedef struct {
    char title[2048], summary[8192], link[4096], published[128], updated[128];
    char href[4096], rel[64];
} Item;

static void append(char *buffer, size_t size, const char *text) {
    size_t used = strlen(buffer), length = strlen(text);
    if (length < size - used) memcpy(buffer + used, text, length + 1);
}

static char *trim(char *text) {
    while (isspace((unsigned char)*text)) ++text;
    size_t length = strlen(text);
    while (length && isspace((unsigned char)text[length - 1])) text[--length] = 0;
    return text;
}

static void normalize_text(char *out, const char *text, int html) {
    int space = 1, tag = 0;
    for (; *text; ++text) {
        unsigned char c = (unsigned char)*text;
        if (html && c == '<') tag = 1;
        if (tag) { if (c == '>') { tag = 0; space = 1; } continue; }
        if (isspace(c)) { space = 1; continue; }
        if (space) {
            *out++ = ' ';
        }
        *out++ = (char)tolower(c);
        space = 0;
    }
    *out = 0;
}

static int word_char(unsigned char c) { return isalnum(c) || c == '_' || c >= 128; }
static int phrase(const char *text, const char *word) {
    size_t n = strlen(word);
    if (!n) return 0;
    for (const char *match = strstr(text, word); match; match = strstr(match + 1, word)) {
        if ((match == text || !word_char((unsigned char)match[-1])) && !word_char((unsigned char)match[n])) return 1;
    }
    return 0;
}

static int compare_terms(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static Json *candidate_for(const Network *net, const Item *item) {
    char text[sizeof item->title + sizeof item->summary + 4];
    normalize_text(text, item->title, 0);
    normalize_text(text + strlen(text), item->summary, 1);
    static const struct { const char *type, *terms[7]; } keywords[] = {
        {"strike", {"strike", "industrial action", "work stoppage"}},
        {"congestion", {"congestion", "backlog", "berthing delay", "berthing delays"}},
        {"severe_weather", {"cyclone", "hurricane", "typhoon", "storm surge", "severe weather", "tropical storm"}},
        {"equipment_failure", {"equipment failure", "crane failure", "equipment outage"}}
    };
    int count = 0;
    const char *type = NULL;
    for (size_t i = 0; i < sizeof keywords / sizeof *keywords; ++i) {
        for (size_t j = 0; keywords[i].terms[j]; ++j) if (phrase(text, keywords[i].terms[j])) {
            type = keywords[i].type;
            ++count;
            break;
        }
    }
    if (!count) return NULL;

    const char **matches = calloc(net->np, sizeof *matches);
    if (!matches) return NULL;
    int ports = 0;
    for (int i = 0; i < net->np; ++i) {
        const char *id = str(net->ports[i], "port_id"), *name = str(net->ports[i], "port_name");
        if (!*name || strlen(name) > 220) continue;
        char lower[224], alias[256];
        normalize_text(lower, name, 0);
        const char *formats[] = {"port of %s", "%s port", "%s harbor", "%s harbour"};
        int found = 0;
        for (int j = 0; j < 4; ++j) {
            snprintf(alias, sizeof alias, formats[j], trim(lower));
            found |= phrase(text, alias);
        }
        if (eq(id, "P01")) found |= phrase(text, "port of new york and new jersey") || phrase(text, "new york harbor");
        if (eq(id, "P05")) found |= phrase(text, "port of durban") || phrase(text, "durban harbour");
        if (eq(id, "P11")) found |= phrase(text, "port of mumbai") || phrase(text, "mumbai harbour") || phrase(text, "bombay harbour");
        if (found) matches[ports++] = id;
    }
    if (!ports && !phrase(text, "port") && !phrase(text, "harbor") && !phrase(text, "harbour") &&
        !phrase(text, "terminal") && !phrase(text, "berth") && !phrase(text, "maritime")) {
        free(matches);
        return NULL;
    }
    Json *result = cJSON_CreateObject();
    if (count == 1) cJSON_AddStringToObject(result, "event_type", type);
    else cJSON_AddNullToObject(result, "event_type");
    if (ports == 1) cJSON_AddStringToObject(result, "port_id", matches[0]);
    else cJSON_AddNullToObject(result, "port_id");
    Json *ids = cJSON_AddArrayToObject(result, "matched_ports");
    qsort(matches, ports, sizeof *matches, compare_terms);
    for (int i = 0; i < ports; ++i) push(ids, matches[i]);
    free(matches);
    cJSON_AddStringToObject(result, "confidence", count == 1 && ports == 1 ?
                            "candidate · review required" : "ambiguous · review required");
    cJSON_AddNullToObject(result, "duration_days");
    cJSON_AddNullToObject(result, "severity");
    return result;
}

static int tracking(const char *part) {
    const char *equal = strchr(part, '=');
    size_t length = equal ? (size_t)(equal - part) : strlen(part);
    return !strnicmp(part, "utm_", 4) ||
           (length == 6 && !strnicmp(part, "fbclid", 6)) ||
           (length == 5 && !strnicmp(part, "gclid", 5));
}

static int normalize_url(char *out, size_t size, char *url) {
    url = trim(url);
    size_t scheme = !strncmp(url, "https://", 8) ? 8 : !strncmp(url, "http://", 7) ? 7 : 0;
    if (!scheme) return 0;
    char *fragment = strchr(url, '#');
    if (fragment) *fragment = 0;
    for (const unsigned char *c = (const unsigned char *)url; *c; ++c)
        if (*c <= 32 || *c == 127 || *c == '\\') return 0;
    char *host = url + scheme, *end = host + strcspn(host, "/?");
    if (host == end || memchr(host, '@', (size_t)(end - host))) return 0;
    for (char *c = host; c < end; ++c) *c = (char)tolower((unsigned char)*c);
    char *query = strchr(end, '?');
    if (query) *query++ = 0;
    snprintf(out, size, "%s%s", url, *end ? "" : "/");
    const char *terms[256];
    int count = 0;
    while (query && *query && count < 256) {
        char *next = strchr(query, '&');
        if (next) *next++ = 0;
        if (*query && !tracking(query)) terms[count++] = query;
        query = next;
    }
    qsort(terms, count, sizeof *terms, compare_terms);
    for (int i = 0; i < count; ++i) { append(out, size, i ? "&" : "?"); append(out, size, terms[i]); }
    return 1;
}

static void set_timestamp(Json *article, char *date) {
    date = trim(date);
    size_t n = strlen(date);
    char iso[144];
    SYSTEMTIME time;
    char calendar[11] = {0};
    if (n >= 10) memcpy(calendar, date, 10);
    if (n >= 19 && day(calendar) >= 0 && (date[10] == 'T' || date[10] == ' ')) {
        if (date[n - 1] == 'Z') snprintf(iso, sizeof iso, "%.*s+00:00", (int)n - 1, date);
        else snprintf(iso, sizeof iso, "%s", date);
        iso[10] = 'T';
        cJSON_AddStringToObject(article, "published", iso);
    } else if (*date && InternetTimeToSystemTime(date, &time, 0)) {
        snprintf(iso, sizeof iso, "%04u-%02u-%02uT%02u:%02u:%02u+00:00",
                 time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
        cJSON_AddStringToObject(article, "published", iso);
    } else cJSON_AddNullToObject(article, "published");
}

static void add_article(const Network *net, const char *source, Item *item, Json *articles) {
    char url[sizeof item->link + 2];
    char *title = trim(item->title);
    if (!*title || !normalize_url(url, sizeof url, item->link)) return;
    Json *candidate = candidate_for(net, item);
    if (!candidate) return;
    Json *article = cJSON_CreateObject();
    cJSON_AddStringToObject(article, "title", title);
    cJSON_AddStringToObject(article, "url", url);
    cJSON_AddStringToObject(article, "source", source);
    set_timestamp(article, *trim(item->published) ? item->published : item->updated);
    cJSON_AddItemToObject(article, "candidate", candidate);
    cJSON_AddItemToArray(articles, article);
}

static const char *local_name(const char *tag) {
    const char *colon = strrchr(tag, ':');
    return colon ? colon + 1 : tag;
}

Json *parse_advisory_feed(const Network *net, const char *source, const char *data, size_t size) {
    if (size > MAX_FEED || memchr(data, 0, size)) return NULL;
    for (size_t i = 0; i + 8 <= size; ++i) {
        if (data[i] != '<' || data[i+1] != '!') continue;
        if (!memcmp(data + i, "<!ENTITY", 8) ||
            (i + 9 <= size && !memcmp(data + i, "<!DOCTYPE", 9))) return NULL;
    }
    yxml_t xml;
    char stack[8192];
    yxml_init(&xml, stack, sizeof stack);
    Json *items = cJSON_CreateArray();
    Item item = {0};
    enum { NONE, TITLE, SUMMARY, LINK, PUBLISHED, UPDATED } field = NONE;
    enum { ATTRIBUTE_NONE, HREF, REL } attribute = ATTRIBUTE_NONE;
    int depth = 0, atom = 0, entry = 0, channel = 0, count = 0, valid_root = 0;
    for (size_t i = 0; i < size; ++i) {
        yxml_ret_t token = yxml_parse(&xml, (unsigned char)data[i]);
        if (token < 0) goto invalid;
        switch (token) {
            case YXML_ELEMSTART: {
                ++depth;
                const char *tag = local_name(xml.elem);
                if (depth == 1) {
                    atom = eq(tag, "feed");
                    valid_root = atom || eq(tag, "rss");
                    if (!valid_root) goto invalid;
                }
                if (!atom && depth == 2) channel = eq(tag, "channel");
                if (((atom && depth == 2 && eq(tag, "entry")) ||
                     (!atom && channel && depth == 3 && eq(tag, "item"))) && count < MAX_ITEMS) {
                    entry = depth;
                    ++count;
                    memset(&item, 0, sizeof item);
                } else if (entry && depth == entry + 1) {
                    if (eq(tag, "title")) field = TITLE;
                    else if (eq(tag, atom ? "summary" : "description")) field = SUMMARY;
                    else if (eq(tag, atom ? "published" : "pubDate")) field = PUBLISHED;
                    else if (atom && eq(tag, "updated")) field = UPDATED;
                    else if (eq(tag, "link")) {
                        field = LINK;
                        item.href[0] = item.rel[0] = 0;
                    }
                }
                break;
            }
            case YXML_CONTENT:
                if (field == TITLE) append(item.title, sizeof item.title, xml.data);
                if (field == SUMMARY) append(item.summary, sizeof item.summary, xml.data);
                if (field == LINK && !atom) append(item.link, sizeof item.link, xml.data);
                if (field == PUBLISHED) append(item.published, sizeof item.published, xml.data);
                if (field == UPDATED) append(item.updated, sizeof item.updated, xml.data);
                break;
            case YXML_ATTRSTART:
                if (atom && field == LINK && depth == entry + 1) {
                    if (eq(xml.attr, "href")) attribute = HREF;
                    if (eq(xml.attr, "rel")) attribute = REL;
                }
                break;
            case YXML_ATTRVAL:
                if (attribute == HREF) append(item.href, sizeof item.href, xml.data);
                if (attribute == REL) append(item.rel, sizeof item.rel, xml.data);
                break;
            case YXML_ATTREND: attribute = ATTRIBUTE_NONE; break;
            case YXML_ELEMEND:
                if (entry && depth == entry + 1) {
                    if (field == LINK && atom && !*item.link && (!*item.rel || eq(item.rel, "alternate")))
                        append(item.link, sizeof item.link, item.href);
                    field = NONE;
                }
                if (entry && depth == entry) { add_article(net, source, &item, items); entry = 0; }
                --depth;
                break;
            default: break;
        }
    }
    if (valid_root && yxml_eof(&xml) == YXML_OK) return items;
invalid:
    cJSON_Delete(items);
    return NULL;
}

static char *fetch(HINTERNET session, const char *url, size_t *size) {
    HINTERNET request = InternetOpenUrlA(session, url, NULL, 0, INTERNET_FLAG_NO_UI |
        INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_AUTH | INTERNET_FLAG_RELOAD |
        INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_AUTO_REDIRECT, 0);
    if (!request) return NULL;
    char *data = NULL;
    DWORD status, length = sizeof status;
    if (HttpQueryInfoA(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &length, NULL) &&
        status == 200) {
        data = malloc(MAX_FEED + 2);
        if (data) {
            *size = 0;
            while (*size <= MAX_FEED) {
                DWORD got;
                if (!InternetReadFile(request, data + *size, (DWORD)(MAX_FEED + 1 - *size), &got)) break;
                if (!got) { data[*size] = 0; InternetCloseHandle(request); return data; }
                *size += got;
            }
            free(data);
            data = NULL;
        }
    }
    InternetCloseHandle(request);
    return data;
}

static int duplicate(Json *articles, Json *item) {
    char title[2050];
    normalize_text(title, str(item, "title"), 0);
    each(old, articles) {
        if (eq(str(old, "url"), str(item, "url"))) return 1;
        char previous[2050];
        normalize_text(previous, str(old, "title"), 0);
        if (eq(previous, title) && eq(str(old, "published"), str(item, "published"))) return 1;
    }
    return 0;
}

static Json *cached;

void clear_news(void) {
    cJSON_Delete(cached);
    cached = NULL;
}

Json *news(const Network *net) {
    static DWORD loaded_at;
    DWORD now = GetTickCount();
    if (cached && now - loaded_at < 600000) return clone(cached);
    clear_news();
    cached = cJSON_CreateObject();
    Json *articles = cJSON_AddArrayToObject(cached, "articles");
    Json *errors = cJSON_AddArrayToObject(cached, "errors");
    HINTERNET session = InternetOpenA("HarborShift/1.0 (public advisory reader)", INTERNET_OPEN_TYPE_PRECONFIG,
                                     NULL, NULL, 0);
    if (session) {
        DWORD timeout = 5000, retries = 1;
        InternetSetOptionA(session, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof timeout);
        InternetSetOptionA(session, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof timeout);
        InternetSetOptionA(session, INTERNET_OPTION_CONNECT_RETRIES, &retries, sizeof retries);
    }
    for (size_t i = 0; i < sizeof feeds / sizeof *feeds; ++i) {
        size_t size;
        char *xml = session ? fetch(session, feeds[i].url, &size) : NULL;
        Json *items = xml ? parse_advisory_feed(net, feeds[i].name, xml, size) : NULL;
        free(xml);
        if (!items) {
            char message[160];
            snprintf(message, sizeof message, "%s feed unavailable.", feeds[i].name);
            push(errors, message);
        }
        each(item, items) if (!duplicate(articles, item)) cJSON_AddItemToArray(articles, clone(item));
        cJSON_Delete(items);
    }
    if (session) InternetCloseHandle(session);
    loaded_at = GetTickCount();
    return clone(cached);
}
