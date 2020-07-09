// Copyright (c) 2017-2020 The Blocknet developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//******************************************************************************
//******************************************************************************

#include <xbridge/xbridgewalletconnectorbch.h>

#include <xbridge/util/logger.h>
#include <xbridge/xbitcointransaction.h>

#include <primitives/transaction.h>
#include <key.h>
#include <xbridge/util/txlog.h>


//*****************************************************************************
//*****************************************************************************
namespace xbridge
{

namespace rpc
{

//*****************************************************************************
//*****************************************************************************
bool getinfo(const std::string & rpcuser, const std::string & rpcpasswd,
             const std::string & rpcip, const std::string & rpcport,
             WalletInfo & info);

bool getnetworkinfo(const std::string & rpcuser, const std::string & rpcpasswd,
                    const std::string & rpcip, const std::string & rpcport,
                    WalletInfo & info);

bool decodeRawTransaction(const std::string & rpcuser, const std::string & rpcpasswd,
                          const std::string & rpcip, const std::string & rpcport,
                          const std::string & rawtx,
                          std::string & txid, std::string & tx);

bool signRawTransaction(const std::string & rpcuser,
                        const std::string & rpcpasswd,
                        const std::string & rpcip,
                        const std::string & rpcport,
                        std::string & rawtx,
                        bool & complete);
}

enum
{
    SCRIPT_ENABLE_SIGHASH_FORKID = (1U << 16),    // https://github.com/Bitcoin-ABC/bitcoin-abc/blob/64fd3187598221673455a584dc25c63a48db18a0/src/script/script_flags.h#L85
    SCRIPT_ENABLE_REPLAY_PROTECTION = (1U << 17), // https://github.com/Bitcoin-ABC/bitcoin-abc/blob/64fd3187598221673455a584dc25c63a48db18a0/src/script/script_flags.h#L89
};

//******************************************************************************
//******************************************************************************
namespace
{

// https://github.com/Bitcoin-ABC/bitcoin-abc/blob/f25d2ad300d902f1e99ffaf7ff0037bbc586b44b/src/script/sighashtype.h#L14
/** Signature hash types/flags */
enum {
    SIGHASH_ALL = 1,
    SIGHASH_NONE = 2,
    SIGHASH_SINGLE = 3,
    SIGHASH_FORKID = 0x40,
    SIGHASH_ANYONECANPAY = 0x80,
};

/**
 * Base signature hash types
 * Base sig hash types not defined in this enum may be used, but they will be
 * represented as UNSUPPORTED.  See transaction
 * c99c49da4c38af669dea436d3e73780dfdb6c1ecf9958baa52960e8baee30e73 for an
 * example where an unsupported base sig hash of 0 was used.
 */
enum class BaseSigHashType : uint8_t {
    UNSUPPORTED = 0,
    ALL = SIGHASH_ALL,
    NONE = SIGHASH_NONE,
    SINGLE = SIGHASH_SINGLE
};

/** Signature hash type wrapper class */
class SigHashType {
private:
    uint32_t sigHash;

public:
    explicit SigHashType() : sigHash(SIGHASH_ALL) {}

    explicit SigHashType(uint32_t sigHashIn) : sigHash(sigHashIn) {}

    SigHashType withBaseType(BaseSigHashType baseSigHashType) const {
        return SigHashType((sigHash & ~0x1f) | uint32_t(baseSigHashType));
    }

    SigHashType withForkValue(uint32_t forkId) const {
        return SigHashType((forkId << 8) | (sigHash & 0xff));
    }

    SigHashType withForkId(bool forkId = true) const {
        return SigHashType((sigHash & ~SIGHASH_FORKID) |
                           (forkId ? SIGHASH_FORKID : 0));
    }

    SigHashType withAnyoneCanPay(bool anyoneCanPay = true) const {
        return SigHashType((sigHash & ~SIGHASH_ANYONECANPAY) |
                           (anyoneCanPay ? SIGHASH_ANYONECANPAY : 0));
    }

    BaseSigHashType getBaseType() const {
        return BaseSigHashType(sigHash & 0x1f);
    }

    uint32_t getForkValue() const { return sigHash >> 8; }

    bool isDefined() const {
        auto baseType =
            BaseSigHashType(sigHash & ~(SIGHASH_FORKID | SIGHASH_ANYONECANPAY));
        return baseType >= BaseSigHashType::ALL &&
               baseType <= BaseSigHashType::SINGLE;
    }

    bool hasForkId() const { return (sigHash & SIGHASH_FORKID) != 0; }

    bool hasAnyoneCanPay() const {
        return (sigHash & SIGHASH_ANYONECANPAY) != 0;
    }

    uint32_t getRawSigHashType() const { return sigHash; }

    template <typename Stream> void Serialize(Stream &s) const {
        ::Serialize(s, getRawSigHashType());
    }

    template <typename Stream> void Unserialize(Stream &s) {
        ::Unserialize(s, sigHash);
    }

