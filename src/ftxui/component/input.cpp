// Copyright 2022 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <algorithm>   // for max, min
#include <cstddef>     // for size_t
#include <cstdint>     // for uint32_t
#include <functional>  // for function
#include <memory>   // for allocator, shared_ptr, allocator_traits<>::value_type
#include <sstream>  // for basic_istream, stringstream
#include <string>   // for string, basic_string, operator==, getline
#include <utility>  // for move
#include <vector>   // for vector

#include "ftxui/component/captured_mouse.hpp"     // for CapturedMouse
#include "ftxui/component/component.hpp"          // for Make, Input
#include "ftxui/component/component_base.hpp"     // for ComponentBase
#include "ftxui/component/component_options.hpp"  // for InputOption
#include "ftxui/component/event.hpp"  // for Event, Event::ArrowDown, Event::ArrowLeft, Event::ArrowLeftCtrl, Event::ArrowRight, Event::ArrowRightCtrl, Event::ArrowUp, Event::Backspace, Event::Delete, Event::End, Event::Home, Event::Return
#include "ftxui/component/mouse.hpp"  // for Mouse, Mouse::Left, Mouse::Pressed
#include "ftxui/component/screen_interactive.hpp"  // for Component
#include "ftxui/dom/elements.hpp"  // for operator|, reflect, text, Element, xflex, hbox, Elements, frame, operator|=, vbox, focus, focusCursorBarBlinking, select
#include "ftxui/screen/box.hpp"    // for Box
#include "ftxui/screen/string.hpp"           // for string_width
#include "ftxui/screen/string_internal.hpp"  // for GlyphNext, GlyphPrevious, WordBreakProperty, EatCodePoint, CodepointToWordBreakProperty, IsFullWidth, WordBreakProperty::ALetter, WordBreakProperty::CR, WordBreakProperty::Double_Quote, WordBreakProperty::Extend, WordBreakProperty::ExtendNumLet, WordBreakProperty::Format, WordBreakProperty::Hebrew_Letter, WordBreakProperty::Katakana, WordBreakProperty::LF, WordBreakProperty::MidLetter, WordBreakProperty::MidNum, WordBreakProperty::MidNumLet, WordBreakProperty::Newline, WordBreakProperty::Numeric, WordBreakProperty::Regional_Indicator, WordBreakProperty::Single_Quote, WordBreakProperty::WSegSpace, WordBreakProperty::ZWJ
#include "ftxui/screen/util.hpp"             // for clamp
#include "ftxui/util/ref.hpp"                // for StringRef, Ref

namespace ftxui {

namespace {

std::vector<std::string> Split(const std::string& input) {
  std::vector<std::string> output;
  std::stringstream ss(input);
  std::string line;
  while (std::getline(ss, line)) {
    output.push_back(line);
  }
  if (input.back() == '\n') {
    output.emplace_back("");
  }
  return output;
}

size_t GlyphWidth(const std::string& input, size_t iter) {
  uint32_t ucs = 0;
  if (!EatCodePoint(input, iter, &iter, &ucs)) {
    return 0;
  }
  if (IsFullWidth(ucs)) {
    return 2;
  }
  return 1;
}

bool IsWordCodePoint(uint32_t codepoint) {
  switch (CodepointToWordBreakProperty(codepoint)) {
    case WordBreakProperty::ALetter:
    case WordBreakProperty::Hebrew_Letter:
    case WordBreakProperty::Katakana:
    case WordBreakProperty::Numeric:
      return true;

    case WordBreakProperty::CR:
    case WordBreakProperty::Double_Quote:
    case WordBreakProperty::LF:
    case WordBreakProperty::MidLetter:
    case WordBreakProperty::MidNum:
    case WordBreakProperty::MidNumLet:
    case WordBreakProperty::Newline:
    case WordBreakProperty::Single_Quote:
    case WordBreakProperty::WSegSpace:
    // Unexpected/Unsure
    case WordBreakProperty::Extend:
    case WordBreakProperty::ExtendNumLet:
    case WordBreakProperty::Format:
    case WordBreakProperty::Regional_Indicator:
    case WordBreakProperty::ZWJ:
      return false;
  }
  return false;  // NOT_REACHED();
}

bool IsWordCharacter(const std::string& input, size_t iter) {
  uint32_t ucs = 0;
  if (!EatCodePoint(input, iter, &iter, &ucs)) {
    return false;
  }

  return IsWordCodePoint(ucs);
}

// An input box. The user can type text into it.
class InputBase : public ComponentBase, public InputOption {
 public:
  // NOLINTNEXTLINE
  InputBase(InputOption option) : InputOption(std::move(option)) {}

