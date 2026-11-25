#pragma once
#include <QString>
#include <QtGlobal>

// Connection settings shared by the test engine and application entry point.
struct StationEndpoint
{
    // 标识该连接对应的业务工位。
    int stationId;
    // 保存工位服务器的主机名或 IP 地址。
    QString host;
    // 保存工位服务器监听的 TCP 端口。
    quint16 port;
};
