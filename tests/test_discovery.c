#include "opendoor/dashboard.h"
#include "opendoor/discovery.h"
#include "opendoor/scan.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) {                                                       \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                                  \
            ++failures;                                                           \
        }                                                                         \
    } while (0)

static void write_text_file(const char *path, const char *text) {
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    CHECK(descriptor >= 0);
    if (descriptor < 0) return;
    size_t length = strlen(text);
    size_t offset = 0U;
    while (offset < length) {
        ssize_t count = write(descriptor, text + offset, length - offset);
        if (count < 0 && errno == EINTR) continue;
        CHECK(count > 0);
        if (count <= 0) break;
        offset += (size_t)count;
    }
    CHECK(close(descriptor) == 0);
}

static void make_directory(const char *path) {
    CHECK(mkdir(path, 0700) == 0);
}

static const OdPortDeclaration *find_declaration(const OdProjectDiscovery *result,
                                                 const char *relative_path,
                                                 uint16_t port) {
    for (size_t index = 0U; index < result->count; ++index) {
        const OdPortDeclaration *item = &result->items[index];
        if (item->port == port && strcmp(item->relative_path, relative_path) == 0) {
            return item;
        }
    }
    return NULL;
}

static const OdPortDeclaration *find_reference(const OdProjectDiscovery *result,
                                               const char *relative_path,
                                               const char *environment_key) {
    for (size_t index = 0U; index < result->count; ++index) {
        const OdPortDeclaration *item = &result->items[index];
        if (item->environment_key != NULL &&
            strcmp(item->relative_path, relative_path) == 0 &&
            strcmp(item->environment_key, environment_key) == 0 &&
            item->source_kind == OD_SOURCE_COMPOSE) {
            return item;
        }
    }
    return NULL;
}

static const OdPortDeclaration *find_source_reference(
    const OdProjectDiscovery *result,
    OdPortSourceKind source_kind,
    const char *relative_path,
    const char *environment_key) {
    for (size_t index = 0U; index < result->count; ++index) {
        const OdPortDeclaration *item = &result->items[index];
        if (item->source_kind == source_kind && item->environment_key != NULL &&
            strcmp(item->relative_path, relative_path) == 0 &&
            strcmp(item->environment_key, environment_key) == 0) {
            return item;
        }
    }
    return NULL;
}

static const OdPortDeclaration *find_env_definition(
    const OdProjectDiscovery *result,
    const char *relative_path,
    const char *environment_key) {
    return find_source_reference(result, OD_SOURCE_ENV, relative_path,
                                 environment_key);
}

static void test_proc_socket_parsing_and_deduplication(void) {
    const char *tcp4 =
        "  sl  local_address rem_address st tx_queue rx_queue tr tm->when retrnsmt uid timeout inode\n"
        "   0: 0100007F:0BB8 00000000:0000 0A 00000000:00000000 00:00000000 00000000 1000 0 11111\n"
        "   1: 0100007F:0BB8 00000000:0000 0A 00000000:00000000 00:00000000 00000000 1000 0 11111\n";
    const char *udp6 =
        "  sl  local_address rem_address st tx_queue rx_queue tr tm->when retrnsmt uid timeout inode\n"
        "   0: 00000000000000000000000000000000:14E9 00000000000000000000000000000000:0000 07 00000000:00000000 00:00000000 00000000 1001 0 22222\n";
    OdScanSnapshot snapshot;
    OdError error;
    od_scan_snapshot_init(&snapshot, 7U);
    CHECK(od_parse_proc_net(tcp4, AF_INET, OD_PROTOCOL_TCP,
                            &snapshot, &error) == OD_OK);
    CHECK(od_parse_proc_net(udp6, AF_INET6, OD_PROTOCOL_UDP,
                            &snapshot, &error) == OD_OK);
    CHECK(snapshot.endpoint_count == 2U);
    if (snapshot.endpoint_count == 2U) {
        CHECK(strcmp(snapshot.endpoints[0].local_address, "127.0.0.1") == 0);
        CHECK(snapshot.endpoints[0].local_port == 3000U);
        CHECK(snapshot.endpoints[1].local_port == 5353U);
        CHECK(snapshot.endpoints[1].protocol == OD_PROTOCOL_UDP);
    }
    od_scan_snapshot_free(&snapshot);
}

static void test_socket_inode_target(void) {
    uint64_t inode = 0U;
    CHECK(od_parse_socket_inode("socket:[987654]", &inode));
    CHECK(inode == 987654U);
    CHECK(!od_parse_socket_inode("pipe:[987654]", &inode));
    CHECK(!od_parse_socket_inode("socket:[broken]", &inode));
}

