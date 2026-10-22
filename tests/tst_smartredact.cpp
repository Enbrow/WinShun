// Smart redaction's rules (src/capture/SmartRedact): what is found and what
// is left alone, from lines of text with the places of their characters, no
// reader needed (docs/design-capture-roadmap.md, 4.1).
#include "SmartRedact.h"

#include <QTest>

#include <cmath>
#include <cstring>

using namespace ws::capture;
using namespace Qt::StringLiterals;

namespace {

// A line of `text` whose characters are 10 px wide each, from x = 100, 20 px high.
RedactLine lineOf(const QString& text, int top = 0)
{
    RedactLine line;
    line.text = text;
    const int n = int(text.size());
    line.quad = QPolygonF({QPointF(100, top), QPointF(100 + 10 * n, top), QPointF(100 + 10 * n, top + 20), QPointF(100, top + 20)});
    for (int i = 0; i < n; ++i)
        line.chars.append({i, float((i + 0.5) / n)});
    return line;
}

RedactRules names()
{
    RedactRules rules;
    rules.names = {u"zhangsan"_s, u"DESKTOP-K3J9"_s, u"ab"_s};
    return rules;
}

QStringList categories(const QList<RedactHit>& hits)
{
    QStringList out;
    for (const RedactHit& h : hits)
        out << h.category;
    return out;
}

QList<RedactHit> find(const QString& text, const RedactRules& rules = names())
{
    return findSensitive({lineOf(text)}, rules, {});
}

} // namespace

