#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace ot2 {
struct HostService { const char* name; uint64_t (*call)(uint64_t*); };
const std::vector<HostService>& host_services();
const std::vector<HostService>& audio_services();
const std::vector<HostService>& input_services();
const std::vector<HostService>& movie_services();
void set_activity_active(bool active);
void set_io_prefetch(bool enabled);
// nn::oe::FocusState: 1 = focused, 2 = visible without focus, 3 = background.
void set_activity_focus(unsigned state);
void set_audio_active(bool active);
bool probe_audio_recovery();
// Legacy profile only. New launches supply the validated game's ProgramId.
constexpr uint64_t legacy_ot2_program_id=0x0100a3501946e000ULL;
void configure_services(const std::string& romfs,const std::string& user_directory,uint64_t program_id=legacy_ot2_program_id);
uint64_t configured_program_id();
// Coalesced process-wide notifications; the Android UI drains on its main
// thread and retains them across Activity recreation until acknowledged.
enum SaveNotice : unsigned { SaveReadOnly=1, SaveCommitFailed=2, SaveDurabilityUncertain=4 };
unsigned take_save_notice();
}
