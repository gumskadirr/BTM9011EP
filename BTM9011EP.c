/**
 * @file BTM9011EP.c
 * @brief BTM9011EP SPI sürücüsü. Sözleşme ve kurallar için BTM9011EP.h'ye bak.
 *
 * TEK SÜRÜŞ YOLU: tam hız da PWM de timer üzerinden yürür. Tam hız, "duty %100,
 * compare çerçevesi yok" demektir. Süreyi her iki kipte de timer sayar ve dolunca
 * sürücü kendi frenler; uygulamanın sonda bir şey yapması gerekmez. Süre UYGULANAN
 * duty'ye göre ölçeklenir (plan_move_ms), yani config'teki değer "tam hız strok
 * süresi"dir ve strok düşük duty'de de tamamlanır.
 *
 * HATA KİLİDİ geçicidir: BTM_ClearFault() hattı doğrulanmış fren komutuyla yeniden
 * sınar ve tutarsa kilidi açar. FAULT_DWT donanımsal olduğu için temizlenmez.
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

static volatile BTM_State state = BTM_STATE_IDLE;
static volatile uint32_t cs_low_cycles;
static volatile uint8_t tx_byte;
static volatile uint8_t rx_byte;

static volatile uint8_t last_cmd = BTM_CMD_BRAKE;
static volatile uint8_t pwm_cmd;
static volatile uint32_t periods_left;
static volatile uint32_t duty_percent;
static volatile uint32_t move_start_tick;    /* Hareketin başladığı an (ısıl bütçe) */
static volatile uint32_t finish_tick;
static volatile uint32_t move_ms;            /* Planlanan hareket süresi */
static volatile uint8_t move_ms_estimated;   /* duty<100: süre tahmin edildi */
static volatile uint8_t move_ms_clamped;     /* BTM_MAX_MOVE_MS'e kırpıldı */
static volatile uint32_t last_stop_tick;     /* Son hareketin bittiği an */
static volatile uint32_t last_move_ms;       /* O hareketin FİİLEN sürdüğü süre */
static volatile BTM_Direction last_dir;      /* Son sürülen yön */

static volatile uint8_t status_byte;
static volatile uint8_t status_valid;
static volatile uint8_t echo_byte;       /* Son HAM control byte okuması */
static volatile uint32_t dropped_frames;
static volatile uint32_t drop_run;
static volatile uint32_t uv_run;
static volatile BTM_Fault fault = BTM_FAULT_NONE;
static volatile uint32_t hal_error;

/* ==================== Yardımcılar ==================== */

/** Bellek bariyeri: durum yazmaları, onlara bakan ISR'ların göreceği sırayla
 *  tamamlansın (sürücü kesme maskelemediği için sıralama tek güvencedir). */
static void barrier(void) { __DMB(); }
static void set_state(BTM_State s) { state = s; barrier(); }

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
#if defined(__CORTEX_M) && (__CORTEX_M == 7U)
    /* Cortex-M7'de (F7/H7) DWT kilitli olabilir: CTRL yazılabilmesi için önce
     * Lock Access Register'a anahtar yazılır. M4'te bu yazmaç yoktur. */
    DWT->LAR = 0xC5ACCE55U;
#endif
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

/**
 * Hareket süresini duty ile ölçekler ve BTM_MAX_MOVE_MS'e kırpar.
 *
 * Model: ortalama köprü gerilimi duty ile doğrusal, fırça DC motorun boştaki
 * hızı gerilimle doğrusal -> süre ~ 1/duty. Bu bir TAHMİNDİR: aktüatörün ölü
 * bölgesi (çok düşük duty'de hiç kalkmaması), yük ve sürtünme modele girmez.
 * Bu yüzden ölçekleme yapıldığında move_ms_estimated bayrağı kalkar ve gerçek
 * süre kartta ölçülmelidir (README §8).
 *
 * Taşma: travel Init'te BTM_MAX_MOVE_MS (<= 600000) ile sınırlandığı için
 * travel * 100 en çok 6e7'dir; uint32 sınırının çok altında.
 */
