#define _POSIX_C_SOURCE 200809L
#include "mqtt_subscriber.h"

#include <mosquitto.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

struct MqttSubscriber {
    struct mosquitto *client;
    MqttMessageHandler handler;
    void *context;
    int connected;
    int ready;
    int subscription_mid;
    int closing;
    MqttSubscriberError failure;
};

static int active;
static MqttSubscriberError last_error;

static int64_t milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int error_result(MqttSubscriberError error)
{
    last_error = error;
    return error == MQTT_SUBSCRIBER_OK ? 0 : -1;
}

static void on_connect(struct mosquitto *client, void *context, int code)
{
    MqttSubscriber *s = context;
    (void)client;
    s->connected = code == 0;
    if (code != 0) s->failure = MQTT_SUBSCRIBER_CONNACK;
}

static void on_subscribe(struct mosquitto *client, void *context, int mid,
                          int count, const int *granted)
{
    MqttSubscriber *s = context;
    (void)client;
    if (mid != s->subscription_mid || count != 1 || granted == NULL || granted[0] != 1) {
        s->failure = MQTT_SUBSCRIBER_SUBACK;
        return;
    }
    s->ready = 1;
}

static void on_disconnect(struct mosquitto *client, void *context, int code)
{
    MqttSubscriber *s = context;
    (void)client;
    (void)code;
    s->connected = 0;
    if (!s->closing && s->failure == MQTT_SUBSCRIBER_OK) {
        s->failure = MQTT_SUBSCRIBER_DISCONNECTED;
    }
}

static void on_message(struct mosquitto *client, void *context,
                        const struct mosquitto_message *message)
{
    MqttSubscriber *s = context;
    (void)client;
    if (s->failure == MQTT_SUBSCRIBER_OK && !s->closing) {
        s->handler(message->topic, message->payload, (size_t)message->payloadlen, s->context);
    }
}

static int process_events(MqttSubscriber *s, int timeout_ms)
{
    if (s->failure != MQTT_SUBSCRIBER_OK) return -1;
    int result = mosquitto_loop(s->client, timeout_ms, 1);
    /* Prefer the more specific callback diagnostic over the loop's result. */
    if (result != MOSQ_ERR_SUCCESS && s->failure == MQTT_SUBSCRIBER_OK) {
        s->failure = MQTT_SUBSCRIBER_NETWORK;
    }
    return s->failure == MQTT_SUBSCRIBER_OK ? 0 : -1;
}

static int wait_for(MqttSubscriber *s, const int *done, int64_t deadline)
{
    while (!*done && s->failure == MQTT_SUBSCRIBER_OK) {
        int64_t now = milliseconds();
        if (now < 0) {
            s->failure = MQTT_SUBSCRIBER_CLOCK;
            break;
        }
        if (now >= deadline) {
            s->failure = MQTT_SUBSCRIBER_TIMEOUT;
            break;
        }
        int remaining = (int)(deadline - now);
        if (process_events(s, remaining < 100 ? remaining : 100) != 0) break;
    }
    return s->failure == MQTT_SUBSCRIBER_OK && *done ? 0 : -1;
}

int mqtt_subscriber_close(MqttSubscriber **subscriber)
{
    if (subscriber == NULL || *subscriber == NULL) return error_result(MQTT_SUBSCRIBER_OK);
    MqttSubscriber *s = *subscriber;
    MqttSubscriberError result = MQTT_SUBSCRIBER_OK;
    s->closing = 1;
    if (s->connected && s->failure == MQTT_SUBSCRIBER_OK) {
        if (mosquitto_disconnect(s->client) != MOSQ_ERR_SUCCESS) {
            result = MQTT_SUBSCRIBER_CLOSE;
        } else {
            int rc = mosquitto_loop(s->client, 0, 1);
            if (rc != MOSQ_ERR_SUCCESS && rc != MOSQ_ERR_NO_CONN) result = MQTT_SUBSCRIBER_CLOSE;
        }
    }
    mosquitto_destroy(s->client);
    free(s);
    *subscriber = NULL;
    if (mosquitto_lib_cleanup() != MOSQ_ERR_SUCCESS) result = MQTT_SUBSCRIBER_CLOSE;
    active = 0;
    return error_result(result);
}

