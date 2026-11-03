/**
 * @file BTM9011EP.h
 * @brief BTM9011EP SPI sürücüsü (STM32 HAL): normal sürüş, süreli PWM, fren,
 *        elektronik teşhis ve hata durumunda güvenli kapanış.
 *
 * Kurulum, CubeMX ayarları ve örnekler README.md içindedir. Kısaca:
 *   1. MX_GPIO/SPI(/TIM) init sonrası bir kez BTM9011EP_Init().
 *   2. HAL SPI callbacklerinden BTM9011EP_OnSpiComplete/OnSpiError,
 *      PWM kullanılıyorsa TIM callbacklerinden OnTimerPeriod/OnTimerCompare.
 *   3. Komut görevinden BTM9011EP_Service(); status.busy iken en geç 1 ms'de bir.
 *
 * Kurallar:
 *   - Komut fonksiyonları tek görevden, kesmeler açıkken çağrılır; ISR'dan veya
 *     kesmeler kapalıyken çağrılırsa hiçbir şey yapmadan HAL_BUSY döner.
 *   - HAL_OK aktarımın/isteğin kabulüdür; motorun fiziksel hareketini doğrulamaz.
 *   - İlk hata (fault) MCU reset'e kadar kilitli kalır. Service bir kez fren
 *     göndermeyi dener; sonucu status.fault_stop alanında izlenir.
 *   - SPI, CS pini, DWT çevrim sayacı ve verilmişse TIM CH1 sürücüye aittir.
 */
#ifndef BTM9011EP_H
#define BTM9011EP_H

/* Başka bir STM32 ailesi için -DBTM9011EP_HAL_HEADER="\"stm32g4xx_hal.h\"" verilir. */
#ifndef BTM9011EP_HAL_HEADER
#define BTM9011EP_HAL_HEADER "stm32f4xx_hal.h"
#endif
#include BTM9011EP_HAL_HEADER

/* Kesmeli 1 bayt aktarımın zaman aşımı. Kartta ölçülerek doğrulanmalı. */
#ifndef BTM9011EP_ASYNC_TIMEOUT_MS
#define BTM9011EP_ASYNC_TIMEOUT_MS 2U
#endif
#if BTM9011EP_ASYNC_TIMEOUT_MS == 0U
#error "BTM9011EP_ASYNC_TIMEOUT_MS must be greater than zero"
#endif

/* Teşhis komutu ile durum okuması arasındaki bekleme; tick farkı bu değeri aşınca
 * okunur. Veri sayfasındaki 5 us minimumdur, kartta doğrulanmalı. */
#ifndef BTM9011EP_DIAG_SETTLE_MS
#define BTM9011EP_DIAG_SETTLE_MS 2U
#endif
#if (BTM9011EP_DIAG_SETTLE_MS == 0U) || (BTM9011EP_DIAG_SETTLE_MS >= 0x80000000U)
#error "BTM9011EP_DIAG_SETTLE_MS must be in 1..0x7FFFFFFF"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Kontrol baytları */
#define BTM9011EP_CMD_STANDBY        0x00U
#define BTM9011EP_CMD_COAST          0x01U
#define BTM9011EP_CMD_DRIVE_FORWARD  0x51U
#define BTM9011EP_CMD_DRIVE_BACKWARD 0x31U
#define BTM9011EP_CMD_BRAKE_LOW      0x11U
#define BTM9011EP_CMD_BRAKE_HIGH     0x61U

#define BTM9011EP_MASK_SDO_SEL 0x80U  /* Sonraki yanıt durum baytı olsun */
#define BTM9011EP_MASK_SEL     0x08U
#define BTM9011EP_MASK_SEN_EN  0x04U
#define BTM9011EP_MASK_SR      0x02U

/* Durum baytı bitleri */
#define BTM9011EP_STATUS_SDO_SEL 0x80U
#define BTM9011EP_STATUS_OCA     0x40U
#define BTM9011EP_STATUS_OCB     0x20U
#define BTM9011EP_STATUS_OL      0x10U
#define BTM9011EP_STATUS_UV      0x08U
#define BTM9011EP_STATUS_TSDA    0x04U
#define BTM9011EP_STATUS_TSDB    0x02U
#define BTM9011EP_STATUS_EN      0x01U

typedef enum
{
    BTM9011EP_FORWARD = 0U,
    BTM9011EP_BACKWARD
} BTM9011EP_Direction;

