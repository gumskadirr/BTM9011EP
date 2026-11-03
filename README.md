# BTM9011EP_Sade_v2

STM32 HAL için tek BTM9011EP sürücüsü. Hedef: **Actuonix P8 `-S`** (2 kablolu,
strok sonu limit switch'li) lineer aktüatörü iki uç nokta arasında sürmek.
Tam hız veya PWM; ikisi de süreli ve kendi kendine frenler.

FOC gibi mikrosaniye hassasiyetli bir kontrol döngüsüyle **aynı MCU'da** çalışmak
üzere yazılmıştır: hiçbir yerde kesme maskelemez, ISR'larında meşgul bekleme yoktur.

| Dosya | |
|---|---|
| `BTM9011EP.h` / `BTM9011EP.c` | Sürücü (saf HAL, FreeRTOS başlığı include etmez) |
| `tests/` | Host testi; gerçek `.c`'yi taklit HAL ile derler, donanım gerekmez |

Tasarım gerekçeleri:
[`docs/superpowers/specs/2026-11-03-sade-surucu-gelistirme-tasarim.md`](../../docs/superpowers/specs/2026-11-03-sade-surucu-gelistirme-tasarim.md)

---

## 1. Tasarımı belirleyen üç donanım gerçeği

**1. Cihaz kendiliğinden durmaz.** BTM9011EP bir komutu **bir sonraki SPI
çerçevesine kadar** tutar. Frene geçmez, kapanmaz.

**2. Hareketin bittiği cihazdan okunamaz.** Açık yük (OL) biti yalnız üç teşhis
deseninde geçerlidir (datasheet Tablo 12: 0xC1/0xA9/0x89 — hepsinde PWM biti 0,
yani motor sürülmüyor). Sürüş ve fren desenlerinde OL anlamsızdır, bu yüzden hata
maskesinde **yer almaz** ve hareket **süre** ile bitirilir.

**3. Cevap bir çerçeve gecikmelidir** (datasheet Şekil 27):

```
CS  ───┐                      ┌──────────┐                      ┌───
       └──────────────────────┘          └──────────────────────┘
       ▲                      ▲
  CS yükselirken:        CS düşerken:
  cevap çıkış            gönderilen komut
  kaydedicisine          uygulanır
  yüklenir ◄── hangi bayt? BİR ÖNCEKİ çerçevenin SDO_SEL biti seçer
```

Aktüatörün `-S` olması işi rahatlatır: strok sonuna 0.5 mm kala limit switch motor
devresini **açar** (dahili diyotlar ters dönüşe izin verir). Süreyi fazla vermek
stall üretmez, fazlası boşa geçer.

**PWM fiziksel bir pin değildir:** BTM9011EP'nin SPI varyantında PWM girişi yoktur
(pinler: OUTA, SDI, GND, SCLK, OUTB, SDO, CS, VS, IS). PWM, timer'ın periyot
başında sürüş / compare anında fren komutu göndermesiyle SPI üzerinden üretilir.

---

## 2. Gerçek zaman sözleşmesi

**Sürücü hiçbir yerde kesme maskelemez.** Doğrulandı (Cortex-M4 `-O2`, `objdump`):
`cpsid=0`, `cpsie=0`, `msr basepri=0`.

Bu önemli, çünkü NVIC önceliği PRIMASK'e karşı koruma sağlamaz: kontrol döngün ne
kadar yüksek öncelikli olursa olsun `__disable_irq()` onu da bloke ederdi.
Paylaşılan durum bunun yerine şununla korunur: hizalı atomik erişim + yazma
sıralaması (`state` her zaman ISR'ın eylem kapısıdır).

**ISR'larda meşgul bekleme yoktur.** ISR çağrı grafiği `{OnTimerPeriod,
OnTimerCompare, OnSpiComplete, OnSpiError, send_it, eval_status, record_fault}`
ile sınırlıdır; bekleyen kod (`delay_us`, `xfer`) bu kümeden **erişilemez**.
CS aralığı (`tCSL`) ISR'da beklenmez, *sorulur*: yetmiyorsa çerçeve düşer ve sayılır.

Bekleyen kodun tamamı görev bağlamındadır:

| Nerede | En çok |
|---|---|
| `BTM_Init`, standby'dan uyanma (`tWAKEUP`) | 20 µs |
| Çerçeve başına, uçuştaki aktarımın bitmesi | `BTM_INFLIGHT_WAIT_MS` (2 ms) |
| Doğrulanmış komut (3 çerçeve) | 3 × yukarıdaki |

Bir çerçeve ~30 µs sürdüğü için bunlar yalnız emniyet payıdır, normalde dolmaz.
Kesme maskelemezler ama sahip görevi meşgul bekletirler.

### ZORUNLU öncelik dizilimi

```
   kontrol döngüsü (FOC vb.)    ◄── en yüksek
   sürücü SPI + TIM             ◄── birbirine EŞİT, kontrol döngüsünün ALTINDA
   diğer kesmeler
```

SPI ve TIM kesmeleri **birbirini kesmemelidir** (paylaşılan durum mantığı buna
dayanır), ikisi de kontrol döngüsünün **altında** kalmalıdır.

---

## 3. Tek sürüş yolu

Tam hız da PWM de **aynı yoldan** yürür: süreyi timer sayar.

| | Tam hız (`BTM_Move`) | PWM (`BTM_MovePwm`) |
|---|---|---|
| duty | 100 | 1..99 |
| Periyot başında | sürüş çerçevesi | sürüş çerçevesi |
| Compare anında | **çerçeve yok** | fren çerçevesi |
| Çerçeve / periyot | 1 | 2 |
| Süre dolunca | sürücü kendi frenler | sürücü kendi frenler |

Bunun iki sonucu var:

- **Uygulamanın sonda bir şey yapması gerekmez.** `BTM_Brake()` yalnız *erken*
  durdurma içindir.
- **Her iki kipte de her periyotta bir status baytı okunur**, çünkü sürüş
  çerçevesi her periyot tekrar gider. Cihaz hataları hareketin ortasında görülür.

### Durum diyagramı

```
              ┌──────────────┐
    ┌────────►│     IDLE     │◄─────────┐
    │         │ son komut:   │          │
    │         │    fren      │          │
    │         └──────┬───────┘          │
    │                │                  │
    │     BTM_Move / BTM_MovePwm        │
    │                ▼                  │
    │         ┌──────────────┐          │
    │         │    MOVING    │          │
    │         │ timer sayıyor│          │
    │         └───┬──────┬───┘          │
    │             │      │              │
    │  süre dolar │      │ BTM_Brake    │
    │             ▼      │ (erken)      │
    │      ┌─────────────┴──┐           │
    │      │   FINISHING    │           │
    │      │ son fren gidiyor│──────────┘
    │      └────────────────┘  SPI cplt
    │
    └── Herhangi bir hatada ──► FAULT (reset gerekir)
```

### Hata durumu

```
  echo ≠ gönderilen, 0xFF, veya bit7 beklenmedik  ──► FAULT_COMM
  status baytında OCA|OCB|TSDA|TSDB (anında)      ──► FAULT_DEVICE
  status baytında UV, BTM_UV_CONFIRM kez üst üste ──► FAULT_DEVICE
  HAL_SPI_ErrorCallback                           ──► FAULT_SPI
  BTM_DROP_LIMIT kadar ardışık düşen çerçeve      ──► FAULT_OVERLAP
  Init'te DWT saymıyor                            ──► FAULT_DWT

  Hatada: timer durur, durum FAULT olur, bütün komutlar HAL_ERROR döner.
  Kilit MCU RESET'ine kadar kalır.
```

`HAL_BUSY` hata değildir: hat bir an meşgul demektir, çağrı tekrarlanabilir.

**UV neden teyit ister:** fırça DC motorun kalkış akımı ve 12 V'ta 450 mA stall
akımı, hareket başında UV'nin geçici set olmasını gerçekçi kılıyor.

---

## 4. Üç çerçeveli doğrulanmış komut

Cevap bir çerçeve gecikmeli olduğu (1. bölüm, 3. gerçek) için gönderilen baytın
echo'su ancak bir sonraki çerçevede görülür:

| Çerçeve | Gönderilen | Okunan | Ne işe yarar |
|---|---|---|---|
| 1 | komut, `SDO_SEL=0` | önceki çerçeveninki | atılır |
| 2 | aynı komut, `SDO_SEL=1` | **control byte echo'su** | paket doğrulama |
| 3 | aynı komut, `SDO_SEL=1` | **status byte** | saklanır |

Üçü de aynı komutu uygular (`SDO_SEL` köprü çıkışlarını etkilemez), ~90 µs.

**Doğrulama kuralı çerçeve tipine göre ayrıdır:** echo çerçevesinde bit 7 **0**
olmalı ve bit 6..0 gönderilenle eşleşmeli; status çerçevesinde bit 7 **1** olmalı.
Tek bir "bit7 set ise status" kuralı yanlıştır: cihaz cevap vermiyorken MISO
pull-up `0xFF` verir, bit 7'si 1'dir ve o testi geçerdi. Echo çerçevesinde ise
beklenen 0'a uymadığı için yakalanır — `BTM_Init` hattı bu sayede motora hiç komut
vermeden doğrular.

Çerçeve akışı sırasında (MOVING/FINISHING) `SDO_SEL` sabit 1'dir, yani her cevap
status baytıdır.

---

## 5. Çerçeve zamanlaması ve CPU

```
        ┌─ tCSL ─┐┌tlead┐┌──── 8 bit @ SCK ────┐┌tlag┐
        │  4 µs  ││250ns││      ~24 µs         ││125n│
   CS ──┘        └──────┐                      ┌────┘
                        └─ ████████████████████ ┘
```

Datasheet Tablo 15: `tCSL` ≥ 4 µs, `tlead` ≥ 250 ns, `tlag` ≥ 125 ns,
`tWAKEUP` = 20 µs.

| Tasarruf | Nasıl |
|---|---|
| `tWAKEUP` (20 µs) | **Yalnız standby'dan ilk çerçevede.** Datasheet 8.5: *"twakeup is not needed when the device is in normal operation mode."* Sonrasında `tlead` = 250 ns yeter |
| `tCSL` (4 µs) | ISR'da beklenmez, DWT ile **sorulur**; periyotlar ms mertebesinde olduğu için zaten sağlanır |
| Kesme sayısı | `BTM_PWM_HZ` varsayılanı **200 Hz** → saniyede 200 (tam hız) veya 400 (PWM) çerçeve |

Kaba hesap: çerçeve başına ~8–10 µs CPU → **%0.2–0.4** mertebesi. Kartta ölçülmeli.

Düşük frekansın ikinci faydası duty alt sınırıdır: 1 kHz'de bir çerçeve 100 µs'lik
pencereye zor sığdığı için alt sınır ~%10'du, 200 Hz'de (5 ms periyot) ~%1'e iner.
Bedeli duyulabilir bantta hafif ıslık (aktüatör zaten 50 dB @ 45 cm).

---

## 6. Kurulum

### 6.1 CubeMX

| Kaynak | Ayar |
|---|---|
| SPI | Full-Duplex Master, 8 bit, MSB First, **CPOL Low + CPHA 2 Edge** (Mode 1), NSS Software, CRC Disabled, prescaler ≈ 328 kHz. **Global interrupt Enabled**, DMA boş |
| SPI GPIO | SCK/MOSI No pull, **MISO Pull-up**, hız Very High |
| CS | GPIO_Output, Push-Pull, No pull, **başlangıç Low** (CS aktif High) |
| TIM | Internal Clock, **Channel1 = Output Compare No Output**, CH1 Mode Frozen, auto-reload preload Enable |
| TIM NVIC | Update + CC1; gelişmiş timer'da (TIM1/TIM8) **iki IRQ de** |
| HAL timebase | FreeRTOS varsa SysTick ona ait → timebase ayrı bir TIM (TIM6/TIM7) |
| Register Callback | SPI ve TIM için **DISABLE** |

Timer frekansı `BTM_PWM_HZ` ile **birebir aynı** olmalı. `ARR = 999` ile:

| Timer saati | PSC |
|---|---|
| 84 MHz (F4 APB1) | 419 |
| 168 MHz (F4 APB2) | 839 |

### 6.2 Kod

```c
#include "BTM9011EP.h"

static const BTM_Config btm_cfg = {
    .spi         = &hspi2,
    .cs_port     = BTM_CS_GPIO_Port,
    .cs_pin      = BTM_CS_Pin,
    .tim         = &htim1,       /* zorunlu */
    .forward_ms  = 6000U,        /* KARTTA ÖLÇÜLECEK: strok süresi + pay */
    .backward_ms = 6000U,
};

if (BTM_Init(&btm_cfg) != HAL_OK) {
    BTM_Status st; BTM_GetStatus(&st);
    /* FAULT_COMM -> cihaz bağlı değil / SPI modu yanlış / pinler ters
       FAULT_DWT  -> çekirdekte DWT yok veya debugger almış */
    Error_Handler();
}
```

Callback yönlendirmeleri (USER CODE 4). Callback projede zaten varsa yeni fonksiyon
yazma, satırı mevcut olanın içine ekle:

```c
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *h)        { BTM_OnSpiComplete(h); }
void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *h)           { BTM_OnSpiError(h); }
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *h)   { BTM_OnTimerPeriod(h); }
void HAL_TIM_OC_DelayElapsedCallback(TIM_HandleTypeDef *h) { BTM_OnTimerCompare(h); }
```

### 6.3 Sahip görev

```c
static void actuator_move(BTM_Direction dir, uint32_t duty)
{
    uint32_t rem;
    uint8_t  cmd;

    if (((duty >= 100U) ? BTM_Move(dir) : BTM_MovePwm(dir, duty)) != HAL_OK) {
        return;                              /* HAL_BUSY ise tekrar denenebilir */
    }

    while ((rem = BTM_RemainingMs()) > 0U) {
        /* hareket boyunca görev UYUR: yoklama yok */
        if (xQueueReceive(cmd_q, &cmd, pdMS_TO_TICKS(rem)) == pdTRUE) {
            if (cmd == CMD_STOP) { BTM_Brake(); return; }
        }
    }
    /* süre doldu: sürücü KENDİ frenledi, yapılacak bir şey yok */
}
```

Komutlar başka görevden (UART, CAN) geliyorsa sürücüyü oradan çağırma; kuyrukla
sahip göreve aktar. Komut fonksiyonları ISR'dan veya kesmeler kapalıyken
çağrılırsa `HAL_BUSY` döner. `BTM_RemainingMs` ve `BTM_GetStatus` her yerden
çağrılabilir.

---

## 7. Ayarlar

| Makro | Varsayılan | Not |
|---|---|---|
| `BTM_PWM_HZ` | 200 | Timer frekansıyla aynı olmalı |
| `BTM_FRAME_US` | 40 | Duty alt sınırı buradan; kartta ölçülmeli |
| `BTM_DROP_LIMIT` | 3 | Kaç ardışık düşen çerçeve `FAULT_OVERLAP` yapsın |
| `BTM_UV_CONFIRM` | 3 | UV kaç çerçevede teyit edilsin |
| `BTM_SPI_TIMEOUT_MS` | 10 | Polling çerçevesinin HAL timeout'u |
| `BTM_INFLIGHT_WAIT_MS` | 2 | Uçuştaki çerçevenin beklenme süresi |
| `BTM_FINISH_TIMEOUT_MS` | 5 | Son fren çerçevesi takılırsa boşa geçme |
| `BTM_CMD_FORWARD` / `_BACKWARD` | 0x51 / 0x31 | **Kartta ölçülmeli** (8. bölüm) |
| `BTM9011EP_HAL_HEADER` | `"stm32f4xx_hal.h"` | Başka aile için override |

---

## 8. Kartta yapılacaklar

Sıra önemli — ilk iki madde motora komut vermeden yapılır.

1. **Hat doğrulaması (motor bağlı değilken).** `BTM_Init()` dönüşünü kontrol et.
   `HAL_ERROR` + `FAULT_COMM` → cihaz bağlı değil, SPI modu yanlış veya pinler
   ters. Bu adım geçmeden motoru bağlama.
2. **Status okuması** üçüncü çerçevede geliyor mu (bit7 = 1), boşta hata var mı.
3. **Yön etiketleri.** Varsayılan `BTM_CMD_FORWARD = 0x51`, ama datasheet Tablo
   7'de o desen "reverse". Mekanik ileri hangisiyse ona göre, kodu değil
   `-DBTM_CMD_FORWARD=0x31 -DBTM_CMD_BACKWARD=0x51` tanımlarını kullan.
4. **Yön başına tam strok süresi** → `forward_ms` / `backward_ms` + pay.
5. **`tlead` varsayımı.** `send_it` ISR'da açık gecikme koymaz; HAL kurulum
   yükünün 250 ns'yi aştığı varsayılır. Osiloskopla doğrula — **sürücüdeki tek
   doğrulanmamış varsayım budur.**
6. **Limit switch titremesi.** Köprü sürüş komutundayken yük mili 0.5 mm geri
   iterse switch tekrar kapanıp motor yeniden vurabilir.
7. **Gerçek CPU yükü** (FreeRTOS run-time stats veya GPIO toggle + osiloskop).

---

## 9. Bilinen sınırlar

- **Konum takibi yoktur.** Sürücü "hangi uçtayım" bilgisi tutmaz; gerekiyorsa
  uygulama tutar. Konum sensörü de yoktur, hareket süre tabanlıdır.
- **Düşük duty'de strok tamamlanmaz.** `forward_ms`/`backward_ms` tam hız
  süresidir ve periyot sayısı duty ile ölçeklenmez.
- **Hata kilidi reset'e kadar kalır.** Geçici bir UV dalgalanması bile MCU reset'i
  gerektirir (`BTM_UV_CONFIRM` bunu azaltmak içindir).
- **Açık yük / kopuk kablo tespiti yoktur** (1. bölüm, 2. gerçek). Kopuk kabloda
  hareket komutu `HAL_OK` döner ve hiçbir şey hareket etmez.
- **Ters yöne geçişte ölü süre yoktur.** Biten hareket frenle bittiği için araya
  bir fren girer, ama bu yalnız bir çerçevedir (~90 µs). Mekanik olarak daha uzun
  bir fren arası gerekiyorsa uygulama iki hareket arasında kendisi beklemeli.
- **Aktüatörün azami duty cycle'ı %20** (PWM duty'si değil, ısıl sınır). 6 saniyelik
  strok ≈ 24 saniye dinlenme ister; sürücü bunu takip etmez.
- **`HAL_OK` fiziksel hareketi doğrulamaz.**
- **Tek cihaz; SPI ve CS paylaşılamaz.**
- **Öncelik dizilimi uygulamanın sorumluluğundadır** (2. bölüm).

---

## 10. Test

```sh
./tests/run_tests.sh
```

Gerçek `BTM9011EP.c`'yi taklit HAL ile derler; donanım gerekmez. Taklit cihaz
modeli datasheet Şekil 27'deki **bir çerçeve gecikmeli cevabı** taklit eder, yani
4. bölümdeki üçlü şema modelden doğrulanır.

18 grup / 74 doğrulama: Init ve hat doğrulaması, kopuk cihaz (`0xFF`), config
zorunlulukları, tam hızda kendiliğinden fren, her periyotta status okunması, PWM
akışı, `RemainingMs` ve erken fren, ters yön, OC, UV teyidi, OL'in hata
üretmemesi, düşen çerçeve sayacı, fault'ta frenin gerçekten gönderilmesi, SPI hata
yolu, geçici meşguliyetin fault üretmemesi, takılı `FINISHING`'den çıkış,
`FAULT_DWT`'nin okunabilmesi, ISR'ların yabancı handle'ları yok sayması.

> **MSYS2 notu:** `/mingw64` ve `/ucrt64` DLL'leri PATH'te karışırsa `gcc` hata
> mesajı bile basmadan exit 1 döner. `run_tests.sh` PATH'i `ucrt64` ile sınırlar.

Fiziksel zamanlama ve motor davranışı kartta ayrıca doğrulanmalıdır (8. bölüm).
