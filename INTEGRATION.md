# BTM9011EP sürücüsünü projeye entegre etme

Bu belge sürücüyü **herhangi bir STM32 HAL projesine** (CubeMX/CubeIDE, bare-metal
veya RTOS) bağlamanın adımlarını anlatır. Tasarım gerekçesi ve sınırlar için
[README.md](README.md), kopyalanabilir kod için [`examples/`](examples/) klasörü.

- [1. Gereksinimler](#1-gereksinimler)
- [2. Çevre birimleri](#2-çevre-birimleri)
- [3. Adım 1 — Dosyaları ekle ve include et](#3-adım-1--dosyaları-ekle-ve-include-et)
- [4. Adım 2 — HAL callback'lerini bağla](#4-adım-2--hal-callbacklerini-bağla)
- [5. Adım 3 — Yapılandırma ve başlatma](#5-adım-3--yapılandırma-ve-başlatma)
- [6. Adım 4 — İleri/geri hareket ve durum okuma](#6-adım-4--ileri-geri-hareket-ve-durum-okuma)
- [7. Sahip görev kuralı](#7-sahip-görev-kuralı)
- [8. Entegrasyon kontrol listesi](#8-entegrasyon-kontrol-listesi)
- [9. Devreye alma sırası](#9-devreye-alma-sırası)
- [10. Sorun giderme](#10-sorun-giderme)
- [11. Doğrulama durumu](#11-doğrulama-durumu)

---

## 1. Gereksinimler

| Gereken | Not |
|---|---|
| STM32 HAL (F4 varsayılan) | Başka aile: `-DBTM9011EP_HAL_HEADER="\"stm32h7xx_hal.h\""` (README §6.1) |
| DWT çevrim sayacı olan çekirdek | Cortex-M3/M4/M7. **M0/M0+ çalışmaz** |
| 1 SPI (kesmeli, DMA'sız) | Sürücüye ayrılır, paylaşılmaz |
| 1 GPIO çıkışı | Chip select, **aktif High** |
| 1 timer: CH1 Output Compare + kesme | Sürücüye ayrılır. Update ve CC1 kesmesi ikisi de gerekir |
| `USE_HAL_SPI_REGISTER_CALLBACKS = 0` ve `USE_HAL_TIM_REGISTER_CALLBACKS = 0` | `stm32xxxx_hal_conf.h` |

DWT sayacı (`DWT->CYCCNT`) yalnız sürücüye aittir; başka kod onu sıfırlamamalı.

## 2. Çevre birimleri

CubeMX'te (veya elle) şu ayarlar yapılır. Pin ve çevre birimi seçimi sana kalmıştır;
tek koşul ayrılan SPI ve timer'ın başka işte kullanılmamasıdır.

### SPI

| Alan | Değer |
|---|---|
| Mod | Full-Duplex Master |
| Veri boyutu / bit sırası | 8 bit / MSB First |
| **CPOL / CPHA** | **Low / 2 Edge** (SPI Mode 1) |
| NSS | Software |
| CRC | Disabled |
| Hız | ≈ **328 kHz** SCK (F4'te 42 MHz APB1 ile prescaler 128) |
| Kesme | **SPI global interrupt Enabled**, DMA kullanılmaz |
| MISO pini | **Pull-up**. Cihaz bağlı değilken hat yüzmez, `0xFF` okunur ve `BTM_Init` bunu yakalar |
| SCK / MOSI / MISO hızı | Very High |

### CS pini

GPIO Output, Push-Pull, No pull. **Başlangıç seviyesi Low** (cihaz aktif High'dır;
`MX_GPIO_Init` içinde `HAL_GPIO_WritePin(..., GPIO_PIN_RESET)` olmalı).

### Timer

| Alan | Değer |
|---|---|
| Clock source | Internal |
| Channel 1 | **Output Compare, No Output**; mod **Frozen** (`TIM_OCMODE_TIMING`) |
| Prescaler / Period (ARR) | Frekans `BTM_PWM_HZ`'e (varsayılan **200 Hz**) eşit olacak şekilde, aşağıya bak |
| Auto-reload preload | Enable |
| Kesmeler | Update **ve** CC1. Genel amaçlı timer'da tek global IRQ ikisini taşır; TIM1/TIM8'de **iki IRQ da** açılır |

**Frekans formülü:** `timer_saati / ((PSC + 1) × (ARR + 1)) = BTM_PWM_HZ`.
`ARR = 999` seçilirse `PSC + 1 = timer_saati / (BTM_PWM_HZ × 1000)`:

| Timer besleme saati | PSC (200 Hz) |
|---|---|
| 84 MHz (F4 APB1: TIM2..TIM7, TIM12..14) | 419 |
| 168 MHz (F4 APB2: TIM1, TIM8..11) | 839 |

"Timer besleme saati" APB çarpanı dahil değerdir (APB prescaler ≠ 1 ise saat ×2).
CubeMX Clock Configuration ekranında "APBx Timer clocks" olarak görünür.
`BTM_Init` frekansı kendisi hesaplayıp `BTM_PWM_HZ` ile karşılaştırır; **%5'ten fazla
sapma varsa `HAL_ERROR` döner**.

### NVIC önceliği (zorunlu)

```
   kontrol döngün / ADC / hızlı kesmelerin  ◄── en yüksek (sayısal olarak en küçük)
   sürücü SPI + TIM                          ◄── ikisi BİRBİRİNE EŞİT, yukarıdakilerin ALTINDA
   diğer kesmeler
```

- **SPI ve timer kesmeleri aynı preemption önceliğinde olmalı.** Sürücünün paylaşılan
  durum mantığı birbirlerini kesmemelerine dayanır.
- Sürücü kesme maskelemez ve ISR'ında bekleme yapmaz; zamana duyarlı bir kontrol
  döngün varsa onu sürücünün **üstüne** koy.
- Sürücü ISR'ları RTOS API'si çağırmaz; FreeRTOS'ta `configMAX_SYSCALL_INTERRUPT_PRIORITY`
  kısıtı sürücü için geçerli değildir.
- HAL timebase'i SysTick'ten başka bir timer'a taşıdıysan (FreeRTOS'ta olağan), o
  timer sürücünün timer'ı olamaz.

## 3. Adım 1 — Dosyaları ekle ve include et

**3.1 Derlemeye ekle.** `BTM9011EP.c` ve `BTM9011EP.h` dosyalarını projeye kopyala,
klasörü hem **source** hem **include** yoluna ekle (CubeIDE: Properties → C/C++ General →
Paths and Symbols). Sürücü yalnız HAL başlığını include eder; FreeRTOS gerektirmez.
**`examples/` klasörünü derlemeye ekleme** (kaynak klasörün altındaysa dışarıda bırak):
örnekler HAL callback'lerini tanımlar ve projedekilerle çakışır.

**3.2 Include'u ekle.** Sürücüyü çağıracağın dosyada (burada `Core/Src/main.c`),
`USER CODE Includes` bloğuna:

```c
/* USER CODE BEGIN Includes */
#include "BTM9011EP.h"
/* USER CODE END Includes */
```

Eklenmezse derleme `unknown type name 'BTM_Config'` ile durur. Bu adımdan sonra proje
**derlenmeli** (henüz çağrı yok).

**3.3 (İsteğe bağlı) Ayar sembolleri.** Sürücü dosyasını değiştirmeden derleyici
sembolü olarak verilir: `BTM_CMD_FORWARD=0x31U`, `BTM_CMD_BACKWARD=0x51U` (yön ters
çıkarsa, §9 adım 2), `BTM_PWM_HZ`, `BTM_REST_RATIO`, `BTM_REVERSE_MS` (README §7).

## 4. Adım 2 — HAL callback'lerini bağla

Sürücünün çalışması için HAL'in dört callback'i ona yönlendirilmelidir. **Hiçbiri
yönlendirilmezse sürücü sessizce takılır** (SPI çerçevesi hiç bitmez, süre hiç dolmaz).
Sürücü kendi handle'ı dışındakileri yok sayar, çağrıyı koşulsuz yapabilirsin.

Satırları CubeMX'in `USER CODE` blokları **içine** yaz; dışına yazarsan CubeMX kodu
yeniden ürettiğinde silinir.

**4.1 SPI callback'leri** — `/* USER CODE BEGIN 4 */` bloğunda. Projede zaten varsa
(`SPI1` filtresiyle başlıyorsa) yenisini yazma, sürücü satırını **en üste, filtrenin
üstüne** ekle; altına konursa SPI2 olayı sürücüye hiç ulaşmaz:

```c
/* USER CODE BEGIN 4 */
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    BTM_OnSpiComplete(hspi);            /* <-- EN ÜSTE, filtreden önce */

    if (hspi->Instance != SPI1) { return; }   /* senin mevcut kodun */
    /* ... */
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    BTM_OnSpiError(hspi);               /* <-- EN ÜSTE, filtreden önce */

    if (hspi->Instance != SPI1) { return; }
    /* ... */
}
/* USER CODE END 4 */
```

Projede bu callback'ler yoksa yukarıdakilerin yalnız ilk satırlı hali yeterlidir.

**4.2 Timer callback'leri.** `HAL_TIM_PeriodElapsedCallback` HAL timebase'i
(`HAL_IncTick`) için CubeMX tarafından zaten üretilmiştir; sürücü satırı
**`USER CODE BEGIN Callback 0` bloğunun içine**, timebase `if`'inden önce konur:

```c
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */
  BTM_OnTimerPeriod(htim);              /* <-- bu bloğun İÇİNDE */
  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM6)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}
```

`HAL_TIM_OC_DelayElapsedCallback` genellikle projede yoktur; `USER CODE 4` bloğuna
yeni yaz (projede varsa satırı içine ekle):

```c
/* USER CODE BEGIN 4 */
void HAL_TIM_OC_DelayElapsedCallback(TIM_HandleTypeDef *htim)
{
    BTM_OnTimerCompare(htim);
}
/* USER CODE END 4 */
```

Aynı callback'e başka sürücüler de bağlıysa hepsi sırayla çağrılır; sürücü hiçbir şeyi
tüketmez. Dört satırı ekledikten sonra proje **derlenmeli**.

## 5. Adım 3 — Yapılandırma ve başlatma

**5.1 `BTM_Config`'i tanımla.** `main.c`'de `USER CODE BEGIN PV` bloğuna.
`hspi2` ve `htim4`, CubeMX'in `main.c` başında tanımladığı handle'lardır; kendi
SPI/timer adlarınla değiştir:

```c
/* USER CODE BEGIN PV */
static const BTM_Config btm_cfg = {
    .spi         = &hspi2,
    .cs_port     = BTM_CS_GPIO_Port,   /* main.h: CubeMX'te CS pinine verdiğin etiket */
    .cs_pin      = BTM_CS_Pin,
    .tim         = &htim4,
    .tim_clk_hz  = 84000000U,          /* timer'ın BESLEME saati; PSC ile bölünmüş değer değil */
    .forward_ms  = 6000U,              /* KARTTA ÖLÇ: tam hız strok süresi + pay */
    .backward_ms = 6000U,
};
static volatile BTM_Status btm_dbg;    /* debugger'da izlemek için */
/* USER CODE END PV */
```

**5.2 `BTM_Init`'i çağır.** Bir kez, **kesmeler açıkken ve görev bağlamında**,
yani RTOS'ta scheduler başladıktan sonra sahip görevin ilk işi olarak. CubeMX'in
ürettiği görev gövdesinde `USER CODE` bloğunun başına (projende `StartMainTask`,
`USER CODE BEGIN 5`):

```c
void StartMainTask(void *argument)
{
  /* USER CODE BEGIN 5 */
  BTM_Status st;

  if (BTM_Init(&btm_cfg) != HAL_OK) {
      (void)BTM_GetStatus(&st);   /* debugger'da st.fault ve st.echo_byte'a bak */
      btm_dbg = st;
      Error_Handler();            /* veya: hatayı kaydet, sistemi çalışır bırak */
  }

  /* Infinite loop */
  for(;;)
  {
      /* ... mevcut döngün ... */
  }
  /* USER CODE END 5 */
}
```

`BTM_Init` hattı motora komut vermeden, doğrulanmış bir fren komutuyla sınar.
**Dönüş değerini mutlaka kontrol et**; yok sayarsan kablo/pin/SPI modu hatası sessizce
geçer. `Error_Handler()` sistemi durduruyorsa cihaz bağlı değilken kart hiç açılmaz;
bu istenmiyorsa hatayı bir değişkende tutup diğer görevlerin çalışmasına izin ver.

RTOS'suz projede aynı bloğu `USER CODE BEGIN 2`'ye (tüm `MX_*_Init` çağrılarından
sonra, `while(1)`'den önce) koy.

> **Scheduler'dan önce** (`osKernelStart()` öncesi) çağırmak, kesmeler açık ve HAL
> timebase çalışıyorsa genellikle işe yarar, ama sürücünün sözleşmesi görev bağlamıdır
> ve bu yol doğrulanmış değildir. Şüphede çağrıyı sahip görevin başına taşı.

| `BTM_Init` dönüşü | `BTM_GetStatus().fault` | Anlamı |
|---|---|---|
| `HAL_OK` | `NONE` | Hat ve cihaz doğrulandı |
| `HAL_ERROR` | `COMM` | Cihaz cevap vermiyor: besleme/GND, SPI modu, pinler, MISO pull-up |
| `HAL_ERROR` | `DWT` | DWT sayacı saymıyor (debugger almış olabilir; M0'da yok) |
| `HAL_ERROR` | `NONE` | **Config reddedildi:** `tim`/`spi` NULL, `tim_clk_hz` yanlış veya PSC/ARR ile frekans tutmuyor, strok süresi 0 veya `BTM_MAX_MOVE_MS`'ten büyük |

Bu adımdan sonra projeyi **derle, yükle**. Motor BAĞLI DEĞİLKEN `BTM_Init` HAL_OK
dönmeli; dönmüyorsa §10'a bak, devam etme.

## 6. Adım 4 — İleri/geri hareket ve durum okuma

Önce hattı doğrula, **sonra** motoru bağla ve hareket dene. Aşağıdaki iki blok
`BTM_Init` başarılı olduktan sonra, aynı görevde çağrılır (kodun tamamı:
[`examples/first_test.c`](examples/first_test.c)).

**6.1 Hat kontrolü (motor bağlı değil).** `BTM_Init` sonrasına:

```c
(void)BTM_Poll();             /* boştayken TAZE status baytı (3 çerçeve, ~90 µs) */
(void)BTM_GetStatus(&st);
btm_dbg = st;                 /* beklenen: st.fault == BTM_FAULT_NONE,
                                 st.echo_byte == st.last_cmd (fren: 0x11) */
```

**6.2 İleri, sonra geri (motor bağlı).**

```c
if (BTM_Move(BTM_DIR_FORWARD) == HAL_OK) {
    while (BTM_RemainingMs() > 0U) { osDelay(10); }   /* RTOS'suz: HAL_Delay(10) */
}

osDelay(500);                 /* ters yön ölü süresi (100 ms) + pay */

if (BTM_Move(BTM_DIR_BACKWARD) == HAL_OK) {
    while (BTM_RemainingMs() > 0U) { osDelay(10); }
}

(void)BTM_GetStatus(&st);
btm_dbg = st;
```

PWM için `BTM_MovePwm(BTM_DIR_BACKWARD, 60)` (60 = %60 duty). Süre dolunca sürücü
**kendi frenler**; ek çağrı gerekmez.

### Dönüş değerleri

| Dönüş | Ne yapmalı |
|---|---|
| `HAL_OK` | Komut gitti, echo tuttu. Fiziksel hareketi **doğrulamaz** |
| `HAL_BUSY` | **Hiçbir şey olmadı**, sonra tekrar dene. Sebep: hat meşgul, ters yön ölü süresi (`BTM_REVERSE_MS`) veya ısıl dinlenme (`BTM_REST_RATIO`) dolmadı, ya da ISR/kesmeler kapalı bağlamdan çağrıldı |
| diğer | Komut cihaza gitmiş olabilir: `BTM_Brake()` çağır, `BTM_GetStatus` ile nedene bak |

PWM'de süre duty ile ölçeklenir (%50'de 2 katı); bu bir tahmindir, kartta doğrula
(README §3.1). Duty 0 fren, ≥100 tam hızdır.

### Erken durdurma

```c
while ((rem = BTM_RemainingMs()) > 0U) {
    /* rem ms içinde durdurma isteği geldiyse: */
    (void)BTM_Brake();  break;
}
```

`BTM_RemainingMs()` 0 döndüğünde sürücü fren çerçevesine geçmiştir. RTOS'ta bu süre
kadar kuyrukta uyunur, yoklama yapılmaz (§7).

### Durum alanları

`BTM_GetStatus` kesme maskelemez, ISR'dan da çağrılabilir. Hareket sırasında status
her periyotta tazelenir; boştayken yalnız `BTM_Poll` tazeler (hareket sürerken
`HAL_BUSY` döner, `GetStatus` son değeri verir).

| Alan | Anlamı |
|---|---|
| `state` | `IDLE`, `MOVING`, `FINISHING`, `FAULT` |
| `fault` | `NONE`, `COMM`, `DEVICE`, `SPI`, `OVERLAP`, `DWT` |
| `status_byte` | Ham cihaz baytı; bitler: `BTM_ST_OCA/OCB` (aşırı akım), `TSDA/TSDB` (aşırı ısı), `UV` |
| `echo_byte` vs `last_cmd` | Eşitse hat sağlam; `0xFF` cihaz yok |
| `move_ms`, `move_ms_estimated`, `move_ms_clamped` | Planlanan süre; tahmin mi, kırpıldı mı |

### Hata kilidi

Hata kilitlidir (`state == FAULT`). Geçici sebepte (UV dalgalanması, geciken kesme)
`BTM_ClearFault()` kilidi açar: `HAL_OK` açıldı, `HAL_BUSY` hiçbir şey değişmedi
(tekrarla), `HAL_ERROR`/`HAL_TIMEOUT` sebep sürüyor. **Politika uygulamanındır:**
sınırlı sayıda dene, denemeler arasında bekle, sebebi (`fault`, `status_byte`)
incelemeden körlemesine döngüye sokma. `FAULT_DWT` temizlenemez. Örnek:
`actuator_recover()` ([`examples/baremetal.c`](examples/baremetal.c)).

## 7. Sahip görev kuralı

İlk denemeden sonra komutlar başka yerden (UART, CAN, buton) gelecekse uyulacak kural:

- Komut fonksiyonları (`Move`, `MovePwm`, `Brake`, `Poll`, `ClearFault`) **tek bir
  görevden**, kesmeler açıkken çağrılır. ISR'dan çağrılırsa `HAL_BUSY` döner.
- `BTM_RemainingMs` ve `BTM_GetStatus` her bağlamdan çağrılabilir.
- Komutu alan yerde sürücüyü çağırma: komutu kuyruğa koy, sahip görev uygulasın.
  RTOS örneği: [`examples/freertos_task.c`](examples/freertos_task.c) — görev hareket
  boyunca kuyrukta `BTM_RemainingMs()` kadar uyur, `STOP` gelince `BTM_Brake()` çağırır.
- Süper döngüde (RTOS'suz) ana döngü sahip görevdir: `BTM_RemainingMs()` bitene kadar
  döngüyü döndür, durdurma isteğini orada yokla.

## 8. Entegrasyon kontrol listesi

Kodu yazdıktan ve ilk derlemeden önce hepsini işaretle. Çoğu hata bu listedeki bir
maddeden çıkar.

**Derleme**
- [ ] `BTM9011EP.c` derlemede, klasörü include yolunda; `examples/` derlemede **değil**
- [ ] Çağıran dosyada `#include "BTM9011EP.h"` var (CubeMX'in `USER CODE Includes` bloğunda)
- [ ] `USE_HAL_SPI_REGISTER_CALLBACKS` ve `USE_HAL_TIM_REGISTER_CALLBACKS` = 0

**CubeMX / çevre birimleri**
- [ ] SPI: Master, 8 bit, MSB, CPOL Low, CPHA 2 Edge, NSS Software, ≈328 kHz, kesme açık, DMA yok
- [ ] MISO pull-up; CS pini Output, **başlangıç Low**
- [ ] Timer: Internal clock, CH1 Output Compare No Output (Frozen), auto-reload preload Enable,
      kesme açık, PSC/ARR ile frekans `BTM_PWM_HZ`
- [ ] `stm32xxxx_it.c`'de `SPI_IRQHandler` → `HAL_SPI_IRQHandler(&hspiX)`,
      `TIM_IRQHandler` → `HAL_TIM_IRQHandler(&htimX)`
- [ ] SPI ve timer NVIC önceliği **eşit** ve zamana duyarlı kesmelerin altında

**Callback'ler (§4)**
- [ ] Dört yönlendirme de var
- [ ] SPI callback'lerinde sürücü satırı, varsa `Instance != SPIx` filtresinin **üstünde**
- [ ] `HAL_TIM_PeriodElapsedCallback`'te sürücü satırı timebase `if` bloğunun **dışında**
- [ ] Satırlar `USER CODE` bloklarının içinde

**Başlatma ve çağrı (§5-§7)**
- [ ] `BTM_Config.tim_clk_hz` = timer'ın **besleme** saati (APB çarpanı dahil)
- [ ] `BTM_Init` dönüş değeri kontrol ediliyor
- [ ] `BTM_Init` kesmeler açıkken, tercihen sahip görevden çağrılıyor
- [ ] Sürücü komutlarını **tek** görev çağırıyor; ISR'dan çağrı yok
- [ ] `HAL_BUSY` dönüşü hata sayılmıyor, tekrar denenebilir sayılıyor

**Kartta ilk açılış**
- [ ] Motor BAĞLI DEĞİLKEN `BTM_Init` HAL_OK, `echo_byte == last_cmd` (§9, adım 1)

## 9. Devreye alma sırası

Sıra önemlidir; ilk adımlar motora komut vermeden yapılır.

1. **Hat (motor BAĞLI DEĞİL, BTM9011EP bağlı):** `BTM_Init` `HAL_OK` dönmeli;
   `BTM_Poll` sonrası `fault == NONE` ve `echo_byte == last_cmd` (fren: `0x11`).
   Geçmeden motoru bağlama.
2. **Yön etiketleri:** Motoru bağla, `BTM_Move(BTM_DIR_FORWARD)` ile gerçek yönü gör.
   Varsayılan `0x51` datasheet Tablo 7'de "reverse" desenidir; mekanik yön OUTA/OUTB
   bağlantısına bağlıdır. Ters çıkarsa kodu değil `BTM_CMD_FORWARD` /
   `BTM_CMD_BACKWARD` tanımlarını takas et.
3. **Strok süresi:** Uçtan uca süreyi ölç, **%20 pay** ekleyip `forward_ms` /
   `backward_ms`'e yaz (limit switch'li `-S` varyantında fazlası zararsızdır).
4. **PWM:** `BTM_MovePwm(dir, 60)` ile strok tamamlanıyor mu; `move_ms_clamped` 0 mı;
   aktüatörün hiç kalkmadığı **ölü bölge** duty'sini not et.
5. **Hata kilidi:** Kilidi bilerek sına: hareket sırasında SPI kablosunu çıkar →
   `FAULT_COMM`; `BTM_ClearFault()` `HAL_ERROR` dönmeli; kabloyu tak → `HAL_OK`.
6. **Kapılar:** `BTM_Move(FORWARD)` bitince hemen `BACKWARD` → `HAL_BUSY` (ölü süre /
   dinlenme), motor sürülmemeli.
7. **Osiloskop (varsa):** CS Low süresi ≥ 4 µs; CS yükselişi ile ilk SCK arası
   ≥ 250 ns (sürücüdeki tek doğrulanmamış varsayım, README §8).
8. **Yük:** Sürücü hareket ederken diğer kesmelerin (ADC, kontrol döngüsü) örnekleme
   hızı değişmemeli; `dropped_frames` artıyorsa NVIC öncelikleri (§2) veya uzun ISR
   şüphelidir.

## 10. Sorun giderme

| Belirti | Bak |
|---|---|
| `BTM_Init` → `FAULT_COMM` | VS beslemesi ve ortak GND, CPOL Low/CPHA 2 Edge, MISO pull-up, CS doğru pin mi, **callback yönlendirmesi** (§4) |
| `BTM_Init` → `HAL_ERROR`, `fault == NONE` | Config reddedildi: `tim_clk_hz`, PSC/ARR ile `BTM_PWM_HZ` uyuşmuyor, `tim`/`spi` NULL, strok süresi 0 veya > `BTM_MAX_MOVE_MS` |
| `BTM_Init` → `FAULT_DWT` | Debugger'sız dene; M0/M0+'da DWT yoktur |
| Hiç kesme gelmiyor, sürücü takılıyor | `Register Callback` Disable mı; `HAL_SPI_IRQHandler`/`HAL_TIM_IRQHandler` ISR'larda çağrılıyor mu; SPI ve timer NVIC'te açık mı; callback'ler yönlendirilmiş mi (özellikle filtre üstü, §4) |
| `HAL_OK` ama motor dönmüyor | Motor kablosu kopuk olabilir; sürücü açık yük tespiti yapmaz. Motoru doğrudan besleyerek dene |
| `FAULT_OVERLAP` | SPI/timer kesmesi geciktiriliyor: NVIC önceliği ve uzun ISR'lar. Sebep geçtiyse `BTM_ClearFault()` |
| `FAULT_DEVICE` + UV | Besleme çöküyor: akım kapasitesi, kablo kesiti. Geçiciyse `BTM_ClearFault()` |
| `FAULT_DEVICE` + OCA/OCB | Aşırı akım: aktüatör sıkışmış/kısa devre. Sebep giderilmeden `ClearFault` `HAL_ERROR` döner |
| `BTM_Move` sürekli `HAL_BUSY`, fault yok | Koruma kapısı kapalı: ters yön için 100 ms, ısıl dinlenme için hareketin **fiilen sürdüğü** sürenin 4 katı. `move_ms` o süreyi söyler |
| `BTM_ClearFault` sürekli `HAL_BUSY` | Uçuştaki SPI aktarımı bitmiyor: SPI callback'i/IRQ'su bağlı mı |
| PWM'de strok tamamlanmıyor | `move_ms_clamped` 1 mi (duty'yi yükselt veya `BTM_MAX_MOVE_MS`'i büyüt); değilse doğrusal tahmin yetersiz, `forward_ms`'e pay ekle |
| Durum baytı güncellenmiyor | Boştayken bayt son çerçeveden kalmadır: `BTM_Poll()` çağır |

## 11. Doğrulama durumu

Bu belgedeki kodun neyle doğrulandığı açıkça ayrılmıştır:

| Doğrulama | Durum |
|---|---|
| `BTM9011EP.c` ve `examples/*.c`, gerçek STM32F4 HAL başlıklarına karşı `arm-none-eabi-gcc 13.3.1`, `-mcpu=cortex-m4 -O2 -Wall -Wextra -Werror` ile derleniyor | ✔ uyarısız |
| Kartta: `BTM_Init` HAL_OK, echo == last_cmd | ☐ henüz işaretlenmedi |
| Kartta: ileri/geri hareket, `RemainingMs` bitişi, kendi kendine fren | ☐ |
| Kartta: `BTM_Poll` + `BTM_GetStatus` | ☐ |
| Kartta: hata kilidi ve `ClearFault` | ☐ |
| Kartta: ters yön / dinlenme kapıları (`HAL_BUSY`) | ☐ |
| Osiloskop: `tlead ≥ 250 ns` | ☐ |

Otomatik host testi bu depoda yoktur. Fiziksel zamanlama ve motor davranışı yalnız
kartta doğrulanabilir; kutular sonuç alındıkça işaretlenir (kart, HAL sürümü ve
tarih yanına yazılmalıdır).
