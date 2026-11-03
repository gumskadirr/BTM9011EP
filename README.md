# BTM9011EP sürücüsü (v2)

STM32 HAL için tek BTM9011EP sürücüsü: normal sürüş, süreli PWM, fren, latch
(kilit aktüatörü), elektronik teşhis ve hata durumunda güvenli kapanış.

Sürücü pin veya çevrebirim adına bağlı değildir; SPI, CS ve timer handle'ları
config ile verilir. Bu klasör başka bir projeye olduğu gibi kopyalanabilir.

| Dosya | İçerik |
|---|---|
| `BTM9011EP.h` / `BTM9011EP.c` | Sürücü |
| `README.md` | Kod tarafı entegrasyon ve API (bu belge) |
| [CUBEMX_KURULUM.md](CUBEMX_KURULUM.md) | CubeMX / `.ioc` gereklilikleri, saat ve NVIC hesapları |

## Entegrasyon

### 1. Dosyalar

`BTM9011EP.c` dosyasını derlemeye, bu klasörü include yollarına ekle
(CubeIDE: Paths and Symbols → Source Location ve Includes). Projede v1 sürücüsü
(`Lib/BTM9011EP`) varsa derlemeden çıkar; iki sürümün header adı aynıdır.

STM32F4 dışındaki aileler için `-DBTM9011EP_HAL_HEADER="\"stm32g4xx_hal.h\""`
tanımla. Çekirdekte DWT bulunmalıdır (Cortex-M3/M4/M7).

### 2. CubeMX

Sürücüye ayrılmış bir SPI, bir CS pini ve (PWM / latch için) bir timer gerekir.
Ayarlar ve hesaplar [CUBEMX_KURULUM.md](CUBEMX_KURULUM.md) içindedir.

### 3. Config ve Init

`MX_*_Init()` çağrılarından sonra, ana döngü veya scheduler başlamadan önce
(`main.c` USER CODE 2):

```c
#include "BTM9011EP.h"

static const BTM9011EP_Config btm_config = {
    .spi     = &hspiX,              /* sürücüye ayrılmış SPI */
    .cs_port = BTM_CS_GPIO_Port,    /* CubeMX label'ına göre */
    .cs_pin  = BTM_CS_Pin,
    .timer   = &htimY,              /* PWM ve latch için; ikisi de yoksa NULL */

    /* Latch kullanılmayacaksa bu üç alanı yazma (0 = kapalı). */
    .latch_lock_dir  = BTM9011EP_FORWARD,  /* kilitleme yönü; açma tersi */
    .latch_travel_ms = 2000U,              /* strok süresi, kartta ölçülmeli */
    .latch_duty      = 100U,               /* 0 ise %100 */
};

if (BTM9011EP_Init(&btm_config) != HAL_OK)
{
    Error_Handler();
}
```

Init config'i kopyalar ve başlangıç frenini polling SPI ile gönderir. Timer'ı
başlatmaz; timer'ı yalnız sürücü başlatıp durdurur (`HAL_TIM_Base_Start_IT`
uygulamaya eklenmez).

### 4. Callback yönlendirmeleri

`main.c` USER CODE 4. Callback projede zaten tanımlıysa yeni fonksiyon yazma;
satırı mevcut fonksiyonun başına ekle. Sürücü kendi handle'ı dışındaki çağrıları
yok sayar.

```c
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)        { BTM9011EP_OnSpiComplete(hspi); }
void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)           { BTM9011EP_OnSpiError(hspi); }
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)   { BTM9011EP_OnTimerPeriod(htim); }
void HAL_TIM_OC_DelayElapsedCallback(TIM_HandleTypeDef *htim) { BTM9011EP_OnTimerCompare(htim); }
```

- SPI callbackleri her zaman gerekir (teşhis ve hata freni kesmeli aktarım kullanır).
- TIM callbackleri PWM ve latch için gerekir.
- HAL timebase bir TIMx ise (FreeRTOS projelerinde olağan) `HAL_TIM_PeriodElapsedCallback`
  CubeMX tarafından üretilir; satır onun `USER CODE Callback 1` bloğuna yazılır.

### 5. Sahip bağlam ve Service

Komut fonksiyonları (`Drive`, `Brake`, `PwmStart`, `SetDuty`, `DiagStart`,
`TransferByte`, `LatchLock/Unlock`) ve `BTM9011EP_Service()` **tek bir bağlamdan**
çağrılır: ana döngü veya tek bir RTOS görevi. `GetStatus` her yerden çağrılabilir.

`Service`, `status.busy == 1` iken (PWM/latch hareketi, teşhis veya hata
kapanışı sürüyor) **en geç 1 ms'de bir** zorunludur. `busy == 0` iken iş yapmaz.
Gecikmeli Service yanlış hata üretmez; yalnız gerçek bir hatanın işlenmesini geciktirir.

Bare-metal:

```c
while (1)
{
    BTM9011EP_Service();
    /* komut kaynağı → BTM9011EP_LatchLock() vb. */
}
```

FreeRTOS (sahip görev, komut beklediği yerde):

