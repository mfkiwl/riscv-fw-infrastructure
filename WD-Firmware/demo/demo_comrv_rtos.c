/*
* SPDX-License-Identifier: Apache-2.0
* Copyright 2019 Western Digital Corporation or its affiliates.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http:*www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
#ifdef D_HI_FIVE1
    #include <stdlib.h>
#endif
#include "common_types.h"
#include "comrv_api.h"
#include "demo_platform_al.h"
#include "rtosal_task_api.h"
#include "rtosal_mutex_api.h"
#include "rtosal_queue_api.h"
#include "rtosal_time_api.h"
#include "psp_traps_interrupts.h"
#include "demo_utils.h"

/**
* definitions
*/
#define M_DEMO_COMRV_RTOS_FENCE()   M_PSP_INST_FENCE(); \
                                    M_PSP_INST_FENCEI();

/* The rate the data is sent to the queue, specified in milliseconds, and
converted to ticks (using the definition of D_TICK_TIME_MS) */
#define D_MAIN_QUEUE_SEND_PERIOD_TICKS         (200/D_TICK_TIME_MS)

/* The number of items the queue can hold.  This is 1 as the receive task
has a higher priority than the send task, so will remove items as they are added,
meaning the send task should always find the queue empty. */
#define D_MAIN_QUEUE_LENGTH                    (1)

/* Stack size of the tasks in this application */
#define D_RX_TASK_STACK_SIZE                    450
#define D_TX_TASK_STACK_SIZE                    450

/**
* macros
*/

/**
* types
*/

/**
* local prototypes
*/
void demoRtosalCreateTasks(void *pParam);
void demoRtosalReceiveMsgTask( void *pvParameters );
void demoRtosalTxMsgTask( void *pvParameters );
void demoRtosalcalculateTimerPeriod(void);

/**
* external prototypes
*/

/**
* global variables
*/

#ifdef D_COMRV_FW_INSTRUMENTATION
comrvInstrumentationArgs_t g_stInstArgs;
#endif /* D_COMRV_FW_INSTRUMENTATION */

/* globals related to the tasks in this demo */
static rtosalTask_t stRxTask;
static rtosalTask_t stTxTask;
static rtosalStackType_t uRxTaskStackBuffer[D_RX_TASK_STACK_SIZE];
static rtosalStackType_t uTxTaskStackBuffer[D_TX_TASK_STACK_SIZE];
static s08_t cQueueBuffer[D_MAIN_QUEUE_LENGTH * sizeof(u32_t)];
static rtosalMsgQueue_t stMsgQueue;
static rtosalMutex_t stComrvMutex;
static u32_t uiPrevIntState;

/**
* functions
*/

/**
 * demoStart - startup point of the demo application. called from main function.
 *
 */
void demoStart(void)
{
   comrvInitArgs_t stComrvInitArgs = { 1 };

   /* init comrv */
   comrvInit(&stComrvInitArgs);

   rtosalStart(demoRtosalCreateTasks);
}

/**
 * demoRtosalCreateTasks
 *
 * Initialize the application:
 * - Register unhandled exceptions, Ecall exception and Timer ISR
 * - Create the message queue
 * - Create the Tx and Rx tasks
 * - Create the comrv mutex
 *
 * This function is called from RTOS abstraction layer. After its
 * completion, the scheduler is kicked on and the tasks are start to be active
 *
 */
void demoRtosalCreateTasks(void *pParam)
{

   u32_t res;
   ePspExceptionCause_t cause;

   /* Disable the machine external & timer interrupts until setup is done. */
   pspDisableInterruptNumberMachineLevel(D_PSP_INTERRUPTS_MACHINE_EXT);
   pspDisableInterruptNumberMachineLevel(D_PSP_INTERRUPTS_MACHINE_TIMER);

   /* register exception handlers - at the beginning, register 'pspTrapUnhandled' to all exceptions */
   for (cause = E_EXC_INSTRUCTION_ADDRESS_MISALIGNED ; cause < E_EXC_LAST_COMMON ; cause++)
   {
      /* Skip ECALL entry as we already registered there a handler */
      if (E_EXC_ENVIRONMENT_CALL_FROM_MMODE == cause)
      {
         continue;
      }
      pspRegisterExceptionHandler(pspTrapUnhandled, cause);
   }

   /* Enable the Machine-External bit in MIE */
   pspEnableInterruptNumberMachineLevel(D_PSP_INTERRUPTS_MACHINE_EXT);

   /* Create the queue used by the send-msg and receive-msg tasks. */
   res = rtosalMsgQueueCreate(&stMsgQueue, cQueueBuffer, D_MAIN_QUEUE_LENGTH,
            sizeof(u32_t), NULL);
   if (res != D_RTOSAL_SUCCESS)
   {
      demoOutputMsg("Msg-Q creation failed\n", 22);
      M_DEMO_ENDLESS_LOOP();
   }

   /* Create the rx task */
   res = rtosalTaskCreate(&stRxTask, (s08_t*)"RX", E_RTOSAL_PRIO_29,
            demoRtosalReceiveMsgTask, (u32_t)NULL, D_RX_TASK_STACK_SIZE,
            uRxTaskStackBuffer, 0, D_RTOSAL_AUTO_START, 0);
   if (res != D_RTOSAL_SUCCESS)
   {
      demoOutputMsg("Rx-Task creation failed\n", 24);
      M_DEMO_ENDLESS_LOOP();
   }

   /* Create the tx task in exactly the same way */
   res = rtosalTaskCreate(&stTxTask, (s08_t*)"TX", E_RTOSAL_PRIO_29,
            demoRtosalTxMsgTask, (u32_t)NULL, D_TX_TASK_STACK_SIZE,
           uTxTaskStackBuffer, 0, D_RTOSAL_AUTO_START, 0);
   if (res != D_RTOSAL_SUCCESS)
   {
      demoOutputMsg("Tx-Task creation failed\n", 24);
      M_DEMO_ENDLESS_LOOP();
   }

   /* create the comrv mutex */
   res = rtosalMutexCreate(&stComrvMutex, (s08_t*)"comrv", D_RTOSAL_INHERIT);
   if (res != D_RTOSAL_SUCCESS)
   {
      demoOutputMsg("comrv mutex creation failed\n", 28);
      M_DEMO_ENDLESS_LOOP();
   }

   /* Calculates timer period */
   demoRtosalcalculateTimerPeriod();
}