typedef struct
{
    SPI_HandleTypeDef *spi;    /* Zorunlu. */
    GPIO_TypeDef *cs_port;     /* Zorunlu; CS aktif High. */
    uint16_t cs_pin;           /* Zorunlu. */
    TIM_HandleTypeDef *timer;  /* İsteğe bağlı; NULL ise PWM kullanılamaz. */

    /* Latch (isteğe bağlı, timer gerekir). latch_travel_ms 0 ise kapalıdır. */
    BTM9011EP_Direction latch_lock_dir; /* Kilitleme yönü; açma tersidir. */
    uint32_t latch_travel_ms;           /* Strok süresi; bitince fren. */
    uint32_t latch_duty;                /* 0 ise %100 (tam sürüş). */
} BTM9011EP_Config;

typedef enum
{
    BTM9011EP_MODE_NORMAL = 0U,
    BTM9011EP_MODE_PWM,
    BTM9011EP_MODE_PWM_STOPPING   /* Son fren gönderiliyor, sonra NORMAL. */
} BTM9011EP_Mode;

typedef enum
{
    BTM9011EP_FAULT_NONE = 0U,
    BTM9011EP_FAULT_OVERLAP,      /* Önceki SPI çerçevesi bitmeden yenisi istendi. */
    BTM9011EP_FAULT_SPI,
    BTM9011EP_FAULT_TIMER,
    BTM9011EP_FAULT_TIMEOUT
} BTM9011EP_Fault;

/* Hata sonrası kapanış. COMPLETE yazılım kapanışıdır, fiziksel duruş onayı değildir. */
typedef enum
{
    BTM9011EP_FAULT_STOP_IDLE = 0U,
    BTM9011EP_FAULT_STOP_PENDING,
    BTM9011EP_FAULT_STOP_BRAKING,
    BTM9011EP_FAULT_STOP_COMPLETE,
    BTM9011EP_FAULT_STOP_FAILED
} BTM9011EP_FaultStop;

typedef enum
{
    BTM9011EP_DIAG_OPEN_LOAD_HSA = 1U,  /* 0xC1: OUTA High, OUTB Low */
    BTM9011EP_DIAG_OPEN_LOAD_HSB,       /* 0xA9: OUTA Low, OUTB High */
    BTM9011EP_DIAG_SHORT_TO_GND         /* 0x89 */
} BTM9011EP_DiagType;

/* Teşhis sonucu. Yalnız NO_FLAG ve DETECTED geçerli bir ölçümdür. */
typedef enum
{
    BTM9011EP_DIAG_RESULT_NONE = 0U,     /* Henüz test yapılmadı. */
    BTM9011EP_DIAG_RESULT_RUNNING,
    BTM9011EP_DIAG_RESULT_NO_FLAG,       /* OL=0 (sağlık garantisi değildir). */
    BTM9011EP_DIAG_RESULT_DETECTED,      /* OL=1 */
    BTM9011EP_DIAG_RESULT_INVALID,       /* Yanıtta SDO_SEL biti yok. */
    BTM9011EP_DIAG_RESULT_CANCELLED,     /* BTM9011EP_Brake() ile iptal edildi. */
    BTM9011EP_DIAG_RESULT_FAILED         /* Aktarım hatası; fault alanına bak. */
} BTM9011EP_DiagResult;

/* Latch durumu. Konum sensörü yoktur: LOCKED/UNLOCKED strok süresinin
 * tamamlandığını gösterir, fiziksel konumu doğrulamaz. */
typedef enum
{
    BTM9011EP_LATCH_UNKNOWN = 0U,  /* Reset, Drive veya doğrudan PwmStart sonrası. */
    BTM9011EP_LATCH_LOCKING,
    BTM9011EP_LATCH_LOCKED,
    BTM9011EP_LATCH_UNLOCKING,
    BTM9011EP_LATCH_UNLOCKED,
    BTM9011EP_LATCH_STOPPED,       /* Hareket Brake ile kesildi; konum arada. */
    BTM9011EP_LATCH_FAULT          /* Sürücü fault'ta. */
} BTM9011EP_LatchState;

typedef struct
{
    BTM9011EP_Mode mode;
    uint8_t busy;                 /* Aktarım/PWM/teşhis/kapanış sürüyor. */
    uint8_t last_command;         /* Son başarılı çerçevenin komutu. */
    uint8_t last_command_valid;
    uint8_t rx_byte;              /* Son alınan ham bayt. */
    uint32_t duty_percent;        /* İstenen PWM duty. */

    BTM9011EP_Fault fault;        /* İlk hata; reset'e kadar korunur. */
    HAL_StatusTypeDef last_status;
    uint32_t hal_error;           /* İlk hatadaki HAL_SPI_GetError değeri. */
    BTM9011EP_FaultStop fault_stop;
    HAL_StatusTypeDef fault_stop_status;

    BTM9011EP_DiagType diag_type;     /* Son istenen test (yoksa 0). */
    BTM9011EP_DiagResult diag_result;
    uint8_t diag_status;              /* NO_FLAG/DETECTED ise cihaz durum baytı. */
    uint32_t diag_tick;               /* O baytın alındığı HAL tick. */

    BTM9011EP_LatchState latch_state;
} BTM9011EP_Status;

