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

#include "test/lib/raft_utils.hh"
#include <boost/test/unit_test.hpp>

using raft::term_t, raft::index_t, raft::server_id;

// Helper for visitors
template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
// TODO: remove this deduction guidance once it's not needed (C++20)
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

class disconnected_nodes {
    std::unordered_set<server_id> _nodes; // Nodes currently disconnected
public:
    void disconnect(server_id id) {
        _nodes.insert(id);
    }
    void reconnect(server_id id) {
        _nodes.erase(id);
    }
    bool is_disconnected(server_id id) {
        return _nodes.find(id) != _nodes.end();
    }
};

void election_threshold(seastar::lw_shared_ptr<raft::fsm> fsm) {
    for (int i = 0; i <= raft::ELECTION_TIMEOUT.count(); i++) {
        fsm->tick();
    }
}

void election_timeout(seastar::lw_shared_ptr<raft::fsm> fsm) {
    for (int i = 0; i <= 2 * raft::ELECTION_TIMEOUT.count(); i++) {
        fsm->tick();
    }
}

struct failure_detector: public raft::failure_detector {
    bool alive = true;
    // TODO: use map id:alive
    bool is_alive(server_id from) override {
        return alive;
    }
};

// XXX create log_entries
raft::log create_log(std::vector<log_entry> entries, unsigned start_idx) {
    raft::log_entries log_entries;

    raft::snapshot snp;  // XXX init

    unsigned i = start_idx;
    for (auto e : entries) {
        raft::command command;
        ser::serialize(command, e.value);
        // XXX log_entries.emplace_back(raft::log_entry{raft::term_t(e.term), raft::index_t(i++), std::move(command)});
    }

    return raft::log(std::move(snp), std::move(log_entries));
}

// Runs a configured test case
// NOTE: creates ids for number of nodes, and fsms are first few as also specified
class Test {
protected:
    unsigned _nodes;
    std::string _name;
    unsigned _initial_term;
    std::optional<unsigned> initial_leader;
    std::vector<std::vector<log_entry>> initial_logs;
    std::vector<step> _steps;

    term_t _current_term;
    disconnected_nodes out; 
    std::vector<failure_detector> fds;
    std::vector<seastar::lw_shared_ptr<raft::fsm>> _fsms;

