/**
 * @file BTM9011EP.c
 * @brief BTM9011EP SPI sürücüsü. Sözleşme ve kurallar için BTM9011EP.h'ye bak.
 *
 * TEK SÜRÜŞ YOLU: tam hız da PWM de timer üzerinden yürür. Tam hız, "duty %100,
 * compare çerçevesi yok" demektir. Süreyi her iki kipte de timer sayar ve dolunca
 * sürücü kendi frenler; uygulamanın sonda bir şey yapması gerekmez.
 *
 * EŞZAMANLILIK — kesme maskelemesi YOK:
 *   - Komut fonksiyonları yalnız sahip görevden çağrılır (tek yazar).
 *   - Sürücünün SPI ve TIM kesmeleri birbirini kesmez (eşit öncelik şartı).
 *   - `state` ISR'ların eylem kapısıdır: ISR yalnız MOVING iken çerçeve gönderir.
 *     Durdururken ÖNCE `state` yazılır, SONRA timer durdurulur; başlatırken sıra
 *     terstir. Böylece ISR hiçbir zaman yarı kurulmuş bir hareket görmez.
 *   - Paylaşılan alanlar hizalı 8/32-bit; tekil erişimleri Cortex-M'de atomiktir.
 */
#include "BTM9011EP.h"
#include <stddef.h>

/* Datasheet Tablo 15 */
#define BTM_TWAKEUP_US 20U   /* tWAKEUP: yalnız standby'dan ilk çerçevede (Init) */
#define BTM_TLEAD_US   1U    /* tlead min 250 ns; yalnız görev yolunda, pay olarak */
#define BTM_TCSL_US    5U    /* tCSL min 4 us */
#define BTM_SPIN_GUARD 200000U /* Görev yolu bekleme döngülerinin emniyet sayacı */

/* cfg/cycles_per_us/tcsl_cycles yalnız Init içinde, `inited` set edilmeden önce
 * yazılır; ISR'lar önce `inited`'i kontrol eder ve Init'teki __DMB() bu yazmaları
 * ondan önce sıraya sokar (yayınlama kalıbı). */
static BTM_Config cfg;
static volatile uint8_t inited;
static uint32_t cycles_per_us = 1U;
static volatile uint32_t tcsl_cycles;
static volatile uint8_t awake;            /* cs_drop() ISR'dan yazar, xfer() okur */

static volatile uint32_t state_seq;
static volatile BTM_State state = BTM_STATE_IDLE;
static volatile uint32_t cs_low_cycles;
static volatile uint8_t tx_byte;
static volatile uint8_t rx_byte;

static volatile uint8_t last_cmd = BTM_CMD_BRAKE;
static volatile uint8_t pwm_cmd;
static volatile uint32_t periods_left;
static volatile uint32_t duty_percent;
static volatile uint32_t move_deadline;
static volatile uint32_t finish_tick;

static volatile uint8_t status_byte;
static volatile uint8_t status_valid;
static volatile uint32_t dropped_frames;
static volatile uint32_t drop_run;
static volatile uint32_t uv_run;
static volatile BTM_Fault fault = BTM_FAULT_NONE;
static volatile uint32_t hal_error;

/* ==================== Yardımcılar ==================== */

static void publish(void) { __DMB(); state_seq++; }
static void set_state(BTM_State s) { state = s; publish(); }

static uint8_t task_context(void)
{
    return ((__get_IPSR() == 0U) && ((__get_PRIMASK() & 1U) == 0U)) ? 1U : 0U;
}

/** Yalnız görev yolunda. ISR'larda bekleme yapılmaz. */
static void delay_us(uint32_t us)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t target = cycles_per_us * us;
    uint32_t guard = 0U;

    while ((DWT->CYCCNT - start) < target)
    {
        if (++guard > BTM_SPIN_GUARD) { break; }
    }
}

static uint8_t dwt_start(void)
{
    uint32_t a, b;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    a = DWT->CYCCNT; __NOP(); __NOP(); __NOP(); __NOP(); b = DWT->CYCCNT;
    return (b != a) ? 1U : 0U;
}

/** CS yeterince uzun Low kaldı mı (tCSL). ISR bunu BEKLEMEZ, sorar. */
static uint8_t cs_gap_ok(void)
{
    return ((DWT->CYCCNT - cs_low_cycles) >= tcsl_cycles) ? 1U : 0U;
}

