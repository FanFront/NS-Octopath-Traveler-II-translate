#pragma once
#include <cstdint>
#include <string>
#include <optional>
namespace ot2 {
struct TextInputRequest {uint64_t id{};std::u16string title,subtitle,guide,initial;int minimum{},maximum{};};
std::optional<TextInputRequest> pending_text_input();
bool complete_text_input(uint64_t id,bool accepted,const std::u16string& text);
void set_text_input_available(bool);
uint64_t show_keyboard(uint64_t* registers);
}