 private:
  // Component implementation:
  Element Render() override {
    const bool is_focused = Focused();
    const auto focused = (!is_focused && !hovered_) ? select
                         : insert()                 ? focusCursorUnderline
                                                    : focusCursorUnderline;

    auto transform_func =
        transform ? transform : InputOption::Default().transform;
    // placeholder.
    if (content->empty() && placeholder().length() > 0) {
      auto element = text(placeholder()) | xflex | frame;
      if (is_focused) {
        element |= focus;
      }

      return transform_func({
                 std::move(element), hovered_, is_focused,
                 true  // placeholder
             }) |
             reflect(box_);
    }

    Elements elements;
    const std::vector<std::string> lines = Split(*content);

    cursor_position() = util::clamp(cursor_position(), 0, (int)content->size());

    // Find the line and index of the cursor.
    int cursor_line = 0;
    int cursor_char_index = cursor_position();
    for (const auto& line : lines) {
      if (cursor_char_index <= (int)line.size()) {
        break;
      }

      cursor_char_index -= line.size() + 1;
      cursor_line++;
    }

    if (lines.empty()) {
      elements.push_back(text("") | focused);
    }

    elements.reserve(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) {
      const std::string& line = lines[i];

      // This is not the cursor line.
      if (int(i) != cursor_line) {
        elements.push_back(Text(line));
        continue;
      }

      // The cursor is at the end of the line.
      if (cursor_char_index >= (int)line.size()) {
        elements.push_back(hbox({
                               Text(line),
                               text(" ") | focused | reflect(cursor_box_),
                           }) |
                           xflex);
        continue;
      }

      // The cursor is on this line.
      const int glyph_start = cursor_char_index;
      const int glyph_end = GlyphNext(line, glyph_start);
      const std::string part_before_cursor = line.substr(0, glyph_start);
      const std::string part_at_cursor =
          line.substr(glyph_start, glyph_end - glyph_start);
      const std::string part_after_cursor = line.substr(glyph_end);
      auto element = hbox({
                         Text(part_before_cursor),
                         Text(part_at_cursor) | focused | reflect(cursor_box_),
                         Text(part_after_cursor),
                     }) |
                     xflex;
      elements.push_back(element);
    }

    auto element = vbox(std::move(elements)) | frame;
    return transform_func({
               std::move(element), hovered_, is_focused,
               false  // placeholder
           }) |
           xflex | reflect(box_);
  }

  Element Text(const std::string& input) {
    if (!password()) {
      return text(input);
    }

    std::string out;
    out.reserve(10 + input.size() * 3 / 2);
    for (size_t i = 0; i < input.size(); ++i) {
      out += password_char();
    }
    return text(out);
  }

  bool HandleBackspace() {
    if (cursor_position() == 0) {
      return false;
    }
    const size_t start = GlyphPrevious(content(), cursor_position());
    const size_t end = cursor_position();
    content->erase(start, end - start);
    cursor_position() = start;
    return true;
  }

  bool HandleArrowUp() {
    if (cursor_position() == (int)content->size()) {
      return false;
    }
    return true;
  }

  bool HandleArrowDown() {
    if (cursor_position() == (int)content->size()) {
      return false;
    }
    return true;
  }

  bool HandleReturn() {
    if (multiline()) {
      HandleCharacter("\n");
    }
    on_enter();
    return true;
  }

  bool HandleDelete() {
    if (cursor_position() == (int)content->size()) {
      return false;
    }
    const size_t start = cursor_position();
    const size_t end = GlyphNext(content(), cursor_position());
    content->erase(start, end - start);
    return true;
  }

  bool HandleCharacter(const std::string& character) {
    if (content().length() >= max_input_len())
      return true;
    if (!insert() && cursor_position() < (int)content->size() &&
        content()[cursor_position()] != '\n') {
      HandleDelete();
    }
    content->insert(cursor_position(), character);
    cursor_position() += character.size();
    on_change();
    return true;
  }

  bool OnEvent(Event event) override {
    cursor_position() = util::clamp(cursor_position(), 0, (int)content->size());

    if (event == Event::Return) {
      return HandleReturn();
    }
    if (event.is_character()) {
      return HandleCharacter(event.character());
    }
    if (event == Event::Backspace) {
      return HandleBackspace();
    }
    if (event == Event::ArrowUp) {
      return HandleArrowUp();
    }
    if (event == Event::ArrowDown) {
      return HandleArrowDown();
    }
    return false;
  }

  bool Focusable() const final { return true; }

  bool hovered_ = false;

  Box box_;
  Box cursor_box_;
};

}  // namespace

/// @brief An input box for editing text.
/// @param option Additional optional parameters.
/// @ingroup component
/// @see InputBase
///
/// ### Example
///
/// ```cpp
/// auto screen = ScreenInteractive::FitComponent();
/// std::string content= "";
/// std::string placeholder = "placeholder";
/// Component input = Input({
///   .content = &content,
///   .placeholder = &placeholder,
/// })
/// screen.Loop(input);
/// ```
///
/// ### Output
///
/// ```bash
/// placeholder
/// ```
Component Input(InputOption option) {
  return Make<InputBase>(std::move(option));
}

/// @brief An input box for editing text.
/// @param content The editable content.
/// @param option Additional optional parameters.
/// @ingroup component
/// @see InputBase
///
/// ### Example
///
/// ```cpp
/// auto screen = ScreenInteractive::FitComponent();
/// std::string content= "";
/// std::string placeholder = "placeholder";
/// Component input = Input(content, {
///   .placeholder = &placeholder,
///   .password = true,
/// })
/// screen.Loop(input);
/// ```
///
/// ### Output
///
/// ```bash
/// placeholder
/// ```
Component Input(StringRef content, InputOption option) {
  option.content = std::move(content);
  return Make<InputBase>(std::move(option));
}

/// @brief An input box for editing text.
/// @param content The editable content.
/// @param option Additional optional parameters.
/// @ingroup component
/// @see InputBase
///
/// ### Example
///
/// ```cpp
/// auto screen = ScreenInteractive::FitComponent();
/// std::string content= "";
/// std::string placeholder = "placeholder";
/// Component input = Input(content, placeholder);
/// screen.Loop(input);
/// ```
///
/// ### Output
///
/// ```bash
/// placeholder
/// ```
Component Input(StringRef content, StringRef placeholder, InputOption option) {
  option.content = std::move(content);
  option.placeholder = std::move(placeholder);
  return Make<InputBase>(std::move(option));
}

}  // namespace ftxui
