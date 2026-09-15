#include "Module.h"
#include "Moden.h"
#include "Integration.h"
#include <iostream>


//Controle
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

#define I2C_ADDR 0x12

int i2c_open() {
    int fd = open("/dev/i2c-1", O_RDWR);
    if (fd < 0) {
        perror("Erro ao abrir /dev/i2c-1");
        exit(1);
    }

    if (ioctl(fd, I2C_SLAVE, I2C_ADDR) < 0) {
        perror("Erro ao configurar I2C_SLAVE");
        exit(1);
    }
    return fd;
}


bool talking = false;


unsigned long int communication_timeout;
const unsigned long int communication_timeout_limit = 1000;

SatPacket satPacket;
GSPacket gsPacket;
HealthStatus hStatus;
control controlD;
uint8_t rx_pointer = 0;

void sendSatPacket() {
    tx_send((uint8_t*)&satPacket, satPacket.length);
    std::cout << "Protocol: " << satPacket.protocol << "\n";
    std::cout << "Operation: " << satPacket.operation << "\n";
}

void sendHealthStatus() {
    tx_send((uint8_t*)&hStatus, hStatus.length);
    std::cout << "Protocol: " << hStatus.protocol << "\n";
    std::cout << "Operation: " << hStatus.operation << "\n";
    std::cout << "Number of Packages: " << (int)hStatus.numberOfPackages << "\n";
}

void updateRFComm() {
    std::cout << "\n Waiting for packet. \n";

    uint8_t b;
    if (modemAvailable()) {
        while (modemAvailable()) {
            b = modemRead();
            ((uint8_t*)(&gsPacket))[rx_pointer++] = b; //transforma a struct em array de bytes e vai colocando cada byte no seu lugar.

            if (rx_pointer > 0 && rx_pointer == gsPacket.length) {
                std::cout << "Packet received from the ground station: ";
                for (int i = 0; i < gsPacket.length; i++) {
                    std::cout << (int)((uint8_t*)&gsPacket)[i] << " ";
                }
                std::cout << std::endl;
                onReceive();
                rx_pointer = 0;
            }

            if (rx_pointer > sizeof(gsPacket)) {
                rx_pointer = 0;
            }
        }

        //communication_timeout = millis() + communication_timeout_limit;
    }
}


//Essa função decide o que fazer de acordo com o comando recebido
void onReceive() {
    std::cout << "Protocol processing:" << std::endl;
    std::cout << "\nProtocol:" << (int)gsPacket.protocol << std::endl;
    std::cout << "\nOperation:" << (int)gsPacket.operation << std::endl;
    
    switch (gsPacket.protocol) {
        case HEALTH_PROTOCOL:
            std::cout << "\nHEALTHPROTOCOL\n" << std::endl;
            switchHealthProtocol();
            break;
        case AIS_PROTOCOL:
            std::cout << "\nAISPROTOCOL\n" << std::endl;
            switchAISProtocol();
            break;
        case MEC_PROTOCOL:
            std::cout << "\nMECPROTOCOL\n" << std::endl;
            switchMECProtocol();
            break;
        case IMAGING_PROTOCOL:
            std::cout << "\nIMAGINGPROTOCOL\n" << std::endl;
            switchImagingProtocol();
            break;
        case CONTROL_PROTOCOL:
            std::cout << "\nCONTROLPROTOCOL\n" << std::endl;
            switchControlProtocol();
            break;
        case STATUS_PROTOCOL:
            std::cout << "\nSTATUSPROTOCOL\n" << std::endl;
            switchStatusProtocol();
            break;
        case TERM_PROTOCOL:
            std::cout << "\nTERMPROTOCOL\n" << std::endl;
            switchThermalProtocol();
            break;
        default:
        // ignora
        break;
    }

}

void switchHealthProtocol() {
    switch (gsPacket.operation) {
        case GENERATE_HEALTH_DATA:
            std::cout << "GENERATE_HEALTH_DATA" << std::endl;
            generateHealthData();
            hStatus.length = sizeof(HealthStatus);
            hStatus.protocol = HEALTH_PROTOCOL;
            hStatus.operation = GENERATE_HEALTH_DATA;
            hStatus.numberOfPackages = HealthDataCounter;
            sendHealthStatus();
            sleep(10);
            break;

        case RESEND_HEALTH_DATA:
            std::cout << "RESEND HDATA" << std::endl;
            sleep(10);
            break;
        case CONFIRMATION_HEALTH_DATA:
            std::cout << "CHEGOU A CONFIRMACAO" << std::endl;
            sendHealthData();
            sleep(10);
            break;
        default:
        // ignora
        break;
    }
}

