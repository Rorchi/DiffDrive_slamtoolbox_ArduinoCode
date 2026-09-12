#!/usr/bin/env python3
"""USB-serial Arduino Mega -> ROS 2 IMU, wheel ticks, odometry and odom TF bridge."""
import json
import math

import rclpy
import serial
from geometry_msgs.msg import TransformStamped
from nav_msgs.msg import Odometry
from rclpy.node import Node
from sensor_msgs.msg import BatteryState, Imu
from std_msgs.msg import Bool, Float32, Int64MultiArray
from tf2_ros import TransformBroadcaster


def yaw_quaternion(yaw):
    return 0.0, 0.0, math.sin(yaw / 2.0), math.cos(yaw / 2.0)


class SerialSensorBridge(Node):
    def __init__(self):
        super().__init__('serial_sensor_bridge')
        self.declare_parameter('port', '/dev/ttyACM0')
        self.declare_parameter('baudrate', 115200)
        self.declare_parameter('wheel_radius_m', 0.05)
        self.declare_parameter('wheel_separation_m', 0.30)
        self.declare_parameter('ticks_per_revolution', 600.0)
        self.declare_parameter('left_encoder_sign', 1.0)
        self.declare_parameter('right_encoder_sign', 1.0)
        self.declare_parameter('odom_frame', 'odom')
        self.declare_parameter('base_frame', 'base_link')
        self.declare_parameter('imu_frame', 'imu_link')
        self.declare_parameter('battery_frame', 'battery_link')

        p = self.get_parameter
        self.radius = float(p('wheel_radius_m').value)
        self.separation = float(p('wheel_separation_m').value)
        self.ticks_per_rev = float(p('ticks_per_revolution').value)
        self.left_sign = float(p('left_encoder_sign').value)
        self.right_sign = float(p('right_encoder_sign').value)
        self.odom_frame = p('odom_frame').value
        self.base_frame = p('base_frame').value
        self.imu_frame = p('imu_frame').value
        self.battery_frame = p('battery_frame').value
        self.serial = serial.Serial(p('port').value, int(p('baudrate').value), timeout=0.2)

        self.imu_pub = self.create_publisher(Imu, '/imu/data_raw', 20)
        self.encoder_pub = self.create_publisher(Int64MultiArray, '/wheel/encoders', 20)
        self.odom_pub = self.create_publisher(Odometry, '/odom', 20)
        self.battery_pub = self.create_publisher(BatteryState, '/battery', 10)
        self.battery_power_pub = self.create_publisher(Float32, '/battery/power', 10)
        self.battery_time_pub = self.create_publisher(Float32, '/battery/remaining_minutes', 10)
        self.low_battery_pub = self.create_publisher(Bool, '/battery/low', 10)
        self.tf_broadcaster = TransformBroadcaster(self)
        self.last_enc = None
        self.last_stamp = None
        self.x = self.y = self.yaw = 0.0
        self.create_timer(0.002, self.read_serial)  # Mega 10 Hz yayini icin yeterli.
        self.get_logger().info(f"Listening on {p('port').value} at {p('baudrate').value} baud")

    def read_serial(self):
        try:
            line = self.serial.readline().decode('utf-8', errors='replace').strip()
            if line:
                self.handle_packet(json.loads(line))
        except json.JSONDecodeError:
            self.get_logger().warn('Invalid serial packet ignored', throttle_duration_sec=5.0)
        except serial.SerialException as exc:
            self.get_logger().error(f'Serial error: {exc}', throttle_duration_sec=5.0)

    def handle_packet(self, data):
        required = ('enc_l', 'enc_r')
        if not all(k in data for k in required):
            self.get_logger().warn('Incomplete serial packet ignored', throttle_duration_sec=5.0)
            return
        stamp = self.get_clock().now().to_msg()
        if data.get('imu_ok', True):
            self.publish_imu(data, stamp)
        self.publish_odometry(data, stamp)
        self.publish_battery(data, stamp)

    def publish_imu(self, d, stamp):
        msg = Imu()
        msg.header.stamp = stamp
        msg.header.frame_id = self.imu_frame
        # MPU6050 has no absolute orientation estimate; -1 means "unavailable".
        msg.orientation_covariance[0] = -1.0
        msg.angular_velocity.x, msg.angular_velocity.y, msg.angular_velocity.z = float(d['gx']), float(d['gy']), float(d['gz'])
        msg.linear_acceleration.x, msg.linear_acceleration.y, msg.linear_acceleration.z = float(d['ax']), float(d['ay']), float(d['az'])
        self.imu_pub.publish(msg)

    def publish_odometry(self, d, stamp):
        left, right = int(d['enc_l']), int(d['enc_r'])
        ticks = Int64MultiArray(data=[left, right])
        self.encoder_pub.publish(ticks)
        now = self.get_clock().now()
        if self.last_enc is None:
            self.last_enc, self.last_stamp = (left, right), now
            return
        dt = (now - self.last_stamp).nanoseconds * 1e-9
        if dt <= 0.0:
            return
        meters_per_tick = (2.0 * math.pi * self.radius) / self.ticks_per_rev
        dl = (left - self.last_enc[0]) * meters_per_tick * self.left_sign
        dr = (right - self.last_enc[1]) * meters_per_tick * self.right_sign
        ds, dtheta = (dl + dr) / 2.0, (dr - dl) / self.separation
        self.x += ds * math.cos(self.yaw + dtheta / 2.0)
        self.y += ds * math.sin(self.yaw + dtheta / 2.0)
        self.yaw = math.atan2(math.sin(self.yaw + dtheta), math.cos(self.yaw + dtheta))
        self.last_enc, self.last_stamp = (left, right), now

        qx, qy, qz, qw = yaw_quaternion(self.yaw)
        odom = Odometry()
        odom.header.stamp, odom.header.frame_id, odom.child_frame_id = stamp, self.odom_frame, self.base_frame
        odom.pose.pose.position.x, odom.pose.pose.position.y = self.x, self.y
        odom.pose.pose.orientation.x, odom.pose.pose.orientation.y = qx, qy
        odom.pose.pose.orientation.z, odom.pose.pose.orientation.w = qz, qw
        odom.twist.twist.linear.x, odom.twist.twist.angular.z = ds / dt, dtheta / dt
        self.odom_pub.publish(odom)

        tf = TransformStamped()
        tf.header.stamp, tf.header.frame_id, tf.child_frame_id = stamp, self.odom_frame, self.base_frame
        tf.transform.translation.x, tf.transform.translation.y = self.x, self.y
        tf.transform.rotation.x, tf.transform.rotation.y, tf.transform.rotation.z, tf.transform.rotation.w = qx, qy, qz, qw
        self.tf_broadcaster.sendTransform(tf)

    def publish_battery(self, d, stamp):
        msg = BatteryState()
        msg.header.stamp = stamp
        msg.header.frame_id = self.battery_frame
        msg.design_capacity = float(d.get('capacity', 3.3))
        msg.temperature = math.nan
        msg.cell_voltage = [math.nan] * 4
        msg.power_supply_technology = BatteryState.POWER_SUPPLY_TECHNOLOGY_LIPO
        msg.location = 'main_battery'
        msg.serial_number = ''

        fields = ('voltage', 'current', 'power', 'charge', 'capacity', 'percentage')
        battery_ok = d.get('battery_ok', False) is True and all(key in d for key in fields)
        power_msg = Float32()
        remaining_time_msg = Float32()
        low_battery_msg = Bool()

        if not battery_ok:
            msg.voltage = math.nan
            msg.current = math.nan
            msg.charge = math.nan
            msg.capacity = math.nan
            msg.percentage = math.nan
            msg.power_supply_status = BatteryState.POWER_SUPPLY_STATUS_UNKNOWN
            msg.power_supply_health = BatteryState.POWER_SUPPLY_HEALTH_UNKNOWN
            msg.present = False
            power_msg.data = math.nan
            remaining_time_msg.data = math.nan
            low_battery_msg.data = False
        else:
            msg.voltage = float(d['voltage'])
            msg.current = float(d['current'])
            msg.charge = float(d['charge'])
            msg.capacity = float(d['capacity'])
            msg.percentage = max(0.0, min(1.0, float(d['percentage'])))
            msg.power_supply_health = BatteryState.POWER_SUPPLY_HEALTH_GOOD
            msg.present = True
            power_msg.data = float(d['power'])
            remaining_minutes = d.get('remaining_minutes')
            remaining_time_msg.data = (
                float(remaining_minutes) if remaining_minutes is not None else math.nan
            )
            low_battery_msg.data = d.get('low_battery', False) is True

            if msg.current < -0.05:
                msg.power_supply_status = BatteryState.POWER_SUPPLY_STATUS_DISCHARGING
            elif msg.current > 0.05:
                msg.power_supply_status = BatteryState.POWER_SUPPLY_STATUS_CHARGING
            elif msg.percentage >= 0.99:
                msg.power_supply_status = BatteryState.POWER_SUPPLY_STATUS_FULL
            else:
                msg.power_supply_status = BatteryState.POWER_SUPPLY_STATUS_NOT_CHARGING

        self.battery_pub.publish(msg)
        self.battery_power_pub.publish(power_msg)
        self.battery_time_pub.publish(remaining_time_msg)
        self.low_battery_pub.publish(low_battery_msg)

def main():
    rclpy.init()
    node = SerialSensorBridge()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.serial.close()
        node.destroy_node()
        rclpy.shutdown()


if __name__ == '__main__':
    main()
