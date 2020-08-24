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
#include <seastar/core/pipe.hh>
#include "fsm.hh"

namespace raft {

// A single uniquely identified participant of the Raft group.
class server {
public:
    explicit server(server_id uuid, std::unique_ptr<rpc> rpc,
        std::unique_ptr<state_machine> state_machine,
        std::unique_ptr<storage> storage);

    server(server&&) = delete;

    enum class wait_type {
        committed,
        applied
    };

    // Adds command to replicated log
    // Returned future is resolved depending on wait_type parameter:
    //  'committed' - when the entry is committed
    //  'applied'   - when the entry is applied (happens after it is commited)
    // The function has to be called on a leader, throws not_a_leader exception otherwise.
    // May fail because of internal error or because leader changed and an entry was replaced
    // by another leader. In the later case dropped_entry exception will be returned.
    future<> add_entry(command command, wait_type type);

    // This function is called by append_entries RPC
    void append_entries(server_id from, append_request_recv append_request);

    // This function is called by append_entries_reply RPC
    void append_entries_reply(server_id from, append_reply reply);

    // This function is called to handle RequestVote RPC.
    void request_vote(server_id from, vote_request vote_request);
    // Handle response to RequestVote RPC
    void request_vote_reply(server_id from, vote_reply vote_reply);

    // Apply incomming snapshot, future resolves when application is complete
    future<> apply_snapshot(server_id from, install_snapshot snp);

    // Adds new server to a cluster. If a node is already a member
    // of the cluster does nothing Provided node_info is passed to
    // rpc::new_node() on each node in a cluster as it learns
    // about joining node. Connection info can be passed there.
    // Can be called on a leader only, otherwise throws not_a_leader.
    // Cannot be called until previous add/remove server completes
    // otherwise conf_change_in_progress exception is returned.
    future<> add_server(server_id id, bytes node_info, clock_type::duration timeout);

    // Removes a server from the cluster. If the server is not a member
    // of the cluster does nothing. Can be called on a leader only
    // otherwise throws not_a_leader.
    // Cannot be called until previous add/remove server completes
    // otherwise conf_change_in_progress exception is returned.
    future<> remove_server(server_id id, clock_type::duration timeout);

    // Load persisted state and start background work that needs
    // to run for this Raft server to function; The object cannot
    // be used until the returned future is resolved.
    future<> start();

    // Stop this Raft server, all submitted but not completed
    // operations will get an error and callers will not be able
    // to know if they succeeded or not. If this server was
    // a leader it will relinquish its leadership and cease
    // replication.
    future<> abort();

    // This function needs to be called before attempting read
    // from the local state machine. The read can proceed only if
    // the future returned by the function resolved successfully.
    // If called not on a leader it  not_a_leader error. After calling
    // this function the result of all completed
    // add_entries(wait_type::applied) can be observed by direct
    // access to the local state machine.
    future<> read_barrier();

    // Ad hoc functions for testing

    void make_me_leader();
protected:
    // Protocol deterministic finite-state machine
    std::unique_ptr<fsm> _fsm;
    // id of this server
    server_id _id;
private:
    std::unique_ptr<rpc> _rpc;
    std::unique_ptr<state_machine> _state_machine;
    std::unique_ptr<storage> _storage;
    seastar::timer<lowres_clock> _ticker;

    seastar::pipe<std::vector<log_entry_ptr>> _apply_entries = seastar::pipe<std::vector<log_entry_ptr>>(10);

    struct op_status {
        term_t term; // term the entry was added with
        promise<> done; // notify when done here
    };

    // Entries that have a waiter that needs to be notified when the
    // respective entry is known to be committed.
    std::map<index_t, op_status> _awaited_commits;

    // Entries that have a waiter that needs to be notified after
    // the respective entry is applied.
    std::map<index_t, op_status> _awaited_applies;

    // Contains active snapshot transfers, to be waited on exit.
    std::unordered_map<server_id, future<>> _snapshot_transfers;

    // The optional is engaged when incomming snapshot is received
    // And signalled when it is successfully applied or there was an error
    std::optional<promise<snapshot_reply>> _snapshot_application_done;

    // Called to commit entries (on a leader or otherwise).
    void notify_waiters(std::map<index_t, op_status>& waiters, const std::vector<log_entry_ptr>& entries);

    // Called when a node wins an election.
    future<> start_leadership();

    // Called when a node stops being a leader. The returned
    // future resolves when all the leader background work is
    // stopped.
    future<> stop_leadership();

    // This fiber process fsm output, by doing the following steps in that order:
    //  - persists current term and voter
    //  - persists unstable log entries on disk.
    //  - sends out messages
    future<> io_fiber(index_t stable_idx);

    // This fiber runs in the background and applies committed entries.
    future<> applier_fiber();

    template <typename T> future<> add_entry_internal(T command, wait_type type);
    template <typename Message> future<> send_message(server_id id, Message m);

    // Apply a dummy entry. Dummy entry is not propagated to the
    // state machine, but waiting for it to be "applied" ensures
    // all previous entries are applied as well.
    // Resolves when the entry is committed.
    // The function has to be called on the leader, throws otherwise
    // May fail because of an internal error or because the leader
    // has changed and the entry was replaced by another one,
    // submitted to the new leader.
    future<> apply_dummy_entry();

    // Send snapshot in the background and notify FSM about the result.
    void send_snapshot(server_id id, install_snapshot&& snp);

    future<> _applier_status = make_ready_future<>();
    future<> _io_status = make_ready_future<>();

    friend std::ostream& operator<<(std::ostream& os, const server& s);
};

} // namespace raft

