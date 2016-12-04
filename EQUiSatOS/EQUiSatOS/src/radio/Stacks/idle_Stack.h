/*
 * idle_Stack.h
 *
 * Created: 10/18/2016 9:46:57 PM
 *  Author: rj16
 */ 

#ifndef IDLE_STACK_H_
#define IDLE_STACK_H_

#define MUTEX_WAIT_TIME_TICKS 10
#define IDLE_STACK_MAX 4
#include <asf.h>
#include "State_Structs.h"

typedef struct idle_Stack
{
	idle_data_t*     data[IDLE_STACK_MAX];
	int16_t     size;
	int16_t     top_index;
	int16_t     bottom_index;	
	SemaphoreHandle_t mutex;
} idle_Stack;

idle_Stack* idle_Stack_Init();
idle_data_t* idle_Stack_Top(idle_Stack* S);
void idle_Stack_Push(idle_Stack* S, idle_data_t* val);

#endif