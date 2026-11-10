/* Süper döngü / tek görevli kullanım (INTEGRATION.md §5-§7).
 * Örnek: SPI2 + TIM4 + CS=PB12. Kendi handle ve pinlerinle değiştir. */
#include <stdbool.h>
#include "BTM9011EP.h"

extern SPI_HandleTypeDef hspi2;
extern TIM_HandleTypeDef htim4;
#define BTM_CS_GPIO_Port GPIOB
#define BTM_CS_Pin       GPIO_PIN_12

static const BTM_Config btm_cfg = {
    .spi         = &hspi2,
    .cs_port     = BTM_CS_GPIO_Port,
    .cs_pin      = BTM_CS_Pin,
    .tim         = &htim4,
    .tim_clk_hz  = 84000000U,  /* timer'ın BESLEME saati (APB çarpanı dahil) */
    .forward_ms  = 6000U,      /* KARTTA ÖLÇ: tam hız strok süresi + pay */
    .backward_ms = 6000U,
};

/* Bir kez, kesmeler açıkken (RTOS'ta scheduler başladıktan sonra, görevden). */
HAL_StatusTypeDef actuator_init(void)
{
    return BTM_Init(&btm_cfg);   /* HAL_ERROR: BTM_GetStatus().fault'a bak */
}

/* Hareketi başlatır ve bitene kadar bekler. duty >= 100 tam hız.
 * stop() true dönerse hareket erken kesilir. */
HAL_StatusTypeDef actuator_move(BTM_Direction dir, uint32_t duty, bool (*stop)(void))
{
    HAL_StatusTypeDef res = (duty >= 100U) ? BTM_Move(dir) : BTM_MovePwm(dir, duty);

    if (res != HAL_OK) {
        /* HAL_BUSY: hiçbir şey olmadı (hat meşgul, ters yön ölü süresi veya
         * ısıl dinlenme). Diğerleri: komut gitmiş olabilir, bir kez daha frenle. */
        if (res != HAL_BUSY) { (void)BTM_Brake(); }
        return res;
    }
    while (BTM_RemainingMs() > 0U) {
        if ((stop != NULL) && stop()) { return BTM_Brake(); }
        HAL_Delay(1);
    }
    return BTM_Brake();          /* süre dolunca sürücü zaten frenledi; emniyet payı */
}

/* Boştayken taze status. HAL_BUSY dönerse st son bilinen değerdir. */
HAL_StatusTypeDef actuator_status(BTM_Status *st)
{
    HAL_StatusTypeDef res = BTM_Poll();
    (void)BTM_GetStatus(st);
    return res;
}

/* Hata kilidinden sınırlı denemeyle çıkış. 1: yeniden kullanılabilir. */
uint8_t actuator_recover(void)
{
    for (uint32_t i = 0U; i < 3U; i++) {
        HAL_StatusTypeDef res = BTM_ClearFault();
        BTM_Status st;

        if (res == HAL_OK) { return 1U; }
        if (res != HAL_BUSY) {
            (void)BTM_GetStatus(&st);
            if (st.fault != BTM_FAULT_NONE) { return 0U; }  /* sebep sürüyor */
        }
        HAL_Delay(10);
    }
    return 0U;
}
