# Kâşif Çelebi Batarya İzleme ve Uyarı Sistemi Raporu

**Tarih:** 12 Eylül 2026  
**Denetleyici:** Arduino Mega 2560  
**Üst bilgisayar:** NVIDIA Jetson / ROS 2 Humble  
**Batarya:** 4S 3300 mAh 40C LiPo  
**Ölçüm sensörü:** INA219, 32 V / 2 A kalibrasyonu

## 1. Amaç ve kapsam

Bu eklenti, robotun ana bataryasının gerilimini ve tüm robot tarafından çekilen
akımı ölçmek, yüksek doğruluklu bir doluluk tahmini oluşturmak, tahmini kalan
çalışma süresini hesaplamak ve kullanıcıyı LED ile buzzer üzerinden uyarmak için
hazırlanmıştır.

Arduino, güvenlik açısından Jetson'dan bağımsız çalışır. Pil ölçümü, LED seçimi
ve kritik sesli uyarı Arduino üzerinde yürütülür. Jetson, Arduino'nun seri JSON
çıktısını ROS 2 mesajlarına dönüştürür.

```text
4S LiPo ── INA219 ── Robotun tüm güç yükleri
              │
              └── I²C ── Arduino Mega
                              │
                              ├── D22: buzzer
                              ├── D23: kırmızı LED
                              ├── D24: sarı LED
                              ├── D25: yeşil LED
                              │
                              └── USB / JSON ── Jetson ── ROS 2 topic'leri
```

## 2. Fiziksel bağlantılar

### 2.1 INA219 bağlantısı

INA219, robotun bütün tüketimini görebilmesi için ana pozitif hatta seri
bağlanır:

```text
Pil (+) ── INA219 VIN+
                    VIN- ── Robot güç dağıtımı / regülatör girişleri (+)

Pil (-) ────────────────── Robot GND
   └────────────────────── Arduino GND
   └────────────────────── INA219 GND
```

| INA219 pini | Arduino Mega / sistem bağlantısı |
|---|---|
| `VCC` | Modülün desteklediği besleme; mevcut sistemde 5 V |
| `GND` | Ortak GND |
| `SDA` | Mega pin 20 / SDA |
| `SCL` | Mega pin 21 / SCL |
| `VIN+` | 4S LiPo pozitif uç |
| `VIN-` | Robotun ana pozitif güç girişi |

INA219 adresi varsayılan olarak `0x40` değerindedir. MPU6050 ile aynı I²C
hattını paylaşır fakat adresleri farklıdır.

### 2.2 LED bağlantıları

| İşlev | Mega pini | Bağlantı |
|---|---:|---|
| Kırmızı LED | D23 | D23 → 220–330 Ω → LED anot; LED katot → GND |
| Sarı LED | D24 | D24 → 220–330 Ω → LED anot; LED katot → GND |
| Yeşil LED | D25 | D25 → 220–330 Ω → LED anot; LED katot → GND |

Her LED için ayrı seri direnç kullanılmalıdır. Yazılım aynı anda yalnızca bir
pil durum rengini etkinleştirir.

### 2.3 Buzzer bağlantısı

Kullanılan üç pinli buzzer modülü LOW seviyede tetiklenmektedir:

| Buzzer pini | Bağlantı |
|---|---|
| `S`, `SIG` veya `I/O` | Mega D22 |
| `+` veya `VCC` | 5 V |
| `-` veya `GND` | Ortak GND |

D22 `HIGH` iken buzzer kapalı, `LOW` iken açıktır. Modül sabit tonlu aktif
buzzer davranışı gösterdiği için farklı nota frekansları yerine ritmik bildirim
deseni kullanılır.

> 4S LiPo'nun 12–16,8 V çıkışı Arduino'nun 5 V pinine veya Jetson'a doğrudan
> uygulanmamalıdır. Her cihaz uygun gerilim ve akım kapasitesine sahip bir
> regülatör üzerinden beslenmelidir.

## 3. Batarya ölçüm ve yüzde hesabı

Sistem aşağıdaki batarya tanımını kullanır:

