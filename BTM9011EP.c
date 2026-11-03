/**
 * @file BTM9011EP.c
 * @brief BTM9011EP SPI sürücüsü. Kullanım için BTM9011EP.h ve README.md.
 *
 * Eşzamanlılık modeli: ana döngü ile ISR'ların paylaştığı kararlar kısa
 * kesme korumasında (lock/unlock) verilir. SPI ve TIM kesmeleri aynı öncelikte
 * olmalıdır; birbirlerini kesmezler.
 */
#include "BTM9011EP.h"

#define BTM_SPI_TIMEOUT_MS   20U   /* Polling aktarım */
#define BTM_REVERSE_GAP_MS   200U  /* PWM yön/istek değişiminde fren aralığı */
#define BTM_PWM_MIN_PERCENT  10U
#define BTM_PWM_MAX_PERCENT  90U
#define BTM_CS_LOW_US        5U    /* Çerçeveler arası CS Low */
#define BTM_CS_SETUP_US      20U   /* CS High -> ilk SCK */
#define BTM_CS_HOLD_US       1U    /* Son SCK -> CS Low (komut burada uygulanır) */

typedef enum
{
    DIAG_IDLE = 0U,
    DIAG_CMD_PENDING,
    DIAG_CMD_WAIT,
    DIAG_SETTLING,
    DIAG_READ_PENDING,
    DIAG_READ_WAIT,
    DIAG_BRAKE_PENDING,
    DIAG_BRAKE_WAIT
} DiagStep;

static BTM9011EP_Config cfg;
static uint8_t initialized;

/* SPI */
static volatile BTM9011EP_Mode mode = BTM9011EP_MODE_NORMAL;
static volatile uint8_t spi_busy;      /* Kesmeli çerçeve sürüyor */
static volatile uint8_t manual_busy;   /* Polling çerçeve sürüyor */
static volatile uint32_t spi_start_tick;
static uint8_t tx_buf, rx_buf;         /* IT aktarımı bitene kadar korunur */
static volatile uint8_t rx_byte;
static volatile uint8_t last_cmd, last_cmd_valid;
static volatile HAL_StatusTypeDef last_status = HAL_OK;

/* PWM */
static volatile uint32_t duty_percent = 50U;
static volatile uint32_t applied_duty;
static volatile uint8_t pending_cmd;   /* 0: bekleyen istek yok */
static volatile uint32_t pending_ms;
static volatile uint8_t run_cmd = BTM9011EP_CMD_BRAKE_LOW;
static volatile uint32_t run_ms;
static volatile uint8_t run_active, restart_pending, stop_brake_sent, stopped_once;
static volatile uint32_t run_start_tick, restart_tick, last_stop_tick;

/* Latch: BTM9011EP_LatchState; PWM bitişinde ISR'dan da güncellenir. */
static volatile uint8_t latch_state = BTM9011EP_LATCH_UNKNOWN;

/* Hata kilidi ve kapanış */
static volatile BTM9011EP_Fault fault = BTM9011EP_FAULT_NONE;
static volatile uint32_t hal_error;
static volatile BTM9011EP_FaultStop fault_stop = BTM9011EP_FAULT_STOP_IDLE;
static volatile HAL_StatusTypeDef fault_stop_status = HAL_OK;
static volatile HAL_StatusTypeDef fault_brake_status = HAL_BUSY;
static volatile uint8_t fault_brake_done;
static uint8_t fault_service_started;
static uint32_t quiesce_tick;
static HAL_StatusTypeDef fault_cleanup_status = HAL_OK;

/* Teşhis: tek test, tek sonuç */
static volatile struct
{
    uint8_t step;         /* DiagStep */
    uint8_t type;         /* BTM9011EP_DiagType */
    uint8_t cancel;
    uint8_t result;       /* BTM9011EP_DiagResult, akış bitince yayınlanır */
    uint8_t sample;       /* Okuma sonucu, son fren bitene kadar bekler */
    uint8_t status;
    uint32_t settle_tick;
    uint32_t sample_tick;
} diag;

static void send_command_it(uint8_t command);
static void record_fault(BTM9011EP_Fault f, HAL_StatusTypeDef status);
static void pwm_stop_progress(void);
static void diag_on_complete(void);
static void diag_service(void);

/* ==================== Yardımcılar ==================== */

static inline uint32_t lock(void)
{
    uint32_t state = __get_PRIMASK();
    __disable_irq();
    return state;
}

static inline void unlock(uint32_t state)
{
    __set_PRIMASK(state);
}

