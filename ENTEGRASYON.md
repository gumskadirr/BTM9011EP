# Basit entegrasyon: STM32F407VG (DISC1), bare-metal

Sürücüyü sıfırdan bir projeye bağlamanın en kısa yolu. FreeRTOS yok; ana döngü
"sahip görev" olarak çalışır (sürücü saf HAL olduğu için fark etmez).

**Donanım seçimi ve gerekçesi**

| | Seçim | Neden |
|---|---|---|
| SPI | **SPI2** — PB13 SCK, PB14 MISO, PB15 MOSI | DISC1'de kart üstü LIS3DSH ivmeölçer SPI1'e (PA5/PA6/PA7) ve CS olarak PE3'e bağlı; SPI1 kullanırsan hat çakışır |
| CS | **PB12** (label `BTM_CS`) | SPI2'ye komşu, boşta |
| Timer | **TIM3** | Genel amaçlı; tek IRQ (`TIM3_IRQn`) hem update hem CC'yi taşır, gelişmiş timer'daki iki ayrı IRQ derdi yok |
| Timebase | SysTick | Bare-metal |
| Buton | PA0 (kart üstü User butonu, aktif High) | Test için |
| LED | PD12 yeşil / PD14 kırmızı | Durum göstergesi |

**Saat değerleri** (168 MHz, APB1 = 42 MHz → APB1 timer saati 84 MHz):

- SPI2 prescaler **128** → 42 MHz / 128 = **328 kHz** SCK
- TIM3 **PSC = 419, ARR = 999** → 84 MHz / 420 / 1000 = **200 Hz** = `BTM_PWM_HZ`

---

## Adım 1 — CubeMX projesi

Yeni proje → STM32F407VGTx (veya Board Selector → STM32F4DISCOVERY).

### 1.1 Saat

**Clock Configuration**: HSE 8 MHz, PLL ile **HCLK = 168 MHz**. (Board Selector
kullandıysan hazır gelir.) APB1 prescaler 4, APB2 prescaler 2 — varsayılan.

### 1.2 SPI2

**Connectivity → SPI2 → Mode: Full-Duplex Master**, Hardware NSS Signal: Disable.

**Parameter Settings:**

| Alan | Değer |
|---|---|
| Frame Format | Motorola |
| Data Size | 8 Bits |
| First Bit | MSB First |
| Prescaler | **128** (→ 328.125 KBits/s) |
| Clock Polarity (CPOL) | **Low** |
| Clock Phase (CPHA) | **2 Edge** |
| CRC Calculation | Disabled |
| NSS Signal Type | Software |

**GPIO Settings:** PB13/PB15 (SCK/MOSI) No pull-up and no pull-down;
**PB14 (MISO) → Pull-up**; üçü de Maximum output speed = Very High.

> MISO pull-up şart: cihaz bağlı değilken hattın yüzmesini engeller ve okunan bayt
> `0xFF` olur — sürücü bunu echo kontrolünde yakalar.

**NVIC Settings:** `SPI2 global interrupt` → **Enabled**.

**DMA Settings:** boş bırak.

### 1.3 CS pini

**Pinout** ekranında **PB12**'ye tıkla → `GPIO_Output`.

**System Core → GPIO → PB12:**

| Alan | Değer |
|---|---|
| GPIO output level | **Low** |
| GPIO mode | Output Push Pull |
| GPIO Pull-up/Pull-down | No pull-up and no pull-down |
| Maximum output speed | Very High |
| User Label | **`BTM_CS`** |

> Başlangıç seviyesi **Low** olmalı: CS aktif High'dır, High başlarsa cihaz ilk
> çerçeveden önce seçili kalır.

### 1.4 TIM3

**Timers → TIM3 → Clock Source: Internal Clock**, **Channel1: Output Compare No Output**.

**Parameter Settings:**

| Alan | Değer |
|---|---|
| Prescaler | **419** |
| Counter Mode | Up |
| Counter Period | **999** |
| Internal Clock Division | No Division |
| auto-reload preload | **Enable** |
| Trigger Output (TRGO) Parameters → Trigger Event Selection | Reset |
| Output Compare Channel 1 → Mode | **Frozen** |
| Output Compare Channel 1 → Pulse | 0 (sürücü her harekette yazar) |

**NVIC Settings:** `TIM3 global interrupt` → **Enabled**.