static void test_occupied_ports_are_unique_and_sorted(void) {
    OdScanSnapshot snapshot;
    OdError error;
    od_scan_snapshot_init(&snapshot, 1U);
    snapshot.endpoints = calloc(4U, sizeof(*snapshot.endpoints));
    CHECK(snapshot.endpoints != NULL);
    if (snapshot.endpoints == NULL) return;
    snapshot.endpoint_count = 4U;
    snapshot.endpoint_capacity = 4U;
    snapshot.endpoints[0].local_port = 5000U;
    snapshot.endpoints[1].local_port = 3000U;
    snapshot.endpoints[2].local_port = 5000U;
    snapshot.endpoints[3].local_port = 4000U;

    uint16_t *ports = NULL;
    size_t count = 0U;
    CHECK(od_discover_occupied_ports(&snapshot, &ports, &count, &error) == OD_OK);
    CHECK(count == 3U);
    if (count == 3U) {
        CHECK(ports[0] == 3000U);
        CHECK(ports[1] == 4000U);
        CHECK(ports[2] == 5000U);
    }
    free(ports);
    od_scan_snapshot_free(&snapshot);
}

static void test_recursive_env_discovery_records_exact_spans(void) {
    char root[] = "/tmp/opendoor-discovery-env-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char services[512];
    char api[512];
    char root_env[512];
    char nested_env[512];
    (void)snprintf(services, sizeof(services), "%s/services", root);
    (void)snprintf(api, sizeof(api), "%s/services/api", root);
    (void)snprintf(root_env, sizeof(root_env), "%s/.env", root);
    (void)snprintf(nested_env, sizeof(nested_env), "%s/services/api/.env.local", root);
    make_directory(services);
    make_directory(api);
    const char *root_text =
        "# root\n"
        "API_PORT=3000\n"
        "QUOTED_PORT = \"5173\" # frontend\n"
        "DUP=3000\n"
        "AMBIG=3000 3001\n"
        "COMMENT=# 9999\n";
    write_text_file(root_env, root_text);
    write_text_file(nested_env, "WORKER_PORT='9000'\n");

    OdProjectDiscovery result;
    OdError error;
    CHECK(od_discover_project_ports(root, &result, &error) == OD_OK);
    CHECK(result.count == 4U);

    const OdPortDeclaration *api_port = find_declaration(&result, "./.env", 3000U);
    CHECK(api_port != NULL);
    if (api_port != NULL) {
        CHECK(api_port->source_kind == OD_SOURCE_ENV);
        CHECK(api_port->declaration_kind == OD_DECLARATION_LITERAL);
        CHECK(api_port->write_kind == OD_WRITE_ENV_LITERAL);
        CHECK(strcmp(api_port->relative_folder, "./") == 0);
        CHECK(api_port->line == 2U);
        CHECK(api_port->column == 10U);
        CHECK(api_port->byte_offset == 16U);
        CHECK(api_port->byte_length == 4U);
        CHECK(api_port->file_size == strlen(root_text));
        CHECK(api_port->file_hash == UINT64_C(0xf38c8fecda946e4c));
        CHECK(strcmp(api_port->line_text, "API_PORT=3000") == 0);
        CHECK(api_port->definition_index == SIZE_MAX);
    }
    const OdPortDeclaration *quoted = find_declaration(&result, "./.env", 5173U);
    CHECK(quoted != NULL);
    if (quoted != NULL) {
        CHECK(quoted->line == 3U);
        CHECK(quoted->column == 16U);
        CHECK(quoted->byte_offset == 36U);
        CHECK(quoted->byte_length == 4U);
    }
    const OdPortDeclaration *duplicate =
        find_env_definition(&result, "./.env", "DUP");
    CHECK(duplicate != NULL);
    if (duplicate != NULL) CHECK(duplicate->port == 3000U);
    const OdPortDeclaration *worker =
        find_declaration(&result, "./services/api/.env.local", 9000U);
    CHECK(worker != NULL);
    if (worker != NULL) {
        CHECK(strcmp(worker->relative_folder, "./services/api") == 0);
        CHECK(worker->line == 1U);
        CHECK(worker->column == 14U);
        CHECK(worker->byte_offset == 13U);
        CHECK(worker->byte_length == 4U);
    }

    od_project_discovery_free(&result);
    CHECK(unlink(nested_env) == 0);
    CHECK(unlink(root_env) == 0);
    CHECK(rmdir(api) == 0);
    CHECK(rmdir(services) == 0);
    CHECK(rmdir(root) == 0);
}

