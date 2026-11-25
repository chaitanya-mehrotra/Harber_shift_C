#ifndef HARBORSHIFT_CLI_H
#define HARBORSHIFT_CLI_H
#include "backend.h"
int validate_csv(const char *directory, int export_json);
int cli(const Network *net, int count, char **args);
#endif