    /**
     * Handy operators.
     */
    friend constexpr bool operator==(const SigHashType &a,
                                     const SigHashType &b) {
        return a.sigHash == b.sigHash;
    }

    friend constexpr bool operator!=(const SigHashType &a,
                                     const SigHashType &b) {
        return !(a == b);
    }
};

//******************************************************************************
//******************************************************************************
// https://github.com/Bitcoin-ABC/bitcoin-abc/blob/019603ed8d7227ce2d813d5c8cf6f46015349a82/src/script/interpreter.cpp#L1448
template <class T> uint256 GetPrevoutHash(const T &txTo) {
    CHashWriter ss(SER_GETHASH, 0);
    for (const auto &txin : txTo.vin) {
        ss << txin.prevout;
    }
    return ss.GetHash();
}
// https://github.com/Bitcoin-ABC/bitcoin-abc/blob/019603ed8d7227ce2d813d5c8cf6f46015349a82/src/script/interpreter.cpp#L1456
template <class T> uint256 GetSequenceHash(const T &txTo) {
    CHashWriter ss(SER_GETHASH, 0);
    for (const auto &txin : txTo.vin) {
        ss << txin.nSequence;
    }
    return ss.GetHash();
}
// https://github.com/Bitcoin-ABC/bitcoin-abc/blob/019603ed8d7227ce2d813d5c8cf6f46015349a82/src/script/interpreter.cpp#L1464
template <class T> uint256 GetOutputsHash(const T &txTo) {
    CHashWriter ss(SER_GETHASH, 0);
    for (const auto &txout : txTo.vout) {
        ss << txout;
    }
    return ss.GetHash();
}

//******************************************************************************
//******************************************************************************
typedef struct { // helper for bch cache pointer
    uint256 hashPrevouts;
    uint256 hashSequence;
    uint256 hashOutputs;
} cache_t;
// Reference: https://github.com/Bitcoin-ABC/bitcoin-abc/blob/019603ed8d7227ce2d813d5c8cf6f46015349a82/src/script/interpreter.cpp#L1488
uint256 SignatureHash(const CScript &scriptCode, const CTransactionPtr & tx,
                      unsigned int nIn, SigHashType sigHashType,
                      const CAmount amount)
{
    // XBRIDGE
    auto & txTo = *tx;
    uint32_t flags = SCRIPT_ENABLE_SIGHASH_FORKID | SCRIPT_ENABLE_REPLAY_PROTECTION;
    cache_t *cache = nullptr;
    // END XBRIDGE

    if (flags & SCRIPT_ENABLE_REPLAY_PROTECTION) {
        // Legacy chain's value for fork id must be of the form 0xffxxxx.
        // By xoring with 0xdead, we ensure that the value will be different
        // from the original one, even if it already starts with 0xff.
        uint32_t newForkValue = sigHashType.getForkValue() ^ 0xdead;
        sigHashType = sigHashType.withForkValue(0xff0000 | newForkValue);
    }

    if (sigHashType.hasForkId() && (flags & SCRIPT_ENABLE_SIGHASH_FORKID)) {
        uint256 hashPrevouts;
        uint256 hashSequence;
        uint256 hashOutputs;

        if (!sigHashType.hasAnyoneCanPay()) {
            hashPrevouts = cache ? cache->hashPrevouts : GetPrevoutHash(txTo);
        }

        if (!sigHashType.hasAnyoneCanPay() &&
            (sigHashType.getBaseType() != BaseSigHashType::SINGLE) &&
            (sigHashType.getBaseType() != BaseSigHashType::NONE)) {
            hashSequence = cache ? cache->hashSequence : GetSequenceHash(txTo);
        }

        if ((sigHashType.getBaseType() != BaseSigHashType::SINGLE) &&
            (sigHashType.getBaseType() != BaseSigHashType::NONE)) {
            hashOutputs = cache ? cache->hashOutputs : GetOutputsHash(txTo);
        } else if ((sigHashType.getBaseType() == BaseSigHashType::SINGLE) &&
                   (nIn < txTo.vout.size())) {
            CHashWriter ss(SER_GETHASH, 0);
            ss << txTo.vout[nIn];
            hashOutputs = ss.GetHash();
        }

        CHashWriter ss(SER_GETHASH, 0);
        // Version
        ss << txTo.nVersion;
        // Input prevouts/nSequence (none/all, depending on flags)
        ss << hashPrevouts;
        ss << hashSequence;
        // The input being signed (replacing the scriptSig with scriptCode +
        // amount). The prevout may already be contained in hashPrevout, and the
        // nSequence may already be contain in hashSequence.
        ss << txTo.vin[nIn].prevout;
        ss << scriptCode;
        ss << amount;
        ss << txTo.vin[nIn].nSequence;
        // Outputs (none/one/all, depending on flags)
        ss << hashOutputs;
        // Locktime
        ss << txTo.nLockTime;
        // Sighash type
        ss << sigHashType;

        TXLOG() << "===============FOR DEV===============" << std::endl;
        TXLOG() << "===============FOR DEV===============" << std::endl;
        TXLOG() << std::to_string(txTo.nVersion)
                  << " " << hashPrevouts.GetHex()
                  << " " << hashSequence.GetHex()
                  << " " << txTo.vin[nIn].prevout.ToString()
                  << " " << HexStr(scriptCode)
                  << " " << std::to_string(amount)
                  << " " << std::to_string(txTo.vin[nIn].nSequence)
                  << " " << hashOutputs.GetHex()
                  << " " << std::to_string(txTo.nLockTime)
                  << " " << std::to_string(uint8_t(sigHashType.getRawSigHashType()))
                  << " " << std::endl;
        TXLOG() << "===============END FOR DEV===============" << std::endl;
        TXLOG() << "===============END FOR DEV===============" << std::endl;

        return ss.GetHash();
    }

    // XBRIDGE should never end up here
    return {};
    // XBRIDGE

//    static const uint256 one(uint256S(
//            "0000000000000000000000000000000000000000000000000000000000000001"));
//
//    // Check for invalid use of SIGHASH_SINGLE
//    if ((sigHashType.getBaseType() == BaseSigHashType::SINGLE) &&
//        (nIn >= txTo.vout.size())) {
//        //  nOut out of range
//        return one;
//    }
//
//    // Wrapper to serialize only the necessary parts of the transaction being
//    // signed
//    CTransactionSignatureSerializer<T> txTmp(txTo, scriptCode, nIn,
//                                             sigHashType);
//
//    // Serialize and hash
//    CHashWriter ss(SER_GETHASH, 0);
//    ss << txTmp << sigHashType;
//    return ss.GetHash();
}

} // namespace


//******************************************************************************
//******************************************************************************
namespace {

typedef std::vector<unsigned char> data;

//******************************************************************************
//******************************************************************************
/**
 * The cashaddr character set for encoding.
 */
const char *CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

//******************************************************************************
//******************************************************************************
/**
 * The cashaddr character set for decoding.
 */
const int8_t CHARSET_REV[128] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 15, -1, 10, 17, 21, 20, 26, 30, 7,
    5,  -1, -1, -1, -1, -1, -1, -1, 29, -1, 24, 13, 25, 9,  8,  23, -1, 18, 22,
    31, 27, 19, -1, 1,  0,  3,  16, 11, 28, 12, 14, 6,  4,  2,  -1, -1, -1, -1,
    -1, -1, 29, -1, 24, 13, 25, 9,  8,  23, -1, 18, 22, 31, 27, 19, -1, 1,  0,
    3,  16, 11, 28, 12, 14, 6,  4,  2,  -1, -1, -1, -1, -1};

//******************************************************************************
//******************************************************************************
/**
 * Concatenate two byte arrays.
 */
data Cat(data x, const data &y) {
    x.insert(x.end(), y.begin(), y.end());
    return x;
}

//******************************************************************************
//******************************************************************************
/**
 * This function will compute what 8 5-bit values to XOR into the last 8 input
 * values, in order to make the checksum 0. These 8 values are packed together
 * in a single 40-bit integer. The higher bits correspond to earlier values.
 */
uint64_t PolyMod(const data &v) {
    /**
     * The input is interpreted as a list of coefficients of a polynomial over F
     * = GF(32), with an implicit 1 in front. If the input is [v0,v1,v2,v3,v4],
     * that polynomial is v(x) = 1*x^5 + v0*x^4 + v1*x^3 + v2*x^2 + v3*x + v4.
     * The implicit 1 guarantees that [v0,v1,v2,...] has a distinct checksum
     * from [0,v0,v1,v2,...].
     *
     * The output is a 40-bit integer whose 5-bit groups are the coefficients of
     * the remainder of v(x) mod g(x), where g(x) is the cashaddr generator, x^8
     * + {19}*x^7 + {3}*x^6 + {25}*x^5 + {11}*x^4 + {25}*x^3 + {3}*x^2 + {19}*x
     * + {1}. g(x) is chosen in such a way that the resulting code is a BCH
     * code, guaranteeing detection of up to 4 errors within a window of 1025
     * characters. Among the various possible BCH codes, one was selected to in
     * fact guarantee detection of up to 5 errors within a window of 160
     * characters and 6 erros within a window of 126 characters. In addition,
     * the code guarantee the detection of a burst of up to 8 errors.
     *
     * Note that the coefficients are elements of GF(32), here represented as
     * decimal numbers between {}. In this finite field, addition is just XOR of
     * the corresponding numbers. For example, {27} + {13} = {27 ^ 13} = {22}.
     * Multiplication is more complicated, and requires treating the bits of
     * values themselves as coefficients of a polynomial over a smaller field,
     * GF(2), and multiplying those polynomials mod a^5 + a^3 + 1. For example,
     * {5} * {26} = (a^2 + 1) * (a^4 + a^3 + a) = (a^4 + a^3 + a) * a^2 + (a^4 +
     * a^3 + a) = a^6 + a^5 + a^4 + a = a^3 + 1 (mod a^5 + a^3 + 1) = {9}.
     *
     * During the course of the loop below, `c` contains the bitpacked
     * coefficients of the polynomial constructed from just the values of v that
     * were processed so far, mod g(x). In the above example, `c` initially
     * corresponds to 1 mod (x), and after processing 2 inputs of v, it
     * corresponds to x^2 + v0*x + v1 mod g(x). As 1 mod g(x) = 1, that is the
     * starting value for `c`.
     */
    uint64_t c = 1;
    for (uint8_t d : v) {
        /**
         * We want to update `c` to correspond to a polynomial with one extra
         * term. If the initial value of `c` consists of the coefficients of
         * c(x) = f(x) mod g(x), we modify it to correspond to
         * c'(x) = (f(x) * x + d) mod g(x), where d is the next input to
         * process.
         *
         * Simplifying:
         * c'(x) = (f(x) * x + d) mod g(x)
         *         ((f(x) mod g(x)) * x + d) mod g(x)
         *         (c(x) * x + d) mod g(x)
         * If c(x) = c0*x^5 + c1*x^4 + c2*x^3 + c3*x^2 + c4*x + c5, we want to
         * compute
         * c'(x) = (c0*x^5 + c1*x^4 + c2*x^3 + c3*x^2 + c4*x + c5) * x + d
         *                                                             mod g(x)
         *       = c0*x^6 + c1*x^5 + c2*x^4 + c3*x^3 + c4*x^2 + c5*x + d
         *                                                             mod g(x)
         *       = c0*(x^6 mod g(x)) + c1*x^5 + c2*x^4 + c3*x^3 + c4*x^2 +
         *                                                             c5*x + d
         * If we call (x^6 mod g(x)) = k(x), this can be written as
         * c'(x) = (c1*x^5 + c2*x^4 + c3*x^3 + c4*x^2 + c5*x + d) + c0*k(x)
         */

        // First, determine the value of c0:
        uint8_t c0 = c >> 35;

        // Then compute c1*x^5 + c2*x^4 + c3*x^3 + c4*x^2 + c5*x + d:
        c = ((c & 0x07ffffffff) << 5) ^ d;

        // Finally, for each set bit n in c0, conditionally add {2^n}k(x):
        if (c0 & 0x01) {
            // k(x) = {19}*x^7 + {3}*x^6 + {25}*x^5 + {11}*x^4 + {25}*x^3 +
            //        {3}*x^2 + {19}*x + {1}
            c ^= 0x98f2bc8e61;
        }

        if (c0 & 0x02) {
            // {2}k(x) = {15}*x^7 + {6}*x^6 + {27}*x^5 + {22}*x^4 + {27}*x^3 +
            //           {6}*x^2 + {15}*x + {2}
            c ^= 0x79b76d99e2;
        }

        if (c0 & 0x04) {
            // {4}k(x) = {30}*x^7 + {12}*x^6 + {31}*x^5 + {5}*x^4 + {31}*x^3 +
            //           {12}*x^2 + {30}*x + {4}
            c ^= 0xf33e5fb3c4;
        }

        if (c0 & 0x08) {
            // {8}k(x) = {21}*x^7 + {24}*x^6 + {23}*x^5 + {10}*x^4 + {23}*x^3 +
            //           {24}*x^2 + {21}*x + {8}
            c ^= 0xae2eabe2a8;
        }

        if (c0 & 0x10) {
            // {16}k(x) = {3}*x^7 + {25}*x^6 + {7}*x^5 + {20}*x^4 + {7}*x^3 +
            //            {25}*x^2 + {3}*x + {16}
            c ^= 0x1e4f43e470;
        }
    }

    /**
     * PolyMod computes what value to xor into the final values to make the
     * checksum 0. However, if we required that the checksum was 0, it would be
     * the case that appending a 0 to a valid list of values would result in a
     * new valid list. For that reason, cashaddr requires the resulting checksum
     * to be 1 instead.
     */
    return c ^ 1;
}

//******************************************************************************
//******************************************************************************
/**
 * Convert to lower case.
 *
 * Assume the input is a character.
 */
inline uint8_t LowerCase(uint8_t c)
{
    // ASCII black magic.
    return c | 0x20;
}

//******************************************************************************
//******************************************************************************
/**
 * Expand the address prefix for the checksum computation.
 */
data ExpandPrefix(const std::string &prefix)
{
    data ret;
    ret.resize(prefix.size() + 1);
    for (size_t i = 0; i < prefix.size(); ++i)
    {
        ret[i] = prefix[i] & 0x1f;
    }

    ret[prefix.size()] = 0;
    return ret;
}

//******************************************************************************
//******************************************************************************
/**
 * Verify a checksum.
 */
bool VerifyChecksum(const std::string &prefix, const data &payload)
{
    return PolyMod(Cat(ExpandPrefix(prefix), payload)) == 0;
}

//******************************************************************************
//******************************************************************************
/**
 * Create a checksum.
 */
data CreateChecksum(const std::string &prefix, const data &payload) {
    data enc = Cat(ExpandPrefix(prefix), payload);
    // Append 8 zeroes.
    enc.resize(enc.size() + 8);
    // Determine what to XOR into those 8 zeroes.
    uint64_t mod = PolyMod(enc);
    data ret(8);
    for (size_t i = 0; i < 8; ++i) {
        // Convert the 5-bit groups in mod to checksum values.
        ret[i] = (mod >> (5 * (7 - i))) & 0x1f;
    }

    return ret;
}

} // namespace


