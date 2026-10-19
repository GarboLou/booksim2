// Minimal unit-test helpers. Each tests/test_*.cpp is its own binary, run by
// `make test` from the booksim2 root.

#ifndef _TEST_UTIL_HPP_
#define _TEST_UTIL_HPP_

#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "booksim.hpp"
#include "booksim_config.hpp"
#include "booksim_init.hpp"
#include "network.hpp"

static int g_test_failures = 0;
static int g_test_checks = 0;

#define CHECK(cond) do {                                                    \
    ++g_test_checks;                                                        \
    if(!(cond)) {                                                           \
      ++g_test_failures;                                                    \
      std::cout << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond     \
                << std::endl;                                               \
    }                                                                       \
  } while(0)

#define CHECK_EQ(a, b) do {                                                 \
    ++g_test_checks;                                                        \
    if(!((a) == (b))) {                                                     \
      ++g_test_failures;                                                    \
      std::cout << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #a " == " \
                #b " (" << (a) << " vs " << (b) << ")" << std::endl;        \
    }                                                                       \
  } while(0)

static inline int TestSummary(const char * name) {
  std::cout << name << ": " << g_test_checks << " checks, " << g_test_failures
            << " failures" << std::endl;
  return g_test_failures ? 1 : 0;
}

// Parse a config file and apply key=value overrides, the way Engine::Create
// does (ParseString appends the terminating ';').
static inline void LoadConfig(BookSimConfig & config, const std::string & file,
                              const std::vector<std::pair<std::string, std::string> > & ov) {
  config.ParseFile(file);
  for(size_t i = 0; i < ov.size(); ++i) {
    std::ostringstream os;
    os << ov[i].first << " = " << ov[i].second;
    config.ParseString(os.str());
  }
}

// Silence the topology dump the network constructors print. _sink is declared
// first: members are built in declaration order, and _old's initializer uses
// _sink.
class QuietCout {
  std::ostringstream _sink;
  std::streambuf * _old;
public:
  QuietCout() : _old(std::cout.rdbuf(_sink.rdbuf())) {}
  ~QuietCout() { std::cout.rdbuf(_old); }
  std::string Text() const { return _sink.str(); }
};

#endif
