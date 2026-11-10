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
 *   BTM_Init(&cfg);                        // timer frekansını ve hattı doğrular
 *   BTM_Move(BTM_DIR_FORWARD);             // veya BTM_MovePwm(dir, 60)
 *   while ((rem = BTM_RemainingMs()) > 0U)
 *       if (<rem ms içinde durdurma komutu geldi>) { BTM_Brake(); break; }
 *   // süre dolduysa sürücü KENDİ frenledi; ek bir şey gerekmez
 *
 * Kurallar:
 *   - Komut fonksiyonları tek sahip görevden, kesmeler açıkken çağrılır; ISR'dan
 *     veya kesmeler kapalıyken çağrılırsa hiçbir şey yapmadan HAL_BUSY döner.
 *   - Aktüatörü koruyan iki kapı vardır (README §3.3): ters yöne geçişte ölü süre
 *     (BTM_REVERSE_MS) ve ısıl dinlenme (BTM_REST_RATIO). Kapı kapalıysa hareket
 *     komutu HAL_BUSY döner; hiçbir şey olmaz, çağrı tekrarlanabilir.
 *   - Boştayken status baytı tazelenmez; güncel bayt için BTM_Poll() çağrılır.
 *   - HAL_OK çerçevenin gittiğini ve echo'nun tuttuğunu söyler; fiziksel hareketi
 *     doğrulamaz.
 *   - Hata kilitlidir: çıkmak için BTM_ClearFault() (FAULT_DWT hariç) veya reset.
 *   - SPI, CS pini, TIM CH1 ve DWT sayacı yalnız bu sürücüye aittir.
 *
 * TAŞINABİLİRLİK: Çekirdekte DWT çevrim sayacı bulunmalıdır (Cortex-M3/M4/M7;
 * M0/M0+'da yoktur). Cortex-M7'de DWT kilidi otomatik açılır. Başka bir STM32
 * ailesi için -DBTM9011EP_HAL_HEADER="\"stm32h7xx_hal.h\"" verilir; timer
 * PSC/ARR değerleri o ailenin saat ağacına göre yeniden hesaplanmalıdır.
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

/* Aşağıdakiler sürücünün iç zamanlama sabitleridir, ayar düğmesi DEĞİLDİR: testler
 * yalnız bu değerlerle koşuyor ve birkaçı birbirine bağlı (ör. BTM_FRAME_US duty alt
 * sınırını, o da süre ölçeklemesini etkiler). Değiştirmek gerekirse burada değiştir
 * ve testleri yeniden koştur. */
#define BTM_FRAME_US 40U           /* Bir çerçevenin kabaca süresi; duty alt sınırı buradan */
#define BTM_DROP_LIMIT 3U          /* Bu kadar ardışık düşen çerçeve -> FAULT_OVERLAP */
#define BTM_UV_CONFIRM 3U          /* UV bu kadar ardışık çerçevede teyit edilirse hata */
#define BTM_SPI_TIMEOUT_MS 10U     /* Polling çerçevesinin HAL timeout'u */
#define BTM_INFLIGHT_WAIT_MS 2U    /* Uçuştaki çerçevenin beklenme süresi (görev bağlamı) */
#define BTM_FINISH_TIMEOUT_MS 5U   /* Son fren çerçevesi takılırsa boşa geçme süresi */

/** AKTÜATÖRÜN ISIL SINIRI: bir hareketten sonra, o hareketin FİİLEN sürdüğü
 *  sürenin bu katı kadar dinlenme dolmadan yeni hareket kabul edilmez (HAL_BUSY).
 *  Actuonix P8 azami %20 duty cycle ister, yani 1 sür / 4 dinlen. Başka bir
 *  aktüatörde veri sayfasındaki orana göre değiştir; 0 bu kapıyı kapatır. */
#ifndef BTM_REST_RATIO
#define BTM_REST_RATIO 4U
#endif

/** TERS YÖNE GEÇİŞ ÖLÜ SÜRESİ (ms): fren sonrası motor hâlâ dönüyor olabilir;
 *  bu süre dolmadan ters yön komutu kabul edilmez (HAL_BUSY). Dönen motora ters
 *  gerilim bindirmek aşırı akıma (OCA/OCB) ve hata kilidine yol açar. */
#ifndef BTM_REVERSE_MS
#define BTM_REVERSE_MS 100U
#endif

/** Bir hareketin azami süresi. İki işi var:
 *  1) forward_ms/backward_ms bununla sınırlanır (Init reddeder) — böylece duty
 *     ölçeklemesindeki çarpımların taşmadığı ispatlanabilir;
 *  2) düşük duty'de ölçeklenen süre buraya kırpılır (duty %1 -> 100 kat).
 *  Kırpılma olduysa BTM_Status.move_ms_clamped = 1 olur. */
#define BTM_MAX_MOVE_MS 60000U

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
    uint32_t tim_clk_hz;       /* Zorunlu; timer'ın besleme saati (Hz). Init bununla
                                * PSC/ARR'dan gerçek frekansı hesaplar ve BTM_PWM_HZ
                                * ile uyuşmazsa HAL_ERROR döner. F4'te APB1 timer'ı
                                * (TIM2..TIM7) 84000000U, APB2 (TIM1/TIM8..) 168000000U. */
    uint32_t forward_ms;       /* İleri TAM HIZ strok süresi + emniyet payı.
                                * 1..BTM_MAX_MOVE_MS; düşük duty'de sürücü bunu
                                * kendi ölçekler. */
    uint32_t backward_ms;      /* Geri strok süresi + emniyet payı (aynı sınır). */
} BTM_Config;

