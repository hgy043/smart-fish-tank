# ============================================================================
# 智能鱼缸 AI 生态监测系统 — 顶层工程文件
#
# 目标芯片 : ESP32-S3-WROOM-1-N16R8 (16MB Flash / 8MB Octal PSRAM)
# 开发板   : 粤嵌 GEC-ESP32-S3-小智开发板 V1.0.1
# ESP-IDF  : v5.5.3  (已验证版本，与参考工程一致)
# ============================================================================
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(smart_fish_tank)
