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
#include <ranges>
#include <seastar/core/sleep.hh>
#include <seastar/core/future-util.hh>
#include <seastar/core/coroutine.hh>
#include "server.hh"
#include "log.hh"

using namespace std::chrono_literals;

namespace raft {

server::server(server_id uuid, std::unique_ptr<rpc> rpc, std::unique_ptr<state_machine> state_machine,
        std::unique_ptr<storage> storage) :
                    _rpc(std::move(rpc)), _state_machine(std::move(state_machine)), _storage(std::move(storage)),
                    _id(uuid) {
    _rpc->set_server(*this);
}

future<> server::start() {
    auto [term, vote] = co_await _storage->load_term_and_vote();
    auto snapshot  = co_await _storage->load_snapshot();
    auto snp_id = snapshot.id;
    auto log_entries = co_await _storage->load_log();
    auto log = raft::log(std::move(snapshot), std::move(log_entries));
    index_t stable_idx = log.stable_idx();
    _fsm = std::make_unique<fsm>(_id, term, vote, std::move(log));
    assert(_fsm->get_current_term() != term_t(0));

    if (snp_id) {
        co_await _state_machine->load_snapshot(snp_id);
    }

    // start fiber to persist entries added to in-memory log
    _io_status = io_fiber(stable_idx);
    // start fiber to apply committed entries
    _applier_status = applier_fiber();

    _ticker.arm_periodic(100ms);
    _ticker.set_callback([this] {
        _fsm->tick();
    });

    co_return;
}

template <typename T>
future<> server::add_entry_internal(T command, wait_type type) {
    logger.trace("An entry is submitted on a leader");

    // lock access to the raft log while it is been updated

    // @todo: ensure the reference to the entry is stable between
    // yields, before removing _log_lock.
    const log_entry& e = _fsm->add_entry(std::move(command));

    auto& container = type == wait_type::committed ? _awaited_commits : _awaited_applies;

    // This will track the commit/apply status of the entry
    auto [it, inserted] = container.emplace(e.idx, op_status{e.term, promise<>()});
    assert(inserted);
    return it->second.done.get_future();
}

future<> server::add_entry(command command, wait_type type) {
    return add_entry_internal(std::move(command), type);
}

future<> server::apply_dummy_entry() {
    return add_entry_internal(log_entry::dummy(), wait_type::applied);
}
void server::append_entries(server_id from, append_request_recv append_request) {
    _fsm->step(from, std::move(append_request));
}

void server::append_entries_reply(server_id from, append_reply reply) {
    _fsm->step(from, std::move(reply));
}

void server::request_vote(server_id from, vote_request vote_request) {
    _fsm->step(from, std::move(vote_request));
}

void server::request_vote_reply(server_id from, vote_reply vote_reply) {
    _fsm->step(from, std::move(vote_reply));
}

void server::notify_waiters(std::map<index_t, op_status>& waiters, const std::vector<log_entry_ptr>& entries) {
    index_t commit_idx = entries.back()->idx;
    index_t first_idx = entries.front()->idx;

    while (waiters.size() != 0) {
        auto it = waiters.begin();
        if (it->first > commit_idx) {
            break;
        }
        auto [entry_idx, status] = std::move(*it);

        // if there is a waiter entry with an index smaller than first entry
        // it means that notification is out of order which is prohinited
        assert(entry_idx >= first_idx);

        waiters.erase(it);
        if (status.term == entries[entry_idx - first_idx]->term) {
            status.done.set_value();
        } else {
            // term does not match which means that between the entry was submitted
            // and committed there was a leadership change and the entry was replaced.
            status.done.set_exception(dropped_entry());
        }
    }
}

template <typename Message>
future<> server::send_message(server_id id, Message m) {
    return std::visit([this, id] (auto&& m) {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, append_reply>) {
              return _rpc->send_append_entries_reply(id, m);
          } else if constexpr (std::is_same_v<T, keep_alive>) {
              _rpc->send_keepalive(id, m);
              return make_ready_future<>();
          } else if constexpr (std::is_same_v<T, append_request_send>) {
              return _rpc->send_append_entries(id, m);
          } else if constexpr (std::is_same_v<T, vote_request>) {
              return _rpc->send_vote_request(id, m);
          } else if constexpr (std::is_same_v<T, vote_reply>) {
              return _rpc->send_vote_reply(id, m);
          } else if constexpr (std::is_same_v<T, install_snapshot>) {
              // Send in the background.
              send_snapshot(id, std::move(m));
              return make_ready_future<>();
          } else if constexpr (std::is_same_v<T, snapshot_reply>) {
              assert(_snapshot_application_done);
              // send reply to install_snapshot here
              _snapshot_application_done->set_value(std::move(m));
              _snapshot_application_done = std::nullopt;
              return make_ready_future<>();
          }

          logger.error("log fiber {} tried to send unknown message type", _id);
          ::abort();
          return make_ready_future<>();
    }, std::move(m));
}