| Parametre | Değer |
|---|---:|
| Hücre sayısı | 4S |
| Nominal kapasite | 3,3 Ah / 3300 mAh |
| Tam dolu hücre gerilimi | 4,20 V |
| Alt hücre gerilimi | 3,00 V |
| Tam dolu paket gerilimi | 16,80 V |
| Alt paket gerilimi | 12,00 V |
| Ölçülen en yüksek sistem akımı | Yaklaşık 1,5 A |
| INA219 kalibrasyon aralığı | 32 V / 2 A |

Yüzde hesabı yalnızca gerilimden doğrusal olarak yapılmaz. Aşağıdaki yöntemler
birlikte kullanılır:

1. Arduino açıldığında paket gerilimi, LiPo açık-devre gerilim (OCV) tablosunda
   hücre başına değerlendirilerek başlangıç yüzdesi bulunur.
2. Çalışma sırasında INA219 akımı zamanla trapez integrasyonu kullanılarak
   toplanır. Bu yöntemle tüketilen Ah miktarı hesaplanır (coulomb counting).
3. Gerilim ve akım ölçümlerine EMA filtresi uygulanarak motor kaynaklı kısa
   sıçramaların göstergeyi oynatması engellenir.
4. Akım 0,08 A veya altında 30 saniye kaldığında pil dinlenmiş kabul edilir ve
   coulomb sayacı OCV tahminine doğru çok yavaş düzeltilir.
5. Kalan Ah ve yüzde, Arduino EEPROM'unda 16 döner kayıt yuvasına kaydedilir.
   Kayıt en erken iki dakikada bir ve yüzde en az 1 değiştiğinde yapılır.
6. EEPROM verisi CRC ile doğrulanır. Böylece yeniden başlatma sonrasında pil
   sayacının korunması ve bozuk kayıtların reddedilmesi sağlanır.

INA219 gerilim ve akım kalibrasyon katsayıları yazılımda ayrı tanımlanmıştır.
Mutlak ölçüm doğruluğu istenirse bu katsayılar referans multimetre ve ampermetre
ile karşılaştırılarak ayarlanabilir.

## 4. LED davranışı

| Tahmini doluluk | Gösterge |
|---:|---|
| %80–100 | Yeşil LED; her 3 saniyede bir 300 ms yanar |
| %50–79 | Sarı LED sürekli yanar |
| %0–49 | Kırmızı LED sürekli yanar |

Geçiş noktalarında yüzde 2 histerezis vardır. Örneğin küçük ölçüm değişimleri
nedeniyle yüzde 80 çevresinde yeşil ve sarı LED'in sürekli yer değiştirmesi
önlenir. INA219 algılanmazsa kırmızı LED 500 ms aralıklarla yanıp söner ve buzzer
kapalı tutulur.

## 5. Kalan süre ve buzzer alarmı

Kalan süre, son 60 saniyeyi temsil eden filtrelenmiş ortalama deşarj akımından
hesaplanır:

```text
Kalan dakika = (kalan kapasite Ah / ortalama deşarj akımı A) × 60
```

İlk 10 saniyede yeterli akım geçmişi olmadığı için kalan süre `null` olarak
gönderilir. Ortalama akım 0,05 A altında olduğunda güvenilir süre tahmini
yapılmaz.

Alarm aşağıdaki koşullardan biri gerçekleştiğinde açılır:

- Tahmini kalan süre 5 dakika veya altında 10 saniye boyunca kalır.
- Paket gerilimi 12,00 V veya altında 2 saniye boyunca kalır.
- Hesaplanan doluluk yüzde 3 veya altına iner.

Alarm açıldığında buzzer 3 saniyelik ritmik bildirim verir. Bildirimin başlangıcı
20 saniyede bir tekrarlanır; her ritimden sonra 17 saniye sessizlik vardır.
Kalan süre 6 dakikanın, SoC yüzde 5'in ve paket gerilimi 12,40 V'un üzerine
çıktığında alarm bırakılır.

## 6. Arduino–Jetson seri protokolü

Arduino ROS 2 topic'i doğrudan yayımlamaz. Her 100 ms'de USB seri portuna tek
satırlık JSON veri gönderir. Batarya ile ilgili alanlar şunlardır:

```json
{
  "battery_ok": true,
  "voltage": 16.638,
  "current": -0.504,
  "power": -8.389,
  "charge": 3.1420,
  "capacity": 3.30,
  "percentage": 0.9521,
  "cell_voltage_avg": 4.159,
  "average_discharge_current": 0.488,
  "remaining_minutes": 386.4,
  "battery_confidence": 0.466,
  "low_battery": false
}
```

