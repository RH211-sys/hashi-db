/*
    模块名：网络服务 Controller
    模块地位：实现网络服务入口的依赖绑定与生命周期转发。
    模块功能描述：对外提供网络服务配置、命令执行器绑定和生命周期管理。
*/

#include "controller.h"

#include "../command/command_executor.h"

#include <utility>

namespace mydb::net {

/*
    函数：Controller
    传参：config：网络服务配置
    功能：创建网络服务 Controller
    返回值：无
*/
Controller::Controller(ServerConfig config) : config(std::move(config)) {}

/*
    函数：bindStorageController
    传参：storageController：现有数据存储层 Controller
    功能：绑定阶段测试用存储命令执行器
    返回值：无
*/
void Controller::bindStorageController(::Controller& storageController) {
    executor = std::make_shared<StorageControllerExecutor>(storageController);
}

/*
    函数：start
    传参：无
    功能：创建并启动网络服务端
    返回值：是否启动成功
*/
bool Controller::start() {
    if (!server) {
        // 服务实例尚未创建，按当前配置和执行器组装 NetworkServer。
        server = std::make_unique<NetworkServer>(config, executor);
    }
    return server->start();
}

/*
    函数：stop
    传参：无
    功能：立即停止网络服务端
    返回值：无
*/
void Controller::stop() {
    if (server) {
        // 服务实例已创建，将立即停止请求转发给 NetworkServer。
        server->stop();
    }
}

/*
    函数：wait
    传参：无
    功能：等待服务线程退出；stop() 当前同步 join
    返回值：无
*/
void Controller::wait() {
    if (server) {
        // 服务实例已创建，等待其服务线程完成退出。
        server->wait();
    }
}

/*
    函数：getState
    传参：无
    功能：读取网络服务状态
    返回值：ServerState
*/
ServerState Controller::getState() const {
    return server ? server->getState() : ServerState::CREATED;
}

}
