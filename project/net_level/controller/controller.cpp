/*
    模块名：网络服务 Controller
    功能描述：对外提供网络服务配置、命令执行器绑定和生命周期管理。
*/

#include "controller.h"

#include "../command/command_executor.h"

#include <utility>

namespace mydb::net {

/*
    函数：Controller
    参数：config：网络服务配置
    功能：创建网络服务 Controller
    返回：无
*/
Controller::Controller(ServerConfig config) : config(std::move(config)) {}

/*
    函数：bindStorageController
    参数：storageController：现有数据存储层 Controller
    功能：绑定阶段测试用存储命令执行器
    返回：无
*/
void Controller::bindStorageController(::Controller& storageController) {
    executor = std::make_shared<StorageControllerExecutor>(storageController);
}

/*
    函数：start
    参数：无
    功能：创建并启动网络服务端
    返回：是否启动成功
*/
bool Controller::start() {
    if (!server) {
        server = std::make_unique<NetworkServer>(config, executor);
    }
    return server->start();
}

/*
    函数：stop
    参数：无
    功能：立即停止网络服务端
    返回：无
*/
void Controller::stop() {
    if (server) {
        server->stop();
    }
}

/*
    函数：wait
    参数：无
    功能：等待服务线程退出；stop() 当前同步 join
    返回：无
*/
void Controller::wait() {
    if (server) {
        server->wait();
    }
}

/*
    函数：getState
    参数：无
    功能：读取网络服务状态
    返回：ServerState
*/
ServerState Controller::getState() const {
    return server ? server->getState() : ServerState::CREATED;
}

}
