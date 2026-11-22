#ifndef INTERSCITY_QUERY_H
#define INTERSCITY_QUERY_H

#include <stddef.h>

#define ISQ_UUID_LEN 36
#define ISQ_DATE_LEN 40
#define ISQ_TEXT_LEN 32

/* Último valor de uma capability, como veio do InterSCity. */
typedef struct {
    int present;              /* 0 = capability ausente na resposta */
    double number;            /* sim_motor_temperature, sim_current, sim_vibration_rms,
                                 sim_rpm, sim_failure_within_24h */
    char text[ISQ_TEXT_LEN];  /* sim_operating_state */
    char date[ISQ_DATE_LEN];  /* timestamp exatamente como o InterSCity devolve */
} IsqSample;

typedef struct {
    char uuid[ISQ_UUID_LEN + 1];
    IsqSample operating_state;
    IsqSample motor_temperature;
    IsqSample current;
    IsqSample vibration_rms;  /* SIM-11 */
    IsqSample rpm;            /* SIM-11 */
    IsqSample failure_within_24h;
} IsqMachine;

/* Um ponto de uma série histórica. Usa `text` ou `number` conforme a série. */
typedef struct {
    char date[ISQ_DATE_LEN];
    char text[ISQ_TEXT_LEN];
    double number;
} IsqPoint;

typedef struct {
    IsqPoint *items;
    size_t count;
    size_t capacity;
} IsqSeries;

typedef struct {
    IsqSeries operating_state;
    IsqSeries motor_temperature;
    IsqSeries current;
    IsqSeries vibration_rms;  /* SIM-11 */
    IsqSeries rpm;            /* SIM-11 */
    IsqSeries failure_within_24h;
} IsqHistory;

/* POST {base_url}/collector/resources/data/last com {"uuids":[...]}.
 * Preenche out[0..*out_count) na ordem da resposta.
 * 404 "Resource not found" (nenhum recurso com dados) é sucesso com *out_count = 0.
 * Retorna 0 em sucesso; -1 em erro, com a mensagem em `error`. */
int interscity_query_last(const char *base_url, long timeout_ms,
                          const char *const *uuids, size_t uuid_count,
                          IsqMachine *out, size_t out_capacity, size_t *out_count,
                          char *error, size_t error_size);

/* Verifica o formato AAAA-MM-DDTHH:MM:SS (UTC, sem fuso). */
int interscity_datetime_is_valid(const char *s);

/* POST {base_url}/collector/resources/{uuid}/data com start_date/end_date.
 * Séries ordenadas por data e sem timestamps repetidos. Resposta
 * 404 "Resource not found" significa sem dados e devolve séries vazias.
 * Retorna 0 em sucesso; -1 em erro. Libere com interscity_history_free(). */
int interscity_query_history(const char *base_url, long timeout_ms, const char *uuid,
                             const char *start, const char *end, IsqHistory *out,
                             char *error, size_t error_size);

void interscity_history_free(IsqHistory *history);

#endif