/* Polling HAL çağrıları ve kesme koruması varsayımı için gerekli bağlam. */
static inline uint8_t task_context(void)
{
    return ((__get_IPSR() == 0U) && (__get_PRIMASK() == 0U)) ? 1U : 0U;
}

static void delay_us(uint32_t us)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t cycles = (SystemCoreClock / 1000000U) * us;

    while ((uint32_t)(DWT->CYCCNT - start) < cycles)
    {
        __NOP();
    }
}

static void frame_begin(void)
{
    delay_us(BTM_CS_LOW_US);
    HAL_GPIO_WritePin(cfg.cs_port, cfg.cs_pin, GPIO_PIN_SET);
    delay_us(BTM_CS_SETUP_US);
}

static void frame_end(void)
{
    delay_us(BTM_CS_HOLD_US);
    HAL_GPIO_WritePin(cfg.cs_port, cfg.cs_pin, GPIO_PIN_RESET);
}

static inline uint8_t diag_active(void)
{
    return (diag.step != DIAG_IDLE) ? 1U : 0U;
}

/* Kilit altında çağrılır. */
static uint8_t normal_path_busy(void)
{
    return ((diag_active() != 0U) || (mode != BTM9011EP_MODE_NORMAL) ||
            (spi_busy != 0U) || (manual_busy != 0U)) ? 1U : 0U;
}

static uint32_t limit_duty(uint32_t requested)
{
    if ((requested == 0U) || (requested >= 100U))
    {
        return (requested == 0U) ? 0U : 100U;
    }
    if (requested < BTM_PWM_MIN_PERCENT)
    {
        return BTM_PWM_MIN_PERCENT;
    }
    return (requested > BTM_PWM_MAX_PERCENT) ? BTM_PWM_MAX_PERCENT : requested;
}

/* ==================== Başlatma ve durum ==================== */

HAL_StatusTypeDef BTM9011EP_Init(const BTM9011EP_Config *config)
{
    if ((config == NULL) || (config->spi == NULL) ||
        (config->cs_port == NULL) || (config->cs_pin == 0U))
    {
        return HAL_ERROR;
    }
    if ((initialized != 0U) || (task_context() == 0U))
    {
        return HAL_BUSY;
    }

    cfg = *config;
    initialized = 1U;

    /* CYCCNT sıfırlanmaz; projedeki diğer kullanıcılar etkilenmez. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    HAL_GPIO_WritePin(cfg.cs_port, cfg.cs_pin, GPIO_PIN_RESET);
    return BTM9011EP_TransferByte(BTM9011EP_CMD_BRAKE_LOW, NULL);
}

HAL_StatusTypeDef BTM9011EP_GetStatus(BTM9011EP_Status *status)
{
    uint32_t s;

    if (status == NULL)
    {
        return HAL_ERROR;
    }

    s = lock();
    status->mode = mode;
    /* Hata sonrası kilitli PWM_STOPPING modu tek başına meşguliyet sayılmaz. */
    status->busy = ((diag_active() != 0U) || (spi_busy != 0U) || (manual_busy != 0U) ||
                    (mode == BTM9011EP_MODE_PWM) ||
                    ((mode == BTM9011EP_MODE_PWM_STOPPING) && (fault == BTM9011EP_FAULT_NONE)) ||
                    (fault_stop == BTM9011EP_FAULT_STOP_PENDING) ||
                    (fault_stop == BTM9011EP_FAULT_STOP_BRAKING)) ? 1U : 0U;
    status->last_command = last_cmd;
    status->last_command_valid = last_cmd_valid;
    status->rx_byte = rx_byte;
    status->duty_percent = duty_percent;
    status->fault = fault;
    status->last_status = last_status;
    status->hal_error = hal_error;
    status->fault_stop = fault_stop;
    status->fault_stop_status = fault_stop_status;
    status->diag_type = (BTM9011EP_DiagType)diag.type;
    status->diag_result = (BTM9011EP_DiagResult)diag.result;
    status->diag_status = diag.status;
    status->diag_tick = diag.sample_tick;
    status->latch_state = (fault != BTM9011EP_FAULT_NONE)
                          ? BTM9011EP_LATCH_FAULT : (BTM9011EP_LatchState)latch_state;
    unlock(s);

    return (initialized != 0U) ? HAL_OK : HAL_ERROR;
}

/* ==================== Normal komutlar (polling) ==================== */

