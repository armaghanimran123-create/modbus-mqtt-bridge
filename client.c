#include <modbus.h>
#include <mosquitto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>

volatile int running = 1;

void handle_sigint(int sig) {
    running = 0;
    printf("\nShutting down gracefully...\n");
}

typedef struct {
    char modbus_ip[64];
    int modbus_port;
    int register_start;
    int register_count;
    char mqtt_broker[128];
    int mqtt_port;
    char mqtt_topic[256];
    int poll_interval_sec;
} Config;

int load_config(const char *filename, Config *cfg) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "Could not open config file: %s\n", filename);
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char key[128], value[256];
        if (line[0] == '\n' || line[0] == '#') continue;
        if (sscanf(line, "%127[^=]=%255[^\n]", key, value) == 2) {
            if (strcmp(key, "modbus_ip") == 0) strncpy(cfg->modbus_ip, value, sizeof(cfg->modbus_ip));
            else if (strcmp(key, "modbus_port") == 0) cfg->modbus_port = atoi(value);
            else if (strcmp(key, "register_start") == 0) cfg->register_start = atoi(value);
            else if (strcmp(key, "register_count") == 0) cfg->register_count = atoi(value);
            else if (strcmp(key, "mqtt_broker") == 0) strncpy(cfg->mqtt_broker, value, sizeof(cfg->mqtt_broker));
            else if (strcmp(key, "mqtt_port") == 0) cfg->mqtt_port = atoi(value);
            else if (strcmp(key, "mqtt_topic") == 0) strncpy(cfg->mqtt_topic, value, sizeof(cfg->mqtt_topic));
            else if (strcmp(key, "poll_interval_sec") == 0) cfg->poll_interval_sec = atoi(value);
        }
    }
    fclose(f);
    return 0;
}

modbus_t *modbus_reconnect(Config *cfg) {
    modbus_t *ctx = modbus_new_tcp(cfg->modbus_ip, cfg->modbus_port);
    if (ctx == NULL) {
        fprintf(stderr, "Failed to create Modbus context\n");
        return NULL;
    }
    if (modbus_connect(ctx) == -1) {
        fprintf(stderr, "Modbus reconnect failed: %s\n", modbus_strerror(errno));
        modbus_free(ctx);
        return NULL;
    }
    printf("Modbus (re)connected.\n");
    return ctx;
}

int mqtt_reconnect(struct mosquitto *mosq, Config *cfg) {
    // Try the built-in reconnect first (reuses existing connection settings)
    int rc = mosquitto_reconnect(mosq);
    if (rc == MOSQ_ERR_SUCCESS) {
        printf("MQTT (re)connected.\n");
        return 0;
    }

    // If that fails, try a full fresh connect
    fprintf(stderr, "MQTT reconnect failed: %s. Trying fresh connect...\n",
            mosquitto_strerror(rc));
    rc = mosquitto_connect(mosq, cfg->mqtt_broker, cfg->mqtt_port, 60);
    if (rc == MOSQ_ERR_SUCCESS) {
        printf("MQTT (re)connected (fresh).\n");
        return 0;
    }

    fprintf(stderr, "MQTT fresh connect also failed: %s\n", mosquitto_strerror(rc));
    return -1;
}

int main() {
    signal(SIGINT, handle_sigint);

    Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (load_config("config.txt", &cfg) == -1) return -1;

    printf("Loaded config:\n");
    printf("  Modbus: %s:%d (registers %d-%d)\n", cfg.modbus_ip, cfg.modbus_port,
           cfg.register_start, cfg.register_start + cfg.register_count - 1);
    printf("  MQTT:   %s:%d, topic '%s'\n", cfg.mqtt_broker, cfg.mqtt_port, cfg.mqtt_topic);
    printf("  Poll interval: %d sec\n\n", cfg.poll_interval_sec);

    // ---- Initial Modbus connection ----
    modbus_t *ctx = modbus_reconnect(&cfg);
    if (ctx == NULL) return -1;

    // ---- Initial MQTT connection ----
    mosquitto_lib_init();
    struct mosquitto *mosq = mosquitto_new("modbus_client", true, NULL);
    if (!mosq) {
        fprintf(stderr, "Failed to create Mosquitto instance\n");
        modbus_close(ctx);
        modbus_free(ctx);
        mosquitto_lib_cleanup();
        return -1;
    }

    if (mosquitto_connect(mosq, cfg.mqtt_broker, cfg.mqtt_port, 60) != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "Initial MQTT connection failed\n");
        modbus_close(ctx);
        modbus_free(ctx);
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return -1;
    }
    printf("Connected to MQTT broker.\n\n");

    uint16_t tab_reg[64];
    int modbus_failures = 0;
    int mqtt_failures = 0;

    while (running) {
        // ---- Modbus read ----
        int rc = modbus_read_registers(ctx, cfg.register_start, cfg.register_count, tab_reg);
        if (rc == -1) {
            modbus_failures++;
            fprintf(stderr, "Modbus read failed (%d in a row): %s\n",
                    modbus_failures, modbus_strerror(errno));
            modbus_close(ctx);
            modbus_free(ctx);
            printf("Attempting Modbus reconnect...\n");
            ctx = modbus_reconnect(&cfg);
            if (ctx == NULL) {
                fprintf(stderr, "Modbus reconnect failed. Retrying in %d sec.\n",
                        cfg.poll_interval_sec);
            } else {
                modbus_failures = 0;
            }
            sleep(cfg.poll_interval_sec);
            continue;
        }
        modbus_failures = 0;

        // ---- Decode values ----
        float voltage      = tab_reg[0] / 10.0;
        float current      = tab_reg[1] / 100.0;
        float power_factor = tab_reg[2] / 1000.0;
        float frequency    = tab_reg[3] / 100.0;

        char payload[256];
        snprintf(payload, sizeof(payload),
            "{\"voltage\": %.1f, \"current\": %.2f, \"power_factor\": %.3f, \"frequency\": %.2f}",
            voltage, current, power_factor, frequency);

        // ---- MQTT publish ----
        mosquitto_loop(mosq, 100, 1);
        int prc = mosquitto_publish(mosq, NULL, cfg.mqtt_topic,
                                    strlen(payload), payload, 0, false);
        if (prc != MOSQ_ERR_SUCCESS) {
            mqtt_failures++;
            fprintf(stderr, "MQTT publish failed (%d in a row): %s\n",
                    mqtt_failures, mosquitto_strerror(prc));
            printf("Attempting MQTT reconnect...\n");
            if (mqtt_reconnect(mosq, &cfg) == 0) {
                mqtt_failures = 0;
            } else {
                fprintf(stderr, "MQTT reconnect failed. Will retry next cycle.\n");
            }
        } else {
            mqtt_failures = 0;
            printf("Published: %s\n", payload);
        }

        sleep(cfg.poll_interval_sec);
    }

    printf("Closing connections...\n");
    if (ctx) {
        modbus_close(ctx);
        modbus_free(ctx);
    }
    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return 0;
}
