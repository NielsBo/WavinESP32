#pragma once
#include <Arduino.h>

// Minimal JSON API. See README for the full contract.
//   GET  /health
//   GET  /rooms
//   GET  /rooms/<id>
//   POST /rooms/<id>   {"manualTemp":21.5} | {"mode":"manual"|"standby"|"eco"|"comfort"}
//                      requires header  X-Api-Token: <API_TOKEN>
void httpBegin();
void httpLoop();