//******************************************************************************
//******************************************************************************
// TODO Blocknet BCH disable cashaddr
namespace cashaddr {
//
////******************************************************************************
////******************************************************************************
///**
// * Encode a cashaddr string.
// */
//std::string Encode(const std::string &prefix, const data &payload)
//{
//    data checksum = CreateChecksum(prefix, payload);
//    data combined = Cat(payload, checksum);
//    std::string ret = prefix + ':';
//
//    ret.reserve(ret.size() + combined.size());
//    for (uint8_t c : combined) {
//        ret += CHARSET[c];
//    }
//
//    return ret;
//}
//
////******************************************************************************
////******************************************************************************
///**
// * Decode a cashaddr string.
// */
//std::pair<std::string, data> Decode(const std::string &str,
//                                    const std::string &default_prefix) {
//    // Go over the string and do some sanity checks.
//    bool lower = false, upper = false, hasNumber = false;
//    size_t prefixSize = 0;
//    for (size_t i = 0; i < str.size(); ++i) {
//        uint8_t c = str[i];
//        if (c >= 'a' && c <= 'z') {
//            lower = true;
//            continue;
//        }
//
//        if (c >= 'A' && c <= 'Z') {
//            upper = true;
//            continue;
//        }
//
//        if (c >= '0' && c <= '9') {
//            // We cannot have numbers in the prefix.
//            hasNumber = true;
//            continue;
//        }
//
//        if (c == ':') {
//            // The separator cannot be the first character, cannot have number
//            // and there must not be 2 separators.
//            if (hasNumber || i == 0 || prefixSize != 0) {
//                return {};
//            }
//
//            prefixSize = i;
//            continue;
//        }
//
//        // We have an unexpected character.
//        return {};
//    }
//
//    // We can't have both upper case and lowercase.
//    if (upper && lower) {
//        return {};
//    }
//
//    // Get the prefix.
//    std::string prefix;
//    if (prefixSize == 0) {
//        prefix = default_prefix;
//    } else {
//        prefix.reserve(prefixSize);
//        for (size_t i = 0; i < prefixSize; ++i) {
//            prefix += LowerCase(str[i]);
//        }
//
//        // Now add the ':' in the size.
//        prefixSize++;
//    }
//
//    // Decode values.
//    const size_t valuesSize = str.size() - prefixSize;
//    data values(valuesSize);
//    for (size_t i = 0; i < valuesSize; ++i) {
//        uint8_t c = str[i + prefixSize];
//        // We have an invalid char in there.
//        if (c > 127 || CHARSET_REV[c] == -1) {
//            return {};
//        }
//
//        values[i] = CHARSET_REV[c];
//    }
//
//    // Verify the checksum.
//    if (!VerifyChecksum(prefix, values)) {
//        return {};
//    }
//
//    return {std::move(prefix), data(values.begin(), values.end() - 8)};
//}
//
} // namespace cashaddr


