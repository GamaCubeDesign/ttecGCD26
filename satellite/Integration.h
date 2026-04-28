#ifndef INTEGRATION_H
#define INTEGRATION_H

#include <stdint.h>
#include <stdlib.h>



struct healthData {
    float batteryTemperature1;
    float batteryTemperature2;
    float temperatureOut;
    uint8_t length;
};
extern healthData health;


struct imagingData {
    uint8_t length;
};

typedef struct celula {
    healthData dado;
    struct celula* prox;
} fila;

void parseHealth();
void parseImaging();
void parseControle();

void sendHealthData();
void SendImagingData();
void sendThermalControlData();
void sendControlData();

int verifyFile();
void RemoveFile();
void generateHealthData();

extern fila* HealthFIFO;
extern fila* ImagingFIFO;
extern fila* ThermalControlFIFO;

extern int HealthDataCounter;
extern int ImagingDataCounter;
extern int ThermalControlDataCounter;

extern const char *HealthFil;
extern const char *ImagingFile;
extern const char *ThermalControlFile;
extern const char *ControlFile;
extern const char *AISFile;

extern bool run;

fila* CreateFIFO();
fila* Enqueue(fila* f, healthData x);
int Dequeue(fila* f, healthData* y);
void DestroyFIFO(fila *f);
void ShowFIFO(fila* f);

void initSubsystems();
void ShutDownSystem();

void activateThermalControl();
void deactivateThermalControl();

#endif
