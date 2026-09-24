/*
    模块名：连接会话
    功能描述：维护 HELLO 协商、认证主体和当前协议阶段。
*/

#include "session.h"

#include <utility>

namespace mydb::net {

bool Session::negotiate(std::uint16_t version, std::uint64_t featureBits) {
    if (version != PROTOCOL_VERSION) {
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
        return true;
    }
    if (!negotiated) {
        return opcode == Opcode::HELLO || opcode == Opcode::PING || opcode == Opcode::QUIT;
    }
    if (!authenticated) {
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
