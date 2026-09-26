#include "PairingQrCode.h"

#include <QByteArray>
#include <QPainter>
#include <QVector>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace {

constexpr int kVersion = 10;
constexpr int kSize = kVersion * 4 + 17;
constexpr int kDataCodewords = 216;
constexpr int kEccCodewordsPerBlock = 26;
constexpr int kBlockCount = 5;
constexpr int kQuietZone = 4;
constexpr int kMaxPayloadBytes = 213;

uint8_t gfMultiply(uint8_t x, uint8_t y) {
    int result = 0;
    for (int i = 7; i >= 0; --i) {
        result = (result << 1) ^ ((result >> 7) * 0x11D);
        result ^= ((y >> i) & 1) * x;
    }
    return static_cast<uint8_t>(result);
}

QVector<uint8_t> reedSolomonDivisor(int degree) {
    QVector<uint8_t> result(degree);
    result[degree - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; ++i) {
        for (int j = 0; j < degree; ++j) {
            result[j] = gfMultiply(result[j], root);
            if (j + 1 < degree) result[j] ^= result[j + 1];
        }
        root = gfMultiply(root, 0x02);
    }
    return result;
}

QVector<uint8_t> reedSolomonRemainder(const QVector<uint8_t>& data,
                                      const QVector<uint8_t>& divisor) {
    QVector<uint8_t> result(divisor.size());
    for (uint8_t value : data) {
        const uint8_t factor = value ^ result.front();
        for (int i = 0; i + 1 < result.size(); ++i) result[i] = result[i + 1];
        result.back() = 0;
        for (int i = 0; i < result.size(); ++i)
            result[i] ^= gfMultiply(divisor[i], factor);
    }
    return result;
}

void appendBits(QVector<bool>& bits, uint32_t value, int count) {
    for (int i = count - 1; i >= 0; --i) bits.push_back(((value >> i) & 1u) != 0);
}

QVector<uint8_t> makeDataCodewords(const QByteArray& payload) {
    if (payload.size() > kMaxPayloadBytes) return {};
    QVector<bool> bits;
    bits.reserve(kDataCodewords * 8);
    appendBits(bits, 0x4, 4); // Byte mode.
    appendBits(bits, static_cast<uint32_t>(payload.size()), 16);
    for (unsigned char value : payload) appendBits(bits, value, 8);
    const int capacity = kDataCodewords * 8;
    for (int i = 0; i < 4 && bits.size() < capacity; ++i) bits.push_back(false);
    while ((bits.size() & 7) != 0) bits.push_back(false);

    QVector<uint8_t> data;
    data.reserve(kDataCodewords);
    for (int offset = 0; offset < bits.size(); offset += 8) {
        uint8_t value = 0;
        for (int bit = 0; bit < 8; ++bit)
            value = static_cast<uint8_t>((value << 1) | (bits[offset + bit] ? 1 : 0));
        data.push_back(value);
    }
    for (int pad = 0; data.size() < kDataCodewords; ++pad)
        data.push_back((pad & 1) == 0 ? 0xEC : 0x11);
    return data;
}

QVector<uint8_t> addErrorCorrection(const QVector<uint8_t>& data) {
    if (data.size() != kDataCodewords) return {};
    const QVector<uint8_t> divisor = reedSolomonDivisor(kEccCodewordsPerBlock);
    std::array<QVector<uint8_t>, kBlockCount> blocks;
    std::array<QVector<uint8_t>, kBlockCount> ecc;
    int offset = 0;
    for (int block = 0; block < kBlockCount; ++block) {
        const int length = block == kBlockCount - 1 ? 44 : 43;
        blocks[block] = data.sliced(offset, length);
        ecc[block] = reedSolomonRemainder(blocks[block], divisor);
        offset += length;
    }

    QVector<uint8_t> result;
    result.reserve(346);
    for (int column = 0; column < 44; ++column)
        for (const auto& block : blocks)
            if (column < block.size()) result.push_back(block[column]);
    for (int column = 0; column < kEccCodewordsPerBlock; ++column)
        for (const auto& block : ecc) result.push_back(block[column]);
    return result;
}

class QrMatrix {
public:
    QrMatrix() : modules_(kSize * kSize, -1), function_(kSize * kSize, false) {}

