# BTM9011EP v2 — CubeMX (.ioc) kurulumu ve gereklilikler

Bu belge sürücünün ihtiyaç duyduğu çevrebirimlerin STM32CubeMX / CubeIDE `.ioc`
dosyasında nasıl ayarlanacağını anlatır. Kurallar projeden bağımsızdır; somut
değerler için 6. bölümdeki örnek projelere bakılabilir. Kod tarafındaki adımlar
[README.md](README.md) içindedir.

## 1. Gereklilik özeti

| Kaynak | Durum | Ayar | Not |
|---|---|---|---|
| SPI | **Zorunlu** | Full-Duplex Master, 8 bit, MSB, Mode 1, NSS Software | Yalnız sürücüye ait; DMA kapalı |
| SPI global interrupt | **Zorunlu** | Enabled | Teşhis, PWM ve hata freni kesmeli aktarım kullanır |
| CS GPIO | **Zorunlu** | GPIO_Output, Push-Pull, başlangıç **Low** | CS **aktif High** |
| Timer | PWM / latch için **zorunlu** | Internal clock, **CH1** Output Compare No Output, 1 kHz | Yalnız sürücüye ait; fiziksel pin yok |
| Timer kesmeleri | Timer varsa zorunlu | Update + CC1; SPI ile **aynı preemption** | Gelişmiş timer'da iki ayrı IRQ |
| HAL timebase | **Zorunlu** | `HAL_GetTick` 1 ms | Bare-metal: SysTick; FreeRTOS: ayrı bir TIMx |
| HAL modülleri | **Zorunlu** | SPI, TIM, GPIO **HAL** (LL değil) | Timer kullanılmasa da TIM HAL derlenmeli |
| Register Callback | **Zorunlu** | SPI ve TIM için **DISABLE** | Bkz. 3.6 |
| DWT çevrim sayacı | Otomatik | — | Init açar; CS gecikmeleri için. Sayaç sıfırlanmaz |

## 2. Saat hesapları

Sürücü saatten bağımsızdır (`SystemCoreClock` ve `HAL_GetTick` kullanır); yalnız
SPI hızı ve timer frekansı doğru ayarlanmalıdır.

**Hangi bus?** Çevrebirimin bağlı olduğu bus Reference Manual'da yazar. STM32F4 için:

| Bus | SPI | Timer |
|---|---|---|
| APB1 | SPI2, SPI3 | TIM2–TIM7, TIM12–TIM14 |
| APB2 | SPI1, SPI4 | TIM1, TIM8, TIM9–TIM11 |

APB prescaler'ı 1 değilse **timer saati APB saatinin 2 katıdır**. CubeMX Clock
Configuration ekranında "APBx timer clocks" değeri doğrudan görünür.

**SPI hızı:** `SCK = APB_saati / prescaler`. Önerilen hedef ≈ 328 kHz
(APB 42 MHz → 128, APB 84 MHz → 256). Daha yüksek SCK, PWM'deki `OVERLAP` payını
artırır; prescaler'ı düşürmeden önce BTM9011EP veri sayfasındaki azami SCK'yi ve
hat uzunluğunu kontrol et. Daha düşük SCK çerçeveyi uzatır ve %10 duty'de (1 kHz'de
100 µs) sığmayabilir.

**Timer frekansı:** `f = timer_saati / ((PSC + 1) × (ARR + 1))`. 1 kHz için
`ARR = 999`, `PSC = timer_saati / 1 MHz − 1`:

| Timer saati | PSC | ARR |
|---|---|---|
| 84 MHz (ör. F4 APB1 timer) | 83 | 999 |
| 168 MHz (ör. F4 APB2 timer) | 167 | 999 |

PWM periyodu yaklaşık 0.6 ms'den kısa olmamalıdır (≈1.6 kHz üstü önerilmez);
1 kHz önerilen değerdir. PWM/latch süreleri `HAL_GetTick` ile ölçülür.

