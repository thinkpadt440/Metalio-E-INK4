#pragma once
#include <cassert>
enum class PowerNeed { LocalMic, StandbyInhibit };
class PowerPolicy {
public:
    static PowerPolicy& GetInstance() { static PowerPolicy policy; return policy; }
    void Acquire(PowerNeed need) { ++counts[static_cast<int>(need)]; }
    void Release(PowerNeed need) { auto& value = counts[static_cast<int>(need)]; assert(value > 0); --value; }
    bool idle() const { return counts[0] == 0 && counts[1] == 0; }
private:
    int counts[2] = {};
};
