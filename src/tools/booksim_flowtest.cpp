// $Id$

/*
 Copyright (c) 2007-2015, Trustees of The Leland Stanford Junior University
 All rights reserved. Distributed under the BookSim BSD license; see LICENSE.md.
*/

// booksim_flowtest -- replay a collective communication schedule over a BookSim
// topology and report completion time.
//
// This is the standalone half of the SimAI coupling: it exercises exactly the
// flow-injection and completion-callback paths that the ASTRA-sim backend will
// use, but with a schedule the caller writes by hand (or generates with
// utils/gen_collective.py) instead of one produced by SimCCL. That keeps the
// hard parts of the BookSim side testable without needing NCCL, CUDA, or the
// astra-sim build.
//
// Trace format, one flow per line, '#' starts a comment:
//
//     <id> <src> <dst> <bytes> [after=<id>[,<id>...]] [at=<ns>] [class=P|NP|Cpl]
//
// A flow is released when every flow named in `after` has been fully received
// AND the clock has reached `at` (default 0). Dependencies are on the RECEIVE
// side, which is what a ring collective needs: rank i cannot forward its
// step k+1 chunk until it has received the step k chunk from rank i-1.
//
// class= is the PCIe ordering class (Posted/Non-Posted/Completion) for the
// VC-range/credit-pool separation described in PCIE_MODEL_LIMITATIONS.md --
// omitted means untagged, the full undivided VC pool (pre-existing behavior).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "engine/booksim_engine.hpp"

namespace {

struct TraceFlow {
  long long        id;
  int              src;
  int              dst;
  unsigned long long bytes;
  unsigned long long at;
  int              pcie_class;  // 0=unset, 1=P, 2=NP, 3=Cpl -- see FlowDesc::pcie_class
  std::vector<long long> after;

  int              deps_remaining;
  bool             released;
  booksim::FlowId  engine_id;
  unsigned long long t_done;

  TraceFlow() : id(-1), src(-1), dst(-1), bytes(0), at(0), pcie_class(0),
                deps_remaining(0), released(false),
                engine_id(booksim::kInvalidFlow), t_done(0) {}
};

std::vector<TraceFlow>          g_flows;
std::map<long long, size_t>     g_by_id;      // trace id -> index
std::map<booksim::FlowId, size_t> g_by_engine; // engine id -> index
std::vector<std::vector<size_t> > g_dependents;
size_t g_completed = 0;
unsigned long long g_last_completion = 0;
bool g_verbose = false;

void Die(const std::string & msg) {
  std::cerr << "booksim_flowtest: " << msg << std::endl;
  exit(2);
}

void SplitCsv(const std::string & s, std::vector<long long> & out) {
  std::string cur;
  for(size_t i = 0; i <= s.size(); ++i) {
    if(i == s.size() || s[i] == ',') {
      if(!cur.empty()) out.push_back(atoll(cur.c_str()));
      cur.clear();
    } else {
      cur += s[i];
    }
  }
}

void LoadTrace(const std::string & path) {
  std::ifstream in(path.c_str());
  if(!in.is_open()) Die("cannot open trace " + path);

  std::string line;
  int line_no = 0;
  while(std::getline(in, line)) {
    ++line_no;
    std::string::size_type hash = line.find('#');
    if(hash != std::string::npos) line = line.substr(0, hash);

    std::istringstream ss(line);
    std::vector<std::string> tok;
    std::string t;
    while(ss >> t) tok.push_back(t);
    if(tok.empty()) continue;
    if(tok.size() < 4) {
      std::ostringstream os;
      os << "trace line " << line_no << ": need at least <id> <src> <dst> <bytes>";
      Die(os.str());
    }

    TraceFlow f;
    f.id    = atoll(tok[0].c_str());
    f.src   = atoi(tok[1].c_str());
    f.dst   = atoi(tok[2].c_str());
    f.bytes = strtoull(tok[3].c_str(), NULL, 10);

    for(size_t i = 4; i < tok.size(); ++i) {
      if(tok[i].compare(0, 6, "after=") == 0) {
        SplitCsv(tok[i].substr(6), f.after);
      } else if(tok[i].compare(0, 3, "at=") == 0) {
        f.at = strtoull(tok[i].c_str() + 3, NULL, 10);
      } else if(tok[i].compare(0, 6, "class=") == 0) {
        std::string v = tok[i].substr(6);
        if(v == "P") f.pcie_class = 1;
        else if(v == "NP") f.pcie_class = 2;
        else if(v == "Cpl") f.pcie_class = 3;
        else {
          std::ostringstream os;
          os << "trace line " << line_no << ": unknown class '" << v
             << "' (expected P, NP, or Cpl)";
          Die(os.str());
        }
      } else {
        std::ostringstream os;
        os << "trace line " << line_no << ": unknown field '" << tok[i] << "'";
        Die(os.str());
      }
    }

    if(g_by_id.count(f.id)) {
      std::ostringstream os;
      os << "trace line " << line_no << ": duplicate flow id " << f.id;
      Die(os.str());
    }
    g_by_id[f.id] = g_flows.size();
    g_flows.push_back(f);
  }

  // Resolve dependency edges now that every id is known.
  g_dependents.resize(g_flows.size());
  for(size_t i = 0; i < g_flows.size(); ++i) {
    for(size_t k = 0; k < g_flows[i].after.size(); ++k) {
      long long dep = g_flows[i].after[k];
      std::map<long long, size_t>::iterator it = g_by_id.find(dep);
      if(it == g_by_id.end()) {
        std::ostringstream os;
        os << "flow " << g_flows[i].id << " depends on unknown flow " << dep;
        Die(os.str());
      }
      g_dependents[it->second].push_back(i);
      ++g_flows[i].deps_remaining;
    }
  }
}

} // namespace

