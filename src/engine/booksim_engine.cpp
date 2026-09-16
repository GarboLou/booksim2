// $Id$

/*
 Copyright (c) 2007-2015, Trustees of The Leland Stanford Junior University
 All rights reserved. Distributed under the BookSim BSD license; see LICENSE.md.
*/

#include <cassert>
#include <iostream>
#include <sstream>

#include "engine/booksim_engine.hpp"

#include "booksim.hpp"
#include "booksim_config.hpp"
#include "booksim_init.hpp"
#include "flowtrafficmanager.hpp"
#include "globals.hpp"
#include "network.hpp"

namespace booksim {

const FlowId kInvalidFlow = ~(FlowId)0;

struct Engine::Impl {
  BookSimConfig         config;
  std::vector<Network*> net;
  FlowTrafficManager *  tm;
  Impl() : tm(NULL) {}
};

namespace {

// Trampolines: FlowTrafficManager's callback type is structurally identical to
// booksim::FlowCb but nominally distinct, so bridge through a small context.
struct CbCtx {
  FlowCb cb;
  void * ctx;
  CbCtx() : cb(NULL), ctx(NULL) {}
};
CbCtx g_send_cb;
CbCtx g_recv_cb;

void SendTrampoline(FlowTrafficManager::FlowId id, void * user,
                    unsigned long long t, void * /*ctx*/) {
  if(g_send_cb.cb) g_send_cb.cb((FlowId)id, user, (uint64_t)t, g_send_cb.ctx);
}
void RecvTrampoline(FlowTrafficManager::FlowId id, void * user,
                    unsigned long long t, void * /*ctx*/) {
  if(g_recv_cb.cb) g_recv_cb.cb((FlowId)id, user, (uint64_t)t, g_recv_cb.ctx);
}

// Fail loudly on configurations the coupling has not been validated against,
// rather than producing plausible-looking but wrong numbers.
void CheckInvariants(const BookSimConfig & config) {
  const std::string router = config.GetStr("router");
  if(router != "iq") {
    std::cerr << "booksim::Engine: router must be 'iq' (got '" << router
              << "'). The idle fast-forward analysis only covers IQRouter."
              << std::endl;
    exit(-1);
  }

  const std::string bp = config.GetStr("buffer_policy");
  if(bp != "private" && bp != "shared") {
    std::cerr << "booksim::Engine: buffer_policy must be 'private' or 'shared' "
              << "(got '" << bp << "'). The feedback policies carry "
              << "absolute-time state that a clock jump would invalidate."
              << std::endl;
    exit(-1);
  }

  if(config.GetInt("subnets") != 1) {
    std::cerr << "booksim::Engine: subnets must be 1; the topology routing "
              << "table is a single process-wide object." << std::endl;
    exit(-1);
  }

  if(config.GetStr("sim_type") != "flow") {
    std::cerr << "booksim::Engine: sim_type must be 'flow'." << std::endl;
    exit(-1);
  }
}

} // namespace

Engine::Engine() : p_(new Impl) {}

Engine * Engine::Create(const std::string & config_file,
                        const std::vector<std::pair<std::string, std::string> > & overrides)
{
  if(trafficManager != NULL) {
    std::cerr << "booksim::Engine: an engine already exists in this process."
              << std::endl;
    return NULL;
  }

  Engine * e = new Engine();
  Impl * p = e->p_;

  p->config.ParseFile(config_file);
  for(size_t i = 0; i < overrides.size(); ++i) {
    // Configuration::ParseString appends the terminating ';' itself; adding one
    // here produces "key = value;;" and a parse error.
    std::ostringstream os;
    os << overrides[i].first << " = " << overrides[i].second;
    p->config.ParseString(os.str());
  }

  CheckInvariants(p->config);

  // Must precede Network/TrafficManager construction: both look up
  // gRoutingFunctionMap, which this populates.
  BooksimGlobalInit(p->config);

  const int subnets = p->config.GetInt("subnets");
  p->net.resize(subnets);
  for(int i = 0; i < subnets; ++i) {
    std::ostringstream name;
    name << "network_" << i;
    p->net[i] = Network::New(p->config, name.str());
  }

  p->tm = new FlowTrafficManager(p->config, p->net);
  trafficManager = p->tm;

  g_send_cb = CbCtx();
  g_recv_cb = CbCtx();
  p->tm->SetOnSendComplete(&SendTrampoline, NULL);
  p->tm->SetOnRecvComplete(&RecvTrampoline, NULL);

  return e;
}

Engine::~Engine()
{
  for(size_t i = 0; i < p_->net.size(); ++i) delete p_->net[i];
  delete p_->tm;
  // Simulate() asserts trafficManager is NULL before constructing one, and
  // ~TrafficManager deletes gWatchOut without nulling it; clear both so a
  // second Engine can be created in the same process.
  trafficManager = NULL;
  BooksimGlobalCleanup();
  g_send_cb = CbCtx();
  g_recv_cb = CbCtx();
  delete p_;
}

int      Engine::NumNodes() const { return p_->net[0]->NumNodes(); }
uint64_t Engine::Time()     const { return p_->tm->Now(); }
bool     Engine::IsIdle()   const { return p_->tm->IsIdle(); }
void     Engine::Step()           { p_->tm->Step(); }
void     Engine::AdvanceTo(uint64_t t) { p_->tm->AdvanceTo(t); }
uint64_t Engine::FlowsOutstanding() const { return p_->tm->FlowsOutstanding(); }

FlowId Engine::InjectFlow(const FlowDesc & d)
{
  return (FlowId)p_->tm->InjectFlow(d.src, d.dst, d.bytes, d.cls, d.pcie_class, d.user);
}

void Engine::SetOnSendComplete(FlowCb cb, void * ctx) {
  g_send_cb.cb = cb; g_send_cb.ctx = ctx;
}
void Engine::SetOnRecvComplete(FlowCb cb, void * ctx) {
  g_recv_cb.cb = cb; g_recv_cb.ctx = ctx;
}

void Engine::DumpStats(std::ostream & os) const   { p_->tm->WriteStats(os); }
void Engine::DumpFlowCsv(std::ostream & os) const { p_->tm->DumpFlowCsv(os); }

} // namespace booksim
