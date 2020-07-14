/*
 * Copyright (C) 2020 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */
#pragma once

#include <map>
#include <vector>
#include <unordered_map>
#include <functional>
#include <boost/container/deque.hpp>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/future.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/condition-variable.hh>
#include "bytes_ostream.hh"
#include "utils/UUID.hh"

namespace raft {
// Keeps user defined command. A user is responsible to serialize a state machine operation
// into it before passing to raft and deserialize in apply() before applying
using command = bytes_ostream;
using command_cref = std::reference_wrapper<const command>;

namespace internal {

template<typename Tag>
class typed_uint64 {
    uint64_t _val;
public:
    typed_uint64() : _val(0) {}
    explicit typed_uint64(uint64_t v) : _val(v) {}
    typed_uint64(const typed_uint64&) = default;
    typed_uint64(typed_uint64&&) = default;
    typed_uint64& operator=(const typed_uint64&) = default;
    auto operator<=>(const typed_uint64&) const = default;

    uint64_t operator()() const {
        return _val;
    };
    operator uint64_t() const {
        return _val;
    }
    typed_uint64& operator++() { // pre increment
        ++_val;
        return *this;
    }
    typed_uint64 operator++(int) { // post increment
        uint64_t v = _val++;
        return typed_uint64(v);
    }
    typed_uint64& operator--() { // pre decrement
        --_val;
        return *this;
    }
    typed_uint64 operator--(int) { // post decrement
        uint64_t v = _val--;
        return typed_uint64(v);
    }
    typed_uint64 operator+(const typed_uint64& o) const {
        return typed_uint64(_val + o._val);
    }
    typed_uint64 operator-(const typed_uint64& o) const {
        return typed_uint64(_val - o._val);
    }
};

template<typename Tag>
struct generic_id {
    utils::UUID id;
    bool operator==(const generic_id& o) const {
        return id == o.id;
    }
};
template<typename Tag>
std::ostream& operator<<(std::ostream& os, const generic_id<Tag>& id) {
    os << id.id;
    return os;
}

} // end of namespace internal
} // end of namespace raft

namespace std {

template<typename Tag>
struct hash<raft::internal::generic_id<Tag>> {
    size_t operator()(const raft::internal::generic_id<Tag>& id) const {
        return hash<utils::UUID>()(id.id);
    }
};
} // end of namespace std