namespace {

booksim::Engine * g_engine = NULL;

// A flow is runnable once its dependencies have all been received. Release is
// still gated on the clock reaching its `at` time.
std::set<size_t> g_ready;   // dependency-satisfied, not yet injected

void MarkReady(size_t idx) {
  g_ready.insert(idx);
}

void OnRecv(booksim::FlowId id, void * /*user*/, uint64_t t, void * /*ctx*/) {
  std::map<booksim::FlowId, size_t>::iterator it = g_by_engine.find(id);
  if(it == g_by_engine.end()) return;
  size_t idx = it->second;

  g_flows[idx].t_done = t;
  ++g_completed;
  if(t > g_last_completion) g_last_completion = t;

  if(g_verbose) {
    std::cout << "  t=" << t << "  flow " << g_flows[idx].id << " ("
              << g_flows[idx].src << "->" << g_flows[idx].dst << ", "
              << g_flows[idx].bytes << " B) received" << std::endl;
  }

  for(size_t k = 0; k < g_dependents[idx].size(); ++k) {
    size_t d = g_dependents[idx][k];
    if(--g_flows[d].deps_remaining == 0) MarkReady(d);
  }
}

// Inject everything whose dependencies are met and whose release time has
// arrived. Returns the earliest future release time among flows that are ready
// but still waiting on the clock, or 0 if there is none.
unsigned long long ReleaseReady(unsigned long long now) {
  unsigned long long next_time = 0;
  std::vector<size_t> launched;

  for(std::set<size_t>::iterator it = g_ready.begin(); it != g_ready.end(); ++it) {
    TraceFlow & f = g_flows[*it];
    if(f.at > now) {
      if(next_time == 0 || f.at < next_time) next_time = f.at;
      continue;
    }
    booksim::FlowDesc d;
    d.src = f.src; d.dst = f.dst; d.bytes = f.bytes; d.cls = 0;
    d.pcie_class = f.pcie_class;
    // Thread the trace file's own id through as the opaque tag so a merged
    // multi-collective run's --csv output can be split back apart per
    // constituent trace (see utils/merge_traces.py). Not used for scheduling.
    d.user = reinterpret_cast<void*>(static_cast<intptr_t>(f.id));
    f.engine_id = g_engine->InjectFlow(d);
    f.released = true;
    g_by_engine[f.engine_id] = *it;
    launched.push_back(*it);
    if(g_verbose) {
      std::cout << "  t=" << now << "  flow " << f.id << " (" << f.src << "->"
                << f.dst << ", " << f.bytes << " B) injected" << std::endl;
    }
  }
  for(size_t i = 0; i < launched.size(); ++i) g_ready.erase(launched[i]);
  return next_time;
}

} // namespace

