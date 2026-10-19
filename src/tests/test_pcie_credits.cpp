// PCIe VC0 credit model at unit level: header and data credits, their
// derivation from the config, whole-TLP admission and credit conservation.

#include <cstdlib>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

#include "test_util.hpp"
#include "buffer_state.hpp"
#include "credit.hpp"
#include "flit.hpp"
#include "pcie_credits.hpp"
#include "routefunc.hpp"

namespace {

int Flits(int payload, int overhead, int flit_bytes) {
  return (payload + overhead + flit_bytes - 1) / flit_bytes;
}

void DataReturns() {
  // 256 B payload, 24 B overhead, 64 B flits: payload bytes complete at
  // 40, 104, 168, 232, 256 -> 2, 6, 10, 14, 16 units.
  std::vector<int> r = PcieDataReturns(256, 24, 64, 5);
  int expect256[] = {2, 4, 4, 4, 2};
  CHECK_EQ(r.size(), 5u);
  for(int i = 0; i < 5; ++i) CHECK_EQ(r[i], expect256[i]);
  r = PcieDataReturns(64, 24, 64, 2);           // short final TLP: 4 units
  CHECK_EQ(r[0] + r[1], 4);
  CHECK_EQ(r[0], 2);
  r = PcieDataReturns(100, 24, 64, 2);          // non-16-B multiple: 7 units
  CHECK_EQ(r[0], 2);
  CHECK_EQ(r[1], 5);
  r = PcieDataReturns(8, 24, 64, 1);
  CHECK_EQ(r[0], 1);
  // Every payload, flit size and overhead: non-negative, sums to the units.
  for(int fb = 16; fb <= 128; fb *= 2) {
    for(int ov = 0; ov <= 40; ov += 8) {
      for(int p = 1; p <= 1024; ++p) {
        int n = Flits(p, ov, fb);
        std::vector<int> v = PcieDataReturns(p, ov, fb, n);
        int sum = 0;
        bool nonneg = true;
        for(size_t i = 0; i < v.size(); ++i) { sum += v[i]; nonneg &= v[i] >= 0; }
        CHECK(nonneg);
        CHECK_EQ(sum, PcieDataUnits(p));
      }
    }
  }
}

void StorageBound() {
  // Header-limited: hdr full TLPs.
  CHECK_EQ(PcieStorageFlits(4, -1, 256, 24, 64), 4 * 5);
  // Data-limited at 64 B flits: one flit per data credit at most (tiny TLPs).
  CHECK_EQ(PcieStorageFlits(-1, 128, 256, 24, 64), 128);
  CHECK_EQ(PcieStorageFlits(8, 128, 256, 24, 64), 40);
  // Upper bound: random admitted sets never exceed it.
  srand(7);
  for(int trial = 0; trial < 2000; ++trial) {
    int hdr = 1 + rand() % 40, units = 16 + rand() % 400;
    int bound = PcieStorageFlits(hdr, units, 256, 24, 64);
    int h = 0, u = 0, flits = 0;
    while(true) {
      int p = 1 + rand() % 256;
      if(h + 1 > hdr || u + PcieDataUnits(p) > units) break;
      ++h; u += PcieDataUnits(p); flits += Flits(p, 24, 64);
    }
    CHECK(flits <= bound);
  }
}

BookSimConfig * Vc0Config() {
  BookSimConfig * c = new BookSimConfig;
  c->Assign("num_vcs", 15);
  c->Assign("write_request_begin_vc", 0);  c->Assign("write_request_end_vc", 4);
  c->Assign("read_request_begin_vc", 5);   c->Assign("read_request_end_vc", 9);
  c->Assign("read_reply_begin_vc", 10);    c->Assign("read_reply_end_vc", 14);
  c->Assign("buffer_policy", std::string("shared"));
  c->Assign("output_buffer_size", 4);
  c->Assign("pcie_credits", 1);
  return c;
}

void Derivation() {
  BookSimConfig * c = Vc0Config();
  c->Assign("pcie_ph", 8);            // 8 TLPs -> 40 flits
  c->Assign("pcie_pd_bytes", 1024);   // 64 credits -> at most 64 flits
  c->Assign("pcie_nph", 0);           // infinite
  c->Assign("pcie_npd_bytes", 0);
  c->Assign("pcie_cplh", 0);
  c->Assign("pcie_cpld_bytes", 512);  // 32 credits -> at most 32 flits
  {
    QuietCout q;
    ApplyPcieCreditConfig(*c);
  }
  std::vector<int> size = c->GetIntArray("private_buf_size");
  CHECK_EQ(size.size(), 3u);
  CHECK_EQ(size[0], 40);
  CHECK_EQ(size[1], 1 << 20);
  CHECK_EQ(size[2], 32);
  CHECK_EQ(c->GetInt("buf_size"), 40 + (1 << 20) + 32);
  CHECK_EQ(c->GetInt("private_bufs"), 3);
  CHECK_EQ(c->GetStr("private_buf_start_vc"), std::string("{0,5,10}"));
  CHECK_EQ(c->GetStr("private_buf_end_vc"), std::string("{4,9,14}"));
  CHECK(CheckPoolsMatchTypes(*c).empty());
  SharedPoolLayout l = ReadSharedPoolLayout(*c);
  CHECK_EQ(l.shared, 0);
  delete c;

  // A pool that spans two types, or a nonzero common pool, is rejected.
  c = Vc0Config();
  c->Assign("private_bufs", 2);
  c->Assign("private_buf_size", std::string("{40,40}"));
  c->Assign("private_buf_start_vc", std::string("{0,10}"));
  c->Assign("private_buf_end_vc", std::string("{9,14}"));
  c->Assign("buf_size", 80);
  CHECK(!CheckPoolsMatchTypes(*c).empty());
  c->Assign("private_bufs", 3);
  c->Assign("private_buf_size", std::string("{40,40,40}"));
  c->Assign("private_buf_start_vc", std::string("{0,5,10}"));
  c->Assign("private_buf_end_vc", std::string("{4,9,14}"));
  c->Assign("buf_size", 130);
  CHECK(CheckPoolsMatchTypes(*c).find("common shared pool") != std::string::npos);
  c->Assign("buf_size", 120);
  CHECK(CheckPoolsMatchTypes(*c).empty());
  delete c;
}

// Run ApplyPcieCreditConfig in a child; it must exit nonzero with `expect`
// in its message.
void ExpectConfigError(void (*setup)(BookSimConfig &), const char * expect) {
  int fds[2];
  if(pipe(fds) != 0) { CHECK(false); return; }
  pid_t pid = fork();
  if(pid == 0) {
    dup2(fds[1], 1);
    close(fds[0]);
    BookSimConfig * c = Vc0Config();
    setup(*c);
    ApplyPcieCreditConfig(*c);
    _exit(0);
  }
  close(fds[1]);
  std::string out;
  char buf[512];
  ssize_t n;
  while((n = read(fds[0], buf, sizeof(buf))) > 0) out.append(buf, n);
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) != 0);
  if(out.find(expect) == std::string::npos) {
    std::cout << "  expected '" << expect << "' in: " << out;
    CHECK(false);
  }
}

