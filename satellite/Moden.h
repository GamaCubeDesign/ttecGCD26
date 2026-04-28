#ifndef MODEN_H
#define MODEN_H

#include "LoRa.h"


extern LoRa_ctl modem;
extern uint8_t rxBuffer[255];
extern unsigned int rxBufferPointer;
extern unsigned int rxBufferSize;
extern uint8_t modemAvailable();
extern uint8_t modemRead();
extern void *rx_f(void *p);
extern void tx_f(txData *tx);
extern void initRFModule();
extern void tx_send(uint8_t *buf, unsigned int size);
extern void modemFinish(); 


#endif