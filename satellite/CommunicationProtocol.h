#ifndef COMMUNICATION_PROTOCOL_H
#define COMMUNICATION_PROTOCOL_H
#include <cstdint>  

enum Protocol : uint8_t {
    HEALTH_PROTOCOL = 0,
    AIS_PROTOCOL = 1,
    MEC_PROTOCOL = 2,
    IMAGING_PROTOCOL = 3,
    CONTROL_PROTOCOL = 4,
    TERM_PROTOCOL = 5,
    STATUS_PROTOCOL = 6,
};

enum Operation : uint8_t {
    //health data
    GENERATE_HEALTH_DATA = 0,
    RESEND_HEALTH_DATA = 1,
    CONFIRMATION_HEALTH_DATA = 12,

    //status data
    INITCOMM = 2,
    VERIFY_FILE = 3,
    SHUT_DOWN_SYSTEM = 4,

    //Mechanical control
    OPEN_ANTENNAS = 5,

    //Termal control
    ACTIVATE_THERMAL_CONTROL = 6,
    DEACTIVATE_THERMAL_CONTROL = 7,

    //Control
    SOLAR_VECTOR = 8,
    TWO_VECTORS = 9,
    SUN_POINTING = 10,
    STABILIZATION = 11,

};


struct SatPacket{
    uint8_t length;
    Protocol protocol;
    Operation operation;
};


struct GSPacket{
    uint8_t length;
    Protocol protocol;
    Operation operation;
    uint8_t vector1;
    uint8_t vector1b;
    uint8_t vector2;
    uint8_t vector2b;
};


struct control{
    uint8_t length;
    Protocol protocol;
    Operation operation;
    uint8_t vector1;
    uint8_t vector2;
    uint8_t solarVector;   
};

struct HealthStatus{
    uint8_t length;
    Protocol protocol;
    Operation operation;
    uint8_t numberOfPackages;
};

extern SatPacket satPacket;
extern GSPacket gsPacket;
extern HealthStatus hStatus;
extern control controlD;
#endif