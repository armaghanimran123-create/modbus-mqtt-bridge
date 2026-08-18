#include <modbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

int main() {
    modbus_t *ctx;
    modbus_mapping_t *mb_mapping;

    ctx = modbus_new_tcp("127.0.0.1", 1502);
    if (ctx == NULL) {
        fprintf(stderr, "Failed to create Modbus context\n");
        return -1;
    }

    mb_mapping = modbus_mapping_new(0, 0, 10, 0);
    if (mb_mapping == NULL) {
        fprintf(stderr, "Failed to allocate mapping: %s\n", modbus_strerror(errno));
        modbus_free(ctx);
        return -1;
    }

    int base_voltage = 2300;
    int base_current = 502;
    int base_pf = 998;
    int base_freq = 4995;

    srand(time(NULL));

    int server_socket = modbus_tcp_listen(ctx, 1);
    if (server_socket == -1) {
        fprintf(stderr, "Failed to listen: %s\n", modbus_strerror(errno));
        modbus_free(ctx);
        return -1;
    }

    printf("Modbus simulator running on 127.0.0.1:1502\n");
    printf("Waiting for a client connection...\n");

    modbus_tcp_accept(ctx, &server_socket);
    printf("Client connected!\n");

    while (1) {
        mb_mapping->tab_registers[0] = base_voltage + (rand() % 11 - 5);
        mb_mapping->tab_registers[1] = base_current + (rand() % 21 - 10);
        mb_mapping->tab_registers[2] = base_pf + (rand() % 7 - 3);
        mb_mapping->tab_registers[3] = base_freq + (rand() % 11 - 5);

        uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH];
        int rc = modbus_receive(ctx, query);
        if (rc > 0) {
            modbus_reply(ctx, query, rc, mb_mapping);
        } else if (rc == -1) {
            printf("Client disconnected or error.\n");
            break;
        }
    }

    modbus_mapping_free(mb_mapping);
    close(server_socket);
    modbus_free(ctx);
    return 0;
}