`current` ve `power`, ROS `BatteryState` işaret kuralına uygun olarak deşarj
sırasında negatiftir. `average_discharge_current` ise süre hesabını kolaylaştırmak
için pozitif büyüklük olarak gönderilir. Yüzde değeri `0.0–1.0` aralığındadır.

## 7. Yayımlanan ROS 2 topic'leri

Jetson'daki `serial_bridge.py`, Arduino JSON verilerini aşağıdaki ROS 2
arayüzlerine dönüştürür:

| Topic / TF | Mesaj tipi | İçerik |
|---|---|---|
| `/imu/data_raw` | `sensor_msgs/msg/Imu` | MPU6050 ivme ve açısal hız verisi |
| `/wheel/encoders` | `std_msgs/msg/Int64MultiArray` | Sol ve sağ encoder tick sayıları |
| `/odom` | `nav_msgs/msg/Odometry` | Diferansiyel sürüş odometrisi |
| `/tf` | TF2 | `odom → base_link` dönüşümü |
| `/battery` | `sensor_msgs/msg/BatteryState` | Gerilim, akım, kalan/yüklü kapasite, yüzde ve durum |
| `/battery/power` | `std_msgs/msg/Float32` | Anlık güç; deşarj sırasında negatif |
| `/battery/remaining_minutes` | `std_msgs/msg/Float32` | Tahmini kalan çalışma süresi |
| `/battery/low` | `std_msgs/msg/Bool` | Arduino tarafından üretilen düşük pil alarmı |

`/battery` mesajında `design_capacity` değeri Arduino'dan gelen `3.3 Ah`
bilgisinden alınır. INA219 yalnız toplam paket gerilimini ölçtüğü için gerçek tek
hücre gerilimleri bilinmez ve `cell_voltage` alanları `NaN` bırakılır.

LED ve buzzer için ayrı ROS topic'i yoktur. Bunlar Jetson veya ROS bağlantısı
olmasa bile Arduino tarafından yerel olarak kontrol edilir.

Topic'ler Jetson üzerinde şu komutlarla kontrol edilebilir:

```bash
ros2 topic echo /battery
ros2 topic echo /battery/power
ros2 topic echo /battery/remaining_minutes
ros2 topic echo /battery/low
ros2 topic hz /battery
```

## 8. Doğrulama sonucu

Firmware PlatformIO ile Arduino Mega 2560 hedefi için başarıyla derlenmiş ve
`/dev/ttyUSB0` üzerindeki karta yüklenmiştir. Son doğrulamada INA219 ve MPU6050
algılanmış, yeni JSON alanları seri portta görülmüş ve `low_battery` durumu
`false` olarak doğrulanmıştır.

Örnek doğrulama ölçümü:

| Ölçüm | Değer |
|---|---:|
| Paket gerilimi | 16,638 V |
| Sistem akımı | Yaklaşık 0,504 A deşarj |
| Hesaplanan SoC | %95,21 |
| Tahmini kalan süre | 386,4 dakika |
| Düşük pil alarmı | Kapalı |

## 9. Sınırlamalar ve güvenlik notları

- INA219 yalnız toplam paket gerilimini ölçer. Tek bir hücrenin 3,00 V altına
  indiğini paket toplamından kesin olarak tespit edemez. Hücre bazlı koruma için
  uygun 4S BMS veya balans soketinden hücre ölçümü gereklidir.
- INA219 2 A ölçüm aralığı robotta görülen 1,5 A için yeterlidir; stall veya arıza
  akımı 2 A'ı aşarsa ölçüm doygunlaşabilir ve standart modülün şöntü zorlanabilir.
- Yüzde hesabının gerçek doğruluğu pilin yaşı, sıcaklığı, gerçek kapasitesi ve
  INA219 kalibrasyonuna bağlıdır.
- EEPROM kaydı yeniden başlatma kaybını azaltır; farklı dolulukta yeni bir pil
  takıldığında başlangıç OCV kontrolü kayıtla büyük uyuşmazlığı reddeder.
- 12,00 V paket sınırı hücre başına ortalama 3,00 V anlamına gelir. Hücreler
  dengesizse bir hücre bu ortalamadan daha düşük olabilir; BMS kullanılmalıdır.
