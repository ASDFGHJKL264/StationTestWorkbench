#include "framecodec.h"
#include <QJsonDocument>
#include <QJsonParseError>
#include <QtEndian>
#include <utility>

namespace
{
// 保存 STW2 固定帧头的字节长度。
constexpr qsizetype Header = 12;
// 为 bytes 的前 count 字节计算 CRC16 校验码。
quint16 crc16(const char *bytes, qsizetype count)
{
    // 累计保存逐字节更新的 CRC16 计算结果。
    quint16 crc = 0xffff;
    for (qsizetype i = 0; i < count; ++i)
    {
        crc ^= quint8(bytes[i]);
        for (int bit = 0; bit < 8; ++bit)
            crc = crc & 1 ? (crc >> 1) ^ 0xa001 : crc >> 1;
    }
    return crc;
}
} // namespace
namespace Wire
{
QByteArray encode(const QJsonObject &object)
{
    // 保存紧凑 JSON 数据，作为传输帧的有效载荷。
    const auto json = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (json.isEmpty() || json.size() > MaxPayload)
        return {};
    // 依次组装帧头、JSON 数据和末尾校验码。
    QByteArray frame(Header, '\0');
    frame.replace(0, 4, "STW2");
    frame[4] = 2;
    qToBigEndian<quint16>(Header, frame.data() + 6);
    qToBigEndian<quint32>(quint32(json.size()), frame.data() + 8);
    frame += json;
    // 保存当前帧头及数据的校验值，准备追加到帧尾。
    const auto crc = crc16(frame.constData(), frame.size());
    frame.append(char(crc & 0xff));
    frame.append(char(crc >> 8));
    return frame;
}

QList<QJsonObject> Parser::append(const QByteArray &bytes)
{
    // 收集本次追加字节后解析出的完整业务报文。
    QList<QJsonObject> objects;
    // Parsing in bounded slices also handles a caller delivering a huge coalesced read.
    for (qsizetype pos = 0; pos < bytes.size(); pos += 16384)
    {
        m_buffer.append(bytes.constData() + pos, qMin<qsizetype>(16384, bytes.size() - pos));
        consume(objects);
        if (m_buffer.size() > MaxBuffered)
        {
            m_buffer.clear();
            m_error = QStringLiteral("receive buffer limit exceeded");
        }
    }
    return objects;
}

void Parser::consume(QList<QJsonObject> &objects)
{
    while (!m_buffer.isEmpty())
    {
        // 定位缓存中下一处有效帧头标记。
        const auto magic = m_buffer.indexOf("STW2");
        if (magic < 0)
        {
            // 保留最多三个尾部字节，以等待可能被拆开的帧头。
            // Keep the possible start of a split magic word.
            const auto keep = qMin<qsizetype>(3, m_buffer.size());
            if (m_buffer.size() > keep)
                m_error = QStringLiteral("discarded bytes before frame magic");
            m_buffer = m_buffer.right(keep);
            return;
        }
        if (magic > 0)
        {
            m_buffer.remove(0, magic);
            m_error = QStringLiteral("discarded bytes before frame magic");
        }
        if (m_buffer.size() < Header)
            return;
        // 读取帧头声明的 JSON 长度，用于边界校验。
        const auto payloadSize = qFromBigEndian<quint32>(m_buffer.constData() + 8);
        if (quint8(m_buffer[4]) != 2 || m_buffer[5] != 0 ||
            qFromBigEndian<quint16>(m_buffer.constData() + 6) != Header || payloadSize == 0 ||
            payloadSize > MaxPayload)
        {
            m_error = QStringLiteral("unsupported header or payload length");
            m_buffer.remove(0, 1);
            continue;
        }
        // 计算包含帧头、数据和校验码的完整帧长度。
        const qsizetype frameSize = Header + payloadSize + 2;
        if (m_buffer.size() < frameSize)
            return;
        // 取出帧尾携带的校验码，与本地计算值核对。
        const auto expected = qFromLittleEndian<quint16>(m_buffer.constData() + frameSize - 2);
        if (crc16(m_buffer.constData(), frameSize - 2) != expected)
        {
            m_error = QStringLiteral("CRC mismatch");
            m_buffer.remove(0, 1);
            continue;
        }
        // 接收 JSON 解析失败的位置和原因。
        QJsonParseError error;
        // 保存由完整帧数据解析出的 JSON 文档。
        const auto doc = QJsonDocument::fromJson(m_buffer.mid(Header, payloadSize), &error);
        m_buffer.remove(0, frameSize);
        if (error.error != QJsonParseError::NoError || !doc.isObject())
        {
            m_error = QStringLiteral("payload must be a valid JSON object");
            continue;
        }
        objects.append(doc.object());
    }
}
QString Parser::takeError()
{
    return std::exchange(m_error, {});
}
void Parser::reset()
{
    m_buffer.clear();
    m_error.clear();
}
} // namespace Wire