    void drawFunctionPatterns() {
        for (int i = 0; i < kSize; ++i) {
            setFunction(6, i, (i & 1) == 0);
            setFunction(i, 6, (i & 1) == 0);
        }
        drawFinder(3, 3);
        drawFinder(kSize - 4, 3);
        drawFinder(3, kSize - 4);

        constexpr std::array<int, 3> centers{6, 28, 50};
        for (int row = 0; row < static_cast<int>(centers.size()); ++row) {
            for (int column = 0; column < static_cast<int>(centers.size()); ++column) {
                const bool overlapsFinder =
                    (row == 0 && column == 0) ||
                    (row == 0 && column == static_cast<int>(centers.size()) - 1) ||
                    (row == static_cast<int>(centers.size()) - 1 && column == 0);
                if (!overlapsFinder) drawAlignment(centers[column], centers[row]);
            }
        }
        drawFormatBits(0);
        drawVersionBits();
    }

    void drawCodewords(const QVector<uint8_t>& data) {
        int bitIndex = 0;
        for (int right = kSize - 1; right >= 1; right -= 2) {
            if (right == 6) right = 5;
            for (int vertical = 0; vertical < kSize; ++vertical) {
                const int y = ((right + 1) & 2) == 0
                    ? kSize - 1 - vertical : vertical;
                for (int offset = 0; offset < 2; ++offset) {
                    const int x = right - offset;
                    if (isFunction(x, y)) continue;
                    bool black = false;
                    if (bitIndex < data.size() * 8)
                        black = ((data[bitIndex >> 3] >> (7 - (bitIndex & 7))) & 1) != 0;
                    modules_[index(x, y)] = black ? 1 : 0;
                    ++bitIndex;
                }
            }
        }
    }

    void applyMask(int mask) {
        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                if (!isFunction(x, y) && maskBit(mask, x, y))
                    modules_[index(x, y)] ^= 1;
            }
        }
    }

    void drawFormatBits(int mask) {
        // Error-correction level M uses format value 00.
        const int data = mask;
        int remainder = data;
        for (int i = 0; i < 10; ++i)
            remainder = (remainder << 1) ^ ((remainder >> 9) * 0x537);
        const int bits = ((data << 10) | remainder) ^ 0x5412;
        auto bit = [bits](int i) { return ((bits >> i) & 1) != 0; };

        for (int i = 0; i <= 5; ++i) setFunction(8, i, bit(i));
        setFunction(8, 7, bit(6));
        setFunction(8, 8, bit(7));
        setFunction(7, 8, bit(8));
        for (int i = 9; i < 15; ++i) setFunction(14 - i, 8, bit(i));
        for (int i = 0; i < 8; ++i) setFunction(kSize - 1 - i, 8, bit(i));
        for (int i = 8; i < 15; ++i) setFunction(8, kSize - 15 + i, bit(i));
        setFunction(8, kSize - 8, true);
    }

    int penalty() const {
        int result = 0;
        auto value = [this](int x, int y) { return modules_[index(x, y)] > 0; };
        for (int y = 0; y < kSize; ++y) {
            int run = 1;
            for (int x = 1; x < kSize; ++x) {
                if (value(x, y) == value(x - 1, y)) ++run;
                else { if (run >= 5) result += 3 + run - 5; run = 1; }
            }
            if (run >= 5) result += 3 + run - 5;
        }
        for (int x = 0; x < kSize; ++x) {
            int run = 1;
            for (int y = 1; y < kSize; ++y) {
                if (value(x, y) == value(x, y - 1)) ++run;
                else { if (run >= 5) result += 3 + run - 5; run = 1; }
            }
            if (run >= 5) result += 3 + run - 5;
        }
        for (int y = 0; y + 1 < kSize; ++y)
            for (int x = 0; x + 1 < kSize; ++x) {
                const bool color = value(x, y);
                if (value(x + 1, y) == color && value(x, y + 1) == color &&
                    value(x + 1, y + 1) == color) result += 3;
            }

        constexpr std::array<bool, 11> patternA{
            false, false, false, false, true, false, true, true, true, false, true};
        constexpr std::array<bool, 11> patternB{
            true, false, true, true, true, false, true, false, false, false, false};
        auto finderPenalty = [&](auto sample) {
            int score = 0;
            for (int start = 0; start + 11 <= kSize; ++start) {
                bool a = true, b = true;
                for (int i = 0; i < 11; ++i) {
                    a = a && sample(start + i) == patternA[i];
                    b = b && sample(start + i) == patternB[i];
                }
                if (a || b) score += 40;
            }
            return score;
        };
        for (int y = 0; y < kSize; ++y)
            result += finderPenalty([&](int x) { return value(x, y); });
        for (int x = 0; x < kSize; ++x)
            result += finderPenalty([&](int y) { return value(x, y); });

        int dark = 0;
        for (int value : modules_) if (value > 0) ++dark;
        const int total = kSize * kSize;
        result += std::abs(dark * 20 - total * 10) / total * 10;
        return result;
    }

    bool black(int x, int y) const { return modules_[index(x, y)] > 0; }

