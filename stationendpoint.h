#pragma once
#include <QString>
#include <QtGlobal>

// Connection settings shared by the test engine and application entry point.
struct StationEndpoint
{
    int stationId;
    QString host;
    quint16 port;
};