static void test_env_template_and_unrelated_files_are_ignored(void) {
    char root[] = "/tmp/opendoor-discovery-ignore-XXXXXX";
    char outside[] = "/tmp/opendoor-discovery-outside-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    CHECK(mkdtemp(outside) != NULL);
    static const char *const skipped[] = {
        ".git", "node_modules", "vendor", "build", "build-debug", "dist"
    };
    char path[512];
    char nested[512];
    for (size_t index = 0U; index < sizeof(skipped) / sizeof(skipped[0]); ++index) {
        (void)snprintf(path, sizeof(path), "%s/%s", root, skipped[index]);
        make_directory(path);
        (void)snprintf(nested, sizeof(nested), "%s/%s/.env", root, skipped[index]);
        write_text_file(nested, "SKIPPED_PORT=6100\n");
    }
    static const char *const excluded[] = {
        ".env.example", ".env.sample", ".env.template",
        ".env.local.example", ".env.prod.sample", ".env.ci.template",
        ".env.opendoor.bak", ".env.local.opendoor.bak",
        ".env.opendoor.tmp.1234"
    };
    for (size_t index = 0U; index < sizeof(excluded) / sizeof(excluded[0]); ++index) {
        (void)snprintf(path, sizeof(path), "%s/%s", root, excluded[index]);
        write_text_file(path, "EXAMPLE_PORT=6200\n");
    }
    (void)snprintf(path, sizeof(path), "%s/ports.txt", root);
    write_text_file(path, "TEXT_PORT=6300\n");
    (void)snprintf(path, sizeof(path), "%s/.env.production", root);
    write_text_file(path, "REAL_PORT=7000\n");
    (void)snprintf(nested, sizeof(nested), "%s/.env", outside);
    write_text_file(nested, "OUTSIDE_PORT=6400\n");
    char link_path[512];
    (void)snprintf(link_path, sizeof(link_path), "%s/linked", root);
    CHECK(symlink(outside, link_path) == 0);

    OdProjectDiscovery result;
    OdError error;
    CHECK(od_discover_project_ports(root, &result, &error) == OD_OK);
    CHECK(result.count == 1U);
    if (result.count == 1U) {
        CHECK(result.items[0].port == 7000U);
        CHECK(strcmp(result.items[0].relative_path, "./.env.production") == 0);
    }
    od_project_discovery_free(&result);

    CHECK(unlink(link_path) == 0);
    (void)snprintf(path, sizeof(path), "%s/.env.production", root);
    CHECK(unlink(path) == 0);
    (void)snprintf(path, sizeof(path), "%s/ports.txt", root);
    CHECK(unlink(path) == 0);
    for (size_t index = 0U; index < sizeof(excluded) / sizeof(excluded[0]); ++index) {
        (void)snprintf(path, sizeof(path), "%s/%s", root, excluded[index]);
        CHECK(unlink(path) == 0);
    }
    for (size_t index = 0U; index < sizeof(skipped) / sizeof(skipped[0]); ++index) {
        (void)snprintf(nested, sizeof(nested), "%s/%s/.env", root, skipped[index]);
        CHECK(unlink(nested) == 0);
        (void)snprintf(path, sizeof(path), "%s/%s", root, skipped[index]);
        CHECK(rmdir(path) == 0);
    }
    CHECK(rmdir(root) == 0);
    (void)snprintf(path, sizeof(path), "%s/.env", outside);
    CHECK(unlink(path) == 0);
    CHECK(rmdir(outside) == 0);
}

static void test_compose_block_scalars_are_not_port_sections(void) {
    char root[] = "/tmp/opendoor-discovery-compose-scalar-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char compose[512];
    (void)snprintf(compose, sizeof(compose), "%s/compose.yaml", root);
    const char *text =
        "services:\n"
        "  app:\n"
        "    command: |\n"
        "      ports:\n"
        "        - \"3000:80\"\n"
        "    environment:\n"
        "      NOTE: >-\n"
        "        ports:\n"
        "          - \"4000:80\"\n"
        "    ports:\n"
        "      - \"5000:80\"\n";
    write_text_file(compose, text);

    OdProjectDiscovery result;
    OdError error;
    CHECK(od_discover_project_ports(root, &result, &error) == OD_OK);
    CHECK(find_declaration(&result, "./compose.yaml", 3000U) == NULL);
    CHECK(find_declaration(&result, "./compose.yaml", 4000U) == NULL);
    const OdPortDeclaration *actual =
        find_declaration(&result, "./compose.yaml", 5000U);
    CHECK(actual != NULL);
    if (actual != NULL) CHECK(actual->write_kind == OD_WRITE_COMPOSE_LITERAL);

    od_project_discovery_free(&result);
    CHECK(unlink(compose) == 0);
    CHECK(rmdir(root) == 0);
}

static void test_env_reference_requires_one_direct_assignment(void) {
    char root[] = "/tmp/opendoor-discovery-env-ambiguity-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char env_path[512];
    char compose[512];
    (void)snprintf(env_path, sizeof(env_path), "%s/.env", root);
    (void)snprintf(compose, sizeof(compose), "%s/compose.yaml", root);
    write_text_file(env_path,
                    "A=3000\n"
                    "B=3000\n"
                    "API_PORT=3100\n"
                    "API_PORT=${OTHER_PORT}\n");
    write_text_file(compose,
                    "services:\n"
                    "  app:\n"
                    "    ports:\n"
                    "      - \"${B}:80\"\n"
                    "      - \"${API_PORT}:81\"\n");

    OdProjectDiscovery result;
    OdError error;
    CHECK(od_discover_project_ports(root, &result, &error) == OD_OK);
    const OdPortDeclaration *a = find_env_definition(&result, "./.env", "A");
    const OdPortDeclaration *b = find_env_definition(&result, "./.env", "B");
    CHECK(a != NULL && b != NULL && a != b);
    const OdPortDeclaration *b_reference =
        find_reference(&result, "./compose.yaml", "B");
    CHECK(b_reference != NULL);
    if (b_reference != NULL) {
        CHECK(b_reference->port == 3000U);
        CHECK(b_reference->definition_index < result.count);
        if (b_reference->definition_index < result.count) {
            CHECK(strcmp(result.items[b_reference->definition_index].environment_key,
                         "B") == 0);
        }
    }
    const OdPortDeclaration *ambiguous =
        find_reference(&result, "./compose.yaml", "API_PORT");
    CHECK(ambiguous != NULL);
    if (ambiguous != NULL) {
        CHECK(ambiguous->port == 0U);
        CHECK(ambiguous->definition_index == SIZE_MAX);
        CHECK(ambiguous->declaration_kind == OD_DECLARATION_UNRESOLVABLE);
    }

    od_project_discovery_free(&result);
    CHECK(unlink(compose) == 0);
    CHECK(unlink(env_path) == 0);
    CHECK(rmdir(root) == 0);
}

