#pragma once
#include <string>

namespace ot2 {
// Caller serializes filesystem operations. The committed directory keeps its
// public name; only an atomic directory exchange can replace it.
class SaveTransaction {
public:
    explicit SaveTransaction(std::string committed);
    int begin();
    int commit();
    void poison(int error);
    bool failed() const {return failure_!=0;}
    bool ready() const {return ready_;}
    bool published() const {return published_;}
    const std::string& working() const {return working_;}
    const std::string& committed() const {return committed_;}
private:
    std::string committed_,working_,snapshot_;
    int failure_=0;
    bool ready_=false,published_=false;
    int rollback();
};
// Fault injection exists only in the standalone contract-test executable.
int save_test_fault(const char* point);
}
