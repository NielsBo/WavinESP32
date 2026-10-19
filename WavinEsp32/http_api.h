#pragma once
#include <Arduino.h>

// Minimal JSON API. See README for the full contract.
//   GET  /health
//   GET  /rooms
//   GET  /rooms/<id>
//   POST /rooms/<id>   any of {"manualTemp"|"comfortTemp"|"ecoTemp"|"standbyTemp"|"holidayTemp"|"partyTemp":21.5,
//                              "mode":"manual"|"standby"|"eco"|"comfort"|"schedule"|
//                                     "temp-eco"|"temp-comfort"|"party"|"holiday",
//                              "durationMin":180, "cooling":true}
//                      requires header  X-Api-Token: <API_TOKEN>
void httpBegin();
void httpLoop();
