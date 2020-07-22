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
#include "fsm.hh"

namespace raft {

log_entry& log::operator[](size_t i) {
    assert(index_t(i) >= _start_idx);
    return _log[i - _start_idx];
}

void log::emplace_back(log_entry&& e) {
    _log.emplace_back(std::move(e));
}

bool log::empty() const {
    return _log.empty();
}

index_t log::last_idx() const {
    return index_t(_log.size()) + _start_idx - index_t(1);
}

index_t log::next_idx() const {
    return last_idx() + index_t(1);
}

void log::truncate_head(size_t i) {
    auto it = _log.begin() + (i - _start_idx);
    _log.erase(it, _log.end());
}

index_t log::start_idx() const {
    return _start_idx;
}

void log::stable_to(index_t idx) {
    assert(idx <= last_idx());
    _stable_idx = idx;
}

fsm::fsm(server_id id, term_t current_term, server_id voted_for, log log) :
        _my_id(id), _current_term(current_term), _voted_for(voted_for),
        _log(std::move(log)) {

    logger.trace("{}: starting log length {}", _my_id, _log.last_idx());

    assert(_current_leader.is_nil());
}

const log_entry& fsm::add_entry(command command) {
    // It's only possible to add entries on a leader
    check_is_leader();

    _log.emplace_back(log_entry{_current_term, _log.next_idx(), std::move(command)});

    return _log[_log.last_idx()];
}

void fsm::become_leader() {

    assert(_state != server_state::LEADER);
    assert(!_progress);
    _state = server_state::LEADER;
    _current_leader = _my_id;
    _progress.emplace();
}

void fsm::become_follower(server_id leader) {

    assert(_state != server_state::FOLLOWER);
    _current_leader = leader;
    _state = server_state::FOLLOWER;
    _progress = std::nullopt;
}

void fsm::stable_to(term_t term, index_t idx) {
    if (_log.last_idx() < idx) {
        // The log was truncated while being persisted
        return;
    }

    if (_log[idx].term == term) {
        // If the terms do not match it means the log was truncated.
        _log.stable_to(idx);
        if (is_leader()) {
            (*_progress)[_my_id].match_idx = idx;
            (*_progress)[_my_id].next_idx = index_t{idx + 1};
        }
    }
}

} // end of namespace raft
