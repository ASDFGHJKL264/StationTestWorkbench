#include <QtTest>
#include <QJsonDocument>
#include <QtEndian>
#include "framecodec.h"

namespace
{
// 手工封装 json 并计算 CRC，供畸形 JSON 和协议校验测试使用。
QByteArray rawFrame(const QByteArray &json)
{
    // 暂存手工组装的协议帧字节。
    QByteArray b(12, '\0');
    b.replace(0, 4, "STW2");
    b[4] = 2;
    qToBigEndian<quint16>(12, b.data() + 6);
    qToBigEndian<quint32>(json.size(), b.data() + 8);
    b += json;
    // 累积手工计算的 CRC16 校验值。
    quint16 crc = 0xffff;
    for (const auto byte : b)
    {
        crc ^= quint8(byte);
        for (int i = 0; i < 8; ++i)
            crc = crc & 1 ? (crc >> 1) ^ 0xa001 : crc >> 1;
    }
    b += char(crc & 255);
    b += char(crc >> 8);
    return b;
}
} // namespace
class ProtocolTests : public QObject
{
    Q_OBJECT
  private slots:
    // 验证任意拆包和多帧粘连都能还原业务报文。
    void fragmentedAndCoalesced()
    {
        // 提供拆包验证中的首个合法握手报文。
        const QJsonObject first{
            {"kind", "hello"}, {"sequence", 1}, {"session", QStringLiteral("中文会话")}};
        // 提供粘连在首帧之后的合法终态报文。
        const QJsonObject second{{"kind", "terminal"}, {"value", 5.1}};
        // 保存用于分段、粘连或噪声验证的协议帧字节。
        const auto frame = Wire::encode(first);
        for (qsizetype split = 0; split <= frame.size(); ++split)
        {
            // 解析本次输入的协议字节并保存未完整数据。
            Wire::Parser parser;
            // 收集分段输入后解析出的业务报文。
            auto actual = parser.append(frame.left(split));
            actual += parser.append(frame.mid(split) + Wire::encode(second));
            QCOMPARE(actual, QList<QJsonObject>({first, second}));
            QCOMPARE(parser.bufferedBytes(), 0);
            QVERIFY(parser.takeError().isEmpty());
        }
    }
    // 验证逐字节输入及前置噪声后能够恢复合法报文。
    void byteByByteAndGarbageRecovery()
    {
        // 解析本次输入的协议字节并保存未完整数据。
        Wire::Parser parser;
        // 保存合法报文解析后的期望内容。
        const QJsonObject expected{{"kind", "ping"}};
        // 保存用于分段、粘连或噪声验证的协议帧字节。
        const auto frame = QByteArray("junkSTW") + Wire::encode(expected);
        // 收集输入字节解析出的业务报文。
        QList<QJsonObject> decoded;
        for (const auto c : frame)
            decoded += parser.append(QByteArray(1, c));
        QCOMPARE(decoded, QList<QJsonObject>({expected}));
        QVERIFY(!parser.takeError().isEmpty());
        QVERIFY(parser.takeError().isEmpty());
    }
    // 提供版本、标志、头长度和校验字节的损坏位置。
    void invalidHeaderAndCrcAreRejected_data()
    {
        QTest::addColumn<int>("offset");
        QTest::newRow("version") << 4;
        QTest::newRow("flags") << 5;
        QTest::newRow("header-size") << 7;
        QTest::newRow("oversized-length") << 8;
        QTest::newRow("payload-crc") << 14;
    }
    // 验证损坏帧被拒绝后仍能继续读取后续合法帧。
    void invalidHeaderAndCrcAreRejected()
    {
        // 指定本次数据驱动测试需要破坏的字节位置。
        QFETCH(int, offset);
        // 保存合法报文解析后的期望内容。
        const QJsonObject expected{{"kind", "pong"}};
        // 保存故意篡改头部或校验内容的协议帧。
        auto bad = Wire::encode(expected);
        bad[offset] = char(quint8(bad[offset]) ^ 0x40);
        // 解析本次输入的协议字节并保存未完整数据。
        Wire::Parser parser;
        // 收集输入字节解析出的业务报文。
        const auto decoded = parser.append(bad + Wire::encode(expected));
        QCOMPARE(decoded, QList<QJsonObject>({expected}));
        QVERIFY(!parser.takeError().isEmpty());
        QVERIFY(parser.bufferedBytes() <= Wire::MaxBuffered);
    }
    // 验证正确 CRC 不能让畸形 JSON 或非对象报文通过。
    void validCrcCannotAuthorizeMalformedJson()
    {
        // 解析本次输入的协议字节并保存未完整数据。
        Wire::Parser parser;
        // 保存合法报文解析后的期望内容。
        const QJsonObject expected{{"kind", "valid"}};
        QCOMPARE(parser.append(rawFrame("{") + rawFrame("[1,2]") + Wire::encode(expected)),
                 QList<QJsonObject>({expected}));
        QVERIFY(!parser.takeError().isEmpty());
    }
    // 验证大量噪声和超长输入下解析缓冲仍有界。
    void boundedNoiseAndHugeCallerRead()
    {
        // 解析本次输入的协议字节并保存未完整数据。
        Wire::Parser parser;
        // 保存合法报文解析后的期望内容。
        const QJsonObject expected{{"kind", "survived"}};
        QCOMPARE(parser.append(QByteArray(4 * 1024 * 1024, 'x') + Wire::encode(expected)),
                 QList<QJsonObject>({expected}));
        QVERIFY(parser.bufferedBytes() <= Wire::MaxBuffered);
        QVERIFY(Wire::encode({{"large", QString(70 * 1024, 'x')}}).isEmpty());
        parser.append(Wire::encode(expected).left(8));
        QVERIFY(parser.bufferedBytes() > 0);
        parser.reset();
        QCOMPARE(parser.bufferedBytes(), 0);
        QCOMPARE(parser.append(Wire::encode(expected)), QList<QJsonObject>({expected}));
    }
    // 验证最大帧未收齐时缓冲有界，末字节到达后可完成解析。
    void incompleteMaximumFrameHasBoundedStorage()
    {
        // 保存用于分段、粘连或噪声验证的协议帧字节。
        const auto frame = Wire::encode({{"large", QString(65000, 'a')}});
        QVERIFY(!frame.isEmpty());
        // 解析本次输入的协议字节并保存未完整数据。
        Wire::Parser parser;
        QVERIFY(parser.append(frame.left(frame.size() - 1)).isEmpty());
        QVERIFY(parser.bufferedBytes() <= Wire::MaxBuffered);
        QCOMPARE(parser.append(frame.right(1)).size(), 1);
    }
};
QTEST_GUILESS_MAIN(ProtocolTests)
#include "test_protocol.moc"
