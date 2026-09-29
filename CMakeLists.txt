# 凿物平台 · 摄像头端固件（Seeed Studio XIAO ESP32S3 Sense）
#
# 链路：摄像头 → 平台注册（ed25519 自证）→ 平台回 edge_ip:ingest_port → RTSP 推流到边缘服务器
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(zaowu_edge-camera_XIAO-ESP32-S3)