    void _run_test() {
fmt::print("Test {}\n", _name);

        raft::index_t idx;
        raft::fsm_output output;
        raft::vote_request vreq;
        raft::vote_reply vrepl;
        raft::append_reply arepl;

        //
        // Run
        //
fmt::print("Run\n");

        for (auto& [actions, expect]: _steps) {
            // Actions
            for (auto& action: actions) {
                std::visit(overloaded{
                    [&](simple_action& action) {
                        switch (action) {
                        case receptive_all:
                            for (auto fsm: _fsms) {
                                election_timeout(fsm);
                            }
                                break;
                        }
                    },
                    [&](struct candidate& action) {
                        election_threshold(_fsms[action.id]);
                    },
                    [&](struct receptive& action) {
                        election_timeout(_fsms[action.id]);
                    },
                    [&](struct elect& action) {
                        unsigned candidate = action.id;
                        server_id candidate_id{utils::UUID(0, action.id + 1)};
                        // Make all fsms receptive
                        for (auto fsm: _fsms) {
                            election_threshold(fsm);
                            auto output = fsm->get_output();  // ignore output (i.e. vote requests)
                        }
                        // Make defined fsm candidate, force votes
                        // NOTE: we skip doing vote handling at other fsms for now
                        election_timeout(_fsms[candidate]);
                        BOOST_CHECK(_fsms[candidate]->is_candidate());
                        auto output = _fsms[candidate]->get_output();
                        _current_term = output.term;
                        for (auto& [id, msg] : output.messages) {
                            auto vreq = get_req<raft::vote_request>(msg);
                            _fsms[candidate]->step(id, raft::vote_reply{vreq.current_term, true});
                        }
                        BOOST_CHECK(_fsms[candidate]->is_leader());
fmt::print("     candidate output term {} entries {} messages {} committed {}\n", output.term, output.log_entries.size(), output.messages.size(), output.committed.size());
                        // Finally, handle dummy entry propagation
                        output = _fsms[candidate]->get_output();
                        get_req<raft::log_entry::dummy>(output.log_entries[0]->data);
                        output = _fsms[candidate]->get_output();
                        BOOST_CHECK(output.messages.size() == _nodes - 1);
                        for (auto& [id, msg] : output.messages) {
                            // Get request for one node
                            auto areq = get_req<raft::append_request>(msg);
                            BOOST_CHECK(areq.entries.size() == 1);
                            raft::log_entry_ptr areq_lep =  areq.entries.back();
                            BOOST_CHECK(areq_lep->term == _current_term);
                            get_req<raft::log_entry::dummy>(areq_lep->data);
                            unsigned dst_id = id.id.get_least_significant_bits() - 1;
                            if (dst_id < _fsms.size()) {
                                // Propagate and get reply from other fsms
                                _fsms[dst_id]->step(candidate_id, std::move(areq));
                                auto follower_output = _fsms[dst_id]->get_output();
                                BOOST_CHECK(follower_output.messages.size() == 1);
                                auto& [reply_dst, reply] = follower_output.messages.back();
                                BOOST_CHECK(reply_dst == candidate_id);
                                auto arep = get_req<raft::append_reply>(reply);
                                get_req<raft::append_reply::accepted>(arep.result);
                                _fsms[candidate]->step(id, std::move(arep));
                            } else {
                                // Reply from virtual nodes
                                _fsms[candidate]->step(id,
                                        raft::append_reply{_current_term, areq_lep->idx,
                                                raft::append_reply::accepted{areq_lep->idx}});
                            }
                        }
                        output = _fsms[candidate]->get_output();
                        BOOST_CHECK(output.committed.size() == 1); // Dummy committed
                    },
                    [&](struct disconnect& action) {
                        out.disconnect(server_id{utils::UUID(0, action.id + 1)});
                    },
                    [&](struct reconnect& action) {
                        out.reconnect(server_id{utils::UUID(0, action.id + 1)});
                    },
                }, action);
            }

            // Expected
            for (auto& e: expect) {
                output = _fsms[e.id]->get_output();
fmt::print("     expect [{}] output term {} entries {} messages {} committed {}\n", e.id, output.term, output.log_entries.size(), output.messages.size(), output.committed.size());
                if (e.follower) {
fmt::print("    [{}] is follower {} {}\n", e.id, e.follower, _fsms[e.id]->is_follower());
                    BOOST_CHECK(_fsms[e.id]->is_follower());
                }
                if (e.candidate) {
fmt::print("    [{}] is candidate {} {}\n", e.id, e.candidate, _fsms[e.id]->is_candidate());
                    BOOST_CHECK(_fsms[e.id]->is_candidate());
                }
                if (e.leader) {
fmt::print("    [{}] is leader {} {}\n", e.id, e.leader, _fsms[e.id]->is_leader());
                    BOOST_CHECK(_fsms[e.id]->is_leader());
                }
                if (e.term) {
fmt::print("    [{}] term {} output term {}\n", e.id, e.term, output.term);
                    BOOST_CHECK(output.term == e.term);
                }
            }
        }

    }
    template<typename T, typename S>
    T get_req(S obj) {
        T ret;
        BOOST_REQUIRE_NO_THROW(ret = std::get<T>(obj));
        return ret;
    }
public:
    Test(test_case test) : _nodes(test.nodes), _name(test.name),
            _initial_term(test.initial_term), _steps(std::move(test.steps)) {
        for (unsigned n = 0; n < test.fsms; n++) {
            fds.emplace_back(failure_detector{});
        }

        raft::configuration cfg;
        raft::fsm_config fsm_cfg{.append_request_threshold = 1};
        cfg.current.reserve(_nodes);
        std::vector<server_id> ids;
        for (unsigned s = 0; s < _nodes; s++) {
            ids.emplace_back(server_id{utils::UUID(0, s + 1)});
            cfg.current.emplace(raft::server_address{ids[s]});
        }
        for (unsigned f = 0; f < test.fsms; f++) {
#if 0
            raft::log log;
            if (test.initial_logs.size() > f) {
                log = create_log(test.initial_logs[f], 1);
            } else {
                log = raft::log{raft::snapshot{.config = cfg}};
            }
#else
            raft::log log{raft::snapshot{.config = cfg}};
#endif
            _fsms.push_back(seastar::make_lw_shared<raft::fsm>(ids[f], term_t{_initial_term},
                        server_id{}, std::move(log), fds.at(f), fsm_cfg));
        }
        _run_test();
    }
};


void Tester::_test(test_case test) {
    Test t{std::move(test)};
}