### 1.5 Buton ve LED (test için)

- **PA0** → `GPIO_Input`, No pull (kart üstünde harici pull-down var), label `USER_BTN`
- **PD12** → `GPIO_Output`, Low, label `LED_OK`
- **PD14** → `GPIO_Output`, Low, label `LED_ERR`

### 1.6 NVIC öncelikleri — KRİTİK

**System Core → NVIC:**

| Kesme | Preemption Priority |
|---|---|
| SPI2 global interrupt | **1** |
| TIM3 global interrupt | **1** |
| Time base: System tick timer | 0 |

**SPI2 ve TIM3 aynı preemption önceliğinde olmalı** — sürücünün paylaşılan durum
mantığı birbirlerini kesmemelerine dayanır. İleride FOC gibi bir kontrol döngüsü
eklersen onu bunların **üstüne** (sayısal olarak daha küçük) koyacaksın.

### 1.7 Project Manager

| Ayar | Değer |
|---|---|
| Toolchain / IDE | STM32CubeIDE |
| Code Generator → Generate peripheral initialization as a pair of .c/.h | işaretli (tercih) |
| Code Generator → Keep User Code when re-generating | **işaretli** |
| Advanced Settings → Driver Selector: SPI, TIM, GPIO | **HAL** |
| Advanced Settings → Register Callback: SPI, TIM | **DISABLE** |

> Register Callback `ENABLE` olursa HAL zayıf (`__weak`) callback'leri çağırmaz ve
> sürücü SPI tamamlanmasını hiç görmez.

**GENERATE CODE.**

---

## Adım 2 — Sürücüyü projeye ekle

`BTM9011EP.c` ve `BTM9011EP.h` dosyalarını projeye kopyala. Örnek yerleşim:

```
<proje>/Drivers/BTM9011EP/BTM9011EP.c
<proje>/Drivers/BTM9011EP/BTM9011EP.h
```

CubeIDE'de: proje sağ tık → **Properties → C/C++ General → Paths and Symbols**

- **Source Location** sekmesi → Add Folder → `Drivers/BTM9011EP`
- **Includes** sekmesi → Languages: GNU C → Add → Workspace → `Drivers/BTM9011EP`

Derle. Bu noktada hata olmamalı (henüz çağrılmıyor).

---

## Adım 3 — Kod

Aşağıdaki parçalar `Core/Src/main.c` içindeki USER CODE bloklarına girer.
Blok adları CubeMX'in ürettiği yorum satırlarıdır; **blokların dışına yazma**,
yoksa yeniden kod üretiminde silinir.

### 3.1 `/* USER CODE BEGIN Includes */`

```c
#include "BTM9011EP.h"
#include <stdio.h>
```

### 3.2 `/* USER CODE BEGIN PV */`

```c
/* TAM HIZ strok süreleri: 4. aşamada kartta ÖLÇÜLECEK (düşük duty'de sürücü
 * bunları kendi ölçekler; üst sınır BTM_MAX_MOVE_MS = 60000).
 * Şimdilik güvenli bir üst değer.
 * P8 "-S" varyantında fazla vermek zararsız: strok sonunda limit switch motor
 * devresini açar, stall olmaz. */
static const BTM_Config btm_cfg = {
    .spi         = &hspi2,
    .cs_port     = BTM_CS_GPIO_Port,
    .cs_pin      = BTM_CS_Pin,
    .tim         = &htim3,
    .tim_clk_hz  = 84000000U,      /* TIM3 APB1'de: 84 MHz. Init bununla PSC/ARR'dan
                                    * gerçek frekansı ölçer; BTM_PWM_HZ (200) ile
                                    * uyuşmazsa HAL_ERROR döner. */
    .forward_ms  = 8000U,
    .backward_ms = 8000U,
};

static uint8_t btm_ready;          /* Init başarılı mı */
static uint8_t next_forward = 1U;  /* Sıradaki hareketin yönü */
```

### 3.3 `/* USER CODE BEGIN 2 */` — Init

`MX_GPIO_Init()`, `MX_SPI2_Init()`, `MX_TIM3_Init()` çağrılarından **sonra**,
`while (1)` döngüsünden önce:

```c
if (BTM_Init(&btm_cfg) == HAL_OK)
{
    btm_ready = 1U;
    HAL_GPIO_WritePin(LED_OK_GPIO_Port, LED_OK_Pin, GPIO_PIN_SET);
}
else
{
    BTM_Status st;
    (void)BTM_GetStatus(&st);
    /* st.fault == BTM_FAULT_COMM -> cihaz bağlı değil / SPI modu yanlış / pinler ters
       st.fault == BTM_FAULT_DWT  -> DWT sayacı çalışmıyor (debugger almış olabilir)
       st.fault == BTM_FAULT_NONE -> config reddedildi: tim_clk_hz / Prescaler / Period
                                     uyuşmuyor (200 Hz çıkmıyor) veya strok süresi geçersiz */
    HAL_GPIO_WritePin(LED_ERR_GPIO_Port, LED_ERR_Pin, GPIO_PIN_SET);
}
```

### 3.4 `/* USER CODE BEGIN 4 */` — Callback yönlendirmeleri

Dördü de gerekli. Bu fonksiyonlar projede başka bir yerde tanımlıysa yeni
fonksiyon yazma, sadece `BTM_On...` satırını mevcut gövdenin içine ekle.

```c
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    BTM_OnSpiComplete(hspi);
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    BTM_OnSpiError(hspi);
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    BTM_OnTimerPeriod(htim);
}

void HAL_TIM_OC_DelayElapsedCallback(TIM_HandleTypeDef *htim)
{
    BTM_OnTimerCompare(htim);
}
```

> Sürücü kendi handle'ı dışındaki çağrıları yok sayar, bu yüzden aynı callback'i
> başka çevrebirimler de kullanıyorsa sorun olmaz.

### 3.5 `/* USER CODE BEGIN 4 */` — Buton ve durum görevleri

```c
/** 30 ms filtreli buton. Basıldığı an 1 döner. */
static uint8_t button_pressed(void)
{
    static uint8_t last_raw, stable;
    static uint32_t change_tick;
    uint8_t raw = (HAL_GPIO_ReadPin(USER_BTN_GPIO_Port, USER_BTN_Pin) == GPIO_PIN_SET) ? 1U : 0U;
    uint32_t now = HAL_GetTick();

    if (raw != last_raw)
    {
        last_raw = raw;
        change_tick = now;
    }
    if ((raw != stable) && ((now - change_tick) >= 30U))
    {
        stable = raw;
        if (stable != 0U)
        {
            return 1U;   /* yeni basış */
        }
    }
    return 0U;
}

/** Basış: hareket yoksa başlat (yön dönüşümlü), varsa erken durdur. */
static void btm_task(void)
{
    if (!btm_ready)
    {
        return;
    }

    if (button_pressed())
    {
        if (BTM_RemainingMs() > 0U)
        {
            (void)BTM_Brake();                      /* erken durdurma */
        }
        else
        {
            BTM_Direction dir = next_forward ? BTM_DIR_FORWARD : BTM_DIR_BACKWARD;

            /* Tam hız için BTM_Move(dir); %60 hız için BTM_MovePwm(dir, 60U) */
            HAL_StatusTypeDef res = BTM_Move(dir);

            if (res == HAL_OK)
            {
                next_forward = !next_forward;
            }
            else if (res != HAL_BUSY)
            {
                /* Sürüş komutu cihaza GİTMİŞ olabilir (üç çerçeveli komutun ortasında
                 * hata). Sürücü fren denedi; emniyet payı olarak bir kez daha frenle.
                 * Fault'u aşağıdaki led_task gösterir. */
                (void)BTM_Brake();
            }
            /* HAL_BUSY: hiçbir şey olmadı. Hat bir an meşgul olabilir, ya da aktüatörü
             * koruyan kapılardan biri kapalıdır (README §3.3): ters yöne geçmek için
             * 100 ms, ısıl dinlenme için önceki hareketin 4 katı. Bir sonraki basışta
             * tekrar denenir — hızlı hızlı basmak aktüatörü yakmaz. */
        }
    }
}

/** Yeşil: boşta. Yanıp sönen yeşil: hareket sürüyor. Kırmızı: fault. */
static void led_task(void)
{
    BTM_Status st;

    if (BTM_GetStatus(&st) != HAL_OK)
    {
        return;
    }

    if (st.fault != BTM_FAULT_NONE)
    {
        HAL_GPIO_WritePin(LED_OK_GPIO_Port, LED_OK_Pin, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(LED_ERR_GPIO_Port, LED_ERR_Pin, GPIO_PIN_SET);
    }
    else if (st.state == BTM_STATE_IDLE)
    {
        HAL_GPIO_WritePin(LED_OK_GPIO_Port, LED_OK_Pin, GPIO_PIN_SET);
    }
    else
    {
        /* Hareket sürerken 100 ms'de bir yanıp sön */
        HAL_GPIO_WritePin(LED_OK_GPIO_Port, LED_OK_Pin,
                          ((HAL_GetTick() / 100U) & 1U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
}
```