HAL_StatusTypeDef BTM9011EP_TransferByte(uint8_t command, uint8_t *response)
{
    uint8_t received = 0U;
    HAL_StatusTypeDef result;
    uint32_t s;

    if (initialized == 0U)
    {
        return HAL_ERROR;
    }
    if (task_context() == 0U)
    {
        return HAL_BUSY;
    }

    s = lock();
    if (fault != BTM9011EP_FAULT_NONE)
    {
        unlock(s);
        return HAL_ERROR;
    }
    if (normal_path_busy() != 0U)
    {
        unlock(s);
        return HAL_BUSY;
    }
    manual_busy = 1U;
    unlock(s);

    frame_begin();
    result = HAL_SPI_TransmitReceive(cfg.spi, &command, &received, 1U,
                                     BTM_SPI_TIMEOUT_MS);
    frame_end();

    s = lock();
    last_status = result;
    if (result == HAL_OK)
    {
        rx_byte = received;
        last_cmd = command;
        last_cmd_valid = 1U;
        if (response != NULL)
        {
            *response = received;
        }
    }
    else if (result != HAL_BUSY)
    {
        record_fault(BTM9011EP_FAULT_SPI, result);
    }
    manual_busy = 0U;
    unlock(s);
    return result;
}

HAL_StatusTypeDef BTM9011EP_Drive(BTM9011EP_Direction direction)
{
    HAL_StatusTypeDef result;

    if ((direction != BTM9011EP_FORWARD) && (direction != BTM9011EP_BACKWARD))
    {
        return HAL_ERROR;
    }
    result = BTM9011EP_TransferByte((direction == BTM9011EP_FORWARD)
                                    ? BTM9011EP_CMD_DRIVE_FORWARD
                                    : BTM9011EP_CMD_DRIVE_BACKWARD, NULL);
    if (result == HAL_OK)
    {
        latch_state = BTM9011EP_LATCH_UNKNOWN;   /* Süresiz sürüş: konum bilinmez. */
    }
    return result;
}

HAL_StatusTypeDef BTM9011EP_Brake(void)
{
    uint32_t s;

    if (initialized == 0U)
    {
        return HAL_ERROR;
    }
    if (task_context() == 0U)
    {
        return HAL_BUSY;
    }

    s = lock();
    if (fault != BTM9011EP_FAULT_NONE)
    {
        unlock(s);
        return HAL_ERROR;
    }
    if (diag_active() != 0U)
    {
        /* Süren çerçeve bitince diag_on_complete iptali freni sıraya alır. */
        diag.cancel = 1U;
        if ((spi_busy == 0U) && (diag.step != DIAG_BRAKE_WAIT))
        {
            diag.step = DIAG_BRAKE_PENDING;
        }
        unlock(s);
        return HAL_OK;
    }
    if (mode == BTM9011EP_MODE_NORMAL)
    {
        unlock(s);
        return BTM9011EP_TransferByte(BTM9011EP_CMD_BRAKE_LOW, NULL);
    }
    if (mode == BTM9011EP_MODE_PWM)
    {
        mode = BTM9011EP_MODE_PWM_STOPPING;
        run_active = 0U;
        restart_pending = 0U;
        pending_cmd = 0U;
        applied_duty = 0U;
        stop_brake_sent = 0U;
        if ((latch_state == BTM9011EP_LATCH_LOCKING) ||
            (latch_state == BTM9011EP_LATCH_UNLOCKING))
        {
            latch_state = BTM9011EP_LATCH_STOPPED;
        }
    }
    unlock(s);
    pwm_stop_progress();
    return HAL_OK;
}

/* ==================== PWM ==================== */

