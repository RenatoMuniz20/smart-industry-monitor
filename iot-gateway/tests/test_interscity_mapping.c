#define _POSIX_C_SOURCE 200809L
#include "interscity_mapping.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UUID_1 "18fa502d-2427-4485-a3a6-0b3f64585634"
#define UUID_11 "a1ef2f0a-648f-4e30-9de3-cf39bfb6128c"

/* Large structures are static to keep the test stack small. */
static InterscityMapping mapping;
static InterscityMapping before;

static void write_temp(const char *content, char path[64])
{
    strcpy(path, "/tmp/sim-mapping-XXXXXX");
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *file = fdopen(fd, "w");
    assert(file != NULL);
    assert(fputs(content, file) >= 0);
    assert(fclose(file) == 0);
}

static void expect_error(const char *content, InterscityMappingError expected, size_t expected_line)
{
    char path[64];
    write_temp(content, path);
    memset(&mapping, 0xA5, sizeof mapping);
    memcpy(&before, &mapping, sizeof mapping);
    InterscityMappingError error = INTERSCITY_MAPPING_OK;
    size_t line = 999;
    assert(interscity_mapping_load(path, &mapping, &error, &line) == -1);
    assert(error == expected);
    assert(line == expected_line);
    assert(memcmp(&mapping, &before, sizeof mapping) == 0); /* out unchanged */
    assert(remove(path) == 0);
}

static void test_valid_format(void)
{
    char path[64];
    write_temp("# comentário\n"
               "\n"
               "   \t \n"
               "   # comentário indentado\n"
               "1 " UUID_1 "\n"
               "\t11\t " UUID_11 "  \n"
               "2147483647 00000000-0000-4000-8000-000000000000", /* no final newline */
               path);
    InterscityMappingError error = INTERSCITY_MAPPING_UUID;
    size_t line = 999;
    assert(interscity_mapping_load(path, &mapping, &error, &line) == 0);
    assert(error == INTERSCITY_MAPPING_OK && line == 0);
    assert(mapping.count == 3);
    assert(strcmp(interscity_mapping_lookup(&mapping, 1), UUID_1) == 0);
    assert(strcmp(interscity_mapping_lookup(&mapping, 11), UUID_11) == 0);
    assert(interscity_mapping_lookup(&mapping, 2147483647) != NULL);
    assert(interscity_mapping_lookup(&mapping, 2) == NULL);
    assert(interscity_mapping_lookup(&mapping, 0) == NULL);
    assert(interscity_mapping_lookup(NULL, 1) == NULL);
    /* error and error_line are optional. */
    assert(interscity_mapping_load(path, &mapping, NULL, NULL) == 0);
    assert(remove(path) == 0);
}

static void test_repository_file(void)
{
    const char *path = getenv("SIM_MAPPING_FILE");
    if (path == NULL) path = "../config/interscity/machines.conf";
    static const int ids[] = {1, 2, 3, 6, 7, 8, 9, 11, 12, 13};
    InterscityMappingError error;
    size_t line;
    assert(interscity_mapping_load(path, &mapping, &error, &line) == 0);
    assert(mapping.count == sizeof ids / sizeof ids[0]);
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; ++i) {
        assert(interscity_mapping_lookup(&mapping, ids[i]) != NULL);
    }
    assert(strcmp(interscity_mapping_lookup(&mapping, 1), UUID_1) == 0);
    assert(interscity_mapping_lookup(&mapping, 4) == NULL);
}