static void test_compose_literals_and_direct_env_references(void) {
    char root[] = "/tmp/opendoor-discovery-compose-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char worker[512];
    char root_env[512];
    char root_compose[512];
    char worker_env[512];
    char worker_compose[512];
    (void)snprintf(worker, sizeof(worker), "%s/worker", root);
    (void)snprintf(root_env, sizeof(root_env), "%s/.env", root);
    (void)snprintf(root_compose, sizeof(root_compose), "%s/compose.yaml", root);
    (void)snprintf(worker_env, sizeof(worker_env), "%s/worker/.env.local", root);
    (void)snprintf(worker_compose, sizeof(worker_compose),
                   "%s/worker/docker-compose.yml", root);
    make_directory(worker);
    write_text_file(root_env,
                    "API_PORT=3100\n"
                    "ADMIN_PORT=3200\n"
                    "METRICS_PORT=3300\n");
    const char *compose_text =
        "services:\n"
        "  api:\n"
        "    ports:\n"
        "      - \"3000:80\"\n"
        "      - 3001:81/tcp\n"
        "      - \"${API_PORT:-3050}:82\"\n"
        "      - '${ADMIN_PORT}:83'\n"
        "      - $METRICS_PORT:84\n";
    write_text_file(root_compose, compose_text);
    write_text_file(worker_env, "WORKER_PORT=3400\n");
    write_text_file(worker_compose,
                    "services:\n"
                    "  worker:\n"
                    "    ports:\n"
                    "      - \"$WORKER_PORT:85\"\n");

    OdProjectDiscovery result;
    OdError error;
    CHECK(od_discover_project_ports(root, &result, &error) == OD_OK);

    const OdPortDeclaration *quoted =
        find_declaration(&result, "./compose.yaml", 3000U);
    CHECK(quoted != NULL);
    if (quoted != NULL) {
        CHECK(quoted->source_kind == OD_SOURCE_COMPOSE);
        CHECK(quoted->declaration_kind == OD_DECLARATION_LITERAL);
        CHECK(quoted->write_kind == OD_WRITE_COMPOSE_LITERAL);
        CHECK(quoted->line == 4U);
        CHECK(quoted->column == 10U);
        CHECK(quoted->byte_length == 4U);
        CHECK(strncmp(compose_text + quoted->byte_offset, "3000", 4U) == 0);
    }
    const OdPortDeclaration *protocol =
        find_declaration(&result, "./compose.yaml", 3001U);
    CHECK(protocol != NULL);
    if (protocol != NULL) {
        CHECK(protocol->write_kind == OD_WRITE_COMPOSE_LITERAL);
        CHECK(protocol->line == 5U);
        CHECK(protocol->column == 9U);
    }

    const OdPortDeclaration *api =
        find_reference(&result, "./compose.yaml", "API_PORT");
    CHECK(api != NULL);
    if (api != NULL) {
        CHECK(api->port == 3100U);
        CHECK(api->fallback_port == 3050U);
        CHECK(api->declaration_kind == OD_DECLARATION_ENV_REFERENCE);
        CHECK(api->write_kind == OD_WRITE_MANUAL_ONLY);
        CHECK(api->definition_index < result.count);
        if (api->definition_index < result.count) {
            const OdPortDeclaration *definition = &result.items[api->definition_index];
            CHECK(definition->source_kind == OD_SOURCE_ENV);
            CHECK(strcmp(definition->relative_path, "./.env") == 0);
            CHECK(strcmp(definition->environment_key, "API_PORT") == 0);
        }
        CHECK(api->byte_length == 4U);
        CHECK(strncmp(compose_text + api->byte_offset, "3050", 4U) == 0);
    }
    const OdPortDeclaration *admin =
        find_reference(&result, "./compose.yaml", "ADMIN_PORT");
    CHECK(admin != NULL);
    if (admin != NULL) {
        CHECK(admin->port == 3200U);
        CHECK(admin->fallback_port == 0U);
        CHECK(admin->definition_index < result.count);
    }
    const OdPortDeclaration *metrics =
        find_reference(&result, "./compose.yaml", "METRICS_PORT");
    CHECK(metrics != NULL);
    if (metrics != NULL) {
        CHECK(metrics->port == 3300U);
        CHECK(metrics->definition_index < result.count);
    }
    const OdPortDeclaration *worker_reference =
        find_reference(&result, "./worker/docker-compose.yml", "WORKER_PORT");
    CHECK(worker_reference != NULL);
    if (worker_reference != NULL) {
        CHECK(worker_reference->port == 3400U);
        CHECK(worker_reference->definition_index < result.count);
        if (worker_reference->definition_index < result.count) {
            CHECK(strcmp(result.items[worker_reference->definition_index].relative_path,
                         "./worker/.env.local") == 0);
        }
    }

    CHECK(od_validate_discovery_text(OD_SOURCE_ENV, "PORT=3000\n", 10U,
                                     &error) == OD_OK);
    CHECK(od_validate_discovery_text(OD_SOURCE_COMPOSE, compose_text,
                                     strlen(compose_text), &error) == OD_OK);
    CHECK(od_validate_discovery_text(OD_SOURCE_PACKAGE_JSON, "{}", 2U,
                                     &error) == OD_ERROR_UNSUPPORTED);

    od_project_discovery_free(&result);
    CHECK(unlink(worker_compose) == 0);
    CHECK(unlink(worker_env) == 0);
    CHECK(unlink(root_compose) == 0);
    CHECK(unlink(root_env) == 0);
    CHECK(rmdir(worker) == 0);
    CHECK(rmdir(root) == 0);
}

