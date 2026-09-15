#include <iostream>
#include <thread>
#include <unistd.h>

#include "Moden.h"
#include "Module.h"
#include "Integration.h"

#include "LoRa.h"



using namespace std;


void loop(){
    updateRFComm();
}


int main(int argc, char *argv[]){
    std::cout << "Telemetry and remote control system with LoRa RA-02 using Iarasat's Raspberry Pi\n";

    std::cout << "Initializing the data storage subsystems....\n";
    initSubsystems();

    std::cout << "\nTrying to initialize the LoRa module\n";

    initRFModule();

    std::cout << "\nEntering the loop...\n";

    sleep(5);
    
    run = true;

    while(run){
        loop();
    }

    std::cout << "\nLoop terminated.\n";
    modemFinish();
    std::cout << "\nBye bye!\n";
    return 0;

}