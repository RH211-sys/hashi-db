/*
    模块名：连接会话
    模块地位：实现连接会话的协商、认证与权限阶段维护。
    模块功能描述：维护 HELLO（协议协商命令）、认证主体和当前协议阶段。
*/

#include "session.h"

#include <utility>

namespace mydb::net {

bool Session::negotiate(std::uint16_t version, std::uint64_t featureBits) {
    if (version != PROTOCOL_VERSION) {
        // 客户端版本与当前实现不匹配，协商失败且不更新会话状态。
        return false;
    }
    protocolVersion = version;
    this->featureBits = featureBits;
    negotiated = true;
    return true;
}

void Session::authenticate(std::string principal) {
    this->principal = std::move(principal);
    authenticated = true;
}

bool Session::canExecute(Opcode opcode, RequestSource source) const {
    if (source == RequestSource::LOCAL_ADMIN) {
        // 本地管理请求绕过远程会话阶段限制。
        return true;
    }
    if (!negotiated) {
        // 协议尚未协商，只允许协商、探活和退出命令。
        return opcode == Opcode::HELLO || opcode == Opcode::PING || opcode == Opcode::QUIT;
    }
    if (!authenticated) {
        // 协议已协商但尚未认证，只允许认证、探活和退出命令。
        return opcode == Opcode::AUTH || opcode == Opcode::PING || opcode == Opcode::QUIT;
    }
    return true;
}

bool Session::isAuthenticated() const {
    return authenticated;
}

bool Session::isNegotiated() const {
    return negotiated;
}

const std::string& Session::getPrincipal() const {
    return principal;
}

std::uint64_t Session::getFeatureBits() const {
    return featureBits;
}

}
