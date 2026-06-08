#ifndef JSON_DATA_H
#define JSON_DATA_H

#include <stdint.h>

typedef struct
{
    int time;
    char url[100];
    char model[100];
    char key[100];
}ai_model_t;

ai_model_t *json_sdcard_txt_aimodel(void);

#endif

