#include "Integration.h"
#include "CommunicationProtocol.h"
#include "Moden.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>

#include <unistd.h>
#include <sys/types.h>
#include <signal.h>


using json = nlohmann::json;


fila* HealthFIFO = NULL;
fila* ImagingFIFO = NULL;
fila* ThermalControlFIFO = NULL;

int HealthDataCounter = 0;
int ImagingDataCounter = 0;
int ThermalControlDataCounter = 0;

bool run = false;

healthData health;
imagingData imaging;

const char *HealthFile = "HealthFile.json";
const char *ImagingFile = "ImagingFile.json";
const char *ThermalControlFile = "controleTermico.json";
const char *ControlFile = "controle.json";
const char *AISFile = "AISFile.json";

fila* CreateFIFO() {
    fila* f = (fila*) malloc(sizeof(fila));
    if (!f) return NULL;
    f->prox = f;
    return f;
}

fila* Enqueue(fila* f, healthData x) {
    fila* novo = (fila*) malloc(sizeof(fila));
    if (!novo) return f;
    novo -> prox = f -> prox;
    f -> prox = novo;
    f -> dado = x;
    return novo;
}

// Desinfileira do início
int Dequeue(fila* f, healthData* y) {
    if (f->prox == f) return 1; // fila vazia
    fila* lixo = f->prox;
    *y = lixo->dado;
    f->prox = lixo->prox;
    free(lixo);
    return 0; // sucesso
}

void DestroyFIFO(fila *f){
    fila *atual = f -> prox;
    fila *prox;

    while(atual != f){
        prox = atual -> prox;
        free(atual);
        atual = prox;
    }
    free(f);
}



void initSubsystems(){
    HealthFIFO = CreateFIFO();
    ImagingFIFO = CreateFIFO();
    ThermalControlFIFO = CreateFIFO();

    if (!HealthFIFO || !ImagingFIFO || !ThermalControlFIFO) {
        std::cout << "\nSome or all of the structures have not been initialized.\n" << std::endl;
        return;
    }
    
    std::cout << "\nAll structures have been successfully initialized.\n" << std::endl;
    std::cout << "\nHealth data FIFO: \n" << std::endl;
    ShowFIFO(HealthFIFO);
    std::cout << "\nImaging data FIFO: \n" << std::endl;
    ShowFIFO(ImagingFIFO);  
    std::cout << "\nThermal control data FIFO: \n" << std::endl;
    ShowFIFO(ThermalControlFIFO);
}

void ShutDownSystem(){
    run = false;
    std::cout << "\nShutting down the system...\n" << std::endl;
    std::cout <<"\ndestroying all the FIFOs...\n" << std::endl;
    DestroyFIFO(HealthFIFO);
    DestroyFIFO(ImagingFIFO);
    DestroyFIFO(ThermalControlFIFO);

}


void parseHealth(){
    std::ifstream file("HealthData.json");
    json j;
    file >> j;

    health.batteryTemperature1 = j["temperatures_c"][0].value("temp_c", 0.0f);
    health.batteryTemperature2 = j["temperatures_c"][1].value("temp_c", 0.0f);
    health.temperatureOut      = j["temperatures_c"][2].value("temp_c", 0.0f);
    health.length = sizeof(health);
}


void parseControle(){
    std::ifstream file("controle.json");
    json j;
    file >> j;

    //thermalControlD.batteryTemperature1 = j["temperatures_c"][0].value("temp_c", 0.0f);
    //thermalControlD.batteryTemperature2 = j["temperatures_c"][1].value("temp_c", 0.0f);
    //thermalControlD.temperatureOut      = j["temperatures_c"][2].value("temp_c", 0.0f);

    controlD.length = sizeof(controlD);
}

void parseImaging(){
    std::ifstream file("ImagingFile.json");
    json j;
    file >> j;
    
    
    imaging.length = sizeof(imaging);
}


void sendHealthData(){
    std::cout << "\nEnviando todos os pacotes.\n" << std::endl;
    int i = 0;
    for(i = 0; i < HealthDataCounter; i++){
        healthData hd;
        Dequeue(HealthFIFO, &hd);
        tx_send((uint8_t*)&hd, hd.length);
        std::cout << "Pacote " << i+1 << " enviado." << std::endl;
        sleep(1);
    }
    std::cout << "\nHealth data sent.\n" << std::endl;
}

void sendThermalControlData(){
    std::cout << "\nEnviando os dados de controle térmico disponíveis.\n" << std::endl;
    //tx_send((uint8_t*)&thermalControlD, thermalControlD.length);
    std::cout << "\nThermal control data sent.\n" << std::endl;
}

void sendControlData(){
    std::cout << "\nEnviando os dados de controle disponíveis.\n" << std::endl;
    tx_send((uint8_t*)&controlD, controlD.length);
    std::cout << "\nControl data sent.\n" << std::endl;
}