static void test_invalid_lines(void)
{
    expect_error("1 " UUID_1 "\r\n", INTERSCITY_MAPPING_CR, 1);
    expect_error("# ok\n1 18FA502D-2427-4485-A3A6-0B3F64585634\n", INTERSCITY_MAPPING_UUID, 2);
    expect_error("1 18fa502d24274485a3a60b3f64585634\n", INTERSCITY_MAPPING_UUID, 1);
    expect_error("1 18fa502d-2427-4485-a3a6-0b3f6458563g\n", INTERSCITY_MAPPING_UUID, 1);
    expect_error("1 18fa502d-2427-4485-a3a6-0b3f645856345\n", INTERSCITY_MAPPING_UUID, 1);
    expect_error("0 " UUID_1 "\n", INTERSCITY_MAPPING_MACHINE_ID, 1);
    expect_error("-1 " UUID_1 "\n", INTERSCITY_MAPPING_MACHINE_ID, 1);
    expect_error("abc " UUID_1 "\n", INTERSCITY_MAPPING_MACHINE_ID, 1);
    expect_error("2147483648 " UUID_1 "\n", INTERSCITY_MAPPING_MACHINE_ID, 1);
    expect_error("1\n", INTERSCITY_MAPPING_SYNTAX, 1);
    expect_error("1 " UUID_1 " extra\n", INTERSCITY_MAPPING_SYNTAX, 1);
    expect_error("1 " UUID_1 " # comentário\n", INTERSCITY_MAPPING_SYNTAX, 1);
    expect_error("1 " UUID_1 "\n11 " UUID_11 "\n1 " UUID_11 "\n", INTERSCITY_MAPPING_DUPLICATE_ID, 3);
    expect_error("1 " UUID_1 "\n01 " UUID_11 "\n", INTERSCITY_MAPPING_DUPLICATE_ID, 2);
    expect_error("1 " UUID_1 "\n2 " UUID_1 "\n", INTERSCITY_MAPPING_DUPLICATE_UUID, 2);
    expect_error("", INTERSCITY_MAPPING_EMPTY, 0);
    expect_error("# só comentários\n\n", INTERSCITY_MAPPING_EMPTY, 0);

    char long_line[400];
    memset(long_line, 'x', 300);
    strcpy(long_line + 300, "\n");
    expect_error(long_line, INTERSCITY_MAPPING_LINE_TOO_LONG, 1);
}

static void test_capacity(void)
{
    static char content[(INTERSCITY_MAPPING_CAPACITY + 1) * 64];
    size_t used = 0;
    for (int i = 1; i <= INTERSCITY_MAPPING_CAPACITY + 1; ++i) {
        int written = snprintf(content + used, sizeof content - used,
                               "%d %08x-0000-4000-8000-%012x\n", i, (unsigned)i, (unsigned)i);
        assert(written > 0 && (size_t)written < sizeof content - used);
        used += (size_t)written;
    }
    expect_error(content, INTERSCITY_MAPPING_TOO_MANY, INTERSCITY_MAPPING_CAPACITY + 1);
}

static void test_arguments_and_open(void)
{
    InterscityMappingError error;
    size_t line = 999;
    assert(interscity_mapping_load(NULL, &mapping, &error, &line) == -1);
    assert(error == INTERSCITY_MAPPING_ARGUMENT && line == 0);
    assert(interscity_mapping_load("/tmp", NULL, &error, NULL) == -1);
    assert(error == INTERSCITY_MAPPING_ARGUMENT);

    memset(&mapping, 0x5A, sizeof mapping);
    memcpy(&before, &mapping, sizeof mapping);
    assert(interscity_mapping_load("/nonexistent/sim/machines.conf", &mapping, &error, &line) == -1);
    assert(error == INTERSCITY_MAPPING_OPEN && line == 0);
    assert(memcmp(&mapping, &before, sizeof mapping) == 0);
}

static void test_error_strings(void)
{
    for (int e = INTERSCITY_MAPPING_OK; e <= INTERSCITY_MAPPING_EMPTY; ++e) {
        const char *text = interscity_mapping_error_string((InterscityMappingError)e);
        assert(text != NULL && *text != '\0');
        assert(strcmp(text, "Erro desconhecido") != 0);
    }
    assert(strcmp(interscity_mapping_error_string((InterscityMappingError)999), "Erro desconhecido") == 0);
}

int main(void)
{
    test_valid_format();
    test_repository_file();
    test_invalid_lines();
    test_capacity();
    test_arguments_and_open();
    test_error_strings();
    puts("test_interscity_mapping: todos os testes passaram.");
    return 0;
}