/**
 * send an item to the rtos queue
 *
 * pValueToSend - Item to send to the queue
 */
void _OVERLAY_ OvlFuncSendMsg(u32_t* pValueToSend)
{
   demoOutputMsg("Sending to queue\n", 17);

   /* Send to the queue - causing the queue receive task to unblock and
   increment its counter.  0 is used as the block time so the sending
   operation will not block - it shouldn't need to block as the queue
   should always be empty at this point in the code. */
   rtosalMsgQueueSend(&stMsgQueue, pValueToSend, 0, D_RTOSAL_FALSE);
}

/**
 * send an item to the rtos queue
 *
 * pValueToSend - Item to send to the queue
 */
void _OVERLAY_ OvlFuncTx(u32_t* pValueToSend)
{
   u32_t count = 0;

   /* Place this task in the blocked state until it is time to run again.
   The block time is specified in ticks.
   The task will not consume any CPU time while it is in the Blocked state. */
   rtosalTaskSleep(D_MAIN_QUEUE_SEND_PERIOD_TICKS);

   OvlFuncSendMsg(pValueToSend);

   /* the purpose of this loop is to wait for a context switch while
      in an overlay function  - every odd call*/
   for ( ; count < 100000 ; count ++)
   {
      M_PSP_NOP();
      M_PSP_NOP();
      M_PSP_NOP();
      M_PSP_NOP();
      M_PSP_NOP();
   }
}

/**
 * Tx task function
 *
 * pvParameters - not in use
 */
void demoRtosalTxMsgTask( void *pvParameters )
{
   u32_t ulValueToSend = 0UL;

   for( ;; )
   {
      /* value to send to the queue */
      ulValueToSend++;

      /* send to queue */
      OvlFuncTx(&ulValueToSend);
   }
}

/**
 * Recieve an item from the rtos queue
 *
 * pvParameters - holds the received queue item value
 */
void _OVERLAY_ OvlFuncRx(u32_t* pReceivedValue)
{
   rtosalMsgQueueRecieve(&stMsgQueue, pReceivedValue, portMAX_DELAY);
}

/**
 * demoRtosalReceiveMsgTask - Rx task function
 *
 * void *pvParameters - not in use
 */
void demoRtosalReceiveMsgTask( void *pvParameters )
{
   char stringValue[10];
   u32_t ulReceivedValue;

   for( ;; )
   {
      OvlFuncRx(&ulReceivedValue);

      itoa(ulReceivedValue,stringValue, 10);
      demoOutputMsg("Received: ", 10);
      demoOutputMsg(stringValue, 3);
      demoOutputMsg("\n",1);
   }
}

/**
* enter critical section
*
* @param None
*
* @return 0 - success, non-zero - failure
*/
u32_t comrvEnterCriticalSectionHook(void)
{
   if (rtosalGetSchedulerState() != D_RTOSAL_SCHEDULER_NOT_STARTED)
   {
      if (rtosalMutexWait(&stComrvMutex, D_RTOSAL_WAIT_FOREVER) != D_RTOSAL_SUCCESS)
      {
         return 1;
      }
   }
   else
   {
      M_PSP_INTERRUPTS_DISABLE_IN_MACHINE_LEVEL(&uiPrevIntState);
   }

   return 0;
}

/**
* exit critical section
*
* @param None
*
* @return 0 - success, non-zero - failure
*/
u32_t comrvExitCriticalSectionHook(void)
{
   if (rtosalGetSchedulerState() != D_RTOSAL_SCHEDULER_NOT_STARTED)
   {
      if (rtosalMutexRelease(&stComrvMutex) != D_RTOSAL_SUCCESS)
      {
         return 1;
      }
   }
   else
   {
      M_PSP_INTERRUPTS_RESTORE_IN_MACHINE_LEVEL(uiPrevIntState);
   }

   return 0;
}

/**
 * demoRtosalcalculateTimerPeriod - Calculates Timer period
 *
 */
void demoRtosalcalculateTimerPeriod(void)
{
   u32_t uiTimerPeriod = 0;

    #if (0 == D_CLOCK_RATE) || (0 == D_TICK_TIME_MS)
        #error "Core frequency values definitions are missing"
    #endif

   uiTimerPeriod = (D_CLOCK_RATE * D_TICK_TIME_MS / D_PSP_MSEC);
   /* Store calculated timerPeriod for future use */
   rtosalTimerSetPeriod(uiTimerPeriod);
}
