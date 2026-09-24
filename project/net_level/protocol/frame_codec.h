#pragma once
#ifndef _MYDB_NET_FRAME_CODEC_H_
#define _MYDB_NET_FRAME_CODEC_H_

/*
    模块名：网络帧编解码器
    功能描述：增量解析 TCP 字节流中的固定头、TLV body 和完整帧，并将响应对象编码为可发送字节。
*/

#include "protocol.h"
#include <cstddef>
#include <string>
#include <vector>

namespace mydb::net {

/*
    类型名：DecodeStatus
    功能：表示增量帧解析器本次处理的结果。
*/
enum class DecodeStatus : std::uint8_t {
    NEED_MORE,                                   // 数据不足：等待下一段 TCP 字节
    FRAME_READY,                                 // 帧完成：frame 已填充一个完整帧
    INVALID_FRAME,                               // 帧非法：格式、版本或字段校验失败
    FRAME_TOO_LARGE                              // 帧过大：声明长度超过配置上限
};

/*
    类名：FrameCodec
    功能：处理 TCP 字节流和协议 Frame 之间的转换。
        - 保存半包数据并支持拆包、粘包。
        - 校验固定头和 Body 长度。
        - 只处理协议格式，不解释业务命令。
    友元类：无
*/
class FrameCodec {
private:
    std::uint32_t maxFrameBytes;                 // 帧上限：限制单帧和相关内存分配的最大值
    ByteBuffer inputBuffer;                      // 输入缓冲：保存尚未组成完整帧的字节

public:
    /*
        函数：FrameCodec
        参数：maxFrameBytes：单帧允许的最大字节数
        功能：创建帧编解码器并设置长度上限
        返回：无
    */
    explicit FrameCodec(std::uint32_t maxFrameBytes = DEFAULT_MAX_FRAME_BYTES);

    /*
        函数：feed
        参数：data：新收到的字节；frame：解析成功后写入的完整帧
        功能：向增量解析器追加字节，并尽可能解析一个完整帧
        返回：DecodeStatus，表示需要更多数据、解析成功或协议错误
    */
    DecodeStatus feed(const ByteBuffer& data, Frame& frame);

    /*
        函数：encode
        参数：frame：需要发送的完整帧
        功能：将协议帧编码为网络字节序的连续字节
        返回：编码后的字节；编码失败时返回空结果
    */
    ByteBuffer encode(const Frame& frame) const;

    /*
        函数：reset
        参数：无
        功能：清空尚未组成完整帧的输入数据
        返回：无
    */
    void reset();
};

}

#endif
