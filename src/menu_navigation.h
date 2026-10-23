#pragma once
#include <algorithm>
#include <vector>

// Explicit two-level navigation, independent of ImGui's spatial/tab navigation.
// Each category remembers its last enabled option; disabled options are skipped.
class MenuNavigation {
 public:
  enum class Command { Up, Down, Left, Right, Select, Back, Apply, Reload };
  enum class Action { None, Decrease, Increase, Apply, Reload, Close };

  void SetOptions(std::vector<std::vector<bool>> enabled) {
    enabled_ = std::move(enabled);
    remembered_.resize(enabled_.size(), -1);
    section_ = std::clamp(section_, 0, std::max(0, int(enabled_.size()) - 1));
    for (int i = 0; i < int(enabled_.size()); ++i) {
      if (!Enabled(i, remembered_[i])) remembered_[i] = FirstEnabled(i);
    }
    if (Option() < 0) in_options_ = false;
  }

  Action Handle(Command command) {
    if (command == Command::Apply) return Action::Apply;
    if (command == Command::Reload) return Action::Reload;
    if (command == Command::Back) {
      if (!in_options_) return Action::Close;
      in_options_ = false;
      return Action::None;
    }
    if (enabled_.empty()) return Action::None;
    if (command == Command::Up || command == Command::Down) {
      int delta = command == Command::Up ? -1 : 1;
      if (!in_options_) {
        section_ = (section_ + delta + int(enabled_.size())) % int(enabled_.size());
      } else {
        const int count = int(enabled_[section_].size());
        for (int offset = 1; offset <= count; ++offset) {
          int index = (Option() + delta * offset + count) % count;
          if (Enabled(section_, index)) { remembered_[section_] = index; break; }
        }
      }
    } else if (!in_options_) {
      if (command == Command::Select || command == Command::Right) EnterSection(section_);
    } else if (command == Command::Left) {
      return Action::Decrease;
    } else if (command == Command::Right || command == Command::Select) {
      return Action::Increase;
    }
    return Action::None;
  }

  void EnterSection(int section) {
    if (section < 0 || section >= int(enabled_.size())) return;
    section_ = section;
    in_options_ = Option() >= 0;
  }
  void FocusOption(int option) {
    if (!Enabled(section_, option)) return;
    remembered_[section_] = option;
    in_options_ = true;
  }
  int Section() const { return section_; }
  int Option() const { return remembered_.empty() ? -1 : remembered_[section_]; }
  bool InOptions() const { return in_options_; }

 private:
  bool Enabled(int section, int option) const {
    return section >= 0 && section < int(enabled_.size()) &&
        option >= 0 && option < int(enabled_[section].size()) && enabled_[section][option];
  }
  int FirstEnabled(int section) const {
    const auto& options = enabled_[section];
    auto first = std::find(options.begin(), options.end(), true);
    return first == options.end() ? -1 : int(first - options.begin());
  }
  std::vector<std::vector<bool>> enabled_;
  std::vector<int> remembered_;
  int section_ = 0;
  bool in_options_ = false;
};
