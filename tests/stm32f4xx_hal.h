/* Yalnız derleme/akış doğrulaması için taklit HAL. Hedef donanımda kullanılmaz. */
#ifndef MOCK_STM32F4XX_HAL_H
#define MOCK_STM32F4XX_HAL_H

#include <stdint.h>

typedef enum { HAL_OK = 0, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;

typedef enum { HAL_SPI_STATE_RESET = 0, HAL_SPI_STATE_READY, HAL_SPI_STATE_BUSY_TX_RX } HAL_SPI_StateTypeDef;

typedef struct { HAL_SPI_StateTypeDef State; uint32_t ErrorCode; } SPI_HandleTypeDef;
typedef struct { uint32_t dummy; } GPIO_TypeDef;
typedef struct { uint32_t PSC; uint32_t SR; } TIM_TypeDef;
typedef struct { TIM_TypeDef *Instance; uint32_t ARR; uint32_t CNT; uint32_t CCR1; } TIM_HandleTypeDef;

#define GPIO_PIN_RESET 0U
#define GPIO_PIN_SET   1U
#define TIM_CHANNEL_1  0U
#define TIM_FLAG_UPDATE (1UL << 0)
#define TIM_FLAG_CC1    (1UL << 1)

typedef struct { volatile uint32_t DEMCR; } CoreDebug_Type;
typedef struct { volatile uint32_t CTRL; volatile uint32_t CYCCNT; volatile uint32_t LAR; } DWT_Type;

extern CoreDebug_Type *CoreDebug;
extern uint32_t SystemCoreClock;

/** Gerçek DWT serbest koşan bir sayaçtır. Taklitte her erişimde ilerletiyoruz ki
 *  hem Init'in "sayıyor mu" kontrolü geçsin hem de bekleme döngüleri sonlansın. */
DWT_Type *mock_dwt_tick(void);
#define DWT (mock_dwt_tick())

#define CoreDebug_DEMCR_TRCENA_Msk (1UL << 24)
#define DWT_CTRL_CYCCNTENA_Msk     (1UL << 0)

static inline uint32_t __get_PRIMASK(void) { return 0U; }
static inline uint32_t __get_IPSR(void) { return 0U; }
static inline void __disable_irq(void) { }
static inline void __enable_irq(void) { }
#define __NOP() do { } while (0)
#define __DMB() __atomic_thread_fence(__ATOMIC_SEQ_CST)

uint32_t HAL_GetTick(void);
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, uint32_t state);
HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *h, uint8_t *tx, uint8_t *rx, uint16_t n, uint32_t to);
HAL_StatusTypeDef HAL_SPI_TransmitReceive_IT(SPI_HandleTypeDef *h, uint8_t *tx, uint8_t *rx, uint16_t n);
uint32_t HAL_SPI_GetError(SPI_HandleTypeDef *h);
HAL_StatusTypeDef HAL_TIM_Base_Start_IT(TIM_HandleTypeDef *h);
HAL_StatusTypeDef HAL_TIM_Base_Stop_IT(TIM_HandleTypeDef *h);
HAL_StatusTypeDef HAL_TIM_OC_Start_IT(TIM_HandleTypeDef *h, uint32_t ch);
HAL_StatusTypeDef HAL_TIM_OC_Stop_IT(TIM_HandleTypeDef *h, uint32_t ch);

#define __HAL_TIM_GET_AUTORELOAD(h)        ((h)->ARR)
#define __HAL_TIM_SET_COUNTER(h, v)        ((h)->CNT = (v))
#define __HAL_TIM_SET_COMPARE(h, ch, v)    ((h)->CCR1 = (v))
#define __HAL_TIM_CLEAR_FLAG(h, f)         ((h)->Instance->SR &= ~(uint32_t)(f))

#endif