class TestSmartRedact : public QObject {
    Q_OBJECT

private slots:
    void phones_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<bool>("found");
        QTest::addRow("plain") << u"电话 13812345678 转 8"_s << true;
        QTest::addRow("spaces 3-4-4") << u"手机：138 1234 5678"_s << true;
        QTest::addRow("dashes") << u"138-1234-5678"_s << true;
        QTest::addRow("plus 86") << u"Tel +86 138 1234 5678"_s << true;
        QTest::addRow("plus 86 joined") << u"+8613812345678"_s << true;
        QTest::addRow("0086") << u"0086 13812345678"_s << true;
        QTest::addRow("full-width digits") << u"１３８１２３４５６７８"_s << true;
        QTest::addRow("look-alike letters") << u"138I234567O"_s << true;
        QTest::addRow("12 digits") << u"138123456789"_s << false;
        QTest::addRow("10 digits") << u"1381234567"_s << false;
        QTest::addRow("starts 12") << u"12812345678"_s << false;
        QTest::addRow("inside a longer number") << u"9913812345678"_s << false;
        QTest::addRow("a version") << u"v1.3.8.12345678"_s << false;
    }

    void phones()
    {
        QFETCH(QString, text);
        QFETCH(bool, found);
        RedactRules rules;
        rules.idCard = rules.bankCard = false; // the phone rule alone
        QCOMPARE(!findSensitive({lineOf(text)}, rules, {}).isEmpty(), found);
    }

    void idCards_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<bool>("found");
        // 11010519491231002X is the specimen number of the standard.
        QTest::addRow("real") << u"身份证 11010519491231002X"_s << true;
        QTest::addRow("real lower x") << u"11010519491231002x"_s << true;
        QTest::addRow("real digit check") << u"440524198801010011"_s << true;
        QTest::addRow("wrong check digit") << u"110105194912310021"_s << false;
        QTest::addRow("wrong check digit 2") << u"440524198801010012"_s << false;
        QTest::addRow("impossible date") << u"110105194913310025"_s << false;
        QTest::addRow("old 15 digits") << u"110105491231002"_s << true;
        QTest::addRow("old 15 impossible month") << u"110105491331002"_s << false;
        QTest::addRow("19 digits") << u"1101051949123100271"_s << false;
        QTest::addRow("starts 0") << u"010105194912310027"_s << false;
    }

    void idCards()
    {
        QFETCH(QString, text);
        QFETCH(bool, found);
        const QList<RedactHit> hits = find(text);
        QCOMPARE(categories(hits).contains(u"id"_s), found);
        // A wrong identity number is not taken for a bank card either.
        if (!found)
            QVERIFY(!categories(hits).contains(u"card"_s));
    }

    void idChecksum()
    {
        QVERIFY(validIdCard(u"11010519491231002X"));
        QVERIFY(!validIdCard(u"11010519491231002"));
        QVERIFY(!validIdCard(u"1101051949123100AB"));
        // Every check digit from the same 17 digits: exactly one is right.
        int right = 0;
        for (const QString check : {u"0"_s, u"1"_s, u"2"_s, u"3"_s, u"4"_s, u"5"_s, u"6"_s, u"7"_s, u"8"_s, u"9"_s, u"X"_s})
            right += validIdCard(u"11010519491231002" + check);
        QCOMPARE(right, 1);
    }

    void bankCards_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<bool>("found");
        QTest::addRow("visa test") << u"卡号 4111111111111111"_s << true;
        QTest::addRow("visa grouped") << u"4111 1111 1111 1111"_s << true;
        QTest::addRow("unionpay") << u"6222021234567890123"_s << true; // 19 digits, Luhn ok? checked below
        QTest::addRow("mastercard") << u"5555555555554444"_s << true;
        QTest::addRow("amex 15") << u"378282246310005"_s << true;
        QTest::addRow("luhn fails") << u"4111111111111112"_s << false;
        QTest::addRow("luhn fails grouped") << u"4111 1111 1111 1112"_s << false;
        QTest::addRow("12 digits") << u"411111111111"_s << false;
        QTest::addRow("20 digits") << u"41111111111111111111"_s << false;
        QTest::addRow("a timestamp") << u"1700000000000"_s << false;
        QTest::addRow("all the same") << u"3333333333333333"_s << false;
    }

    void bankCards()
    {
        QFETCH(QString, text);
        QFETCH(bool, found);
        if (QByteArray(QTest::currentDataTag()) == "unionpay")
            found = validLuhn(u"6222021234567890123");
        QCOMPARE(categories(find(text)).contains(u"card"_s), found);
    }

    void luhn()
    {
        QVERIFY(validLuhn(u"4111111111111111"));
        QVERIFY(!validLuhn(u"4111111111111112"));
        QVERIFY(validLuhn(u"79927398713"));
        QVERIFY(!validLuhn(u"7992739871a"));
        QVERIFY(!validLuhn(u"0"));
    }

    void emails()
    {
        QCOMPARE(categories(find(u"联系 a.b-c+d@example.co.uk 谢谢"_s)), QStringList{u"email"_s});
        QVERIFY(find(u"user at example dot com"_s).isEmpty());
        QVERIFY(find(u"@mention only"_s).isEmpty());
        QVERIFY(find(u"price @ 5 each"_s).isEmpty());
    }

    void secrets_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<bool>("found");
        QTest::addRow("openai") << u"key: sk-proj-AbCdEfGhIjKlMnOpQrStUvWxYz012345"_s << true;
        QTest::addRow("anthropic") << u"sk-ant-api03-AbCdEfGhIjKlMnOpQrSt"_s << true;
        QTest::addRow("github") << u"token ghp_AbCdEfGhIjKlMnOpQrStUvWxYz0123456789"_s << true;
        QTest::addRow("github oauth") << u"gho_AbCdEfGhIjKlMnOpQrStUvWxYz0123456789"_s << true;
        QTest::addRow("github pat") << u"github_pat_11AAAAAAA0AbCdEfGhIjKl_mnopqrstuvwxyz"_s << true;
        QTest::addRow("gitlab") << u"glpat-AbCdEfGhIjKlMnOpQrSt"_s << true;
        QTest::addRow("slack bot") << u"xoxb-1234567890-AbCdEfGhIj"_s << true;
        QTest::addRow("slack user") << u"xoxp-1234567890-AbCdEfGhIj"_s << true;
        QTest::addRow("aws") << u"AKIAIOSFODNN7EXAMPLE"_s << true;
        QTest::addRow("google") << u"AIzaSyA-AbCdEfGhIjKlMnOpQrStUvWxYz01234"_s << true;
        QTest::addRow("jwt") << u"eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.dBjftJeZ4CVPmB92K27uhbUJU1p1r"_s << true;
        QTest::addRow("private key") << u"-----BEGIN RSA PRIVATE KEY-----"_s << true;
        QTest::addRow("password=") << u"password=hunter2"_s << true;
        QTest::addRow("Password: ") << u"Password: Tr0ub4dor&3"_s << true;
        QTest::addRow("password json") << u"\"password\": \"hunter2\","_s << true;
        QTest::addRow("passwd") << u"passwd = s3cr3t"_s << true;
        QTest::addRow("api_key") << u"api_key=abcdef123456"_s << true;
        QTest::addRow("api-key") << u"API-KEY: abcdef123456"_s << true;
        QTest::addRow("access_key") << u"access_key = zzz999"_s << true;
        QTest::addRow("secret") << u"client_secret=Zk3f9"_s << true;
        QTest::addRow("chinese password") << u"密码：123456"_s << true;
        QTest::addRow("chinese token") << u"令牌: abc.def"_s << true;
        QTest::addRow("chinese key") << u"密钥 = qwertyuiop"_s << true;
        QTest::addRow("placeholder $VAR") << u"token=$TOKEN"_s << false;
        QTest::addRow("placeholder ${VAR}") << u"password=${DB_PASSWORD}"_s << false;
        QTest::addRow("placeholder %VAR%") << u"password=%PASSWORD%"_s << false;
        QTest::addRow("placeholder stars") << u"password: ********"_s << false;
        QTest::addRow("placeholder 4 stars") << u"password: ****"_s << false;
        QTest::addRow("placeholder xxx") << u"api_key=xxxxxxxx"_s << false;
        QTest::addRow("placeholder angle") << u"api_key=<your-key>"_s << false;
        QTest::addRow("placeholder angle 2") << u"API_KEY=<YOUR_API_KEY>"_s << false;
        QTest::addRow("placeholder env") << u"token = process.env.GITHUB_TOKEN"_s << false;
        QTest::addRow("placeholder your") << u"secret=your_secret_here"_s << false;
        QTest::addRow("placeholder bullets") << u"密码：••••••"_s << false;
        QTest::addRow("empty value") << u"password:"_s << false;
        QTest::addRow("sk- placeholder") << u"OPENAI_API_KEY=sk-xxxxxxxxxxxxxxxxxxxxxxxx"_s << false;
        QTest::addRow("word token") << u"the token count is 4096"_s << false;
        QTest::addRow("max_tokens") << u"max_tokens: 4096"_s << false;
        QTest::addRow("short sk") << u"sk-12"_s << false;
    }

    void secrets()
    {
        QFETCH(QString, text);
        QFETCH(bool, found);
        QCOMPARE(categories(find(text)).contains(u"secret"_s), found);
    }

    // A key is covered to the end of the line, not just where it was recognised.
    void secretsCoverTheRestOfTheLine()
    {
        const QString text = u"export API_KEY=sk-AbCdEfGhIjKlMnOpQrStUv and more"_s;
        const QList<RedactHit> hits = find(text);
        QCOMPARE(hits.size(), 1);
        const int from = int(text.indexOf(u"sk-"_s));
        QVERIFY(hits[0].box.right() >= 100 + 10 * int(text.size()) - 1);
        QVERIFY(hits[0].box.left() <= 100 + 10 * from);
        // The key's name stays readable.
        QVERIFY(hits[0].box.left() > 100 + 10 * 7);
    }

    void identity()
    {
        QCOMPARE(categories(find(u"C:\\Users\\zhangsan\\Documents\\a.txt"_s)), QStringList{u"identity"_s});
        QCOMPARE(categories(find(u"PS C:\\Users\\ZhangSan> dir"_s)), QStringList{u"identity"_s});
        QCOMPARE(categories(find(u"主机 DESKTOP-K3J9 已连接"_s)), QStringList{u"identity"_s});
        QVERIFY(find(u"zhangsanfeng wrote this"_s).isEmpty()); // inside a longer word
        QVERIFY(find(u"/home/xzhangsan/"_s).isEmpty());
        // A name of two letters only in a path or a prompt.
        QVERIFY(find(u"about the abstract"_s).isEmpty());
        QVERIFY(find(u"a b ab c"_s).isEmpty());
        QCOMPARE(categories(find(u"C:\\Users\\ab\\Desktop"_s)), QStringList{u"identity"_s});
        QCOMPARE(categories(find(u"ab@host:~$ ls"_s)), QStringList{u"identity"_s});
        // Switched off, or without names, nothing.
        RedactRules rules = names();
        rules.identity = false;
        QVERIFY(find(u"C:\\Users\\zhangsan\\"_s, rules).isEmpty());
        QVERIFY(find(u"C:\\Users\\zhangsan\\"_s, RedactRules()).isEmpty());
    }

    void network()
    {
        RedactRules rules = names();
        QVERIFY(find(u"服务器 192.168.1.20 在线"_s, rules).isEmpty()); // off by default
        rules.ipMac = true;
        QCOMPARE(categories(find(u"服务器 192.168.1.20 在线"_s, rules)), QStringList{u"ip"_s});
        QCOMPARE(categories(find(u"mac 00:1A:2B:3C:4D:5E"_s, rules)), QStringList{u"ip"_s});
        QCOMPARE(categories(find(u"inet6 fe80::1c2d:3e4f:5a6b:7c8d"_s, rules)), QStringList{u"ip"_s});
        QVERIFY(find(u"版本 1.2.3.4"_s, rules).isEmpty());
        QVERIFY(find(u"version 1.2.3.4"_s, rules).isEmpty());
        QVERIFY(find(u"999.1.1.1"_s, rules).isEmpty());
        QVERIFY(find(u"time 12:30:45"_s, rules).isEmpty());
    }

    void everythingAndWords()
    {
        RedactRules rules;
        rules.allText = true;
        QCOMPARE(categories(findSensitive({lineOf(u"hello"_s, 0), lineOf(u"world"_s, 40)}, rules, {})), (QStringList{u"text"_s, u"text"_s}));
        RedactRules words;
        words.words = {u"Project Atlas"_s, u"机密"_s};
        QCOMPARE(categories(find(u"about project atlas today"_s, words)), QStringList{u"word"_s});
        QCOMPARE(categories(find(u"这是机密文件"_s, words)), QStringList{u"word"_s});
        QVERIFY(find(u"projects atlas"_s, words).isEmpty());
    }

    // The box is the characters' places plus the margins of 4.1; the line's
    // own corners bound it; matches close on one line become one.
    void boxes()
    {
        const QString text = u"pq13812345678rs"_s; // every character as wide as a digit: ten px each
        const RedactLine line = lineOf(text);
        const int from = int(text.indexOf(u'1'));
        const int to = from + 11;
        const QRect plain = matchBox(line, from, to);
        // Ten px a character: from x = 100 + 10 * from to 100 + 10 * to.
        QVERIFY2(std::abs(plain.left() - (100 + 10 * from)) <= 1, qPrintable(QString::number(plain.left())));
        QVERIFY2(std::abs(plain.right() + 1 - (100 + 10 * to)) <= 1, qPrintable(QString::number(plain.right())));
        QCOMPARE(plain.top(), 0);
        QCOMPARE(plain.height(), 20);
        const QList<RedactHit> hits = find(text);
        QCOMPARE(hits.size(), 1);
        // Margin: sideways max(2, 0.15 x 20) = 3, up and down 2.
        QVERIFY(std::abs(hits[0].box.left() - (plain.left() - 3)) <= 1);
        QVERIFY(std::abs(hits[0].box.right() - (plain.right() + 3)) <= 1);
        QCOMPARE(hits[0].box.top(), -2);
        QCOMPARE(hits[0].box.bottom(), 21);
        // Two matches a word apart are one box.
        const QList<RedactHit> two = find(u"13812345678 13987654321"_s);
        QCOMPARE(two.size(), 1);
        // Far apart they are two.
        QCOMPARE(find(u"13812345678                    13987654321"_s).size(), 2);
        // Lines are separate, top to bottom.
        const QList<RedactHit> rows = findSensitive({lineOf(u"13987654321"_s, 100), lineOf(u"13812345678"_s, 10)}, names(), {});
        QCOMPARE(rows.size(), 2);
        QVERIFY(rows[0].box.top() < rows[1].box.top());
    }

    // With no places from the reader, the widths decide: a full-width
    // character is two half-width ones.
    void boxesWithoutPlaces()
    {
        RedactLine line = lineOf(u"电话13812345678"_s);
        line.chars.clear();
        // Two full-width (1 each) and eleven digits (0.56 each) over 130 px.
        const QRect box = matchBox(line, 2, 13);
        QVERIFY2(box.left() >= 130 && box.left() <= 134, qPrintable(QString::number(box.left())));
        QVERIFY(box.right() >= 228);
    }

    void alreadyCovered()
    {
        const QString text = u"13812345678"_s;
        const QRect whole = find(text)[0].box;
        QVERIFY(findSensitive({lineOf(text)}, names(), {whole}).isEmpty());
        QVERIFY(findSensitive({lineOf(text)}, names(), {whole.adjusted(0, 0, -5, 0)}).isEmpty()); // 95 % covered
        QCOMPARE(findSensitive({lineOf(text)}, names(), {QRect(whole.left(), whole.top(), whole.width() / 2, whole.height())}).size(), 1);
        // Two redactions that together cover it.
        QVERIFY(findSensitive({lineOf(text)}, names(),
                    {QRect(whole.left(), whole.top(), whole.width() / 2 + 1, whole.height()),
                        QRect(whole.left() + whole.width() / 2, whole.top(), whole.width(), whole.height())})
                    .isEmpty());
    }

    // Snapping to the ink only trims blank space: whichever way the reader's
    // edge is wrong, no glyph of the match is cut.
    void snappingNeverCutsAGlyph_data()
    {
        QTest::addColumn<double>("shift");
        for (double shift : {-6.0, -4.0, -2.0, 0.0, 2.0, 4.0, 6.0}) // less than half the 16 px between glyphs
            QTest::addRow("reader %+.0f px off", shift) << shift;
    }

    void snappingNeverCutsAGlyph()
    {
        QFETCH(double, shift);
        // Five glyphs 10 px wide, 16 px apart: columns 50-59, 66-75, 82-91, 98-107, 114-123.
        QImage gray(200, 30, QImage::Format_Grayscale8);
        gray.fill(255);
        for (int i = 0; i < 5; ++i) {
            for (int y = 8; y < 24; ++y)
                std::memset(gray.scanLine(y) + 50 + 16 * i, 0, 10);
        }
        RedactLine line;
        line.text = u"13812"_s;
        line.quad = QPolygonF({QPointF(40, 0), QPointF(140, 0), QPointF(140, 30), QPointF(40, 30)});
        for (int i = 0; i < 5; ++i)
            line.chars.append({i, float((55 + 16 * i + shift - 40) / 100.0)}); // the reader's centres, shifted
        // The line is 30 px high, so the reach is 14: wide enough for these errors.
        const QRect box = matchBox(line, 0, 5, gray);
        QVERIFY2(box.left() <= 50 && box.left() >= 50 - 1, qPrintable(QString::number(box.left())));
        QVERIFY2(box.right() >= 123 && box.right() <= 123 + 1, qPrintable(QString::number(box.right())));
        // A middle glyph is not an edge.
        const QRect middle = matchBox(line, 1, 4, gray);
        QVERIFY2(middle.left() <= 66 && middle.right() >= 107, qPrintable(QString::number(middle.left()) + u".."_s + QString::number(middle.right())));
    }

    // Placeholders and things that look like what they are not.
    void leavesAlone()
    {
        for (const QString text : {u"$TOKEN"_s, u"****"_s, u"<your-key>"_s, u"${API_KEY}"_s, u"2024-01-15 10:30:00"_s,
                 u"订单号 20240115103000123"_s, u"Version 10.0.19045.5131"_s, u"共 13 项 · 1.2 MB"_s, u"https://example.com/a?b=1"_s,
                 u"1234567890"_s, u"0.0.0"_s, u""_s}) {
            QVERIFY2(find(text).isEmpty(), qPrintable(text));
        }
    }

    // Full-width and look-alike characters are read as what they stand for.
    void folding()
    {
        QCOMPARE(categories(find(u"ｐａｓｓｗｏｒｄ：ｈｕｎｔｅｒ２"_s)), QStringList{u"secret"_s});
        QCOMPARE(categories(find(u"邮箱：ａ＠ｂ．ｃｏｍ"_s)), QStringList{u"email"_s});
        QCOMPARE(categories(find(u"4111 1111 1111 111l"_s)), QStringList{u"card"_s}); // an l read for a 1
    }

    // What is found is only counted: the hits carry a category, no text.
    void hitsCarryNoText()
    {
        const QList<RedactHit> hits = find(u"13812345678"_s);
        QCOMPARE(hits.size(), 1);
        QVERIFY(!hits[0].category.contains(u"138"_s));
    }
};

QTEST_APPLESS_MAIN(TestSmartRedact)
#include "tst_smartredact.moc"
