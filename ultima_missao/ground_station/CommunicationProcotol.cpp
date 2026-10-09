#include "CommunicationProtocol.h"
#include <LoRa.h>

GSPacket packet;
SatPacket resposta;
control controlPacket;
HealthStatus Hstatus;
healthData health;

// Função para limpar o buffer
void clearSerial() {
    while (Serial.available() > 0) {
        Serial.read();
    }
}

void sendPacket(Protocol protocol, Operation operation) {

    packet.length = sizeof(GSPacket);
    packet.protocol = protocol;
    packet.operation = operation;
    packet.vector1 = 0;
    packet.vector2 = 0;
    
    Serial.print("}-->\n\n");

    Serial.print("PACKAGE GENERATED: \n\n");

    Serial.print("SIZE: ");
    Serial.println(packet.length);

    Serial.print("PROTOCOL: ");
    Serial.println(packet.protocol);

    Serial.print("OPERATION: ");
    Serial.println(packet.operation);

    uint8_t *data = (uint8_t*)&packet;

    Serial.print("Sending Packet: ");
    Serial.print("{");
    for (int i = 0; i < packet.length; i++) {
        Serial.print((int)data[i]);
        Serial.print(" ");
    }
    Serial.println("}");
    

    LoRa.beginPacket();
    LoRa.write(data, packet.length);
    LoRa.endPacket();

    Serial.println("Package sent!\n");
    Serial.print("}-->\n\n");

    return;
}

void sendVectors() {
    packet.length = sizeof(packet);
    packet.protocol = CONTROL_PROTOCOL;
    packet.operation = TWO_VECTORS;
    
    
    // VETOR 1
    Serial.print("Digite o número do vetor1: \n");
    while (Serial.available() == 0) {}           
    uint8_t vetor1 = Serial.parseInt();

    Serial.printf("Vetor 1: %d\n\n", vetor1);
    

    // VETOR 1B
    clearSerial();
    Serial.print("Digite o número do vetor1b: \n");
    while (Serial.available() == 0) {}           
    uint8_t vetor1b = Serial.parseInt();

    Serial.printf("Vetor 1b: %d\n\n", vetor1b);

    // VETOR 2
    clearSerial();
    Serial.print("Digite o número do vetor2: \n");
    while (Serial.available() == 0) {}            
    uint8_t vetor2 = Serial.parseInt();

    Serial.printf("Vetor 2: %d\n\n", vetor2);

    // VETOR 2b
    clearSerial();
    Serial.print("Digite o número do vetor2: \n");
    while (Serial.available() == 0) {}            
    uint8_t vetor2b = Serial.parseInt();

    Serial.printf("Vetor 2b: %d\n\n", vetor2b);

    
    packet.vector1 = vetor1;
    packet.vector1b = vetor1b;
    packet.vector2 = vetor2;
    packet.vector2b = vetor2b;

    Serial.print("}-->\n\n");

    Serial.print("PACKAGE GENERATED: \n\n");

    Serial.print("SIZE: ");
    Serial.println(packet.length);

    Serial.print("PROTOCOL: ");
    Serial.println(packet.protocol);

    Serial.print("OPERATION: ");
    Serial.println(packet.operation);

    uint8_t *data = (uint8_t*)&packet;

    Serial.print("Sending Packet: ");
    Serial.print("{");
    for (int i = 0; i < packet.length; i++) {
        Serial.print((int)data[i]);
        Serial.print(" ");
    }
    Serial.println("}");
    

    LoRa.beginPacket();
    LoRa.write(data, packet.length);
    LoRa.endPacket();

    Serial.println("Package sent!\n");
    Serial.print("}-->\n\n");

    clearSerial();
    return;
}
bool receivePacket(SatPacket *packet, unsigned long timeout_ms) {
    unsigned long start = millis();

    while (millis() - start < timeout_ms) {
        int packetSize = LoRa.parsePacket();
        if (packetSize == 0) continue;  // Nenhum pacote ainda

        Serial.println("\n<--{ PACKET RECEIVED }");

        if (packetSize != sizeof(SatPacket)) {
            Serial.println("Packet size mismatch!");
            Serial.print("Expected: "); Serial.println(sizeof(SatPacket));
            Serial.print("Received: "); Serial.println(packetSize);
            while (LoRa.available()) LoRa.read(); // limpa buffer
            return false;
        }

        uint8_t buffer[sizeof(SatPacket)];
        int index = 0;
        
        while (LoRa.available() && index < sizeof(SatPacket)) {
            buffer[index++] = (uint8_t)LoRa.read();
        }

        memcpy(packet, buffer, sizeof(SatPacket));

        Serial.println("Decoded Packet Content:");
        Serial.print("SIZE: "); Serial.println(packet->length);
        Serial.print("PROTOCOL: "); Serial.println(packet->protocol);
        Serial.print("OPERATION: "); Serial.println(packet->operation);

        Serial.print("Packet Bytes: { ");
        for (int i = 0; i < sizeof(SatPacket); i++) {
            Serial.print((int)buffer[i]);
            Serial.print(" ");
        }
        Serial.println("}");
        Serial.println("<--}\n");

        return true;
    }

    Serial.println("Timeout: no response received.\n");
    return false;
}