HAL_StatusTypeDef BTM9011EP_PwmStart(BTM9011EP_Direction direction,
                                     uint32_t duty, uint32_t duration_ms)
{
    HAL_StatusTypeDef result = HAL_OK;
    uint32_t s;

    if ((initialized == 0U) || (cfg.timer == NULL) || (duration_ms == 0U) ||
        ((direction != BTM9011EP_FORWARD) && (direction != BTM9011EP_BACKWARD)))
    {
        return HAL_ERROR;
    }
    if (task_context() == 0U)
    {
        return HAL_BUSY;
    }

    s = lock();
    if (fault != BTM9011EP_FAULT_NONE)
    {
        unlock(s);
        return HAL_ERROR;
    }
    if ((diag_active() != 0U) || (mode == BTM9011EP_MODE_PWM_STOPPING) ||
        (manual_busy != 0U) || ((mode == BTM9011EP_MODE_NORMAL) && (spi_busy != 0U)))
    {
        unlock(s);
        return HAL_BUSY;
    }

    /* Timer ISR'ı yön/süre çiftinin yarısını göremez. */
    duty_percent = duty;
    pending_ms = duration_ms;
    pending_cmd = (direction == BTM9011EP_FORWARD)
                  ? BTM9011EP_CMD_DRIVE_FORWARD : BTM9011EP_CMD_DRIVE_BACKWARD;
    latch_state = BTM9011EP_LATCH_UNKNOWN;   /* LatchLock/Unlock hemen yeniden yazar. */

    if (mode == BTM9011EP_MODE_NORMAL)
    {
        mode = BTM9011EP_MODE_PWM;
        __HAL_TIM_SET_COUNTER(cfg.timer, 0U);
        __HAL_TIM_SET_COMPARE(cfg.timer, TIM_CHANNEL_1,
                              __HAL_TIM_GET_AUTORELOAD(cfg.timer) / 2U);
        __HAL_TIM_CLEAR_FLAG(cfg.timer, TIM_FLAG_UPDATE | TIM_FLAG_CC1);

        result = HAL_TIM_OC_Start_IT(cfg.timer, TIM_CHANNEL_1);
        if (result == HAL_OK)
        {
            result = HAL_TIM_Base_Start_IT(cfg.timer);
        }
        if (result != HAL_OK)
        {
            HAL_StatusTypeDef base_stop = HAL_TIM_Base_Stop_IT(cfg.timer);
            HAL_StatusTypeDef oc_stop = HAL_TIM_OC_Stop_IT(cfg.timer, TIM_CHANNEL_1);

            pending_cmd = 0U;
            mode = ((base_stop == HAL_OK) && (oc_stop == HAL_OK))
                   ? BTM9011EP_MODE_NORMAL : BTM9011EP_MODE_PWM_STOPPING;
            record_fault(BTM9011EP_FAULT_TIMER, result);
        }
        last_status = result;
    }
    unlock(s);
    return result;
}

void BTM9011EP_SetDuty(uint32_t percent)
{
    uint32_t s;

    if ((initialized == 0U) || (cfg.timer == NULL))
    {
        return;
    }
    s = lock();
    if (diag_active() == 0U)
    {
        duty_percent = percent;
    }
    unlock(s);
}

/* ==================== Latch ==================== */

static HAL_StatusTypeDef latch_move(uint8_t lock_request)
{
    BTM9011EP_Direction dir = cfg.latch_lock_dir;
    HAL_StatusTypeDef result;
    uint32_t s;

    if ((initialized == 0U) || (cfg.latch_travel_ms == 0U) ||
        (fault != BTM9011EP_FAULT_NONE))
    {
        return HAL_ERROR;
    }
    /* PWM sürerken PwmStart yeni isteği kabul edip yön değiştirirdi; latch'te
     * hareket bitmeden yenisi yok. NORMAL'den çıkışı yalnız komut görevi yapar. */
    if (mode != BTM9011EP_MODE_NORMAL)
    {
        return HAL_BUSY;
    }
    if (lock_request == 0U)
    {
        dir = (dir == BTM9011EP_FORWARD) ? BTM9011EP_BACKWARD : BTM9011EP_FORWARD;
    }
    result = BTM9011EP_PwmStart(dir, (cfg.latch_duty == 0U) ? 100U : cfg.latch_duty,
                                cfg.latch_travel_ms);
    if (result == HAL_OK)
    {
        s = lock();
        latch_state = (lock_request != 0U) ? BTM9011EP_LATCH_LOCKING
                                           : BTM9011EP_LATCH_UNLOCKING;
        unlock(s);
    }
    return result;
}

HAL_StatusTypeDef BTM9011EP_LatchLock(void)
{
    return latch_move(1U);
}

HAL_StatusTypeDef BTM9011EP_LatchUnlock(void)
{
    return latch_move(0U);
}

/* PWM_STOPPING'de iki adım: önce son fren, o çerçeve bitince timer durdurulur.
 * Ana döngüden (Brake) ve SPI tamamlanma ISR'ından çağrılır. */
