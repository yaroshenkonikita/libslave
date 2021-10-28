#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iterator>
#include <memory>

#include <alloca.h>
#include <mysql/my_global.h>
#undef min
#undef max
#undef test

#include "binlog_pos.h"
#include "slave_log_event.h"

namespace
{
void hex2bin(uint8_t* dst, const char* src, size_t sz_src)
{
    if (!dst || !src) return;

    uint8_t cur = 0;
    for (size_t i = 0; i < sz_src; ++i)
    {
        const char c = src[i];

             if ('0' <= c && c <= '9') cur |= c - '0';
        else if ('A' <= c && c <= 'F') cur |= c - 'A' + 10;
        else if ('a' <= c && c <= 'f') cur |= c - 'a' + 10;
        else throw std::runtime_error("hex2bin failed: bad symbol in hex data");

        if (0 == i % 2)
        {
            cur <<= 4;
        }
        else
        {
            *dst++ = cur;
            cur = 0;
        }
    }
}

template <class F>
void backup_invoke_restore(F& f, char* begin, char* end)
{
    if (begin == end)
        return;
    const char backup = *end;
    *end = '\0';
    f(const_cast<const char*>(begin));
    *end = backup;
}

// Parse list on const char* using delimiter and call function with each element
template <class F, class C>
void parse_list_f_custom(const std::string& aList, F f, C c, const char* delim = ",")
{
    std::unique_ptr<char[]> sHeap;
    char* s = nullptr;
    const size_t sRequiredSize = aList.size() + 1;
    if (sRequiredSize <= 65536)
    {
        s = (char*)::alloca(sRequiredSize);
    }
    else
    {
        sHeap.reset(new char[sRequiredSize]);
        s = sHeap.get();
    }

    memcpy(s, aList.data(), aList.size());
    s[aList.size()] = '\0';

    for (char* p = s; '\0' != *p;)
    {
        p += strspn(p, delim);
        if (!c(aList, f, p))
        {
            auto q = p + strcspn(p, delim);
            backup_invoke_restore(f, p, q);
            p = q;
        }
    }
}

template <typename F>
void parse_list_f(const std::string& aList, F f, const char* delim = ",")
{
    parse_list_f_custom(aList, f, [] (const std::string&, F&, char*) { return false; }, delim);
}

// Parse list on long long using delimiter and call function with each element
template <typename F>
void parse_list_ll_f(const std::string& aList, F f, const char* delim = ",")
{
    parse_list_f(aList, [&f] (const char* s) { f(atoll(s)); }, delim);
}

// Parse list using delimiter and put strings into container
template <class Cont>
void parse_list_cont(const std::string& aList, Cont& aCont, const char* delim = ",")
{
    parse_list_f(aList,
            [&aCont] (const char* s)
            {
                aCont.insert(aCont.end(), s);
            }
        , delim);
}
} // namespace anonymous