bool receiveHealthStatus(HealthStatus *Hstatus, unsigned long timeout_ms) {
    unsigned long start = millis();

    while (millis() - start < timeout_ms) {
        int packetSize = LoRa.parsePacket();
        if (packetSize == 0) continue;  // Nenhum pacote ainda

        Serial.println("\n<--{ HEALTH PACKET RECEIVED }");

        // Verifica tamanho do pacote
        if (packetSize != sizeof(HealthStatus)) {
            Serial.println("Packet size mismatch!");
            Serial.print("Expected: "); Serial.println(sizeof(HealthStatus));
            Serial.print("Received: "); Serial.println(packetSize);
            while (LoRa.available()) LoRa.read(); // limpa buffer
            return false;
        }

        // Lê os bytes do pacote
        uint8_t buffer[sizeof(HealthStatus)];
        int index = 0;
        while (LoRa.available() && index < sizeof(HealthStatus)) {
            buffer[index++] = (uint8_t)LoRa.read();
        }

        // Copia os bytes para a struct Hstatus
        memcpy(Hstatus, buffer, sizeof(HealthStatus));

        // Imprime conteúdo decodificado
        Serial.println("Decoded HealthStatus Packet Content:");
        Serial.print("SIZE: "); Serial.println(Hstatus->length);
        Serial.print("PROTOCOL: "); Serial.println(Hstatus->protocol);
        Serial.print("OPERATION: "); Serial.println(Hstatus->operation);
        Serial.print("NUMBER OF PACKAGES: "); Serial.println(Hstatus->numberOfPackages);

        // Imprime bytes brutos
        Serial.print("Packet Bytes: { ");
        for (int i = 0; i < sizeof(HealthStatus); i++) {
            Serial.print((int)buffer[i]);
            Serial.print(" ");
        }
        Serial.println("}");
        Serial.println("<--}\n");

        return true;
    }

    Serial.println("Timeout: no response received.\n");
    return false;
}

bool receiveHealthData(healthData *Hdata, unsigned long timeout_ms) {
    unsigned long start = millis();

    while (millis() - start < timeout_ms) {
        int packetSize = LoRa.parsePacket();
        if (packetSize == 0) continue; // Nenhum pacote ainda

        Serial.println("\n<--{ HEALTH DATA PACKET RECEIVED }");

        // 1. Verifica o tamanho do pacote
        if (packetSize != sizeof(healthData)) {
            Serial.println("Packet size mismatch!");
            Serial.print("Expected: "); Serial.println(sizeof(healthData));
            Serial.print("Received: "); Serial.println(packetSize);
            while (LoRa.available()) LoRa.read(); // limpa buffer
            return false;
        }

        // 2. Lê os bytes do pacote
        uint8_t buffer[sizeof(healthData)];
        int index = 0;
        while (LoRa.available() && index < sizeof(healthData)) {
            buffer[index++] = (uint8_t)LoRa.read();
        }

        // 3. Copia os bytes para a struct Hdata
        memcpy(Hdata, buffer, sizeof(healthData));

        // 4. Imprime o Conteúdo Decodificado
        Serial.println("Decoded healthData Packet Content:");
        Serial.print("  Battery Temp 1: "); Serial.print(Hdata->batteryTemperature1, 2); Serial.println(" °C");
        Serial.print("  Battery Temp 2: "); Serial.print(Hdata->batteryTemperature2, 2); Serial.println(" °C");
        Serial.print("  External Temp:  "); Serial.print(Hdata->temperatureOut, 2); Serial.println(" °C");
        Serial.print("  Packet Length:  "); Serial.print(Hdata->length); Serial.println(" bytes");

        Serial.print("RSSI: "); Serial.print(LoRa.packetRssi());
        Serial.print(" | SNR: "); Serial.println(LoRa.packetSnr());
        Serial.println("<--}\n");

        // Retorna sucesso APÓS receber e processar UM pacote
        return true; 
    }

    Serial.println("Timeout: no response received.\n");
    return false; // Tempo esgotado sem receber o pacote
}


void onReceive() {
    Serial.println("\nProcessing received packet...");

    switch (resposta.protocol) {
        case HEALTH_PROTOCOL:
            Serial.println("Protocol: HEALTH_PROTOCOL");
            switch (resposta.operation) {
                case GENERATE_HEALTH_DATA:
                
                    break;

                default:
                    Serial.println("Operação desconhecida no protocolo HEALTH_PROTOCOL");
                    break;
            }
            break;
        
            case STATUS_PROTOCOL:
            Serial.println("Protocol: STATUS_PROTOCOL");
            switch (resposta.operation) {
                case INITCOMM:
                    Serial.println("GAMASAT -> HI GROUND!!");
                    break;

                default:
                    Serial.println("Operação desconhecida no protocolo STATUS_PROTOCOL");
                    break;
            }
            break;

        default:
            Serial.println("Protocolo desconhecido!");
            break;
    }

    Serial.println("Packet processed.\n");
}
