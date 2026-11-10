# BTM9011EP


STM32 HAL için tek BTM9011EP sürücüsü. Hedef: **Actuonix P8 `-S`** (2 kablolu,
strok sonu limit switch'li) lineer aktüatörü iki uç nokta arasında sürmek.
Tam hız veya PWM; ikisi de süreli ve kendi kendine frenler.

> **v2'den farkı iki maddedir** (ayrıntı: [`DEGISIKLIKLER.md`](DEGISIKLIKLER.md)):
> 1. `BTM_ClearFault()` — hata kilidi artık MCU reset'i gerektirmiyor;
> 2. hareket süresi uygulanan duty ile ölçekleniyor, yani düşük duty'de de strok
>    tamamlanıyor (`BTM_Status.move_ms` + iki bayrak bunu bildiriyor).

FOC gibi mikrosaniye hassasiyetli bir kontrol döngüsüyle **aynı MCU'da** çalışmak
üzere yazılmıştır: hiçbir yerde kesme maskelemez, ISR'larında meşgul bekleme yoktur.

| Dosya | |
|---|---|
| `BTM9011EP.h` / `BTM9011EP.c` | Sürücü (saf HAL, FreeRTOS başlığı include etmez) |
| `ENTEGRASYON.md` | STM32F407 DISC1 üzerinde sıfırdan kurulum, CubeMX ekran ekran |
| `DEGISIKLIKLER.md` | v2 → v3 ve v3 → v3.1 değişiklikleri, taşıma notları |
| `tests/` | Host testi; gerçek `.c`'yi taklit HAL ile derler, donanım gerekmez |

Önceki sürümler (`BTM9011EP_Sade` = v1, `BTM9011EP_Sade_v2`) ve onları karşılaştıran
`KARSILASTIRMA_RAPORU.md` **git geçmişindedir** — ilk kayıtta (`git show 447b921`).
Çalışma klasöründen çıkarıldılar, çünkü v3 ikisinin de işlevsel kapsamı ve üç klasör
aynı dosya adlarını taşıdığı için yanlış sürümü karta kopyalamak kolaydı.
Raporun §7.1 ve §7.2 önerileri v3'te uygulanmıştır (`DEGISIKLIKLER.md`).

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
| `BTM_ClearFault` (bekleme + doğrulanmış komut) | 4 × `BTM_INFLIGHT_WAIT_MS` |

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
| Hareket süresi | `forward_ms`/`backward_ms` | **duty ile ölçeklenir** (bkz. §3.1) |
| Süre dolunca | sürücü kendi frenler | sürücü kendi frenler |

Bunun iki sonucu var:

- **Uygulamanın sonda bir şey yapması gerekmez.** `BTM_Brake()` yalnız *erken*
  durdurma içindir.
- **Her iki kipte de her periyotta bir status baytı okunur**, çünkü sürüş
  çerçevesi her periyot tekrar gider. Cihaz hataları hareketin ortasında görülür.

### 3.1 Süre duty ile ölçeklenir

Config'teki `forward_ms`/`backward_ms` **tam hız** strok süresidir. PWM'de sürücü
süreyi uygulanan duty'ye göre kendi büyütür:

```
   move_ms = ceil(travel_ms * 100 / duty)        (sonra BTM_MAX_MOVE_MS'e kırpılır)

   duty %100 ->  6000 ms        duty %50 -> 12000 ms        duty %25 -> 24000 ms
```

Böylece düşük duty'de de strok tamamlanır; uygulamanın süreyi elle büyütmesi gerekmez.

**Bu bir TAHMİNDİR.** Model "ortalama köprü gerilimi duty ile doğrusal, boştaki hız
gerilimle doğrusal → süre ~ 1/duty" varsayımına dayanır; aktüatörün ölü bölgesi (çok
düşük duty'de hiç kalkmaması), yük ve sürtünme modele girmez. Bu yüzden sürücü ne
yaptığını bildirir:

| `BTM_Status` alanı | Anlamı |
|---|---|
| `move_ms` | Bu hareket için **planlanan** toplam süre (ölçeklenmiş, kırpılmışsa kırpılmış) |
| `move_ms_estimated` | 1 → duty < 100, süre yukarıdaki doğrusal modelle tahmin edildi |
| `move_ms_clamped` | 1 → süre `BTM_MAX_MOVE_MS`'e kırpıldı, **strok tamamlanmayabilir** |

`BTM_RemainingMs()` değeri **periyot sayacından** hesaplanır — hareketi bitiren şey
o olduğu için bildirilen süre ile gerçek bitiş ayrışamaz. Ölçeklenmiş süreyi gösterir,
yani sahip görev `rem` kadar
uyuduğunda hareketin gerçek sonunda uyanır.

`-S` varyantında fazla süre zararsızdır (limit switch devreyi açar), bu yüzden
ölçeklemenin fazla tarafa sapması güvenli yöndür. Gerçek değer kartta ölçülmelidir (§8).

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
    └───────────── BTM_ClearFault() ◄── yalnız hat yeniden doğrulanırsa
                          │
                   ┌──────┴───────────────────┐
                   │          FAULT           │ ◄── herhangi bir
                   │ kilitli; hepsi HAL_ERROR │     hatada
                   └──────────────────────────┘
```

`FAULT` → `IDLE` geçişi yalnız `BTM_ClearFault()` ile olur ve **hattın yeniden
doğrulanmasına bağlıdır**: arızanın sebebi sürüyorsa kilit geri gelir (§3.2).

### 3.3 Aktüatörü koruyan iki kapı

Hareket başlatmanın iki ön koşulu var; ikisi de aktüatörün donanım gerçeği, bu yüzden
uygulamaya bırakılmayıp sürücüde uygulanır. Kapı kapalıysa `BTM_Move`/`BTM_MovePwm`
**`HAL_BUSY`** döner: hiçbir şey olmaz, fault kaydedilmez, çağrı sonra tekrarlanabilir.

| Kapı | Kural | Neden |
|---|---|---|
| **Ters yön ölü süresi** (`BTM_REVERSE_MS`, 100 ms) | Önceki hareketin bitişinden bu kadar süre geçmeden **ters** yön kabul edilmez | Fren sonrası motor hâlâ dönüyor olabilir; dönen motora ters gerilim bindirmek aşırı akımdır → `OCA`/`OCB` → hata kilidi |
| **Isıl dinlenme** (`BTM_REST_RATIO`, 4) | Yeni hareket, önceki hareketin **fiilen sürdüğü** sürenin bu katı kadar dinlenme dolmadan kabul edilmez | Actuonix P8 azami **%20 duty cycle** ister (1 sür / 4 dinlen); aşılırsa motor sargısı ısınır |

Dinlenme **planlanan değil geçen** süre üzerinden hesaplanır: 6 s'lik strok 2 s sonra
`BTM_Brake()` ile kesildiyse 8 s dinlenme ister, 24 s değil. PWM'de süre duty ile
ölçeklendiği için (§3.1) dinlenme de ölçeklenmiş süreye göre uzar — `BTM_Status.move_ms`
bu sürenin ne olduğunu söyler.

İlk hareketten önce iki kapı da etkisizdir. Hata ile kesilen bir hareketin sürüş süresi
ısıl bütçeye **yazılmaz** (9. bölüm).

### 3.2 Hata durumu ve kilidin açılması

```
  echo ≠ gönderilen, 0xFF, veya bit7 beklenmedik  ──► FAULT_COMM
  status baytında OCA|OCB|TSDA|TSDB (anında)      ──► FAULT_DEVICE
  status baytında UV, BTM_UV_CONFIRM kez üst üste ──► FAULT_DEVICE
  HAL_SPI_ErrorCallback                           ──► FAULT_SPI
  BTM_DROP_LIMIT kadar ardışık düşen çerçeve      ──► FAULT_OVERLAP
  Init'te DWT saymıyor                            ──► FAULT_DWT

  Hatada: timer durur, durum FAULT olur, bütün komutlar HAL_ERROR döner.
```

**`BTM_ClearFault()` kilidi açar** — ama "hatayı yok sayma" değil **yeniden sınama**dır:
timer'ı durdurur, ardışık sayaçları sıfırlar ve hattı **doğrulanmış fren komutuyla**
(3 çerçeve) yeniden dener.

| Dönüş | Anlamı |
|---|---|
| `HAL_OK` | Hata yoktu, ya da temizlendi **ve** hat doğrulandı → durum `IDLE`, köprü frende |
| `HAL_BUSY` | Hat bir an meşgul; **hiçbir şey değişmedi**, hata yerinde, çağrı tekrarlanabilir |
| `HAL_ERROR` | Sebep sürüyor (yeni fault `BTM_GetStatus`'ta görünür), ya da `FAULT_DWT`, ya da Init edilmemiş |

- **`FAULT_DWT` temizlenemez:** DWT sayacı saymıyorken `delay_us` güvenilmez kalır.
  Reset gerekir (veya debugger'ı bırakmak).
- **`dropped_frames` sıfırlanmaz:** ömür boyu teşhis sayacıdır, geçmiş silinmez.
- Hangi hataların temizlenmesi anlamlıdır: `FAULT_OVERLAP` (kesme gecikmesi geçmiştir),
  `FAULT_SPI`, geçici `UV` kaynaklı `FAULT_DEVICE`. Kalıcı bir sebep (kısa devre →
  `OCA`, kopuk SPI → `COMM`) zaten `HAL_ERROR` ile geri gelir.

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

Timer frekansı `BTM_PWM_HZ` ile **birebir aynı** olmalı. `BTM_Init` bunu artık
kendisi ölçer: `tim_clk_hz / ((PSC + 1) * (ARR + 1))` değerini `BTM_PWM_HZ` ile
karşılaştırır ve %5'ten fazla saparsa `HAL_ERROR` döner (fault kaydedilmez, config
reddedilir). Bu yüzden config'e timer'ın besleme saati de yazılır. `ARR = 999` ile:

| Timer saati | PSC |
|---|---|
| 84 MHz (F4 APB1: TIM2..TIM7) | 419 |
| 168 MHz (F4 APB2: TIM1, TIM8..) | 839 |

`tim_clk_hz` bu tablodaki **timer saati** değeridir, PSC ile bölünmüş değer değil.

### 6.2 Kod

```c
#include "BTM9011EP.h"

static const BTM_Config btm_cfg = {
    .spi         = &hspi2,
    .cs_port     = BTM_CS_GPIO_Port,
    .cs_pin      = BTM_CS_Pin,
    .tim         = &htim1,       /* zorunlu */
    .tim_clk_hz  = 168000000U,   /* zorunlu: timer'ın besleme saati (htim1 APB2'de).
                                  * Init bununla PSC/ARR'dan gerçek frekansı ölçer ve
                                  * BTM_PWM_HZ ile uyuşmazsa HAL_ERROR döner. */
    .forward_ms  = 6000U,        /* KARTTA ÖLÇÜLECEK: TAM HIZ strok süresi + pay.
                                  * Düşük duty'de sürücü bunu kendi ölçekler (§3.1);
                                  * üst sınır BTM_MAX_MOVE_MS. */
    .backward_ms = 6000U,
};

if (BTM_Init(&btm_cfg) != HAL_OK) {
    BTM_Status st; BTM_GetStatus(&st);
    /* FAULT_COMM -> cihaz bağlı değil / SPI modu yanlış / pinler ters
       FAULT_DWT  -> çekirdekte DWT yok veya debugger almış
       fault NONE -> config reddedildi: tim_clk_hz / PSC / ARR uyuşmuyor, strok
                     süresi 0 veya BTM_MAX_MOVE_MS'ten büyük */
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

    HAL_StatusTypeDef res = (duty >= 100U) ? BTM_Move(dir) : BTM_MovePwm(dir, duty);

    if (res != HAL_OK) {
        /* HAL_BUSY: hat bir an meşgul, ya da ısıl dinlenme / ters yön ölü süresi
         *   dolmadı (§3.3). Hiçbir şey olmadı; sonra tekrar dene.
         * Diğerleri: sürüş komutu cihaza GİTMİŞ olabilir. Sürücü fren denedi ve
         *   fault kaydetti; emniyet payı olarak bir kez daha frenle, sonra
         *   kurtarma politikasını uygula (§6.4). */
        if (res != HAL_BUSY) { (void)BTM_Brake(); }
        return;
    }

    while ((rem = BTM_RemainingMs()) > 0U) {
        /* hareket boyunca görev UYUR: yoklama yok */
        if (xQueueReceive(cmd_q, &cmd, pdMS_TO_TICKS(rem)) == pdTRUE) {
            if (cmd == CMD_STOP) { BTM_Brake(); return; }
        }
    }

    /* Süre dolduğunda sürücü ZATEN frenledi. Buradaki çağrı emniyet payıdır:
     * TIM callback'leri yanlışlıkla yönlendirilmemişse sürücünün içinde hiçbir
     * kod çalışmaz ve motor süresiz sürüşte kalırdı. Maliyeti 3 çerçeve (~90 us),
     * idempotenttir. */
    (void)BTM_Brake();
}
```

Komutlar başka görevden (UART, CAN) geliyorsa sürücüyü oradan çağırma; kuyrukla
sahip göreve aktar. Komut fonksiyonları ISR'dan veya kesmeler kapalıyken
çağrılırsa `HAL_BUSY` döner. `BTM_RemainingMs` ve `BTM_GetStatus` her yerden
çağrılabilir.

### 6.4 Hata kilidinden çıkma (politika uygulamada)

Sürücü kendi kendine denemez; kaç kez ve hangi aralıkla denenecek uygulamanın
kararıdır. Sahip görevde sınırlı denemeli bir örnek:

```c
#define BTM_CLEAR_TRIES 3U

/** Hata kilidini sınırlı sayıda dener. 1 döndüyse sürücü yeniden kullanılabilir. */
static uint8_t actuator_recover(void)
{
    uint32_t i;

    for (i = 0U; i < BTM_CLEAR_TRIES; i++) {
        HAL_StatusTypeDef res = BTM_ClearFault();

        if (res == HAL_OK) { return 1U; }            /* kilit açıldı, durum IDLE */

        if (res != HAL_BUSY) {                       /* HAL_ERROR veya HAL_TIMEOUT */
            BTM_Status st;
            (void)BTM_GetStatus(&st);
            /* Sebep sürüyor: st.fault + st.status_byte'a bak.
             * FAULT_DWT, OCA/OCB veya COMM ise denemeyi bırak — fiziksel sebep. */
            if (st.fault != BTM_FAULT_NONE) { return 0U; }
        }
        /* HAL_BUSY: hiçbir şey değişmedi, biraz bekle ve tekrar dene */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return 0U;
}
```

**Körlemesine döngüye sokma.** `ClearFault` hattı yeniden sınadığı için kalıcı bir
arızada her denemede 3 çerçeve harcar ve `HAL_ERROR` döner; sebep (`st.fault`,
`st.status_byte`) incelenmeden tekrarlamak arızayı maskelemekten başka işe yaramaz.
UV gibi geçici bir sebebin gerçekten geçmesi için denemeler arasında beklemek gerekir.

---

## 6.4 Başka bir STM32 ailesi (ör. STM32H7)

Sürücü aile bağımsızdır; yalnız HAL başlığı verilir:

```
-DBTM9011EP_HAL_HEADER="\"stm32h7xx_hal.h\""
```

**Cortex-M7 (F7/H7) DWT kilidi otomatik açılır.** M7'de DWT, `CTRL` yazılmadan önce
Lock Access Register'a anahtar yazılmasını isteyebilir; sürücü bunu
`__CORTEX_M == 7` koşuluyla kendi yapar (M4'te o yazmaç yoktur). Açılmasaydı
`CYCCNT` saymaz ve `BTM_Init` güvenli biçimde `HAL_ERROR` + `FAULT_DWT` dönerdi.

Doğrulananlar (STM32Cube_FW_H7 v1.12.1 ve arm-none-eabi-gcc 13.3 ile) — bu tablo
**v2 kodu üzerinde** üretildi:

| | Durum |
|---|---|
| Kullanılan HAL fonksiyonları ve makroları H7 HAL'de var | ✔ |
| `cortex-m7 -O2` sıkı uyarılarla derleniyor | ✔ (0 uyarı) |
| M7 yolunda `LAR` yazması üretiliyor | ✔ |
| M7 yolunda host testleri geçiyor | ✔ (v2'de 18 grup / 74 doğrulama) |
| Kesme maskeleme | ✔ yok (`cpsid=0`) |

> **v3 notu:** v3'ün iki değişikliği (`BTM_ClearFault`, duty ölçeklemesi) yeni HAL
> çağrısı veya çekirdeğe özgü komut **eklemez** — yalnız mevcut yardımcıları ve
> tamsayı aritmetiğini kullanır, bu yüzden H7 uyumunun bozulması beklenmez. Yine de
> yukarıdaki tablo v3 kodu için **yeniden koşturulmamıştır**; H7'ye taşınırken
> `cortex-m7 -O2` derlemesi ve host testleri tekrar çalıştırılmalıdır.

Aile değişince **yeniden hesaplanması gerekenler**:

- **Timer PSC/ARR.** `ARR = 999` ile `PSC + 1 = timer_saati / (BTM_PWM_HZ × 1000)`:

  | Timer saati | PSC (200 Hz) |
  |---|---|
  | 84 MHz (F4 APB1) | 419 |
  | 168 MHz (F4 APB2) | 839 |
  | 200 MHz (H7, örnek) | 999 |
  | 240 MHz (H7, örnek) | 1199 |

  H7'nin saat ağacı yapılandırmaya göre değişir; değeri CubeMX Clock Configuration
  ekranındaki "APBx timer clocks" alanından al.
- **SPI prescaler**, hedef ≈ 328 kHz SCK.
- **`BTM_FRAME_US`**, osiloskopla ölçülüp doğrulanmalı.

H7'ye özgü iki not:

- **D-Cache sorun değil:** sürücü DMA kullanmaz, bütün SPI tamponları yalnız CPU
  tarafından erişilir, dolayısıyla cache bakımı (`SCB_CleanDCache` vb.) gerekmez.
- **H7'nin SPI çevrebirimi farklı bir IP'dir** (FIFO'lu); HAL API'si aynıdır ama
  init yapısında ek alanlar vardır (FifoThreshold, MasterKeepIOState...). Bunlar
  CubeMX tarafından doldurulur, sürücüyü ilgilendirmez. Tek baytlık aktarımın
  FIFO ile davranışı kartta doğrulanmalıdır.

Cortex-M0/M0+'da **çalışmaz**: o çekirdeklerde DWT çevrim sayacı yoktur.

---

## 7. Ayarlar

Derleyici satırından (`-D...`) verilebilen tek şey, **karta ve aktüatöre bağlı**
şu beş değerdir:

| Makro | Varsayılan | Not |
|---|---|---|
| `BTM_PWM_HZ` | 200 | Timer frekansıyla aynı olmalı; `BTM_Init` bunu ölçüp doğrular (§6.1). Aralık 50..1500 |
| `BTM_REST_RATIO` | 4 | Isıl dinlenme katsayısı (§3.3). Aktüatörün azami duty cycle'ından gelir: %20 → 4. `0` kapıyı kapatır |
| `BTM_REVERSE_MS` | 100 | Ters yöne geçiş ölü süresi, ms (§3.3) |
| `BTM_CMD_FORWARD` / `_BACKWARD` | 0x51 / 0x31 | **Kartta ölçülmeli** (8. bölüm) |
| `BTM9011EP_HAL_HEADER` | `"stm32f4xx_hal.h"` | Başka aile için override |

Geri kalanlar sürücünün **iç zamanlama sabitleridir**, ayar düğmesi değil: hiçbiri
dışarıdan override edilemez, çünkü testler yalnız bu değerlerle koşuyor ve birkaçı
birbirine bağlı (`BTM_FRAME_US` duty alt sınırını, o da süre ölçeklemesini etkiler).
Değiştirmek gerekiyorsa `BTM9011EP.h`'de değiştirip testleri yeniden koştur.

| Sabit | Değer | Ne işe yarar |
|---|---|---|
| `BTM_FRAME_US` | 40 | Bir çerçevenin kabaca süresi; duty alt sınırı buradan (kartta ölçülmeli) |
| `BTM_DROP_LIMIT` | 3 | Kaç ardışık düşen çerçeve `FAULT_OVERLAP` yapsın |
| `BTM_UV_CONFIRM` | 3 | UV kaç çerçevede teyit edilsin |
| `BTM_SPI_TIMEOUT_MS` | 10 | Polling çerçevesinin HAL timeout'u |
| `BTM_INFLIGHT_WAIT_MS` | 2 | Uçuştaki çerçevenin beklenme süresi |
| `BTM_FINISH_TIMEOUT_MS` | 5 | Son fren çerçevesi takılırsa boşa geçme |
| `BTM_MAX_MOVE_MS` | 60000 | Bir hareketin azami süresi: `forward_ms`/`backward_ms` üst sınırı (Init reddeder) **ve** duty ölçeklemesinin kırpma noktası |

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
4. **Yön başına tam strok süresi** → `forward_ms` / `backward_ms` + pay
   (`BTM_MAX_MOVE_MS`'i aşarsa `BTM_Init` `HAL_ERROR` döner).
5. **Duty ölçeklemesinin gerçekliği (§3.1).** `BTM_MovePwm(dir, 50)` ile strok
   gerçekten tamamlanıyor mu? Doğrusal model tahminidir: aktüatörün **ölü bölgesi**
   (hangi duty'nin altında hiç kalkmıyor) ve yük altındaki gerçek hızı ölç. Model
   eksik kalıyorsa `forward_ms`'e pay ekle — `-S` varyantında fazla süre zararsızdır.
6. **`tlead` varsayımı.** `send_it` ISR'da açık gecikme koymaz; HAL kurulum
   yükünün 250 ns'yi aştığı varsayılır. Osiloskopla doğrula — **sürücüdeki tek
   doğrulanmamış varsayım budur.**
7. **Limit switch titremesi.** Köprü sürüş komutundayken yük mili 0.5 mm geri
   iterse switch tekrar kapanıp motor yeniden vurabilir.
8. **Gerçek CPU yükü** (FreeRTOS run-time stats veya GPIO toggle + osiloskop).

---

## 9. Bilinen sınırlar

- **Konum takibi yoktur.** Sürücü "hangi uçtayım" bilgisi tutmaz; gerekiyorsa
  uygulama tutar. Konum sensörü de yoktur, hareket süre tabanlıdır.
- **Duty ölçeklemesi bir tahmindir** (§3.1). Süre `1/duty` ile büyütülür; aktüatörün
  ölü bölgesi ve yük etkisi modele girmez. `move_ms_estimated` bunu bildirir, gerçek
  değer kartta ölçülmelidir. Ölçeklenen süre `BTM_MAX_MOVE_MS`'i aşarsa kırpılır
  (`move_ms_clamped = 1`) ve **o durumda strok tamamlanmayabilir**.
- **Hata kilidinden çıkmak uygulamanın kararıdır.** `BTM_ClearFault()` var, ama
  sürücü kendi kendine denemez: kaç kez ve hangi aralıkla denenecek, kaç denemeden
  sonra vazgeçilecek uygulamaya aittir. `FAULT_DWT` hiç temizlenemez (reset gerekir).
- **Açık yük / kopuk kablo tespiti yoktur** (1. bölüm, 2. gerçek). Kopuk kabloda
  hareket komutu `HAL_OK` döner ve hiçbir şey hareket etmez.
- **Ters yön ölü süresi ve ısıl dinlenme sürücüde uygulanır** (§3.3), ama **hata ile
  kesilen** bir hareketin sürüş süresi ısıl bütçeye yazılmaz: `FAULT` yolunda süre
  kaydedilmez, yani `BTM_ClearFault()` sonrası dinlenme kapısı o hareketi hiç olmamış
  sayar. Uzun bir hareket hatayla kesildiyse dinlenmeyi uygulama beklemelidir.
- **Ölü süre ve dinlenme sayaçları hareketin bitişine dayanır**, motorun gerçekten
  durduğuna değil: 100 ms, ölçülmüş değil seçilmiş bir paydır. Ağır yükte
  `BTM_REVERSE_MS`'i kartta ölçüp büyüt.
- **Aktüatörün azami duty cycle'ı %20** (PWM duty'si değil, ısıl sınır). Sürücü bunu
  `BTM_REST_RATIO` ile uygular (§3.3) ve hesabı **geçen** süre üzerinden yapar. Dikkat:
  duty ölçeklemesi (§3.1) sürüş süresini uzatır, yani %25 duty'de 6 saniyelik strok
  24 saniye sürer ve ≈96 saniye dinlenme ister — kapı bunu kendisi hesaplar, ama
  hareket sıklığını buna göre planlamak uygulamaya aittir.
- **Ölçekleme, `-S` OLMAYAN bir aktüatörde stall penceresini de uzatır.** Bu sürücü
  limit switch'li `-S` varyantı için yazıldı: strok sonunda devre açılır, fazla süre
  boşa geçer. Limit switch'i olmayan bir aktüatörde strok sonuna gelindikten sonraki
  süre **stall**'dır ve duty ölçeklemesi bu pencereyi duty oranında büyütür. Böyle bir
  aktüatörde `forward_ms` payını küçük tut veya ölçeklemeyi devre dışı bırak
  (`BTM_MovePwm` yerine süreyi kendin hesaplayıp `BTM_Move`'u kısa sürelerle kullan).
- **`HAL_OK` fiziksel hareketi doğrulamaz.**
- **Status baytı boştayken kendiliğinden tazelenmez.** Hareket sırasında her periyotta
  okunur; boştayken son çerçeveden kalmadır. Güncel bayt gerekiyorsa `BTM_Poll()`
  çağrılır (3 çerçeve, ~90 µs).
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

**38 grup / 193 doğrulama:** Init ve hat doğrulaması, kopuk cihaz (`0xFF`), config
zorunlulukları, tam hızda kendiliğinden fren, her periyotta status okunması, PWM
akışı, `RemainingMs` ve erken fren, ters yön, OC, UV teyidi, OL'in hata
üretmemesi, düşen çerçeve sayacı, fault'ta frenin gerçekten gönderilmesi, SPI hata
yolu, geçici meşguliyetin fault üretmemesi, takılı `FINISHING`'den çıkış,
`FAULT_DWT`'nin okunabilmesi, ISR'ların yabancı handle'ları yok sayması, ham baytların
saklanması.

v3'te eklenen 10 grup (21–30): `ClearFault` sonrası kilidin açılması ve hareketin
tekrar edilebilmesi, sebep sürüyorsa kilidin geri gelmesi, `FAULT_DWT`'nin
temizlenememesi, hata yokken `ClearFault`'un çerçeve harcamaması, meşgul hatta
`HAL_BUSY` dönüp **hiçbir şeyi değiştirmemesi**, `dropped_frames`'in korunması,
duty ölçeklemesi (süre, periyot sayısı ve `RemainingMs`), `BTM_MAX_MOVE_MS` kırpması
ve bayrağı, config sınırı, ölçeklenmiş sürenin sonunda sürücünün kendi frenlemesi,
"kabloyu düzelttim" akışı (Init `FAULT_COMM` → `ClearFault` → hareket).

v3.1'de eklenen 8 grup (31–38): sürüş komutu gittikten sonra echo tutmazsa ve çerçeve
ortasında SPI hata verirse **frenin de gönderilmesi**, timer frekansı `BTM_PWM_HZ` ile
uyuşmuyorsa Init'in reddetmesi, ters yön ölü süresi, ısıl dinlenmenin fiilen süren
süreye dayanması ve `BTM_Poll`'un taze status baytı getirmesi; ayrıca timer'ın bekleyen `UIF`
bayrağının hareketi bir periyot kısaltmaması (taklit HAL artık bekleyen bayrakla
başlatmanın kesmeyi anında tetiklemesini de modelliyor) ve `BTM_RemainingMs`'in
duvar saatinden değil periyot sayacından gelmesi. Polling çerçevesinin
hata yolu için taklit HAL'e `spi_fail_in` kancası eklendi; o dal daha önce hiç test
edilmiyordu, çünkü taklit her zaman `HAL_OK` dönüyordu.

İki geri alma yolu da kapsanıyor: hat `ClearFault`'un **ilk beklemesinde** meşgulse
ve hat ilk beklemeyi geçip **doğrulanmış komutun içinde** meşgulleşirse. İkincisi
için taklit HAL'e `busy_on_gettick` kancası eklendi; o dal olmasa sessizce
test edilmemiş kalırdı.

> **MSYS2 notu:** `/mingw64` ve `/ucrt64` DLL'leri PATH'te karışırsa `gcc` hata
> mesajı bile basmadan exit 1 döner. `run_tests.sh` PATH'i `ucrt64` ile sınırlar.

Fiziksel zamanlama ve motor davranışı kartta ayrıca doğrulanmalıdır (8. bölüm).
