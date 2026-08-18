#include <modbus.h>
#include <mosquitto.h>
#include <stdio.h>

int main() {
    printf("libmodbus and libmosquitto linked successfully!\n");
    printf("Modbus library loaded.\n");
    mosquitto_lib_init();
    printf("Mosquitto library initialized.\n");
    mosquitto_lib_cleanup();
    return 0;
}

