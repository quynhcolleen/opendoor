#ifndef OPENDOOR_DISCOVERY_H
#define OPENDOOR_DISCOVERY_H

#include "opendoor/common.h"
#include "opendoor/model.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    OD_CONFIDENCE_POSSIBLE = 1,
    OD_CONFIDENCE_LIKELY = 2,
    OD_CONFIDENCE_CONFIRMED = 3
} OdConfidence;

typedef struct {
    char stable_id[40];
    char *name;
    char *variable;
    char *group;
    uint16_t port;
    unsigned protocols;
    OdConfidence confidence;
    bool selected;
    OdStringList sources;
} OdCandidate;

typedef struct {
    OdCandidate *items;
    size_t count;
} OdCandidateList;

void od_candidate_list_init(OdCandidateList *list);
void od_candidate_list_free(OdCandidateList *list);
OdStatus od_discover_compose_text(const char *path,
                                  const char *text,
                                  OdCandidateList *list,
                                  OdError *error);
OdStatus od_discover_dotenv_text(const char *path,
                                 const char *text,
                                 OdCandidateList *list,
                                 OdError *error);
OdStatus od_discover_package_json(const char *path,
                                  const char *text,
                                  OdCandidateList *list,
                                  OdError *error);
OdStatus od_discover_makefile_text(const char *path,
                                   const char *text,
                                   OdCandidateList *list,
                                   OdError *error);
OdStatus od_candidates_merge(OdCandidateList *destination,
                             const OdCandidateList *source,
                             OdError *error);

#endif