static uint32_t plan_move_ms(uint32_t travel, uint32_t duty)
{
    uint32_t ms = travel;

    move_ms_estimated = 0U;
    move_ms_clamped = 0U;
    if (duty == 0U) { duty = 1U; }   /* çağrı yolları engelliyor; sıfıra bölme kalkanı */

    if (duty < 100U)
    {
        ms = ((travel * 100U) + (duty - 1U)) / duty;   /* yukarı yuvarla */
        move_ms_estimated = 1U;
    }
    if (ms > BTM_MAX_MOVE_MS)
    {
        ms = BTM_MAX_MOVE_MS;
        move_ms_clamped = 1U;      /* strok tamamlanmayabilir */
    }
    return ms;
}

/**
 * Timer gerçekten BTM_PWM_HZ'de mi dönüyor?
 *
 * Süre sayımı periyotları saydığı için (periods_left), timer başka bir frekansta
 * dönerse BÜTÜN süreler aynı oranda kayar: yanlış PSC girildiğinde hareket iki kat
 * uzun sürer, ısıl bütçe iki katına çıkar ve BTM_RemainingMs gerçekle ayrışır.
 * Kurulumda en sık yapılan hata budur (F4'te timer'ın APB1'de mi APB2'de mi olduğu
 * karıştırılır), bu yüzden kart üstünde değil Init'te yakalanır. %5 tolerans,
 * tam bölünmeyen PSC/ARR seçimlerine yer bırakır.
 */
static uint8_t tim_hz_ok(const BTM_Config *c)
{
    uint32_t div, hz;

    if ((c->tim_clk_hz == 0U) || (c->tim->Instance == NULL)) { return 0U; }
    div = (c->tim->Instance->PSC + 1U) * (__HAL_TIM_GET_AUTORELOAD(c->tim) + 1U);
    if (div == 0U) { return 0U; }

    hz = c->tim_clk_hz / div;
    return ((hz >= (BTM_PWM_HZ - (BTM_PWM_HZ / 20U))) &&
            (hz <= (BTM_PWM_HZ + (BTM_PWM_HZ / 20U)))) ? 1U : 0U;
}

/**
 * Hareket bitti: FİİLEN süren sürüş süresini ve bitiş anını saklar. Isıl dinlenme
 * (BTM_REST_RATIO) ve ters yön ölü süresi (BTM_REVERSE_MS) bunlardan hesaplanır.
 *
 * Erken frende planlanan değil GEÇEN süre esas alınır: 6 s'lik strok 2 s sonra
 * kesildiyse aktüatör 2 s ısındı, 24 s değil 8 s dinlenme ister. Ölçü duvar saatidir
 * (periyot sayısı değil): kesmeler gecikse bile köprü o süre boyunca sürüyordu.
 *
 * YALNIZ gerçekten süren bir hareketten çıkarken çağrılır; boştaki bir fren
 * çağrısı dinlenme saatini yeniden başlatmamalıdır.
 */