## 3. Adım adım CubeMX

### 3.1 CS pini

**Pinout** ekranında boş bir pine tıkla → `GPIO_Output`. **System Core → GPIO**:

| Alan | Değer |
|---|---|
| GPIO output level | **Low** |
| Mode | Output Push Pull |
| Pull | No pull |
| User Label | Ör. `BTM_CS` → `main.h` içinde `BTM_CS_Pin` / `BTM_CS_GPIO_Port` |

Başlangıç seviyesi Low olmalıdır; High başlarsa cihaz ilk çerçeveden önce seçili
kalır. Label projedeki başka bir pinle (ör. sensör CS'i) aynı olmamalıdır.

### 3.2 SPI

**Connectivity → SPIx → Mode:** `Full-Duplex Master`, Hardware NSS Signal: `Disable`.

| Parametre | Değer |
|---|---|
| Frame Format | Motorola |
| Data Size | 8 Bits |
| First Bit | MSB First |
| Prescaler | 2. bölüme göre (≈328 kHz) |
| Clock Polarity (CPOL) | Low |
| Clock Phase (CPHA) | **2 Edge** (SPI Mode 1) |
| CRC Calculation | Disabled |
| NSS Signal Type | Software |

- **GPIO Settings:** SCK/MOSI No pull, MISO **Pull-up**; hız Very High. Pull-up,
  cihaz bağlı değilken MISO'nun yüzmesini önler.
- **NVIC Settings:** `SPIx global interrupt` → **Enabled**.
- **DMA Settings:** boş bırak. Hata kapanışında DMA biti açık bulunursa sürücü SPI'ı
  sıfırlamayı reddeder (`fault_stop = FAILED`).

### 3.3 Timer (PWM / latch)

**Timers → TIMy → Mode:** Clock Source `Internal Clock`, Channel1 `Output Compare No Output`.

| Parametre | Değer |
|---|---|
| Prescaler / Counter Period | 2. bölüme göre (1 kHz) |
| Counter Mode | Up |
| auto-reload preload | Enable |
| Trigger Output (TRGO) | Reset |
| CH1 Mode | **Frozen (used for Timing base)** |
| CH1 Pulse | Herhangi (sürücü her periyotta yazar) |

**NVIC Settings:**

- Genel amaçlı timer (TIM2–TIM5 vb.): `TIMy global interrupt` → Enabled.
- Gelişmiş timer (TIM1/TIM8): **iki kesme** de açılır —
  `TIMy update interrupt (and TIMx global interrupt)` ve `TIMy capture compare interrupt`.
  Yalnız biri açılırsa PWM periyodu veya fren anı hiç işlenmez.

Timer'ı uygulama başlatmaz; `PwmStart` / `LatchLock` başlatır, bitince sürücü durdurur.

### 3.4 HAL timebase

- Bare-metal: **System Core → SYS → Timebase Source = SysTick**.
- FreeRTOS: SysTick FreeRTOS'a aittir; Timebase Source olarak kullanılmayan bir
  basic timer (ör. TIM6/TIM7) seç. Bu durumda `HAL_TIM_PeriodElapsedCallback`
  CubeMX tarafından `main.c` içinde üretilir; sürücü satırı onun USER CODE bloğuna yazılır.

### 3.5 NVIC

**Zorunlu kural:** SPI ve timer kesmeleri (gelişmiş timer'da update ve CC) **aynı
preemption önceliğinde** olmalıdır; birbirlerini kesmemelidir. Sürücünün paylaşılan
durum mantığı buna dayanır.

**Bare-metal:** Priority Group 0 (`NVIC_PRIORITYGROUP_0`) kullanılıyorsa tüm
kesmeler preemption 0'dır ve kural kendiliğinden sağlanır.

**FreeRTOS (Priority Group 4):**

- FreeRTOS API'si çağıran kesmeler sayısal olarak
  `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` (CubeMX varsayılanı 5) ve üstünde olmalıdır.
  NVIC tablosunda **"Uses FreeRTOS functions"** işaretliyken CubeMX bu sınırın altına
  inmeye izin vermez.
- Sürücünün kesmeleri FreeRTOS API'si çağırmaz. İki seçenek:
  1. Sürücü kesmelerini 5'te bırak, projenin diğer sık/uzun kesmelerini 6, 7…'ye al.
  2. Sürücü kesmelerinde "Uses FreeRTOS functions" işaretini kaldırıp 4 veya daha
     yükseğe al.
- Sürücü kesmeleriyle aynı veya daha yüksek öncelikteki sık kesmeler (ör. 20 kHz ADC)
  PWM çerçevesini geciktirip `BTM9011EP_FAULT_OVERLAP` üretebilir. Sürücü kesmesi de
  daha düşük öncelikli kesmeleri bir çerçeve boyunca (≈30 µs) geciktirebilir; hızlı
  örnekleme yapan kesmelerin bunu tolere ettiği kartta doğrulanmalıdır.
- HAL timebase kesmesi uzun süre bloklanmamalıdır; zaman aşımları `HAL_GetTick` ile ölçülür.

### 3.6 Project Manager

| Ayar | Değer |
|---|---|
| Code Generator → Keep User Code when re-generating | **İşaretli** (sürücü çağrıları USER CODE bloklarında) |
| Advanced Settings → Driver Selector: SPI, TIM, GPIO | **HAL** |
| Advanced Settings → Register Callback: SPI, TIM | **DISABLE** |

**Register Callback notu:** `ENABLE` yapılırsa (`USE_HAL_SPI_REGISTER_CALLBACKS = 1`)
HAL zayıf (`__weak`) `HAL_SPI_TxRxCpltCallback` vb. fonksiyonları çağırmaz. O durumda
yönlendirmeler `HAL_SPI_RegisterCallback` / `HAL_TIM_RegisterCallback` ile
kaydedilmelidir; aksi halde sürücü SPI tamamlanmasını görmez ve
`BTM9011EP_FAULT_TIMEOUT` hatasına düşer.

## 4. Kod üretimi sonrası kontrol listesi

`SPIx`, `TIMy` ve CS label'ını kendi projenle değiştir:

- [ ] `Core/Inc/main.h`: `<LABEL>_Pin`, `<LABEL>_GPIO_Port` tanımlı.
- [ ] `Core/Src/main.c`: `MX_SPIx_Init()` ve `MX_TIMy_Init()` üretilmiş; `MX_GPIO_Init`
      içinde CS için `HAL_GPIO_WritePin(..., GPIO_PIN_RESET)` var.
- [ ] `hspiX.Init.CLKPhase = SPI_PHASE_2EDGE`, `CLKPolarity = SPI_POLARITY_LOW`,
      `NSS = SPI_NSS_SOFT`; prescaler beklenen değerde.
- [ ] `htimY.Init.Prescaler` / `Period` 1 kHz veriyor (2. bölüm).
- [ ] `Core/Src/stm32f4xx_it.c`: `SPIx_IRQHandler` → `HAL_SPI_IRQHandler(&hspiX)`;
      timer IRQ handler'ları → `HAL_TIM_IRQHandler(&htimY)` (gelişmiş timer'da iki handler).
- [ ] `Core/Src/stm32f4xx_hal_msp.c`: SPI ve timer IRQ'ları için `HAL_NVIC_SetPriority`
      **aynı preemption** değeriyle ve `HAL_NVIC_EnableIRQ` var.
- [ ] `Core/Inc/stm32f4xx_hal_conf.h`: `HAL_SPI_MODULE_ENABLED`, `HAL_TIM_MODULE_ENABLED`,
      `HAL_GPIO_MODULE_ENABLED` açık; `USE_HAL_SPI/TIM_REGISTER_CALLBACKS` 0.

Ardından README'deki kod adımlarını uygula: Init, dört callback yönlendirmesi,
sahip bağlamdan `BTM9011EP_Service()`.

## 5. Büyük projeye taşırken

| Konu | Serbest | Koşul |
|---|---|---|
| SPI birimi | Herhangi bir SPI | Mode 1, 8 bit, MSB, NSS Software, Full-Duplex Master, IT açık, DMA kapalı |
| SPI paylaşımı | **Hayır** | Aynı SPI hattına başka cihaz bağlanmamalı; sürücü hata durumunda SPI'ı kapatıp yeniden başlatır |
| CS pini | Herhangi bir GPIO | Push-Pull çıkış, başlangıç Low |
| Timer birimi | Genel amaçlı veya gelişmiş | **CH1** kullanılmalı (sürücüde sabit); Output Compare No Output, update ve CC1 kesmesi |
| Timer paylaşımı | **Hayır** | Sürücü timer'ı başlatıp durdurur, sayacı ve CCR1'i yazar |
| HAL timebase | SysTick veya TIMx | `HAL_GetTick` 1 ms çalışmalı |
| MCU ailesi | Diğer STM32'ler | `-DBTM9011EP_HAL_HEADER="\"stm32xxxx_hal.h\""`; çekirdekte DWT bulunmalı (Cortex-M3/M4/M7; M0/M0+'da yok) |
| Çağrı bağlamı | Tek sahip | Komutlar ve Service aynı ana döngü/görevden, kesmeler açıkken |