### 3.6 `/* USER CODE BEGIN 4 */` — UART paket akışı (buton yerine)

Komutlar UART'tan paket olarak geliyorsa akış şöyle: **kesme paketi alır ve bir bayrak
kaldırır, sürücüyü ANA DÖNGÜ çağırır.** Sürücüyü doğrudan RX kesmesinden çağırmak
işe yaramaz — komut fonksiyonları ISR bağlamında hiçbir şey yapmadan `HAL_BUSY` döner
(sözleşme: `BTM9011EP.h`).

Paket biçimi (üç bayt gelir, dört bayt döner):

| Yön | Bayt 0 | Bayt 1 | Bayt 2 | Bayt 3 |
|---|---|---|---|---|
| Gelen | `0x3C` | komut | duty (0..100; 100 = tam hız) | — |
| Dönen | `0x3E` | `HAL_StatusTypeDef` | `status_byte` (cihazdan HAM) | `BTM_Fault` |

```c
/* USER CODE BEGIN PV */
#define PKT_RX_STX   0x3CU
#define PKT_TX_STX   0x3EU
#define PKT_FORWARD  1U
#define PKT_BACKWARD 2U
#define PKT_STOP     3U
#define PKT_STATUS   4U

static uint8_t rx_pkt[3];
static volatile uint8_t pkt_ready;     /* ISR -> ana döngü */
/* USER CODE END PV */
```

```c
/* USER CODE BEGIN 4 */

/** RX kesmesi: SADECE bayrak kaldırır. Sürücü burada ÇAĞRILMAZ. */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart != &huart2) { return; }
    pkt_ready = 1U;
    /* Yeniden silahlandırma ana döngüde: paket işlenmeden üzerine yazılmasın. */
}

/** Ana döngüden çağrılır: paketi uygular ve cevabı gönderir. */
static void uart_task(void)
{
    BTM_Status st;
    HAL_StatusTypeDef res;
    uint8_t tx_pkt[4];

    if (!pkt_ready) { return; }

    if (rx_pkt[0] != PKT_RX_STX)
    {
        res = HAL_ERROR;
    }
    else
    {
        uint8_t duty = rx_pkt[2];

        switch (rx_pkt[1])
        {
        case PKT_FORWARD:
            res = (duty >= 100U) ? BTM_Move(BTM_DIR_FORWARD)
                                 : BTM_MovePwm(BTM_DIR_FORWARD, duty);
            break;
        case PKT_BACKWARD:
            res = (duty >= 100U) ? BTM_Move(BTM_DIR_BACKWARD)
                                 : BTM_MovePwm(BTM_DIR_BACKWARD, duty);
            break;
        case PKT_STOP:
            res = BTM_Brake();
            break;
        case PKT_STATUS:
            res = BTM_Poll();        /* cihazdan TAZE status baytı okur */
            break;
        default:
            res = HAL_ERROR;
            break;
        }

        /* HAL_BUSY dışındaki hatada sürüş komutu cihaza gitmiş olabilir: emniyet freni. */
        if ((res != HAL_OK) && (res != HAL_BUSY)) { (void)BTM_Brake(); }
    }

    (void)BTM_GetStatus(&st);
    tx_pkt[0] = PKT_TX_STX;
    tx_pkt[1] = (uint8_t)res;          /* 0 OK, 1 ERROR, 2 BUSY (tekrar dene), 3 TIMEOUT */
    tx_pkt[2] = st.status_byte;        /* cihazın HAM status baytı */
    tx_pkt[3] = (uint8_t)st.fault;
    (void)HAL_UART_Transmit(&huart2, tx_pkt, sizeof(tx_pkt), 10U);

    pkt_ready = 0U;
    (void)HAL_UART_Receive_IT(&huart2, rx_pkt, sizeof(rx_pkt));
}
/* USER CODE END 4 */
```

