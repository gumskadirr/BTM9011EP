/* BTM9011EP_Sade_v3 host testi: GERÇEK BTM9011EP.c'yi taklit HAL ile derler.
 * Cihaz modeli datasheet Şekil 27'deki bir çerçeve gecikmeli cevabı taklit eder.
 * .c doğrudan include edilir ki testler statik durumu sıfırlayabilsin. */
#include "stm32f4xx_hal.h"
#include <stdio.h>
#include <string.h>

/* ---------- Taklit çevre (sürücüden önce) ---------- */
static CoreDebug_Type cd_storage;
static DWT_Type dwt_storage;
CoreDebug_Type *CoreDebug = &cd_storage;
uint32_t SystemCoreClock = 168000000U;

static int dwt_frozen;                  /* 1 ise sayaç saymaz (DWT yok senaryosu) */

DWT_Type *mock_dwt_tick(void)
{
    if (!dwt_frozen) { dwt_storage.CYCCNT += 1000U; }
    return &dwt_storage;
}

static uint32_t tick;
static uint32_t spi_free_after;         /* N tick sonra SPI hazır olur */
static int spi_fail_in;                 /* >0: N. SPI çağrısı BİR KEZ HAL_ERROR döner */
static int busy_on_gettick;             /* N'inci HAL_GetTick'te hat MEŞGULLEŞİR:
                                         * "çerçeve yolunda meşgulleşme" senaryosu */
static SPI_HandleTypeDef hspi;
static GPIO_TypeDef gpio;
static TIM_HandleTypeDef htim;
static TIM_TypeDef tim_inst;

/* ---------- Cihaz modeli ---------- */
static unsigned char dev_pending;
static unsigned char dev_status_bits;
static int dev_absent;                  /* 1 ise hat boş: MISO pull-up -> 0xFF */
static int frame_count;
static unsigned char frame_log[256];
static int cs_level;

static unsigned char device_frame(unsigned char sdi)
{
    unsigned char out = dev_absent ? 0xFFU : dev_pending;

    if (frame_count < (int)sizeof(frame_log)) { frame_log[frame_count] = sdi; }
    frame_count++;

    /* Bu çerçevenin SDO_SEL biti SONRAKİ çerçevenin cevabını seçer (Şekil 27). */
    if ((sdi & 0x80U) != 0U) { dev_pending = (unsigned char)(dev_status_bits | 0x80U); }
    else                     { dev_pending = (unsigned char)(sdi & 0x7FU); }
    return out;
}

static int it_pending;
static unsigned char *it_rx;
static unsigned char it_tx_copy;
static int tim_running;

/* ---------- Gerçek sürücü ---------- */
#include "BTM9011EP.c"

/* ---------- HAL taklidi ---------- */
uint32_t HAL_GetTick(void)
{
    tick++;
    if (busy_on_gettick > 0)
    {
        busy_on_gettick--;
        if (busy_on_gettick == 0) { hspi.State = HAL_SPI_STATE_BUSY_TX_RX; }
    }
    if (spi_free_after > 0U)
    {
        spi_free_after--;
        if (spi_free_after == 0U) { hspi.State = HAL_SPI_STATE_READY; }
    }
    return tick;
}

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, uint32_t st)
{
    (void)port; (void)pin;
    cs_level = (st == GPIO_PIN_SET) ? 1 : 0;
}

HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *h, uint8_t *tx, uint8_t *rx,
                                          uint16_t n, uint32_t to)
{
    (void)n; (void)to;
    if (spi_fail_in > 0)
    {
        spi_fail_in--;
        if (spi_fail_in == 0) { h->ErrorCode = 2U; return HAL_ERROR; }
    }
    *rx = device_frame(*tx);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_SPI_TransmitReceive_IT(SPI_HandleTypeDef *h, uint8_t *tx, uint8_t *rx, uint16_t n)
{
    (void)n;
    h->State = HAL_SPI_STATE_BUSY_TX_RX;
    it_tx_copy = *tx; it_rx = rx; it_pending = 1;
    return HAL_OK;
}

/** Gerçek donanımda kesmeyle gelen tamamlanmayı taklit eder. */
static void spi_done(void)
{
    if (!it_pending) { return; }
    it_pending = 0;
    *it_rx = device_frame(it_tx_copy);
    hspi.State = HAL_SPI_STATE_READY;
    BTM_OnSpiComplete(&hspi);
}

/** Bir PWM periyodu: periyot ISR + compare ISR, aralarda SPI tamamlanmaları. */
static void one_period(void)
{
    BTM_OnTimerPeriod(&htim);  spi_done();
    BTM_OnTimerCompare(&htim); spi_done();
}

uint32_t HAL_SPI_GetError(SPI_HandleTypeDef *h) { return h->ErrorCode; }
/** Gercek donanimda durdurulan timer'in SR'sinde UIF bekleyebilir; sayici yeniden
 *  baslatilinca NVIC o bekleyen kesmeyi ANINDA calistirir. Taklit bunu yapar. */
HAL_StatusTypeDef HAL_TIM_Base_Start_IT(TIM_HandleTypeDef *h)
{
    tim_running = 1;
    if ((h->Instance != NULL) && ((h->Instance->SR & TIM_FLAG_UPDATE) != 0U))
    {
        h->Instance->SR &= ~(uint32_t)TIM_FLAG_UPDATE;
        BTM_OnTimerPeriod(h);
    }
    return HAL_OK;
}
HAL_StatusTypeDef HAL_TIM_Base_Stop_IT(TIM_HandleTypeDef *h)  { (void)h; tim_running = 0; return HAL_OK; }
HAL_StatusTypeDef HAL_TIM_OC_Start_IT(TIM_HandleTypeDef *h, uint32_t ch) { (void)h; (void)ch; return HAL_OK; }
HAL_StatusTypeDef HAL_TIM_OC_Stop_IT(TIM_HandleTypeDef *h, uint32_t ch)  { (void)h; (void)ch; return HAL_OK; }

/* ---------- Test altyapısı ---------- */
static int fails;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", (msg)); fails++; } \
    else { printf("  ok  : %s\n", (msg)); } \
} while (0)

static void reset_all(void)
{
    memset(&cfg, 0, sizeof(cfg));
    inited = 0U; cycles_per_us = 1U; tcsl_cycles = 0U; awake = 0U;
    state = BTM_STATE_IDLE; cs_low_cycles = 0U;
    tx_byte = 0U; rx_byte = 0U; last_cmd = BTM_CMD_BRAKE;
    pwm_cmd = 0U; periods_left = 0U; duty_percent = 0U;
    move_start_tick = 0U; finish_tick = 0U;
    move_ms = 0U; move_ms_estimated = 0U; move_ms_clamped = 0U;
    last_stop_tick = 0U; last_move_ms = 0U; last_dir = BTM_DIR_FORWARD;
    status_byte = 0U; status_valid = 0U; echo_byte = 0U;
    dropped_frames = 0U; drop_run = 0U; uv_run = 0U;
    fault = BTM_FAULT_NONE; hal_error = 0U;

    memset(&hspi, 0, sizeof(hspi));
    memset(&htim, 0, sizeof(htim));
    hspi.State = HAL_SPI_STATE_READY;
    /* Karttaki dogru kurulum: 84 MHz / ((419+1) * (999+1)) = 200 Hz = BTM_PWM_HZ */
    memset(&tim_inst, 0, sizeof(tim_inst));
    tim_inst.PSC = 419U;
    htim.Instance = &tim_inst;
    htim.ARR = 999U;
    tick = 1000U; spi_free_after = 0U; busy_on_gettick = 0; dwt_frozen = 0;
    spi_fail_in = 0;
    dev_pending = 0x00U; dev_status_bits = BTM_ST_EN; dev_absent = 0;
    frame_count = 0; memset(frame_log, 0, sizeof(frame_log));
    cs_level = 0; it_pending = 0; tim_running = 0;
}

static BTM_Config make_cfg(void)
{
    BTM_Config c;
    memset(&c, 0, sizeof(c));
    c.spi = &hspi; c.cs_port = &gpio; c.cs_pin = 1U; c.tim = &htim;
    c.forward_ms = 6000U; c.backward_ms = 6000U;
    c.tim_clk_hz = 84000000U;
    return c;
}

int main(void)
{
    BTM_Config c;
    BTM_Status st;
    uint32_t rem;
    int i;

    printf("1. Init (saglikli cihaz)\n");
    reset_all(); c = make_cfg();
    CHECK(BTM_Init(&c) == HAL_OK, "Init HAL_OK");
    CHECK(frame_count == 3, "dogrulanmis komut 3 cerceve");
    CHECK(frame_log[0] == BTM_CMD_BRAKE, "cerceve 1: fren, SDO_SEL=0");
    CHECK(frame_log[1] == (BTM_CMD_BRAKE | 0x80U), "cerceve 2: echo icin SDO_SEL=1");
    CHECK(frame_log[2] == (BTM_CMD_BRAKE | 0x80U), "cerceve 3: status");
    (void)BTM_GetStatus(&st);
    CHECK(st.fault == BTM_FAULT_NONE, "fault yok");
    CHECK(st.status_valid == 1U, "status bayti saklandi");
    CHECK((st.status_byte & BTM_ST_SDO_SEL) != 0U, "status baytinda bit7 set");
    CHECK(cs_level == 0, "CS Low'da birakildi");
    CHECK(BTM_Init(&c) == HAL_BUSY, "ikinci Init HAL_BUSY");

    printf("2. Init: cihaz bagli degil (0xFF echo'da yakalanir)\n");
    reset_all(); dev_absent = 1; c = make_cfg();
    CHECK(BTM_Init(&c) == HAL_ERROR, "Init HAL_ERROR");
    (void)BTM_GetStatus(&st);
    CHECK(st.fault == BTM_FAULT_COMM, "fault = COMM");

    printf("3. Init: timer ve config zorunlulugu\n");
    reset_all(); c = make_cfg(); c.tim = NULL;
    CHECK(BTM_Init(&c) == HAL_ERROR, "timer NULL -> HAL_ERROR");
    reset_all(); c = make_cfg(); c.forward_ms = 0U;
    CHECK(BTM_Init(&c) == HAL_ERROR, "strok suresi 0 -> HAL_ERROR");
    reset_all(); memset(&c, 0, sizeof(c));
    CHECK(BTM_Init(&c) == HAL_ERROR, "bos config -> HAL_ERROR");

    printf("4. Tam hiz: timer sayar, sure dolunca SURUCU KENDI frenler\n");
    reset_all(); c = make_cfg(); c.forward_ms = 10U;   /* 200 Hz'de 2 periyot */
    (void)BTM_Init(&c);
    frame_count = 0;
    CHECK(BTM_Move(BTM_DIR_FORWARD) == HAL_OK, "Move HAL_OK");
    CHECK(frame_count == 3, "once dogrulanmis komut (3 cerceve)");
    CHECK(frame_log[0] == BTM_CMD_FORWARD, "ileri komutu gonderildi");
    CHECK(tim_running == 1, "timer basladi");
    (void)BTM_GetStatus(&st);
    CHECK(st.state == BTM_STATE_MOVING, "durum MOVING");
    CHECK(st.duty_percent == 100U, "duty 100");
    frame_count = 0;
    one_period(); one_period();
    CHECK(frame_count == 2, "tam hizda periyot basina TEK cerceve (fren yok)");
    CHECK(frame_log[0] == (BTM_CMD_FORWARD | 0x80U), "surus cercevesi, SDO_SEL=1");
    BTM_OnTimerPeriod(&htim);                    /* sure doldu */
    CHECK(state == BTM_STATE_FINISHING, "durum FINISHING (son fren yolda)");
    spi_done();
    CHECK(tim_running == 0, "timer durdu");
    (void)BTM_GetStatus(&st);
    CHECK(st.state == BTM_STATE_IDLE, "durum IDLE (kendi bitirdi)");
    CHECK(st.last_cmd == BTM_CMD_BRAKE, "son komut FREN");
    CHECK(BTM_RemainingMs() == 0U, "RemainingMs 0");

    printf("5. Tam hizda da her periyotta status okunur\n");
    reset_all(); c = make_cfg(); c.forward_ms = 100U;
    (void)BTM_Init(&c);
    (void)BTM_Move(BTM_DIR_FORWARD);
    status_byte = 0U; status_valid = 0U;
    one_period();
    (void)BTM_GetStatus(&st);
    CHECK(st.status_valid == 1U, "surus cercevesinden status geldi");
    CHECK((st.status_byte & BTM_ST_SDO_SEL) != 0U, "gecerli status bayti");

    printf("6. PWM: periyot basinda surus, compare aninda fren\n");
    reset_all(); c = make_cfg(); c.forward_ms = 10U;
    (void)BTM_Init(&c);
    CHECK(BTM_MovePwm(BTM_DIR_FORWARD, 50U) == HAL_OK, "MovePwm HAL_OK");
    (void)BTM_GetStatus(&st);
    CHECK(st.duty_percent == 50U, "duty 50");
    CHECK(htim.CCR1 == 500U, "CCR1 = (ARR+1) * %50");
    CHECK(periods_left == 4U, "%50 duty: periyot sayisi 2 KATI (sure olceklendi)");
    frame_count = 0;
    one_period(); one_period();
    CHECK(frame_count == 4, "PWM'de periyot basina IKI cerceve");
    CHECK(frame_log[0] == (BTM_CMD_FORWARD | 0x80U), "periyot basi: surus");
    CHECK(frame_log[1] == (BTM_CMD_BRAKE | 0x80U), "compare ani: fren");
    one_period(); one_period();                  /* olceklenmis surenin kalani */
    BTM_OnTimerPeriod(&htim); spi_done();
    (void)BTM_GetStatus(&st);
    CHECK(st.state == BTM_STATE_IDLE, "PWM kendi bitirdi");
    CHECK(st.last_cmd == BTM_CMD_BRAKE, "son komut fren");

    printf("7. RemainingMs ve erken Brake\n");
    reset_all(); c = make_cfg();
    (void)BTM_Init(&c);
    (void)BTM_Move(BTM_DIR_BACKWARD);
    rem = BTM_RemainingMs();
    CHECK((rem > 5900U) && (rem <= 6000U), "RemainingMs ~6000");
    frame_count = 0;
    CHECK(BTM_Brake() == HAL_OK, "erken Brake HAL_OK");
    CHECK(tim_running == 0, "timer durduruldu");
    CHECK(frame_log[0] == BTM_CMD_BRAKE, "fren gonderildi");
    (void)BTM_GetStatus(&st);
    CHECK(st.state == BTM_STATE_IDLE, "durum IDLE");
    CHECK(BTM_RemainingMs() == 0U, "RemainingMs 0");

    printf("8. Ters yon: biten hareket zaten frende bittigi icin arada fren var\n");
    reset_all(); c = make_cfg(); c.forward_ms = 10U; c.backward_ms = 10U;
    (void)BTM_Init(&c);
    (void)BTM_Move(BTM_DIR_FORWARD);
    one_period(); one_period();
    BTM_OnTimerPeriod(&htim); spi_done();        /* kendi frenledi */
    CHECK(last_cmd == BTM_CMD_BRAKE, "hareket frenle bitti");
    frame_count = 0;
    tick += BTM_REVERSE_MS + 10U;                /* ters yon olu suresi (test 34) */
    CHECK(BTM_Move(BTM_DIR_BACKWARD) == HAL_OK, "ters yon kabul edildi");
    CHECK(frame_log[0] == BTM_CMD_BACKWARD, "geri komutu (oncesinde kopru frendeydi)");

    printf("9. OCA -> aninda fault, komutlar reddedilir\n");
    reset_all(); c = make_cfg();
    (void)BTM_Init(&c);
    dev_status_bits = (unsigned char)(BTM_ST_EN | BTM_ST_OCA);
    CHECK(BTM_Move(BTM_DIR_FORWARD) == HAL_ERROR, "Move HAL_ERROR");
    (void)BTM_GetStatus(&st);
    CHECK(st.fault == BTM_FAULT_DEVICE, "fault = DEVICE");
    CHECK(st.state == BTM_STATE_FAULT, "durum FAULT");
    CHECK(BTM_Move(BTM_DIR_FORWARD) == HAL_ERROR, "fault kilitli (reset gerekir)");

    printf("10. UV teyit ister (BTM_UV_CONFIRM=%u)\n", (unsigned)BTM_UV_CONFIRM);
    reset_all(); c = make_cfg(); c.forward_ms = 1000U;
    (void)BTM_Init(&c);
    dev_status_bits = (unsigned char)(BTM_ST_EN | BTM_ST_UV);
    (void)BTM_Move(BTM_DIR_FORWARD);
    (void)BTM_GetStatus(&st);
    CHECK(st.fault == BTM_FAULT_NONE, "tek UV fault uretmedi");
    for (i = 0; i < (int)BTM_UV_CONFIRM; i++) { one_period(); }
    (void)BTM_GetStatus(&st);
    CHECK(st.fault == BTM_FAULT_DEVICE, "teyit tamamlaninca fault");

    printf("11. OL biti fault uretmez (yalniz teshis desenlerinde gecerli)\n");
    reset_all(); c = make_cfg();
    (void)BTM_Init(&c);
    dev_status_bits = (unsigned char)(BTM_ST_EN | BTM_ST_OL);
    CHECK(BTM_Move(BTM_DIR_FORWARD) == HAL_OK, "OL set iken Move HAL_OK");
    (void)BTM_GetStatus(&st);
    CHECK(st.fault == BTM_FAULT_NONE, "OL fault uretmedi");

    printf("12. Dusen cerceve sayilir, limitte OVERLAP\n");
    reset_all(); c = make_cfg(); c.forward_ms = 1000U;
    (void)BTM_Init(&c);
    (void)BTM_Move(BTM_DIR_FORWARD);
    hspi.State = HAL_SPI_STATE_BUSY_TX_RX;
    for (i = 0; i < (int)BTM_DROP_LIMIT; i++) { BTM_OnTimerPeriod(&htim); }
    (void)BTM_GetStatus(&st);
    CHECK(st.dropped_frames == BTM_DROP_LIMIT, "dusen cerceveler sayildi");
    CHECK(st.fault == BTM_FAULT_OVERLAP, "fault = OVERLAP");
    CHECK(tim_running == 0, "fault timer'i durdurdu");

    printf("13. Fault'ta bile fren gercekten gonderilir\n");
    spi_free_after = 3U;                         /* ucustaki cerceve bitiyor */
    last_cmd = 0xAAU;
    (void)BTM_Brake();
    CHECK(last_cmd == BTM_CMD_BRAKE, "fren cercevesi gonderildi");

    printf("14. SPI hata geri cagrisi\n");
    reset_all(); c = make_cfg(); c.forward_ms = 1000U;
    (void)BTM_Init(&c);
    (void)BTM_Move(BTM_DIR_FORWARD);
    hspi.ErrorCode = 4U;
    BTM_OnSpiError(&hspi);
    (void)BTM_GetStatus(&st);
    CHECK(st.fault == BTM_FAULT_SPI, "fault = SPI");
    CHECK(st.hal_error == 4U, "hal_error kaydedildi");
    CHECK(cs_level == 0, "CS High'da birakilmadi");
    CHECK(tim_running == 0, "timer durduruldu");

    printf("15. Gecici SPI mesguliyeti fault uretmez, HAL_BUSY doner\n");
    reset_all(); c = make_cfg();
    (void)BTM_Init(&c);
    hspi.State = HAL_SPI_STATE_BUSY_TX_RX;       /* hic bosalmaz */
    CHECK(BTM_Move(BTM_DIR_FORWARD) == HAL_BUSY, "Move HAL_BUSY (yeniden denenebilir)");
    (void)BTM_GetStatus(&st);
    CHECK(st.fault == BTM_FAULT_NONE, "kalici fault latchlenmedi");
    CHECK(st.state == BTM_STATE_IDLE, "durum IDLE");
    hspi.State = HAL_SPI_STATE_READY;
    CHECK(BTM_Move(BTM_DIR_FORWARD) == HAL_OK, "SPI bosalinca ayni cagri calisti");

    printf("16. Takili FINISHING kilitlenmez\n");
    reset_all(); c = make_cfg(); c.forward_ms = 10U;
    (void)BTM_Init(&c);
    (void)BTM_Move(BTM_DIR_FORWARD);
    one_period(); one_period();
    BTM_OnTimerPeriod(&htim);                    /* FINISHING, tamamlanma KASTEN yok */
    CHECK(state == BTM_STATE_FINISHING, "durum FINISHING");
    hspi.State = HAL_SPI_STATE_READY;
    tick += BTM_REVERSE_MS + 50U;                /* ters yon olu suresi (test 34) */
    CHECK(BTM_Move(BTM_DIR_BACKWARD) == HAL_OK, "takili durum cozuldu");

    printf("17. DWT saymiyorsa FAULT_DWT okunabilir\n");
    reset_all(); dwt_frozen = 1; c = make_cfg();
    CHECK(BTM_Init(&c) == HAL_ERROR, "Init HAL_ERROR");
    CHECK(BTM_GetStatus(&st) == HAL_OK, "Init basarisiz olsa da GetStatus calisti");
    CHECK(st.fault == BTM_FAULT_DWT, "fault = DWT");

    printf("18. ISR'lar kendi handle'i disindaki cagrilari yok sayar\n");
    reset_all(); c = make_cfg(); c.forward_ms = 1000U;
    (void)BTM_Init(&c);
    (void)BTM_Move(BTM_DIR_FORWARD);
    frame_count = 0;
    {
        TIM_HandleTypeDef other_tim; SPI_HandleTypeDef other_spi;
        memset(&other_tim, 0, sizeof(other_tim));
        memset(&other_spi, 0, sizeof(other_spi));
        BTM_OnTimerPeriod(&other_tim);
        BTM_OnTimerCompare(&other_tim);
        BTM_OnSpiComplete(&other_spi);
        BTM_OnSpiError(&other_spi);
    }
    CHECK(frame_count == 0, "baska handle: hicbir cerceve gonderilmedi");
    (void)BTM_GetStatus(&st);
    CHECK(st.fault == BTM_FAULT_NONE, "baska handle fault uretmedi");

    printf("19. Son fren cercevesi gidemezse SESSIZCE IDLE olunmaz\n");
    reset_all(); c = make_cfg(); c.forward_ms = 10U;
    (void)BTM_Init(&c);
    (void)BTM_Move(BTM_DIR_FORWARD);
    BTM_OnTimerPeriod(&htim);          /* surus cercevesi ucusta; spi_done KASTEN yok */
    BTM_OnTimerPeriod(&htim);          /* duser */
    BTM_OnTimerPeriod(&htim);          /* sure doldu: son fren BASLATILAMAZ */
    (void)BTM_GetStatus(&st);
    CHECK(st.state != BTM_STATE_IDLE, "IDLE ilan EDILMEDI");
    CHECK(st.fault == BTM_FAULT_OVERLAP, "fault = OVERLAP (durus garanti edilemedi)");

    printf("20. Ham baytlar her zaman saklanir (arıza teshisi)\n");
    /* Saglikli: echo == gonderilen, status gecerli */
    reset_all(); c = make_cfg();
    CHECK(BTM_Init(&c) == HAL_OK, "Init HAL_OK");
    (void)BTM_GetStatus(&st);
    CHECK(st.echo_byte == BTM_CMD_BRAKE, "echo_byte == gonderilen komut");
    CHECK(st.last_cmd == BTM_CMD_BRAKE, "last_cmd == gonderilen komut");
    CHECK(st.status_valid == 1U, "status gecerli");
    /* Cihaz hatta yok: her iki ham bayt da 0xFF olarak SAKLANMALI */
    reset_all(); dev_absent = 1; c = make_cfg();
    CHECK(BTM_Init(&c) == HAL_ERROR, "Init HAL_ERROR");
    (void)BTM_GetStatus(&st);
    CHECK(st.fault == BTM_FAULT_COMM, "fault = COMM");
    CHECK(st.echo_byte == 0xFFU, "echo_byte = 0xFF SAKLANDI (cihaz hatta yok)");
    CHECK(st.last_cmd == BTM_CMD_BRAKE, "last_cmd yaninda duruyor (karsilastirilabilir)");
    /* Echo tutuyor ama status bozuk: hat saglam, cihaz tuhaf */
    reset_all(); c = make_cfg();
    (void)BTM_Init(&c);
    dev_status_bits = 0x01U;            /* bit7'yi cihaz modeli ekler */
    frame_count = 0;
    (void)BTM_Move(BTM_DIR_FORWARD);
    (void)BTM_GetStatus(&st);
    CHECK(st.echo_byte == BTM_CMD_FORWARD, "echo tuttu -> hat saglam");
    CHECK(st.status_valid == 1U, "status iyi bicimli");

    printf("21. ClearFault: gecici OVERLAP sonrasi kilit acilir (reset gerekmez)\n");
    reset_all(); c = make_cfg(); c.forward_ms = 1000U;
    (void)BTM_Init(&c);
    (void)BTM_Move(BTM_DIR_FORWARD);
    hspi.State = HAL_SPI_STATE_BUSY_TX_RX;
    for (i = 0; i < (int)BTM_DROP_LIMIT; i++) { BTM_OnTimerPeriod(&htim); }
    (void)BTM_GetStatus(&st);
    CHECK(st.fault == BTM_FAULT_OVERLAP, "onkosul: fault = OVERLAP");
    hspi.State = HAL_SPI_STATE_READY;            /* gecikme gecti, hat bosaldi */
    frame_count = 0;
    CHECK(BTM_ClearFault() == HAL_OK, "ClearFault HAL_OK");
    CHECK(frame_count == 3, "hat dogrulanmis FREN ile yeniden sinandi (3 cerceve)");
    CHECK(frame_log[0] == BTM_CMD_BRAKE, "sinama komutu fren");
    (void)BTM_GetStatus(&st);
    CHECK(st.fault == BTM_FAULT_NONE, "fault temizlendi");
    CHECK(st.state == BTM_STATE_IDLE, "durum IDLE");
    CHECK(st.last_cmd == BTM_CMD_BRAKE, "son komut fren");
    CHECK(st.hal_error == 0U, "hal_error sifirlandi");
    CHECK(st.dropped_frames == BTM_DROP_LIMIT, "dropped_frames KORUNDU (omur boyu)");
    CHECK(BTM_Move(BTM_DIR_FORWARD) == HAL_OK, "temizlikten sonra hareket edilebiliyor");

    printf("22. ClearFault: sebep suruyorsa kilit GERI gelir\n");
    reset_all(); c = make_cfg();
    (void)BTM_Init(&c);
    dev_status_bits = (unsigned char)(BTM_ST_EN | BTM_ST_OCA);
    CHECK(BTM_Move(BTM_DIR_FORWARD) == HAL_ERROR, "onkosul: OCA -> fault");
    CHECK(BTM_ClearFault() == HAL_ERROR, "sebep suruyor -> HAL_ERROR");
    (void)BTM_GetStatus(&st);
    CHECK(st.fault == BTM_FAULT_DEVICE, "fault = DEVICE (yok sayilmadi)");
    CHECK(st.state == BTM_STATE_FAULT, "durum FAULT");
    dev_status_bits = BTM_ST_EN;                 /* ariza gecti */
    CHECK(BTM_ClearFault() == HAL_OK, "sebep gidince temizlendi");
    (void)BTM_GetStatus(&st);
    CHECK(st.fault == BTM_FAULT_NONE, "fault yok");
    CHECK(st.state == BTM_STATE_IDLE, "durum IDLE");

    printf("23. ClearFault: FAULT_DWT temizlenemez (donanimsal)\n");
    reset_all(); dwt_frozen = 1; c = make_cfg();
    CHECK(BTM_Init(&c) == HAL_ERROR, "onkosul: Init HAL_ERROR");
    CHECK(BTM_ClearFault() == HAL_ERROR, "Init edilmemis -> HAL_ERROR");
    inited = 1U;                                 /* DWT dalini acikca sina */
    CHECK(BTM_ClearFault() == HAL_ERROR, "inited olsa da FAULT_DWT temizlenmez");
    (void)BTM_GetStatus(&st);
    CHECK(st.fault == BTM_FAULT_DWT, "fault = DWT yerinde duruyor");

    printf("24. ClearFault: hata yokken zararsiz ve cerceve harcamaz\n");
    reset_all(); c = make_cfg();
    (void)BTM_Init(&c);
    frame_count = 0;
    CHECK(BTM_ClearFault() == HAL_OK, "hata yokken HAL_OK");
    CHECK(frame_count == 0, "gereksiz cerceve gonderilmedi");
    (void)BTM_GetStatus(&st);
    CHECK(st.state == BTM_STATE_IDLE, "durum bozulmadi");

    printf("25. ClearFault: hat mesgulse HICBIR SEY degismez (HAL_BUSY)\n");
    reset_all(); c = make_cfg(); c.forward_ms = 1000U;
    (void)BTM_Init(&c);
    (void)BTM_Move(BTM_DIR_FORWARD);
    hspi.ErrorCode = 4U;
    BTM_OnSpiError(&hspi);
    (void)BTM_GetStatus(&st);
    CHECK(st.fault == BTM_FAULT_SPI, "onkosul: fault = SPI");
    hspi.State = HAL_SPI_STATE_BUSY_TX_RX;       /* hic bosalmaz */
    frame_count = 0;
    CHECK(BTM_ClearFault() == HAL_BUSY, "mesgul hat -> HAL_BUSY");
    CHECK(frame_count == 0, "cerceve gonderilmedi");
    (void)BTM_GetStatus(&st);
    CHECK(st.fault == BTM_FAULT_SPI, "fault geri konuldu");
    CHECK(st.state == BTM_STATE_FAULT, "durum FAULT geri konuldu");
    CHECK(st.hal_error == 4U, "hal_error korundu");
    hspi.State = HAL_SPI_STATE_READY;
    CHECK(BTM_ClearFault() == HAL_OK, "hat bosalinca ayni cagri temizledi");

    /* Hat ILK beklemeyi gecip DOGRULANMIS KOMUTUN ICINDE mesgullesirse: geri alma
     * dali. Ic sayaclar da birebir geri konmali (disa aktarilmadiklari icin
     * dogrudan bakiliyor). */
    reset_all(); c = make_cfg(); c.forward_ms = 1000U;
    (void)BTM_Init(&c);
    (void)BTM_Move(BTM_DIR_FORWARD);
    hspi.State = HAL_SPI_STATE_BUSY_TX_RX;
    for (i = 0; i < (int)BTM_DROP_LIMIT; i++) { BTM_OnTimerPeriod(&htim); }
    CHECK(fault == BTM_FAULT_OVERLAP, "onkosul: fault = OVERLAP");
    CHECK(drop_run == BTM_DROP_LIMIT, "onkosul: drop_run dolu");
    hspi.State = HAL_SPI_STATE_READY;            /* ilk bekleme gecsin */
    busy_on_gettick = 2;                         /* cerceve yolunda mesgullesir */
    CHECK(BTM_ClearFault() == HAL_BUSY, "cerceve yolunda mesgullesme -> HAL_BUSY");
    CHECK(fault == BTM_FAULT_OVERLAP, "fault birebir geri konuldu");
    CHECK(drop_run == BTM_DROP_LIMIT, "drop_run geri konuldu (tam no-op)");
    CHECK(state == BTM_STATE_FAULT, "durum FAULT geri konuldu");

    printf("26. Hareket suresi UYGULANAN duty ile olceklenir\n");
    reset_all(); c = make_cfg(); c.forward_ms = 1000U; c.backward_ms = 1000U;
    (void)BTM_Init(&c);
    CHECK(BTM_Move(BTM_DIR_FORWARD) == HAL_OK, "tam hiz Move HAL_OK");
    (void)BTM_GetStatus(&st);
    CHECK(st.move_ms == 1000U, "tam hiz: move_ms == forward_ms");
    CHECK(st.move_ms_estimated == 0U, "tam hiz: tahmin bayragi yok");
    CHECK(st.move_ms_clamped == 0U, "tam hiz: kirpma yok");
    CHECK(periods_left == (1000U * BTM_PWM_HZ) / 1000U, "periyot sayisi tam hiza gore");
    (void)BTM_Brake();
    tick += 100U;                                /* isil dinlenme (test 35): erken
                                                  * frende fiilen surus cok kisaydi */
    CHECK(BTM_MovePwm(BTM_DIR_FORWARD, 50U) == HAL_OK, "MovePwm %50 HAL_OK");
    (void)BTM_GetStatus(&st);
    CHECK(st.duty_percent == 50U, "duty 50 uygulandi");
    CHECK(st.move_ms == 2000U, "%50 duty -> sure 2 KATI (strok tamamlanir)");
    CHECK(st.move_ms_estimated == 1U, "tahmin bayragi kalkti");
    CHECK(st.move_ms_clamped == 0U, "kirpma yok");
    CHECK(periods_left == (2000U * BTM_PWM_HZ) / 1000U, "periyot sayisi da olcekli");
    rem = BTM_RemainingMs();
    CHECK((rem > 1900U) && (rem <= 2000U), "RemainingMs olceklenmis sureyi gosteriyor");

    printf("27. Cok dusuk duty: sure BTM_MAX_MOVE_MS'e kirpilir ve bildirilir\n");
    reset_all(); c = make_cfg(); c.forward_ms = 30000U;
    (void)BTM_Init(&c);
    CHECK(BTM_MovePwm(BTM_DIR_FORWARD, 10U) == HAL_OK, "MovePwm %10 HAL_OK");
    (void)BTM_GetStatus(&st);
    CHECK(st.move_ms == BTM_MAX_MOVE_MS, "sure azami degere kirpildi");
    CHECK(st.move_ms_clamped == 1U, "kirpma BILDIRILDI (strok tamamlanmayabilir)");
    CHECK(st.move_ms_estimated == 1U, "tahmin bayragi da kalkti");

    printf("28. Config: strok suresi BTM_MAX_MOVE_MS ile sinirli\n");
    reset_all(); c = make_cfg(); c.forward_ms = BTM_MAX_MOVE_MS + 1U;
    CHECK(BTM_Init(&c) == HAL_ERROR, "sinirin ustu -> HAL_ERROR");
    reset_all(); c = make_cfg(); c.backward_ms = BTM_MAX_MOVE_MS + 1U;
    CHECK(BTM_Init(&c) == HAL_ERROR, "geri yon icin de gecerli");
    reset_all(); c = make_cfg(); c.forward_ms = BTM_MAX_MOVE_MS;
    CHECK(BTM_Init(&c) == HAL_OK, "sinirdaki deger kabul edilir");

    printf("29. Olceklenmis sure dolunca surucu KENDI frenler (PWM)\n");
    reset_all(); c = make_cfg(); c.forward_ms = 10U;   /* %50 -> 20 ms -> 4 periyot */
    (void)BTM_Init(&c);
    (void)BTM_MovePwm(BTM_DIR_FORWARD, 50U);
    CHECK(periods_left == 4U, "4 periyot (10 ms / %50 @ 200 Hz)");
    one_period(); one_period(); one_period(); one_period();
    (void)BTM_GetStatus(&st);
    CHECK(st.state == BTM_STATE_MOVING, "olceklenmis sure henuz dolmadi");
    BTM_OnTimerPeriod(&htim);
    CHECK(state == BTM_STATE_FINISHING, "sure doldu: son fren yolda");
    spi_done();
    (void)BTM_GetStatus(&st);
    CHECK(st.state == BTM_STATE_IDLE, "durum IDLE");
    CHECK(st.last_cmd == BTM_CMD_BRAKE, "son komut FREN");

    printf("30. Init FAULT_COMM ile dondu, kablo duzeltildi: ClearFault kurtarir\n");
    reset_all(); dev_absent = 1; c = make_cfg();
    CHECK(BTM_Init(&c) == HAL_ERROR, "onkosul: cihaz yok -> Init HAL_ERROR");
    (void)BTM_GetStatus(&st);
    CHECK(st.fault == BTM_FAULT_COMM, "onkosul: fault = COMM");
    CHECK(BTM_ClearFault() == HAL_ERROR, "cihaz hala yokken temizlenmez");
    dev_absent = 0;                              /* kablo takildi */
    CHECK(BTM_ClearFault() == HAL_OK, "kablo takilinca ClearFault HAL_OK");
    (void)BTM_GetStatus(&st);
    CHECK(st.fault == BTM_FAULT_NONE, "fault temizlendi");
    CHECK(st.echo_byte == BTM_CMD_BRAKE, "echo tuttu -> hat saglam");
    CHECK(BTM_Move(BTM_DIR_FORWARD) == HAL_OK, "Init tekrar cagirmadan hareket edilebiliyor");

    printf("31. Surus komutu gitti ama echo tutmadi: FREN de gonderilir\n");
    reset_all(); c = make_cfg(); c.forward_ms = 1000U;
    (void)BTM_Init(&c);
    dev_absent = 1;                          /* MISO koptu: echo 0xFF gelir */
    frame_count = 0; memset(frame_log, 0, sizeof(frame_log));
    CHECK(BTM_Move(BTM_DIR_FORWARD) != HAL_OK, "echo tutmadi -> hata dondu");
    (void)BTM_GetStatus(&st);
    CHECK(st.fault == BTM_FAULT_COMM, "fault = COMM");
    CHECK(frame_count == 6, "3 surus + 3 FREN cercevesi (surus komutu cihaza uygulanmisti)");
    CHECK((frame_log[5] & 0x7FU) == BTM_CMD_BRAKE, "son cerceve FREN");
    CHECK(tim_running == 0, "timer baslatilmadi");

    printf("32. Surus komutunun ortasinda SPI hatasi: FREN de gonderilir\n");
    reset_all(); c = make_cfg(); c.forward_ms = 1000U;
    (void)BTM_Init(&c);
    frame_count = 0; memset(frame_log, 0, sizeof(frame_log));
    spi_fail_in = 2;                         /* 2. cerceve bir kez HAL_ERROR */
    CHECK(BTM_Move(BTM_DIR_FORWARD) != HAL_OK, "SPI hatasi -> hata dondu");
    (void)BTM_GetStatus(&st);
    CHECK(st.fault == BTM_FAULT_SPI, "fault = SPI");
    CHECK(frame_count == 4, "1 surus cercevesi + 3 FREN cercevesi");
    CHECK((frame_log[3] & 0x7FU) == BTM_CMD_BRAKE, "son cerceve FREN");
    CHECK(tim_running == 0, "timer baslatilmadi");

    printf("33. Init: timer gercekten BTM_PWM_HZ'de donmuyorsa reddedilir\n");
    reset_all(); c = make_cfg(); tim_inst.PSC = 839U;   /* 84 MHz -> 100 Hz, yarisi */
    CHECK(BTM_Init(&c) == HAL_ERROR, "yanlis PSC -> HAL_ERROR");
    reset_all(); c = make_cfg(); c.tim_clk_hz = 0U;
    CHECK(BTM_Init(&c) == HAL_ERROR, "timer saati verilmemis -> HAL_ERROR");
    reset_all(); c = make_cfg(); htim.Instance = NULL;
    CHECK(BTM_Init(&c) == HAL_ERROR, "timer Instance bos -> HAL_ERROR");
    reset_all(); c = make_cfg();                        /* PSC 419 / ARR 999 / 84 MHz */
    CHECK(BTM_Init(&c) == HAL_OK, "dogru kurulum kabul edilir");
    reset_all(); c = make_cfg(); tim_inst.PSC = 430U;   /* 194.9 Hz: %5 toleransta */
    CHECK(BTM_Init(&c) == HAL_OK, "kucuk sapma kabul edilir");

    printf("34. Ters yone gecerken olu sure beklenir\n");
    reset_all(); c = make_cfg(); c.forward_ms = 10U; c.backward_ms = 10U;
    (void)BTM_Init(&c);
    (void)BTM_Move(BTM_DIR_FORWARD);
    one_period(); one_period();
    BTM_OnTimerPeriod(&htim); spi_done();                /* kendi frenledi */
    CHECK(BTM_Move(BTM_DIR_BACKWARD) == HAL_BUSY, "frenin hemen ardindan ters yon reddedilir");
    tick += BTM_REVERSE_MS + 10U;
    CHECK(BTM_Move(BTM_DIR_BACKWARD) == HAL_OK, "olu sure dolunca ters yon kabul edilir");

    printf("35. Isil dinlenme: FIILEN suren sure x BTM_REST_RATIO beklenir\n");
    reset_all(); c = make_cfg();                         /* forward_ms = 6000 */
    (void)BTM_Init(&c);
    (void)BTM_Move(BTM_DIR_FORWARD);
    tick += 2000U;                                       /* 2 saniye surdu */
    CHECK(BTM_Brake() == HAL_OK, "erken fren HAL_OK");
    CHECK(BTM_Move(BTM_DIR_FORWARD) == HAL_BUSY, "dinlenme dolmadan reddedilir");
    tick += (2000U * BTM_REST_RATIO) - 1500U;            /* dinlenmenin cogu gecti */
    CHECK(BTM_Move(BTM_DIR_FORWARD) == HAL_BUSY, "kismi dinlenme yetmez");
    tick += 2000U;
    CHECK(BTM_Move(BTM_DIR_FORWARD) == HAL_OK, "dinlenme dolunca kabul edilir");
    (void)BTM_GetStatus(&st);
    CHECK(st.state == BTM_STATE_MOVING, "hareket gercekten basladi");

    printf("36. BTM_Poll: bostayken cihazdan TAZE status bayti okur\n");
    reset_all(); c = make_cfg();
    (void)BTM_Init(&c);
    dev_status_bits = BTM_ST_EN | BTM_ST_UV;             /* besleme dustu */
    (void)BTM_GetStatus(&st);
    CHECK((st.status_byte & BTM_ST_UV) == 0U, "onbellekteki bayt hala eski");
    frame_count = 0; memset(frame_log, 0, sizeof(frame_log));
    CHECK(BTM_Poll() == HAL_OK, "Poll HAL_OK");
    CHECK(frame_count == 3, "dogrulanmis komut: 3 cerceve");
    CHECK((frame_log[0] & 0x7FU) == BTM_CMD_BRAKE, "FREN komutu tekrarlanir (kopruye etkisiz)");
    (void)BTM_GetStatus(&st);
    CHECK((st.status_byte & BTM_ST_UV) != 0U, "TAZE bayt: UV goruldu");
    CHECK(st.fault == BTM_FAULT_NONE, "tek UV okumasi fault uretmez (teyit ister)");
    CHECK(st.state == BTM_STATE_IDLE, "durum IDLE kaldi");

    reset_all(); c = make_cfg();                         /* harekette gerekmez */
    (void)BTM_Init(&c);
    (void)BTM_Move(BTM_DIR_FORWARD);
    CHECK(BTM_Poll() == HAL_BUSY, "harekette Poll HAL_BUSY (status zaten her periyotta)");

    printf("37. Timer'in bekleyen UIF bayragi hareketi kisaltmaz\n");
    reset_all(); c = make_cfg(); c.forward_ms = 1000U;      /* 200 periyot */
    (void)BTM_Init(&c);
    tim_inst.SR = TIM_FLAG_UPDATE;                          /* onceki hareketten kalan */
    CHECK(BTM_Move(BTM_DIR_FORWARD) == HAL_OK, "Move HAL_OK");
    CHECK(periods_left == (1000U * BTM_PWM_HZ) / 1000U, "periyot sayimi eksilmedi");
    CHECK(it_pending == 0, "hareket basinda ucusta hayalet cerceve yok");
    (void)BTM_GetStatus(&st);
    CHECK(st.dropped_frames == 0U, "dusen cerceve yok");

    printf("38. RemainingMs periyot sayimina dayanir, duvar saatine degil\n");
    reset_all(); c = make_cfg(); c.forward_ms = 100U;        /* 200 Hz'de 20 periyot */
    (void)BTM_Init(&c);
    (void)BTM_Move(BTM_DIR_FORWARD);
    CHECK(BTM_RemainingMs() == 100U, "basta tam sure");
    one_period(); one_period();                              /* 2 periyot = 10 ms */
    CHECK(BTM_RemainingMs() == 90U, "iki periyot sonra 90 ms");
    tick += 500U;                                            /* kesmeler gecikti */
    CHECK(BTM_RemainingMs() == 90U, "saat ilerledi ama hareket bitmedi: deger ayni");
    (void)BTM_GetStatus(&st);
    CHECK(st.state == BTM_STATE_MOVING, "surucu hala MOVING");

    printf("\n%s (fails=%d)\n", (fails == 0) ? "TUM TESTLER GECTI" : "BASARISIZ", fails);
    return (fails == 0) ? 0 : 1;
}