//******************************************************************************
//******************************************************************************
namespace {
//
///**
// * Convert from one power-of-2 number base to another.
// *
// * If padding is enabled, this always return true. If not, then it returns true
// * of all the bits of the input are encoded in the output.
// */
//template <int frombits, int tobits, bool pad, typename O, typename I>
//bool ConvertBits(O &out, I it, I end) {
//    size_t acc = 0;
//    size_t bits = 0;
//    constexpr size_t maxv = (1 << tobits) - 1;
//    constexpr size_t max_acc = (1 << (frombits + tobits - 1)) - 1;
//    while (it != end) {
//        acc = ((acc << frombits) | *it) & max_acc;
//        bits += frombits;
//        while (bits >= tobits) {
//            bits -= tobits;
//            out.push_back((acc >> bits) & maxv);
//        }
//        ++it;
//    }
//
//    // We have remaining bits to encode but do not pad.
//    if (!pad && bits) {
//        return false;
//    }
//
//    // We have remaining bits to encode so we do pad.
//    if (pad && bits) {
//        out.push_back((acc << (tobits - bits)) & maxv);
//    }
//
//    return true;
//}
//
//// Convert the data part to a 5 bit representation.
//template <class T>
//std::vector<uint8_t> PackAddrData(const T &id, uint8_t type)
//{
//    uint8_t version_byte(type << 3);
//    size_t size = id.size();
//    uint8_t encoded_size = 0;
//    switch (size * 8) {
//        case 160:
//            encoded_size = 0;
//            break;
//        case 192:
//            encoded_size = 1;
//            break;
//        case 224:
//            encoded_size = 2;
//            break;
//        case 256:
//            encoded_size = 3;
//            break;
//        case 320:
//            encoded_size = 4;
//            break;
//        case 384:
//            encoded_size = 5;
//            break;
//        case 448:
//            encoded_size = 6;
//            break;
//        case 512:
//            encoded_size = 7;
//            break;
//        default:
//            throw std::runtime_error(
//                "Error packing cashaddr: invalid address length");
//    }
//    version_byte |= encoded_size;
//    std::vector<uint8_t> data = {version_byte};
//    data.insert(data.end(), std::begin(id), std::end(id));
//
//    std::vector<uint8_t> converted;
//    // Reserve the number of bytes required for a 5-bit packed version of a
//    // hash, with version byte.  Add half a byte(4) so integer math provides
//    // the next multiple-of-5 that would fit all the data.
//    converted.reserve(((size + 1) * 8 + 4) / 5);
//    ConvertBits<8, 5, true>(converted, std::begin(data), std::end(data));
//
//    return converted;
//}
//
////******************************************************************************
////******************************************************************************
//enum CashAddrType : uint8_t { PUBKEY_TYPE = 0, SCRIPT_TYPE = 1 };
//
////******************************************************************************
////******************************************************************************
//struct CashAddrContent
//{
//    CashAddrType type;
//    std::vector<uint8_t> hash;
//};
//
////******************************************************************************
////******************************************************************************
//CashAddrContent DecodeCashAddrContent(const std::string & addr,
//                                      const std::string & prefix)
//{
//    std::string outprefix;
//    std::vector<uint8_t> payload;
//    std::tie(outprefix, payload) = cashaddr::Decode(addr, prefix);
//
//    if (outprefix != prefix)
//    {
//        return {};
//    }
//
//    if (payload.empty())
//    {
//        return {};
//    }
//
//    // Check that the padding is zero.
//    size_t extrabits = payload.size() * 5 % 8;
//    if (extrabits >= 5)
//    {
//        // We have more padding than allowed.
//        return {};
//    }
//
//    uint8_t last = payload.back();
//    uint8_t mask = (1 << extrabits) - 1;
//    if (last & mask)
//    {
//        // We have non zero bits as padding.
//        return {};
//    }
//
//    std::vector<uint8_t> data;
//    data.reserve(payload.size() * 5 / 8);
//    ConvertBits<5, 8, false>(data, begin(payload), end(payload));
//
//    // Decode type and size from the version.
//    uint8_t version = data[0];
//    if (version & 0x80)
//    {
//        // First bit is reserved.
//        return {};
//    }
//
//    auto type = CashAddrType((version >> 3) & 0x1f);
//    uint32_t hash_size = 20 + 4 * (version & 0x03);
//    if (version & 0x04)
//    {
//        hash_size *= 2;
//    }
//
//    // Check that we decoded the exact number of bytes we expected.
//    if (data.size() != hash_size + 1)
//    {
//        return {};
//    }
//
//    // Pop the version.
//    data.erase(data.begin());
//    return {type, std::move(data)};
//}
//
} // namespace

