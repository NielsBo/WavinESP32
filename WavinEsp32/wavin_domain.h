#pragma once
#include <Arduino.h>
#include "config.h"

// Register categories (docs/WAVIN_REGISTER_MAP.md section 2)
#define CAT_MAIN       0x00
#define CAT_ELEMENTS   0x01
#define CAT_PACKED     0x02
#define CAT_CHANNELS   0x03
#define CAT_RELAYS     0x04
#define CAT_CLOCK      0x05
#define CAT_SCHEDULES  0x06
#define CAT_INFO       0x07

// ELEMENTS (0x01) register indices
#define EL_ADDRESS_L   0x00
#define EL_ADDRESS_H   0x01
#define EL_ASSIGN_L    0x02
#define EL_ASSIGN_H    0x03
#define EL_AIR_TEMP    0x04
#define EL_FLOOR_TEMP  0x05
#define EL_DEW_POINT   0x06
#define EL_HUMIDITY    0x07
#define EL_STATUS      0x08
#define EL_RSSI        0x09
#define EL_BATTERY     0x0A
#define EL_SYNC_GROUP  0x0B
#define EL_BLOCK_START EL_AIR_TEMP     // read 0x04..0x0B in one transaction
#define EL_BLOCK_LEN   8

// Element STATUS (0x08) bits
#define EL_ST_ALIVE     0x8000
#define EL_ST_LOST      0x0800
#define EL_ST_LOW_BATT  0x0400
#define EL_ST_NAT_ELEM  0x0200
#define EL_ST_TP        0x0100
#define EL_ST_TP_ACT    0x0080   // thermostat output active == calling for heat
#define EL_ST_ACTIVE    0x0040
#define EL_ST_RFSH      0x0010

// PACKED DATA (0x02) register indices  (page = sync group / room)
#define PD_MANUAL      0x00
#define PD_COMFORT     0x01
#define PD_ECO         0x02
#define PD_HOLIDAY     0x03
#define PD_STANDBY     0x04
#define PD_PARTY       0x05
#define PD_MODE_LEN    0x06
#define PD_CONFIG      0x07
#define PD_MIN_TEMP    0x08
#define PD_MAX_TEMP    0x09
#define PD_FLOOR_MIN   0x0A
#define PD_FLOOR_MAX   0x0B
#define PD_ALARM_MIN   0x0C
#define PD_ALARM_MAX   0x0D
#define PD_HYSTERESIS  0x0E
#define PD_DESIRED     0x10   // read-only
#define PD_PAGE_LEN    17     // whole page in one read

// PACKED DATA CONFIG (0x07) bits
#define PD_CFG_MODE_MASK  0x0007
#define PD_CFG_SCHED_ENA  0x0008
#define PD_CFG_HOTEL      0x0200
#define PD_CFG_CTRL_LOCK  0x0400
#define PD_CFG_INT_LOCK   0x0800
#define PD_CFG_ADAPT      0x1000
#define PD_CFG_COOL       0x2000
#define PD_CFG_FLOOR_ENA  0x4000
#define PD_CFG_FLOOR_SENS 0x8000

// MODE[2:0] values (meaning depends on SCHED ENA)
#define MODE_MANUAL    0
#define MODE_STANDBY   1
#define MODE_ECO       2
#define MODE_COMFORT   3
#define MODE_PARTY     4
#define MODE_HOLIDAY   5

// CHANNELS (0x03)
#define CH_TIMER_EVENT 0x00
#define CH_CURRENT     0x01
#define CH_PRIMARY_EL  0x02
#define CH_OUTP_ON     0x0010   // bit in CH_TIMER_EVENT

struct Room {
  bool     valid;
  int      id;            // elementIndex + 1  (legacy R{i} numbering)
  int      elementIndex;
  int      syncGroup;
  float    current;       // air temperature, degC
  float    floorTemp;     // NAN if unknown
  float    dewPoint;      // NAN if unknown
  float    desired;       // currently active setpoint, degC
  float    manual;        // manual-mode setpoint, degC
  int      humidity;      // %RH, -1 if unknown
  int      battery;       // %, -1 if unknown
  float    rssiElement;   // dBm
  float    rssiUnit;      // dBm
  bool     output;        // heat demand (status bit 7)
  bool     alive;
  bool     lowBattery;
  uint16_t config;        // raw PD_CONFIG
};

void  wavinDomainBegin();

// Polls all elements and refreshes the room cache. Returns rooms found.
int   pollAllRooms();

int          roomCount();                 // number of valid rooms in the cache
const Room*  roomAt(int idx);             // 0..roomCount()-1
const Room*  roomById(int id);            // nullptr if unknown
uint32_t     lastPollAt();                // millis() of last successful poll

// Control. Both re-read the room afterwards so the cache reflects reality.
bool setRoomManualTemp(int id, float celsius);      // clamped 0..30
bool setRoomMode(int id, const char *mode);         // "manual" | "standby" | "eco" | "comfort"

const char* modeName(uint16_t config);