static void cs_drop(void)
{
    /* tlag (125 ns): buraya gelene kadarki komut yükü bunu zaten aşar. */
    HAL_GPIO_WritePin(cfg.cs_port, cfg.cs_pin, GPIO_PIN_RESET);
    cs_low_cycles = DWT->CYCCNT;
    awake = 1U;
}

/** Çerçeve periyoda sığmalı: duty hem sürüş hem fren penceresi için sınırlanır. */
static uint32_t clamp_duty(uint32_t d)
{
    uint32_t period_us = 1000000U / BTM_PWM_HZ;
    uint32_t floor_pct = ((BTM_FRAME_US * 100U) + period_us - 1U) / period_us;

    if (floor_pct < 1U)  { floor_pct = 1U; }
    if (floor_pct > 45U) { floor_pct = 45U; }
    if (d < floor_pct)   { d = floor_pct; }
    if (d > (100U - floor_pct)) { d = 100U - floor_pct; }
    return d;
}

static uint8_t dir_cmd(BTM_Direction d)
{
    return (d == BTM_DIR_FORWARD) ? (uint8_t)BTM_CMD_FORWARD : (uint8_t)BTM_CMD_BACKWARD;
}

static uint32_t dir_travel_ms(BTM_Direction d)
{
    return (d == BTM_DIR_FORWARD) ? cfg.forward_ms : cfg.backward_ms;
}

static void stop_timer(void)
{
    if (cfg.tim != NULL)
    {
        (void)HAL_TIM_OC_Stop_IT(cfg.tim, TIM_CHANNEL_1);
        (void)HAL_TIM_Base_Stop_IT(cfg.tim);
    }
}

/* ==================== Hata ==================== */

static void record_fault(BTM_Fault f)
{
    state = BTM_STATE_FAULT;   /* ÖNCE ISR'ın eylem kapısını kapat */
    __DMB();
    if (fault == BTM_FAULT_NONE)
    {
        hal_error = (cfg.spi != NULL) ? HAL_SPI_GetError(cfg.spi) : 0U;
        __DMB();
        fault = f;
    }
    publish();
    stop_timer();
}

static void drop_frame(void)
{
    dropped_frames++;
    drop_run++;
    if (drop_run >= BTM_DROP_LIMIT) { record_fault(BTM_FAULT_OVERLAP); }
}

/** Status baytını değerlendirir; ISR'dan da çağrılır.
 *  OL biti bilerek bakılmaz: yalnız teşhis desenlerinde geçerlidir, sürüş ve fren
 *  desenlerinde değil (datasheet Tablo 12). */
static void eval_status(uint8_t s)
{
    /* 0xFF: MISO pull-up'lı, cihaz cevap vermiyor. Bit 7'si 1 olduğu için
     * "bit7 set ise status baytıdır" testini tek başına geçerdi. */
    if ((s == 0xFFU) || ((s & BTM_ST_SDO_SEL) == 0U))
    {
        record_fault(BTM_FAULT_COMM);
        return;
    }
    status_byte = s;
    status_valid = 1U;

    if ((s & BTM_ST_HARD) != 0U) { record_fault(BTM_FAULT_DEVICE); return; }

    /* UV teyit ister: kalkış akımı geçici olarak set edebilir. */
    if ((s & BTM_ST_UV) != 0U)
    {
        uv_run++;
        if (uv_run >= BTM_UV_CONFIRM) { record_fault(BTM_FAULT_DEVICE); }
    }
    else
    {
        uv_run = 0U;
    }
}

/* ==================== Çerçeveler ==================== */

/** Polling çerçevesi (görev yolu). Uçuştaki aktarımı sınırlı süre bekler; bitmezse
 *  HAL_BUSY döner — bu GEÇİCİ bir durumdur, fault değildir. */
