#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace Wire
{
// 限制单帧 JSON 数据长度，避免接收超大报文。
// STW2 | version:u8 | flags:u8 | headerBytes:u16BE | payloadBytes:u32BE
// | UTF-8 JSON object | CRC16/Modbus:u16LE (header and payload).
constexpr qsizetype MaxPayload = 64 * 1024;
// 限制解析器暂存字节数，防止残缺帧持续占用内存。
constexpr qsizetype MaxBuffered = MaxPayload + 12 + 2 + 16384;
// 将 object 报文封装为带帧头和校验码的传输字节。
QByteArray encode(const QJsonObject &object);
class Parser
{
  public:
    // 接收 bytes 字节片段，返回本次拼出的完整 JSON 报文。
    QList<QJsonObject> append(const QByteArray &bytes);
    // 取走最近一次解析错误，供调用方记录日志。
    QString takeError();
    // 清空暂存字节和错误，准备解析新连接的数据。
    void reset();
    // 返回尚未解析完成的暂存字节数量。
    qsizetype bufferedBytes() const
    {
        return m_buffer.size();
    }

  private:
    // 从接收缓存提取完整帧，追加到 objects 报文列表。
    void consume(QList<QJsonObject> &objects);
    // 暂存尚未拼成完整帧的接收字节。
    QByteArray m_buffer;
    // 记录最近一次帧格式或校验错误。
    QString m_error;
};
} // namespace Wire
