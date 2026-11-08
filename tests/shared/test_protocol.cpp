#include <QtTest>
#include <QJsonDocument>
#include <QtEndian>
#include "shared/protocol/framecodec.h"

namespace
{
QByteArray rawFrame(const QByteArray &json)
{
    QByteArray b(12, '\0');
    b.replace(0, 4, "STW2");
    b[4] = 2;
    qToBigEndian<quint16>(12, b.data() + 6);
    qToBigEndian<quint32>(json.size(), b.data() + 8);
    b += json;
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
    void fragmentedAndCoalesced()
    {
        const QJsonObject first{
            {"kind", "hello"}, {"sequence", 1}, {"session", QStringLiteral("中文会话")}};
        const QJsonObject second{{"kind", "terminal"}, {"value", 5.1}};
        const auto frame = Wire::encode(first);
        for (qsizetype split = 0; split <= frame.size(); ++split)
        {
            Wire::Parser parser;
            auto actual = parser.append(frame.left(split));
            actual += parser.append(frame.mid(split) + Wire::encode(second));
            QCOMPARE(actual, QList<QJsonObject>({first, second}));
            QCOMPARE(parser.bufferedBytes(), 0);
            QVERIFY(parser.takeError().isEmpty());
        }
    }
    void byteByByteAndGarbageRecovery()
    {
        Wire::Parser parser;
        const QJsonObject expected{{"kind", "ping"}};
        const auto frame = QByteArray("junkSTW") + Wire::encode(expected);
        QList<QJsonObject> decoded;
        for (const auto c : frame)
            decoded += parser.append(QByteArray(1, c));
        QCOMPARE(decoded, QList<QJsonObject>({expected}));
        QVERIFY(!parser.takeError().isEmpty());
        QVERIFY(parser.takeError().isEmpty());
    }
    void invalidHeaderAndCrcAreRejected_data()
    {
        QTest::addColumn<int>("offset");
        QTest::newRow("version") << 4;
        QTest::newRow("flags") << 5;
        QTest::newRow("header-size") << 7;
        QTest::newRow("oversized-length") << 8;
        QTest::newRow("payload-crc") << 14;
    }
    void invalidHeaderAndCrcAreRejected()
    {
        QFETCH(int, offset);
        const QJsonObject expected{{"kind", "pong"}};
        auto bad = Wire::encode(expected);
        bad[offset] = char(quint8(bad[offset]) ^ 0x40);
        Wire::Parser parser;
        const auto decoded = parser.append(bad + Wire::encode(expected));
        QCOMPARE(decoded, QList<QJsonObject>({expected}));
        QVERIFY(!parser.takeError().isEmpty());
        QVERIFY(parser.bufferedBytes() <= Wire::MaxBuffered);
    }
    void validCrcCannotAuthorizeMalformedJson()
    {
        Wire::Parser parser;
        const QJsonObject expected{{"kind", "valid"}};
        QCOMPARE(parser.append(rawFrame("{") + rawFrame("[1,2]") + Wire::encode(expected)),
                 QList<QJsonObject>({expected}));
        QVERIFY(!parser.takeError().isEmpty());
    }
    void boundedNoiseAndHugeCallerRead()
    {
        Wire::Parser parser;
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
    void incompleteMaximumFrameHasBoundedStorage()
    {
        const auto frame = Wire::encode({{"large", QString(65000, 'a')}});
        QVERIFY(!frame.isEmpty());
        Wire::Parser parser;
        QVERIFY(parser.append(frame.left(frame.size() - 1)).isEmpty());
        QVERIFY(parser.bufferedBytes() <= Wire::MaxBuffered);
        QCOMPARE(parser.append(frame.right(1)).size(), 1);
    }
};
QTEST_GUILESS_MAIN(ProtocolTests)
#include "test_protocol.moc"