void SendImagingData(){
    tx_send((uint8_t*)&imaging, imaging.length);
}

int verifyFile(){
    std::string HFileName = "sensor_data.json";
    std::string IFileName = "imaging_data.json";
    std::string TFileName = "thermalControl_data.json";

    FILE *file = fopen(HFileName.c_str(), "r"); 

    if (file != NULL) {
        std::cout << "The file exists\n\n";
        std::fclose(file);
        return 1;
    }else {
        std::cout << "The file does not exist\n";
        return 0;
    }
}

void RemoveFile(const char *filename){
    if(remove(filename) == 0){
        std::cout << "File deleted successfully.\n";
    } else {
        std::cout << "Error deleting the file.\n";
    }
}
void generateHealthData(){
    //Falta adicionar a execução do código de OBC para gerar o .json
    
    if(verifyFile()){
        parseHealth();
        HealthFIFO = Enqueue(HealthFIFO, health);
        HealthDataCounter++;
        std::cout << "Pacote gerado e enfileirado." << std::endl;

    } else {
        std::cout << "Health data file not found." << std::endl;
    }
}


pid_t thermal_pid = -1;  // PID global do processo filho


void activateThermalControl(){
    pid_t pid = fork();

    if (pid == 0) {
        // Processo filho -> executa o outro programa
        execl("./controletermico.o", "controletermico.o", NULL);
        perror("Erro ao executar thermal_program");
        exit(1); // garante que o filho não continue no pai
    }
    else if (pid > 0) {
        thermal_pid = pid; // <--- salva o PID do filho
        printf("Controle térmico ativado (processo separado iniciado) PID=%d\n", thermal_pid);
    }
    else {
        perror("Erro no fork");
    }
}


void deactivateThermalControl(){
    if (thermal_pid > 0) {
        if (kill(thermal_pid, SIGKILL) == 0) {
            printf("Thermal control encerrado (PID %d)\n", thermal_pid);
        } else {
            perror("Erro ao encerrar processo");
        }
        thermal_pid = -1;
    } else {
        printf("Nenhum processo thermal está rodando.\n");
    }
}


void ShowFIFO(fila* f) {
    if(!f){
        std::cout << "\nFIFO not initialized!\n" << std::endl;
        return;
    }
    if (f->prox == f) {
        std::cout << "Empty FIFO!" << std::endl;
        std::cout << "Only the head node allocated." << std::endl;
        std::cout << "               _____________________ " << std::endl;
        std::cout << "              |                     |" << std::endl;
        std::cout << "              V                     |" << std::endl;
        std::cout << "                                    |" << std::endl;
        std::cout << "          Head node                 |" << std::endl;
        std::cout << "                                    |" << std::endl;
        std::cout << "              |_____________________|" << std::endl;
        std::cout << "" << std::endl;

        return;
    }
}
/*
    fila* atual = f->prox;
    int i = 0;

    
    std::cout << "               _____________________ " << std::endl;
    std::cout << "              |                     |" << std::endl;
    std::cout << "              V                     |" << std::endl;
    while (atual != f) { // percorre até voltar ao nó cabeça
        std::cout << "                                    |" << std::endl;
        std::cout << "  Packet " << i << ":                         |" << std::endl;
        std::cout << "  Voltage: " << atual->dado.voltage << " V                  |" << std::endl;
        std::cout << "  Current: " << atual->dado.current << " mA                 |" << std::endl;
        std::cout << "  Power: " << atual->dado.power << " mW                  |" << std::endl;
        std::cout << "  Shunt: " << atual->dado.shunt << " mV                    |" << std::endl;
        std::cout << "  Temperature BMP180: " << atual->dado.temperature << " °C       |" << std::endl;
        std::cout << "  Pressure BMP180: " << atual->dado.pressure << " hPa       |" << std::endl;
        std::cout << "  Altitude BMP180: " << atual->dado.altitude << " m          |" << std::endl;
        std::cout << "  Temperature DS18B20: " << atual->dado.temperatureD << " °C      |" << std::endl;
        std::cout << "  SD Memory Usage: " << atual->dado.sd_memory_usage << " MB           |" << std::endl;
        std::cout << "                                    |" << std::endl;
        std::cout << "              |                     |" << std::endl;
        std::cout << "              V                     |" << std::endl;
        atual = atual->prox;
        i++;
    }
    std::cout << "                                    |" << std::endl;
    std::cout << "          Head node                 |" << std::endl;
    std::cout << "                                    |" << std::endl;
    std::cout << "              |_____________________|" << std::endl;
    std::cout << "" << std::endl;
    std::cout << "" << std::endl;

}*/



/*
int main(){

    activateThermalControl();
    int i = 0;

    for (int i = 0; i <= 50; i++) {
    printf("[principal] Contador: %d\n", i);
    fflush(stdout);
    sleep(1);
    }

    deactivateThermalControl();
    return 0;
}*/
