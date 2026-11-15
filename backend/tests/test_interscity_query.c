/* Driver de teste do cliente de consulta ao InterSCity (SIM-06.2 e 06.4).
 * Recebe a URL base de um InterSCity falso (tests/test_interscity_query.py). */
#include "interscity_query.h"

#include <stdio.h>
#include <string.h>

#define UUID_A "18fa502d-2427-4485-a3a6-0b3f64585634"
#define UUID_B "c29d711f-1f60-44c4-91ea-d72f49343c4e"

static int failures = 0;

#define CHECK(cond, label)                          \
    do {                                            \
        if (cond) {                                 \
            printf("PASS: %s\n", label);            \
        } else {                                    \
            printf("FAIL: %s\n", label);            \
            failures++;                             \
        }                                           \
    } while (0)

int main(int argc, char **argv)
{
    const char *uuids[] = {UUID_A, UUID_B};
    const char *bad_uuids[] = {"x; drop table"};
    IsqMachine out[4];
    IsqHistory h;
    size_t count = 0;
    char route[256];
    char err[256];
    int rc;

    if (argc != 2) {
        fprintf(stderr, "uso: %s <url_base>\n", argv[0]);
        return 2;
    }

    /* 1. Resposta válida: duas máquinas, B sem sim_current. */
    snprintf(route, sizeof route, "%s/ok", argv[1]);
    rc = interscity_query_last(route, 3000, uuids, 2, out, 4, &count, err, sizeof err);
    CHECK(rc == 0 && count == 2, "resposta válida devolve 2 máquinas");
    CHECK(strcmp(out[0].uuid, UUID_A) == 0, "uuid da primeira máquina preservado");
    CHECK(out[0].operating_state.present && strcmp(out[0].operating_state.text, "IDLE") == 0,
          "sim_operating_state texto 'IDLE'");
    CHECK(out[0].motor_temperature.present
          && out[0].motor_temperature.number > 49.5099 && out[0].motor_temperature.number < 49.5101,
          "sim_motor_temperature numérico 49.51");
    CHECK(strcmp(out[0].motor_temperature.date, "2024-01-01T00:21:00.000Z") == 0,
          "timestamp copiado como veio do InterSCity");
    CHECK(out[0].current.present && out[0].current.number > 3.19 && out[0].current.number < 3.21,
          "sim_current numérico 3.2");
    CHECK(out[1].failure_within_24h.present && out[1].failure_within_24h.number == 0.0,
          "zero é lido como valor (failure = 0)");
    CHECK(out[1].current.present == 0, "capability ausente fica present = 0");
    CHECK(strcmp(out[1].operating_state.text, "RUNNING") == 0, "segunda máquina: estado RUNNING");

    /* 2. HTTP 500 -> erro com código. */
    snprintf(route, sizeof route, "%s/http500", argv[1]);
    rc = interscity_query_last(route, 3000, uuids, 2, out, 4, &count, err, sizeof err);
    CHECK(rc == -1 && strstr(err, "HTTP 500") != NULL, "HTTP 500 vira erro com o código");

    /* 3. Corpo que não é JSON -> erro de parse. */
    snprintf(route, sizeof route, "%s/badjson", argv[1]);
    rc = interscity_query_last(route, 3000, uuids, 2, out, 4, &count, err, sizeof err);
    CHECK(rc == -1 && strstr(err, "JSON") != NULL, "corpo inválido vira erro de JSON");

    /* 4. Lista vazia -> sucesso com zero máquinas. */
    snprintf(route, sizeof route, "%s/empty", argv[1]);
    rc = interscity_query_last(route, 3000, uuids, 2, out, 4, &count, err, sizeof err);
    CHECK(rc == 0 && count == 0, "lista vazia devolve 0 máquinas sem erro");

    /* 5. UUID inválido é recusado antes de qualquer envio. */
    rc = interscity_query_last(route, 3000, bad_uuids, 1, out, 4, &count, err, sizeof err);
    CHECK(rc == -1 && strstr(err, "uuid inválido") != NULL, "uuid com caracteres proibidos é recusado");

    /* 6. Servidor fora do ar -> erro de rede. */
    rc = interscity_query_last("http://127.0.0.1:1/ok", 3000, uuids, 2, out, 4, &count, err, sizeof err);
    CHECK(rc == -1 && strstr(err, "falha de rede") != NULL, "conexão recusada vira falha de rede");

    /* 7. Histórico: duplicatas removidas e ordem cronológica. */
    snprintf(route, sizeof route, "%s/hist", argv[1]);
    rc = interscity_query_history(route, 3000, UUID_A, "2024-01-01T00:00:00",
                                  "2024-01-01T01:00:00", &h, err, sizeof err);
    CHECK(rc == 0, "histórico válido sem erro");
    CHECK(h.motor_temperature.count == 2, "4 registros repetidos viram 2 timestamps");
    CHECK(h.motor_temperature.count == 2
          && strcmp(h.motor_temperature.items[0].date, "2024-01-01T00:24:00.000Z") == 0
          && h.motor_temperature.items[0].number > 49.07 && h.motor_temperature.items[0].number < 49.09,
          "primeiro ponto: 00:24 com 49.08");
    CHECK(h.motor_temperature.count == 2
          && strcmp(h.motor_temperature.items[1].date, "2024-01-01T00:43:00.000Z") == 0
          && h.motor_temperature.items[1].number > 42.00 && h.motor_temperature.items[1].number < 42.02,
          "segundo ponto: 00:43 com 42.01");
    CHECK(h.operating_state.count == 1 && strcmp(h.operating_state.items[0].text, "IDLE") == 0,
          "estado textual preservado no histórico");
    CHECK(h.current.count == 0, "capability ausente vira série vazia");
    interscity_history_free(&h);

    /* 8. 404 'Resource not found' significa sem dados. */
    snprintf(route, sizeof route, "%s/histnone", argv[1]);
    rc = interscity_query_history(route, 3000, UUID_A, "2024-01-01T00:00:00",
                                  "2024-01-01T01:00:00", &h, err, sizeof err);
    CHECK(rc == 0 && h.motor_temperature.count == 0, "404 'Resource not found' vira histórico vazio");
    interscity_history_free(&h);

    /* 9. Data fora do formato é recusada antes do envio. */
    rc = interscity_query_history(route, 3000, UUID_A, "2024-1-1",
                                  "2024-01-01T01:00:00", &h, err, sizeof err);
    CHECK(rc == -1 && strstr(err, "formato") != NULL, "data fora do formato é recusada");

    /* 10. Início posterior ao fim é recusado. */
    rc = interscity_query_history(route, 3000, UUID_A, "2024-01-01T02:00:00",
                                  "2024-01-01T01:00:00", &h, err, sizeof err);
    CHECK(rc == -1 && strstr(err, "posterior") != NULL, "início posterior ao fim é recusado");

    if (failures > 0) {
        printf("test_interscity_query: %d falha(s).\n", failures);
        return 1;
    }
    printf("test_interscity_query: todos os testes passaram.\n");
    return 0;
}
