# Değişiklikler

## v3 → v3.1

YAGNI incelemesinin bulguları. Davranış sözleşmesi aynı kaldı; eklenen iki kapı
hareket komutlarına yeni bir `HAL_BUSY` sebebi getirdi.

| | Bulgu | Ne geldi |
|---|---|---|
| **1** | Üç çerçeveli komutun ortasında hata çıkarsa sürüş komutu cihaza uygulanmış, ama fren gönderilmemiş oluyordu: aktüatör süresiz sürebilirdi | `start_move`, `HAL_BUSY` dışındaki her hatada doğrulanmış **fren** dener; README §6.3 ve ENTEGRASYON örnekleri de `HAL_BUSY` dışındaki hatada `BTM_Brake()` çağırıyor |
| **2** | Timer'ın gerçekten `BTM_PWM_HZ`'de döndüğü hiç kontrol edilmiyordu; yanlış PSC bütün süreleri aynı oranda kaydırıyordu | `BTM_Config.tim_clk_hz` alanı + `BTM_Init` içinde `tim_clk_hz / ((PSC+1)*(ARR+1))` ölçümü, %5 toleransla; uymazsa `HAL_ERROR` |
| **3** | Aktüatörün %20 ısıl sınırı ve ters yön ölü süresi "uygulamaya ait" diye yazılmıştı, ama dokümanın kendi örnek kodu ikisini de ihlal ediyordu | `BTM_REST_RATIO` (fiilen süren sürenin katı kadar dinlenme) ve `BTM_REVERSE_MS` (ters yön öncesi ölü süre) kapıları; ikisi de `HAL_BUSY` döner |
| **4** | Boştayken güncel status baytı okumanın yolu yoktu; UART "durum sorgusu" paketine eski bayt ile cevap veriliyordu | `BTM_Poll()` — fren komutunu tekrarlayıp taze status baytı getirir (3 çerçeve, köprüye etkisiz) |
| **5** | Polling çerçevesinin hata yolu hiç test edilmiyordu (taklit HAL her zaman `HAL_OK` dönüyordu) | Taklit HAL'e `spi_fail_in` kancası; 1. bulgunun iki yolu da test edildi |
| **6** | Durdurulan timer'ın bekleyen `UIF` bayrağı temizlenmiyordu: yeniden başlatmada NVIC o kesmeyi anında çalıştırıyor, hareket bir periyot eksik başlıyordu | `start_move` sayıcıyı sıfırladıktan sonra `__HAL_TIM_CLEAR_FLAG(UPDATE \| CC1)` çağırıyor |
| **7** | Sürenin iki kaynağı vardı: hareketi `periods_left` bitiriyor, `BTM_RemainingMs` ise tick tabanlı `move_deadline`'dan okuyordu — ikisi ayrışabiliyordu | `BTM_RemainingMs` artık `periods_left`'ten hesaplanıyor. `move_deadline` silindi; yerine yalnız ısıl bütçe için `move_start_tick` var (duvar saati, çünkü aktüatör gerçek süre boyunca ısınır) |
| **8** | `state_seq` sayacı yazılıyor ama **hiç okunmuyordu**: yarım bırakılmış bir seqlock | Sayaç silindi; `publish()` adı `barrier()` oldu — yaptığı tek şey `__DMB()`, ismi de artık onu söylüyor |
| **9** | 12 derleme-zamanı ayar düğmesinin yedisi hiçbir yerde override edilmiyordu ve tek bir değer kombinasyonuyla test ediliyordu | Yedisi sade `#define` oldu (iç sabit); `-D` ile verilebilen beş değer kaldı: `BTM_PWM_HZ`, `BTM_REST_RATIO`, `BTM_REVERSE_MS`, `BTM_CMD_FORWARD/_BACKWARD`, `BTM9011EP_HAL_HEADER` |

Kod boyutu: `BTM9011EP.c` 626 → 720 satır, `BTM9011EP.h` 232 → 259 satır
(6–9 arası bulgular başlıktan 13 satır eksiltti).
Test: **38 grup / 193 doğrulama geçiyor** (v3: 30 grup / 155).