Init'ten sonra ilk alımı silahlandır (`USER CODE BEGIN 2`):

```c
(void)HAL_UART_Receive_IT(&huart2, rx_pkt, sizeof(rx_pkt));
```

Dikkat edilecek üç nokta:

- **`HAL_BUSY` hata değildir.** Hareket sürüyordur, ya da aktüatörü koruyan kapılardan
  biri kapalıdır (README §3.3). Karşı taraf aynı paketi sonra tekrar gönderebilir.
  Hareketin ne kadar süreceğini `BTM_RemainingMs()` söyler; istenirse cevap paketine
  eklenir.
- **Durum sorgusu `BTM_Poll` ister.** `BTM_GetStatus` önbellekteki son baytı verir;
  boştayken bu bayt eski olabilir. `BTM_Poll` 3 çerçeve harcayıp günceli getirir,
  köprüye dokunmaz. Hareket sırasında gerekmez (her periyotta okunuyor) ve `HAL_BUSY` döner.
- **UART kesmesinin önceliği serbesttir**, çünkü içinde sürücü kodu çalışmaz. Sürücünün
  SPI ve TIM kesmeleri için §1.6'daki eşit öncelik şartı aynen geçerlidir.

### 3.7 `/* USER CODE BEGIN WHILE */` — Ana döngü

```c
while (1)
{
    btm_task();      /* buton akışı */
    uart_task();     /* paket akışı (§3.6); ikisinden birini kullan */
    led_task();

    /* USER CODE END WHILE */
    /* USER CODE BEGIN 3 */
}
```

Hareketin sonunda yapılacak bir şey yok: süre dolunca sürücü kendi frenler.

**Ama bir emniyet alışkanlığı edin.** Üretim kodunda hareketi bekleyen döngünün
sonuna idempotent bir `BTM_Brake()` koy:

```c
while (BTM_RemainingMs() > 0U) { /* bekle veya komut dinle */ }
(void)BTM_Brake();   /* emniyet payı; sürücü zaten frenledi */
```

Sebebi: TIM callback'leri yanlışlıkla yönlendirilmemişse sürücünün içinde **hiçbir
kod çalışmaz** — ne periyot sayılır ne fren gönderilir — ve motor süresiz sürüşte
kalır. Sürücü bunu kendi başına fark edemez (çalışacak kodu yoktur). Bu tek satır
o hatayı kapatır; maliyeti 3 çerçeve (~90 µs).

---

## Adım 4 — Devreye alma sırası

Sıra önemli. **İlk iki adımda motor bağlı olmayacak.**

### Aşama 1: Hat doğrulaması (motor BAĞLI DEĞİL, BTM9011EP bağlı)

Derle, yükle, çalıştır.

| Gördüğün | Anlamı |
|---|---|
| **Yeşil LED yanar** | `BTM_Init` geçti: SPI modu, bit sırası, pinler ve cihaz doğru. Devam et. |
| **Kırmızı LED yanar** | Debugger'da `st.fault`'a bak. `FAULT_COMM` → cihaz cevap vermiyor: besleme (VS), lehim, CPOL/CPHA, MISO pull-up ve CS pinini kontrol et. `FAULT_DWT` → debugger DWT'yi almış olabilir, debugger'sız dene. |

Bu aşama geçmeden motoru bağlama.

### Aşama 2: Yön etiketlerini belirle

Motoru bağla. Butona bas; aktüatör bir yöne gitmeli.

`BTM_CMD_FORWARD` varsayılanı `0x51`, ama datasheet Tablo 7'de o desen "reverse".
Hangisinin mekanik "ileri" olduğu OUTA/OUTB'nin aktüatöre nasıl lehimlendiğine
bağlı. Ters çıkarsa **kodu değiştirme**, proje tanımlarına ekle:

CubeIDE: Properties → C/C++ Build → Settings → MCU GCC Compiler → Preprocessor →
Defined symbols:

```
BTM_CMD_FORWARD=0x31U
BTM_CMD_BACKWARD=0x51U
```

### Aşama 3: Strok süresini ölç

Aktüatörü bir uçtan diğerine götüren süreyi kronometreyle ölç (veya butona basıp
hareketin durduğu ana kadar say). Ölçülen süreye **%20 pay** ekleyip
`forward_ms` / `backward_ms` değerlerini güncelle.