static void test_compose_rejects_unsupported_or_ambiguous_mappings(void) {
    char root[] = "/tmp/opendoor-discovery-compose-reject-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char local_env[512];
    char prod_env[512];
    char compose[512];
    (void)snprintf(local_env, sizeof(local_env), "%s/.env.local", root);
    (void)snprintf(prod_env, sizeof(prod_env), "%s/.env.production", root);
    (void)snprintf(compose, sizeof(compose), "%s/compose.yaml", root);
    write_text_file(local_env,
                    "CONFLICT_PORT=5100\n"
                    "OTHER_PORT=5200\n");
    write_text_file(prod_env,
                    "CONFLICT_PORT=5101\n"
                    "INDIRECT_PORT=$OTHER_PORT\n");
    const char *compose_text =
        "services:\n"
        "  bad:\n"
        "    ports:\n"
        "      - \"127.0.0.1:3000:80\"\n"
        "      - \"3000-3002:80\"\n"
        "      - target: 80\n"
        "        published: 3000\n"
        "      - \"3000:80 4000:90\"\n"
        "      # - \"6000:80\"\n"
        "      - \"${CONFLICT_PORT}:81\"\n"
        "      - \"${INDIRECT_PORT}:82\"\n";
    write_text_file(compose, compose_text);

    OdProjectDiscovery result;
    OdError error;
    CHECK(od_discover_project_ports(root, &result, &error) == OD_OK);
    CHECK(find_declaration(&result, "./compose.yaml", 3000U) == NULL);
    CHECK(find_declaration(&result, "./compose.yaml", 4000U) == NULL);
    CHECK(find_declaration(&result, "./compose.yaml", 6000U) == NULL);
    const OdPortDeclaration *conflict =
        find_reference(&result, "./compose.yaml", "CONFLICT_PORT");
    CHECK(conflict != NULL);
    if (conflict != NULL) {
        CHECK(conflict->port == 0U);
        CHECK(conflict->declaration_kind == OD_DECLARATION_UNRESOLVABLE);
        CHECK(conflict->definition_index == SIZE_MAX);
        CHECK(conflict->write_kind == OD_WRITE_MANUAL_ONLY);
    }
    const OdPortDeclaration *indirect =
        find_reference(&result, "./compose.yaml", "INDIRECT_PORT");
    CHECK(indirect != NULL);
    if (indirect != NULL) {
        CHECK(indirect->port == 0U);
        CHECK(indirect->declaration_kind == OD_DECLARATION_UNRESOLVABLE);
        CHECK(indirect->definition_index == SIZE_MAX);
    }

    const char *malformed =
        "services:\n"
        "  bad:\n"
        "    ports:\n"
        "      - \"7000:80\n";
    CHECK(od_validate_discovery_text(OD_SOURCE_COMPOSE, malformed,
                                     strlen(malformed), &error) == OD_ERROR_INVALID);
    CHECK(od_validate_discovery_text(OD_SOURCE_ENV, "PORT='3000\n", 11U,
                                     &error) == OD_ERROR_INVALID);

    od_project_discovery_free(&result);
    CHECK(unlink(compose) == 0);
    CHECK(unlink(prod_env) == 0);
    CHECK(unlink(local_env) == 0);
    CHECK(rmdir(root) == 0);
}