//******************************************************************************
//******************************************************************************
xbridge::CTransactionPtr createTransaction(const bool txWithTimeField);
xbridge::CTransactionPtr createTransaction(const std::vector<XTxIn> & inputs,
                                           const std::vector<std::pair<std::string, double> > & outputs,
                                           const uint64_t COIN,
                                           const uint32_t txversion,
                                           const uint32_t lockTime,
                                           const bool txWithTimeField);

//******************************************************************************
//******************************************************************************
BchWalletConnector::BchWalletConnector() : BtcWalletConnector() {}

// TODO Blocknet BCH disable cashaddr
// https://github.com/blocknetdx/blocknet/blob/2b15bf86273614dfcb959f4d219663246e5c4ac1/src/xbridge/xbridgewalletconnectorbch.cpp#L801

//******************************************************************************
//******************************************************************************
xbridge::CTransactionPtr createTransaction(const bool txWithTimeField = false);
xbridge::CTransactionPtr createTransaction(const WalletConnector & conn,
                                           const std::vector<XTxIn> & inputs,
                                           const std::vector<std::pair<std::string, double> >  & outputs,
                                           const uint64_t COIN,
                                           const uint32_t txversion,
                                           const uint32_t lockTime,
                                           const bool txWithTimeField = false);

//******************************************************************************
//******************************************************************************
bool BchWalletConnector::createRefundTransaction(const std::vector<XTxIn> & inputs,
                                                 const std::vector<std::pair<std::string, double> > & outputs,
                                                 const std::vector<unsigned char> & mpubKey,
                                                 const std::vector<unsigned char> & mprivKey,
                                                 const std::vector<unsigned char> & innerScript,
                                                 const uint32_t lockTime,
                                                 std::string & txId,
                                                 std::string & rawTx)
{
    xbridge::CTransactionPtr txUnsigned = createTransaction(*this, inputs, outputs, COIN, txVersion, lockTime, txWithTimeField);
    // Correctly set tx input sequence. If lockTime is specified sequence must be 2^32-2, otherwise 2^32-1 (Final)
    uint32_t sequence = lockTime > 0 ? xbridge::SEQUENCE_FINAL-1 : xbridge::SEQUENCE_FINAL;
    txUnsigned->vin[0].nSequence = sequence;

    CScript inner(innerScript.begin(), innerScript.end());

    CScript redeem;
    {
        CScript tmp;
        tmp << ToByteVector(mpubKey) << OP_TRUE << ToByteVector(inner);

        SigHashType sigHashType = SigHashType(SIGHASH_ALL).withForkId();
        std::vector<unsigned char> signature;
        uint256 hash = SignatureHash(inner, txUnsigned, 0, sigHashType, inputs[0].amount*COIN);
        if (!m_cp.sign(mprivKey, hash, signature))
        {
            // cancel transaction
            LOG() << "sign transaction error, transaction canceled " << __FUNCTION__;
//                sendCancelTransaction(xtx, crNotSigned);
            return false;
        }

        signature.push_back(uint8_t(sigHashType.getRawSigHashType()));

        redeem << signature;
        redeem += tmp;
    }

    xbridge::CTransactionPtr tx(createTransaction(txWithTimeField));
    if (!tx)
    {
        ERR() << "transaction not created " << __FUNCTION__;
//            sendCancelTransaction(xtx, crBadSettings);
        return false;
    }
    tx->nVersion  = txUnsigned->nVersion;
    tx->nTime     = txUnsigned->nTime;
    tx->vin.push_back(CTxIn(txUnsigned->vin[0].prevout, redeem, sequence));
    tx->vout      = txUnsigned->vout;
    tx->nLockTime = txUnsigned->nLockTime;

    rawTx = tx->toString();

    std::string json;
    std::string reftxid;
    if (!rpc::decodeRawTransaction(m_user, m_passwd, m_ip, m_port, rawTx, reftxid, json))
    {
        LOG() << "decode signed transaction error, transaction canceled " << __FUNCTION__;
//            sendCancelTransaction(xtx, crRpcError);
            return true;
    }

    txId  = reftxid;

    return true;
}

