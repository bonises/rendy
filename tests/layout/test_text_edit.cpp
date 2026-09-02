#include <catch2/catch_test_macros.hpp>

#include "ui/text_edit.hpp"

using namespace rendy::ui::edit;

TEST_CASE("utf8 codepoint stepping", "[ui][edit]") {
    const std::string text = "aåä🦊b"; // 1 + 2 + 2 + 4 + 1 bytes
    REQUIRE(nextCp(text, 0) == 1);
    REQUIRE(nextCp(text, 1) == 3);
    REQUIRE(nextCp(text, 3) == 5);
    REQUIRE(nextCp(text, 5) == 9);
    REQUIRE(nextCp(text, 9) == 10);
    REQUIRE(prevCp(text, 10) == 9);
    REQUIRE(prevCp(text, 9) == 5);
    REQUIRE(prevCp(text, 5) == 3);
    REQUIRE(prevCp(text, 1) == 0);
    REQUIRE(prevCp(text, 0) == 0);
}

TEST_CASE("insert replaces the selection", "[ui][edit]") {
    State s;
    insert(&s, "hello world");
    REQUIRE(s.text == "hello world");
    REQUIRE(s.cursor == 11);
    s.anchor = 0;
    s.cursor = 5; // "hello" selected
    insert(&s, "hej");
    REQUIRE(s.text == "hej world");
    REQUIRE(s.cursor == 3);
    REQUIRE_FALSE(s.hasSelection());
}

TEST_CASE("backspace and delete", "[ui][edit]") {
    State s;
    insert(&s, "aä");
    REQUIRE(eraseBackward(&s)); // removes the 2-byte ä in one go
    REQUIRE(s.text == "a");
    REQUIRE(eraseBackward(&s));
    REQUIRE_FALSE(eraseBackward(&s)); // empty: no change

    insert(&s, "abc");
    s.cursor = s.anchor = 0;
    REQUIRE(eraseForward(&s));
    REQUIRE(s.text == "bc");
    // Selection deletes exactly the selection.
    s.cursor = 2;
    s.anchor = 1;
    REQUIRE(eraseForward(&s));
    REQUIRE(s.text == "b");
}

TEST_CASE("word movement", "[ui][edit]") {
    State s;
    insert(&s, "foo  bar_baz qux");
    moveHome(&s, false);
    moveRight(&s, false, true);
    REQUIRE(s.cursor == 3); // end of "foo"
    moveRight(&s, false, true);
    REQUIRE(s.cursor == 12); // end of "bar_baz"
    moveLeft(&s, false, true);
    REQUIRE(s.cursor == 5); // start of "bar_baz"
    // ctrl+backspace removes a whole word
    moveEnd(&s, false);
    REQUIRE(eraseBackward(&s, true));
    REQUIRE(s.text == "foo  bar_baz ");
}

TEST_CASE("plain arrows collapse selection to its edge", "[ui][edit]") {
    State s;
    insert(&s, "abcdef");
    s.anchor = 1;
    s.cursor = 4;
    moveLeft(&s, false, false);
    REQUIRE(s.cursor == 1); // left edge, not cursor-1
    s.anchor = 1;
    s.cursor = 4;
    moveRight(&s, false, false);
    REQUIRE(s.cursor == 4); // right edge
    // shift+arrow extends instead
    moveRight(&s, true, false);
    REQUIRE(s.anchor == 4);
    REQUIRE(s.cursor == 5);
}

TEST_CASE("select all and home/end", "[ui][edit]") {
    State s;
    insert(&s, "hejsan");
    selectAll(&s);
    REQUIRE(s.selBegin() == 0);
    REQUIRE(s.selEnd() == 6);
    moveHome(&s, false);
    REQUIRE(s.cursor == 0);
    REQUIRE_FALSE(s.hasSelection());
    moveEnd(&s, true);
    REQUIRE(s.hasSelection());
    REQUIRE(s.selEnd() == 6);
}