Dokümanda ayrıca: `ENTEGRASYON.md` §3.6'ya UART paket akışı örneği eklendi
(RX kesmesi bayrak kaldırır, sürücüyü ana döngü çağırır), README §3.3 iki kapıyı
anlatıyor, §7 ayar tablosu ve §9 bilinen sınırlar güncellendi.

---

## v2 → v3 Değişiklikleri

`KARSILASTIRMA_RAPORU.md` §7.1 ve §7.2 önerilerinin uygulanması (rapor artık git
geçmişinde: `git show 447b921:KARSILASTIRMA_RAPORU.md`). Başka hiçbir
davranış değiştirilmedi; gerçek zaman sözleşmesi ve SPI protokol mantığı aynı.

| | Öneri | Ne geldi |
|---|---|---|
| **§7.1** | Hata kilidi MCU reset'i gerektiriyordu | `BTM_ClearFault()` — hattı doğrulanmış fren komutuyla yeniden sınar, tutarsa kilidi açar |
| **§7.2** | `MovePwm` düşük duty'de stroku tamamlamıyordu | Hareket süresi uygulanan duty ile ölçeklenir + `move_ms` / `move_ms_estimated` / `move_ms_clamped` alanları |

Kod boyutu: `BTM9011EP.c` 500 → 621 satır, `BTM9011EP.h` 187 → 229 satır.
Test: **30 grup / 155 doğrulama geçiyor** (v2: 20 grup / 86).

---

## 1. §7.1 — `BTM_ClearFault()`

### API

```c
HAL_StatusTypeDef BTM_ClearFault(void);
```