int mqtt_subscriber_connect(MqttSubscriber **out, const char *host, int port,
                            const char *client_id, int timeout_ms,
                            MqttMessageHandler handler, void *context)
{
    if (out == NULL || *out != NULL || host == NULL || *host == '\0' ||
        client_id == NULL || *client_id == '\0' || handler == NULL ||
        port < 1 || port > 65535 || timeout_ms < 1 || timeout_ms > 5000) {
        return error_result(MQTT_SUBSCRIBER_ARGUMENT);
    }
    if (active) return error_result(MQTT_SUBSCRIBER_ACTIVE);
    int64_t start = milliseconds();
    if (start < 0) return error_result(MQTT_SUBSCRIBER_CLOCK);
    if (mosquitto_lib_init() != MOSQ_ERR_SUCCESS) return error_result(MQTT_SUBSCRIBER_LIBRARY);
    MqttSubscriber *s = calloc(1, sizeof *s);
    if (s == NULL) {
        mosquitto_lib_cleanup();
        return error_result(MQTT_SUBSCRIBER_MEMORY);
    }
    active = 1;
    s->handler = handler;
    s->context = context;
    s->client = mosquitto_new(client_id, true, s);
    if (s->client == NULL) {
        free(s);
        mosquitto_lib_cleanup();
        active = 0;
        return error_result(MQTT_SUBSCRIBER_MEMORY);
    }
    mosquitto_connect_callback_set(s->client, on_connect);
    mosquitto_subscribe_callback_set(s->client, on_subscribe);
    mosquitto_disconnect_callback_set(s->client, on_disconnect);
    mosquitto_message_callback_set(s->client, on_message);
    if (mosquitto_int_option(s->client, MOSQ_OPT_PROTOCOL_VERSION, MQTT_PROTOCOL_V311)
        != MOSQ_ERR_SUCCESS) s->failure = MQTT_SUBSCRIBER_LIBRARY;
    else if (mosquitto_connect(s->client, host, port, 60) != MOSQ_ERR_SUCCESS)
        s->failure = MQTT_SUBSCRIBER_CONNECT;
    else if (wait_for(s, &s->connected, start + timeout_ms) == 0) {
        if (mosquitto_subscribe(s->client, &s->subscription_mid,
                                "smart-industry/machines/+/telemetry", 1) != MOSQ_ERR_SUCCESS)
            s->failure = MQTT_SUBSCRIBER_SUBSCRIBE;
        else wait_for(s, &s->ready, start + timeout_ms);
    }
    if (s->failure != MQTT_SUBSCRIBER_OK) {
        MqttSubscriberError failure = s->failure;
        mqtt_subscriber_close(&s);
        return error_result(failure);
    }
    *out = s;
    return error_result(MQTT_SUBSCRIBER_OK);
}

int mqtt_subscriber_poll(MqttSubscriber *s, int timeout_ms)
{
    if (s == NULL || timeout_ms < 0 || timeout_ms > 5000)
        return error_result(MQTT_SUBSCRIBER_ARGUMENT);
    process_events(s, timeout_ms);
    return error_result(s->failure);
}

MqttSubscriberError mqtt_subscriber_last_error(void)
{
    return last_error;
}

const char *mqtt_subscriber_error_string(MqttSubscriberError error)
{
    switch (error) {
    case MQTT_SUBSCRIBER_OK: return "Sucesso";
    case MQTT_SUBSCRIBER_ARGUMENT: return "Argumento inválido";
    case MQTT_SUBSCRIBER_ACTIVE: return "Já existe um assinante ativo";
    case MQTT_SUBSCRIBER_MEMORY: return "Memória indisponível";
    case MQTT_SUBSCRIBER_LIBRARY: return "Erro ao inicializar/configurar libmosquitto";
    case MQTT_SUBSCRIBER_CLOCK: return "Erro no relógio monotônico";
    case MQTT_SUBSCRIBER_CONNECT: return "Falha na conexão DNS/TCP";
    case MQTT_SUBSCRIBER_CONNACK: return "Conexão recusada no CONNACK";
    case MQTT_SUBSCRIBER_SUBSCRIBE: return "Falha ao enviar SUBSCRIBE";
    case MQTT_SUBSCRIBER_SUBACK: return "SUBACK com MID ou QoS inválido";
    case MQTT_SUBSCRIBER_TIMEOUT: return "Timeout aguardando CONNACK/SUBACK";
    case MQTT_SUBSCRIBER_NETWORK: return "Erro no processamento de rede";
    case MQTT_SUBSCRIBER_DISCONNECTED: return "Conexão interrompida";
    case MQTT_SUBSCRIBER_CLOSE: return "Erro ao encerrar conexão MQTT";
    default: return "Erro desconhecido";
    }
}