static void test_package_scripts_are_manual_only(void) {
    char root[] = "/tmp/opendoor-discovery-package-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char invalid_directory[512];
    char package[512];
    char invalid_package[512];
    (void)snprintf(invalid_directory, sizeof(invalid_directory), "%s/invalid", root);
    (void)snprintf(package, sizeof(package), "%s/package.json", root);
    (void)snprintf(invalid_package, sizeof(invalid_package),
                   "%s/invalid/package.json", root);
    make_directory(invalid_directory);
    const char *package_text =
        "{\n"
        "  \"scripts\": {\n"
        "    \"dev\": \"vite --port 5173\",\n"
        "    \"short\": \"serve -p 8080\",\n"
        "    \"escaped\": \"node app --port\\u00209090\",\n"
        "    \"equals\": \"vite --port=7000\",\n"
        "    \"number\": \"echo 7100\"\n"
        "  },\n"
        "  \"outside\": \"node --port 6000\"\n"
        "}\n";
    write_text_file(package, package_text);
    write_text_file(invalid_package,
                    "{\"scripts\": {\"bad\": \"node --port 6200\",}}\n");

    OdProjectDiscovery result;
    OdError error;
    CHECK(od_discover_project_ports(root, &result, &error) == OD_OK);
    const OdPortDeclaration *long_flag =
        find_declaration(&result, "./package.json", 5173U);
    const OdPortDeclaration *short_flag =
        find_declaration(&result, "./package.json", 8080U);
    const OdPortDeclaration *escaped =
        find_declaration(&result, "./package.json", 9090U);
    CHECK(long_flag != NULL);
    CHECK(short_flag != NULL);
    CHECK(escaped != NULL);
    const OdPortDeclaration *items[] = {long_flag, short_flag, escaped};
    for (size_t index = 0U; index < sizeof(items) / sizeof(items[0]); ++index) {
        if (items[index] == NULL) continue;
        CHECK(items[index]->source_kind == OD_SOURCE_PACKAGE_JSON);
        CHECK(items[index]->declaration_kind == OD_DECLARATION_LITERAL);
        CHECK(items[index]->write_kind == OD_WRITE_MANUAL_ONLY);
        CHECK(items[index]->definition_index == SIZE_MAX);
    }
    if (escaped != NULL) {
        CHECK(escaped->byte_length == 4U);
        CHECK(strncmp(package_text + escaped->byte_offset, "9090", 4U) == 0);
    }
    CHECK(find_declaration(&result, "./package.json", 6000U) == NULL);
    CHECK(find_declaration(&result, "./package.json", 7000U) == NULL);
    CHECK(find_declaration(&result, "./package.json", 7100U) == NULL);
    CHECK(find_declaration(&result, "./invalid/package.json", 6200U) == NULL);
    CHECK(result.warning_count == 1U);
    if (result.warning_count == 1U) {
        CHECK(strstr(result.warnings[0], "invalid/package.json") != NULL);
    }

    od_project_discovery_free(&result);
    CHECK(unlink(invalid_package) == 0);
    CHECK(unlink(package) == 0);
    CHECK(rmdir(invalid_directory) == 0);
    CHECK(rmdir(root) == 0);
}

static void test_makefile_ports_are_manual_only(void) {
    char root[] = "/tmp/opendoor-discovery-make-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char env_path[512];
    char makefile[512];
    (void)snprintf(env_path, sizeof(env_path), "%s/.env", root);
    (void)snprintf(makefile, sizeof(makefile), "%s/Makefile", root);
    write_text_file(env_path,
                    "API_PORT=4100\n"
                    "ADMIN_PORT=4200\n");
    const char *make_text =
        "PORT = 3000\n"
        "serve:\n"
        "\tvite --port 5173\n"
        "\tserver -p 8080\n"
        "\tproxy --port $API_PORT\n"
        "\tadmin -p ${ADMIN_PORT}\n"
        "COMMENTED = 6000 # 6100 is documentation\n"
        "# PORT=6200\n"
        "COMPUTED = $$(expr 6300 + 1)\n"
        "NESTED = $(shell echo 6400)\n"
        "bad:\n"
        "\tserver --port $(PORT)\n"
        "\tserver --port=6500\n";
    write_text_file(makefile, make_text);

    OdProjectDiscovery result;
    OdError error;
    CHECK(od_discover_project_ports(root, &result, &error) == OD_OK);
    static const uint16_t literal_ports[] = {3000U, 5173U, 6000U, 8080U};
    for (size_t index = 0U;
         index < sizeof(literal_ports) / sizeof(literal_ports[0]); ++index) {
        const OdPortDeclaration *item =
            find_declaration(&result, "./Makefile", literal_ports[index]);
        CHECK(item != NULL);
        if (item != NULL) {
            CHECK(item->source_kind == OD_SOURCE_MAKEFILE);
            CHECK(item->declaration_kind == OD_DECLARATION_LITERAL);
            CHECK(item->write_kind == OD_WRITE_MANUAL_ONLY);
        }
    }
    const OdPortDeclaration *api = find_source_reference(
        &result, OD_SOURCE_MAKEFILE, "./Makefile", "API_PORT");
    const OdPortDeclaration *admin = find_source_reference(
        &result, OD_SOURCE_MAKEFILE, "./Makefile", "ADMIN_PORT");
    CHECK(api != NULL);
    CHECK(admin != NULL);
    if (api != NULL) {
        CHECK(api->port == 4100U);
        CHECK(api->definition_index < result.count);
        CHECK(api->write_kind == OD_WRITE_MANUAL_ONLY);
    }
    if (admin != NULL) {
        CHECK(admin->port == 4200U);
        CHECK(admin->definition_index < result.count);
        CHECK(admin->write_kind == OD_WRITE_MANUAL_ONLY);
    }
    CHECK(find_declaration(&result, "./Makefile", 6100U) == NULL);
    CHECK(find_declaration(&result, "./Makefile", 6200U) == NULL);
    CHECK(find_declaration(&result, "./Makefile", 6300U) == NULL);
    CHECK(find_declaration(&result, "./Makefile", 6400U) == NULL);
    CHECK(find_declaration(&result, "./Makefile", 6500U) == NULL);

    od_project_discovery_free(&result);
    CHECK(unlink(makefile) == 0);
    CHECK(unlink(env_path) == 0);
    CHECK(rmdir(root) == 0);
}