| Dönüş | Anlamı |
|---|---|
| `HAL_OK` | Hata yoktu, ya da temizlendi **ve** hat doğrulandı → durum `IDLE`, köprü frende |
| `HAL_BUSY` | Hat bir an meşgul; **hiçbir şey değişmedi**, hata yerinde duruyor, tekrar denenebilir |
| `HAL_ERROR` | Sebep sürüyor (yeni fault `BTM_GetStatus`'ta), ya da `FAULT_DWT`, ya da Init edilmemiş |

### Tasarım kararları ve gerekçeleri

**Temizleme = yeniden sınama, yok sayma değil.** Fonksiyon `fault`'u sıfırlayıp
dönmüyor; `send_verified(BTM_CMD_BRAKE)` ile 3 çerçevelik doğrulanmış komut
gönderiyor. Cihaz konuşmuyorsa veya status baytı hâlâ kirliyse `record_fault`
yeniden tetiklenir ve kilit geri gelir. Böylece "temizlendi" demek "hat şu an
sağlam ve köprü frende" demek oluyor.

**`FAULT_DWT` temizlenemez.** DWT çevrim sayacı saymıyorken `delay_us` ve
`cs_gap_ok` güvenilmez kalır — yeniden sınamanın bir anlamı yok. Reset (veya
debugger'ı bırakmak) gerekir.

**`HAL_BUSY` yolunda hiçbir şey değişmez.** Uçuştaki aktarım `BTM_INFLIGHT_WAIT_MS`
içinde bitmezse fonksiyon erken döner; `fault`, `hal_error` ve `FAULT` durumu
dokunulmadan kalır. Hiçbir şey doğrulanamadığı için kilidi açmak yanlış olurdu.
Fault'un sıfırlanıp sonra geri konulduğu kısa pencerede timer durmuş ve durum
`MOVING` değildir, yani ISR'lar çerçeve göndermez.

**`dropped_frames` sıfırlanmaz.** Ömür boyu teşhis sayacıdır; arıza geçmişi
silinmemeli. Sıfırlananlar yalnız ardışık sayaçlar (`drop_run`, `uv_run`) ve
`hal_error`.

**Politika sürücüde değil uygulamada.** Sürücü kendi kendine denemez: kaç kez,
hangi aralıkla, kaç denemeden sonra vazgeçileceği uygulamanın kararı. README §6.4'te
sınırlı denemeli örnek var.

### Sıra (ISR'larla yarış yok)

```
  stop_timer()                      -> yeni periyot/compare ISR'ı gelmez
  uçuştaki aktarımı bekle (bounded) -> rx_byte üzerinde yarış kalmaz
  sayaçları sıfırla, fault = NONE
  state = IDLE  (publish)
  send_verified(BRAKE)              -> tutmazsa fault geri gelir
```

---

## 2. §7.2 — Süre duty ile ölçekleniyor

### Formül

```
   move_ms = ceil(travel_ms * 100 / duty)     sonra BTM_MAX_MOVE_MS'e kırpılır
   periods_left = move_ms * BTM_PWM_HZ / 1000
```

`travel_ms`, config'teki `forward_ms`/`backward_ms` yani **tam hız** strok süresi.

| duty | `forward_ms = 6000` için `move_ms` |
|---|---|
| 100 | 6000 |
| 50 | 12000 |
| 25 | 24000 |
| 10 | 60000 (kırpıldı: `move_ms_clamped = 1`) |

### Yeni `BTM_Status` alanları

```c
uint32_t move_ms;           /* bu hareket için PLANLANAN toplam süre */
uint8_t  move_ms_estimated; /* 1: duty < 100, süre doğrusal modelle tahmin edildi */
uint8_t  move_ms_clamped;   /* 1: BTM_MAX_MOVE_MS'e kırpıldı, strok tamamlanmayabilir */
```

`BTM_RemainingMs()` de ölçeklenmiş süreye göre sayar, yani sahip görev `rem` kadar
uyuduğunda hareketin gerçek sonunda uyanır.

### Dürüstlük notu: bu bir tahmindir

Model: ortalama köprü gerilimi duty ile doğrusal → fırça DC motorun boştaki hızı
gerilimle doğrusal → süre ~ 1/duty. **Modele girmeyenler:** aktüatörün ölü bölgesi
(çok düşük duty'de hiç kalkmaması), yük, sürtünme, besleme sarkması. Bu yüzden
sürücü ölçekleme yaptığını `move_ms_estimated` ile bildiriyor ve README §8'e
"düşük duty'de gerçek strok süresini ölç" maddesi eklendi.

Sapma yönü güvenli: `-S` varyantında fazla süre zararsızdır (strok sonunda limit
switch motor devresini açar), eksik süre ise strokun yarıda kalmasıdır.

### Yeni ayar: `BTM_MAX_MOVE_MS` (varsayılan 60000, aralık 100..600000)

İki iş yapıyor:

1. **Config üst sınırı.** `BTM_Init`, `forward_ms`/`backward_ms` bu değerin
   üstündeyse `HAL_ERROR` döner. Böylece `plan_move_ms()` içindeki `travel * 100`
   çarpımının taşmadığı ispatlanabilir (en çok 6e7 < 4.29e9).
2. **Ölçekleme kırpma noktası.** duty %1'de süre 100 kat olurdu; kırpılır ve
   `move_ms_clamped` ile bildirilir.

---

## 3. DAVRANIŞ DEĞİŞİKLİKLERİ — v2'den taşırken

Bunlar sessiz değişiklikler değil, bilerek yapılmış davranış değişiklikleridir.

### 3.1 `BTM_MovePwm` artık daha uzun sürüyor

**Etkisi:** `BTM_MovePwm(dir, 50)` v2'de `forward_ms` kadar sürüyordu, v3'te iki
katı. `BTM_RemainingMs()` de bunu yansıtır.

**Yapılacak:** uygulama bu eksikliği telafi etmek için süreyi kendi büyütüyorsa
(ör. `forward_ms`'i duty'ye göre elle hesaplayıp config'e yazmak, veya düşük duty
için ayrı bir config tutmak) **o telafiyi kaldır** — yoksa çift ölçekleme olur ve
hareket gereğinden çok uzun sürer. `-S` varyantında bu tehlikeli değil ama
gereksiz ısınma ve bekleme üretir.

**Bilinçli kabul edilmesi gereken ödünç — `forward_ms`'in ROLÜ değişti.** v2'de o
değer bir **emniyet zaman aşımı**ydı: ne olursa olsun motor en çok `forward_ms`
sürüyordu. v3'te bir **mesafe tahmini**ne dönüştü ve azami sürüş süresi duty ile
büyüyor (tavan: `BTM_MAX_MOVE_MS`, varsayılan 60 s). Yani strok ortasında bir
sıkışmada duty %10 ile motor 6 saniye değil 60 saniye komut alır.

Gerçek koruma hâlâ yerinde: sıkışma aşırı akım üretir, status baytı **her periyotta**
okunur ve `OCA`/`OCB` anında `FAULT_DEVICE` yapar; strok sonunda `-S` limit switch'i
devreyi açar. Yine de sıkı bir tavan istiyorsan `BTM_MAX_MOVE_MS`'i tam hız strok
süresinin 2–3 katına çek:

```
-DBTM_MAX_MOVE_MS=15000     /* 6 s tam hız strok için makul bir tavan */
```

Bu durumda düşük duty'de `move_ms_clamped = 1` görürsün — "strok tamamlanmayabilir"
demektir ve bilinçli bir seçimdir.

### 3.2 `BTM_Init` bazı eski config'leri reddeder

**Etkisi:** `forward_ms` veya `backward_ms` > 60000 ise `HAL_ERROR` (fault kaydı
yok, yalnız config reddi).

**Yapılacak:** süreler 60 saniyeyi aşıyorsa ya değerleri düşür ya
`-DBTM_MAX_MOVE_MS=...` ile sınırı büyüt.

### 3.3 `BTM_Status` yapısı büyüdü

**Etkisi:** 3 yeni alan **sonuna** eklendi; mevcut alanların adı, tipi ve sırası
değişmedi. Kaynak uyumlu, ama `sizeof(BTM_Status)` değişti.

**Yapılacak:** sürücüyü ve onu kullanan tüm birimleri **yeniden derle**. Durumu
UART paketine kopyalayan kod sabit bir paket boyutu varsayıyorsa gözden geçir.

### 3.4 "Fault = kalıcı" varsayımı artık doğru değil

**Etkisi:** v2'de `FAULT` durumundan çıkışın tek yolu reset'ti; v3'te
`BTM_ClearFault()` var.

**Yapılacak:** uygulama fault görünce `NVIC_SystemReset()` çağırıyorsa veya kalıcı
bir "arızalı" bayrağı kaldırıyorsa, bunun yerine sınırlı denemeli kurtarma
düşünülebilir (README §6.4). Zorunlu değil: hiç `BTM_ClearFault()` çağırmayan bir
uygulama v2 gibi davranmaya devam eder.

### 3.5 Geriye uyumlu olanlar

- `BTM_Move()` (tam hız) davranışı **birebir aynı**: duty 100'de ölçekleme yok.
- `BTM_Brake()`, `BTM_Init()` (geçerli config ile), `BTM_GetStatus()` imzaları ve
  semantiği aynı.
- Callback yönlendirmeleri aynı (dört tane, yenisi yok).
- CubeMX ayarları, PSC/ARR, SPI modu, NVIC öncelikleri **aynı**.
- Kesme maskeleme hâlâ yok; ISR'larda hâlâ meşgul bekleme yok (`BTM_ClearFault`
  görev bağlamı fonksiyonudur, ISR'dan çağrılırsa `HAL_BUSY` döner).

---

## 4. Dosya dosya değişiklikler

### `BTM9011EP.h`

- `BTM_MAX_MOVE_MS` ayarı + `#error` aralık kontrolü.
- `BTM_Config.forward_ms`/`backward_ms` yorumları: "TAM HIZ süresi" ve üst sınır.
- `BTM_Status`: `move_ms`, `move_ms_estimated`, `move_ms_clamped`;
  `dropped_frames`'e "ClearFault sıfırlamaz" notu.
- `BTM_MovePwm` doküman bloğu: ölçekleme, tahmin uyarısı, kırpma.
- `BTM_ClearFault()` bildirimi + dönüş sözleşmesi.
- Başlıktaki kural listesi: "İlk hata MCU reset'ine kadar kilitlidir" →
  "çıkmak için `BTM_ClearFault()` (FAULT_DWT hariç) veya reset".

### `BTM9011EP.c`

- Dosya başı yorumu: duty ölçeklemesi ve temizlenebilir kilit.
- Yeni statikler: `move_ms`, `move_ms_estimated`, `move_ms_clamped`.
- Yeni yardımcı: `plan_move_ms()` (ölçekleme + kırpma + bayraklar, sıfıra bölme
  kalkanı, taşma gerekçesi yorumda).
- `BTM_Init`: strok sürelerinin `BTM_MAX_MOVE_MS` kontrolü.
- `start_move`: `move_ms` üzerinden `move_deadline` ve `periods_left`.
- Yeni fonksiyon: `BTM_ClearFault()`.
- `BTM_GetStatus`: üç yeni alanın kopyalanması.

### `tests/test_btm.c`

- `reset_all()`: yeni statikler sıfırlanıyor.
- Grup 6 güncellendi: duty %50'de periyot sayısının iki katı olduğu artık
  **açıkça** doğrulanıyor (`periods_left == 4`) ve hareket 4 periyotta bitiyor.
  *Bu, v2 testinin tek kırılan yeri ve kırılması beklenen yerdi.*
- Taklit HAL'e `busy_on_gettick` kancası: hattın **çerçeve yolunda** meşgulleşmesini
  kurgulayabilmek için. Bu olmadan `BTM_ClearFault`'un geri alma dalına hiçbir
  testten ulaşılamıyordu (ilk bekleme her zaman önce dönüyordu).
- Yeni gruplar 21–30: ClearFault'un beş yolu (başarı, sebep sürüyor, DWT, hata yok,
  meşgul hat — hem ilk beklemede hem çerçeve yolunda), iç sayaçların birebir geri
  konması, duty ölçeklemesi, kırpma, config sınırı, ölçeklenmiş sürenin sonunda
  kendiliğinden fren, "kabloyu düzelttim" kurtarma akışı.

### `README.md` / `ENTEGRASYON.md`

- README: §3.1 (süre ölçeklemesi), §3.2 (hata ve kilidin açılması), §6.4 (kurtarma
  politikası örneği), §2 bekleme tablosuna `ClearFault` satırı, §7 ayar tablosuna
  `BTM_MAX_MOVE_MS`, §8'e duty ölçeklemesini ölçme maddesi, §9 sınırların
  güncellenmesi (ısıl bütçenin `move_ms` üzerinden kurulması ve `-S` olmayan
  aktüatörde uzayan stall penceresi dahil), §10 test sayısı 30/155, kırık spec
  bağlantısının düzeltilmesi, H7 doğrulama tablosuna "v3 için yeniden koşturulmadı"
  notu.
- ENTEGRASYON: Aşama 4'te otomatik ölçekleme ve kontrol edilecekler, yeni Aşama 4b
  (kilidi ve çıkışını bilerek sınama), sorun giderme tablosuna 4 yeni satır.

---

## 5. Doğrulama

```
$ sh tests/run_tests.sh
TUM TESTLER GECTI (fails=0)        30 grup / 155 doğrulama
```

Ek olarak `-Wall -Wextra -Wconversion -Wsign-conversion -O2` ile **0 uyarı**
(host derleyicisi; `-Wshadow`'da çıkan tek uyarı testin kendi `hspi`/`htim`
globalleriyle parametre adlarının çakışmasıdır, sürücüye ait değildir).

**Kartta ölçülmemiş olanlar** (v2'den devrolan + yenisi):

- `tlead` (CS yükselişi → ilk SCK ≥ 250 ns) — sürücüdeki tek doğrulanmamış varsayım.
- `BTM_FRAME_US` gerçek çerçeve süresi.
- Yön etiketlerinin mekanik karşılığı.
- **YENİ:** duty ölçeklemesinin gerçekliği — aktüatörün ölü bölgesi ve yük
  altındaki hızı (README §8, madde 5).