static void pwm_stop_progress(void)
{
    HAL_StatusTypeDef base_result;
    HAL_StatusTypeDef oc_result;
    uint32_t s = lock();

    if ((mode != BTM9011EP_MODE_PWM_STOPPING) || (cfg.timer == NULL) ||
        (fault != BTM9011EP_FAULT_NONE) || (spi_busy != 0U))
    {
        unlock(s);
        return;
    }
    if (stop_brake_sent == 0U)
    {
        stop_brake_sent = 1U;
        unlock(s);
        send_command_it(BTM9011EP_CMD_BRAKE_LOW);
        return;
    }
    unlock(s);

    base_result = HAL_TIM_Base_Stop_IT(cfg.timer);
    oc_result = HAL_TIM_OC_Stop_IT(cfg.timer, TIM_CHANNEL_1);
    if ((base_result != HAL_OK) || (oc_result != HAL_OK))
    {
        record_fault(BTM9011EP_FAULT_TIMER,
                     (base_result != HAL_OK) ? base_result : oc_result);
        return;
    }
    __HAL_TIM_CLEAR_FLAG(cfg.timer, TIM_FLAG_UPDATE | TIM_FLAG_CC1);
    last_stop_tick = HAL_GetTick();
    stopped_once = 1U;
    stop_brake_sent = 0U;
    s = lock();
    /* Süre doldu; Brake ile kesilen hareket STOPPED olarak kalır. */
    if (latch_state == BTM9011EP_LATCH_LOCKING)
    {
        latch_state = BTM9011EP_LATCH_LOCKED;
    }
    else if (latch_state == BTM9011EP_LATCH_UNLOCKING)
    {
        latch_state = BTM9011EP_LATCH_UNLOCKED;
    }
    mode = BTM9011EP_MODE_NORMAL;
    unlock(s);
}

/* ==================== Kesmeli SPI çerçevesi ==================== */

static void send_command_it(uint8_t command)
{
    HAL_StatusTypeDef start;
    uint32_t s;

    /* Hata kilidinde yalnız Service'in kapanış freni geçer. */
    if ((fault != BTM9011EP_FAULT_NONE) &&
        ((fault_stop != BTM9011EP_FAULT_STOP_BRAKING) ||
         (command != BTM9011EP_CMD_BRAKE_LOW)))
    {
        return;
    }
    if (spi_busy != 0U)
    {
        record_fault(BTM9011EP_FAULT_OVERLAP, HAL_BUSY);
        return;
    }

    spi_start_tick = HAL_GetTick();
    spi_busy = 1U;
    tx_buf = command;
    frame_begin();

    if (fault == BTM9011EP_FAULT_NONE)
    {
        last_status = HAL_BUSY;
    }
    start = HAL_SPI_TransmitReceive_IT(cfg.spi, &tx_buf, &rx_buf, 1U);

    /* Aktarım HAL dönmeden bitmiş olabilir; callback'in sonucunu ezme. */
    s = lock();
    if ((spi_busy != 0U) && (fault == BTM9011EP_FAULT_NONE))
    {
        last_status = start;
    }
    unlock(s);

    if (start != HAL_OK)
    {
        frame_end();
        spi_busy = 0U;
        if (fault == BTM9011EP_FAULT_NONE)
        {
            record_fault(BTM9011EP_FAULT_SPI, start);
        }
        else if (fault_stop == BTM9011EP_FAULT_STOP_BRAKING)
        {
            fault_brake_status = start;
            fault_brake_done = 1U;
        }
    }
}

/* ==================== HAL callbackleri ==================== */

/* Periyot başında sürüş komutu, compare anında fren: duty = sürüş / periyot. */
void BTM9011EP_OnTimerPeriod(TIM_HandleTypeDef *htim)
{
    uint32_t now;
    uint32_t duty;

    if ((initialized == 0U) || (htim == NULL) || (htim != cfg.timer) ||
        (mode != BTM9011EP_MODE_PWM) || (fault != BTM9011EP_FAULT_NONE) ||
        (manual_busy != 0U))
    {
        return;
    }
    now = HAL_GetTick();

    if (pending_cmd != 0U)
    {
        run_cmd = pending_cmd;
        run_ms = pending_ms;
        pending_cmd = 0U;

        if (run_active != 0U)
        {
            /* Çalışırken yeni istek: önce fren, sonra fren aralığı. */
            run_active = 0U;
            applied_duty = 0U;
            restart_pending = 1U;
            restart_tick = now;
            send_command_it(BTM9011EP_CMD_BRAKE_LOW);
            return;
        }
        if (restart_pending != 0U)
        {
            restart_tick = now;   /* Aralıkta gelen yeni istek aralığı yeniler. */
            return;
        }
        if ((stopped_once != 0U) &&
            ((uint32_t)(now - last_stop_tick) < BTM_REVERSE_GAP_MS))
        {
            restart_pending = 1U; /* Yakın zamandaki duruşun kalan aralığını koru. */
            restart_tick = last_stop_tick;
            return;
        }
        run_active = 1U;
        run_start_tick = now;
    }

    if (restart_pending != 0U)
    {
        if ((uint32_t)(now - restart_tick) < BTM_REVERSE_GAP_MS)
        {
            return;
        }
        restart_pending = 0U;
        run_active = 1U;
        run_start_tick = now;
    }

    if (run_active == 0U)
    {
        return;
    }

    if ((uint32_t)(now - run_start_tick) >= run_ms)
    {
        mode = BTM9011EP_MODE_PWM_STOPPING;
        run_active = 0U;
        applied_duty = 0U;
        stop_brake_sent = 0U;
        pwm_stop_progress();
        return;
    }

    duty = limit_duty(duty_percent);
    applied_duty = duty;
    if (duty == 0U)
    {
        send_command_it(BTM9011EP_CMD_BRAKE_LOW);
        return;
    }
    if (duty < 100U)
    {
        __HAL_TIM_SET_COMPARE(htim, TIM_CHANNEL_1,
                              ((__HAL_TIM_GET_AUTORELOAD(htim) + 1U) * duty) / 100U);
    }
    send_command_it(run_cmd);
}

