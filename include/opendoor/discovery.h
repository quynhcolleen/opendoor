#ifndef OPENDOOR_DISCOVERY_H
#define OPENDOOR_DISCOVERY_H

#include "opendoor/scan.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    OD_SOURCE_ENV = 0,
    OD_SOURCE_COMPOSE,
    OD_SOURCE_PACKAGE_JSON,
    OD_SOURCE_MAKEFILE
} OdPortSourceKind;

typedef enum {
    OD_DECLARATION_LITERAL = 0,
    OD_DECLARATION_ENV_REFERENCE,
    OD_DECLARATION_UNRESOLVABLE
} OdPortDeclarationKind;

typedef enum {
    OD_WRITE_MANUAL_ONLY = 0,
    OD_WRITE_ENV_LITERAL,
    OD_WRITE_COMPOSE_LITERAL
} OdPortWriteKind;

typedef struct {
    OdPortSourceKind source_kind;
    OdPortDeclarationKind declaration_kind;
    OdPortWriteKind write_kind;
    uint16_t port;
    uint16_t fallback_port;
    size_t line;
    size_t column;
    size_t byte_offset;
    size_t byte_length;
    size_t file_size;
    uint64_t file_hash;
    size_t definition_index;
    char *absolute_path;
    char *relative_path;
    char *relative_folder;
    char *environment_key;
    char *line_text;
    char *manual_reason;
} OdPortDeclaration;

typedef struct {
    char *relative_path;
    char *relative_folder;
    char *environment_key;
    size_t value_offset;
    bool direct_port;
} OdEnvironmentAssignment;

typedef struct {
    OdPortDeclaration *items;
    size_t count;
    OdEnvironmentAssignment *environment_assignments;
    size_t environment_assignment_count;
    char **warnings;
    size_t warning_count;
} OdProjectDiscovery;

OdStatus od_discover_project_ports(const char *project_root,
                                   OdProjectDiscovery *result,
                                   OdError *error);
void od_project_discovery_free(OdProjectDiscovery *result);

OdStatus od_validate_discovery_text(OdPortSourceKind source_kind,
                                    const char *text,
                                    size_t length,
                                    OdError *error);

OdStatus od_discover_occupied_ports(const OdScanSnapshot *snapshot,
                                    uint16_t **ports,
                                    size_t *count,
                                    OdError *error);

#endif
