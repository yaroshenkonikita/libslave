#pragma once

#include <vector>
#include <string>
#include <map>
#include <ostream>
#include <utility>

namespace slave
{

// transaction interval with numbers from "first" to "second", inclusively
using gtid_interval_t = std::pair<int64_t, int64_t>;

// any interval having "second" < "first" is considered empty
inline bool isIntervalEmpty(const gtid_interval_t& x) { return x.second < x.first; }

// any interval having "second" < "first" is considered empty
inline gtid_interval_t makeEmptyInterval() { return gtid_interval_t(1, 0); }

// find the intersection of two transaction intervals
gtid_interval_t intersectIntervals(const gtid_interval_t& x, const gtid_interval_t& y);


// list of transaction intervals, where the intervals are stored
// in ascending order by initial transaction numbers
using gtid_interval_list_t = std::vector<gtid_interval_t>;

// find the intersection of two lists of transaction intervals
gtid_interval_list_t intersectIntervalsLists(const gtid_interval_list_t& x,
                                             const gtid_interval_list_t& y);


// GTID set - represents a complete position in a binlog, actually a map:
// key - source server uuid, value - list of its transaction intervals
using gtid_set_t = std::map<std::string, gtid_interval_list_t>;

// find the intersection of two GTID sets,
// not very useful per se, see Position::shiftToThePast() instead
gtid_set_t intersectGtidSets(const gtid_set_t& x, const gtid_set_t& y);


// single transaction: first - source server uuid, second - transaction number
using gtid_t = std::pair<std::string, int64_t>;


struct Position
{
    std::string   log_name;
    unsigned long log_pos = 0;
    gtid_set_t    gtid_executed;

    Position() {}

    Position(std::string _log_name, unsigned long _log_pos)
    :   log_name(std::move(_log_name))
    ,   log_pos(_log_pos)
    {}

    bool empty() const { return (log_name.empty() || log_pos == 0) && gtid_executed.empty(); }
    void clear() { log_name.clear(); log_pos = 0; gtid_executed.clear(); }

    void parseGtid(const std::string& input);
    void addGtid(const gtid_t& gtid);
    size_t encodedGtidSize() const;
    void encodeGtid(unsigned char* buf);

    // This method helps to rewind the current position of the client
    // back to as early in time as specified by another position,
    // which is typically received from a client reading some other replica.
    // In this situation we have to avoid adding any new sources,
    // that our client's replica isn't aware of, or binlog request will fail.
    void shiftToThePast(const Position &other);

    bool reachedOtherPos(const Position& other) const;

    std::string str() const;
    std::string formatGtid() const;
};

inline std::ostream& operator<<(std::ostream& os, const Position& pos)
{
    os << pos.str();
    return os;
}

} // namespace slave