void SetBufSize(BookSimConfig & c)      { c.Assign("buf_size", 64); }
void SetOddData(BookSimConfig & c)      { c.Assign("pcie_pd_bytes", 1000); }
void SetTinyData(BookSimConfig & c)     { c.Assign("pcie_pd_bytes", 128); }
void SetOverlap(BookSimConfig & c)      { c.Assign("read_request_begin_vc", 4); }
void SetNoOutputBuf(BookSimConfig & c)  { c.Assign("output_buffer_size", -1); }
void SetPrivate(BookSimConfig & c)      { c.Assign("buffer_policy", std::string("private")); }
void SetSpeedup(BookSimConfig & c)      { c.Assign("output_speedup", 2); }

void ConfigErrors() {
  ExpectConfigError(SetBufSize, "remove them");
  ExpectConfigError(SetOddData, "multiple of 16");
  ExpectConfigError(SetTinyData, "cannot admit even one full TLP");
  ExpectConfigError(SetOverlap, "overlap");
  ExpectConfigError(SetNoOutputBuf, "finite output_buffer_size");
  ExpectConfigError(SetPrivate, "buffer_policy = shared");
  ExpectConfigError(SetSpeedup, "output_speedup = 1");
}

// A TLP as the traffic manager builds it: flits with the payload and each
// flit's data-credit share.
std::vector<Flit *> MakeTlp(int payload, int vc, Flit::FlitType type) {
  int n = Flits(payload, 24, 64);
  std::vector<int> ret = PcieDataReturns(payload, 24, 64, n);
  std::vector<Flit *> tlp;
  static int id = 0;
  for(int i = 0; i < n; ++i) {
    Flit * f = Flit::New();
    f->id = id++;
    f->pid = id;
    f->head = (i == 0);
    f->tail = (i == n - 1);
    f->vc = vc;
    f->cl = 0;
    f->type = type;
    f->pcie_payload = payload;
    f->pcie_data_ret = ret[i];
    tlp.push_back(f);
  }
  return tlp;
}

