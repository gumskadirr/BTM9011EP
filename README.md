# BTM9011EP

STM32 HAL için tek BTM9011EP sürücüsü. Hedef: **Actuonix P8 `-S`** (2 kablolu,
strok sonu limit switch'li) lineer aktüatörü iki uç nokta arasında sürmek.
Tam hız veya PWM; ikisi de süreli ve kendi kendine frenler.

FOC gibi mikrosaniye hassasiyetli bir kontrol döngüsüyle **aynı MCU'da** çalışmak
üzere yazılmıştır: hiçbir yerde kesme maskelemez, ISR'larında meşgul bekleme yoktur.

| Dosya | |
|---|---|
| `BTM9011EP.h` / `BTM9011EP.c` | Sürücü (saf HAL, FreeRTOS başlığı include etmez) |
| [`INTEGRATION.md`](INTEGRATION.md) | **Projeye entegrasyon:** çevre birimleri, callback'ler, kullanım, devreye alma |
| [`examples/`](examples/) | Derlenmiş örnek kod: callback'ler, ilk deneme, süper döngü, FreeRTOS sahip görevi |

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

Çevre birimi ayarları, callback yönlendirmeleri, `BTM_Config`, hareket/durum/hata
kullanımı ve sahip görev kuralı [`INTEGRATION.md`](INTEGRATION.md)'de.

## 6.1 Başka bir STM32 ailesi (ör. STM32H7)

Sürücü aile bağımsızdır; yalnız HAL başlığı verilir:

```
-DBTM9011EP_HAL_HEADER="\"stm32h7xx_hal.h\""
```

**Cortex-M7 (F7/H7) DWT kilidi otomatik açılır.** M7'de DWT, `CTRL` yazılmadan önce
Lock Access Register'a anahtar yazılmasını isteyebilir; sürücü bunu
`__CORTEX_M == 7` koşuluyla kendi yapar (M4'te o yazmaç yoktur). Açılmasaydı
`CYCCNT` saymaz ve `BTM_Init` güvenli biçimde `HAL_ERROR` + `FAULT_DWT` dönerdi.

> **Doğrulanmadı:** H7 için bu depoda yeniden üretilebilir bir derleme veya kart
> sonucu yoktur. Taşırken `cortex-m7 -O2 -Wall -Wextra` derlemesini ve
> [`INTEGRATION.md` §9](INTEGRATION.md#9-devreye-alma-sırası) devreye alma adımlarını
> kendin çalıştır. Sürücü kesme maskelemez (`objdump`'ta `cpsid`/`cpsie`/`msr basepri`
> bulunmaz); bu özellik her derlemede yeniden kontrol edilmelidir.

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
| `BTM_PWM_HZ` | 200 | Timer frekansıyla aynı olmalı; `BTM_Init` bunu ölçüp doğrular ([INTEGRATION.md §2](INTEGRATION.md#2-çevre-birimleri)). Aralık 50..1500 |
| `BTM_REST_RATIO` | 4 | Isıl dinlenme katsayısı (§3.3). Aktüatörün azami duty cycle'ından gelir: %20 → 4. `0` kapıyı kapatır |
| `BTM_REVERSE_MS` | 100 | Ters yöne geçiş ölü süresi, ms (§3.3) |
| `BTM_CMD_FORWARD` / `_BACKWARD` | 0x51 / 0x31 | **Kartta ölçülmeli** (8. bölüm) |
| `BTM9011EP_HAL_HEADER` | `"stm32f4xx_hal.h"` | Başka aile için override |

Geri kalanlar sürücünün **iç zamanlama sabitleridir**, ayar düğmesi değil: hiçbiri
dışarıdan override edilemez, çünkü sürücü yalnız bu değerlerle doğrulandı ve birkaçı
birbirine bağlı (`BTM_FRAME_US` duty alt sınırını, o da süre ölçeklemesini etkiler).
Değiştirmek gerekiyorsa `BTM9011EP.h`'de değiştirip kartta yeniden doğrula.

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

## 10. Doğrulama

Bu depoda otomatik host testi **yoktur**. Doğrulama durumu (derleme ve kart
sonuçları, hangi HAL sürümü ve kartla) [`INTEGRATION.md` §11](INTEGRATION.md#11-doğrulama-durumu)
tablosunda tutulur; fiziksel zamanlama ve motor davranışı kartta doğrulanmalıdır
(8. bölüm).