typedef struct
{
    BTM_State state;
    BTM_Fault fault;
    uint32_t hal_error;      /* Hata anındaki HAL_SPI_GetError değeri. */
    uint8_t last_cmd;        /* Son uygulanan komut (SDO_SEL maskesiz). */
    uint8_t status_byte;     /* Son okunan HAM status baytı (geçersiz olsa da). */
    uint8_t status_valid;    /* SON okunan baytın iyi biçimli olup olmadığı. */
    uint8_t echo_byte;       /* Son HAM control byte okuması. last_cmd ile
                              * karşılaştır: eşitse hat sağlam, değilse hat/mod
                              * sorunu (0xFF = cihaz hatta yok). */
    uint32_t duty_percent;
    uint32_t dropped_frames; /* Ömür boyu toplam; ClearFault bunu SIFIRLAMAZ. */
    uint32_t move_ms;        /* Son hareket için PLANLANAN süre (duty ile
                              * ölçeklenmiş ve gerekiyorsa kırpılmış). Tam hızda
                              * forward_ms/backward_ms'e eşittir. */
    uint8_t move_ms_estimated; /* 1: duty < 100 olduğu için süre "hız ~ duty"
                                * doğrusal modeliyle TAHMİN edildi. */
    uint8_t move_ms_clamped;   /* 1: süre BTM_MAX_MOVE_MS'e kırpıldı; strok bu
                                * sürede TAMAMLANMAYABİLİR. */
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
 *
 *  SÜRE DUTY İLE ÖLÇEKLENİR: duty %50'de süre 2 katına çıkarılır, yani
 *  forward_ms/backward_ms "tam hız strok süresi" olarak kalır ve strok düşük
 *  duty'de de tamamlanır. Ölçekleme "hız ~ duty" doğrusal modeline dayanan bir
 *  TAHMİNDİR (aktüatörün ölü bölgesi ve yük etkisi modele girmez); bu yüzden
 *  BTM_Status.move_ms_estimated = 1 olur ve gerçek değer kartta ölçülmelidir.
 *  Ölçeklenen süre BTM_MAX_MOVE_MS'i aşarsa kırpılır ve move_ms_clamped = 1
 *  olur — o durumda strok tamamlanmayabilir. */
HAL_StatusTypeDef BTM_MovePwm(BTM_Direction dir, uint32_t duty_percent);

/** Hareketi ERKEN keser: timer'ı durdurur, doğrulanmış fren gönderir. Süre
 *  dolduysa gerekmez (sürücü zaten frenledi), ama çağrılması zararsızdır. */
HAL_StatusTypeDef BTM_Brake(void);

/** Hata kilidini açar: geçici bir arıza (UV dalgalanması, kesme gecikmesinden
 *  doğan OVERLAP) sonrası MCU reset'i gerekmesin diye. Timer'ı durdurur,
 *  sayaçları sıfırlar ve hattı DOĞRULANMIŞ FREN komutuyla yeniden sınar.
 *
 *  @retval HAL_OK    Hata yoktu, ya da temizlendi ve hat doğrulandı (durum IDLE).
 *  @retval HAL_BUSY  Hat bir an meşgul; HİÇBİR ŞEY değişmedi, hata yerinde
 *                    duruyor, çağrı tekrarlanabilir.
 *  @retval HAL_ERROR Temizlenemedi: arızanın sebebi sürüyor (yeni fault kaydı
 *                    BTM_GetStatus'ta görünür) veya fault FAULT_DWT (donanımsal,
 *                    temizlenemez) veya sürücü Init edilmemiş.
 *  @retval HAL_TIMEOUT  HAL'in SPI polling timeout'u; fault kaydedilmiştir, yani
 *                    HAL_ERROR ile aynı anlama gelir. Uygulama "HAL_OK ve HAL_BUSY
 *                    dışındaki her şey başarısızlıktır" diye okumalıdır.
 *
 *  Yalnız sahip görevden, kesmeler açıkken çağrılır (ISR'dan HAL_BUSY döner).
 *  dropped_frames ömür boyu sayaç olduğu için SIFIRLANMAZ. */
HAL_StatusTypeDef BTM_ClearFault(void);

/** Boştayken cihazdan TAZE status baytı okur; sonra BTM_GetStatus ile okunur.
 *  UART paket akışı için: "durum sorgusu" paketine güncel bayt ile cevap verilir.
 *
 *  Fren komutunu tekrarlar — boştayken köprü zaten frende olduğu için çıkışlara
 *  etkisi yoktur. 3 çerçeve (~90 us) harcar.
 *
 *  @retval HAL_OK    Taze bayt okundu ve değerlendirildi.
 *  @retval HAL_BUSY  Hareket sürüyor (çerçeveler status'u zaten her periyotta
 *                    tazeliyor), hat bir an meşgul, ya da ISR'dan çağrıldı.
 *  @retval HAL_ERROR Hata kilidi var (kurtarma BTM_ClearFault'un işi) veya Init
 *                    edilmemiş. Okuma cihaz hatası gösterirse fault kaydedilir. */
HAL_StatusTypeDef BTM_Poll(void);

/** Hareketin bitmesine kalan süre (ms); hareket yoksa 0. Sahip görevin bloke
 *  süresi olarak kullanılır. ISR'dan da çağrılabilir.
 *
 *  Değer, hareketi bitiren PERİYOT SAYACINDAN hesaplanır (duvar saatinden değil),
 *  bu yüzden bildirilen süre ile hareketin gerçek bitişi ayrışamaz: 0 döndüğünde
 *  sürücü gerçekten fren çerçevesine geçmiştir. */
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