// The receiver frees each flit and returns its credit; one message per flit.
void ReturnAll(BufferState & bs, const std::vector<Flit *> & tlp) {
  for(size_t i = 0; i < tlp.size(); ++i) {
    Credit * c = Credit::New();
    c->vc.insert(tlp[i]->vc);
    c->AddPcieReturn(tlp[i]->vc, tlp[i]);
    bs.ProcessCredit(c);
    c->Free();
  }
}

void Send(BufferState & bs, const std::vector<Flit *> & tlp) {
  bs.TakeBuffer(tlp[0]->vc);
  for(size_t i = 0; i < tlp.size(); ++i) bs.SendingFlit(tlp[i]);
}

void WholeTlpAdmission() {
  // Small header and data limits (2 headers, 20 data credits); private storage large
  // enough that only protocol credits bind.
  BookSimConfig c;
  c.Assign("num_vcs", 4);
  c.Assign("vc_buf_size", 256);
  c.Assign("pcie_credits", 1);
  c.Assign("pcie_ph", 2);
  c.Assign("pcie_pd_bytes", 320);   // 20 data credits
  QuietCout q0;
  BufferState bs(c, NULL, "bs");

  std::vector<Flit *> a = MakeTlp(256, 0, Flit::WRITE_REQUEST);   // 16 credits
  CHECK(bs.CanAdmitTLP(a[0]));
  CHECK(!bs.IsFullFor(0, a[0]));
  Send(bs, a);
  // Body flits did not debit again: exactly 1 header and 16 data credits.
  CHECK_EQ(bs.PcieHeadersUsed(PCIE_P), 1);
  CHECK_EQ(bs.PcieDataUsed(PCIE_P), 16);

  // 4 data credits left and one header: a 256 B head cannot be sent ...
  std::vector<Flit *> b = MakeTlp(256, 1, Flit::WRITE_REQUEST);
  CHECK(!bs.CanAdmitTLP(b[0]));
  CHECK(bs.IsFullFor(1, b[0]));
  CHECK(!bs.IsFullFor(1, b[1]));      // body flits need only a flit slot
  // ... but a 64 B final payload needs only 4.
  std::vector<Flit *> s = MakeTlp(64, 1, Flit::WRITE_REQUEST);
  CHECK(bs.CanAdmitTLP(s[0]));
  Send(bs, s);
  CHECK_EQ(bs.PcieHeadersUsed(PCIE_P), 2);
  CHECK_EQ(bs.PcieDataUsed(PCIE_P), 20);

  // Header exhaustion: data returned but no header yet.
  std::vector<Flit *> tiny = MakeTlp(16, 2, Flit::WRITE_REQUEST);
  ReturnAll(bs, std::vector<Flit *>(s.begin(), s.end() - 1));   // all but the tail
  CHECK_EQ(bs.PcieHeadersUsed(PCIE_P), 2);
  CHECK(!bs.CanAdmitTLP(tiny[0]));
  ReturnAll(bs, std::vector<Flit *>(s.end() - 1, s.end()));     // tail: header back
  CHECK_EQ(bs.PcieHeadersUsed(PCIE_P), 1);
  CHECK(bs.CanAdmitTLP(tiny[0]));

  // Other types are separate pools (and infinite here).
  std::vector<Flit *> cpl = MakeTlp(256, 3, Flit::READ_REPLY);
  CHECK(bs.CanAdmitTLP(cpl[0]));

  // Return the 256 B TLP in one message carrying records for two VCs.
  Send(bs, tiny);
  Credit * m = Credit::New();
  for(size_t i = 0; i < a.size(); ++i) {
    if(i > 0) {                     // one slot per VC per message
      bs.ProcessCredit(m);
      m->Free();
      m = Credit::New();
    }
    m->vc.insert(0);
    m->AddPcieReturn(0, a[i]);
    if(i == 0) {
      m->vc.insert(2);
      m->AddPcieReturn(2, tiny[0]);   // tiny is a single flit: head and tail
    }
  }
  CHECK(m->pcie.size() >= 1);
  bs.ProcessCredit(m);
  m->Free();
  // Every finite pool recovers its configured capacity.
  CHECK_EQ(bs.PcieHeadersUsed(PCIE_P), 0);
  CHECK_EQ(bs.PcieDataUsed(PCIE_P), 0);
  CHECK(bs.PcieCreditsIdle());

  // Pooled objects come back clean.
  Credit * fresh = Credit::New();
  CHECK(fresh->pcie.empty());
  fresh->Free();
  Flit * f = a[0];
  f->Free();
  Flit * g = Flit::New();
  CHECK_EQ(g->pcie_payload, -1);
  CHECK_EQ(g->pcie_data_ret, 0);
  CHECK_EQ(g->path_key, -1);
  CHECK_EQ(g->lf_unit, -1);
  g->Free();
}

