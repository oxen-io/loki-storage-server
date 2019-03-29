#include "swarm.h"
#include "http_connection.h"

#include "service_node.h"

#include <boost/log/trivial.hpp>
#include <stdlib.h>

namespace loki {

static bool swarm_exists(const all_swarms_t& all_swarms,
                         const swarm_id_t& swarm) {

    const auto it = std::find_if(
        all_swarms.begin(), all_swarms.end(),
        [&swarm](const SwarmInfo& si) { return si.swarm_id == swarm; });

    return it != all_swarms.end();
}

Swarm::~Swarm() = default;

SwarmEvents Swarm::update_swarms(const all_swarms_t& swarms) {

    /// TODO: need a fast check that the swarms are the same and exit early

    SwarmEvents events = {};

    swarm_id_t our_swarm_idx = UINT32_MAX;

    /// Find us:
    for (auto swarm_idx = 0u; swarm_idx < swarms.size(); ++swarm_idx) {

        const auto& snodes = swarms[swarm_idx].snodes;

        for (auto node_idx = 0u; node_idx < snodes.size(); ++node_idx) {

            if (our_address == snodes[node_idx]) {
                our_swarm_idx = swarm_idx;
            }
        }
    }

    const auto& our_swarm = swarms[our_swarm_idx].snodes;

    if (our_swarm_idx == UINT32_MAX) {
        BOOST_LOG_TRIVIAL(error) << "ERROR: WE ARE NOT IN ANY SWARM";
        return events;
    }

    if (swarm_peers_.empty()) {

        assert(cur_swarm_id_ == UINT64_MAX);

        BOOST_LOG_TRIVIAL(info)
            << "EVENT: started SN in swarm: " << our_swarm_idx;

    } else {

        /// Are we in a new swarm?
        if (cur_swarm_id_ != swarms[our_swarm_idx].swarm_id) {

            BOOST_LOG_TRIVIAL(info) << "EVENT: got moved into a new swarm: "
                                    << swarms[our_swarm_idx].swarm_id;

            /// Check that our old swarm still exists
            if (!swarm_exists(swarms, cur_swarm_id_)) {

                BOOST_LOG_TRIVIAL(info)
                    << "EVENT: our old swarm got DISSOLVED!";
                events.decommissioned = true;
            }
        }

        /// don't bother checking the rest
        if (!events.decommissioned) {

            /// See if anyone joined our swarm
            for (auto& sn : our_swarm) {

                auto it =
                    std::find(swarm_peers_.begin(), swarm_peers_.end(), sn);

                if (it == swarm_peers_.end()) {
                    BOOST_LOG_TRIVIAL(info)
                        << "EVENT: detected new SN: " << to_string(sn);
                    events.new_snodes.push_back(sn);
                }
            }

            /// See if there are any new swarms

            for (const auto& swarm_info : swarms) {

                bool found = false;

                for (const auto& prev_si : all_cur_swarms_) {

                    if (prev_si.swarm_id == swarm_info.swarm_id) {
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    BOOST_LOG_TRIVIAL(info) << "EVENT: detected a new swarm: "
                                            << swarm_info.swarm_id;
                    events.new_swarms.push_back(swarm_info.swarm_id);
                }
            }
        }
    }

    /// NOTE: need to be careful and make sure we don't miss any
    /// swarm update (e.g. if we don't update frequently enough)

    cur_swarm_id_ = swarms[our_swarm_idx].swarm_id;
    all_cur_swarms_ = swarms;
    swarm_peers_ = our_swarm;

    return events;
}

static uint64_t hex_to_u64(const std::string& pk) {

    if (pk.size() != 66) {
        throw std::invalid_argument("invalid pub key size");
    }

    /// Create a buffer for 16 characters null terminated
    char buf[17] = {};

    /// Note: pk is expected to contain two leading characters
    /// (05 for the messenger) that do not participate in mapping

    /// Note: if conversion is not possible, we will still
    /// get a value in res (possibly 0 or UINT64_MAX), which
    /// we are not handling at the moment
    uint64_t res = 0;
    for (auto it = pk.begin() + 2; it < pk.end(); it += 16) {
        memcpy(buf, &(*it), 16);
        res ^= strtoull(buf, nullptr, 16);
    }

    return res;
}

swarm_id_t get_swarm_by_pk(const std::vector<SwarmInfo>& all_swarms,
                           const std::string& pk) {

    const uint64_t res = hex_to_u64(pk);

    /// We reserve UINT64_MAX as a sentinel swarm id for unassigned snodes
    constexpr swarm_id_t MAX_ID = std::numeric_limits<uint64_t>::max() - 1;
    constexpr swarm_id_t SENTINEL_ID = std::numeric_limits<uint64_t>::max();

    swarm_id_t cur_best = SENTINEL_ID;
    uint64_t cur_min = SENTINEL_ID;

    /// We don't require that all_swarms is sorted, so we find
    /// the smallest/largest elements in the same loop
    swarm_id_t leftmost_id = SENTINEL_ID;
    swarm_id_t rightmost_id = 0;

    for (const auto& si : all_swarms) {

        uint64_t dist =
            (si.swarm_id > res) ? (si.swarm_id - res) : (res - si.swarm_id);
        if (dist < cur_min) {
            cur_best = si.swarm_id;
            cur_min = dist;
        }

        /// Find the letfmost
        if (si.swarm_id < leftmost_id) {
            leftmost_id = si.swarm_id;
        }

        if (si.swarm_id > rightmost_id) {
            rightmost_id = si.swarm_id;
        }
    }

    // handle special case
    if (res > rightmost_id) {
        // since rightmost is at least as large as leftmost,
        // res >= leftmost_id in this branch, so the value will
        // not overflow; the same logic applies to the else branch
        const uint64_t dist = (MAX_ID - res) + leftmost_id;
        if (dist < cur_min) {
            cur_best = leftmost_id;
        }
    } else if (res < leftmost_id) {
        const uint64_t dist = res + (MAX_ID - rightmost_id);
        if (dist < cur_min) {
            cur_best = rightmost_id;
        }
    }

    return cur_best;
}

std::vector<sn_record_t> Swarm::other_nodes() const {

    std::vector<sn_record_t> result;

    for (auto& swarm : swarm_peers_) {
        if (swarm != our_address) {
            result.push_back(swarm);
        }
    }

    return result;
}

} // namespace loki