`-S` varyantında fazla vermek zararsızdır: limit switch devreyi açar.

### Aşama 4: PWM'i dene

`btm_task()` içindeki `BTM_Move(dir)` satırını değiştir:

```c
if (BTM_MovePwm(dir, 60U) == HAL_OK)
```

Aktüatör belirgin biçimde yavaş gitmeli. 200 Hz hafif bir ses çıkarabilir.

**Süre otomatik ölçeklenir.** `forward_ms` tam hız süresidir; sürücü `%60` duty'de
hareketi `forward_ms × 100 / 60` kadar sürdürür, yani strok tamamlanır. Elle süre
büyütmek gerekmez (v2'de gerekiyordu).

Kontrol et — bu ölçekleme doğrusal bir **tahmindir**:

```c
BTM_Status st;
(void)BTM_GetStatus(&st);
/* st.move_ms           -> bu hareket için planlanan süre (ör. 8000*100/60 = 13334)
 * st.move_ms_estimated -> 1: duty < 100, süre tahmin edildi
 * st.move_ms_clamped   -> 1: BTM_MAX_MOVE_MS'e kırpıldı, strok tamamlanmayabilir */
```

- Strok yine tamamlanmıyorsa model aktüatörün yükü/sürtünmesi için yetersiz:
  `forward_ms`'e pay ekle (`-S` varyantında fazlası zararsız).
- Çok düşük duty'de (ör. %10) aktüatör hiç kalkmayabilir — **ölü bölge**. Hangi
  duty'nin altında kalkmadığını not et ve uygulamada o değerin altına inme.
- `move_ms_clamped = 1` görüyorsan ya duty'yi yükselt ya `BTM_MAX_MOVE_MS`'i büyüt.

### Aşama 4b: Hata kilidini ve çıkışını dene

Kilidin açılabildiğini **bilerek** sınamak, sahada ilk arızada öğrenmekten iyidir:

1. Hareket sürerken SPI veya TIM kesmesini geciktir (ör. uzun bir `HAL_Delay`'i
   yüksek öncelikli bir ISR'a koy) → `FAULT_OVERLAP` + kırmızı LED beklenir.
2. `BTM_ClearFault()` çağır → `HAL_OK` dönmeli, LED yeşile dönmeli, sonraki butona
   basışta aktüatör yine hareket etmeli.
3. Cihazın SPI kablosunu çıkar → komut `FAULT_COMM` vermeli ve `BTM_ClearFault()`
   bu kez `HAL_ERROR` dönmeli (sebep sürüyor). Kabloyu tak, tekrar çağır → `HAL_OK`.

Kodda bağlamanın en kolay yolu butona **uzun basış** vermektir:

```c
/* btm_task() içinde: fault varken basış = kilidi açmayı dene */
BTM_Status st;
(void)BTM_GetStatus(&st);
if (st.fault != BTM_FAULT_NONE) {
    if (button_pressed()) { (void)BTM_ClearFault(); }   /* HAL_BUSY ise tekrar bas */
    return;
}
```

### Aşama 5: Osiloskopla doğrula (varsa)

- CS ve SCK'yi gözle: CS'in Low kalma süresi ≥ 4 µs olmalı.
- **CS yükselişi ile ilk SCK arasındaki süre ≥ 250 ns olmalı.** Sürücüdeki tek
  doğrulanmamış varsayım budur (`send_it` ISR'da açık gecikme koymaz, HAL kurulum
  yükünün bunu aştığını varsayar). Altında çıkarsa haber ver.
- Bir çerçevenin toplam süresini ölç → `BTM_FRAME_US` değerini güncelle.

---

## Sorun giderme

| Belirti | Bak |
|---|---|
| `BTM_Init` → `FAULT_COMM` | VS beslemesi, GND ortak mı, CPOL/CPHA (Low + 2 Edge), MISO pull-up, CS pini doğru pin mi |
| `BTM_Init` → `FAULT_DWT` | Debugger'sız çalıştır; Cortex-M7'de sürücü kilidi kendi açar, M4'te gerek yok |
| Komut `HAL_OK` ama motor dönmüyor | Motor kablosu kopuk olabilir — sürücü bunu göremez (açık yük tespiti yok). Motoru doğrudan 12 V ile dene |
| `FAULT_OVERLAP` | SPI veya TIM kesmesi geciktiriliyor: NVIC önceliklerini ve projedeki uzun ISR'ları kontrol et. Sebep geçtiyse `BTM_ClearFault()` ile kilit açılır |
| `FAULT_DEVICE`, `status_byte`'ta UV | Besleme çöküyor: akım kapasitesi ve kablo kesiti. Geçici dalgalanmaysa `BTM_ClearFault()` yeter |
| `FAULT_DEVICE`, OCA/OCB | Aşırı akım: aktüatör sıkışmış veya kısa devre. Sebep giderilmeden `BTM_ClearFault()` `HAL_ERROR` döner |
| `BTM_ClearFault` sürekli `HAL_BUSY` | Uçuştaki SPI aktarımı bitmiyor: `HAL_SPI_IRQHandler` çağrılıyor mu, `HAL_SPI_TxRxCpltCallback` yönlendirildi mi |
| `BTM_ClearFault` sürekli `HAL_ERROR` | Sebep kalıcı. `BTM_GetStatus` ile `fault` + `status_byte`'a bak; `FAULT_DWT` ise temizlenemez, reset gerekir |
| PWM'de strok tamamlanmıyor | `st.move_ms_clamped` 1 mi (duty'yi yükselt veya `BTM_MAX_MOVE_MS`'i büyüt); değilse doğrusal tahmin yetersiz, `forward_ms`'e pay ekle |
| `BTM_Init` → `HAL_ERROR`, fault yok | Config reddedildi: `tim` NULL, `tim_clk_hz` 0 veya yanlış, `Prescaler`/`Period` ile 200 Hz çıkmıyor, strok süresi 0 veya `BTM_MAX_MOVE_MS`'ten büyük |
| `BTM_Move` sürekli `HAL_BUSY`, fault yok | Aktüatörü koruyan kapı kapalı (README §3.3): ters yön için 100 ms, ısıl dinlenme için önceki hareketin **fiilen sürdüğü** sürenin 4 katı beklenir. `BTM_Status.move_ms` o süreyi söyler |
| Durum baytı güncellenmiyor (hareket yokken) | Boştayken bayt son çerçeveden kalmadır: `BTM_Poll()` çağır |
| Hiç kesme gelmiyor, sürücü takılıyor | `Register Callback` DISABLE mı; `stm32f4xx_it.c` içinde `HAL_SPI_IRQHandler` / `HAL_TIM_IRQHandler` çağrıları var mı |

---

## Kod üretimi sonrası kontrol listesi

- [ ] `Core/Inc/main.h`: `BTM_CS_Pin`, `BTM_CS_GPIO_Port` tanımlı
- [ ] `Core/Src/main.c`: `MX_SPI2_Init()` ve `MX_TIM3_Init()` çağrılıyor;
      `MX_GPIO_Init` içinde CS için `HAL_GPIO_WritePin(..., GPIO_PIN_RESET)` var
- [ ] `hspi2.Init.CLKPhase = SPI_PHASE_2EDGE`, `CLKPolarity = SPI_POLARITY_LOW`,
      `NSS = SPI_NSS_SOFT`, `BaudRatePrescaler = ..._128`
- [ ] `htim3.Init.Prescaler = 419`, `Period = 999` (84 MHz / 420 / 1000 = 200 Hz)
- [ ] `btm_cfg.tim_clk_hz` timer'ın besleme saatine eşit (TIM3 → 84000000U)
- [ ] `Core/Src/stm32f4xx_it.c`: `SPI2_IRQHandler` → `HAL_SPI_IRQHandler(&hspi2)`,
      `TIM3_IRQHandler` → `HAL_TIM_IRQHandler(&htim3)`
- [ ] `stm32f4xx_hal_msp.c`: SPI2 ve TIM3 IRQ'ları için `HAL_NVIC_SetPriority`
      **aynı** preemption değeriyle ve `HAL_NVIC_EnableIRQ`
- [ ] `Core/Inc/stm32f4xx_hal_conf.h`: `HAL_SPI_MODULE_ENABLED`,
      `HAL_TIM_MODULE_ENABLED`, `HAL_GPIO_MODULE_ENABLED` açık;
      `USE_HAL_SPI_REGISTER_CALLBACKS` ve `USE_HAL_TIM_REGISTER_CALLBACKS` = 0