void BTM9011EP_OnTimerCompare(TIM_HandleTypeDef *htim)
{
    if ((initialized == 0U) || (htim == NULL) || (htim != cfg.timer) ||
        (htim->Channel != HAL_TIM_ACTIVE_CHANNEL_1) ||
        (mode != BTM9011EP_MODE_PWM) || (fault != BTM9011EP_FAULT_NONE) ||
        (manual_busy != 0U))
    {
        return;
    }
    if ((run_active != 0U) && (applied_duty > 0U) && (applied_duty < 100U))
    {
        send_command_it(BTM9011EP_CMD_BRAKE_LOW);
    }
}

void BTM9011EP_OnSpiComplete(SPI_HandleTypeDef *hspi)
{
    if ((initialized == 0U) || (hspi != cfg.spi) || (spi_busy == 0U))
    {
        return;
    }

    frame_end();
    rx_byte = rx_buf;
    last_cmd = tx_buf;
    last_cmd_valid = 1U;
    spi_busy = 0U;

    if (fault == BTM9011EP_FAULT_NONE)
    {
        last_status = HAL_OK;
        diag_on_complete();
        pwm_stop_progress();
    }
    else if (fault_stop == BTM9011EP_FAULT_STOP_BRAKING)
    {
        fault_brake_status = HAL_OK;
        fault_brake_done = 1U;
    }
}

void BTM9011EP_OnSpiError(SPI_HandleTypeDef *hspi)
{
    if ((initialized == 0U) || (hspi != cfg.spi) || (spi_busy == 0U))
    {
        return;
    }

    if (fault == BTM9011EP_FAULT_NONE)
    {
        record_fault(BTM9011EP_FAULT_SPI, HAL_ERROR);
    }
    else if (fault_stop == BTM9011EP_FAULT_STOP_BRAKING)
    {
        fault_brake_status = HAL_ERROR;
        fault_brake_done = 1U;
    }
    frame_end();
    spi_busy = 0U;
}

/* ==================== Hata kilidi ve Service ==================== */

/* Yalnız ilk hata kaydedilir; sürüş kapatılır ve kapanış Service'e bırakılır. */
static void record_fault(BTM9011EP_Fault f, HAL_StatusTypeDef status)
{
    uint32_t s = lock();

    if (fault == BTM9011EP_FAULT_NONE)
    {
        fault = f;
        last_status = status;
        hal_error = (f == BTM9011EP_FAULT_TIMER) ? 0U : HAL_SPI_GetError(cfg.spi);
        if (diag_active() != 0U)
        {
            diag.step = DIAG_IDLE;
            diag.cancel = 0U;
            diag.result = BTM9011EP_DIAG_RESULT_FAILED;
        }
        mode = BTM9011EP_MODE_PWM_STOPPING;
        run_active = 0U;
        restart_pending = 0U;
        pending_cmd = 0U;
        applied_duty = 0U;
        fault_stop = BTM9011EP_FAULT_STOP_PENDING;
        fault_stop_status = HAL_BUSY;
    }
    unlock(s);
}

/* SPI'ı beklemeden durdurur. BSY sürerse HAL_BUSY; Service tekrar dener. */
static HAL_StatusTypeDef quiesce_spi(void)
{
    HAL_StatusTypeDef result;
    uint32_t s = lock();

    __HAL_SPI_DISABLE_IT(cfg.spi, SPI_IT_TXE | SPI_IT_RXNE | SPI_IT_ERR);
    __HAL_SPI_DISABLE(cfg.spi);

    /* DMA bu sürücüde kullanılmaz; DMA açıksa HAL abort beklemesine girme. */
    if (READ_BIT(cfg.spi->Instance->CR2, SPI_CR2_TXDMAEN | SPI_CR2_RXDMAEN) != 0U)
    {
        unlock(s);
        return HAL_ERROR;
    }
    if (__HAL_SPI_GET_FLAG(cfg.spi, SPI_FLAG_BSY) != RESET)
    {
        unlock(s);
        return HAL_BUSY;
    }

    /* IT/DMA istekleri kapalıyken HAL_SPI_Abort bekleme dallarına girmez. */
    result = HAL_SPI_Abort(cfg.spi);
    frame_end();
    spi_busy = 0U;
    if (result == HAL_OK)
    {
        result = HAL_SPI_Init(cfg.spi);
    }
    unlock(s);
    return result;
}

