#include <modbus.h>
#include <mosquitto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <stdarg.h>

volatile int running = 1;

// ---- Stats ----
int total_publishes = 0;
int modbus_failures = 0;
int mqtt_failures = 0;
int uptime_cycles = 0;

// ---- Config ----
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

// ---- Logging ----
void get_timestamp(char *buf, size_t len) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", t);
}

void get_date(char *buf, size_t len) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buf, len, "%Y-%m-%d", t);
}

void log_event(const char *message) {
    // Print to console
    char ts[32];
    get_timestamp(ts, sizeof(ts));
    printf("[%s] %s\n", ts, message);

    // Write to logs/basic/YYYY-MM-DD.log
    char date[16];
    get_date(date, sizeof(date));

    char path[128];
    snprintf(path, sizeof(path), "logs/basic/%s.log", date);

    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "[%s] %s\n", ts, message);
        fclose(f);
    }
}

void log_event_fmt(const char *fmt, ...) {
    char message[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    log_event(message);
}

void write_stats() {
    FILE *f = fopen("logs/stats/stats.log", "w");
    if (f) {
        fprintf(f, "total_publishes=%d\n", total_publishes);
        fprintf(f, "modbus_failures=%d\n", modbus_failures);
        fprintf(f, "mqtt_failures=%d\n", mqtt_failures);
        fprintf(f, "uptime_cycles=%d\n", uptime_cycles);
        fclose(f);
    }
}

void handle_sigint(int sig) {
    running = 0;
    log_event("Shutdown requested (Ctrl+C)");
    write_stats();
}

// ---- Config loader ----
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

// ---- Modbus reconnect ----
modbus_t *modbus_reconnect(Config *cfg) {
    modbus_t *ctx = modbus_new_tcp(cfg->modbus_ip, cfg->modbus_port);
    if (ctx == NULL) {
        log_event("Failed to create Modbus context");
        return NULL;
    }
    if (modbus_connect(ctx) == -1) {
        log_event_fmt("Modbus reconnect failed: %s", modbus_strerror(errno));
        modbus_free(ctx);
        return NULL;
    }
    log_event("Modbus (re)connected");
    return ctx;
}

// ---- MQTT reconnect ----
int mqtt_reconnect(struct mosquitto *mosq, Config *cfg) {
    int rc = mosquitto_reconnect(mosq);
    if (rc == MOSQ_ERR_SUCCESS) {
        log_event("MQTT (re)connected");
        return 0;
    }
    log_event_fmt("MQTT reconnect failed: %s. Trying fresh connect...",
                  mosquitto_strerror(rc));
    rc = mosquitto_connect(mosq, cfg->mqtt_broker, cfg->mqtt_port, 60);
    if (rc == MOSQ_ERR_SUCCESS) {
        log_event("MQTT (re)connected (fresh)");
        return 0;
    }
    log_event_fmt("MQTT fresh connect also failed: %s", mosquitto_strerror(rc));
    return -1;
}

int main() {
    signal(SIGINT, handle_sigint);

    // Create log directories
    mkdir("logs", 0755);
    mkdir("logs/basic", 0755);
    mkdir("logs/stats", 0755);

    Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (load_config("config.txt", &cfg) == -1) return -1;

    log_event("Starting Modbus-MQTT Bridge");
    log_event_fmt("Config: Modbus %s:%d (registers %d-%d), MQTT %s:%d, topic '%s', poll %dsec",
        cfg.modbus_ip, cfg.modbus_port,
        cfg.register_start, cfg.register_start + cfg.register_count - 1,
        cfg.mqtt_broker, cfg.mqtt_port, cfg.mqtt_topic,
        cfg.poll_interval_sec);

    // ---- Initial Modbus connection ----
    modbus_t *ctx = modbus_reconnect(&cfg);
    if (ctx == NULL) return -1;

    // ---- Initial MQTT connection ----
    mosquitto_lib_init();
    struct mosquitto *mosq = mosquitto_new("modbus_client", true, NULL);
    if (!mosq) {
        log_event("Failed to create Mosquitto instance");
        modbus_close(ctx);
        modbus_free(ctx);
        mosquitto_lib_cleanup();
        return -1;
    }

    if (mosquitto_connect(mosq, cfg.mqtt_broker, cfg.mqtt_port, 60) != MOSQ_ERR_SUCCESS) {
        log_event("Initial MQTT connection failed");
        modbus_close(ctx);
        modbus_free(ctx);
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return -1;
    }
    log_event("MQTT connected");

    // Initialize stats file
    write_stats();

    uint16_t tab_reg[64];

    while (running) {
        uptime_cycles++;

        // ---- Modbus read ----
        int rc = modbus_read_registers(ctx, cfg.register_start,
                                        cfg.register_count, tab_reg);
        if (rc == -1) {
            modbus_failures++;
            log_event_fmt("Modbus read failed (%d in a row): %s",
                          modbus_failures, modbus_strerror(errno));
            modbus_close(ctx);
            modbus_free(ctx);
            ctx = modbus_reconnect(&cfg);
            if (ctx == NULL) {
                log_event_fmt("Modbus reconnect failed. Retrying in %d sec.",
                              cfg.poll_interval_sec);
            } else {
                modbus_failures = 0;
            }
            write_stats();
            sleep(cfg.poll_interval_sec);
            continue;
        }

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
            log_event_fmt("MQTT publish failed (%d in a row): %s",
                          mqtt_failures, mosquitto_strerror(prc));
            if (mqtt_reconnect(mosq, &cfg) == 0) {
                mqtt_failures = 0;
            }
        } else {
            total_publishes++;
            printf("[Published #%d] %s\n", total_publishes, payload);
        }

        // Write stats every cycle
        write_stats();
        sleep(cfg.poll_interval_sec);
    }

    log_event("Shutting down gracefully");
    if (ctx) {
        modbus_close(ctx);
        modbus_free(ctx);
    }
    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return 0;
}
