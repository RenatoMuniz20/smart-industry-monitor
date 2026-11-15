#ifndef INTERSCITY_QUERY_H
#define INTERSCITY_QUERY_H

#include <stddef.h>

#define ISQ_UUID_LEN 36
#define ISQ_DATE_LEN 40
#define ISQ_TEXT_LEN 32

/* Último valor de uma capability, como veio do InterSCity. */
typedef struct {
    int present;              /* 0 = capability ausente na resposta */
    double number;            /* sim_motor_temperature, sim_current, sim_failure_within_24h */
    char text[ISQ_TEXT_LEN];  /* sim_operating_state */
    char date[ISQ_DATE_LEN];  /* timestamp exatamente como o InterSCity devolve */
} IsqSample;

typedef struct {
    char uuid[ISQ_UUID_LEN + 1];
    IsqSample operating_state;
    IsqSample motor_temperature;
    IsqSample current;
    IsqSample failure_within_24h;
} IsqMachine;

/* POST {base_url}/collector/resources/data/last com {"uuids":[...]}.
 * Preenche out[0..*out_count) na ordem da resposta.
 * Retorna 0 em sucesso; -1 em erro, com a mensagem em `error`.
 * Requer curl_global_init() chamado antes (feito aqui de forma idempotente). */
int interscity_query_last(const char *base_url, long timeout_ms,
                          const char *const *uuids, size_t uuid_count,
                          IsqMachine *out, size_t out_capacity, size_t *out_count,
                          char *error, size_t error_size);

#endif