/** Bir kez çağrılır. Başlangıç frenini polling SPI ile gönderir.
 *  Eksik config: HAL_ERROR, ikinci çağrı: HAL_BUSY. Başarısızlıkta reset gerekir. */
HAL_StatusTypeDef BTM9011EP_Init(const BTM9011EP_Config *config);

/** Tek bayt polling aktarım (20 ms timeout). Yalnız NORMAL modda ve boştayken.
 *  response NULL olabilir. Komut motor durumunu değiştirir, otomatik fren yoktur. */
HAL_StatusTypeDef BTM9011EP_TransferByte(uint8_t command, uint8_t *response);

/** Normal sürüş komutu (polling). Süre/fren yok; durdurmak için BTM9011EP_Brake. */
HAL_StatusTypeDef BTM9011EP_Drive(BTM9011EP_Direction direction);

/** Moda göre durdurur:
 *  NORMAL : polling ile 0x11 fren gönderir.
 *  PWM    : asenkron durdurma; son fren bitince mod NORMAL olur (HAL_OK = kabul).
 *  Teşhis : testi iptal eder; Service son freni gönderir, sonuç CANCELLED olur. */
HAL_StatusTypeDef BTM9011EP_Brake(void);

/** Timer gerektirir. Süreli PWM isteği; beklemez, sonraki timer periyodunda başlar.
 *  duration_ms > 0. Çalışan PWM isteği değişirse önce 200 ms fren uygulanır.
 *  Duty: 0 = fren, >= 100 = tam sürüş, aradaki değerler %10..90'a sınırlanır. */
HAL_StatusTypeDef BTM9011EP_PwmStart(BTM9011EP_Direction direction,
                                     uint32_t duty_percent, uint32_t duration_ms);

/** Çalışan PWM'in duty değerini değiştirir; PWM başlatmaz. Teşhis sırasında yok sayılır. */
void BTM9011EP_SetDuty(uint32_t percent);

/** Elektronik teşhis isteği; timer gerekmez. NORMAL modda ve son komut fren,
 *  coast veya standby iken kabul edilir, aksi halde HAL_BUSY. Sonuç Service
 *  tarafından üretilir ve status.diag_result alanından okunur. */
HAL_StatusTypeDef BTM9011EP_DiagStart(BTM9011EP_DiagType type);

/** Config'teki yön, süre ve duty ile süreli PWM başlatır; süre bitince fren
 *  otomatiktir. Yalnız NORMAL modda ve teşhis yokken kabul edilir, hareket
 *  sürerken HAL_BUSY. Latch kapalıysa (travel 0) veya timer yoksa HAL_ERROR.
 *  Durdurmak için BTM9011EP_Brake; sonuç status.latch_state alanındadır. */
HAL_StatusTypeDef BTM9011EP_LatchLock(void);
HAL_StatusTypeDef BTM9011EP_LatchUnlock(void);

/** Donanıma dokunmadan tutarlı bir durum kopyası verir. ISR'dan da çağrılabilir.
 *  NULL veya Init öncesi HAL_ERROR; aksi halde fault olsa da HAL_OK. */
HAL_StatusTypeDef BTM9011EP_GetStatus(BTM9011EP_Status *status);

/** Komut görevinden çağrılır. Teşhis adımlarını, aktarım zaman aşımını ve hata
 *  kapanışını yürütür. status.busy 1 iken en geç 1 ms'de bir zorunludur; busy 0
 *  iken iş yoktur, görev bir sonraki komuta kadar bekleyebilir. */
void BTM9011EP_Service(void);

/* HAL callbacklerinden çağrılır; başka handle'lar yok sayılır. */
void BTM9011EP_OnTimerPeriod(TIM_HandleTypeDef *htim);
void BTM9011EP_OnTimerCompare(TIM_HandleTypeDef *htim);
void BTM9011EP_OnSpiComplete(SPI_HandleTypeDef *hspi);
void BTM9011EP_OnSpiError(SPI_HandleTypeDef *hspi);

#ifdef __cplusplus
}
#endif

#endif /* BTM9011EP_H */
