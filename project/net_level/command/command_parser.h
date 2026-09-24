#pragma once
#ifndef _MYDB_NET_COMMAND_PARSER_H_
#define _MYDB_NET_COMMAND_PARSER_H_

/*
    模块名：命令解析模块
    功能描述：将协议层完整 Frame 校验并转换为命令请求，负责字段提取和基础参数校验，不调用存储层。
*/

#include "../protocol/protocol.h"
#include <string>

namespace mydb::net {

/*
    类名：CommandParser
    功能：将协议帧转换为可执行的结构化命令。
        - 校验 opcode 对应的字段集合。
        - 提取请求参数并检查长度、类型和重复字段。
        - 不访问 Controller、Cache 或 Disk。
    友元类：无
*/
class CommandParser {
public:
    /*
        函数：parse
        参数：frame：FrameCodec 解析出的完整帧；connectionId：所属连接标识
        功能：按照 opcode 定义校验字段，并构造命令请求
        返回：成功返回 CommandRequest；失败返回对应 ErrorCode 和错误信息
    */
    bool parse(const Frame& frame, ConnectionId connectionId, CommandRequest& request,
               ErrorCode& error, std::string& message,
               std::uint32_t maxFrameBytes = DEFAULT_MAX_FRAME_BYTES) const;
};

}

#endif
