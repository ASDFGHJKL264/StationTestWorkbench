#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace Wire
{
// STW2 | version:u8 | flags:u8 | headerBytes:u16BE | payloadBytes:u32BE
// | UTF-8 JSON object | CRC16/Modbus:u16LE (header and payload).
constexpr qsizetype MaxPayload = 64 * 1024;
constexpr qsizetype MaxBuffered = MaxPayload + 12 + 2 + 16384;
QByteArray encode(const QJsonObject &object);
class Parser
{
  public:
    QList<QJsonObject> append(const QByteArray &bytes);
    QString takeError();
    void reset();
    qsizetype bufferedBytes() const
    {
        return m_buffer.size();
    }

  private:
    void consume(QList<QJsonObject> &objects);
    QByteArray m_buffer;
    QString m_error;
};
} // namespace Wire
