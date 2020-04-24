// Copyright (c) 2017-2019 The Blocknet developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//*****************************************************************************
//*****************************************************************************

#include <xbridge/util/xutil.h>

#include <xbridge/xbridgetransactiondescr.h>

#include <amount.h>

#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

#include <openssl/rand.h>

#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/binary_from_base64.hpp>
#include <boost/archive/iterators/transform_width.hpp>
#include <boost/archive/iterators/ostream_iterator.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/date_time/time_facet.hpp>
#include <boost/locale.hpp>
#include <boost/numeric/conversion/cast.hpp>

#ifndef WIN32
#include <execinfo.h>
#endif

//*****************************************************************************
//*****************************************************************************
namespace xbridge
{

using namespace json_spirit;
std::locale loc;

//******************************************************************************
//******************************************************************************
void init()
{
    try
    {
        loc = std::locale ("en_US.UTF8");
    }
    catch (std::runtime_error & e)
    {
        LOG() << "use default locale, " << e.what();
        loc = std::locale (loc, "", std::locale::ctype);
    }
}

//******************************************************************************
//******************************************************************************
std::wstring wide_string(std::string const &s)//, std::locale const &loc)
{
    if (s.empty())
    {
        return std::wstring();
    }

    std::ctype<wchar_t> const &facet = std::use_facet<std::ctype<wchar_t> >(loc);
    char const *first = s.c_str();
    char const *last = first + s.size();
    std::vector<wchar_t> result(s.size());

    facet.widen(first, last, &result[0]);

    return std::wstring(result.begin(), result.end());
}

//******************************************************************************
//******************************************************************************
//std::string narrow_string(std::wstring const &s, char default_char)//, std::locale const &loc, char default_char)
//{
//    if (s.empty())
//    {
//        return std::string();
//    }

//    std::ctype<wchar_t> const &facet = std::use_facet<std::ctype<wchar_t> >(loc);
//    wchar_t const *first = s.c_str();
//    wchar_t const *last = first + s.size();
//    std::vector<char> result(s.size());

//    facet.narrow(first, last, default_char, &result[0]);

//    return std::string(result.begin(), result.end());
//}

//******************************************************************************
//******************************************************************************
std::string mb_string(std::string const &s)
{
    return mb_string(wide_string(s));
}

//******************************************************************************
//******************************************************************************
std::string mb_string(std::wstring const &s)
{
    return boost::locale::conv::utf_to_utf<char>(s);
}

//*****************************************************************************
//*****************************************************************************
const std::string base64_padding[] = {"", "==","="};

//*****************************************************************************
//*****************************************************************************
std::string base64_encode(const std::vector<unsigned char> & s)
{
    return base64_encode(std::string((char *)&s[0], s.size()));
}

//*****************************************************************************
//*****************************************************************************
std::string base64_encode(const std::string& s)
{
    namespace bai = boost::archive::iterators;

    std::stringstream os;

    // convert binary values to base64 characters
    typedef bai::base64_from_binary
    // retrieve 6 bit integers from a sequence of 8 bit bytes
    <bai::transform_width<const char *, 6, 8> > base64_enc; // compose all the above operations in to a new iterator

    std::copy(base64_enc(s.c_str()), base64_enc(s.c_str() + s.size()),
            std::ostream_iterator<char>(os));

    os << base64_padding[s.size() % 3];
    return os.str();
}

//*****************************************************************************
//*****************************************************************************
std::string base64_decode(const std::string& s)
{
    try
    {
        namespace bai = boost::archive::iterators;

        std::stringstream os;

        typedef bai::transform_width<bai::binary_from_base64<const char *>, 8, 6> base64_dec;

        unsigned int size = s.size();

        // Remove the padding characters, cf. https://svn.boost.org/trac/boost/ticket/5629
        if (size && s[size - 1] == '=')
        {
            --size;
            if (size && s[size - 1] == '=')
            {
                --size;
            }
        }
        if (size == 0)
        {
            return std::string();
        }

        std::copy(base64_dec(s.data()), base64_dec(s.data() + size),
                std::ostream_iterator<char>(os));

        return os.str();
    }
    // catch (std::exception &)
    catch (...)
    {
    }
    return std::string();
}

std::string to_str(const std::vector<unsigned char> & obj)
{
    return base64_encode(obj);
}

std::string iso8601(const boost::posix_time::ptime &time)
{
    auto ms = time.time_of_day().total_milliseconds() % 1000;
    auto tm = to_tm(time);
    std::ostringstream ss;
#if __GNUC__ < 5
    char buf[sizeof "2019-12-15T12:00:00"];
    strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
    ss << std::string(buf);
#else
    ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
#endif
    ss << '.' << std::setfill('0') << std::setw(3) << ms; // add milliseconds
    ss << 'Z';
    return ss.str();
}

std::string xBridgeStringValueFromAmount(uint64_t amount)
{
    std::stringstream ss;
    ss << std::fixed << std::setprecision(xBridgeSignificantDigits(xbridge::TransactionDescr::COIN)) << xBridgeValueFromAmount(amount);
    return ss.str();
}

std::string xBridgeStringValueFromPrice(double price)
{
    std::stringstream ss;
    ss << std::fixed << std::setprecision(xBridgeSignificantDigits(xbridge::TransactionDescr::COIN)) << price;
    return ss.str();
}

std::string xBridgeStringValueFromPrice(double price, uint64_t denomination)
{
    std::stringstream ss;
    ss << std::fixed << std::setprecision(xBridgeSignificantDigits(denomination)) << price;
    return ss.str();
}

double xBridgeValueFromAmount(uint64_t amount) {
    return boost::numeric_cast<double>(amount) /
            boost::numeric_cast<double>(xbridge::TransactionDescr::COIN);
}

uint64_t xBridgeAmountFromReal(double val)
{
    double d = val * boost::numeric_cast<double>(xbridge::TransactionDescr::COIN);
    auto r = (int64_t)(d > 0 ? d + 0.5 : d - 0.5);
    return (uint64_t)r;
}

bool xBridgeValidCoin(const std::string coin)
{
    bool f = false;
    int n = 0;
    int j = 0; // count 0s
    // count precision digits, ignore trailing 0s
    for (const char &c : coin) {
        if (!f && c == '.')
            f = true;
        else if (f) {
            n++;
            if (c == '0')
                j++;
            else
                j = 0;
        }
    }
    return n - j <= xBridgeSignificantDigits(xbridge::TransactionDescr::COIN);
}

unsigned int xBridgeSignificantDigits(const int64_t amount)
{
    unsigned int n = 0;
    int64_t i = amount;

    do {
        n++;
        i /= 10;
    } while (i > 1);

    return n;
}

uint64_t timeToInt(const boost::posix_time::ptime& time)
{
    boost::posix_time::ptime start(boost::gregorian::date(1970,1,1));
    boost::posix_time::time_duration timeFromEpoch = time - start;
    boost::int64_t res = timeFromEpoch.total_microseconds();

    return static_cast<uint64_t>(res);
}

boost::posix_time::ptime intToTime(const uint64_t& number)
{
    boost::posix_time::ptime start(boost::gregorian::date(1970,1,1));
    boost::posix_time::ptime res = start + boost::posix_time::microseconds(static_cast<int64_t>(number));

    return res;
}

double price(const xbridge::TransactionDescrPtr ptr)
{
    if(ptr == nullptr) {
        return .0;
    }
    if(fabs(ptr->fromAmount)  < std::numeric_limits<double>::epsilon()) {
        return  .0;
    }
    return xBridgeValueFromAmount(ptr->toAmount) / xBridgeValueFromAmount(ptr->fromAmount);
}
double priceBid(const xbridge::TransactionDescrPtr ptr)
{
    if(ptr == nullptr) {
        return .0;
    }
    if(fabs(ptr->toAmount)  < std::numeric_limits<double>::epsilon()) {
        return  .0;
    }
    return xBridgeValueFromAmount(ptr->fromAmount) / xBridgeValueFromAmount(ptr->toAmount);
}

json_spirit::Object makeError(const xbridge::Error statusCode, const std::string &function, const std::string &message)
{
    Object error;
    error.emplace_back(Pair("error",xbridge::xbridgeErrorText(statusCode,message)));
    error.emplace_back(Pair("code", statusCode));
    error.emplace_back(Pair("name",function));
    return  error;
}

void LogOrderMsg(const std::string & orderId, const std::string & msg, const std::string & func) {
    UniValue o(UniValue::VOBJ);
    o.pushKV("orderid", orderId);
    o.pushKV("function", func);
    o.pushKV("msg", msg);
    LOG() << o.write();
}
void LogOrderMsg(UniValue o, const std::string & msg, const std::string & func) {
    o.pushKV("function", func);
    o.pushKV("msg", msg);
    LOG() << o.write();
}
void LogOrderMsg(xbridge::TransactionDescrPtr & ptr, const std::string & func) {
    LOG() << func << " " << ptr;
}
void LogOrderMsg(xbridge::TransactionPtr & ptr, const std::string & func) {
    LOG() << func << " " << ptr;
}

} // namespace xbridge