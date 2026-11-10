/* İlk deneme kodu (INTEGRATION.md §5-§6). Parçalar CubeMX'in USER CODE
 * bloklarına kopyalanır; bu dosya olduğu gibi derlemeye EKLENMEZ. */
#include "main.h"
#include "cmsis_os.h"
#include "BTM9011EP.h"

/* main.c içinde CubeMX bunları zaten tanımlar; yalnız bu dosyayı derlemek için: */
extern SPI_HandleTypeDef hspi2;
extern TIM_HandleTypeDef htim4;

/* ---- USER CODE BEGIN PV ---- */
static const BTM_Config btm_cfg = {
    .spi         = &hspi2,
    .cs_port     = BTM_CS_GPIO_Port,
    .cs_pin      = BTM_CS_Pin,
    .tim         = &htim4,
    .tim_clk_hz  = 84000000U,  /* timer'ın BESLEME saati; PSC ile bölünmüş değer değil */
    .forward_ms  = 6000U,      /* KARTTA ÖLÇ: tam hız strok süresi + pay */
    .backward_ms = 6000U,
};
static volatile BTM_Status btm_dbg;   /* debugger'da izlemek için */
/* ---- USER CODE END PV ---- */

/* Aşama 1: motor BAĞLI DEĞİL. Sahip görevin (RTOS) başında veya USER CODE 2'de. */
void btm_line_check(void)
{
    BTM_Status st;

    if (BTM_Init(&btm_cfg) != HAL_OK) {
        (void)BTM_GetStatus(&st);          /* st.fault, st.echo_byte'a bak */
        btm_dbg = st;
        Error_Handler();
    }
    (void)BTM_Poll();                      /* boştayken taze status */
    (void)BTM_GetStatus(&st);
    btm_dbg = st;                          /* beklenen: fault==NONE, echo_byte==last_cmd(0x11) */
}

/* Aşama 2: motor bağlı. Bir kez ileri, bir kez geri. */
void btm_move_test(void)
{
    if (BTM_Move(BTM_DIR_FORWARD) == HAL_OK) {
        while (BTM_RemainingMs() > 0U) { osDelay(10); }   /* bare-metal: HAL_Delay(10) */
    }

    osDelay(500);                          /* ters yön ölü süresi (100 ms) + pay */

    if (BTM_Move(BTM_DIR_BACKWARD) == HAL_OK) {
        while (BTM_RemainingMs() > 0U) { osDelay(10); }
    }

    BTM_Status st;
    (void)BTM_GetStatus(&st);
    btm_dbg = st;
}
