#ifndef HARBORSHIFT_DATA_H
#define HARBORSHIFT_DATA_H
#include "backend.h"

Json *read_dataset(const char *csv_directory, Error *error);
int attach_manufacturing(Network *network, Error *error);
#endif