static void fault_service(uint32_t now)
{
    HAL_StatusTypeDef result;
    uint32_t s;

    if ((fault_stop == BTM9011EP_FAULT_STOP_COMPLETE) ||
        (fault_stop == BTM9011EP_FAULT_STOP_FAILED))
    {
        return;
    }

    if (fault_service_started == 0U)
    {
        fault_service_started = 1U;
        quiesce_tick = now;
        if (cfg.timer != NULL)
        {
            HAL_StatusTypeDef base_result = HAL_TIM_Base_Stop_IT(cfg.timer);
            HAL_StatusTypeDef oc_result = HAL_TIM_OC_Stop_IT(cfg.timer, TIM_CHANNEL_1);

            __HAL_TIM_CLEAR_FLAG(cfg.timer, TIM_FLAG_UPDATE | TIM_FLAG_CC1);
            if ((base_result != HAL_OK) || (oc_result != HAL_OK))
            {
                fault_cleanup_status = (base_result != HAL_OK) ? base_result : oc_result;
            }
        }
    }

    if (fault_stop == BTM9011EP_FAULT_STOP_PENDING)
    {
        result = quiesce_spi();
        if ((result == HAL_BUSY) &&
            ((uint32_t)(now - quiesce_tick) < BTM9011EP_ASYNC_TIMEOUT_MS))
        {
            return;
        }
        if (result != HAL_OK)
        {
            fault_brake_status = (result == HAL_BUSY) ? HAL_TIMEOUT : result;
            fault_brake_done = 1U;
        }
        else
        {
            /* Faz HAL çağrısından önce yayınlanır; anında gelen callback geçerlidir. */
            fault_brake_status = HAL_BUSY;
            fault_brake_done = 0U;
            fault_stop = BTM9011EP_FAULT_STOP_BRAKING;
            send_command_it(BTM9011EP_CMD_BRAKE_LOW);
        }
    }

    s = lock();
    if ((fault_stop == BTM9011EP_FAULT_STOP_BRAKING) && (spi_busy != 0U) &&
        ((uint32_t)(HAL_GetTick() - spi_start_tick) >= BTM9011EP_ASYNC_TIMEOUT_MS))
    {
        fault_brake_status = HAL_TIMEOUT;
        fault_brake_done = 1U;
    }
    if (fault_brake_done != 0U)
    {
        if (fault_brake_status != HAL_OK)
        {
            /* Yeni deneme yok: SPI kesmeleri kapalı, CS pasif kalır. */
            __HAL_SPI_DISABLE_IT(cfg.spi, SPI_IT_TXE | SPI_IT_RXNE | SPI_IT_ERR);
            __HAL_SPI_DISABLE(cfg.spi);
            frame_end();
            spi_busy = 0U;
            fault_cleanup_status = fault_brake_status;
        }
        fault_stop_status = fault_cleanup_status;
        fault_stop = (fault_cleanup_status == HAL_OK)
                     ? BTM9011EP_FAULT_STOP_COMPLETE : BTM9011EP_FAULT_STOP_FAILED;
    }
    unlock(s);
}

void BTM9011EP_Service(void)
{
    uint32_t now;
    uint8_t faulted;
    uint32_t s;

    if ((initialized == 0U) || (task_context() == 0U))
    {
        return;
    }

    /* Zaman aşımı kararı ile callback'in tamamlanma yayını birbirini kesemez. */
    s = lock();
    now = HAL_GetTick();
    if ((fault == BTM9011EP_FAULT_NONE) && (spi_busy != 0U) &&
        ((uint32_t)(now - spi_start_tick) >= BTM9011EP_ASYNC_TIMEOUT_MS))
    {
        record_fault(BTM9011EP_FAULT_TIMEOUT, HAL_TIMEOUT);
    }
    faulted = (fault != BTM9011EP_FAULT_NONE) ? 1U : 0U;
    unlock(s);

    if (faulted != 0U)
    {
        fault_service(now);
    }
    else
    {
        diag_service();
    }
}

/* ==================== Teşhis ==================== */