static HAL_StatusTypeDef xfer(uint8_t out, uint8_t *in)
{
    uint8_t tx = out, rx = 0U;
    uint32_t guard = 0U, t0;
    HAL_StatusTypeDef res;

    t0 = HAL_GetTick();
    while (cfg.spi->State != HAL_SPI_STATE_READY)
    {
        if ((HAL_GetTick() - t0) > BTM_INFLIGHT_WAIT_MS) { return HAL_BUSY; }
    }
    while (!cs_gap_ok())
    {
        if (++guard > BTM_SPIN_GUARD) { break; }
    }

    HAL_GPIO_WritePin(cfg.cs_port, cfg.cs_pin, GPIO_PIN_SET);
    delay_us(awake ? BTM_TLEAD_US : BTM_TWAKEUP_US);
    res = HAL_SPI_TransmitReceive(cfg.spi, &tx, &rx, 1U, BTM_SPI_TIMEOUT_MS);
    cs_drop();

    if ((res == HAL_OK) && (in != NULL)) { *in = rx; }
    return res;
}

/**
 * Doğrulanmış komut — üç çerçeve. Cevap bir çerçeve gecikmeli olduğu için
 * (datasheet Şekil 27: cevap CS yükselirken yüklenir, komut CS düşerken uygulanır)
 * gönderilen baytın echo'su ancak bir sonraki çerçevede görülür:
 *   1) komut, SDO_SEL=0  -> cevap önceki çerçeveye ait, atılır
 *   2) komut, SDO_SEL=1  -> control byte echo'su; paketin gittiğini doğrular
 *   3) komut, SDO_SEL=1  -> status baytı; saklanır ve değerlendirilir
 * Üçü de aynı komutu uygular (SDO_SEL biti köprü çıkışlarını etkilemez).
 */
static HAL_StatusTypeDef send_verified(uint8_t cmd)
{
    uint8_t base = (uint8_t)(cmd & (uint8_t)~BTM_SDO_SEL);
    uint8_t echo = 0U, stat = 0U;
    HAL_StatusTypeDef res;

    res = xfer(base, NULL);
    if (res == HAL_BUSY) { return HAL_BUSY; }
    if (res != HAL_OK)   { record_fault(BTM_FAULT_SPI); return res; }

    res = xfer((uint8_t)(base | BTM_SDO_SEL), &echo);
    if (res == HAL_BUSY) { return HAL_BUSY; }
    if (res != HAL_OK)   { record_fault(BTM_FAULT_SPI); return res; }

    res = xfer((uint8_t)(base | BTM_SDO_SEL), &stat);
    if (res == HAL_BUSY) { return HAL_BUSY; }
    if (res != HAL_OK)   { record_fault(BTM_FAULT_SPI); return res; }

    last_cmd = base;
    drop_run = 0U;

    /* Echo çerçevesinde bit 7'nin 0 olması beklenir (çerçeve 1 SDO_SEL=0 idi);
     * 0xFF burada beklenen 0'a uymadığı için yakalanır. */
    if (echo != base) { record_fault(BTM_FAULT_COMM); return HAL_ERROR; }

    eval_status(stat);
    publish();
    return (fault != BTM_FAULT_NONE) ? HAL_ERROR : HAL_OK;
}

/** Kesmeli çerçeve (ISR yolu): meşgul bekleme yok, kesme maskeleme yok.
 *  @return 1 aktarım başladı, 0 çerçeve düştü. */
static uint8_t send_it(uint8_t cmd)
{
    if ((cfg.spi->State != HAL_SPI_STATE_READY) || (!cs_gap_ok()))
    {
        drop_frame();
        return 0U;
    }
    drop_run = 0U;

    /* SDO_SEL sabit 1: her cevap status baytı olur (bir çerçeve gecikmeli). */
    tx_byte = (uint8_t)(cmd | BTM_SDO_SEL);
    HAL_GPIO_WritePin(cfg.cs_port, cfg.cs_pin, GPIO_PIN_SET);

    /* tlead (250 ns) için açık gecikme konmadı: HAL_SPI_TransmitReceive_IT'in
     * kurulum yükü bunu zaten aşar ve ISR'da bekleme yapılmaz. Osiloskopla
     * doğrulanmalı. */
    if (HAL_SPI_TransmitReceive_IT(cfg.spi, (uint8_t *)&tx_byte, (uint8_t *)&rx_byte, 1U) != HAL_OK)
    {
        cs_drop();
        record_fault(BTM_FAULT_SPI);
        return 0U;
    }
    last_cmd = cmd;
    return 1U;
}

