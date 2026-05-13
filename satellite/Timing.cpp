#include "Timing.h"

clock_t timer = clock();

float seconds(){
    return ((float)(clock() - timer)) / CLOCKS_PER_SEC;
}

long millis(){
    return seconds() * 1000;
}