private:
    static int index(int x, int y) { return y * kSize + x; }
    bool isFunction(int x, int y) const { return function_[index(x, y)]; }
    void setFunction(int x, int y, bool black) {
        modules_[index(x, y)] = black ? 1 : 0;
        function_[index(x, y)] = true;
    }
    void drawFinder(int centerX, int centerY) {
        for (int dy = -4; dy <= 4; ++dy) {
            for (int dx = -4; dx <= 4; ++dx) {
                const int x = centerX + dx, y = centerY + dy;
                if (x < 0 || x >= kSize || y < 0 || y >= kSize) continue;
                const int distance = std::max(std::abs(dx), std::abs(dy));
                setFunction(x, y, distance != 2 && distance != 4);
            }
        }
    }
    void drawAlignment(int centerX, int centerY) {
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx)
                setFunction(centerX + dx, centerY + dy,
                            std::max(std::abs(dx), std::abs(dy)) != 1);
    }
    void drawVersionBits() {
        int remainder = kVersion;
        for (int i = 0; i < 12; ++i)
            remainder = (remainder << 1) ^ ((remainder >> 11) * 0x1F25);
        const int bits = (kVersion << 12) | remainder;
        for (int i = 0; i < 18; ++i) {
            const bool black = ((bits >> i) & 1) != 0;
            const int a = kSize - 11 + i % 3;
            const int b = i / 3;
            setFunction(a, b, black);
            setFunction(b, a, black);
        }
    }
    static bool maskBit(int mask, int x, int y) {
        switch (mask) {
            case 0: return (x + y) % 2 == 0;
            case 1: return y % 2 == 0;
            case 2: return x % 3 == 0;
            case 3: return (x + y) % 3 == 0;
            case 4: return (x / 3 + y / 2) % 2 == 0;
            case 5: return (x * y) % 2 + (x * y) % 3 == 0;
            case 6: return ((x * y) % 2 + (x * y) % 3) % 2 == 0;
            case 7: return ((x + y) % 2 + (x * y) % 3) % 2 == 0;
            default: return false;
        }
    }

    QVector<int> modules_;
    QVector<bool> function_;
};

} // namespace

QImage pairingQrCodeImage(const QString& payload, int pixelSize) {
    const QVector<uint8_t> data = makeDataCodewords(payload.toUtf8());
    const QVector<uint8_t> codewords = addErrorCorrection(data);
    if (codewords.isEmpty() || pixelSize <= 0) return {};

    QrMatrix base;
    base.drawFunctionPatterns();
    base.drawCodewords(codewords);
    QrMatrix selected = base;
    int bestPenalty = -1;
    for (int mask = 0; mask < 8; ++mask) {
        QrMatrix candidate = base;
        candidate.applyMask(mask);
        candidate.drawFormatBits(mask);
        const int penalty = candidate.penalty();
        if (bestPenalty < 0 || penalty < bestPenalty) {
            bestPenalty = penalty;
            selected = std::move(candidate);
        }
    }

    QImage image(pixelSize, pixelSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    const int fullModules = kSize + kQuietZone * 2;
    const int scale = std::max(1, pixelSize / fullModules);
    const int rendered = fullModules * scale;
    const int origin = (pixelSize - rendered) / 2 + kQuietZone * scale;
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x)
            if (selected.black(x, y))
                painter.drawRect(origin + x * scale, origin + y * scale, scale, scale);
    return image;
}