namespace slave
{

// find the intersection of two transaction intervals
gtid_interval_t intersectIntervals(const gtid_interval_t& x, const gtid_interval_t& y)
{
    // if any given interval is empty - the result is empty too
    if (isIntervalEmpty(x) || isIntervalEmpty(y))
        return makeEmptyInterval();
    // check if there's no intersection
    if (x.second < y.first || y.second < x.first)
        return makeEmptyInterval();
    return gtid_interval_t(std::max(x.first, y.first), std::min(x.second, y.second));
}

// find the intersection of two lists of transaction intervals
gtid_interval_list_t intersectIntervalsLists(const gtid_interval_list_t& x, const gtid_interval_list_t& y)
{
    gtid_interval_list_t result;
    // straightforward implementation here, time complexity is O(N*M)
    // should be OK, since N and M aren't expected to differ greatly from 1
    for (const auto& x_interval : x)
    {
        for (const auto& y_interval : y)
        {
            auto z_interval = intersectIntervals(x_interval, y_interval);
            if (!isIntervalEmpty(z_interval))
                result.push_back(z_interval);
        }
    }
    return result;
}

// find the intersection of two GTID sets,
// not very useful per se, see Position::shiftToThePast() instead
gtid_set_t intersectGtidSets(const gtid_set_t& x, const gtid_set_t& y)
{
    gtid_set_t result;
    for (const auto& [x_source, x_transactions] : x)
    {
        auto y_it = y.find(x_source);
        if (y.end() != y_it)
        {
            auto z_transactions = intersectIntervalsLists(x_transactions, y_it->second);
            if (!z_transactions.empty())
                result.emplace(x_source, z_transactions);
        }
    }
    return result;
}

// parseGtid parse string with gtid
// example:  ae00751a-cb5f-11e6-9d92-e03f490fd3db:1-12:15-17
// gtid_set: uuid_set [, uuid_set] ... | ''
// uuid_set: uuid:interval[:interval]...
// uuid:     hhhhhhhh-hhhh-hhhh-hhhh-hhhhhhhhhhhh
// h:        [0-9|A-F]
// interval: n[-n] (n >= 1)
void Position::parseGtid(const std::string& input)
{
    if (input.empty())
        return;
    gtid_executed.clear();
    std::string s;
    std::remove_copy_if(input.begin(), input.end(), std::back_inserter(s), [](char c){ return c == ' ' || c == '\n'; });

    std::deque<std::string> cont;
    parse_list_f(s, [this, &cont](const std::string& token)
    {
        cont.clear();
        parse_list_cont(token, cont, ":");
        bool uuid_parsed = false;
        std::string sid;
        for (const auto& x : cont)
        {
            if (!uuid_parsed)
            {
                std::remove_copy(x.begin(), x.end(), std::back_inserter(sid), '-');
                uuid_parsed = true;
            }
            else
            {
                bool first = true;
                gtid_interval_t interval;
                parse_list_ll_f(x, [&first, &interval](int64_t y)
                {
                    if (first)
                    {
                        interval.first = interval.second = y;
                        first = false;
                    }
                    else
                    {
                        interval.second = y;
                    }
                }, "-");
                gtid_executed[sid].push_back(interval);
            }
        }
    });
}

void Position::addGtid(const gtid_t& gtid)
{
    const auto& server_uuid = gtid.first;
    auto trans_no = gtid.second;

    auto& trans_intervals = gtid_executed[server_uuid];
    bool add_new_interval = true;

    for (auto it = trans_intervals.begin(); it != trans_intervals.end(); ++it)
    {
        auto& interval = *it;
        if (interval.second + 1 == trans_no)  // optimize for most frequent case
        {
            ++interval.second;
            add_new_interval = false;
            break;
        }
        if (trans_no >= interval.first && trans_no <= interval.second)
        {
            return;
        }
        if (trans_no + 1 == interval.first)
        {
            --interval.first;
            add_new_interval = false;
            break;
        }
        if (trans_no < interval.first)
        {
            trans_intervals.emplace(it, trans_no, trans_no);
            return;
        }
    }

    if (add_new_interval)
    {
        trans_intervals.emplace_back(trans_no, trans_no);
        return;
    }

    for (auto it = trans_intervals.begin(); it != trans_intervals.end(); ++it)
    {
        auto next_it = std::next(it);
        if (next_it != trans_intervals.end() && it->second + 1 == next_it->first)
        {
            it->second = next_it->second;
            trans_intervals.erase(next_it);
            break;
        }
    }
}

size_t Position::encodedGtidSize() const
{
    if (gtid_executed.empty())
        return 0;
    size_t result = 8;
    for (const auto& x : gtid_executed)
        result += x.second.size() * 16 + 8 + ENCODED_SID_LENGTH;

    return result;
}

void Position::encodeGtid(unsigned char* buf)
{
    if (gtid_executed.empty())
        return;
    int8store(buf, gtid_executed.size());
    size_t offset = 8;
    for (const auto& x : gtid_executed)
    {
        hex2bin(buf + offset, x.first.c_str(), ENCODED_SID_LENGTH * 2);
        offset += ENCODED_SID_LENGTH;
        int8store(buf + offset, x.second.size());
        offset += 8;
        for (const auto& interval : x.second)
        {
            int8store(buf + offset, interval.first);
            offset += 8;
            int8store(buf + offset, interval.second + 1);
            offset += 8;
        }
    }
}

// This method helps to rewind the current position of the client
// back to as early in time as specified by another position,
// which is typically received from a client reading some other replica.
// In this situation we have to avoid adding any new sources,
// that our client's replica isn't aware of, or binlog request will fail.
void Position::shiftToThePast(const Position &other)
{
    if (gtid_executed.empty() || other.gtid_executed.empty())
        throw std::runtime_error("Both positions should contain GTIDs");
    auto result = intersectGtidSets(gtid_executed, other.gtid_executed);
    for (const auto& [x_source, x_transactions] : gtid_executed)
    {
        if (result.end() == result.find(x_source))
            result[x_source] = x_transactions;
    }
    std::swap(result, gtid_executed);
}

bool Position::reachedOtherPos(const Position& other) const
{
    if (gtid_executed.empty() && other.gtid_executed.empty())
        return log_name > other.log_name ||
               (log_name == other.log_name && log_pos >= other.log_pos);

    // if one of the positions does not make use of GTID
    // let's treat as one in the past
    if (gtid_executed.empty())
        return false;
    if (other.gtid_executed.empty())
        return true;

    // now we are to compare `gtid_executed` lists:
    // look through all of the other sources
    // and check if we have all the transactions required
    for (const auto& [sOtherSource, sOtherTransactions]: other.gtid_executed)
    {
        // if no transactions for the source - treat as an error
        // https://dev.mysql.com/doc/refman/5.6/en/replication-gtids-concepts.html
        if (sOtherTransactions.empty())
        {
            throw std::runtime_error(
                "Invalid 'other' GTID: empty interval for UUID " + sOtherSource);
        }

        // since `gtid_executed` keeps track of all the sources, never forgetting them,
        // this means if we see no such `other` source we should read on to see it appear
        auto sCurIt = gtid_executed.find(sOtherSource);
        if (gtid_executed.end() == sCurIt)
            return false;
        const auto& sThisTransactions = sCurIt->second;

        // if no transactions for the source - treat as an error
        // https://dev.mysql.com/doc/refman/5.6/en/replication-gtids-concepts.html
        if (sThisTransactions.empty())
        {
            throw std::runtime_error(
                "Invalid GTID: empty interval for UUID " + sOtherSource);  // the source was found
        }

        // if both sources found, then compare the last position of the transaction intervals
        const auto& sThisLastInterval = sThisTransactions.back();
        auto sThisLastTransaction = sThisLastInterval.second;
        const auto& sOtherLastInterval = sOtherTransactions.back();
        auto sOtherLastTransaction = sOtherLastInterval.second;
        if (sThisLastTransaction < sOtherLastTransaction)
            return false;
    }

    // we've checked all the sources, for each one:
    // current transaction position >= position given,
    // this means we are guaranteed to reach position given
    return true;
}

std::string Position::str() const
{
    std::string result = "'";
    if (!log_name.empty() && log_pos)
        result += log_name + ":" + std::to_string(log_pos) + ", ";

    result += "GTIDs=" + formatGtid() + "'";
    return result;
}

std::string Position::formatGtid() const
{
    if (gtid_executed.empty())
        return "-";

    std::string result;
    bool first_a = true;
    for (const auto& gtid : gtid_executed)
    {
        if (first_a)
            first_a = false;
        else
            result += ",";

        result += gtid.first + ":";
        bool first_b = true;
        for (const auto& interv : gtid.second)
        {
            if (first_b)
                first_b = false;
            else
                result += ":";

            result += std::to_string(interv.first);
            if (interv.first != interv.second)
                result += "-" + std::to_string(interv.second);
        }
    }
    return result;
}

} // namespace slave
