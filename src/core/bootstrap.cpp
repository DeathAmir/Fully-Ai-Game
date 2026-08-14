#include "core/runtime.hpp"

namespace {

class AutoRuntime {
public:
    AutoRuntime() { irx::Runtime::instance().start(); }
    ~AutoRuntime() { irx::Runtime::instance().stop(); }
};

AutoRuntime runtime;

}