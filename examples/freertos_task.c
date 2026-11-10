/* FreeRTOS sahip görevi (INTEGRATION.md §7). Sürücü fonksiyonlarını YALNIZ bu
 * görev çağırır; UART/CAN gibi başka yerlerden komut kuyruğa konur.
 * baremetal.c'deki actuator_init/actuator_move/actuator_recover kullanılır. */
#include <stdbool.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "BTM9011EP.h"

HAL_StatusTypeDef actuator_init(void);
uint8_t actuator_recover(void);

typedef enum { ACT_STOP = 0, ACT_FORWARD, ACT_BACKWARD } act_cmd_t;
typedef struct { act_cmd_t cmd; uint8_t duty; } act_msg_t;

static QueueHandle_t act_q;
void actuator_task(void *arg);

/* Scheduler'dan önce bir kez. */
bool actuator_task_create(void)
{
    act_q = xQueueCreate(4U, sizeof(act_msg_t));
    return (act_q != NULL) &&
           (xTaskCreate(actuator_task, "act", 256U, NULL, tskIDLE_PRIORITY + 2U, NULL) == pdPASS);
}

/* Her yerden, ISR hariç; beklemez. false: kuyruk dolu. */
bool actuator_post(act_cmd_t cmd, uint8_t duty)
{
    act_msg_t m = { cmd, duty };
    return xQueueSendToBack(act_q, &m, 0U) == pdPASS;
}

void actuator_task(void *arg)
{
    act_msg_t m;
    (void)arg;

    (void)actuator_init();                       /* hata olsa da görev ayakta kalır */

    for (;;) {
        (void)xQueueReceive(act_q, &m, portMAX_DELAY);
        if (m.cmd == ACT_STOP) { (void)BTM_Brake(); continue; }

        BTM_Status st;
        (void)BTM_GetStatus(&st);
        if ((st.state == BTM_STATE_FAULT) && (actuator_recover() == 0U)) { continue; }

        BTM_Direction dir = (m.cmd == ACT_FORWARD) ? BTM_DIR_FORWARD : BTM_DIR_BACKWARD;
        HAL_StatusTypeDef res = (m.duty >= 100U) ? BTM_Move(dir) : BTM_MovePwm(dir, m.duty);
        if (res != HAL_OK) {
            if (res != HAL_BUSY) { (void)BTM_Brake(); }
            continue;
        }

        uint32_t rem;
        while ((rem = BTM_RemainingMs()) > 0U) {
            /* Hareket boyunca UYU: yoklama yok. STOP dışındaki komut bu sırada atılır. */
            if (xQueueReceive(act_q, &m, pdMS_TO_TICKS(rem)) == pdTRUE) {
                if (m.cmd == ACT_STOP) { break; }
            }
        }
        (void)BTM_Brake();                       /* erken kesme veya emniyet payı */
    }
}
