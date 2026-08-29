# Jetson ROS 2 USB köprüsü

Arduino Mega her 100 ms'de USB seri hattan JSON Lines paketi yollar. Bu köprü bunları aşağıdaki ROS 2 arayüzlerine dönüştürür:

| ROS adı | Tür | Kaynak / amaç |
|---|---|---|
| `/imu/data_raw` | `sensor_msgs/Imu` | MPU6050 ivme ve açısal hız (SI birimleri) |
| `/wheel/encoders` | `std_msgs/Int64MultiArray` | `[sol_tick, sağ_tick]` ham sayacı |
| `/odom` | `nav_msgs/Odometry` | Diferansiyel sürüş tekerlek odometrisi |
| `odom -> base_link` | TF | `slam_toolbox` için zorunlu dönüşüm |

`slam_toolbox` yalnızca IMU/encoder ile harita çıkarmaz: bir LiDAR düğümünün ayrıca `/scan` (`sensor_msgs/LaserScan`) yayımlaması gerekir. Köprüdeki `odom -> base_link` TF'si ve LiDAR sürücüsündeki `base_link -> laser_frame` statik TF'si hazır olmalıdır.

## Jetson kurulumu ve çalıştırma

Bu dosyayı ROS 2 Python paketinin içine koyabilir veya doğrudan kaynaklanmış ROS ortamında çalıştırabilirsiniz:

```bash
sudo apt install ros-${ROS_DISTRO}-tf2-ros ros-${ROS_DISTRO}-sensor-msgs ros-${ROS_DISTRO}-nav-msgs python3-serial
source /opt/ros/${ROS_DISTRO}/setup.bash
python3 serial_sensor_bridge.py --ros-args -p port:=/dev/ttyACM0 -p wheel_radius_m:=0.050 -p wheel_separation_m:=0.300 -p ticks_per_revolution:=600.0
```

`wheel_radius_m`, `wheel_separation_m` ve özellikle `ticks_per_revolution` gerçek mekanik değerlerle değiştirilmelidir. Robot ileri giderken `/odom` geriye gidiyorsa ilgili `left_encoder_sign` veya `right_encoder_sign` parametresini `-1.0` yapın.

IMU'nun fiziksel yönü ROS eksenleriyle eşleşmelidir: `x` ileri, `y` sol, `z` yukarı. Eşleşmiyorsa paketi yayımlamadan önce eksen dönüşümü yapılmalıdır. IMU'nun yönelim alanı bilinçli olarak boş bırakılır; `robot_localization` ile `/imu/data_raw` ve `/odom` birleştirilerek daha iyi bir `/odometry/filtered` üretilebilir.

Örnek `slam_toolbox` çerçeve ayarları:

```yaml
slam_toolbox:
  ros__parameters:
    odom_frame: odom
    base_frame: base_link
    map_frame: map
    scan_topic: /scan
```