// Conservation over many TLPs of random size, credits returned in random
// interleavings: each TLP returns exactly 1 header and ceil(payload/16).
void Conservation() {
  BookSimConfig c;
  c.Assign("num_vcs", 4);
  c.Assign("vc_buf_size", 4096);
  c.Assign("pcie_credits", 1);
  c.Assign("pcie_ph", 64);
  c.Assign("pcie_pd_bytes", 64 * 256);
  QuietCout q0;
  BufferState bs(c, NULL, "bs");
  srand(99);
  std::vector<std::vector<Flit *> > live;
  int vc = 0;
  for(int step = 0; step < 5000; ++step) {
    if(!live.empty() && rand() % 2) {
      size_t k = rand() % live.size();
      ReturnAll(bs, live[k]);
      live.erase(live.begin() + k);
    } else {
      std::vector<Flit *> t = MakeTlp(1 + rand() % 256, vc, Flit::WRITE_REQUEST);
      if(bs.CanAdmitTLP(t[0]) && bs.IsAvailableFor(vc)) {
        Send(bs, t);
        live.push_back(t);
        vc = (vc + 1) % 4;
      }
    }
    int h = 0, d = 0;
    for(size_t i = 0; i < live.size(); ++i) {
      ++h;
      d += PcieDataUnits(live[i][0]->pcie_payload);
    }
    CHECK_EQ(bs.PcieHeadersUsed(PCIE_P), h);
    CHECK_EQ(bs.PcieDataUsed(PCIE_P), d);
  }
  for(size_t k = 0; k < live.size(); ++k) ReturnAll(bs, live[k]);
  CHECK(bs.PcieCreditsIdle());
}

void NoqSlice() {
  int b, e;
  // Stock: the range divided evenly over the next switch's outputs.
  CHECK(NoqVcSlice(0, 14, 2, 5, 0, b, e)); CHECK_EQ(b, 6); CHECK_EQ(e, 8);
  CHECK(NoqVcSlice(0, 4, 1, 3, 0, b, e));  CHECK_EQ(b, 1); CHECK_EQ(e, 1);
  // One VC per egress: VC = range start + output; spare VCs unused.
  CHECK(NoqVcSlice(5, 9, 3, 4, 1, b, e));  CHECK_EQ(b, 8); CHECK_EQ(e, 8);
  CHECK(NoqVcSlice(5, 9, 4, 5, 1, b, e));  CHECK_EQ(b, 9); CHECK_EQ(e, 9);
  CHECK(!NoqVcSlice(5, 9, 5, 6, 1, b, e)); // a 6-port switch needs 6 VCs
  CHECK(!NoqVcSlice(0, 3, 0, 5, 0, b, e)); // stock: fewer VCs than outputs
}

} // namespace

int main() {
  DataReturns();
  StorageBound();
  Derivation();
  ConfigErrors();
  WholeTlpAdmission();
  Conservation();
  NoqSlice();
  return TestSummary("test_pcie_credits");
}