```c
BTM9011EP_Status st;
TickType_t wait = normal_bekleme;          /* ör. portMAX_DELAY */
if ((BTM9011EP_GetStatus(&st) == HAL_OK) && (st.busy != 0U))
{
    wait = 1U;                             /* 1 tick = 1 ms */
}
/* xQueueReceive(..., wait) → komut varsa sürücü fonksiyonunu çağır */
BTM9011EP_Service();
```

Komutlar başka bir görevden (UART, CAN) geliyorsa sürücüyü oradan çağırma;
komutu kuyruk veya thread flag ile sahip göreve aktar.

## API

| Fonksiyon | Davranış |
|---|---|
| `BTM9011EP_Init(&cfg)` | Bir kez; başlangıç freni. Eksik config `HAL_ERROR`, ikinci çağrı `HAL_BUSY`. |
| `BTM9011EP_Drive(dir)` | Normal ileri (0x51) / geri (0x31), polling. Otomatik durmaz. |
| `BTM9011EP_Brake()` | NORMAL: 0x11 fren. PWM/latch: asenkron durdurma. Teşhis: iptal. |
| `BTM9011EP_PwmStart(dir, duty, ms)` | Süreli PWM isteği; timer gerekir. |
| `BTM9011EP_SetDuty(duty)` | Çalışan PWM'in duty değerini değiştirir. |
| `BTM9011EP_LatchLock()` / `BTM9011EP_LatchUnlock()` | Config'teki yön/süre/duty ile süreli hareket; bitince fren. |
| `BTM9011EP_DiagStart(type)` | Elektronik teşhis isteği; sonuç `Service` ile üretilir. |
| `BTM9011EP_TransferByte(cmd, &rx)` | Ham tek bayt aktarım (polling). `rx` NULL olabilir. |
| `BTM9011EP_GetStatus(&st)` | Donanıma dokunmadan durum kopyası; ISR'dan da çağrılabilir. |
| `BTM9011EP_Service()` | Zaman aşımı, teşhis adımları ve hata kapanışı. |

Dönüş değerleri: `HAL_OK` kabul / başarı, `HAL_BUSY` şu an uygun değil
(sonra tekrar dene), `HAL_ERROR` geçersiz parametre, kapalı özellik veya fault.
`HAL_OK` isteğin kabulüdür; motorun fiziksel hareketini doğrulamaz.

Komut fonksiyonları ISR'dan veya kesmeler kapalıyken çağrılırsa hiçbir şey
yapmadan `HAL_BUSY` döner.

### PWM

PWM fiziksel bir pin değildir: 1 kHz timer ile periyot başında sürüş, CH1 compare
anında fren komutu SPI'dan gönderilir. Duty `0` fren, `>=100` tam sürüş (compare
anında fren yok), aradaki değerler %10..90'a sınırlanır. Çalışan PWM isteği
değişirse önce 200 ms fren uygulanır, süre bundan sonra sayılır. Süre bitince son
fren gönderilir, timer durur ve mod `NORMAL` olur.

### Latch (kilit aktüatörü)

`latch_travel_ms == 0` ise latch kapalıdır ve `LatchLock/Unlock` `HAL_ERROR` döner.
Timer gerekir.

- `LatchLock()` / `LatchUnlock()` config'teki değerlerle `PwmStart(yön, duty, travel)`
  isteği verir. Yalnız `NORMAL` modda kabul edilir; hareket veya teşhis sürerken
  `HAL_BUSY`, fault'ta `HAL_ERROR`. Ters yöne geçişte 200 ms fren aralığı korunur;
  süre aradan sonra sayılır.
- Durdurmak için `BTM9011EP_Brake()`.
- Sonuç `status.latch_state`:

| Durum | Anlam |
|---|---|
| `UNKNOWN` | Reset sonrası, `Drive` veya doğrudan `PwmStart` sonrası |
| `LOCKING` / `UNLOCKING` | Hareket sürüyor |
| `LOCKED` / `UNLOCKED` | Strok süresi tamamlandı (fiziksel konum doğrulanmaz) |
| `STOPPED` | Hareket `Brake` ile kesildi; konum arada |
| `FAULT` | Sürücü fault'ta |

Konum sensörü yoktur. Reset sonrası konum bilinmez; bilinen konuma gelmek için
bir kez kilitle veya aç. Yön ters çıkarsa `latch_lock_dir` değiştirilir.

### Teşhis

```c
/* Önce motor durdurulmuş olmalı (son komut fren/coast/standby). */
if (BTM9011EP_DiagStart(BTM9011EP_DIAG_OPEN_LOAD_HSA) == HAL_OK) { /* kabul */ }

/* Sonraki turlarda: */
BTM9011EP_Status st;
BTM9011EP_GetStatus(&st);
switch (st.diag_result) {
    case BTM9011EP_DIAG_RESULT_RUNNING:  break;                  /* sürüyor */
    case BTM9011EP_DIAG_RESULT_DETECTED: /* açık yük algılandı */ break;
    case BTM9011EP_DIAG_RESULT_NO_FLAG:  /* bayrak yok */         break;
    default: /* INVALID / CANCELLED / FAILED: geçerli ölçüm değil */ break;
}
```