//******************************************************************************
//******************************************************************************
bool BchWalletConnector::createPaymentTransaction(const std::vector<XTxIn> & inputs,
                                                  const std::vector<std::pair<std::string, double> > & outputs,
                                                  const std::vector<unsigned char> & mpubKey,
                                                  const std::vector<unsigned char> & mprivKey,
                                                  const std::vector<unsigned char> & xpubKey,
                                                  const std::vector<unsigned char> & innerScript,
                                                  std::string & txId,
                                                  std::string & rawTx)
{
    xbridge::CTransactionPtr txUnsigned = createTransaction(*this, inputs, outputs, COIN, txVersion, 0, txWithTimeField);

    CScript inner(innerScript.begin(), innerScript.end());

    SigHashType sigHashType = SigHashType(SIGHASH_ALL).withForkId();
    std::vector<unsigned char> signature;
    uint256 hash = SignatureHash(inner, txUnsigned, 0, sigHashType, inputs[0].amount*COIN);
    if (!m_cp.sign(mprivKey, hash, signature))
    {
        // cancel transaction
        LOG() << "sign transaction error, transaction canceled " << __FUNCTION__;
//                sendCancelTransaction(xtx, crNotSigned);
        return false;
    }

    signature.push_back(uint8_t(sigHashType.getRawSigHashType()));

    CScript redeem;
    redeem << xpubKey
           << signature << mpubKey
           << OP_FALSE << ToByteVector(inner);

    xbridge::CTransactionPtr tx(createTransaction(txWithTimeField));
    if (!tx)
    {
        ERR() << "transaction not created " << __FUNCTION__;
//                sendCancelTransaction(xtx, crBadSettings);
        return false;
    }
    tx->nVersion  = txUnsigned->nVersion;
    tx->nTime     = txUnsigned->nTime;
    tx->vin.push_back(CTxIn(txUnsigned->vin[0].prevout, redeem));
    tx->vout      = txUnsigned->vout;

    rawTx = tx->toString();

    TXLOG() << "===============FOR DEV===============" << std::endl;
    TXLOG() << "===============FOR DEV===============" << std::endl;
    TXLOG() << "sighash type: " << std::to_string(sigHashType.getRawSigHashType()) << std::endl;
    TXLOG() << "sighash: " << hash.GetHex() << std::endl;
    TXLOG() << "amount: " << std::to_string(inputs[0].amount) << " " << std::to_string(inputs[0].amount*COIN) << std::endl;
    TXLOG() << "signature: " << HexStr(signature) << std::endl;
    TXLOG() << "mprivkey: " << HexStr(mprivKey) << std::endl;
    TXLOG() << "mpubkey: " << HexStr(mpubKey) << std::endl;
    TXLOG() << "xpubkey: " << HexStr(xpubKey) << std::endl;
    TXLOG() << "innerscript: " << HexStr(ToByteVector(inner)) << std::endl;
    TXLOG() << "p2sh scriptPubKey: " << HexStr(redeem) << std::endl;
    TXLOG() << "rawtx: " << rawTx << std::endl;
    TXLOG() << "===============END FOR DEV===============" << std::endl;
    TXLOG() << "===============END FOR DEV===============" << std::endl;

    std::string json;
    std::string paytxid;
    if (!rpc::decodeRawTransaction(m_user, m_passwd, m_ip, m_port, rawTx, paytxid, json))
    {
            LOG() << "decode signed transaction error, transaction canceled " << __FUNCTION__;
//                sendCancelTransaction(xtx, crRpcError);
        return true;
    }

    txId  = paytxid;

    return true;
}