namespace raft {

// This is user provided id for a snapshot
using snapshot_id = internal::generic_id<struct shapshot_id_tag>;
// Unique identifier of a node in a raft group
using node_id = internal::generic_id<struct node_id_tag>;

using term_t = internal::typed_uint64<struct term_tag>;
using index_t = internal::typed_uint64<struct index_tag>;

using clock_type = lowres_clock;

struct node {
    node_id id;
    // Opaque connection properties
    bytes info;
};

struct configuration {
    std::vector<node> nodes;
};

struct log_entry {
    term_t term;
    index_t index;
    std::variant<command, configuration> data;
};

// this class represents raft log in memory
// note that first value index is 1
// new entries are added at the back
// entries may be dropped form the beginning by snapshotting
// and from the end by new leader replacing stale entries
class log {
    // we need something that can be truncated form both sides.
    // std::deque move constructor is not nothrow hence cannot be used
    boost::container::deque<log_entry> _log;
    // prevents concurrent write to the log
    // it is unique_ptr to make log class movable (smeaophore is not)
    std::unique_ptr<seastar::semaphore> _log_lock = std::make_unique<seastar::semaphore>(1);
    // the index of the first entry in the log (index starts from 1)
    // will be increased by log gc
    index_t _log_starting_index = index_t(1);
public:
    future<seastar::semaphore_units<>> lock();
    log_entry& operator[](size_t i);
    // reserve n additional entries
    void ensure_capacity(size_t n);
    void emplace_back(log_entry&& e);
    // return true if in memory log is empty
    bool empty() const;
    index_t next_idx() const;
    index_t last_idx() const;
    void truncate_head(size_t i);
    index_t start_index() const;
};

struct error : public std::runtime_error {
    error(std::string error) : std::runtime_error(error) {}
};

struct not_leader : public error {
    node_id leader;
    not_leader(node_id l) : error("Not a leader"), leader(l) {}
};

struct dropped_entry : public error {
    dropped_entry() : error("Entry was dropped because of a leader change") {}
};

struct stopped_error : public error {
    stopped_error() : error("Raft instance is stooped") {}
};

struct snapshot {
    // Index and term of last entry in the snapshot
    index_t index;
    term_t term;
    // The committed configuration in the snapshot
    configuration config;
    // Id of the snapshot.
    snapshot_id id;
};

using log_entry_cref = std::reference_wrapper<const log_entry>;

struct append_request_base {
    // leader's term
    term_t current_term;
    // so follower can redirect clients
    // In practice we do not need it since we should know sender's id anyway
    node_id leader_id;
    // index of log entry immediately preceding new ones
    index_t prev_log_index;
    // term of prev_log_index entry
    term_t prev_log_term;
    // leader's commit_index
    index_t leader_commit;
};
struct append_request_send : public append_request_base {
    // log entries to store (empty for heartbeat; may send more than one for efficiency)
    std::vector<log_entry_cref> entries;
};
struct append_request_recv : public append_request_base {
    // same as for append_request_send but unlike it here the message owns the entries
    std::vector<log_entry> entries;
};
struct append_reply {
    // current term, for leader to update itself
    term_t current_term;
    // 'true' if all entries were successfully appended
    // 'false' if a receivers term is larger or there was a mismatch in index/term
    bool appended;
    // the following is valid only if appended == false
    // term of the conflicting entry
    term_t non_matching_term;
    // first index for the conflicting term
    index_t first_idx_for_non_matching_term;
};

// this is an extension of Raft used for keepalive aggregation between multiple groups
struct keep_alive {
    // leader's term
    term_t current_term;
    // so follower can redirect clients
    // here it has to be included since this will be sent not
    // as point to point message but as part of an aggregated one.
    node_id leader_id;
    // leader's commit_index
    index_t leader_commit;
};

struct vote_request {
    // candidate’s term
    term_t term;
    // candidate requesting vote
    node_id candidate_id;
    // index of candidate's last log entry
    index_t last_log_idx;
    // term of candidate's last log entry
    term_t last_log_term;
    // true if pre-vote
    // bool pre_vote
};

struct vote_reply {
    // current term, for candidate to update itself
    term_t term;
    // true means candidate received vote
    bool vote_granted;
};

class rpc;
class storage;

class state_machine {
public:
    virtual ~state_machine() {}
    // This is called after entries are committed (replicated to at least quorum of nodes).
    // Multiple entries can be committed simultaneously.
    // Will be eventually called on all replicas.
    // Raft owns the data since it may be still replicating.
    // Raft will not call another apply until the retuned future will not become ready.
    virtual future<> apply(const std::vector<command_cref> command) = 0;

    // The function suppose to take a snapshot of a state machine
    // To be called during log compaction or when a leader brings
    // a lagging follower up-to-date
    virtual future<snapshot_id> take_snaphot() = 0;

    // The function drops a snapshot with a provided id
    virtual void drop_snapshot(snapshot_id id) = 0;

    // reload state machine from a snapshot id
    // To be used by a restarting node or by a follower that
    // catches up to a leader
    virtual future<> load_snapshot(snapshot_id id) = 0;

    // stops the state machine instance by aborting the work
    // that can be aborted and waiting for all the rest to complete
    // any unfinished apply/snapshot operation may return an error after
    // this function is called
    virtual future<> stop() = 0;
};

class instance;

class rpc {
protected:
    // Pointer to the instance. Needed for passing rpc messages.
    instance* _instance = nullptr;
public:
    virtual ~rpc() {}

