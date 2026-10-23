#include <catch2/catch_test_macros.hpp>
#include "../src/menu_navigation.h"

using Command = MenuNavigation::Command;
using Action = MenuNavigation::Action;

TEST_CASE("Controller selects options and B returns to category navigation", "[guide-menu]") {
  MenuNavigation menu;
  menu.SetOptions({{true, false, true}, {false, true, true}});
  CHECK_FALSE(menu.InOptions());
  CHECK(menu.Handle(Command::Down) == Action::None);
  REQUIRE(menu.Section() == 1);
  CHECK(menu.Handle(Command::Select) == Action::None);
  REQUIRE(menu.InOptions());
  CHECK(menu.Option() == 1);
  CHECK(menu.Handle(Command::Right) == Action::Increase);
  CHECK(menu.Handle(Command::Left) == Action::Decrease);
  CHECK(menu.Handle(Command::Down) == Action::None);
  CHECK(menu.Option() == 2);
  CHECK(menu.Handle(Command::Back) == Action::None);
  REQUIRE_FALSE(menu.InOptions());
  CHECK(menu.Section() == 1);
  menu.Handle(Command::Up);
  CHECK(menu.Section() == 0);
  menu.Handle(Command::Right);
  REQUIRE(menu.InOptions());
  menu.Handle(Command::Down);
  CHECK(menu.Option() == 2); // Disabled option is skipped.
  menu.Handle(Command::Down);
  CHECK(menu.Option() == 0); // Wrap within the options, never into the sidebar.
  menu.Handle(Command::Back);
  menu.Handle(Command::Down);
  menu.Handle(Command::Select);
  CHECK(menu.Option() == 2); // Preserve selection when returning to a category.
  menu.Handle(Command::Back);
  CHECK(menu.Handle(Command::Back) == Action::Close);
}

TEST_CASE("Controller focus survives unavailable HDR and dependent options", "[guide-menu]") {
  MenuNavigation menu;
  menu.SetOptions({{false, false}, {true, true, false}});
  menu.Handle(Command::Select);
  CHECK_FALSE(menu.InOptions());
  menu.Handle(Command::Down);
  menu.Handle(Command::Select);
  menu.FocusOption(1);
  REQUIRE(menu.Option() == 1);
  menu.SetOptions({{false, false}, {true, false, false}});
  REQUIRE(menu.InOptions());
  CHECK(menu.Option() == 0);
  menu.FocusOption(2);
  CHECK(menu.Option() == 0);
  CHECK(menu.Handle(Command::Apply) == Action::Apply);
  CHECK(menu.Handle(Command::Reload) == Action::Reload);
  CHECK(menu.InOptions());
  menu.SetOptions({{false, false}, {false, false, false}});
  CHECK_FALSE(menu.InOptions());
  menu.SetOptions({});
  CHECK(menu.Handle(Command::Down) == Action::None);
  CHECK(menu.Handle(Command::Select) == Action::None);
}
