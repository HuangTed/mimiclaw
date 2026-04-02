#pragma once

/**
 * @file voice_websocket.h
 * @brief Voice WebSocket 模块 - 通过 WebSocket 发送 I2S 音频数据
 * 
 * 该模块负责：
 * - 从 INMP441 麦克风通过 I2S 采集音频数据
 * - 通过 WebSocket 将音频流发送到服务器
 * - 接收服务器返回的语音识别结果
 */

/**
 * @brief 启动语音 WebSocket 系统
 * 
 * 初始化并启动：
 * - I2S 音频采集（16kHz, 16bit, 单声道）
 * - WebSocket 客户端连接
 * - 音频采集任务和发送任务
 * 
 * @note 该函数应在系统初始化时调用一次
 */
void voice_start(void);