static void note_stop(void)
{
    uint32_t now = HAL_GetTick();

    last_move_ms = now - move_start_tick;   /* duvar saati: aktüatör gerçekte bu kadar ısındı */
    last_stop_tick = now;
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
    barrier();
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
    /* Ham bayt HER ZAMAN saklanır, geçerliliği ayrı bayrakta: COMM hatasında
     * "ne okundu" debug'da görünsün. 0xFF = cihaz hatta yok (MISO pull-up);
     * başka bir desen = SPI modu / bit sırası / kablolama. Bit 7'si 1 olduğu
     * için 0xFF, "bit7 set ise status baytıdır" testini tek başına geçerdi. */
    status_byte = s;
    status_valid = ((s != 0xFFU) && ((s & BTM_ST_SDO_SEL) != 0U)) ? 1U : 0U;
    if (!status_valid)
    {
        record_fault(BTM_FAULT_COMM);
        return;
    }

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
    echo_byte = echo;    /* Doğrulamadan ÖNCE sakla: hata halinde de elde kalsın. */

    res = xfer((uint8_t)(base | BTM_SDO_SEL), &stat);
    if (res == HAL_BUSY) { return HAL_BUSY; }
    if (res != HAL_OK)   { record_fault(BTM_FAULT_SPI); return res; }

    last_cmd = base;
    drop_run = 0U;

    /* Echo çerçevesinde bit 7'nin 0 olması beklenir (çerçeve 1 SDO_SEL=0 idi);
     * 0xFF burada beklenen 0'a uymadığı için yakalanır. */
    if (echo != base) { record_fault(BTM_FAULT_COMM); return HAL_ERROR; }

    eval_status(stat);
    barrier();
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
    note_stop();
    stop_timer();
    finish_tick = HAL_GetTick();
    set_state(BTM_STATE_FINISHING);

    /* Son fren çerçevesi başlatılamazsa BOŞA GEÇME. Tam hız kipinde compare
     * çerçevesi olmadığı için son gönderilen komut SÜRÜŞTÜR; burada IDLE ilan
     * etmek "boştayım, hata yok" derken motoru sürüşte bırakmak olurdu.
     * Duruşu garanti edemedik: hata olarak bildir, uygulama görsün. */
    if (!send_it(BTM_CMD_BRAKE)) { record_fault(BTM_FAULT_OVERLAP); }
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
    /* Strok süreleri BTM_MAX_MOVE_MS ile sınırlı: hem anlamsız değerleri eler hem
     * plan_move_ms()'teki çarpımların taşmadığını ispatlanabilir kılar. */
    if ((config->forward_ms == 0U) || (config->forward_ms > BTM_MAX_MOVE_MS) ||
        (config->backward_ms == 0U) || (config->backward_ms > BTM_MAX_MOVE_MS))
    {
        return HAL_ERROR;
    }
    if (!tim_hz_ok(config)) { return HAL_ERROR; }

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
    uint32_t travel, arr, idle;
    HAL_StatusTypeDef res;

    if (!inited)         { return HAL_ERROR; }
    if (!task_context()) { return HAL_BUSY; }
    if (fault != BTM_FAULT_NONE) { return HAL_ERROR; }
    if ((dir != BTM_DIR_FORWARD) && (dir != BTM_DIR_BACKWARD)) { return HAL_ERROR; }

    travel = dir_travel_ms(dir);
    if (travel == 0U) { return HAL_ERROR; }

    release_stuck_finish();
    if (state != BTM_STATE_IDLE) { return HAL_BUSY; }

    /* Aktüatörü koru (ikisi de donanım gerçeği, ayarları BTM9011EP.h'de):
     *   - ters yöne geçişte ölü süre: dönen motora ters gerilim = aşırı akım,
     *   - ısıl dinlenme: P8 azami %20 duty cycle ister.
     * HAL_BUSY döner: geçicidir, çağrı tekrarlanabilir, fault üretmez. İlk
     * hareketten önce last_move_ms 0 olduğu için iki kapı da etkisizdir. */
    idle = HAL_GetTick() - last_stop_tick;
    if (last_move_ms != 0U)
    {
        if ((dir != last_dir) && (idle < BTM_REVERSE_MS)) { return HAL_BUSY; }
        if (idle < (last_move_ms * BTM_REST_RATIO))       { return HAL_BUSY; }
    }

    duty_percent = (duty >= 100U) ? 100U : clamp_duty(duty);
    pwm_cmd = dir_cmd(dir);
    last_dir = dir;

    /* Süre UYGULANAN duty'ye göre ölçeklenir: duty %50 -> süre 2 katı. Böylece
     * forward_ms/backward_ms "tam hız strok süresi" olarak kalır ve strok düşük
     * duty'de de tamamlanır. Kırpıldıysa move_ms_clamped bayrağı kalkar. */
    move_ms = plan_move_ms(travel, duty_percent);
    move_start_tick = HAL_GetTick();
    uv_run = 0U;
    drop_run = 0U;

    /* Komut önce doğrulanmış biçimde gider: hattın ve cihazın sağlığı çerçeve
     * akışı başlamadan bilinsin. Timer henüz durmuş olduğu için ISR çalışmaz. */
    res = send_verified(pwm_cmd);
    if (res != HAL_OK)
    {
        /* HAL_BUSY: ilk çerçeve hiç gitmedi, köprü sürüşe alınmadı — fren gerekmez
         * ve meşgul hatta beklemek boşunadır (HAL_BUSY geçicidir, çevrilmez).
         * Diğer hatalarda çerçeve(ler) GİTTİ, yani köprü sürüşte olabilir: komut
         * CS düşerken uygulanır ve echo/status ancak sonraki çerçevede görülür.
         * Duruşu garanti etmek için fren denenir, hata yine çağırana döner. */
        if (res != HAL_BUSY) { (void)send_verified(BTM_CMD_BRAKE); }
        return res;
    }

    arr = __HAL_TIM_GET_AUTORELOAD(cfg.tim);
    __HAL_TIM_SET_COUNTER(cfg.tim, 0U);
    __HAL_TIM_SET_COMPARE(cfg.tim, TIM_CHANNEL_1, ((arr + 1U) * duty_percent) / 100U);

    /* Durdurulmus timer'in SR'sinde UIF/CC1IF bekleyebilir (sayici dururken bayrak
     * temizlenmez). Sayici yeniden baslatilinca NVIC o bekleyen kesmeyi ANINDA
     * calistirir: hareket bir periyot eksik baslar ve hatta gereksiz bir cerceve
     * girer. Bu yuzden kapi acilmadan once bayraklar temizlenir. */
    __HAL_TIM_CLEAR_FLAG(cfg.tim, TIM_FLAG_UPDATE | TIM_FLAG_CC1);

    periods_left = (move_ms * BTM_PWM_HZ) / 1000U;
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
        if (state == BTM_STATE_MOVING) { note_stop(); }
        state = BTM_STATE_IDLE;   /* ÖNCE kapıyı kapat, SONRA timer'ı durdur */
        __DMB();
        barrier();
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

/**
 * Hata kilidini açar. Sözleşme BTM9011EP.h'de.
 *
 * SIRA ÖNEMLİ: timer durdurulur, uçuştaki çerçevenin bitmesi beklenir, SONRA
 * sayaçlar sıfırlanıp durum IDLE'a alınır. Timer durmuş ve durum MOVING değil
 * olduğu için ISR'lar bu sırada hiçbir çerçeve göndermez.
 *
 * Hattı doğrulanmış FREN komutuyla sınar — temizleme ancak cihaz konuşuyorsa ve
 * status baytı temizse başarılıdır. Arızanın sebebi sürüyorsa (OC hâlâ set, UV
 * hâlâ düşük) send_verified içinde yeni bir fault kaydedilir ve kilit geri gelir;
 * yani bu fonksiyon "hatayı yok sayma" değil "yeniden sınama"dır.
 *
 * HAL_BUSY yolunda HİÇBİR ŞEY değişmez: eski fault ve FAULT durumu geri konur,
 * çünkü hiçbir şey doğrulanamadı.
 */
HAL_StatusTypeDef BTM_ClearFault(void)
{
    BTM_Fault prev_fault;
    uint32_t prev_hal_error, prev_drop_run, prev_uv_run, t0;
    HAL_StatusTypeDef res;

    if (!inited)         { return HAL_ERROR; }
    if (!task_context()) { return HAL_BUSY; }
    if (fault == BTM_FAULT_NONE) { return HAL_OK; }    /* temizlenecek bir şey yok */

    /* DWT sayacı donanımsaldır: yeniden sınamanın anlamı yok, delay_us güvenilmez
     * kalır. Reset (veya debugger'ı bırakmak) gerekir. */
    if (fault == BTM_FAULT_DWT) { return HAL_ERROR; }

    stop_timer();

    /* Fault çerçeve ortasında oluşmuş olabilir; uçuştaki aktarımı sınırlı bekle. */
    t0 = HAL_GetTick();
    while (cfg.spi->State != HAL_SPI_STATE_READY)
    {
        if ((HAL_GetTick() - t0) > BTM_INFLIGHT_WAIT_MS) { return HAL_BUSY; }
    }

    /* HAL_BUSY yolunda durumu BİREBİR geri koyabilmek için hepsi saklanır. */
    prev_fault = fault;
    prev_hal_error = hal_error;
    prev_drop_run = drop_run;
    prev_uv_run = uv_run;

    /* Kapı IDLE'a alınır ve ardışık sayaçlar sıfırlanır. dropped_frames ömür boyu
     * toplam olduğu için KORUNUR: teşhis geçmişi silinmez. */
    drop_run = 0U;
    uv_run = 0U;
    hal_error = 0U;
    fault = BTM_FAULT_NONE;
    __DMB();
    state = BTM_STATE_IDLE;
    barrier();

    res = send_verified(BTM_CMD_BRAKE);
    if (res == HAL_BUSY)
    {
        fault = prev_fault;        /* hiçbir şey doğrulanmadı: eski hali geri koy */
        hal_error = prev_hal_error;
        drop_run = prev_drop_run;
        uv_run = prev_uv_run;
        __DMB();
        state = BTM_STATE_FAULT;
        barrier();
        return HAL_BUSY;
    }
    /* res != HAL_OK ise send_verified zaten record_fault çağırdı: kilit geri geldi
     * ve BTM_GetStatus yeni sebebi gösterir. */
    return res;
}

/**
 * Boştayken cihazdan TAZE status baytı okur. Sözleşme BTM9011EP.h'de.
 *
 * Fren komutunu tekrarlar: boştayken köprü zaten frende olduğu için bu çerçeveler
 * çıkışları değiştirmez, ama cevap olarak güncel status baytını getirir. Hareket
 * sırasında gerekmez — kesmeli çerçeveler her periyotta status okur, bu yüzden
 * MOVING/FINISHING'de HAL_BUSY döner ve hatta çerçeve harcanmaz.
 */
HAL_StatusTypeDef BTM_Poll(void)
{
    if (!inited)         { return HAL_ERROR; }
    if (!task_context()) { return HAL_BUSY; }
    if (fault != BTM_FAULT_NONE) { return HAL_ERROR; }   /* kurtarma ClearFault'un işi */
    if (state != BTM_STATE_IDLE) { return HAL_BUSY; }

    return send_verified(BTM_CMD_BRAKE);
}

uint32_t BTM_RemainingMs(void)
{
    /* Hareketi bitiren şey periyot sayacıdır, bu yüzden kalan süre de ondan
     * hesaplanır: bildirilen değer ile hareketin gerçek bitişi ayrışamaz. */
    if (state != BTM_STATE_MOVING) { return 0U; }
    return (periods_left * 1000U) / BTM_PWM_HZ;
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
    status->echo_byte      = echo_byte;
    status->duty_percent   = duty_percent;
    status->dropped_frames = dropped_frames;
    status->move_ms           = move_ms;
    status->move_ms_estimated = move_ms_estimated;
    status->move_ms_clamped   = move_ms_clamped;
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
        else { barrier(); }
    }
}

void BTM_OnSpiError(SPI_HandleTypeDef *hspi)
{
    if ((!inited) || (hspi != cfg.spi)) { return; }
    /* Bağlı olmasaydı CS High'da kalır ve sürüş komutu kilitli halde donardı. */
    cs_drop();
    record_fault(BTM_FAULT_SPI);
}