Akış: test komutu → yerleşme (`BTM9011EP_DIAG_SETTLE_MS`, varsayılan 2 ms) →
aynı komutla durum okuması → 0x11 fren. Sonuç son fren bittikten sonra
yayınlanır. `diag_status` ham cihaz baytıdır (yalnız NO_FLAG/DETECTED'da geçerli).

| Test | Komut | Değerlendirme |
|---|---|---|
| `BTM9011EP_DIAG_OPEN_LOAD_HSA` | 0xC1 | OL biti |
| `BTM9011EP_DIAG_OPEN_LOAD_HSB` | 0xA9 | OL biti |
| `BTM9011EP_DIAG_SHORT_TO_GND`  | 0x89 | OL biti |

MISO pull-up'lı ve cihaz bağlı değilse okunan bayt `0xFF` olur ve sonuç
`DETECTED` çıkar. `diag_status == 0xFF` şüpheli kabul edilmelidir.

### Hata durumu

İlk hata (`fault`) MCU reset'e kadar kilitli kalır; tüm komutlar `HAL_ERROR`
döner. `Service` timer'ı durdurur, SPI'ı sıfırlar ve bir kez 0x11 fren dener.
Sonuç `fault_stop` (`COMPLETE` / `FAILED`) ve `fault_stop_status` alanlarında
izlenir. `COMPLETE` fiziksel duruş onayı değildir.

| `fault` | Sebep |
|---|---|
| `OVERLAP` | PWM'de önceki SPI çerçevesi bitmeden yenisi istendi (kesme gecikmesi) |
| `SPI` | HAL SPI hatası; `hal_error` = `HAL_SPI_GetError` |
| `TIMER` | Timer başlatılamadı / durdurulamadı |
| `TIMEOUT` | Kesmeli aktarım `BTM9011EP_ASYNC_TIMEOUT_MS` içinde bitmedi |

## Uygulama örneği: butonla deneme

Ana döngüden çağrılır; her basışta yön değiştirerek 500 ms PWM verir:

```c
static void button_task(void)
{
    static uint8_t last_raw, stable, forward = 1U;
    static uint32_t change_tick;
    uint8_t raw = (HAL_GPIO_ReadPin(DIR_BUTTON_GPIO_Port, DIR_BUTTON_Pin) == GPIO_PIN_SET);
    uint32_t now = HAL_GetTick();

    if (raw != last_raw) { last_raw = raw; change_tick = now; }
    if ((raw != stable) && ((now - change_tick) >= 30U)) {   /* 30 ms filtre */
        stable = raw;
        if (raw && (BTM9011EP_PwmStart(forward ? BTM9011EP_FORWARD : BTM9011EP_BACKWARD,
                                       50U, 500U) == HAL_OK)) {
            forward = !forward;
        }
    }
}
```

## v1 → v2 geçiş

| v1 | v2 |
|---|---|
| `latch_forward()` / `latch_backward()` | `BTM9011EP_Drive(...)` (süresiz) veya `BTM9011EP_LatchLock/Unlock()` (süreli) |
| `latch_forward_pwm(d, ms)` | `BTM9011EP_PwmStart(BTM9011EP_FORWARD, d, ms)` |
| `latch_brake()`, `BTM9011EP_PwmStop()` | `BTM9011EP_Brake()` |
| `latch_test(LATCH_TEST_*)` | `BTM9011EP_DiagStart(BTM9011EP_DIAG_*)` |
| `latch_status()`, `BTM9011EP_GetStatus()` | `BTM9011EP_GetStatus()` (`latch_state`, `diag_result`) |
| `test_state` + `last_test_valid` + `device_status_valid` + `last_test_result` | `diag_result` |
| `motion` | `last_command` / `last_command_valid` |
| `BTM9011EP_Process()`, `button_port/pin` | Uygulamaya taşındı (yukarıdaki örnek) |

## Bilinen sınırlar

- Tek cihaz: sürücü durumu statik değişkenlerde tutulur.
- Her SPI çerçevesi CS için yaklaşık 26 µs meşgul bekleme yapar; PWM'de bunun bir
  kısmı kesme içindedir. 328 kHz SCK ile bir çerçeve yaklaşık 55-60 µs sürer ve
  %10 duty'de (100 µs) sığar. Daha yavaş SCK veya büyük kesme gecikmesi `OVERLAP`
  üretebilir. Kartta ölçülmelidir.
- Normal komutlar ve Init polling SPI kullanır (20 ms timeout); kesmeler açık olmalıdır.
- Latch'te konum geri bildirimi yoktur; `LOCKED/UNLOCKED` süre tabanlıdır.

## Test

Sürücünün host testi (taklit HAL ile gerçek `BTM9011EP.c`'yi derler; PWM, latch,
teşhis ve hata senaryoları) referans projede `tests/test_btm9011ep_v2.py`
dosyasındadır. Fiziksel zamanlama ve motor davranışı kartta ayrıca doğrulanmalıdır.
