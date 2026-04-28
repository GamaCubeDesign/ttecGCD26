#include <SPI.h>
#include <LoRa.h>
#include "GroundStationComm.h"
#include "CommunicationProtocol.h"

#define LORA_PIN_SCK      18     // Clock SPI do LoRa
#define LORA_PIN_MISO     19    // MISO (Master In Slave Out)
#define LORA_PIN_MOSI     23    // MOSI (Master Out Slave In)
#define LORA_PIN_NSS      5    // Chip Select (às vezes chamado CS ou SS)
#define LORA_PIN_RESET    14    // Pino de reset do módulo LoRa
#define LORA_PIN_IRQ      26    // Pino de interrupção DIO0 (recepção de pacotes)

//Potência de saída
#define PA_OUTPUT_RFO        0
#define PA_OUTPUT_PA_BOOST   1

bool HealthFIFO = false;
bool ImagingFIFO = false;
int HealthCells = 0;
int ImagingCells = 0;

int qtdHealth = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial);  
  delay(1000);

  Serial.println("\nWelcome to the Iarasat ground station embedded software\n");

  Serial.println("Testing the Gama Ground Station communication system with the LoRa Ra-02 RF module\n");
  
  Serial.println("Inicializando módulo LoRa...");

  SPI.begin(LORA_PIN_SCK, LORA_PIN_MISO, LORA_PIN_MOSI, LORA_PIN_NSS);

  LoRa.setPins(LORA_PIN_NSS, LORA_PIN_RESET, LORA_PIN_IRQ);

  if (!LoRa.begin(433E6)) {
    Serial.println("Falha na inicialização do LoRa.");
  }else{
    Serial.println("LoRa inicializado com sucesso.");
  }
  
  // Configura parâmetros
  //LoRa.explicitHeaderMode();       // Header explícito
  LoRa.setSpreadingFactor(12);     // SF12
  LoRa.setSignalBandwidth(62500);  // 62.5 kHz
  LoRa.setCodingRate4(8);          // CR4/8
  LoRa.enableCrc();                // Habilita CRC
  LoRa.setPreambleLength(6);       // Preamble = 6

  // Define potência de transmissão máxima com PA_BOOST
  LoRa.setTxPower(20, PA_OUTPUT_PA_BOOST);

}

void loop() {
  String comand = "";

  menu();

  while (!Serial.available()) {
    delay(10);
  }

  comand = Serial.readStringUntil('\n');
  comand.trim(); 

  if (comand == "ghdata") {
    Serial.print("Request for health data: \n");
    sendPacket(HEALTH_PROTOCOL, GENERATE_HEALTH_DATA);
    if (receiveHealthStatus(&Hstatus, 7000)) {  // timeout de 3 segundos
        Serial.println("resposta recebida!");
        //onReceive();
        delay(7000);
        qtdHealth = Hstatus.numberOfPackages;
        Serial.print(qtdHealth);
        sendPacket(HEALTH_PROTOCOL, CONFIRMATION_HEALTH_DATA);
        for(int i = 0; i < qtdHealth; i++){
            receiveHealthData(&health, 7000);
        }

    }
  }
  else if (comand == "rhdata") {
    sendPacket(HEALTH_PROTOCOL, RESEND_HEALTH_DATA);
  }
  else if (comand == "initcomm") {
    Serial.print("");
    sendPacket(STATUS_PROTOCOL, INITCOMM);
    if (receivePacket(&resposta, 5000)) {  // timeout de 3 segundos
        Serial.println("resposta recebida!");
        onReceive();
    }
  }
  else if (comand == "vfile") {
    sendPacket(STATUS_PROTOCOL, VERIFY_FILE);
  }
  else if (comand == "vinfo") {
    //sendPacket(STATUS_PROTOCOL, VERIFY_INFO);
  }
  
  //   CONTROLE
  else if (comand == "solarvector") {
    sendPacket(CONTROL_PROTOCOL, SOLAR_VECTOR);
  }
  else if (comand == "2vectors") {
    //sendPacket(CONTROL_PROTOCOL, TWO_VECTORS);
    sendVectors();
  }
  else if (comand == "sunp") {
    sendPacket(CONTROL_PROTOCOL, SUN_POINTING);
  }
  else if (comand == "stabili") {
    sendPacket(CONTROL_PROTOCOL, STABILIZATION);
  }

  //   CONTROLE TERMICO 
  else if (comand == "activatetc") {
    sendPacket(TERM_PROTOCOL, ACTIVATE_THERMAL_CONTROL);
  }
  else if (comand == "deactivatetc") {
    sendPacket(TERM_PROTOCOL, DEACTIVATE_THERMAL_CONTROL);
  }
  else if (comand == "ctdata") {
    sendPacket(TERM_PROTOCOL, SEND_CT_DATA);
  }


  //    ENCERRA SISTEMA DE TTEC DO SATELITE
  else if (comand == "down") {
    sendPacket(STATUS_PROTOCOL, SHUT_DOWN_SYSTEM);
  }
  else {
    Serial.println("Error: nonexistent command.");
  }
}

