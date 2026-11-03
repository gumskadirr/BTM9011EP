/**
 * @file BTM9011EP.h
 * @brief BTM9011EP SPI sürücüsü (STM32 HAL). Actuonix P8 "-S" lineer aktüatörü
 *        iki uç nokta arasında sürer: tam hız veya PWM, ikisi de süreli.
 *
 * Kurulum, durum diyagramları ve gerekçeler: README.md
 *
 * GERÇEK ZAMAN SÖZLEŞMESİ (FOC gibi bir kontrol döngüsüyle aynı MCU'da):
 *   - Sürücü HİÇBİR YERDE kesme maskelemez (PRIMASK/BASEPRI kullanılmaz).
 *   - Sürücünün ISR'larında meşgul bekleme YOKTUR. Bekleyen kod yalnız görev
 *     bağlamındadır: Init'te 20 us, çerçeve başına en çok BTM_INFLIGHT_WAIT_MS.
 *   - ZORUNLU öncelik: kontrol döngüsü EN ÜSTTE; sürücünün SPI ve TIM kesmeleri
 *     birbirine EŞİT ve kontrol döngüsünün ALTINDA.
 *
 * Kullanım:
 *   BTM_Init(&cfg);                        // hattı echo ile doğrular
 *   BTM_Move(BTM_DIR_FORWARD);             // veya BTM_MovePwm(dir, 60)
 *   while ((rem = BTM_RemainingMs()) > 0U)
 *       if (<rem ms içinde durdurma komutu geldi>) { BTM_Brake(); break; }
 *   // süre dolduysa sürücü KENDİ frenledi; ek bir şey gerekmez
 *
 * Kurallar:
 *   - Komut fonksiyonları tek sahip görevden, kesmeler açıkken çağrılır; ISR'dan
 *     veya kesmeler kapalıyken çağrılırsa hiçbir şey yapmadan HAL_BUSY döner.
 *   - HAL_OK çerçevenin gittiğini ve echo'nun tuttuğunu söyler; fiziksel hareketi
 *     doğrulamaz.
 *   - İlk hata MCU reset'ine kadar kilitlidir.
 *   - SPI, CS pini, TIM CH1 ve DWT sayacı yalnız bu sürücüye aittir.
 */
#ifndef BTM9011EP_H
#define BTM9011EP_H

#ifndef BTM9011EP_HAL_HEADER
#define BTM9011EP_HAL_HEADER "stm32f4xx_hal.h"
#endif
#include BTM9011EP_HAL_HEADER

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== Ayarlar ==================== */

/** PWM taşıyıcı frekansı. Timer bu frekansa ayarlı OLMALIDIR: süre periyot
 *  sayılarak ölçülür. Düşük frekans kesme sayısını ve duty alt sınırını düşürür. */
#ifndef BTM_PWM_HZ
#define BTM_PWM_HZ 200U
#endif
#if (BTM_PWM_HZ < 50U) || (BTM_PWM_HZ > 1500U)
#error "BTM_PWM_HZ must be in 50..1500"
#endif

#ifndef BTM_FRAME_US           /* Bir çerçevenin kabaca süresi; duty alt sınırı buradan */
#define BTM_FRAME_US 40U
#endif
#ifndef BTM_DROP_LIMIT         /* Bu kadar ardışık düşen çerçeve -> FAULT_OVERLAP */
#define BTM_DROP_LIMIT 3U
#endif
#ifndef BTM_UV_CONFIRM         /* UV bu kadar ardışık çerçevede teyit edilirse hata */
#define BTM_UV_CONFIRM 3U
#endif
#ifndef BTM_SPI_TIMEOUT_MS     /* Polling çerçevesinin HAL timeout'u */
#define BTM_SPI_TIMEOUT_MS 10U
#endif
#ifndef BTM_INFLIGHT_WAIT_MS   /* Uçuştaki çerçevenin beklenme süresi (görev bağlamı) */
#define BTM_INFLIGHT_WAIT_MS 2U
#endif
#ifndef BTM_FINISH_TIMEOUT_MS  /* Son fren çerçevesi takılırsa boşa geçme süresi */
#define BTM_FINISH_TIMEOUT_MS 5U
#endif

/* Yön komutları. Hangisinin mekanik "ileri" olduğu OUTA/OUTB bağlantısına bağlıdır
 * ve KARTTA ÖLÇÜLMELİDİR. Ters çıkarsa kodu değil bu tanımları takas et.
 * (Datasheet Tablo 7'ye göre 0x51 "reverse", 0x31 "forward" desenidir.) */
#ifndef BTM_CMD_FORWARD
#define BTM_CMD_FORWARD 0x51U
#endif
#ifndef BTM_CMD_BACKWARD
#define BTM_CMD_BACKWARD 0x31U
#endif

#define BTM_CMD_BRAKE   0x11U  /* Low-side fren */
#define BTM_SDO_SEL     0x80U  /* Kontrol baytında: sonraki cevap status olsun */

