// ----------------------------------------------------------------------
// File: RaftState.cc
// Author: Georgios Bitzes - CERN
// ----------------------------------------------------------------------

/************************************************************************
 * quarkdb - a redis-like highly available key-value store              *
 * Copyright (C) 2016 CERN/Switzerland                                  *
 *                                                                      *
 * This program is free software: you can redistribute it and/or modify *
 * it under the terms of the GNU General Public License as published by *
 * the Free Software Foundation, either version 3 of the License, or    *
 * (at your option) any later version.                                  *
 *                                                                      *
 * This program is distributed in the hope that it will be useful,      *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

#include "RaftState.hh"

using namespace quarkdb;

RaftServer RaftState::BLOCKED_VOTE = { "VOTING_BLOCKED_FOR_THIS_TERM", -1 };

RaftState::RaftState(RaftJournal &jr, const RaftServer &me)
: journal(jr), myself(me) {

  status = RaftStatus::OBSERVER;
  if(contains(journal.getNodes(), myself)) {
    status = RaftStatus::FOLLOWER;
  }

  term = journal.getCurrentTerm();
  votedFor = journal.getVotedFor();
}

//------------------------------------------------------------------------------
// Term is atomic, so this function is for cases where we can tolerate race
// conditions. (for example, when observed() has entered but hasn't completed)
//------------------------------------------------------------------------------
RaftTerm RaftState::getCurrentTerm() {
  return term;
}

//------------------------------------------------------------------------------
// This is for cases where we NEED a full, consistent state free of potential
// races.
//
// This is needed because this would not be safe:
// state.getCurrentTerm()
// state.getCurrentStatus()
//
// The state could have changed in-between, leading to horrible bugs.
//------------------------------------------------------------------------------

RaftStateSnapshot RaftState::getSnapshot() {
  std::lock_guard<std::mutex> lock(update);
  return {term, status, leader, votedFor};
}

RaftServer RaftState::getMyself() {
  return myself;
}

void RaftState::declareEvent(RaftTerm observedTerm, const RaftServer &observedLeader) {
  if(observedTerm > term) {
    qdb_event("Progressing raft state: " << term << " ==> " << observedTerm);
  }
  if(!observedLeader.empty()) {
    qdb_event("Recognizing as leader for term " << observedTerm << ": " << observedLeader.toString());
  }
}

bool RaftState::becomeCandidate(RaftTerm forTerm) {
  std::lock_guard<std::mutex> lock(update);

  if(forTerm != term) {
    // we got hit by a race.. do nothing
    return false;
  }

  if(status != RaftStatus::FOLLOWER) {
    qdb_critical("attempted to become a candidate without first being a follower for term " << forTerm);
    return false;
  }

  if(!leader.empty()) {
    qdb_critical("attempted to become a candidate for term " << term << " while having recognized "
                 << leader.toString() << " as leader already");
    return false;
  }

  if(!votedFor.empty()) {
    qdb_critical("attempted to become a candidate for term " << term << " while having voted already for " << votedFor.toString());
    return false;
  }

  votedFor = myself;
  this->updateJournal();
  status = RaftStatus::CANDIDATE;
  return true;
}

bool RaftState::ascend(RaftTerm forTerm) {
  std::lock_guard<std::mutex> lock(update);

  if(forTerm != term) {
    // we got hit by a race.. do nothing
    return false;
  }

  if(status != RaftStatus::CANDIDATE) {
    qdb_critical("attempted to ascend without being a candidate for term " << forTerm << ".");
    return false;
  }

  if(!leader.empty()) {
    // we have already recognized a leader for the current term..
    // something is wrong, do nothing
    qdb_critical("attempted to ascend for term " << term << " while having recognized "
                 << leader.toString() << " as leader already");
    return false;
  }

  if(votedFor != myself) {
    qdb_critical("attempted to ascend in term " << forTerm << " without having voted for myself first");
    return false;
  }

  leader = myself;
  status = RaftStatus::LEADER;
  qdb_event("Ascending as leader for term " << forTerm << ". Long may I reign.");
  return true;
}

//------------------------------------------------------------------------------
// This function should be called AFTER we have established that the raft log
// of the server asking a vote is at least up-to-date as ours.
//------------------------------------------------------------------------------
bool RaftState::grantVote(RaftTerm forTerm, const RaftServer &vote) {
  std::lock_guard<std::mutex> lock(update);

  if(status != RaftStatus::FOLLOWER) {
    qdb_warn("attempted to vote for " << vote.toString() << " while in status " << statusToString(status));
    return false;
  }

  if(forTerm != term) {
    // we got hit by a race.. term has progressed since this
    // function got called. Do nothing.
    return false;
  }

  if(!leader.empty()) {
    // we have already recognized a leader for the current term..
    // voting for another makes zero sense
    qdb_critical("attempted to vote for " << vote.toString() << " and term "
                 << term << " while there's already an established leader: " << leader.toString());
    return false;
  }

  if(!votedFor.empty()) {
    // ok, this is worrying, but could still be explained by a race.
    // but should not normally happen, given that servicing of requestVote is
    // serialized
    qdb_critical("attempted to change vote for term " << term << ": " << votedFor.toString() << " ==> " << vote.toString());
    return false;
  }

  qdb_event("Granting vote for term " << forTerm << " to " << vote.toString());
  votedFor = vote;
  this->updateJournal();
  return true;
}

bool RaftState::joinCluster(RaftTerm forTerm) {
  std::lock_guard<std::mutex> lock(update);

  if(forTerm != term) return false;

  if(status != RaftStatus::OBSERVER) {
    qdb_critical("attempted to join cluster but I'm already not an observer.");
    return false;
  }

  std::vector<RaftServer> nodes = journal.getNodes();
  if(!contains(nodes, myself)) {
    qdb_critical("attempted to join cluster, but I'm not part of the participating nodes: " << serializeNodes(nodes));
    return false;
  }

  status = RaftStatus::FOLLOWER;
  return true;
}

bool RaftState::becomeObserver(RaftTerm forTerm) {
  std::lock_guard<std::mutex> lock(update);

  if(forTerm != term) return false;

  if(status != RaftStatus::FOLLOWER && status != RaftStatus::CANDIDATE) {
    qdb_critical("attempted to become an observer while status = " << statusToString(status));
    return false;
  }

  std::vector<RaftServer> nodes = journal.getNodes();
  if(contains(nodes, myself)) {
    qdb_critical("attempted to become an observer, but I'm still part of the participating nodes: " << serializeNodes(nodes));
    return false;
  }

  status = RaftStatus::OBSERVER;
  return true;
}

//------------------------------------------------------------------------------
// We must call updateJournal after having made changes to either term
// or votedFor.
//------------------------------------------------------------------------------
void RaftState::updateJournal() {
  journal.setCurrentTerm(term, votedFor);
}

bool RaftState::observed(RaftTerm observedTerm, const RaftServer &observedLeader) {
  std::lock_guard<std::mutex> lock(update);

  // observed a newer term, step down if leader / candidate
  if(observedTerm > term) {
    if(status != RaftStatus::OBSERVER) {
      status = RaftStatus::FOLLOWER;
    }
    declareEvent(observedTerm, observedLeader);

    votedFor.clear();
    term = observedTerm;
    leader = observedLeader;

    //--------------------------------------------------------------------------
    // If observedLeader is not empty, we have already discovered the leader for
    // this term, which should never change.
    // We set votedFor to an invalid value to prevent this node from voting for
    // another server in this term after a crash.
    // This is not strictly necessary to do, according to the raft description,
    // but let's be conservative.
    //--------------------------------------------------------------------------
    if(!observedLeader.empty()) {
      votedFor = BLOCKED_VOTE;
    }

    this->updateJournal();
    return true;
  }
  else if(observedTerm == term && leader.empty()) {
    declareEvent(observedTerm, observedLeader);
    leader = observedLeader;

    //--------------------------------------------------------------------------
    // Block any more votes for the current term, same reason as above
    //--------------------------------------------------------------------------
    if(!leader.empty() && votedFor.empty()) {
      votedFor = BLOCKED_VOTE;
      this->updateJournal();
    }

    return true;
  }
  else if(observedTerm == term && !leader.empty() && leader != observedLeader) {
    qdb_critical("attempted to change leader for term " << term << ": " << leader.toString() << " ==> " << observedLeader.toString());
  }

  return false;
}

//------------------------------------------------------------------------------
// Helper function
//------------------------------------------------------------------------------
std::string quarkdb::statusToString(RaftStatus st) {
  if(st == RaftStatus::LEADER) return "LEADER";
  if(st == RaftStatus::FOLLOWER) return "FOLLOWER";
  if(st == RaftStatus::CANDIDATE) return "CANDIDATE";
  if(st == RaftStatus::OBSERVER) return "OBSERVER";
  if(st == RaftStatus::SHUTDOWN) return "SHUTDOWN";

  qdb_throw("unrecognized RaftStatus");
}