static const OdPortRow *find_dashboard_row(const OdDashboard *dashboard,
                                           uint16_t port,
                                           const char *source) {
    for (size_t index = 0U; index < dashboard->count; ++index) {
        if (dashboard->rows[index].port == port &&
            strcmp(dashboard->rows[index].source, source) == 0) {
            return &dashboard->rows[index];
        }
    }
    return NULL;
}

static void test_dashboard_project_ownership_and_all_declaration_matches(void) {
    char root[] = "/tmp/opendoor-dashboard-project-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    static const char *const folders[] = {
        "alpha", "api", "bin", "other", "services", "web", "worker", "zeta"
    };
    char path[512];
    for (size_t index = 0U; index < sizeof(folders) / sizeof(folders[0]); ++index) {
        (void)snprintf(path, sizeof(path), "%s/%s", root, folders[index]);
        make_directory(path);
    }
    char executable[512];
    (void)snprintf(executable, sizeof(executable), "%s/bin/server", root);
    write_text_file(executable, "binary placeholder\n");
    char outside[512];
    (void)snprintf(outside, sizeof(outside), "%s-other", root);
    make_directory(outside);

    OdPortDeclaration declarations[] = {
        {.port = 3000U, .relative_path = "./api/.env", .relative_folder = "./api"},
        {.port = 3000U, .relative_path = "./compose.yaml", .relative_folder = "./"},
        {.port = 3000U, .relative_path = "./api/.env", .relative_folder = "./api"},
        {.port = 4000U, .relative_path = "./api/.env", .relative_folder = "./api"},
        {.port = 4100U, .relative_path = "./api/.env", .relative_folder = "./api"},
        {.port = 4200U, .relative_path = "./services/.env", .relative_folder = "./services"},
        {.port = 4300U, .relative_path = "./web/.env", .relative_folder = "./web"},
        {.port = 4400U, .relative_path = "./other/.env", .relative_folder = "./other"}
    };
    OdProjectDiscovery discovery = {
        .items = declarations,
        .count = sizeof(declarations) / sizeof(declarations[0])
    };
    OdEndpoint endpoints[11] = {0};
    endpoints[0] = (OdEndpoint){.local_port = 3000U, .pid = 10};
    (void)snprintf(endpoints[0].directory, sizeof(endpoints[0].directory),
                   "%s/worker", root);
    endpoints[1] = (OdEndpoint){.local_port = 4000U, .pid = 20};
    (void)snprintf(endpoints[1].directory, sizeof(endpoints[1].directory), "/tmp");
    endpoints[2] = (OdEndpoint){.local_port = 4100U, .pid = 0};
    endpoints[3] = (OdEndpoint){.local_port = 4200U, .pid = 30};
    (void)snprintf(endpoints[3].directory, sizeof(endpoints[3].directory),
                   "%s/services", root);
    endpoints[4] = (OdEndpoint){.local_port = 4200U, .pid = 31};
    (void)snprintf(endpoints[4].directory, sizeof(endpoints[4].directory), "/tmp");
    endpoints[5] = (OdEndpoint){.local_port = 4400U, .pid = 40};
    (void)snprintf(endpoints[5].directory, sizeof(endpoints[5].directory), "%s", outside);
    endpoints[6] = (OdEndpoint){.local_port = 5000U, .pid = 50};
    (void)snprintf(endpoints[6].directory, sizeof(endpoints[6].directory),
                   "%s/worker", root);
    endpoints[7] = endpoints[6];
    endpoints[8] = (OdEndpoint){.local_port = 5100U, .pid = 60};
    (void)snprintf(endpoints[8].executable, sizeof(endpoints[8].executable),
                   "%s", executable);
    endpoints[9] = (OdEndpoint){.local_port = 5200U, .pid = 70};
    (void)snprintf(endpoints[9].directory, sizeof(endpoints[9].directory),
                   "%s/zeta", root);
    endpoints[10] = (OdEndpoint){.local_port = 5200U, .pid = 71};
    (void)snprintf(endpoints[10].directory, sizeof(endpoints[10].directory),
                   "%s/alpha", root);
    OdScanSnapshot snapshot = {
        .endpoints = endpoints,
        .endpoint_count = sizeof(endpoints) / sizeof(endpoints[0])
    };
    OdDashboard dashboard;
    OdError error;
    CHECK(od_dashboard_init(&dashboard, root, &discovery, &snapshot, &error) == OD_OK);
    CHECK(dashboard.count == 10U);

    size_t port_3000_rows = 0U;
    for (size_t index = 0U; index < dashboard.count; ++index) {
        const OdPortRow *row = &dashboard.rows[index];
        if (row->port == 3000U) {
            ++port_3000_rows;
            CHECK(row->declared);
            CHECK(row->status == OD_PORT_RUNNING);
            CHECK(strcmp(row->source, "live") != 0);
        }
        CHECK(row->relative_folder[0] == '.' && row->relative_folder[1] == '/');
        if (row->declared) {
            CHECK(row->source[0] == '.' && row->source[1] == '/');
        }
        if (index > 0U) {
            const OdPortRow *previous = &dashboard.rows[index - 1U];
            int folder_order = strcmp(previous->relative_folder, row->relative_folder);
            CHECK(folder_order < 0 ||
                  (folder_order == 0 && previous->port <= row->port));
            if (folder_order == 0 && previous->port == row->port) {
                CHECK(strcmp(previous->source, row->source) <= 0);
            }
        }
    }
    CHECK(port_3000_rows == 2U);
    const OdPortRow *root_compose =
        find_dashboard_row(&dashboard, 3000U, "./compose.yaml");
    const OdPortRow *api_env =
        find_dashboard_row(&dashboard, 3000U, "./api/.env");
    CHECK(root_compose != NULL && root_compose->status == OD_PORT_RUNNING);
    CHECK(api_env != NULL && api_env->status == OD_PORT_RUNNING);
    const OdPortRow *external =
        find_dashboard_row(&dashboard, 4000U, "./api/.env");
    const OdPortRow *unknown =
        find_dashboard_row(&dashboard, 4100U, "./api/.env");
    const OdPortRow *mixed =
        find_dashboard_row(&dashboard, 4200U, "./services/.env");
    const OdPortRow *idle =
        find_dashboard_row(&dashboard, 4300U, "./web/.env");
    const OdPortRow *boundary =
        find_dashboard_row(&dashboard, 4400U, "./other/.env");
    CHECK(external != NULL && external->status == OD_PORT_IN_USE_OTHER);
    CHECK(unknown != NULL && unknown->status == OD_PORT_IN_USE_OTHER);
    CHECK(mixed != NULL && mixed->status == OD_PORT_RUNNING);
    CHECK(idle != NULL && idle->status == OD_PORT_NOT_RUNNING);
    CHECK(boundary != NULL && boundary->status == OD_PORT_IN_USE_OTHER);

    const OdPortRow *worker = find_dashboard_row(&dashboard, 5000U, "live");
    const OdPortRow *fallback = find_dashboard_row(&dashboard, 5100U, "live");
    const OdPortRow *representative = find_dashboard_row(&dashboard, 5200U, "live");
    CHECK(worker != NULL && strcmp(worker->relative_folder, "./worker") == 0);
    CHECK(fallback != NULL && strcmp(fallback->relative_folder, "./bin") == 0);
    CHECK(representative != NULL &&
          strcmp(representative->relative_folder, "./alpha") == 0);

    od_dashboard_set_page_size(&dashboard, 2U);
    od_dashboard_scroll(&dashboard, 20);
    CHECK(dashboard.scroll == 8U);
    od_dashboard_scroll(&dashboard, -20);
    CHECK(dashboard.scroll == 0U);
    od_dashboard_free(&dashboard);

    CHECK(unlink(executable) == 0);
    for (size_t index = sizeof(folders) / sizeof(folders[0]); index > 0U; --index) {
        (void)snprintf(path, sizeof(path), "%s/%s", root, folders[index - 1U]);
        CHECK(rmdir(path) == 0);
    }
    CHECK(rmdir(outside) == 0);
    CHECK(rmdir(root) == 0);
}

int main(void) {
    test_proc_socket_parsing_and_deduplication();
    test_socket_inode_target();
    test_occupied_ports_are_unique_and_sorted();
    test_recursive_env_discovery_records_exact_spans();
    test_env_template_and_unrelated_files_are_ignored();
    test_compose_block_scalars_are_not_port_sections();
    test_env_reference_requires_one_direct_assignment();
    test_compose_literals_and_direct_env_references();
    test_compose_rejects_unsupported_or_ambiguous_mappings();
    test_package_scripts_are_manual_only();
    test_makefile_ports_are_manual_only();
    test_dashboard_project_ownership_and_all_declaration_matches();
    if (failures != 0) {
        fprintf(stderr, "%d scan checks failed\n", failures);
        return 1;
    }
    puts("scan checks passed");
    return 0;
}