/* Durum baytı bitleri (datasheet Şekil 28) */
#define BTM_ST_SDO_SEL  0x80U
#define BTM_ST_OCA      0x40U
#define BTM_ST_OCB      0x20U
#define BTM_ST_OL       0x10U  /* Yalnız teşhis desenlerinde geçerli; kullanılmaz */
#define BTM_ST_UV       0x08U
#define BTM_ST_TSDA     0x04U
#define BTM_ST_TSDB     0x02U
#define BTM_ST_EN       0x01U

#define BTM_ST_HARD     (BTM_ST_OCA | BTM_ST_OCB | BTM_ST_TSDA | BTM_ST_TSDB)

/* ==================== Tipler ==================== */

typedef enum { BTM_DIR_FORWARD = 0, BTM_DIR_BACKWARD } BTM_Direction;

typedef enum
{
    BTM_STATE_IDLE = 0,    /* Boşta; son komut fren. */
    BTM_STATE_MOVING,      /* Timer süreyi sayıyor (tam hız veya PWM). */
    BTM_STATE_FINISHING,   /* Süre doldu, son fren çerçevesi gidiyor. */
    BTM_STATE_FAULT        /* Kilitli; reset gerekir. */
} BTM_State;

typedef enum
{
    BTM_FAULT_NONE = 0,
    BTM_FAULT_COMM,     /* Echo uyuşmadı / 0xFF / beklenmedik SDO_SEL biti */
    BTM_FAULT_DEVICE,   /* Status baytında OC, TSD veya teyitli UV */
    BTM_FAULT_SPI,      /* HAL SPI hatası; hal_error alanına bak */
    BTM_FAULT_OVERLAP,  /* BTM_DROP_LIMIT kadar ardışık çerçeve düştü */
    BTM_FAULT_DWT       /* DWT çevrim sayacı saymıyor */
} BTM_Fault;

typedef struct
{
    SPI_HandleTypeDef *spi;    /* Zorunlu. */
    GPIO_TypeDef *cs_port;     /* Zorunlu; CS aktif High. */
    uint16_t cs_pin;           /* Zorunlu. */
    TIM_HandleTypeDef *tim;    /* Zorunlu; süreyi timer sayar. */
    uint32_t forward_ms;       /* İleri strok süresi + emniyet payı. */
    uint32_t backward_ms;      /* Geri strok süresi + emniyet payı. */
} BTM_Config;

typedef struct
{
    BTM_State state;
    BTM_Fault fault;
    uint32_t hal_error;      /* Hata anındaki HAL_SPI_GetError değeri. */
    uint8_t last_cmd;        /* Son uygulanan komut (SDO_SEL maskesiz). */
    uint8_t status_byte;     /* Son geçerli status baytı. */
    uint8_t status_valid;
    uint32_t duty_percent;
    uint32_t dropped_frames;
} BTM_Status;

/* ==================== API ==================== */

/** Bir kez. DWT'yi doğrular, CS'i Low'a çeker, hattı doğrulanmış fren komutuyla
 *  sınar. Echo tutmazsa HAL_ERROR + FAULT_COMM (cihaz yok / SPI modu / pinler). */
HAL_StatusTypeDef BTM_Init(const BTM_Config *cfg);

/** Tam hızda süreli strok hareketi. Süre config'ten gelir; dolunca sürücü KENDİ
 *  frenler. Her periyotta bir status baytı okunur. */
HAL_StatusTypeDef BTM_Move(BTM_Direction dir);

/** PWM ile süreli strok hareketi. duty 0 -> fren; 1..99 kullanılabilir aralığa
 *  sınırlanır; >=100 tam hız. Süre dolunca sürücü KENDİ frenler.
 *  NOT: forward_ms/backward_ms tam hız süresidir; düşük duty'de strok bu sürede
 *  tamamlanmaz. */
HAL_StatusTypeDef BTM_MovePwm(BTM_Direction dir, uint32_t duty_percent);

/** Hareketi ERKEN keser: timer'ı durdurur, doğrulanmış fren gönderir. Süre
 *  dolduysa gerekmez (sürücü zaten frenledi), ama çağrılması zararsızdır. */
HAL_StatusTypeDef BTM_Brake(void);

/** Hareketin bitmesine kalan süre (ms); hareket yoksa 0. Sahip görevin bloke
 *  süresi olarak kullanılır. ISR'dan da çağrılabilir. */
uint32_t BTM_RemainingMs(void);

/** Durum kopyası. Kesme maskelemez; alanlar tek tek geçerlidir. ISR'dan da olur. */
HAL_StatusTypeDef BTM_GetStatus(BTM_Status *status);

/* HAL callbacklerinden çağrılır; başka handle'lar yok sayılır. Hiçbiri meşgul
 * bekleme yapmaz ve kesme maskelemez. */
void BTM_OnTimerPeriod(TIM_HandleTypeDef *htim);
void BTM_OnTimerCompare(TIM_HandleTypeDef *htim);
void BTM_OnSpiComplete(SPI_HandleTypeDef *hspi);
void BTM_OnSpiError(SPI_HandleTypeDef *hspi);

#ifdef __cplusplus
}
#endif

#endif /* BTM9011EP_H */
