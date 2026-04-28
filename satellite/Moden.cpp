#include "Moden.h"
#include <stdint.h>

LoRa_ctl modem;
uint8_t rxBuffer[255];
unsigned int rxBufferPointer = 0;
unsigned int rxBufferSize = 0;

uint8_t modemAvailable(){
    printf("\nrxBufferSize: %u\n", rxBufferSize);
    return rxBufferSize;
}

uint8_t modemRead(){
    rxBufferSize--;
    return rxBuffer[rxBufferPointer++];
}


//Esse callback rx_f é chamado pela lib toda vez que chega um pacote LoRa.
//Ele atualiza o rxBufferSize e o rxBufferPointer
void *rx_f(void *p){
    rxData *rx = (rxData *)p;   // rxData é uma struct da biblioteca
    printf("rx done \n");
    printf("CRC error: %d\n", rx->CRC);
    printf("Data size: %d\n", rx->size);
    printf("string: %s\n", rx->buf); //Dados que recebemos
    printf("RSSI: %d\n", rx->RSSI);
    printf("SNR: %f\n", rx->SNR);
    rxBufferPointer = 0;    //Atualiza a variável para 0
    rxBufferSize = rx->size;    //Atualiza o tamanho do buffer para igual size da struct
    memcpy(rxBuffer, rx->buf, rx->size); //Copia os bytes recebidos (rx->buf) para o buffer local rxBuffer. (rx->size) é a quantidade de bytes que são copiados
    free(p);
    return NULL;
}



void tx_f(txData *tx){
    printf("tx done \n");
}

//Função que inicializa o módulo LoRa
void initRFModule(){
    // See for typedefs, enumerations and there values in LoRa.h header file
    modem.spiCS = 0; // Raspberry SPI CE pin number
    modem.tx.callback = tx_f;
    modem.rx.callback = rx_f;
    modem.eth.preambleLen = 6;
    modem.eth.bw = BW62_5;            // Bandwidth 62.5KHz
    modem.eth.sf = SF12;              // Spreading Factor 12
    modem.eth.ecr = CR8;              // Error coding rate CR4/8
    modem.eth.CRC = 1;                // Turn on CRC checking
    modem.eth.freq = 433000000;       // 433.0MHz
    modem.eth.resetGpioN = 20;         // GPI20 on lora RESET pin
    modem.eth.dio0GpioN = 21;         // GPIO21 on lora DIO0 pin to control Rxdone and Txdone interrupts
    modem.eth.outPower = OP20;        // Output power
    modem.eth.powerOutPin = PA_BOOST; // Power Amplifire pin
    modem.eth.AGC = 1;                // Auto Gain Control
    modem.eth.OCP = 240;              // 45 to 240 mA. 0 to turn off protection
    modem.eth.implicitHeader = 0;     // Explicit header mode
    modem.eth.syncWord = 0x12;
    // For detail information about SF, Error Coding Rate, Explicit header, Bandwidth,AGC, Over current protection and other features refer to sx127x datasheet https : // www.semtech.com/uploads/documents/DS_SX1276-7-8-9_W_APP_V5.pdf
    int status = LoRa_begin(&modem);
    
    if (status < 0) {
        printf("Error initializing the LoRa module.\n");
        return;
    }
    printf("LoRa module successfully initialized!\n");
    
    LoRa_receive(&modem);
    printf("\nActive reception mode\n");

}

//Função que envia os dados via LoRa, envia apenas os campos da struct modem: modem.tx.data.buf e modem.tx.data.size
void tx_send(uint8_t *buf, unsigned int size){
    LoRa_stop_receive(&modem);      //LoRa desativa modo de recepção
    char txbuf[255];
    modem.tx.data.buf = txbuf;
    memcpy(modem.tx.data.buf, buf, size); //Copia os dados de buf para o buffer de transmissão, buf de tamanho size
    modem.tx.data.size = size;            // o campo size da struct recebe o tamanho dos dados
    LoRa_send(&modem);      //Função que envia os dados: modem.tx.data.buf e modem.tx.data.size
    printf("Tsym: %f\n", modem.tx.data.Tsym);
    printf("Tpkt: %f\n", modem.tx.data.Tpkt);
    printf("payloadSymbNb: %u\n", modem.tx.data.payloadSymbNb);
    printf("sleep %d seconds to transmitt complete\n", (int)modem.tx.data.Tpkt / 1000);
    
    //Tpkt é um campo da struct modem que indica o tempo que a transmissão leva para ser concluída em milissegundos
    sleep(((int)modem.tx.data.Tpkt / 1000) + 1); //Aqui o valor contido no campo mencionado da struct é forçado a ser int e dividido por 1000 para virar milissegundos e é adicionado +1 para margem de segurança
    //Aqui o sleep serve para garantir que a transmissão seja concluída antes de voltar ao modo de recepção
    
    printf("end\n");
    LoRa_receive(&modem);       //Volta o LoRa para mode de recepção
}


void modemFinish(){
    LoRa_end(&modem);
}