Cortex-M7 (ör. STM32F7/H7) için DWT erişiminden önce `DWT->LAR` kilidinin açılması
gerekebilir; sürücü bunu yapmaz.

## 6. Örnek projeler

Aynı sürücünün iki farklı projede kullanılan değerleri (STM32F407VG, 168 MHz,
APB1 42 MHz / timer 84 MHz, APB2 84 MHz / timer 168 MHz):

| Ayar | Driver_v4 (bare-metal) | GIMBAL_v1 (FreeRTOS) |
|---|---|---|
| SPI | SPI1 (APB2), prescaler 256 → 328 kHz | SPI2 (APB1), prescaler 128 → 328 kHz |
| SCK / MISO / MOSI | PA5 / PB4 / PA7 | PB13 / PB14 / PB15 |
| CS | PE3, label `CS` | PB12, label `CS_2` |
| Timer | TIM2 (APB1), PSC 83, ARR 999 | TIM1 (APB2, gelişmiş), PSC 167, ARR 999 |
| Timer IRQ | `TIM2_IRQn` | `TIM1_UP_TIM10_IRQn` + `TIM1_CC_IRQn` |
| HAL timebase | SysTick | TIM6 |
| NVIC | Group 0; SPI1 (0,0), TIM2 (0,1) | Group 4; SPI2, TIM1 UP/CC, TIM6 = 5; ADC/sensör = 6; UART = 7 |
| Service çağrısı | Ana döngü | MainTask (`app_user_run_once`) |

## 7. Kart notları

- **STM32F407G-DISC1:** Kart üzerindeki LIS3DSH ivmeölçer SPI1'e (PA5/PA6/PA7) ve
  CS olarak PE3'e bağlıdır; ivmeölçerin CS'i aktif Low'dur. BTM9011EP'yi aynı
  SPI/CS'e bağlamak hat çakışması yaratır. Sensör kullanılıyorsa sürücüye ayrı bir
  SPI ve CS ver (GIMBAL_v1 örneği). Kartın kullanıcı kılavuzundan (UM1472) doğrula.
- **MISO pull-up:** Cihaz bağlı değilken okunan bayt `0xFF` olur; teşhis sonucu
  `DETECTED` çıkar. Teşhis sonucu tek başına cihaz bağlantısının kanıtı değildir.