    // Send a snapshot snap to a node node_id.
    // A returned future is resolved when snapshot is sent and successfully applied
    // by a receiver
    virtual future<> send_snapshot(node_id node_id, snapshot snap) = 0;

    // Sends provided append_request to supplied node and waits for a reply
    virtual future<append_reply> send_append_entries(node_id id, const append_request_send& append_request) = 0;

    // Sends vote requests and returns vote reply
    virtual future<vote_reply> send_request_vote(node_id id, const vote_request& vote_request) = 0;

    // This is an extension of Raft used for keepalive aggregation between multiple groups
    // This RPC does not return anything since it will be aggregated for many groups
    // but this means that it cannot reply with larger term and convert a leader that sends it
    // to a follower. A new leader that detects stale leader by processing this message needs to
    // contact it explicitly by issuing empty send_append_entries call.
    virtual void send_keepalive(node_id id, const keep_alive& keep_alive) = 0;

    // When new node is learn this function is called with the info about the node
    virtual void add_node(node_id id, bytes node_info) = 0;

    // When a node is removed from local config this call is executed
    virtual void remove_node(node_id id) = 0;

    // stops the rpc instance by aborting the work
    // that can be aborted and waiting for all the rest to complete
    // any unfinished send operation may return an error after this
    // function is called
    virtual future<> stop() = 0;
private:
    void set_instance(raft::instance& instance) { _instance = &instance; }
    friend instance;
};

// This class represents persistent storage state.
class storage {
public:
    virtual ~storage() {}
    // Persist given term and resets vote atomically
    // Can be called concurrently with other and with itself
    // but an implementation has to make sure that result is leniarizable
    // vs itself and store_vote() function (since both modify the vote)
    virtual future<> store_term(term_t term) = 0;

    // Load persisted term
    // Called during raft instance initialization only, should not run in parallel with store
    virtual future<term_t> load_term() = 0;

    // Persist given vote
    // Can be called concurrently with other and with itself
    // but an implementation has to make sure that result is leniarizable
    // vs itself and store_term() function (since both modify the vote)
    virtual future<> store_vote(node_id vote) = 0;

    // Load persisted vote
    // Called during raft instance initialization only, should not run in parallel with store
    virtual future<std::optional<node_id>> load_vote() = 0;

    // Persist given snapshot and drops all but 'preserve_log_entries'
    // entries from the raft log starting from the beginning
    // This will rewrite previously persisted snapshot
    // Should be called only after previous invocation completes
    // IOW a caller should serialize. Can be called in parallel with
    // store_log_entries() but snap.index should belong to already persisted entry
    virtual future<> store_snapshot(snapshot snap, size_t preserve_log_entries) = 0;

    // Load a saved snapshot
    // This only loads it into memory, but does not apply yet
    // To apply call 'state_machine::load_snapshot(snapshot::id)'
    // Called during raft instance initialization only, should not run in parallel with store
    virtual future<snapshot> load_snapshot() = 0;

    // Persist given log entries
    // can be called without waiting for previous call to resolve, but internally
    // all writes should be serialized info forming one contigious log that holds
    // entris in order of the function invocation.
    virtual future<> store_log_entries(const std::vector<log_entry>& entries) = 0;

    // Persist given log entry
    virtual future<> store_log_entry(const log_entry& entry) = 0;

    // Load saved raft log
    // Called during raft instance initialization only, should not run in parallel with store
    virtual future<log> load_log() = 0;

    // Truncate all entries with index greater that idx in the log
    // and persist the truncation. Can be called in parallel with store_log_entries()
    // but internally should be linearized vs store_log_entries(): store_log_entries()
    // called after truncate_log() should wait for truncation to complete internally before
    // persisting its entries.
    virtual future<> truncate_log(index_t idx) = 0;

    // stops the a storage instance by aborting the work
    // that can be aborted and waiting for all the rest to complete
    // any unfinished store/load operation may return an error after this
    // function is called
    virtual future<> stop() = 0;
};

} // namespace raft