std::string BchWalletConnector::echo() {
//    return HexStr(toXAddr("1PwwMpUEoiVf9uJfqYapWxDgoC8coR6Afe"));

    auto mprivKey = ParseHex("2a369b62ff5ba6ba2d0977a69bd1ffabf590ea0f99d6394a38402741b4a1d796");
    std::vector<XTxIn> inputs;
    inputs.emplace_back(uint256().GetHex(), 0, (double)12000/(double)COIN);
    std::vector<std::pair<std::string, double>> outputs;
    outputs.emplace_back("1PwwMpUEoiVf9uJfqYapWxDgoC8coR6Afe", (double)12000/(double)COIN);
    CKey key; key.Set(mprivKey.begin(), mprivKey.end(), true);
    auto mpubKey = ToByteVector(key.GetPubKey());
    auto xpubKey = mpubKey;
    auto otherPubKey = mpubKey;
    std::string txId, rawTx;
    auto innerScript =
            CScript() << OP_IF
                        << 600000 << OP_CHECKLOCKTIMEVERIFY << OP_DROP
                        << OP_DUP << OP_HASH160 << getKeyId(otherPubKey) << OP_EQUALVERIFY << OP_CHECKSIG
                      << OP_ELSE
                        << OP_DUP << OP_HASH160 << getKeyId(otherPubKey) << OP_EQUALVERIFY << OP_CHECKSIGVERIFY
                        << OP_SIZE << 33 << OP_EQUALVERIFY << OP_HASH160 << getKeyId(xpubKey) << OP_EQUAL
                      << OP_ENDIF;
    createPaymentTransaction(inputs, outputs, mpubKey, mprivKey, xpubKey, ToByteVector(innerScript), txId, rawTx);
    return txId + "\n"
         + rawTx + "\n"
         + "mpubkey " + HexStr(mpubKey) + "\n"
         + "xpubkey " + HexStr(xpubKey) + "\n"
         + "opubkey " + HexStr(otherPubKey);
    // 2 e15426c0d1fbb5b78943c8425a9232fdfc1670d77f987707292a77ec6dce5aca 445066705e799022b7095f7ceca255149f43acfc47e7f59e551f7bce2930b13b COutPoint(0000000000, 0) 76a9148abbcbe0a89bade388d7ae825aef73758b18cbbd88ad82012188a9148abbcbe0a89bade388d7ae825aef73758b18cbbd87 12000 4294967295 39ece86a4ec4efe82e8458fbcf7c590629386a9202b4b09fc181902db7f4a247 0 65
    //Sighash: a9b2bf628d9fc4574c1194dca949624598cceb5177e9cf68df181fa19f52f898

//    auto privkey = ParseHex("2a369b62ff5ba6ba2d0977a69bd1ffabf590ea0f99d6394a38402741b4a1d796");
//    SigHashType sigHashType = SigHashType(SIGHASH_ALL).withForkId();
//    std::vector<unsigned char> signature;
//    std::vector<XTxIn> inputs;
//    inputs.emplace_back(uint256().GetHex(), 0, 12000);
//    std::vector<std::pair<std::string, double>> outputs;
//    outputs.emplace_back("1PwwMpUEoiVf9uJfqYapWxDgoC8coR6Afe", (double)12000/(double)COIN);
//    xbridge::CTransactionPtr txUnsigned = createTransaction(*this, inputs, outputs, COIN, txVersion, 0, txWithTimeField);
//    uint256 hash = SignatureHash(CScript(), txUnsigned, 0, sigHashType, 12000);
//    std::cout << hash.GetHex() << std::endl;
//    if (!m_cp.sign(privkey, hash, signature))
//        return "";
//    signature.push_back(uint8_t(sigHashType.getRawSigHashType()));
//    return HexStr(signature);
}

} // namespace xbridge