future<> server::io_fiber(index_t last_stable) {
    logger.trace("[{}] io_fiber start", _id);
    try {
        while (true) {
            auto batch = co_await _fsm->poll_output();

            if (batch.term != term_t{}) {
                // Current term and vote are always persisted
                // together. A vote may change independently of
                // term, but it's safe to update both in this
                // case.
                co_await _storage->store_term_and_vote(batch.term, batch.vote);
            }

            if (batch.snp) {
                logger.trace("[{}] io_fiber process snapshot {}", _id, batch.snp->id);
                // Persist the snapshot
                co_await _storage->store_snapshot(*batch.snp, 0);
                // Apply it to the state machine
                co_await _state_machine->load_snapshot(batch.snp->id);
            }

            if (batch.log_entries.size()) {
                auto& entries = batch.log_entries;

                if (last_stable > entries[0]->idx) {
                    co_await _storage->truncate_log(entries[0]->idx);
                }

                // Combine saving and truncating into one call?
                // will require storage to keep track of last idx
                co_await _storage->store_log_entries(entries);

                last_stable = (*entries.crbegin())->idx;
            }

            if (batch.messages.size()) {
                // after entries are persisted we can send messages
                co_await seastar::parallel_for_each(std::move(batch.messages), [this] (std::pair<server_id, rpc_message>& message) {
                    return send_message(message.first, std::move(message.second));
                });
            }

            // process committed entries
            if (batch.committed.size()) {
                notify_waiters(_awaited_commits, batch.committed);
                co_await _apply_entries.writer.write(std::move(batch.committed));
            }
        }
    } catch (seastar::broken_condition_variable&) {
        // log fiber is stopped explicitly.
    } catch (...) {
        logger.error("[{}] io fiber stopped because of the error: {}", _id, std::current_exception());
    }
    co_return;
}

void server::send_snapshot(server_id dst, install_snapshot&& snp) {
    future<> f = _rpc->send_snapshot(dst, std::move(snp)).then_wrapped([this, dst] (future<> f) {
        _snapshot_transfers.erase(dst);
        if (f.failed()) {
            logger.error("[{}] Transferring snapshot to {} failed with: {}", _id, dst, f.get_exception());
            _fsm->snapshot_status(dst, false);
        } else {
            logger.trace("[{}] Transferred snapshot to {}", _id, dst);
            _fsm->snapshot_status(dst, true);
        }

    });
    auto res = _snapshot_transfers.emplace(dst, std::move(f));
    assert(res.second);
    return ;
}

future<> server::apply_snapshot(server_id from, install_snapshot snp) {
    _fsm->step(from, std::move(snp));
    // Only one snapshot can be received at a time
    assert(! _snapshot_application_done);
    _snapshot_application_done = promise<snapshot_reply>();
    return _snapshot_application_done->get_future().then([] (snapshot_reply&& reply) {
        if (!reply.success) {
            throw std::runtime_error("Snapshot application failed");
        }
    });
}

future<> server::applier_fiber() {
    logger.trace("applier_fiber start");
    try {
        while (true) {
            auto opt_batch = co_await _apply_entries.reader.read();
            if (!opt_batch) {
                // EOF
                break;
            }
            std::vector<command_cref> commands;
            commands.reserve(opt_batch->size());

            std::ranges::copy(
                    *opt_batch |
                    std::views::filter([] (log_entry_ptr& entry) { return std::holds_alternative<command>(entry->data); }) |
                    std::views::transform([] (log_entry_ptr& entry) { return std::cref(std::get<command>(entry->data)); }),
                    std::back_inserter(commands));

            co_await _state_machine->apply(std::move(commands));
            notify_waiters(_awaited_applies, *opt_batch);
        }
    } catch (...) {
        logger.error("applier fiber {} stopped because of the error: {}", _id, std::current_exception());
    }
    co_return;
}

future<> server::read_barrier() {
    if (_fsm->can_read()) {
        co_return;
    }

    co_await apply_dummy_entry();
    co_return;
}

future<> server::abort() {
    logger.trace("abort() called");
    _fsm->stop();
    {
        // there is not explicit close for the pipe!
        auto tmp = std::move(_apply_entries.writer);
    }
    for (auto& ac: _awaited_commits) {
        ac.second.done.set_exception(stopped_error());
    }
    for (auto& aa: _awaited_applies) {
        aa.second.done.set_exception(stopped_error());
    }
    _awaited_commits.clear();
    _awaited_applies.clear();
    _ticker.cancel();

    if (_snapshot_application_done) {
        _snapshot_application_done->set_exception(std::runtime_error("Snapshot application aborted"));
    }

    auto snp_futures = std::views::values(_snapshot_transfers);
    // For c++20 ranges iterator to an end is of a different type, so adaptor is needed
    // since seastar primitives are not c++20 ready.
    using CI = std::common_iterator<decltype(snp_futures.begin()), decltype(snp_futures.end())>;
    auto snapshots = seastar::when_all_succeed(CI(snp_futures.begin()), CI(snp_futures.end()));

    return seastar::when_all_succeed(std::move(_io_status), std::move(_applier_status),
            _rpc->abort(), _state_machine->abort(), _storage->abort(), std::move(snapshots)).discard_result();
}

void server::make_me_leader() {
    _fsm->become_leader();
}

std::ostream& operator<<(std::ostream& os, const server& s) {
    os << "[id: " << short_id(s._id) << ", fsm (" << s._fsm << ")]\n";
    return os;
}

} // end of namespace raft