void switchAISProtocol(){
    
}

void switchMECProtocol(){
    switch(gsPacket.operation){
        case OPEN_ANTENNAS:
            break;
        default:
        // ignora
            break;
    }

}

void switchImagingProtocol(){

}
void switchControlProtocol(){
    uint16_t vetor1 = 0;
    uint16_t vetor2 = 0;

    switch(gsPacket.operation){

        case SOLAR_VECTOR: {
            std::cout << "\nSOLAR_VECTOR\n" << std::endl;

            int fd = i2c_open();

            uint16_t cmd = '1';
            write(fd, &cmd, 1);

            int result;
            read(fd, &result, sizeof(int));

            printf("Retorno do vetor solar: %d\n", result);

            close(fd);
            sleep(10);
            break;
        }

        case TWO_VECTORS: {
            std::cout << "\nTWO_VECTORS\n" << std::endl;

            std::cout << "Vetor 1: " << (int)gsPacket.vector1 << "\n";
            std::cout << "Vetor 1b: " << (int)gsPacket.vector1b << "\n";
            std::cout << "Vetor 2: " << (int)gsPacket.vector2 << "\n";
            std::cout << "Vetor 2b: " << (int)gsPacket.vector2b << "\n";

            vetor1 = ((uint16_t)gsPacket.vector1b << 8) | gsPacket.vector1;
            vetor2 = ((uint16_t)gsPacket.vector2b << 8) | gsPacket.vector2;

            std::cout << "Vetor 1 completo: " << vetor1 << "\n";
            std::cout << "Vetor 2 completo: " << vetor2 << "\n";

            int fd = i2c_open();

            uint8_t buf[5];
            buf[0] = '2';
            buf[1] = vetor1 & 0xFF;
            buf[2] = vetor1 >> 8;
            buf[3] = vetor2 & 0xFF;
            buf[4] = vetor2 >> 8;

            int w = write(fd, buf, 5);
            if (w != 5) {
                printf("Erro no envio I2C! Enviou %d bytes.\n", w);
            }

            close(fd);
            sleep(10);
            break;
        }

        case SUN_POINTING: {
            std::cout << "\nSUN_POINTING\n" << std::endl;

            int fd = i2c_open();
            uint16_t cmd = '3';
            write(fd, &cmd, 1);

            printf("Comando 3 enviado.\n");

            close(fd);
            sleep(10);
            break;
        }

        case STABILIZATION: {
            std::cout << "\nSTABILIZATION\n" << std::endl;

            int fd = i2c_open();
            uint16_t cmd = '4';
            write(fd, &cmd, 1);

            printf("Comando 4 enviado.\n");

            close(fd);
            sleep(10);
            break;
        }

        default:
            break;
    }
}


void switchStatusProtocol(){
    switch(gsPacket.operation){
        case INITCOMM:
            std::cout << "\nINITCOMM\n" << std::endl;
            satPacket.length = sizeof(SatPacket);
            satPacket.protocol = STATUS_PROTOCOL;
            satPacket.operation = INITCOMM;
            sendSatPacket();
            sleep(10);
            break;
        case VERIFY_FILE:
            std::cout << "\nVERIFY_FILE\n" << std::endl;
            verifyFile();
            break;
        case SHUT_DOWN_SYSTEM:
            std::cout << "\nSHUT DOWN SYSTEM\n" << std::endl;
            ShutDownSystem();
            break;
        default:
        // ignora
        break;
    }

}

void switchThermalProtocol(){
    switch(gsPacket.operation){
        case ACTIVATE_THERMAL_CONTROL:
            std::cout << "\nACTIVATE THERMAL CONTROL\n" << std::endl;
            activateThermalControl();
            break;
        case DEACTIVATE_THERMAL_CONTROL:
            std::cout << "\nDEACTIVATE THERMAL CONTROL\n" << std::endl;
            deactivateThermalControl();
            break;
        default:
        // ignora
        break;
    }
}