/** Süre dolunca periyot ISR'ından: timer durur, son fren gider. */
static void move_finish(void)
{
    stop_timer();
    finish_tick = HAL_GetTick();
    set_state(BTM_STATE_FINISHING);
    /* Motor bu anda zaten frende olabilir (PWM'de önceki compare çerçevesi).
     * Son fren başlatılamazsa boşa geç. */
    if (!send_it(BTM_CMD_BRAKE)) { set_state(BTM_STATE_IDLE); }
}

/** Son fren çerçevesinin tamamlanma geri çağrısı hiç gelmezse (callback bağlı
 *  değil / SPI takıldı) sürücü FINISHING'de kilitlenmesin. Son GÖNDERİLEN komut
 *  zaten frendir, bu yüzden ek fren gerekmez. */
static void release_stuck_finish(void)
{
    if ((state == BTM_STATE_FINISHING) &&
        ((HAL_GetTick() - finish_tick) > BTM_FINISH_TIMEOUT_MS))
    {
        set_state(BTM_STATE_IDLE);
    }
}

/* ==================== API ==================== */

HAL_StatusTypeDef BTM_Init(const BTM_Config *config)
{
    if (inited) { return HAL_BUSY; }
    if ((config == NULL) || (config->spi == NULL) || (config->cs_port == NULL) ||
        (config->tim == NULL)) { return HAL_ERROR; }
    if ((config->forward_ms == 0U) || (config->backward_ms == 0U)) { return HAL_ERROR; }

    cfg = *config;

    if (!dwt_start())
    {
        fault = BTM_FAULT_DWT;
        set_state(BTM_STATE_FAULT);
        return HAL_ERROR;
    }
    cycles_per_us = SystemCoreClock / 1000000U;
    if (cycles_per_us == 0U) { cycles_per_us = 1U; }
    tcsl_cycles = cycles_per_us * BTM_TCSL_US;

    /* Cihaz reset sonrası standby'da: ilk çerçeve tWAKEUP (20 us) ister. */
    awake = 0U;
    HAL_GPIO_WritePin(cfg.cs_port, cfg.cs_pin, GPIO_PIN_RESET);
    cs_low_cycles = DWT->CYCCNT;
    set_state(BTM_STATE_IDLE);

    __DMB();        /* cfg ve türev sabitler ISR'lara ancak bundan sonra görünsün */
    inited = 1U;

    /* Hattı doğrulanmış fren komutuyla sına. */
    return (send_verified(BTM_CMD_BRAKE) != HAL_OK) ? HAL_ERROR : HAL_OK;
}

static HAL_StatusTypeDef start_move(BTM_Direction dir, uint32_t duty)
{
    uint32_t travel, arr;
    HAL_StatusTypeDef res;

    if (!inited)         { return HAL_ERROR; }
    if (!task_context()) { return HAL_BUSY; }
    if (fault != BTM_FAULT_NONE) { return HAL_ERROR; }
    if ((dir != BTM_DIR_FORWARD) && (dir != BTM_DIR_BACKWARD)) { return HAL_ERROR; }

    travel = dir_travel_ms(dir);
    if (travel == 0U) { return HAL_ERROR; }

    release_stuck_finish();
    if (state != BTM_STATE_IDLE) { return HAL_BUSY; }

    duty_percent = (duty >= 100U) ? 100U : clamp_duty(duty);
    pwm_cmd = dir_cmd(dir);
    move_deadline = HAL_GetTick() + travel;
    uv_run = 0U;
    drop_run = 0U;

    /* Komut önce doğrulanmış biçimde gider: hattın ve cihazın sağlığı çerçeve
     * akışı başlamadan bilinsin. Timer henüz durmuş olduğu için ISR çalışmaz. */
    res = send_verified(pwm_cmd);
    if (res != HAL_OK) { return res; }   /* HAL_BUSY geçicidir, çevrilmez */

    arr = __HAL_TIM_GET_AUTORELOAD(cfg.tim);
    __HAL_TIM_SET_COUNTER(cfg.tim, 0U);
    __HAL_TIM_SET_COMPARE(cfg.tim, TIM_CHANNEL_1, ((arr + 1U) * duty_percent) / 100U);

    periods_left = (travel * BTM_PWM_HZ) / 1000U;
    if (periods_left == 0U) { periods_left = 1U; }

    set_state(BTM_STATE_MOVING);   /* kapı açılır; sonra timer başlar */
    if ((HAL_TIM_Base_Start_IT(cfg.tim) != HAL_OK) ||
        (HAL_TIM_OC_Start_IT(cfg.tim, TIM_CHANNEL_1) != HAL_OK))
    {
        set_state(BTM_STATE_IDLE);
        stop_timer();
        (void)send_verified(BTM_CMD_BRAKE);
        return HAL_ERROR;
    }
    return HAL_OK;
}