int main(int argc, char ** argv) {
  std::string config_file, trace_file, csv_file;
  bool fast_forward = true;
  std::vector<std::pair<std::string, std::string> > overrides;

  for(int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if(a == "--csv" && i + 1 < argc) {
      csv_file = argv[++i];
    } else if(a == "--no-ff") {
      fast_forward = false;              // for the fast-forward equivalence test
    } else if(a == "-v") {
      g_verbose = true;
    } else if(a.find('=') != std::string::npos && !config_file.empty()
              && !trace_file.empty()) {
      std::string::size_type eq = a.find('=');
      overrides.push_back(std::make_pair(a.substr(0, eq), a.substr(eq + 1)));
    } else if(config_file.empty()) {
      config_file = a;
    } else if(trace_file.empty()) {
      trace_file = a;
    } else {
      Die("unexpected argument " + a);
    }
  }

  if(config_file.empty() || trace_file.empty()) {
    std::cerr << "usage: booksim_flowtest <config> <trace> [--csv f] [--no-ff] "
              << "[-v] [key=value ...]" << std::endl;
    return 2;
  }

  LoadTrace(trace_file);
  if(g_flows.empty()) Die("trace contains no flows");

  g_engine = booksim::Engine::Create(config_file, overrides);
  if(!g_engine) Die("engine creation failed");
  g_engine->SetOnRecvComplete(&OnRecv, NULL);

  const int nodes = g_engine->NumNodes();
  for(size_t i = 0; i < g_flows.size(); ++i) {
    if(g_flows[i].src < 0 || g_flows[i].src >= nodes ||
       g_flows[i].dst < 0 || g_flows[i].dst >= nodes) {
      std::ostringstream os;
      os << "flow " << g_flows[i].id << " references a node outside 0.."
         << (nodes - 1);
      Die(os.str());
    }
    if(g_flows[i].deps_remaining == 0) MarkReady(i);
  }
  if(g_ready.empty()) Die("every flow has an unmet dependency (cycle in the trace)");

  std::cout << "=== booksim_flowtest ===" << std::endl;
  std::cout << "config  " << config_file << std::endl;
  std::cout << "trace   " << trace_file << " (" << g_flows.size() << " flows, "
            << nodes << " nodes)" << std::endl;
  std::cout << "fast-forward " << (fast_forward ? "on" : "off") << std::endl;

  const unsigned long long kStallLimit = 200000000ull;
  unsigned long long last_progress = 0;
  size_t last_completed = 0;

  while(g_completed < g_flows.size()) {
    unsigned long long now = g_engine->Time();
    unsigned long long next_at = ReleaseReady(now);

    if(g_engine->IsIdle()) {
      // Nothing in flight. Either we are waiting for a future release time, or
      // the remaining flows are blocked on dependencies that can never resolve.
      if(next_at > now) {
        if(fast_forward) {
          g_engine->AdvanceTo(next_at);
          continue;
        }
      } else if(g_ready.empty() && g_engine->FlowsOutstanding() == 0) {
        std::cerr << "booksim_flowtest: deadlock -- network idle with "
                  << (g_flows.size() - g_completed) << " flows incomplete and "
                  << "none runnable. Dependency cycle, or a completion was lost."
                  << std::endl;
        break;
      }
    }

    g_engine->Step();

    if(g_completed != last_completed) {
      last_completed = g_completed;
      last_progress = g_engine->Time();
    } else if(g_engine->Time() - last_progress > kStallLimit) {
      std::cerr << "booksim_flowtest: no completion in " << kStallLimit
                << " cycles; giving up. outstanding="
                << g_engine->FlowsOutstanding() << std::endl;
      break;
    }
  }

  // The last flits have retired, but their credits are still travelling back
  // upstream. Drain them so the engine reaches a genuinely quiescent state --
  // TrafficManager::Run() does the same thing after its own drain loop, and
  // IsIdle() deliberately counts outstanding credits because AdvanceTo() is
  // only safe once the credit channels' wait queues are empty too.
  {
    unsigned long long guard = 0;
    while(!g_engine->IsIdle() && guard < 1000000ull) {
      g_engine->Step();
      ++guard;
    }
  }

  // ---- results -------------------------------------------------------------
  unsigned long long makespan = g_last_completion;
  unsigned long long total_bytes = 0;
  for(size_t i = 0; i < g_flows.size(); ++i) total_bytes += g_flows[i].bytes;

  std::cout << std::endl << "--- results ---" << std::endl;
  std::cout << "flows completed      " << g_completed << " / " << g_flows.size()
            << std::endl;
  std::cout << "makespan             " << makespan << " cycles" << std::endl;
  std::cout << "total bytes moved    " << total_bytes << std::endl;
  if(makespan > 0) {
    // One cycle is one ns by construction, so bytes/cycle is GB/s directly.
    double gbps = (double)total_bytes / (double)makespan;
    std::cout << "aggregate throughput " << gbps << " B/cycle" << std::endl;
  }

  // Exit-state check. A non-empty residue here is the signature of a lost
  // completion, which is the failure mode that looks identical to success.
  std::cout << "engine idle          " << (g_engine->IsIdle() ? "yes" : "NO")
            << std::endl;
  std::cout << "flows outstanding    " << g_engine->FlowsOutstanding()
            << std::endl;

  bool ok = (g_completed == g_flows.size()) && g_engine->IsIdle() &&
            (g_engine->FlowsOutstanding() == 0);

  if(!csv_file.empty()) {
    std::ofstream out(csv_file.c_str());
    if(!out.is_open()) Die("cannot write " + csv_file);
    g_engine->DumpFlowCsv(out);
    std::cout << "per-flow csv         " << csv_file << std::endl;
  }

  delete g_engine;
  g_engine = NULL;

  std::cout << (ok ? "OK" : "FAILED") << std::endl;
  return ok ? 0 : 1;
}