HAL_StatusTypeDef BTM9011EP_DiagStart(BTM9011EP_DiagType type)
{
    uint8_t last;
    uint32_t s;

    if ((initialized == 0U) ||
        ((type != BTM9011EP_DIAG_OPEN_LOAD_HSA) && (type != BTM9011EP_DIAG_OPEN_LOAD_HSB) &&
         (type != BTM9011EP_DIAG_SHORT_TO_GND)))
    {
        return HAL_ERROR;
    }
    if (task_context() == 0U)
    {
        return HAL_BUSY;
    }

    s = lock();
    if (fault != BTM9011EP_FAULT_NONE)
    {
        unlock(s);
        return HAL_ERROR;
    }
    /* Yalnız bilinen güvenli son komuttan sonra: standby (EN=0), coast veya fren. */
    last = last_cmd & (uint8_t)~(BTM9011EP_MASK_SDO_SEL | BTM9011EP_MASK_SEN_EN |
                                 BTM9011EP_MASK_SR);
    if ((normal_path_busy() != 0U) || (last_cmd_valid == 0U) ||
        (((last & BTM9011EP_STATUS_EN) != 0U) && (last != BTM9011EP_CMD_COAST) &&
         (last != BTM9011EP_CMD_BRAKE_LOW) && (last != BTM9011EP_CMD_BRAKE_HIGH)))
    {
        unlock(s);
        return HAL_BUSY;
    }
    diag.type = (uint8_t)type;
    diag.result = BTM9011EP_DIAG_RESULT_RUNNING;
    diag.sample = BTM9011EP_DIAG_RESULT_INVALID;
    diag.cancel = 0U;
    diag.step = DIAG_CMD_PENDING;
    unlock(s);
    return HAL_OK;
}

/* SPI tamamlanma ISR'ından: yalnız sonucu ve adımı kaydeder, çerçeve başlatmaz. */
static void diag_on_complete(void)
{
    switch (diag.step)
    {
        case DIAG_CMD_WAIT:
            diag.settle_tick = HAL_GetTick();
            diag.step = (diag.cancel != 0U) ? DIAG_BRAKE_PENDING : DIAG_SETTLING;
            break;

        case DIAG_READ_WAIT:
            /* İlk yanıt önceki çerçeveye aittir; geçerli örnek ikinci yanıttır. */
            if ((diag.cancel == 0U) && ((rx_buf & BTM9011EP_STATUS_SDO_SEL) != 0U))
            {
                diag.status = rx_buf;
                diag.sample_tick = HAL_GetTick();
                diag.sample = ((rx_buf & BTM9011EP_STATUS_OL) != 0U)
                              ? BTM9011EP_DIAG_RESULT_DETECTED
                              : BTM9011EP_DIAG_RESULT_NO_FLAG;
            }
            diag.step = DIAG_BRAKE_PENDING;
            break;

        case DIAG_BRAKE_WAIT:
            diag.result = (diag.cancel != 0U) ? BTM9011EP_DIAG_RESULT_CANCELLED : diag.sample;
            diag.cancel = 0U;
            diag.step = DIAG_IDLE;
            break;

        default:
            break;
    }
}

/* Service'ten: her çağrıda en çok bir teşhis çerçevesi başlatır. */
static void diag_service(void)
{
    uint8_t command;
    uint32_t s = lock();

    if ((diag_active() == 0U) || (spi_busy != 0U) || (manual_busy != 0U))
    {
        unlock(s);
        return;
    }
    if (diag.step == DIAG_SETTLING)
    {
        if ((uint32_t)(HAL_GetTick() - diag.settle_tick) <= BTM9011EP_DIAG_SETTLE_MS)
        {
            unlock(s);
            return;
        }
        diag.step = DIAG_READ_PENDING;
    }

    if (diag.step == DIAG_BRAKE_PENDING)
    {
        command = BTM9011EP_CMD_BRAKE_LOW;
        diag.step = DIAG_BRAKE_WAIT;
    }
    else if ((diag.step == DIAG_CMD_PENDING) || (diag.step == DIAG_READ_PENDING))
    {
        /* Test komutu iki kez gönderilir; SDO_SEL ile ikinci yanıt durum baytıdır. */
        command = (diag.type == BTM9011EP_DIAG_OPEN_LOAD_HSA) ? 0xC1U :
                  (diag.type == BTM9011EP_DIAG_OPEN_LOAD_HSB) ? 0xA9U : 0x89U;
        diag.step = (diag.step == DIAG_CMD_PENDING) ? DIAG_CMD_WAIT : DIAG_READ_WAIT;
    }
    else
    {
        unlock(s);
        return;
    }
    /* Adım HAL çağrısından önce yayınlanır; HAL dönmeden callback gelebilir. */
    unlock(s);
    send_command_it(command);
}