HAL_StatusTypeDef BTM_Move(BTM_Direction dir) { return start_move(dir, 100U); }

HAL_StatusTypeDef BTM_MovePwm(BTM_Direction dir, uint32_t duty_pct)
{
    if (duty_pct == 0U) { return BTM_Brake(); }
    return start_move(dir, duty_pct);
}

HAL_StatusTypeDef BTM_Brake(void)
{
    uint32_t t0;

    if (!inited)         { return HAL_ERROR; }
    if (!task_context()) { return HAL_BUSY; }

    if (state != BTM_STATE_FAULT)
    {
        state = BTM_STATE_IDLE;   /* ÖNCE kapıyı kapat, SONRA timer'ı durdur */
        __DMB();
        publish();
    }
    stop_timer();

    /* Uçuştaki kesmeli aktarımın bitmesini bekle; bitmezse yine de devam et. */
    t0 = HAL_GetTick();
    while (cfg.spi->State != HAL_SPI_STATE_READY)
    {
        if ((HAL_GetTick() - t0) > BTM_INFLIGHT_WAIT_MS) { break; }
    }

    if (fault != BTM_FAULT_NONE)
    {
        (void)send_verified(BTM_CMD_BRAKE);   /* yine de frenlemeyi dene */
        return HAL_ERROR;
    }
    return send_verified(BTM_CMD_BRAKE);
}

uint32_t BTM_RemainingMs(void)
{
    int32_t diff;

    if (state != BTM_STATE_MOVING) { return 0U; }
    diff = (int32_t)(move_deadline - HAL_GetTick());
    return (diff > 0) ? (uint32_t)diff : 0U;
}

HAL_StatusTypeDef BTM_GetStatus(BTM_Status *status)
{
    /* `inited` şart değil: Init DWT testinde başarısız olursa fault alanı
     * BTM_FAULT_DWT olur ve okunabilmesi gerekir. */
    if (status == NULL) { return HAL_ERROR; }

    status->state          = state;
    status->fault          = fault;
    status->hal_error      = hal_error;
    status->last_cmd       = last_cmd;
    status->status_byte    = status_byte;
    status->status_valid   = status_valid;
    status->duty_percent   = duty_percent;
    status->dropped_frames = dropped_frames;
    return HAL_OK;
}

/* ==================== HAL callbackleri ==================== */

void BTM_OnTimerPeriod(TIM_HandleTypeDef *htim)
{
    if ((!inited) || (htim != cfg.tim) || (state != BTM_STATE_MOVING)) { return; }

    if (periods_left == 0U) { move_finish(); return; }
    periods_left--;
    (void)send_it(pwm_cmd);
}

void BTM_OnTimerCompare(TIM_HandleTypeDef *htim)
{
    if ((!inited) || (htim != cfg.tim) || (state != BTM_STATE_MOVING)) { return; }
    if (duty_percent >= 100U) { return; }   /* tam hız: fren çerçevesi yok */
    (void)send_it(BTM_CMD_BRAKE);
}

void BTM_OnSpiComplete(SPI_HandleTypeDef *hspi)
{
    if ((!inited) || (hspi != cfg.spi)) { return; }
    cs_drop();

    /* Kesmeli çerçevelerde SDO_SEL hep 1 olduğu için gelen bayt status baytıdır. */
    if ((state == BTM_STATE_MOVING) || (state == BTM_STATE_FINISHING))
    {
        eval_status((uint8_t)rx_byte);
        if (state == BTM_STATE_FINISHING) { set_state(BTM_STATE_IDLE); }
        else { publish(); }
    }
}

void BTM_OnSpiError(SPI_HandleTypeDef *hspi)
{
    if ((!inited) || (hspi != cfg.spi)) { return; }
    /* Bağlı olmasaydı CS High'da kalır ve sürüş komutu kilitli halde donardı. */
    cs_drop();
    record_fault(BTM_FAULT_SPI);
}
